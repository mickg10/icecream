#include "p50_zstd_sender.h"
#include "services/comm.h"

#include <boost/asio/this_coro.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <deque>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace icecc::p50 {
namespace {

using Clock = std::chrono::steady_clock;

bool nonzero(CStoreGuid guid) {
    return std::any_of(guid.bytes.begin(), guid.bytes.end(),
                       [](uint8_t byte) { return byte != 0; });
}

bool nonzero_request(PrepareRequestKey request) {
    return request.producer_session != 0 && request.request_token != 0;
}

std::optional<ProfileId> profile_from_cache_mask(uint32_t mask) noexcept {
    if (mask == CACHE_PROFILE_P29V1)
        return ProfileId::P29V1;
    if (mask == CACHE_PROFILE_ZSTD_TU)
        return ProfileId::ZSTD_TU;
    if (mask == CACHE_PROFILE_ZSTD_ROUTE)
        return ProfileId::ZSTD_ROUTE;
    return std::nullopt;
}

bool valid_deadline(Clock::time_point deadline, Clock::duration maximum_duration,
                    Clock::time_point now) {
    return deadline > now && maximum_duration > Clock::duration::zero() &&
           deadline - now <= maximum_duration;
}

bool route_history_profile(ProfileId profile) noexcept {
    return profile == ProfileId::P29V1 || profile == ProfileId::ZSTD_ROUTE;
}

struct AsyncFdCompletion {
    explicit AsyncFdCompletion(boost::asio::any_io_executor executor)
        : timer(std::make_shared<boost::asio::steady_timer>(executor)) {}
    std::mutex mutex;
    bool completed = false;
    bool abandoned = false;
    int fd = -1;
    std::shared_ptr<boost::asio::steady_timer> timer;
    ~AsyncFdCompletion() {
        if (fd >= 0)
            (void)::close(fd);
    }
};

boost::asio::awaitable<int> await_connected_fd(
    AsyncConnectedFdFactory factory, Clock::time_point deadline) {
    const auto executor = co_await boost::asio::this_coro::executor;
    auto state = std::make_shared<AsyncFdCompletion>(executor);
    state->timer->expires_at(deadline);
    try {
        factory(deadline, [state, executor, deadline](int fd) mutable {
            bool close_fd = false;
            bool rejected = false;
            {
                std::lock_guard lock(state->mutex);
                if (state->abandoned || state->completed ||
                    Clock::now() >= deadline) {
                    rejected = true;
                    close_fd = fd >= 0;
                } else {
                    state->completed = true;
                    state->fd = fd;
                }
            }
            if (rejected) {
                if (close_fd)
                    (void)::close(fd);
                return;
            }
            try {
                boost::asio::post(executor, [state] {
                    boost::system::error_code ignored;
                    state->timer->cancel(ignored);
                });
            } catch (...) {
                // The deadline timer remains a wakeup path; the shared state
                // destructor closes any descriptor if the executor is gone.
            }
        });
    } catch (...) {
        std::lock_guard lock(state->mutex);
        state->abandoned = true;
        if (state->fd >= 0)
            (void)::close(state->fd);
        state->fd = -1;
        co_return -1;
    }
    boost::system::error_code wait_error;
    co_await state->timer->async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    (void)wait_error;
    std::lock_guard lock(state->mutex);
    if (!state->completed) {
        state->abandoned = true;
        co_return -1;
    }
    if (Clock::now() >= deadline) {
        if (state->fd >= 0)
            (void)::close(state->fd);
        state->fd = -1;
        co_return -1;
    }
    const int fd = state->fd;
    state->fd = -1;
    co_return fd;
}

std::optional<std::vector<uint8_t>> read_complete_fd(int fd, uint64_t limit) {
    if (fd < 0) return std::nullopt;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
        return std::nullopt;
    const uint64_t size = static_cast<uint64_t>(info.st_size);
    if (size > limit || size > static_cast<uint64_t>(SIZE_MAX)) return std::nullopt;
    std::vector<uint8_t> result(static_cast<size_t>(size));
    size_t offset = 0;
    while (offset != result.size()) {
        const ssize_t count = ::pread(fd, result.data() + offset,
                                      result.size() - offset,
                                      static_cast<off_t>(offset));
        if (count > 0) {
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return std::nullopt;
    }
    return result;
}

}  // namespace

OwnedSourceFd::~OwnedSourceFd() {
    if (fd_ >= 0) (void)::close(fd_);
}

OwnedSourceFd& OwnedSourceFd::operator=(OwnedSourceFd&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) (void)::close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

struct P50ZstdSourceSender::Impl {
    struct CompletedRequest {
        uint64_t raw_bytes = 0;
        Digest128 raw_digest{};
        ZstdSourceTransferResult result{};
    };

    struct PendingReceipt {
        explicit PendingReceipt(boost::asio::any_io_executor executor)
            : notification(std::move(executor)) {}
        R2SentBundle sent{};
        P51SourceArmedFields armed{};
        Clock::time_point deadline{};
        boost::asio::steady_timer notification;
        ClientRunResult client_result{};
        std::optional<bool> system_source_reuse;
        std::exception_ptr failure;
        bool ready = false;
        bool done = false;
    };

    struct WriterWaiter {
        explicit WriterWaiter(boost::asio::any_io_executor executor)
            : notification(std::move(executor)) {}
        boost::asio::steady_timer notification;
        Clock::time_point deadline{};
        bool granted = false;
    };

    struct R2TransferGuard {
        explicit R2TransferGuard(Impl* value) : owner(value) {}
        R2TransferGuard(const R2TransferGuard&) = delete;
        R2TransferGuard& operator=(const R2TransferGuard&) = delete;
        ~R2TransferGuard() { release(); }
        void release() noexcept {
            if (owner) {
                owner->release_r2_transfer();
                owner = nullptr;
            }
        }
        Impl* owner;
    };

    struct R2RequestGuard {
        R2RequestGuard(Impl* value, PrepareRequestKey request_value)
            : owner(value), request(request_value) {}
        R2RequestGuard(const R2RequestGuard&) = delete;
        R2RequestGuard& operator=(const R2RequestGuard&) = delete;
        ~R2RequestGuard() {
            if (!owner) return;
            std::lock_guard lock(owner->r2_transfer_mutex);
            owner->r2_active_requests.erase(request);
        }
        Impl* owner;
        PrepareRequestKey request;
    };

    Impl(CStoreGuid guid, PrepareRequestKey request_value,
         ZstdSourceTransferConfig config_value)
        : c_guid(guid), request(request_value), config(std::move(config_value)) {
        if (!nonzero(c_guid)) throw std::invalid_argument("C store GUID is zero");
        if (!nonzero_request(request))
            throw std::invalid_argument("request/session identity is zero");
        validate_zstd_tu_limits(config.endpoint_caps.zstd);
        if (config.maximum_duration <= Clock::duration::zero())
            throw std::invalid_argument("sender deadline duration is not positive");
        if (config.maximum_duration > std::chrono::seconds(300))
            throw std::invalid_argument("sender deadline duration exceeds five minutes");
        if (config.max_completed_requests == 0)
            throw std::invalid_argument("sender completed-request limit is zero");
        if (config.deadline == Clock::time_point{})
            throw std::invalid_argument("sender requires an absolute deadline");
        if (config.endpoint_caps.zstd.max_raw_bytes > SIZE_MAX)
            throw std::invalid_argument("ZSTD_TU raw limit does not fit this process");
        authority = std::make_shared<P50PreparationAuthority>(
            c_guid, config.endpoint_caps.zstd, config.authority_limits,
            config.compression_level, config.endpoint_caps.profile);
        endpoint = std::make_unique<P50ClientEndpoint>(
            authority, config.endpoint_caps, HistoryNonce{1}, &wire_completions);
    }

    Impl(std::shared_ptr<P50PreparationAuthority> authority_value,
         PreparationRouteKey route_value, PrepareRequestKey request_value,
         ZstdSourceTransferConfig config_value)
        : c_guid(authority_value ? authority_value->c_store_guid() : CStoreGuid{}),
          request(request_value), config(std::move(config_value)),
          authority(std::move(authority_value)), route(std::move(route_value)),
          route_bound(true) {
        if (!authority || c_guid == CStoreGuid{})
            throw std::invalid_argument("sender requires a C preparation authority");
        if (!nonzero_request(request))
            throw std::invalid_argument("request/session identity is zero");
        validate_zstd_tu_limits(config.endpoint_caps.zstd);
        if (config.maximum_duration <= Clock::duration::zero() ||
            config.maximum_duration > std::chrono::seconds(300))
            throw std::invalid_argument("sender deadline duration is invalid");
        if (config.max_completed_requests == 0)
            throw std::invalid_argument("sender completed-request limit is zero");
        if (config.deadline == Clock::time_point{})
            throw std::invalid_argument("sender requires an absolute deadline");
        if (config.endpoint_caps.zstd.max_raw_bytes > SIZE_MAX)
            throw std::invalid_argument("ZSTD_TU raw limit does not fit this process");
        if (config.endpoint_caps.zstd != authority->zstd_limits())
            throw std::invalid_argument("sender and shared authority capabilities differ");
        endpoint = std::make_unique<P50ClientEndpoint>(
            authority, config.endpoint_caps, HistoryNonce{1}, &wire_completions, nullptr,
            std::nullopt, std::function<void(EndpointCancelPermit)>{},
            std::function<void(EndpointCancelPermit, EndpointTerminalResult)>{}, route);
    }

    ZstdSourceTransferResult invalid(ZstdSourceTransferStatus status) const {
        ZstdSourceTransferResult result;
        result.status = status;
        return result;
    }

    void require_replacement(ZstdSourceTransferResult& result,
                             bool persistent_route) noexcept {
        if (!persistent_route)
            return;
        route_replacement_required = true;
        result.replacement_required = true;
    }

    ZstdSourceTransferResult replacement(
        ZstdSourceTransferStatus status, bool persistent_route) noexcept {
        ZstdSourceTransferResult result = invalid(status);
        require_replacement(result, persistent_route);
        return result;
    }

    void quarantine_transport(ZstdSourceTransferResult& result,
                              bool persistent_route) noexcept {
        require_replacement(result, persistent_route);
        if (persistent_route) {
            route_transport_quarantined = true;
            result.route_local_failure = true;
        }
    }

    ZstdSourceTransferResult committed_from_witness(
        const ClientRunResult& run, uint64_t raw_bytes, Digest128 raw_digest,
        uint8_t attempts) const {
        if (!run.committed_commit.has_value() || !run.committed_input.has_value() ||
            run.committed_input->c_store_guid != c_guid ||
            run.committed_input->tu_seq != run.committed_commit->tu_seq ||
            run.committed_commit->raw_digest != raw_digest)
            return invalid(ZstdSourceTransferStatus::CommittedIdentityUnavailable);
        ZstdSourceTransferResult result;
        result.status = ZstdSourceTransferStatus::Committed;
        result.profile = config.endpoint_caps.profile;
        result.committed_input = run.committed_input;
        result.raw_bytes = raw_bytes;
        result.raw_digest = raw_digest;
        result.attempts = attempts;
        return result;
    }

    std::optional<ZstdSourceTransferResult> completed_for(
        PrepareRequestKey key, std::span<const uint8_t> source,
        Digest128 raw_digest) const {
        const auto position = completed.find(key);
        if (position == completed.end())
            return std::nullopt;
        const CompletedRequest& value = position->second;
        if (value.raw_bytes != source.size() || value.raw_digest != raw_digest)
            throw std::invalid_argument(
                "PrepareRequestKey was reused for different input");
        return value.result;
    }

    void remember_completed(PrepareRequestKey key, std::span<const uint8_t> source,
                            Digest128 raw_digest,
                            const ZstdSourceTransferResult& result) {
        if (completed.size() >= config.max_completed_requests)
            throw std::length_error("sender completed-request ledger is full");
        const auto [position, inserted] = completed.emplace(
            key, CompletedRequest{static_cast<uint64_t>(source.size()), raw_digest, result});
        if (!inserted)
            throw std::logic_error("sender completed request was admitted twice");
        (void)position;
    }

    void bind_wire_evidence(ZstdSourceTransferResult& result) const noexcept {
        if (!wire_completions.valid())
            return;
        uint64_t c_to_f_bytes = 0;
        uint64_t f_to_c_bytes = 0;
        for (const AsyncCompletion& completion : wire_completions.completions()) {
            if (completion.stamp.actor != ActorSide::C)
                continue;
            uint64_t* total = nullptr;
            if (completion.stamp.operation == AsyncOperationKind::WriteFragment)
                total = &c_to_f_bytes;
            else if (completion.stamp.operation == AsyncOperationKind::ReadHeader ||
                     completion.stamp.operation == AsyncOperationKind::ReadPayload)
                total = &f_to_c_bytes;
            if (total != nullptr) {
                if (completion.transferred_bytes >
                    std::numeric_limits<uint64_t>::max() - *total)
                    return;
                *total += completion.transferred_bytes;
            }
        }
        result.c_to_f_bytes = c_to_f_bytes;
        result.f_to_c_bytes = f_to_c_bytes;
    }

    PrepareRequestKey begin_transfer() {
        if (config.endpoint_caps.profile == ProfileId::ZSTD_TU) {
            if (used) throw std::logic_error("sender is one-shot");
            used = true;
            return request;
        }
        if (request.request_token == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("route request identity exhausted");
        const PrepareRequestKey current = request;
        ++request.request_token;
        return current;
    }

    CStoreGuid c_guid{};
    PrepareRequestKey request{};
    ZstdSourceTransferConfig config{};
    std::shared_ptr<P50PreparationAuthority> authority;
    PreparationRouteKey route{};
    bool route_bound = false;
    CompletionLog wire_completions;
    std::unique_ptr<P50ClientEndpoint> endpoint;
    std::optional<boost::asio::ip::tcp::socket> r2_socket;
    std::mutex r2_transfer_mutex;
    bool r2_transfer_active = false;
    std::deque<std::shared_ptr<WriterWaiter>> r2_transfer_waiters;
    std::vector<std::shared_ptr<boost::asio::steady_timer>> r2_window_waiters;
    std::deque<std::shared_ptr<PendingReceipt>> r2_receipt_queue;
    // Retain every fully transmitted but not locally verified bundle across a
    // transport loss. The row owns the exact ARMED lease/deadline, codec
    // witness, and PreparedTuHandle/raw source needed by RECOVER/RESET.
    std::map<uint64_t, std::shared_ptr<PendingReceipt>> r2_retained_jobs;
    std::set<PrepareRequestKey> r2_active_requests;
    bool r2_reader_running = false;
    bool r2_ack_pump_running = false;
    uint64_t r2_pending_ack_ordinal = 0;
    uint64_t r2_retained_raw_bytes = 0;
    uint64_t r2_physical_link_generation = 0;
    Id128 r2_relationship_id{};
    uint64_t r2_relationship_epoch = 0;
    uint64_t r2_relationship_ordinal = 1;
    std::map<PrepareRequestKey, CompletedRequest> completed;
    bool used = false;
    bool route_replacement_required = false;
    bool route_transport_quarantined = false;

    void release_r2_transfer() noexcept {
        std::shared_ptr<WriterWaiter> wake;
        {
            std::lock_guard lock(r2_transfer_mutex);
            while (!r2_transfer_waiters.empty()) {
                wake = std::move(r2_transfer_waiters.front());
                r2_transfer_waiters.pop_front();
                if (wake->deadline > Clock::now())
                    break;
                wake.reset();
            }
            if (wake) {
                wake->granted = true;
                // Ownership passes atomically to the FIFO head.
                r2_transfer_active = true;
            } else {
                r2_transfer_active = false;
            }
        }
        if (wake) {
            boost::system::error_code ignored;
            wake->notification.expires_at(Clock::now(), ignored);
        }
    }
};

P50ZstdSourceSender::P50ZstdSourceSender(CStoreGuid c_store_guid,
                                         PrepareRequestKey request,
                                         ZstdSourceTransferConfig config)
    : impl_(std::make_unique<Impl>(c_store_guid, request, std::move(config))) {}

P50ZstdSourceSender::P50ZstdSourceSender(
    std::shared_ptr<P50PreparationAuthority> authority,
    PreparationRouteKey route, PrepareRequestKey request,
    ZstdSourceTransferConfig config)
    : impl_(std::make_unique<Impl>(std::move(authority), route, request,
                                   std::move(config))) {}

P50ZstdSourceSender::~P50ZstdSourceSender() = default;

void P50ZstdSourceSender::retire_for_replacement() noexcept {
    impl_->route_replacement_required = true;
    if (impl_->r2_socket) {
        boost::system::error_code ignored;
        impl_->r2_socket->close(ignored);
    }
}

boost::asio::awaitable<bool> P50ZstdSourceSender::acquire_r2_writer(
    Clock::time_point deadline) {
    const auto executor = co_await boost::asio::this_coro::executor;
    std::shared_ptr<Impl::WriterWaiter> waiter;
    try {
        waiter = std::make_shared<Impl::WriterWaiter>(executor);
        waiter->deadline = deadline;
        waiter->notification.expires_at(deadline);
    } catch (...) {
        co_return false;
    }
    {
        std::lock_guard lock(impl_->r2_transfer_mutex);
        if (!impl_->r2_transfer_active && impl_->r2_transfer_waiters.empty()) {
            impl_->r2_transfer_active = true;
            co_return true;
        }
        impl_->r2_transfer_waiters.push_back(waiter);
    }
    boost::system::error_code wait_error;
    co_await waiter->notification.async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
    {
        std::lock_guard lock(impl_->r2_transfer_mutex);
        if (waiter->granted)
            co_return true;
        std::erase(impl_->r2_transfer_waiters, waiter);
    }
    co_return false;
}

boost::asio::awaitable<void> P50ZstdSourceSender::run_r2_receipt_reader() {
    const auto executor = co_await boost::asio::this_coro::executor;
    if (impl_->config.hold_r2_receipt_reader_for_test) {
        boost::asio::steady_timer test_gate(executor);
        while (impl_->config.hold_r2_receipt_reader_for_test()) {
            test_gate.expires_after(std::chrono::milliseconds(1));
            boost::system::error_code ignored;
            co_await test_gate.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
        }
    }
    for (;;) {
        std::shared_ptr<Impl::PendingReceipt> pending;
        bool wait_for_bundle = false;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (impl_->r2_receipt_queue.empty()) {
                impl_->r2_reader_running = false;
                co_return;
            }
            pending = impl_->r2_receipt_queue.front();
            if (!pending->ready) {
                pending->notification.expires_at(Clock::time_point::max());
                wait_for_bundle = true;
            }
        }
        if (wait_for_bundle) {
            boost::system::error_code wait_error;
            co_await pending->notification.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable,
                                            wait_error));
            continue;
        }

        bool receipt_validated = false;
        try {
            if (!impl_->r2_socket)
                throw std::logic_error("R2 receipt reader lost its live socket");
            pending->client_result = co_await impl_->endpoint->read_r2_receipt(
                *impl_->r2_socket, pending->sent, pending->deadline);
            if (pending->client_result.status != ClientRunStatus::Committed ||
                !pending->client_result.committed_commit)
                throw std::invalid_argument("R2 receipt was not an exact commit");
            if (pending->sent.binding.profile == ProfileId::P29V1)
                pending->system_source_reuse =
                    impl_->authority->p29v1_system_source_reuse(
                        pending->sent.prepared);
            if (impl_->authority->contains(pending->sent.prepared))
                (void)impl_->authority->release(pending->sent.prepared);
            receipt_validated = true;
        } catch (...) {
            pending->failure = std::current_exception();
        }

        std::vector<std::shared_ptr<boost::asio::steady_timer>> wake_window;
        bool start_ack_pump = false;
        if (receipt_validated) {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            impl_->r2_retained_jobs.erase(
                pending->sent.binding.relationship_ordinal);
            impl_->r2_pending_ack_ordinal = std::max(
                impl_->r2_pending_ack_ordinal,
                pending->sent.binding.relationship_ordinal);
            if (!impl_->r2_ack_pump_running) {
                impl_->r2_ack_pump_running = true;
                start_ack_pump = true;
            }
            wake_window.swap(impl_->r2_window_waiters);
        } else {
            impl_->route_replacement_required = true;
            if (impl_->r2_socket) {
                boost::system::error_code ignored;
                impl_->r2_socket->close(ignored);
            }
            std::lock_guard lock(impl_->r2_transfer_mutex);
            wake_window.swap(impl_->r2_window_waiters);
        }

        std::vector<std::shared_ptr<Impl::PendingReceipt>> failed_rows;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (!impl_->r2_receipt_queue.empty() &&
                impl_->r2_receipt_queue.front() == pending)
                impl_->r2_receipt_queue.pop_front();
            pending->done = true;
            if (!receipt_validated) {
                failed_rows.assign(impl_->r2_receipt_queue.begin(),
                                   impl_->r2_receipt_queue.end());
                impl_->r2_receipt_queue.clear();
                impl_->r2_reader_running = false;
                for (const auto& row : failed_rows) {
                    row->failure = pending->failure;
                    row->done = true;
                }
            }
        }
        pending->notification.expires_at(Clock::now());
        for (const auto& timer : wake_window) {
            boost::system::error_code ignored;
            timer->expires_at(Clock::now(), ignored);
        }
        if (start_ack_pump) {
            auto keepalive = shared_from_this();
            boost::asio::co_spawn(
                executor, run_r2_ack_pump(),
                [keepalive = std::move(keepalive)](std::exception_ptr) {});
        }
        if (!receipt_validated) {
            for (const auto& row : failed_rows) {
                row->notification.expires_at(Clock::now());
            }
            co_return;
        }
    }
}

boost::asio::awaitable<void> P50ZstdSourceSender::run_r2_ack_pump() {
    for (;;) {
        uint64_t target = 0;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            target = impl_->r2_pending_ack_ordinal;
            if (target == 0) {
                impl_->r2_ack_pump_running = false;
                co_return;
            }
        }
        const auto deadline = Clock::now() + impl_->config.maximum_duration;
        try {
            if (!co_await acquire_r2_writer(deadline))
                throw boost::system::system_error(boost::asio::error::timed_out);
            Impl::R2TransferGuard writer_guard(impl_.get());
            if (!impl_->r2_socket || !impl_->r2_socket->is_open())
                throw std::logic_error("R2 ACK pump lost its physical link");
            co_await impl_->endpoint->flush_r2_ack(
                *impl_->r2_socket, deadline);
        } catch (...) {
            impl_->route_replacement_required = true;
            if (impl_->r2_socket) {
                boost::system::error_code ignored;
                impl_->r2_socket->close(ignored);
            }
            std::lock_guard lock(impl_->r2_transfer_mutex);
            impl_->r2_ack_pump_running = false;
            co_return;
        }
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (impl_->r2_pending_ack_ordinal <= target)
                impl_->r2_pending_ack_ordinal = 0;
        }
        std::vector<std::shared_ptr<boost::asio::steady_timer>> wake_window;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            wake_window.swap(impl_->r2_window_waiters);
        }
        for (const auto& timer : wake_window) {
            boost::system::error_code ignored;
            timer->expires_at(Clock::now(), ignored);
        }
    }
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer(boost::asio::ip::tcp::endpoint remote,
                               OwnedSourceFd source) {
    const PrepareRequestKey request = impl_->begin_transfer();
    const auto bytes = read_complete_fd(source.get(),
                                        impl_->config.endpoint_caps.zstd.max_raw_bytes);
    if (!bytes)
        return transfer_bytes(ConnectionTarget{remote}, request,
                              impl_->config.deadline, false, {});
    return transfer_bytes(ConnectionTarget{remote}, request, impl_->config.deadline,
                          false,
                          std::make_shared<const std::vector<uint8_t>>(*bytes));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer(boost::asio::ip::tcp::endpoint remote,
                               std::span<const uint8_t> source) {
    const PrepareRequestKey request = impl_->begin_transfer();
    if (source.size() > impl_->config.endpoint_caps.zstd.max_raw_bytes)
        return transfer_bytes(ConnectionTarget{remote}, request,
                              impl_->config.deadline, false, {});
    return transfer_bytes(ConnectionTarget{remote}, request, impl_->config.deadline,
                          false,
                          std::make_shared<const std::vector<uint8_t>>(source.begin(),
                                                                         source.end()));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer(ConnectedFdFactory connection,
                              OwnedSourceFd source) {
    const PrepareRequestKey request = impl_->begin_transfer();
    const auto bytes = read_complete_fd(source.get(),
                                        impl_->config.endpoint_caps.zstd.max_raw_bytes);
    if (!bytes)
        return transfer_bytes(ConnectionTarget{std::move(connection)}, request,
                              impl_->config.deadline, false, {});
    return transfer_bytes(
        ConnectionTarget{std::move(connection)}, request, impl_->config.deadline,
        false,
        std::make_shared<const std::vector<uint8_t>>(*bytes));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer(ConnectedFdFactory connection,
                              std::span<const uint8_t> source) {
    const PrepareRequestKey request = impl_->begin_transfer();
    if (source.size() > impl_->config.endpoint_caps.zstd.max_raw_bytes)
        return transfer_bytes(ConnectionTarget{std::move(connection)}, request,
                              impl_->config.deadline, false, {});
    return transfer_bytes(
        ConnectionTarget{std::move(connection)}, request, impl_->config.deadline,
        false,
        std::make_shared<const std::vector<uint8_t>>(source.begin(), source.end()));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_route(boost::asio::ip::tcp::endpoint remote,
                                    PrepareRequestKey request,
                                    Clock::time_point deadline,
                                    OwnedSourceFd source) {
    const auto bytes = read_complete_fd(source.get(),
                                        impl_->config.endpoint_caps.zstd.max_raw_bytes);
    if (!bytes)
        return transfer_bytes(ConnectionTarget{remote}, request, deadline, true, {});
    return transfer_bytes(ConnectionTarget{remote}, request, deadline, true,
                          std::make_shared<const std::vector<uint8_t>>(*bytes));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_route(boost::asio::ip::tcp::endpoint remote,
                                    PrepareRequestKey request,
                                    Clock::time_point deadline,
                                    std::span<const uint8_t> source) {
    if (source.size() > impl_->config.endpoint_caps.zstd.max_raw_bytes)
        return transfer_bytes(ConnectionTarget{remote}, request, deadline, true, {});
    return transfer_bytes(ConnectionTarget{remote}, request, deadline, true,
                          std::make_shared<const std::vector<uint8_t>>(source.begin(),
                                                                         source.end()));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_route(ConnectedFdFactory connection,
                                    PrepareRequestKey request,
                                    Clock::time_point deadline,
                                    OwnedSourceFd source) {
    const auto bytes = read_complete_fd(source.get(),
                                        impl_->config.endpoint_caps.zstd.max_raw_bytes);
    if (!bytes)
        return transfer_bytes(ConnectionTarget{std::move(connection)}, request,
                              deadline, true, {});
    return transfer_bytes(
        ConnectionTarget{std::move(connection)}, request, deadline, true,
        std::make_shared<const std::vector<uint8_t>>(*bytes));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_route(ConnectedFdFactory connection,
                                    PrepareRequestKey request,
                                    Clock::time_point deadline,
                                    std::span<const uint8_t> source) {
    if (source.size() > impl_->config.endpoint_caps.zstd.max_raw_bytes)
        return transfer_bytes(ConnectionTarget{std::move(connection)}, request,
                              deadline, true, {});
    return transfer_bytes(
        ConnectionTarget{std::move(connection)}, request, deadline, true,
        std::make_shared<const std::vector<uint8_t>>(source.begin(), source.end()));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_route(AsyncConnectedFdFactory connection,
                                    PrepareRequestKey request,
                                    Clock::time_point deadline,
                                    std::span<const uint8_t> source) {
    if (!connection || source.size() >
                           impl_->config.endpoint_caps.zstd.max_raw_bytes)
        co_return impl_->invalid(connection
            ? ZstdSourceTransferStatus::SourceError
            : ZstdSourceTransferStatus::InvalidRequest);
    co_return co_await transfer_bytes(
        ConnectionTarget{std::move(connection)}, request, deadline, true,
        std::make_shared<const std::vector<uint8_t>>(source.begin(), source.end()));
}

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_p51_route(
    P51SourceArmedFields armed, uint64_t physical_link_generation,
    AsyncConnectedFdFactory connection, PrepareRequestKey request,
    Clock::time_point deadline, std::span<const uint8_t> source) {
    const std::optional<ProfileId> armed_profile =
        profile_from_cache_mask(armed.arm.source.cache_profile);
    if (!impl_->route_bound || !armed.valid() || !armed_profile || !connection ||
        physical_link_generation == 0 || !valid_deadline(
            deadline, impl_->config.maximum_duration, Clock::now()) ||
        source.size() > impl_->config.endpoint_caps.zstd.max_raw_bytes ||
        *armed_profile != impl_->config.endpoint_caps.profile ||
        armed.arm.source.assignment_epoch != request.producer_session ||
        armed.arm.source.assignment_nonce != request.request_token)
        co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    {
        std::lock_guard lock(impl_->r2_transfer_mutex);
        // A request key can have only one live owner. In particular, do not
        // let an exact in-flight replay ask the preparation authority for its
        // already-retained handle and emit that TU under a second ordinal.
        // Completed exact replays are handled by completed_for() below.
        if (!impl_->r2_active_requests.insert(request).second)
            co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    }
    Impl::R2RequestGuard request_guard{impl_.get(), request};
    const auto executor = co_await boost::asio::this_coro::executor;
    std::unique_ptr<Impl::R2TransferGuard> writer_guard;
    for (;;) {
        if (!co_await acquire_r2_writer(deadline))
            co_return impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
        writer_guard = std::make_unique<Impl::R2TransferGuard>(impl_.get());
        if (impl_->route_replacement_required)
            co_return impl_->replacement(ZstdSourceTransferStatus::Unavailable, true);
        if (!impl_->r2_socket)
            break;
        try {
            co_await impl_->endpoint->flush_r2_ack(*impl_->r2_socket, deadline);
        } catch (...) {
            impl_->route_replacement_required = true;
            boost::system::error_code ignored;
            impl_->r2_socket->close(ignored);
            co_return impl_->replacement(ZstdSourceTransferStatus::TerminalError,
                                         true);
        }
        if (impl_->endpoint->r2_window_available())
            break;
        auto wait = std::make_shared<boost::asio::steady_timer>(executor);
        wait->expires_at(deadline);
        bool wait_for_credit = false;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (!impl_->endpoint->r2_window_available()) {
                impl_->r2_window_waiters.push_back(wait);
                wait_for_credit = true;
            }
        }
        if (!wait_for_credit)
            break;
        writer_guard.reset();
        boost::system::error_code wait_error;
        co_await wait->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            std::erase(impl_->r2_window_waiters, wait);
        }
        if (Clock::now() >= deadline)
            co_return impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
    }
    const Digest128 raw_digest = digest128(source);
    try {
        if (const auto completed = impl_->completed_for(request, source, raw_digest))
            co_return *completed;
    } catch (...) {
        co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    }
    if (impl_->route_replacement_required)
        co_return impl_->replacement(ZstdSourceTransferStatus::Unavailable, true);
    if (impl_->completed.size() >= impl_->config.max_completed_requests)
        co_return impl_->replacement(ZstdSourceTransferStatus::Unavailable, true);

    PreparedTuHandle prepared;
    try {
        prepared = impl_->authority->prepare_for_route(
            impl_->route, request, source);
    } catch (const P29V1CapabilityUnavailable&) {
        co_return impl_->replacement(ZstdSourceTransferStatus::TerminalError, true);
    } catch (...) {
        co_return impl_->invalid(ZstdSourceTransferStatus::SourceError);
    }
    if (impl_->authority->prepared_profile(prepared) != *armed_profile) {
        try { (void)impl_->authority->release(prepared); } catch (...) {}
        co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    }
    const TuSeq tu_seq = impl_->authority->prepared_tu_seq(prepared);
    JobBind binding;
    binding.reservation_id.bytes = armed.reservation_id;
    binding.physical_link_generation = physical_link_generation;
    binding.wire_job_id = armed.arm.source.wire_job_id;
    binding.assignment_epoch = armed.arm.source.assignment_epoch;
    binding.assignment_nonce = armed.arm.source.assignment_nonce;
    binding.logical_job = armed.arm.source.logical_job;
    binding.compiler_attempt = armed.arm.source.compiler_attempt;
    binding.source_request_id = armed.arm.source.source_request_id;
    binding.tu_seq = tu_seq;
    binding.profile = *armed_profile;
    binding.raw_bytes = source.size();
    binding.raw_digest = raw_digest;

    LinkHello hello;
    hello.profile = binding.profile;
    hello.window = armed.selected_window;
    hello.max_frame_payload = impl_->config.endpoint_caps.wire.max_frame_payload;
    hello.max_raw_bytes = impl_->config.endpoint_caps.zstd.max_raw_bytes;
    hello.max_encoded_bytes = impl_->config.endpoint_caps.zstd.max_encoded_body_bytes;
    hello.max_output_bytes = impl_->config.endpoint_caps.zstd.max_raw_bytes;
    hello.reservation_id = Id128{armed.reservation_id};
    hello.relationship_id = Id128{armed.logical_relationship_id};
    hello.relationship_epoch = armed.relationship_epoch;
    hello.physical_link_generation = physical_link_generation;
    hello.c_store_guid = CStoreGuid{armed.arm.source.c_store_guid};
    hello.c_store_generation = armed.arm.source.c_store_generation;
    hello.f_store_guid = FStoreGuid{armed.f_store_guid};
    hello.f_store_generation = armed.f_store_generation;
    hello.c_control_generation = armed.arm.source.c_control_generation;
    hello.c_control_attempt = armed.arm.source.c_control_attempt;
    hello.system_source_fingerprint = binding.profile == ProfileId::P29V1
        ? impl_->authority->p29v1_system_source_fingerprint(prepared)
        : Digest128{};
    hello.history_nonce = HistoryNonce{1};

    std::shared_ptr<Impl::PendingReceipt> pending;
    bool bundle_complete = false;
    try {
        if (impl_->route_replacement_required)
            throw std::runtime_error("R2 link is quarantined");
        if (impl_->r2_socket &&
            (impl_->r2_physical_link_generation != physical_link_generation ||
             impl_->r2_relationship_id != hello.relationship_id ||
             impl_->r2_relationship_epoch != hello.relationship_epoch))
            throw std::invalid_argument("R2 relationship changed on retained socket");
        if (!impl_->r2_socket) {
            const int fd = co_await await_connected_fd(connection, deadline);
            if (fd < 0)
                throw std::runtime_error("R2 link connector failed");
            boost::system::error_code error;
            auto socket = P50ClientEndpoint::adopt_connected_fd(executor, fd, error);
            if (!socket) {
                impl_->route_replacement_required = true;
                throw std::runtime_error("R2 link socket adoption failed");
            }
            impl_->r2_socket = std::move(*socket);
            impl_->r2_physical_link_generation = physical_link_generation;
            impl_->r2_relationship_id = hello.relationship_id;
            impl_->r2_relationship_epoch = hello.relationship_epoch;
            const LinkState state = co_await impl_->endpoint->open_r2_link(
                *impl_->r2_socket, hello, deadline);
            if (state.f_store_guid != hello.f_store_guid ||
                state.f_store_generation != hello.f_store_generation)
                throw std::invalid_argument("R2 F identity differs from ARMED");
        }

        // ACK control has priority at every complete-bundle boundary. The F
        // side admits against K-Q, so never send a new JOB_BIND on speculative
        // confirmation credit which has not been cumulatively acknowledged.
        co_await impl_->endpoint->flush_r2_ack(*impl_->r2_socket, deadline);
        if (!impl_->endpoint->r2_window_available())
            throw std::logic_error("R2 window was consumed during writer turn");

        binding.relationship_ordinal = impl_->r2_relationship_ordinal;
        pending = std::make_shared<Impl::PendingReceipt>(executor);
        pending->armed = armed;
        pending->deadline = deadline;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            pending->notification.expires_at(Clock::time_point::max());
            const auto [retained, inserted] = impl_->r2_retained_jobs.emplace(
                binding.relationship_ordinal, pending);
            (void)retained;
            if (!inserted)
                throw std::logic_error("duplicate retained R2 relationship ordinal");
            impl_->r2_receipt_queue.push_back(pending);
        }
        // The authority and endpoint reserve their exact receipt witness before
        // the first JOB_BIND byte is made visible on the stream.
        pending->sent = co_await impl_->endpoint->write_r2_bundle(
            *impl_->r2_socket, binding, prepared, deadline);
        bundle_complete = true;
        ++impl_->r2_relationship_ordinal;
        if (impl_->config.after_r2_bundle_sent_for_test) {
            try {
                impl_->config.after_r2_bundle_sent_for_test(
                    pending->sent.binding.relationship_ordinal);
            } catch (...) {
                // Diagnostic test observation cannot alter transfer state.
            }
        }
        bool start_reader = false;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            pending->ready = true;
            if (!impl_->r2_reader_running) {
                impl_->r2_reader_running = true;
                start_reader = true;
            }
        }
        pending->notification.expires_at(Clock::now());
        writer_guard.reset();
        if (start_reader) {
            auto keepalive = shared_from_this();
            boost::asio::co_spawn(
                executor, run_r2_receipt_reader(),
                [keepalive = std::move(keepalive)](std::exception_ptr) {});
        }

      for (;;) {
          {
              std::lock_guard lock(impl_->r2_transfer_mutex);
              if (pending->done)
                  break;
              pending->notification.expires_at(deadline);
          }
          boost::system::error_code wait_error;
          co_await pending->notification.async_wait(
              boost::asio::redirect_error(boost::asio::use_awaitable,
                                          wait_error));
          if (Clock::now() >= deadline) {
              std::lock_guard lock(impl_->r2_transfer_mutex);
              if (!pending->done)
                  throw boost::system::system_error(boost::asio::error::timed_out);
          }
      }
        if (pending->failure)
            std::rethrow_exception(pending->failure);
      if (pending->client_result.status != ClientRunStatus::Committed ||
          !pending->client_result.committed_commit)
          throw std::invalid_argument("R2 receipt was not an exact commit");

      if (!co_await acquire_r2_writer(deadline))
          throw boost::system::system_error(boost::asio::error::timed_out);
      {
          Impl::R2TransferGuard writer_guard(impl_.get());
          try {
              if (impl_->r2_socket)
                  co_await impl_->endpoint->flush_r2_ack(
                      *impl_->r2_socket, deadline);
          } catch (...) {
              // Exact TX_COMMIT was already validated and accepted. Failure
              // to return flow-control credit quarantines only this link.
              impl_->route_replacement_required = true;
              if (impl_->r2_socket) {
                  boost::system::error_code ignored;
                  impl_->r2_socket->close(ignored);
              }
          }
      }
        ZstdSourceTransferResult result;
        result.status = ZstdSourceTransferStatus::Committed;
        result.profile = binding.profile;
        result.committed_input = pending->client_result.committed_input;
        result.raw_bytes = source.size();
        result.raw_digest = raw_digest;
        result.attempts = 1;
        if (binding.profile == ProfileId::P29V1)
            result.system_source_reuse = pending->system_source_reuse;
        impl_->remember_completed(request, source, raw_digest, result);
        co_return result;
    } catch (...) {
        if (pending && !bundle_complete) {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            auto position = std::find(impl_->r2_receipt_queue.begin(),
                                      impl_->r2_receipt_queue.end(), pending);
            if (position != impl_->r2_receipt_queue.end())
                impl_->r2_receipt_queue.erase(position);
            if (pending->sent.binding.relationship_ordinal != 0)
                impl_->r2_retained_jobs.erase(
                    pending->sent.binding.relationship_ordinal);
            pending->failure = std::current_exception();
            pending->done = true;
            pending->notification.expires_at(Clock::now());
        }
        if (!bundle_complete) {
            try {
                if (impl_->authority->contains(prepared)) {
                    const auto release_count = impl_->authority->release(prepared);
                    (void)release_count;
                }
            } catch (...) {}
        } else {
            // Keep the retained TU/witness alive for the independent reader.
            // A caller timeout must not tear down a possibly committed bundle.
            impl_->route_replacement_required = true;
            co_return impl_->replacement(ZstdSourceTransferStatus::TerminalError,
                                         true);
        }
        impl_->route_replacement_required = true;
        if (impl_->r2_socket) {
            boost::system::error_code ignored;
            impl_->r2_socket->close(ignored);
        }
        co_return impl_->replacement(ZstdSourceTransferStatus::TerminalError, true);
    }
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
#pragma GCC diagnostic pop
#endif

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
    // GCC 13 can diagnose Boost.Asio's awaitable-frame allocator as a
    // mismatched new/delete when this coroutine is inlined (GCC PR103993).
    // Boost 1.83 pairs awaitable_frame_tag allocate/deallocate, ultimately
    // using aligned_alloc/aligned_free; keep this workaround local to the
    // coroutine and leave all other -Werror diagnostics enabled.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_bytes(
    ConnectionTarget target,
    PrepareRequestKey request,
    Clock::time_point deadline,
    bool explicit_route,
    std::shared_ptr<const std::vector<uint8_t>> source) {
    if (!valid_deadline(deadline, impl_->config.maximum_duration, Clock::now()))
        co_return impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
    if (!nonzero_request(request) ||
        (explicit_route && impl_->config.endpoint_caps.profile != ProfileId::ZSTD_TU &&
         !route_history_profile(impl_->config.endpoint_caps.profile)))
        co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    if (std::holds_alternative<boost::asio::ip::tcp::endpoint>(target)) {
        const auto remote = std::get<boost::asio::ip::tcp::endpoint>(target);
        if (remote.port() == 0 || remote.address().is_unspecified())
            co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    } else if (std::holds_alternative<ConnectedFdFactory>(target)) {
        if (!std::get<ConnectedFdFactory>(target))
            co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    } else if (!std::get<AsyncConnectedFdFactory>(target)) {
        co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    }
    if (!source)
        co_return impl_->invalid(ZstdSourceTransferStatus::SourceError);

    const Digest128 raw_digest = digest128(*source);
    try {
        if (const auto completed = impl_->completed_for(request, *source, raw_digest))
            co_return *completed;
    } catch (const std::invalid_argument&) {
        co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    }
    /* Exact completed replay remains connection-free even after the bounded
       ledger reaches its ceiling.  A distinct request cannot be admitted:
       make that capacity boundary a sticky cold-replacement request rather
       than silently disabling P50 for the remainder of the process. */
    if (explicit_route && impl_->route_replacement_required) {
        auto result = impl_->replacement(ZstdSourceTransferStatus::Unavailable,
                                         explicit_route);
        result.route_local_failure = impl_->route_transport_quarantined;
        co_return result;
    }
    if (impl_->completed.size() >= impl_->config.max_completed_requests)
        co_return impl_->replacement(ZstdSourceTransferStatus::Unavailable,
                                     explicit_route);

    // One route owner serializes its transfers. Retain only this operation's
    // completions so byte accounting is bounded and includes either retry.
    impl_->wire_completions.clear();

    PreparedTuHandle prepared;
    try {
        if (impl_->config.before_prepare_for_route_for_test)
            impl_->config.before_prepare_for_route_for_test();
        prepared = impl_->route_bound
            ? impl_->authority->prepare_for_route(impl_->route, request, *source)
            : impl_->authority->prepare(request, *source);
    } catch (const P50RoutePoisoned&) {
        // begin_v1() throws this only after terminalizing retained route
        // state.  Preserve that typed ownership boundary: a wrapper cannot
        // reinterpret it as bad input or continue through the same sidecar.
        co_return impl_->replacement(ZstdSourceTransferStatus::TerminalError,
                                     explicit_route);
    } catch (const std::invalid_argument&) {
        co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    } catch (const std::length_error&) {
        co_return impl_->invalid(ZstdSourceTransferStatus::SourceError);
    } catch (const std::logic_error&) {
        // A relationship permits only one uncommitted successor.  A later
        // wrapper may be the first observer after its predecessor died before
        // publishing the poisoned-route result.  Escalate the retained
        // ambiguity to the same sticky cold-replacement path.
        co_return impl_->replacement(ZstdSourceTransferStatus::InvalidRequest,
                                     explicit_route);
    }
    for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
        if (Clock::now() >= deadline) {
            ZstdSourceTransferResult result =
                impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
            result.attempts = static_cast<uint8_t>(attempt - 1);
            impl_->quarantine_transport(result, explicit_route);
            co_return result;
        }
        ClientRunResult run;
        try {
            if (std::holds_alternative<boost::asio::ip::tcp::endpoint>(target)) {
                run = co_await impl_->endpoint->run(
                    std::get<boost::asio::ip::tcp::endpoint>(target), prepared, {},
                    deadline);
            } else {
                int connected_fd = -1;
                try {
                    if (std::holds_alternative<AsyncConnectedFdFactory>(target))
                        connected_fd = co_await await_connected_fd(
                            std::get<AsyncConnectedFdFactory>(target), deadline);
                    else
                        connected_fd = std::get<ConnectedFdFactory>(target)(
                            deadline);
                } catch (...) {
                    ZstdSourceTransferResult result =
                        impl_->invalid(ZstdSourceTransferStatus::TerminalError);
                    result.attempts = attempt;
                    impl_->require_replacement(result, explicit_route);
                    co_return result;
                }
                if (connected_fd < 0) {
                    run.status = Clock::now() >= deadline
                                     ? ClientRunStatus::DeadlineExceeded
                                     : ClientRunStatus::Disconnected;
                } else {
                    run = co_await impl_->endpoint->run_adopted_fd(
                        connected_fd, prepared, {}, deadline);
                }
            }
        } catch (...) {
            ZstdSourceTransferResult result =
                impl_->invalid(ZstdSourceTransferStatus::TerminalError);
            result.attempts = attempt;
            impl_->require_replacement(result, explicit_route);
            co_return result;
        }
        // A validated commit is authoritative even when its completion races
        // the deadline boundary.  The endpoint freezes this witness before
        // its timer may close the owned socket, so a later clock sample must
        // never discard it.
        if (run.status == ClientRunStatus::Committed) {
            ZstdSourceTransferResult result = impl_->committed_from_witness(
                run, source->size(), raw_digest, attempt);
            impl_->bind_wire_evidence(result);
            if (result.status == ZstdSourceTransferStatus::Committed &&
                result.profile == ProfileId::P29V1) {
                try {
                    result.system_source_reuse =
                        impl_->authority->p29v1_system_source_reuse(prepared);
                } catch (...) {
                    result.status =
                        ZstdSourceTransferStatus::CommittedIdentityUnavailable;
                }
                if (!result.system_source_reuse.has_value())
                    result.status =
                        ZstdSourceTransferStatus::CommittedIdentityUnavailable;
            }
            // Endpoint commit consumes the transaction but intentionally does
            // not own the preparation reference.  Release it only after the
            // commit witness is frozen; failed runs retain the exact handle
            // for the bounded retry/reconciliation path.
            try {
                (void)impl_->authority->release(prepared);
            } catch (...) {
                result.status =
                    ZstdSourceTransferStatus::CommittedIdentityUnavailable;
            }
            if (result.status != ZstdSourceTransferStatus::Committed) {
                impl_->require_replacement(result, explicit_route);
                co_return result;
            }
            try {
                impl_->remember_completed(request, *source, raw_digest, result);
            } catch (const std::length_error&) {
                co_return impl_->replacement(
                    ZstdSourceTransferStatus::Unavailable, explicit_route);
            } catch (...) {
                co_return impl_->replacement(
                    ZstdSourceTransferStatus::CommittedIdentityUnavailable,
                    explicit_route);
            }
            co_return result;
        }
        if (run.status == ClientRunStatus::DeadlineExceeded) {
            ZstdSourceTransferResult result =
                impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
            result.attempts = attempt;
            impl_->bind_wire_evidence(result);
            impl_->quarantine_transport(result, explicit_route);
            co_return result;
        }
        if (run.status == ClientRunStatus::TerminalError) {
            ZstdSourceTransferResult result =
                impl_->invalid(ZstdSourceTransferStatus::TerminalError);
            result.attempts = attempt;
            result.terminal_error = run.terminal_error;
            impl_->bind_wire_evidence(result);
            impl_->require_replacement(result, explicit_route);
            co_return result;
        }
        if (attempt == 2) {
            ZstdSourceTransferResult result =
                impl_->invalid(ZstdSourceTransferStatus::RetryExhausted);
            result.attempts = attempt;
            impl_->bind_wire_evidence(result);
            impl_->quarantine_transport(result, explicit_route);
            co_return result;
        }
    }
    co_return impl_->replacement(ZstdSourceTransferStatus::RetryExhausted,
                                 explicit_route);
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
#pragma GCC diagnostic pop
#endif

}  // namespace icecc::p50
