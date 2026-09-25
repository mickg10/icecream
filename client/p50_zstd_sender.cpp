#include "p50_zstd_sender.h"
#include "services/p50_cache_profile_mask.h"

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

bool exact_commit_matches(const TxCommit& commit, const TxBegin& begin) {
    return commit.history_nonce == begin.history_nonce &&
           commit.rel_seq == begin.rel_seq && commit.tu_seq == begin.tu_seq &&
           commit.transaction_digest == begin.transaction_digest &&
           commit.raw_digest == begin.raw_digest &&
           commit.post_state_digest == compute_post_state_digest(
               begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
               begin.tu_seq, begin.transaction_digest);
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
        PrepareRequestKey request{};
        boost::asio::steady_timer notification;
        ClientRunResult client_result{};
        std::optional<bool> system_source_reuse;
        std::exception_ptr failure;
        uint64_t failure_physical_link_generation = 0;
        bool replayed_after_reset = false;
        bool unavailable_by_reset = false;
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
        ~R2RequestGuard() { if (owner) owner->finish_r2_request(request); }
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
                             bool persistent_route,
                             ReplacementTrigger trigger =
                                 ReplacementTrigger::Unattributed) noexcept {
        if (!persistent_route)
            return;
        if (!route_replacement_required) {
            route_replacement_trigger = trigger;
        }
        route_replacement_required = true;
        wake_r2_recovery_waiters();
        wake_r2_completed_capacity_waiters();
        result.replacement_required = true;
        result.replacement_trigger = route_replacement_trigger;
    }

    ZstdSourceTransferResult replacement(
        ZstdSourceTransferStatus status, bool persistent_route,
        ReplacementTrigger trigger =
            ReplacementTrigger::Unattributed) noexcept {
        ZstdSourceTransferResult result = invalid(status);
        require_replacement(result, persistent_route, trigger);
        return result;
    }

    ZstdSourceTransferResult r2_route_replacement_result(
        ZstdSourceTransferStatus status) noexcept {
        ZstdSourceTransferResult result = replacement(status, true);
        result.replacement_trigger = route_replacement_trigger;
        std::lock_guard lock(r2_transfer_mutex);
        if (r2_terminal_rejection) {
            result.status = ZstdSourceTransferStatus::TerminalError;
            result.replacement_required = false;
            result.route_local_failure = true;
            result.r2_link_rejection = r2_terminal_rejection;
        }
        return result;
    }

    std::optional<ZstdSourceRouteRejection> current_r2_route_rejection() {
        std::lock_guard lock(r2_transfer_mutex);
        return r2_terminal_rejection;
    }

    void quarantine_transport(ZstdSourceTransferResult& result,
                              bool persistent_route) noexcept {
        require_replacement(result, persistent_route);
        if (persistent_route) {
            route_transport_quarantined = true;
            result.route_local_failure = true;
        }
    }

    std::optional<ZstdSourceRouteRejection> accept_r2_link_rejection(
        const R2LinkRejected& rejected) noexcept {
        const bool known_reason =
            rejected.rejection.reason == LinkRejectReason::StoreReplaced ||
            rejected.rejection.reason == LinkRejectReason::ReservationMissing;
        // Endpoint verifies the canonical offer digest. Require the full
        // offered record to match this sender's exact in-flight handshake too.
        if (!known_reason || !r2_attempted_offer ||
            rejected.offered != *r2_attempted_offer)
            return std::nullopt;
        const ZstdSourceRouteRejection route_rejection{
            rejected.rejection.reason, rejected.offered};
        {
            std::lock_guard lock(r2_transfer_mutex);
            r2_terminal_rejection = route_rejection;
            if (!route_replacement_required)
                route_replacement_trigger = ReplacementTrigger::Unattributed;
            route_replacement_required = true;
            route_transport_quarantined = true;
            r2_recovery_required = false;
        }
        wake_r2_recovery_waiters();
        wake_r2_completed_capacity_waiters();
        if (r2_socket) {
            boost::system::error_code ignored;
            r2_socket->close(ignored);
        }
        return route_rejection;
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
        std::lock_guard lock(r2_transfer_mutex);
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
        remember_completed_witness(key, static_cast<uint64_t>(source.size()),
                                   raw_digest, result);
    }

    void remember_completed_witness(PrepareRequestKey key, uint64_t raw_bytes,
                                    Digest128 raw_digest,
                                    const ZstdSourceTransferResult& result) {
        {
            std::lock_guard lock(r2_transfer_mutex);
            const bool has_reservation =
                r2_completed_slot_reservations.contains(key);
            if (!has_reservation &&
                completed.size() + r2_completed_slot_reservations.size() >=
                    config.max_completed_requests)
                throw std::length_error("sender completed-request ledger is full");
            const auto [position, inserted] = completed.emplace(
                key, CompletedRequest{raw_bytes, raw_digest, result});
            if (!inserted)
                throw std::logic_error(
                    "sender completed request was admitted twice");
            (void)position;
            if (has_reservation)
                r2_completed_slot_reservations.erase(key);
        }
        wake_r2_completed_capacity_waiters();
    }

    void bind_wire_evidence(ZstdSourceTransferResult& result) const noexcept {
        if (!wire_completions.valid())
            return;
        result.c_to_f_bytes = wire_completions.c_to_f_bytes();
        result.f_to_c_bytes = wire_completions.f_to_c_bytes();
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
    CompletionLog wire_completions{CompletionLog::StorageMode::ClientByteTotals};
    std::unique_ptr<P50ClientEndpoint> endpoint;
    std::optional<boost::asio::ip::tcp::socket> r2_socket;
    mutable std::mutex r2_transfer_mutex;
    bool r2_transfer_active = false;
    std::deque<std::shared_ptr<WriterWaiter>> r2_transfer_waiters;
    std::vector<std::shared_ptr<boost::asio::steady_timer>> r2_window_waiters;
    std::vector<std::shared_ptr<boost::asio::steady_timer>>
        r2_completed_capacity_waiters;
    // One relationship-wide reconnect gate. All transfer callers observe the
    // same deadline so a W30 suffix cannot multiply immediate reconnects.
    std::vector<std::shared_ptr<boost::asio::steady_timer>> r2_recovery_waiters;
    Clock::time_point r2_recovery_retry_not_before{};
    unsigned r2_recovery_failures = 0;
    std::deque<std::shared_ptr<PendingReceipt>> r2_receipt_queue;
    // Retain every fully transmitted but not locally verified bundle across a
    // transport loss. The row owns the exact ARMED lease/deadline, codec
    // witness, and PreparedTuHandle/raw source needed by RECOVER/RESET.
    std::map<uint64_t, std::shared_ptr<PendingReceipt>> r2_retained_jobs;
    std::set<PrepareRequestKey> r2_active_requests;
    // One bounded completed-ledger slot is reserved before an R2 caller can
    // stage a bundle. A fully-sent unresolved witness keeps its reservation
    // after the caller returns, until exact commit or terminal cleanup.
    std::set<PrepareRequestKey> r2_completed_slot_reservations;
    bool r2_reader_running = false;
    uint64_t r2_reader_generation = 0;
    bool r2_ack_pump_running = false;
    uint64_t r2_ack_pump_generation = 0;
    uint64_t r2_pending_ack_ordinal = 0;
    uint64_t r2_retained_raw_bytes = 0;
    uint64_t r2_physical_link_generation = 0;
    uint64_t r2_attempted_physical_generation = 0;
    uint64_t r2_failed_physical_generation = 0;
    Id128 r2_relationship_id{};
    uint64_t r2_relationship_epoch = 0;
    uint64_t r2_relationship_ordinal = 1;
    std::optional<LinkHello> r2_hello;
    std::optional<LinkHello> r2_attempted_offer;
    std::optional<ZstdSourceRouteRejection> r2_terminal_rejection;
    bool r2_recovery_required = false;
    uint64_t r2_recovery_floor = 0;
    Id128 r2_recovery_operation{};
    uint64_t r2_recovery_new_epoch = 0;
    HistoryNonce r2_recovery_new_nonce{};
    uint64_t next_recovery_operation = 1;
    std::map<PrepareRequestKey, CompletedRequest> completed;
    bool used = false;
    bool route_replacement_required = false;
    ReplacementTrigger route_replacement_trigger =
        ReplacementTrigger::Unattributed;
    bool route_transport_quarantined = false;

    void wake_r2_completed_capacity_waiters() noexcept {
        std::vector<std::shared_ptr<boost::asio::steady_timer>> wake;
        {
            std::lock_guard lock(r2_transfer_mutex);
            wake.swap(r2_completed_capacity_waiters);
        }
        for (const auto& timer : wake) {
            boost::system::error_code ignored;
            timer->expires_at(Clock::now(), ignored);
        }
    }

    void finish_r2_request(PrepareRequestKey key) noexcept {
        bool released_slot = false;
        {
            std::lock_guard lock(r2_transfer_mutex);
            r2_active_requests.erase(key);
            const bool retained = std::any_of(
                r2_retained_jobs.begin(), r2_retained_jobs.end(),
                [key](const auto& entry) {
                    return entry.second && entry.second->request == key;
                });
            if (!retained)
                released_slot = r2_completed_slot_reservations.erase(key) != 0;
        }
        if (released_slot)
            wake_r2_completed_capacity_waiters();
    }

    bool retire_expired_r2_witnesses() noexcept {
        bool expired = false;
        {
            std::lock_guard lock(r2_transfer_mutex);
            const auto now = Clock::now();
            expired = std::any_of(
                r2_retained_jobs.begin(), r2_retained_jobs.end(),
                [now](const auto& entry) {
                    const auto& pending = entry.second;
                    if (!pending || pending->deadline > now)
                        return false;
                    return !(pending->done && !pending->failure &&
                             pending->client_result.status ==
                                 ClientRunStatus::Committed &&
                             pending->client_result.committed_commit.has_value());
                });
            if (expired && !route_replacement_required) {
                // An unresolved exact witness cannot be replayed once its
                // original ARM deadline has elapsed. Cold-retire this sender
                // instead of allowing repeated fresh callers to spin through
                // failed recovery attempts or extending that deadline.
                route_replacement_required = true;
                route_replacement_trigger =
                    ReplacementTrigger::ExpiredUnresolvedWitness;
                route_transport_quarantined = true;
            } else {
                expired = false;
            }
        }
        if (expired) {
            if (r2_socket) {
                boost::system::error_code ignored;
                r2_socket->close(ignored);
            }
            wake_r2_recovery_waiters();
            wake_r2_completed_capacity_waiters();
        }
        return expired;
    }

    bool completed_ledger_full() const noexcept {
        std::lock_guard lock(r2_transfer_mutex);
        return completed.size() >= config.max_completed_requests;
    }

    boost::asio::awaitable<bool> reserve_r2_completed_slot(
        PrepareRequestKey key, Clock::time_point deadline,
        bool& permanently_full, bool& recovery_required) {
        const auto executor = co_await boost::asio::this_coro::executor;
        permanently_full = false;
        recovery_required = false;
        for (;;) {
            if (Clock::now() >= deadline)
                co_return false;
            if (retire_expired_r2_witnesses())
                co_return false;
            std::shared_ptr<boost::asio::steady_timer> waiter;
            Clock::time_point wait_deadline = deadline;
            {
                std::lock_guard lock(r2_transfer_mutex);
                if (route_replacement_required)
                    co_return false;
                if (r2_completed_slot_reservations.contains(key))
                    co_return true;  // The unresolved exact witness owns it.
                if (r2_recovery_required) {
                    recovery_required = true;
                    co_return false;
                }
                if (completed.size() >= config.max_completed_requests) {
                    permanently_full = true;
                    co_return false;
                }
                if (completed.size() + r2_completed_slot_reservations.size() <
                    config.max_completed_requests) {
                    r2_completed_slot_reservations.insert(key);
                    co_return true;
                }
                for (const auto& [ordinal, pending] : r2_retained_jobs) {
                    (void)ordinal;
                    const bool positive = pending && pending->done &&
                        !pending->failure &&
                        pending->client_result.status ==
                            ClientRunStatus::Committed &&
                        pending->client_result.committed_commit.has_value();
                    if (pending && !positive)
                        wait_deadline = std::min(wait_deadline,
                                                 pending->deadline);
                }
                if (r2_completed_capacity_waiters.size() >=
                    config.max_completed_requests)
                    co_return false;
                try {
                    waiter = std::make_shared<boost::asio::steady_timer>(executor);
                    waiter->expires_at(wait_deadline);
                    r2_completed_capacity_waiters.push_back(waiter);
                } catch (...) {
                    co_return false;
                }
            }
            if (config.after_r2_completed_capacity_waiter_registered_for_test) {
                try {
                    config.after_r2_completed_capacity_waiter_registered_for_test(key);
                } catch (...) {
                    std::lock_guard lock(r2_transfer_mutex);
                    std::erase(r2_completed_capacity_waiters, waiter);
                    co_return false;
                }
            }
            boost::system::error_code wait_error;
            co_await waiter->async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable,
                                            wait_error));
            {
                std::lock_guard lock(r2_transfer_mutex);
                std::erase(r2_completed_capacity_waiters, waiter);
            }
        }
    }

    void wake_r2_recovery_waiters() noexcept {
        std::vector<std::shared_ptr<boost::asio::steady_timer>> wake;
        {
            std::lock_guard lock(r2_transfer_mutex);
            wake.swap(r2_recovery_waiters);
        }
        for (const auto& timer : wake) {
            boost::system::error_code ignored;
            timer->expires_at(Clock::now(), ignored);
        }
    }

    void note_r2_recovery_failure() noexcept {
        {
            std::lock_guard lock(r2_transfer_mutex);
            constexpr auto kBase = std::chrono::milliseconds(5);
            constexpr auto kMaximum = std::chrono::milliseconds(500);
            const unsigned exponent = std::min(r2_recovery_failures, 7U);
            const auto delay = std::min(kMaximum, kBase * (1U << exponent));
            if (r2_recovery_failures < 7U)
                ++r2_recovery_failures;
            r2_recovery_retry_not_before = Clock::now() + delay;
        }
        wake_r2_completed_capacity_waiters();
    }

    void reset_r2_recovery_backoff() noexcept {
        {
            std::lock_guard lock(r2_transfer_mutex);
            r2_recovery_failures = 0;
            r2_recovery_retry_not_before = Clock::time_point{};
        }
        wake_r2_recovery_waiters();
    }

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
    impl_->wake_r2_recovery_waiters();
    impl_->wake_r2_completed_capacity_waiters();
    if (impl_->r2_socket) {
        boost::system::error_code ignored;
        impl_->r2_socket->close(ignored);
    }
}

size_t P50ZstdSourceSender::retained_completion_records_for_test() const noexcept {
    return impl_->wire_completions.retained_record_count();
}

uint64_t P50ZstdSourceSender::current_r2_physical_generation() const noexcept {
    return std::max(impl_->r2_physical_link_generation,
                    impl_->r2_attempted_physical_generation);
}

bool P50ZstdSourceSender::can_rebind_r2_relationship() const noexcept {
    std::lock_guard lock(impl_->r2_transfer_mutex);
    return !impl_->r2_transfer_active && impl_->r2_transfer_waiters.empty() &&
           impl_->r2_receipt_queue.empty() && impl_->r2_retained_jobs.empty() &&
           impl_->r2_active_requests.empty() &&
           !impl_->r2_recovery_required &&
           impl_->r2_pending_ack_ordinal == 0 &&
           !impl_->r2_ack_pump_running;
}

bool P50ZstdSourceSender::r2_rebind_waitable() const noexcept {
    std::lock_guard lock(impl_->r2_transfer_mutex);
    const bool no_unsettled_bundle = impl_->r2_receipt_queue.empty() &&
        impl_->r2_retained_jobs.empty() && !impl_->r2_recovery_required;
    const bool draining_ack = impl_->r2_pending_ack_ordinal != 0 ||
        impl_->r2_ack_pump_running;
    return no_unsettled_bundle && impl_->r2_transfer_waiters.empty() &&
        (draining_ack || (!impl_->r2_transfer_active &&
                          !impl_->r2_active_requests.empty()));
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

boost::asio::awaitable<bool> P50ZstdSourceSender::wait_for_r2_recovery_retry(
    Clock::time_point deadline) {
    co_return co_await wait_for_r2_retry_not_before(deadline, true);
}

boost::asio::awaitable<bool> P50ZstdSourceSender::wait_for_r2_connect_retry(
    Clock::time_point deadline) {
    co_return co_await wait_for_r2_retry_not_before(deadline, false);
}

boost::asio::awaitable<bool> P50ZstdSourceSender::wait_for_r2_retry_not_before(
    Clock::time_point deadline, bool require_recovery) {
    const auto executor = co_await boost::asio::this_coro::executor;
    for (;;) {
        if (Clock::now() >= deadline)
            co_return false;
        std::shared_ptr<boost::asio::steady_timer> waiter;
        Clock::duration retry_delay{};
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (impl_->route_replacement_required)
                co_return false;
            if ((require_recovery && !impl_->r2_recovery_required) ||
                impl_->r2_recovery_retry_not_before <= Clock::now())
                co_return true;
            waiter = std::make_shared<boost::asio::steady_timer>(executor);
            retry_delay = impl_->r2_recovery_retry_not_before - Clock::now();
            waiter->expires_at(std::min(
                deadline, impl_->r2_recovery_retry_not_before));
            impl_->r2_recovery_waiters.push_back(waiter);
        }
        if (impl_->config.after_r2_recovery_waiter_registered_for_test)
            impl_->config.after_r2_recovery_waiter_registered_for_test(
                retry_delay);
        boost::system::error_code wait_error;
        co_await waiter->async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable,
                                        wait_error));
        bool replaced = false;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            std::erase(impl_->r2_recovery_waiters, waiter);
            replaced = impl_->route_replacement_required;
        }
        if (replaced || Clock::now() >= deadline)
            co_return false;
        // A preceding coordinator may have failed again and advanced the
        // shared timestamp while this caller was asleep; re-read it.
    }
}

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
// Boost.Asio pairs its awaitable-frame class new/delete through the same
// tagged allocator; GCC 13 can nevertheless diagnose the inlined aligned
// allocator as mismatched (PR103993). Keep this suppression scoped to this
// coroutine, as with the existing sender coroutine scopes below.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
boost::asio::awaitable<void> P50ZstdSourceSender::run_r2_receipt_reader(
    uint64_t physical_link_generation) {
    const auto executor = co_await boost::asio::this_coro::executor;
    struct QuiescentNotify {
        const std::function<void()>* callback;
        ~QuiescentNotify() noexcept {
            if (!callback || !*callback)
                return;
            try {
                (*callback)();
            } catch (...) {
                // Deferred owner cleanup must never change an already-settled
                // receipt or escape the sender's detached coroutine.
            }
        }
    } notify{&impl_->config.on_r2_background_quiescent};
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
            if (impl_->r2_physical_link_generation != physical_link_generation ||
                impl_->r2_reader_generation != physical_link_generation)
                co_return;
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
        ClientRunResult read_result;
        std::exception_ptr read_failure;
        try {
            if (!impl_->r2_socket ||
                impl_->r2_physical_link_generation != physical_link_generation)
                throw std::logic_error("R2 receipt reader lost its live socket");
            read_result = co_await impl_->endpoint->read_r2_receipt(
                *impl_->r2_socket, pending->sent, pending->deadline);
            {
                std::lock_guard lock(impl_->r2_transfer_mutex);
                if (impl_->r2_physical_link_generation != physical_link_generation ||
                    impl_->r2_reader_generation != physical_link_generation ||
                    impl_->r2_receipt_queue.empty() ||
                    impl_->r2_receipt_queue.front() != pending)
                    co_return;
            }
            if (read_result.status != ClientRunStatus::Committed ||
                !read_result.committed_commit)
                throw std::invalid_argument("R2 receipt was not an exact commit");
            pending->client_result = std::move(read_result);
            if (pending->sent.binding.profile == ProfileId::P29V1)
                pending->system_source_reuse =
                    impl_->authority->p29v1_system_source_reuse(
                        pending->sent.prepared);
            if (impl_->authority->contains(pending->sent.prepared))
                (void)impl_->authority->release(pending->sent.prepared);
            if (pending->replayed_after_reset &&
                pending->client_result.committed_commit &&
                pending->client_result.committed_input) {
                ZstdSourceTransferResult completed;
                completed.status = ZstdSourceTransferStatus::Committed;
                completed.profile = pending->sent.binding.profile;
                completed.committed_input =
                    pending->client_result.committed_input;
                completed.raw_bytes = pending->sent.binding.raw_bytes;
                completed.raw_digest = pending->sent.binding.raw_digest;
                completed.attempts = 1;
                completed.system_source_reuse = pending->system_source_reuse;
                impl_->remember_completed_witness(
                    pending->request, completed.raw_bytes,
                    completed.raw_digest, completed);
            }
            receipt_validated = true;
        } catch (...) {
            read_failure = std::current_exception();
        }

        std::vector<std::shared_ptr<boost::asio::steady_timer>> wake_window;
        bool start_ack_pump = false;
        const bool is_current_generation =
            impl_->r2_physical_link_generation == physical_link_generation;
        if (receipt_validated) {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (!is_current_generation ||
                impl_->r2_reader_generation != physical_link_generation ||
                impl_->r2_receipt_queue.empty() ||
                impl_->r2_receipt_queue.front() != pending)
                co_return;
            impl_->r2_retained_jobs.erase(
                pending->sent.binding.relationship_ordinal);
            impl_->r2_pending_ack_ordinal = std::max(
                impl_->r2_pending_ack_ordinal,
                pending->sent.binding.relationship_ordinal);
            if (!impl_->r2_ack_pump_running) {
                impl_->r2_ack_pump_running = true;
                impl_->r2_ack_pump_generation = physical_link_generation;
                start_ack_pump = true;
            }
            wake_window.swap(impl_->r2_window_waiters);
        } else {
            const uint64_t confirmed_floor =
                impl_->endpoint->r2_confirmed_prefix();
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (!is_current_generation ||
                impl_->r2_physical_link_generation != physical_link_generation ||
                impl_->r2_reader_generation != physical_link_generation ||
                impl_->r2_receipt_queue.empty() ||
                impl_->r2_receipt_queue.front() != pending)
                co_return;
            pending->failure = std::move(read_failure);
            pending->failure_physical_link_generation = physical_link_generation;
            if (impl_->r2_physical_link_generation == physical_link_generation) {
                impl_->r2_recovery_required = true;
                impl_->r2_failed_physical_generation =
                    physical_link_generation;
                impl_->r2_recovery_floor = confirmed_floor;
                impl_->route_transport_quarantined = true;
                if (impl_->r2_socket) {
                    boost::system::error_code ignored;
                    impl_->r2_socket->close(ignored);
                }
            }
            wake_window.swap(impl_->r2_window_waiters);
        }

        std::vector<std::shared_ptr<Impl::PendingReceipt>> failed_rows;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (impl_->r2_reader_generation != physical_link_generation ||
                impl_->r2_physical_link_generation != physical_link_generation)
                co_return;
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
                    row->failure_physical_link_generation =
                        physical_link_generation;
                    row->done = true;
                }
            }
        }
        if (receipt_validated &&
            impl_->config.after_r2_receipt_validated_for_test) {
            try {
                impl_->config.after_r2_receipt_validated_for_test(
                    pending->sent.binding.relationship_ordinal);
            } catch (...) {
                // A test observation must not rewrite an already validated
                // positive receipt into a transport failure.
            }
        }
        pending->notification.expires_at(Clock::now());
        for (const auto& timer : wake_window) {
            boost::system::error_code ignored;
            timer->expires_at(Clock::now(), ignored);
        }
        if (!receipt_validated)
            impl_->wake_r2_completed_capacity_waiters();
        if (start_ack_pump) {
            auto keepalive = shared_from_this();
            boost::asio::co_spawn(
                executor, run_r2_ack_pump(physical_link_generation),
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
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
#pragma GCC diagnostic pop
#endif

boost::asio::awaitable<void> P50ZstdSourceSender::run_r2_ack_pump(
    uint64_t physical_link_generation) {
    struct QuiescentNotify {
        const std::function<void()>* callback;
        ~QuiescentNotify() noexcept {
            if (!callback || !*callback)
                return;
            try {
                (*callback)();
            } catch (...) {
                // See the receipt-reader counterpart: reaping is advisory.
            }
        }
    } notify{&impl_->config.on_r2_background_quiescent};
    const auto executor = co_await boost::asio::this_coro::executor;
    for (;;) {
        uint64_t target = 0;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            target = impl_->r2_pending_ack_ordinal;
        if (target == 0) {
                if (impl_->r2_ack_pump_generation == physical_link_generation)
                    impl_->r2_ack_pump_running = false;
                co_return;
            }
        }
        if (impl_->r2_physical_link_generation != physical_link_generation ||
            impl_->r2_ack_pump_generation != physical_link_generation) {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (impl_->r2_ack_pump_generation == physical_link_generation)
                impl_->r2_ack_pump_running = false;
            co_return;
        }
        while (impl_->config.hold_r2_ack_pump_for_test &&
               impl_->config.hold_r2_ack_pump_for_test()) {
            if (impl_->route_replacement_required ||
                impl_->r2_physical_link_generation != physical_link_generation)
                break;
            boost::asio::steady_timer hold(executor);
            hold.expires_after(std::chrono::milliseconds(1));
            co_await hold.async_wait(boost::asio::use_awaitable);
        }
        if (impl_->route_replacement_required ||
            impl_->r2_physical_link_generation != physical_link_generation) {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (impl_->r2_ack_pump_generation == physical_link_generation)
                impl_->r2_ack_pump_running = false;
            co_return;
        }
        const auto deadline = Clock::now() + impl_->config.maximum_duration;
        try {
            if (!co_await acquire_r2_writer(deadline))
                throw boost::system::system_error(boost::asio::error::timed_out);
            Impl::R2TransferGuard writer_guard(impl_.get());
            {
                std::lock_guard lock(impl_->r2_transfer_mutex);
                if (impl_->r2_physical_link_generation != physical_link_generation ||
                    impl_->r2_ack_pump_generation != physical_link_generation)
                    co_return;
            }
            if (!impl_->r2_socket || !impl_->r2_socket->is_open())
                throw std::logic_error("R2 ACK pump lost its physical link");
            co_await impl_->endpoint->flush_r2_ack(
                *impl_->r2_socket, deadline);
            {
                std::lock_guard lock(impl_->r2_transfer_mutex);
                if (impl_->r2_physical_link_generation != physical_link_generation ||
                    impl_->r2_ack_pump_generation != physical_link_generation)
                    co_return;
            }
        } catch (...) {
            const uint64_t confirmed_floor =
                impl_->endpoint->r2_confirmed_prefix();
            {
                std::lock_guard lock(impl_->r2_transfer_mutex);
                if (!impl_->route_replacement_required &&
                    impl_->r2_physical_link_generation == physical_link_generation &&
                    impl_->r2_ack_pump_generation == physical_link_generation) {
                    impl_->r2_recovery_required = true;
                    impl_->r2_failed_physical_generation =
                        physical_link_generation;
                    impl_->r2_recovery_floor = confirmed_floor;
                    impl_->route_transport_quarantined = true;
                    if (impl_->r2_socket) {
                        boost::system::error_code ignored;
                        impl_->r2_socket->close(ignored);
                    }
                }
                if (impl_->r2_ack_pump_generation == physical_link_generation)
                    impl_->r2_ack_pump_running = false;
            }
            impl_->wake_r2_completed_capacity_waiters();
            co_return;
        }
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            if (impl_->r2_physical_link_generation != physical_link_generation ||
                impl_->r2_ack_pump_generation != physical_link_generation) {
                co_return;
            }
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

#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
boost::asio::awaitable<void> P50ZstdSourceSender::recover_r2_link(
    AsyncConnectedFdFactory connection, uint64_t requested_generation,
    Clock::time_point deadline) {
    if (impl_->route_replacement_required ||
        !impl_->r2_recovery_required || !impl_->r2_hello || !connection ||
        deadline <= Clock::now())
        throw std::logic_error("R2 recovery has no retained link context");
    if (impl_->r2_reader_running || impl_->r2_ack_pump_running)
        throw std::logic_error("R2 recovery raced a live reader or ACK pump");

    const uint64_t verified_floor_a = impl_->r2_recovery_floor;
    std::vector<R2SentBundle> witnesses =
        impl_->endpoint->r2_pending_witnesses(verified_floor_a);
    if (witnesses.size() > impl_->r2_hello->window)
        throw std::length_error("retained R2 recovery suffix exceeds link window");
    for (const auto& [ordinal, pending] : impl_->r2_retained_jobs) {
        (void)ordinal;
        deadline = std::min(deadline, pending->deadline);
    }
    if (deadline <= Clock::now())
        throw boost::system::system_error(boost::asio::error::timed_out);
    for (const R2SentBundle& witness : witnesses) {
        auto position = impl_->r2_retained_jobs.find(
            witness.binding.relationship_ordinal);
        if (position == impl_->r2_retained_jobs.end())
            throw std::logic_error("sender lost an R2 retained source reservation");
        position->second->sent = witness;
    }

    uint64_t physical_generation = requested_generation;
    const uint64_t generation_floor = std::max(
        impl_->r2_physical_link_generation,
        impl_->r2_attempted_physical_generation);
    if (physical_generation <= generation_floor) {
        if (generation_floor ==
            std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("R2 physical generation exhausted");
        physical_generation = generation_floor + 1;
    }
    impl_->r2_attempted_physical_generation = physical_generation;
    // Reserve/fence this incarnation before any HELLO bytes can escape. A
    // lost STATE/RECOVER/RESET reply must cause the next connection to use a
    // strictly newer generation, not replay this attempt's physical identity.
    impl_->r2_physical_link_generation = physical_generation;
    LinkHello hello = *impl_->r2_hello;
    hello.start_mode = LinkStartMode::Reconnect;
    hello.physical_link_generation = physical_generation;
    hello.verified_receipt_floor = verified_floor_a;

    if (impl_->r2_recovery_operation == Id128{}) {
        if (impl_->next_recovery_operation == 0 ||
            impl_->next_recovery_operation ==
                std::numeric_limits<uint64_t>::max() ||
            hello.relationship_epoch == std::numeric_limits<uint64_t>::max() ||
            hello.history_nonce.value == std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("R2 recovery identity space exhausted");
        impl_->r2_recovery_operation =
            Id128::from_u64(impl_->next_recovery_operation++);
        impl_->r2_recovery_new_epoch = hello.relationship_epoch + 1;
        impl_->r2_recovery_new_nonce = HistoryNonce{
            hello.history_nonce.value + 1};
    }

    if (impl_->r2_socket) {
        boost::system::error_code ignored;
        impl_->r2_socket->close(ignored);
        impl_->r2_socket.reset();
    }
    const int fd = co_await await_connected_fd(connection, deadline);
    if (fd < 0)
        throw std::runtime_error("R2 recovery connector failed");
    if (impl_->route_replacement_required) {
        (void)::close(fd);
        throw boost::system::system_error(boost::asio::error::operation_aborted);
    }
    const auto executor = co_await boost::asio::this_coro::executor;
    boost::system::error_code socket_error;
    auto socket = P50ClientEndpoint::adopt_connected_fd(
        executor, fd, socket_error);
    if (!socket)
        throw boost::system::system_error(socket_error);
    impl_->r2_socket = std::move(*socket);
    impl_->r2_failed_physical_generation = 0;

    impl_->r2_attempted_offer = hello;
    R2RecoveryResult recovered = co_await impl_->endpoint->recover_r2_link(
        *impl_->r2_socket, hello, witnesses, verified_floor_a,
        impl_->r2_recovery_operation, impl_->r2_recovery_new_epoch,
        impl_->r2_recovery_new_nonce, deadline);

    for (const R2TxCommit& receipt : recovered.committed_receipts) {
        auto position = impl_->r2_retained_jobs.find(
            receipt.relationship_ordinal);
        if (position == impl_->r2_retained_jobs.end())
            continue;  // The receipt was already accepted before transport loss.
        const std::shared_ptr<Impl::PendingReceipt> pending = position->second;
        if (!pending->sent.prepared ||
            pending->sent.binding.relationship_ordinal !=
                receipt.relationship_ordinal ||
            !exact_commit_matches(receipt.inner, pending->sent.begin.inner))
            throw std::logic_error("recovery receipt lost its source witness");
        ZstdSourceTransferResult result;
        result.status = ZstdSourceTransferStatus::Committed;
        result.profile = pending->sent.binding.profile;
        result.raw_bytes = pending->sent.binding.raw_bytes;
        result.raw_digest = pending->sent.binding.raw_digest;
        result.committed_input = InputRecordKey{
            impl_->c_guid, receipt.inner.tu_seq};
        result.attempts = 1;
        if (result.profile == ProfileId::P29V1)
            result.system_source_reuse =
                impl_->authority->p29v1_system_source_reuse(
                    pending->sent.prepared);
        impl_->authority->release(pending->sent.prepared);
        impl_->remember_completed_witness(pending->request,
            result.raw_bytes, result.raw_digest, result);
        pending->client_result.status = ClientRunStatus::Committed;
        pending->client_result.observation =
            ClientRunObservation::ExactCommitObserved;
        pending->client_result.committed_commit = receipt.inner;
        pending->client_result.committed_input = result.committed_input;
        pending->failure = nullptr;
        pending->replayed_after_reset = true;
        pending->done = true;
        pending->notification.expires_at(Clock::now());
        impl_->r2_retained_jobs.erase(position);
    }

    const uint64_t settled_prefix_k =
        recovered.reset_request.settled_prefix_k;
    const uint64_t prepared_prefix_p =
        recovered.reset_ack.recovery_prepared_prefix_p;
    if (recovered.reset_ack.recovery_verified_floor_a != verified_floor_a ||
        prepared_prefix_p < settled_prefix_k ||
        prepared_prefix_p - settled_prefix_k > witnesses.size())
        throw std::logic_error(
            "F reset disposition differs from the retained C witness interval");
    std::vector<PreparedTuHandle> unavailable_handles;
    std::vector<std::shared_ptr<Impl::PendingReceipt>> unavailable_pending;
    const uint64_t unavailable_count = prepared_prefix_p - settled_prefix_k;
    for (uint64_t offset = 0; offset < unavailable_count; ++offset) {
        const uint64_t ordinal = settled_prefix_k + 1 + offset;
        const uint32_t bit = uint32_t{1} << static_cast<uint32_t>(offset);
        if ((recovered.reset_ack.unavailable_suffix_mask & bit) == 0)
            continue;
        const size_t witness_index = static_cast<size_t>(
            ordinal - verified_floor_a - 1);
        if (witness_index >= witnesses.size() ||
            witnesses[witness_index].binding.relationship_ordinal != ordinal)
            throw std::logic_error(
                "F unavailable disposition has no exact retained witness");
        auto position = impl_->r2_retained_jobs.find(ordinal);
        if (position == impl_->r2_retained_jobs.end() ||
            position->second->sent.binding != witnesses[witness_index].binding ||
            position->second->sent.prepared != witnesses[witness_index].prepared)
            throw std::logic_error(
                "F unavailable disposition differs from the retained caller");
        unavailable_handles.push_back(position->second->sent.prepared);
        unavailable_pending.push_back(position->second);
    }
    std::vector<std::shared_ptr<Impl::PendingReceipt>> replay_rows;
    replay_rows.reserve(impl_->r2_retained_jobs.size());
    for (const auto& [ordinal, pending] : impl_->r2_retained_jobs) {
        if (ordinal <= settled_prefix_k)
            throw std::logic_error(
                "positive recovery prefix retained an unsettled sender row");
        if (std::find(unavailable_pending.begin(), unavailable_pending.end(),
                      pending) != unavailable_pending.end())
            continue;
        replay_rows.push_back(pending);
    }
    std::map<uint64_t, std::shared_ptr<Impl::PendingReceipt>> reindexed_jobs;
    uint64_t replay_ordinal = settled_prefix_k;
    for (const auto& pending : replay_rows) {
        // The F endpoint reserves UINT64_MAX as its exhausted next-ordinal
        // sentinel; do not rebuild a suffix row at that unadmittable ordinal.
        if (replay_ordinal >= std::numeric_limits<uint64_t>::max() - 1)
            throw std::overflow_error("R2 recovery ordinal space exhausted");
        ++replay_ordinal;
        if (!reindexed_jobs.emplace(replay_ordinal, pending).second)
            throw std::logic_error("R2 recovery suffix reindex collided");
    }

    // All allocating sender-side reconstruction work is complete before the
    // authority changes its retained route ledger.
    impl_->authority->reset_r2_route_for_recovery(
        impl_->route, recovered.link_state.f_store_guid,
        recovered.link_state.history_nonce, unavailable_handles);
    for (const auto& pending : unavailable_pending) {
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            pending->unavailable_by_reset = true;
            pending->failure = nullptr;
            pending->done = true;
            pending->client_result.status = ClientRunStatus::TerminalError;
            pending->client_result.terminal_error = ErrorMessage{
                0, "F reset reports this exact R2 reservation unavailable"};
        }
        pending->notification.expires_at(Clock::now());
    }
    for (const auto& [ordinal, pending] : reindexed_jobs) {
        pending->sent.binding.relationship_ordinal = ordinal;
        pending->sent.binding.physical_link_generation = physical_generation;
        pending->armed.relationship_epoch =
            recovered.reset_request.new_relationship_epoch;
    }
    impl_->r2_retained_jobs.swap(reindexed_jobs);
    {
        std::lock_guard lock(impl_->r2_transfer_mutex);
        impl_->r2_receipt_queue.clear();
    }
    impl_->r2_pending_ack_ordinal = 0;
    impl_->r2_relationship_ordinal =
        settled_prefix_k == std::numeric_limits<uint64_t>::max()
            ? std::numeric_limits<uint64_t>::max()
            : settled_prefix_k + 1;
    impl_->r2_hello = hello;
    impl_->r2_hello->relationship_epoch =
        recovered.reset_request.new_relationship_epoch;
    impl_->r2_hello->history_nonce = recovered.reset_request.new_history_nonce;
    impl_->r2_hello->verified_receipt_floor =
        recovered.reset_request.settled_prefix_k;
    impl_->r2_relationship_epoch =
        recovered.reset_request.new_relationship_epoch;
    impl_->r2_physical_link_generation = physical_generation;
    // The reset is now confirmed. A later transport loss starts a distinct
    // recovery operation from this new epoch/floor; only an uncertain reset
    // reply reuses the previous operation id.
    impl_->r2_recovery_operation = Id128{};
    impl_->r2_recovery_new_epoch = 0;
    impl_->r2_recovery_new_nonce = HistoryNonce{};
    impl_->r2_recovery_floor = settled_prefix_k;
    // Recovery and RESET are complete for this physical generation. Publish
    // that state before starting the receipt reader; a fast reader failure
    // after replay must not be overwritten by a late success assignment.
    impl_->r2_recovery_required = false;
    impl_->route_transport_quarantined = false;
    impl_->r2_failed_physical_generation = 0;
    impl_->reset_r2_recovery_backoff();

    bool reader_started = false;
    for (const auto& pending : replay_rows) {
        const uint64_t ordinal = pending->sent.binding.relationship_ordinal;
        if (pending->deadline <= Clock::now())
            throw boost::system::system_error(boost::asio::error::timed_out);
        impl_->authority->rebuild_r2_entry_for_recovery(
            pending->sent.prepared,
            recovered.link_state.f_system_source_fingerprint);
        JobBind binding = pending->sent.binding;
        pending->sent = co_await impl_->endpoint->write_r2_bundle(
            *impl_->r2_socket, binding, pending->sent.prepared,
            pending->deadline);
        pending->failure = nullptr;
        pending->failure_physical_link_generation = 0;
        pending->done = false;
        pending->ready = true;
        pending->replayed_after_reset = true;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            pending->notification.expires_at(Clock::time_point::max());
            impl_->r2_receipt_queue.push_back(pending);
            if (!impl_->r2_reader_running) {
                impl_->r2_reader_running = true;
                impl_->r2_reader_generation = physical_generation;
                reader_started = true;
            }
        }
        const uint64_t next_ordinal =
            ordinal == std::numeric_limits<uint64_t>::max()
                ? std::numeric_limits<uint64_t>::max()
                : ordinal + 1;
        impl_->r2_relationship_ordinal = std::max(
            impl_->r2_relationship_ordinal, next_ordinal);
        if (reader_started) {
            // The reader drains each F receipt while the sole writer continues
            // rebuilding the bounded suffix. ACK output remains queued behind
            // this transfer's writer permit, so frames cannot interleave.
            auto keepalive = shared_from_this();
            boost::asio::co_spawn(
                executor, run_r2_receipt_reader(physical_generation),
                [keepalive = std::move(keepalive)](std::exception_ptr) {});
            reader_started = false;
        }
        if (impl_->config.disconnect_r2_after_bundle_for_test &&
            impl_->config.disconnect_r2_after_bundle_for_test(ordinal)) {
            // This seam exercises an interruption after the current replay
            // row is queued for receipt but before the next retained row can
            // be staged on the endpoint. Keep the full sender backlog so the
            // next reset can prove that the omitted suffix row is not lost.
            if (impl_->r2_socket) {
                boost::system::error_code ignored;
                impl_->r2_socket->close(ignored);
            }
            throw std::runtime_error(
                "test requested R2 disconnect during replay suffix");
        }
    }
    if (!impl_->r2_socket || !impl_->r2_socket->is_open() ||
        impl_->r2_failed_physical_generation == physical_generation) {
        impl_->r2_recovery_required = true;
        throw std::runtime_error(
            "R2 replay reader failed on the newly recovered physical link");
    }
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13
#pragma GCC diagnostic pop
#endif

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
    const Digest128 raw_digest = digest128(source);
    try {
        if (const auto completed = impl_->completed_for(request, source, raw_digest))
            co_return *completed;
    } catch (...) {
        co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    }
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
    bool completed_slot_reserved = false;
    std::unique_ptr<Impl::R2TransferGuard> writer_guard;
    for (;;) {
        if (!co_await acquire_r2_writer(deadline))
            co_return impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
        writer_guard = std::make_unique<Impl::R2TransferGuard>(impl_.get());
        (void)impl_->retire_expired_r2_witnesses();
        if (impl_->route_replacement_required)
            co_return impl_->r2_route_replacement_result(
                ZstdSourceTransferStatus::Unavailable);
        const Id128 armed_relationship_id{armed.logical_relationship_id};
        if (impl_->r2_hello &&
            impl_->r2_hello->relationship_id == armed_relationship_id &&
            impl_->r2_relationship_id == armed_relationship_id &&
            !impl_->r2_recovery_required &&
            impl_->r2_hello->relationship_epoch ==
                impl_->r2_relationship_epoch) {
            if (armed.relationship_epoch > impl_->r2_relationship_epoch)
                co_return impl_->invalid(
                    ZstdSourceTransferStatus::InvalidRequest);
            // A prior caller may have completed a validated RESET while this
            // request's original ARM was queued locally. Preserve its
            // reservation/job identity but bind it to the sender's verified
            // current relationship epoch and physical generation.
            if (armed.relationship_epoch < impl_->r2_relationship_epoch)
                armed.relationship_epoch = impl_->r2_relationship_epoch;
            physical_link_generation = std::max(
                physical_link_generation,
                current_r2_physical_generation());
        }
        if (impl_->r2_recovery_required) {
            if (impl_->config.before_r2_recovery_for_test)
                impl_->config.before_r2_recovery_for_test(request);
            writer_guard.reset();
            if (!co_await wait_for_r2_recovery_retry(deadline)) {
                ZstdSourceTransferResult failed =
                    impl_->invalid(Clock::now() >= deadline
                        ? ZstdSourceTransferStatus::DeadlineExceeded
                        : ZstdSourceTransferStatus::Unavailable);
                failed.route_local_failure = true;
                co_return failed;
            }
            if (!co_await acquire_r2_writer(deadline)) {
                ZstdSourceTransferResult failed =
                    impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
                failed.route_local_failure = true;
                co_return failed;
            }
            writer_guard = std::make_unique<Impl::R2TransferGuard>(impl_.get());
            bool retry_not_before = false;
            {
                std::lock_guard lock(impl_->r2_transfer_mutex);
                retry_not_before = impl_->r2_recovery_required &&
                    impl_->r2_recovery_retry_not_before > Clock::now();
            }
            if (retry_not_before) {
                writer_guard.reset();
                continue;
            }
            if (!impl_->r2_recovery_required) {
                writer_guard.reset();
                continue;
            }
            try {
                co_await recover_r2_link(connection,
                                         physical_link_generation, deadline);
                if (armed.relationship_epoch >
                        impl_->r2_relationship_epoch ||
                    physical_link_generation >
                        impl_->r2_physical_link_generation)
                    // Recovery succeeded for the shared link. Reject only
                    // this caller's inconsistent offer; do not route it
                    // through the transport-recovery failure handler.
                    co_return impl_->invalid(
                        ZstdSourceTransferStatus::InvalidRequest);
                physical_link_generation =
                    impl_->r2_physical_link_generation;
                armed.relationship_epoch = impl_->r2_relationship_epoch;
            } catch (const R2LinkRejected& rejected) {
                if (auto route_rejection =
                        impl_->accept_r2_link_rejection(rejected)) {
                    ZstdSourceTransferResult failed =
                        impl_->invalid(ZstdSourceTransferStatus::TerminalError);
                    failed.profile = impl_->config.endpoint_caps.profile;
                    failed.route_local_failure = true;
                    failed.r2_link_rejection = std::move(route_rejection);
                    co_return failed;
                }
                if (impl_->retire_expired_r2_witnesses() ||
                    impl_->route_replacement_required)
                    co_return impl_->r2_route_replacement_result(
                        ZstdSourceTransferStatus::Unavailable);
                impl_->note_r2_recovery_failure();
                ZstdSourceTransferResult failed =
                    impl_->invalid(ZstdSourceTransferStatus::Unavailable);
                failed.route_local_failure = true;
                co_return failed;
            } catch (...) {
                if (impl_->retire_expired_r2_witnesses() ||
                    impl_->route_replacement_required)
                    co_return impl_->r2_route_replacement_result(
                        ZstdSourceTransferStatus::Unavailable);
                impl_->note_r2_recovery_failure();
                ZstdSourceTransferResult failed =
                    impl_->invalid(ZstdSourceTransferStatus::Unavailable);
                failed.route_local_failure = true;
                co_return failed;
            }
        }
        // Recover existing retained witnesses before waiting for capacity.
        // Those witnesses may own every ledger slot; a fresh caller must be
        // able to drive their receipt reconciliation, without admitting its
        // own bundle, rather than waiting forever for a slot it cannot free.
        if (!completed_slot_reserved) {
            writer_guard.reset();
            bool permanently_full = false;
            bool recovery_required = false;
            if (!co_await impl_->reserve_r2_completed_slot(
                    request, deadline, permanently_full, recovery_required)) {
                if (recovery_required)
                    continue;
                if (impl_->route_replacement_required)
                    co_return impl_->r2_route_replacement_result(
                        ZstdSourceTransferStatus::Unavailable);
                if (permanently_full)
                    co_return impl_->replacement(
                        ZstdSourceTransferStatus::Unavailable, true,
                        ReplacementTrigger::CompletedRequestCapacity);
                ZstdSourceTransferResult unavailable = impl_->invalid(
                    Clock::now() >= deadline
                        ? ZstdSourceTransferStatus::DeadlineExceeded
                        : ZstdSourceTransferStatus::Unavailable);
                unavailable.route_local_failure = true;
                co_return unavailable;
            }
            completed_slot_reserved = true;
            continue;
        }
        if (!impl_->r2_socket)
            break;
        try {
            co_await impl_->endpoint->flush_r2_ack(*impl_->r2_socket, deadline);
        } catch (...) {
            impl_->r2_recovery_required = true;
            impl_->r2_recovery_floor =
                impl_->endpoint->r2_confirmed_prefix();
            impl_->route_transport_quarantined = true;
            boost::system::error_code ignored;
            impl_->r2_socket->close(ignored);
            ZstdSourceTransferResult failed =
                impl_->invalid(ZstdSourceTransferStatus::Unavailable);
            failed.route_local_failure = true;
            impl_->wake_r2_completed_capacity_waiters();
            co_return failed;
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
    if (impl_->route_replacement_required)
        co_return impl_->r2_route_replacement_result(
            ZstdSourceTransferStatus::Unavailable);
    PreparedTuHandle prepared;
    try {
        if (impl_->config.before_prepare_for_route_for_test)
            impl_->config.before_prepare_for_route_for_test();
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
    bool recovery_needed = false;
    bool connector_succeeded = impl_->r2_socket.has_value();
    bool exact_commit_preserved = false;
    std::optional<ZstdSourceRouteRejection> terminal_rejection;
    uint64_t observed_failure_generation = 0;
    try {
        if (impl_->route_replacement_required)
            co_return impl_->r2_route_replacement_result(
                ZstdSourceTransferStatus::Unavailable);
        if (impl_->r2_socket &&
            (impl_->r2_physical_link_generation != physical_link_generation ||
             impl_->r2_relationship_id != hello.relationship_id ||
             impl_->r2_relationship_epoch != hello.relationship_epoch))
            throw std::invalid_argument("R2 relationship changed on retained socket");
        if (!impl_->r2_socket) {
            int fd = -1;
            for (;;) {
                if (!co_await wait_for_r2_connect_retry(deadline))
                    throw boost::system::system_error(
                        Clock::now() >= deadline
                            ? boost::asio::error::timed_out
                            : boost::asio::error::operation_aborted);
                if (impl_->route_replacement_required)
                    throw boost::system::system_error(
                        boost::asio::error::operation_aborted);
                fd = co_await await_connected_fd(connection, deadline);
                if (fd >= 0) {
                    if (impl_->route_replacement_required ||
                        Clock::now() >= deadline) {
                        (void)::close(fd);
                        throw boost::system::system_error(
                            impl_->route_replacement_required
                                ? boost::asio::error::operation_aborted
                                : boost::asio::error::timed_out);
                    }
                    connector_succeeded = true;
                    break;
                }
                if (Clock::now() >= deadline)
                    throw boost::system::system_error(
                        boost::asio::error::timed_out);
                // No LINK_HELLO escaped and no F state exists yet. Retry the
                // connector under this same writer lease and absolute job
                // deadline; do not invoke RECOVER or ask the authority to
                // prepare another TU. The shared timer/backoff is also woken
                // by retire_for_replacement().
                impl_->note_r2_recovery_failure();
            }
            boost::system::error_code error;
            auto socket = P50ClientEndpoint::adopt_connected_fd(executor, fd, error);
            if (!socket) {
                if (!impl_->route_replacement_required)
                    impl_->route_replacement_trigger =
                        ReplacementTrigger::UnexpectedTransferException;
                impl_->route_replacement_required = true;
                impl_->wake_r2_completed_capacity_waiters();
                throw std::runtime_error("R2 link socket adoption failed");
            }
            impl_->r2_socket = std::move(*socket);
            impl_->r2_physical_link_generation = physical_link_generation;
            impl_->r2_relationship_id = hello.relationship_id;
            impl_->r2_relationship_epoch = hello.relationship_epoch;
            impl_->r2_attempted_offer = hello;
            impl_->reset_r2_recovery_backoff();
            const LinkState state = co_await impl_->endpoint->open_r2_link(
                *impl_->r2_socket, hello, deadline);
            impl_->r2_hello = hello;
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

        if (impl_->r2_relationship_ordinal ==
            std::numeric_limits<uint64_t>::max())
            throw std::overflow_error("R2 relationship ordinal space exhausted");
        binding.relationship_ordinal = impl_->r2_relationship_ordinal;
        pending = std::make_shared<Impl::PendingReceipt>(executor);
        pending->armed = armed;
        pending->deadline = deadline;
        pending->request = request;
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
        EndpointIoControl bundle_control = std::exchange(
            impl_->config.r2_bundle_io_control_for_test, EndpointIoControl{});
        pending->sent = co_await impl_->endpoint->write_r2_bundle(
            *impl_->r2_socket, binding, prepared, deadline,
            std::move(bundle_control));
        bundle_complete = true;
        if (impl_->r2_relationship_ordinal !=
            std::numeric_limits<uint64_t>::max())
            ++impl_->r2_relationship_ordinal;
        if (impl_->config.after_r2_bundle_sent_for_test) {
            try {
                impl_->config.after_r2_bundle_sent_for_test(
                    pending->sent.binding.relationship_ordinal);
            } catch (...) {
                // Diagnostic test observation cannot alter transfer state.
            }
        }
        if (impl_->config.disconnect_r2_after_bundle_for_test &&
            impl_->config.disconnect_r2_after_bundle_for_test(
                pending->sent.binding.relationship_ordinal)) {
            if (impl_->r2_socket) {
                boost::system::error_code ignored;
                impl_->r2_socket->close(ignored);
            }
            throw std::runtime_error(
                "test requested R2 disconnect after complete bundle");
        }
        bool start_reader = false;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            pending->ready = true;
            if (!impl_->r2_reader_running) {
                impl_->r2_reader_running = true;
                impl_->r2_reader_generation =
                    pending->sent.binding.physical_link_generation;
                start_reader = true;
            }
        }
        pending->notification.expires_at(Clock::now());
        writer_guard.reset();
        if (start_reader) {
            auto keepalive = shared_from_this();
            boost::asio::co_spawn(
                executor,
                run_r2_receipt_reader(
                    pending->sent.binding.physical_link_generation),
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
      std::exception_ptr observed_failure;
      bool unavailable_by_reset = false;
      {
          std::lock_guard lock(impl_->r2_transfer_mutex);
          observed_failure = pending->failure;
          unavailable_by_reset = pending->unavailable_by_reset;
          if (observed_failure)
              observed_failure_generation =
                  pending->failure_physical_link_generation;
      }
      if (unavailable_by_reset) {
          ZstdSourceTransferResult unavailable =
              impl_->invalid(ZstdSourceTransferStatus::Unavailable);
          unavailable.profile = pending->sent.binding.profile;
          unavailable.raw_bytes = pending->sent.binding.raw_bytes;
          unavailable.raw_digest = pending->sent.binding.raw_digest;
          co_return unavailable;
      }
      if (observed_failure)
          std::rethrow_exception(observed_failure);
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
              // to return flow-control credit quarantines this link until the
              // same retained relationship is reconciled by RESET.
              if (!impl_->route_replacement_required) {
                  impl_->r2_recovery_required = true;
                  impl_->r2_failed_physical_generation =
                      impl_->r2_physical_link_generation;
                  impl_->r2_recovery_floor =
                      impl_->endpoint->r2_confirmed_prefix();
                  impl_->route_transport_quarantined = true;
                  if (impl_->r2_socket) {
                      boost::system::error_code ignored;
                      impl_->r2_socket->close(ignored);
                  }
              }
          }
      }
      while (impl_->r2_recovery_required && Clock::now() < deadline &&
             !impl_->route_replacement_required) {
          if (!co_await wait_for_r2_recovery_retry(deadline) ||
              !co_await acquire_r2_writer(deadline))
              break;
          auto recovery_guard =
              std::make_unique<Impl::R2TransferGuard>(impl_.get());
          bool attempt = false;
          {
              std::lock_guard lock(impl_->r2_transfer_mutex);
              attempt = impl_->r2_recovery_required &&
                  impl_->r2_recovery_retry_not_before <= Clock::now() &&
                  !impl_->route_replacement_required;
          }
          if (!attempt)
              continue;
          bool recovered = false;
          try {
              co_await recover_r2_link(connection,
                  impl_->r2_physical_link_generation, deadline);
              recovered = true;
          } catch (const R2LinkRejected& rejected) {
              terminal_rejection =
                  impl_->accept_r2_link_rejection(rejected);
              if (!terminal_rejection)
                  impl_->note_r2_recovery_failure();
          } catch (...) {
              // Exact COMMIT remains the result even if credit recovery
              // exhausts this original request's deadline.
              impl_->note_r2_recovery_failure();
          }
          recovery_guard.reset();
          if (recovered)
              break;
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
        result.r2_link_rejection = terminal_rejection
            ? terminal_rejection : impl_->current_r2_route_rejection();
        if (!pending->replayed_after_reset)
            impl_->remember_completed(request, source, raw_digest, result);
        co_return result;
    } catch (...) {
        const std::exception_ptr transfer_failure = std::current_exception();
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            exact_commit_preserved = pending && pending->done &&
                !pending->failure &&
                pending->client_result.status == ClientRunStatus::Committed &&
                pending->client_result.committed_commit.has_value();
        }
        try {
            std::rethrow_exception(transfer_failure);
        } catch (const R2LinkRejected& rejected) {
            terminal_rejection =
                impl_->accept_r2_link_rejection(rejected);
        } catch (...) {
        }
        if (!exact_commit_preserved) {
          const uint64_t error_generation =
              observed_failure_generation != 0
                  ? observed_failure_generation
                  : (pending &&
                             pending->sent.binding.physical_link_generation != 0
                         ? pending->sent.binding.physical_link_generation
                         : physical_link_generation);
          bool stale_generation = false;
          {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            stale_generation =
                impl_->r2_physical_link_generation != error_generation;
          }
          if (stale_generation && pending && !terminal_rejection) {
            // Another caller already advanced this relationship to a newer
            // physical link. This waiter's old socket error must not remove,
            // reset, or release the shared pending row. Join it after leaving
            // this handler, since coroutine awaits are not legal in a catch.
            recovery_needed = true;
          } else {
            bool has_staged_witness =
                pending && bundle_complete && !terminal_rejection.has_value();
            if (pending && !bundle_complete && impl_->r2_socket &&
                !terminal_rejection.has_value()) {
              try {
                auto witnesses = impl_->endpoint->r2_pending_witnesses(
                    impl_->r2_relationship_ordinal - 1);
                if (witnesses.size() == 1) {
                  pending->sent = std::move(witnesses.front());
                  has_staged_witness = true;
                }
              } catch (...) {
              }
            }
            if (pending && !has_staged_witness) {
              std::lock_guard lock(impl_->r2_transfer_mutex);
              auto position = std::find(impl_->r2_receipt_queue.begin(),
                                        impl_->r2_receipt_queue.end(), pending);
              if (position != impl_->r2_receipt_queue.end())
                impl_->r2_receipt_queue.erase(position);
              if (pending->sent.binding.relationship_ordinal != 0)
                impl_->r2_retained_jobs.erase(
                    pending->sent.binding.relationship_ordinal);
              pending->failure = transfer_failure;
              pending->done = true;
              pending->notification.expires_at(Clock::now());
            }
            if (pending && has_staged_witness) {
              {
                std::lock_guard lock(impl_->r2_transfer_mutex);
                auto position =
                    std::find(impl_->r2_receipt_queue.begin(),
                              impl_->r2_receipt_queue.end(), pending);
                if (position != impl_->r2_receipt_queue.end())
                  impl_->r2_receipt_queue.erase(position);
              }
              pending->failure = nullptr;
              pending->done = false;
              pending->ready = false;
            }
            if (!has_staged_witness) {
              try {
                if (impl_->authority->contains(prepared)) {
                  const auto release_count =
                      impl_->authority->release(prepared);
                  (void)release_count;
                }
              } catch (...) {
              }
            }
            if (connector_succeeded) {
              const uint64_t confirmed_floor =
                  impl_->endpoint->r2_confirmed_prefix();
              {
                std::lock_guard lock(impl_->r2_transfer_mutex);
                if (!terminal_rejection.has_value() &&
                    impl_->r2_physical_link_generation == error_generation) {
                  impl_->r2_recovery_required = true;
                  impl_->r2_failed_physical_generation = error_generation;
                  impl_->r2_recovery_floor = confirmed_floor;
                  impl_->route_transport_quarantined = true;
                  if (impl_->r2_socket) {
                    boost::system::error_code ignored;
                    impl_->r2_socket->close(ignored);
                  }
                }
              }
              recovery_needed =
                  !terminal_rejection.has_value() && pending &&
                  has_staged_witness &&
                  impl_->r2_physical_link_generation == error_generation;
            }
          }
        }
    }

    if (exact_commit_preserved) {
        ZstdSourceTransferResult result;
        result.status = ZstdSourceTransferStatus::Committed;
        result.profile = pending->sent.binding.profile;
        result.committed_input = pending->client_result.committed_input;
        result.raw_bytes = pending->sent.binding.raw_bytes;
        result.raw_digest = pending->sent.binding.raw_digest;
        result.attempts = 1;
        result.system_source_reuse = pending->system_source_reuse;
        result.r2_link_rejection = terminal_rejection
            ? terminal_rejection : impl_->current_r2_route_rejection();
        if (!pending->replayed_after_reset)
            impl_->remember_completed(request, source, raw_digest, result);
        co_return result;
    }

    if (terminal_rejection) {
        ZstdSourceTransferResult rejected =
            impl_->invalid(ZstdSourceTransferStatus::TerminalError);
        rejected.profile = binding.profile;
        rejected.route_local_failure = true;
        rejected.r2_link_rejection = std::move(terminal_rejection);
        co_return rejected;
    }

    if (recovery_needed) {
        // Keep this original caller (and its ARMED lease/deadline) alive while
        // the sole link coordinator reconciles the ambiguous suffix. A future
        // compiler request is not required to trigger recovery.
        writer_guard.reset();
        for (;;) {
            {
                std::lock_guard lock(impl_->r2_transfer_mutex);
                if (pending->unavailable_by_reset) {
                    ZstdSourceTransferResult unavailable = impl_->invalid(
                        ZstdSourceTransferStatus::Unavailable);
                    unavailable.profile = pending->sent.binding.profile;
                    unavailable.raw_bytes = pending->sent.binding.raw_bytes;
                    unavailable.raw_digest = pending->sent.binding.raw_digest;
                    co_return unavailable;
                }
                if (pending->done && !pending->failure &&
                    pending->client_result.status == ClientRunStatus::Committed &&
                    pending->client_result.committed_commit) {
                    ZstdSourceTransferResult result;
                    result.status = ZstdSourceTransferStatus::Committed;
                    result.profile = pending->sent.binding.profile;
                    result.committed_input =
                        pending->client_result.committed_input;
                    result.raw_bytes = pending->sent.binding.raw_bytes;
                    result.raw_digest = pending->sent.binding.raw_digest;
                    result.attempts = 1;
                    result.system_source_reuse = pending->system_source_reuse;
                    result.r2_link_rejection = terminal_rejection
                        ? terminal_rejection : impl_->r2_terminal_rejection;
                    co_return result;
                }
                if (pending->done && pending->failure) {
                    // A receipt-reader failure means this replay attempt also
                    // lost its physical link. Keep the exact retained witness
                    // and original caller, clear only the per-attempt result,
                    // then let this same caller reacquire the sole recovery
                    // coordinator below.
                    pending->failure = nullptr;
                    pending->done = false;
                    pending->ready = false;
                }
            }
            if (impl_->route_replacement_required)
                break;
            if (Clock::now() >= deadline)
                break;

            bool recovery_required = false;
            {
                std::lock_guard lock(impl_->r2_transfer_mutex);
                recovery_required = impl_->r2_recovery_required;
            }
            if (recovery_required) {
                if (impl_->route_replacement_required)
                    break;
                writer_guard.reset();
                if (!co_await wait_for_r2_recovery_retry(deadline))
                    break;
                if (!co_await acquire_r2_writer(deadline))
                    break;
                writer_guard = std::make_unique<Impl::R2TransferGuard>(impl_.get());
                {
                    std::lock_guard lock(impl_->r2_transfer_mutex);
                    recovery_required = impl_->r2_recovery_required;
                    if (recovery_required &&
                        impl_->r2_recovery_retry_not_before > Clock::now())
                        recovery_required = false;
                }
                if (!recovery_required) {
                    writer_guard.reset();
                    continue;
                }
                if (impl_->route_replacement_required) {
                    writer_guard.reset();
                    break;
                }
                try {
                    co_await recover_r2_link(connection,
                        impl_->r2_physical_link_generation, deadline);
                } catch (const R2LinkRejected& rejected) {
                    if (auto route_rejection =
                            impl_->accept_r2_link_rejection(rejected)) {
                        terminal_rejection = std::move(route_rejection);
                        break;
                    }
                    impl_->note_r2_recovery_failure();
                } catch (...) {
                    impl_->note_r2_recovery_failure();
                    // Preserve the exact suffix and operation deadline. The
                    // next pass retries with the next physical generation.
                    bool positive_commit = false;
                    {
                        std::lock_guard lock(impl_->r2_transfer_mutex);
                        positive_commit = pending->done && !pending->failure &&
                            pending->client_result.status ==
                                ClientRunStatus::Committed &&
                            pending->client_result.committed_commit.has_value();
                        if (!positive_commit &&
                            !impl_->route_replacement_required) {
                            impl_->r2_recovery_required = true;
                            impl_->r2_failed_physical_generation =
                                impl_->r2_physical_link_generation;
                            impl_->route_transport_quarantined = true;
                        }
                    }
                    if (!positive_commit && !impl_->route_replacement_required &&
                        impl_->r2_socket) {
                        boost::system::error_code ignored;
                        impl_->r2_socket->close(ignored);
                    }
                }
                // Never hold writer ownership while the independent reader
                // settles the replayed receipt or while it reports another
                // physical failure.
                writer_guard.reset();
                continue;
            }

            bool changed = false;
            {
                std::lock_guard lock(impl_->r2_transfer_mutex);
                changed = pending->done || impl_->r2_recovery_required;
                if (!changed)
                    pending->notification.expires_at(deadline);
            }
            if (changed)
                continue;
            boost::system::error_code wait_error;
            co_await pending->notification.async_wait(
                boost::asio::redirect_error(boost::asio::use_awaitable,
                                            wait_error));
        }
        bool exact_commit_observed = false;
        {
            std::lock_guard lock(impl_->r2_transfer_mutex);
            exact_commit_observed = pending->done && !pending->failure &&
                pending->client_result.status == ClientRunStatus::Committed &&
                pending->client_result.committed_commit.has_value();
            if (!exact_commit_observed) {
                pending->failure = std::make_exception_ptr(
                    boost::system::system_error(boost::asio::error::timed_out));
                pending->done = true;
                pending->notification.expires_at(Clock::now());
            }
        }
        if (exact_commit_observed) {
            ZstdSourceTransferResult result;
            result.status = ZstdSourceTransferStatus::Committed;
            result.profile = pending->sent.binding.profile;
            result.committed_input = pending->client_result.committed_input;
            result.raw_bytes = pending->sent.binding.raw_bytes;
            result.raw_digest = pending->sent.binding.raw_digest;
            result.attempts = 1;
            result.system_source_reuse = pending->system_source_reuse;
            result.r2_link_rejection = terminal_rejection
                ? terminal_rejection : impl_->current_r2_route_rejection();
            co_return result;
        }
    }
    if (terminal_rejection) {
        ZstdSourceTransferResult rejected =
            impl_->invalid(ZstdSourceTransferStatus::TerminalError);
        rejected.profile = binding.profile;
        rejected.route_local_failure = true;
        rejected.r2_link_rejection = std::move(terminal_rejection);
        co_return rejected;
    }
    if (auto route_rejection = impl_->current_r2_route_rejection()) {
        ZstdSourceTransferResult rejected =
            impl_->invalid(ZstdSourceTransferStatus::TerminalError);
        rejected.profile = binding.profile;
        rejected.route_local_failure = true;
        rejected.r2_link_rejection = std::move(route_rejection);
        co_return rejected;
    }
    ZstdSourceTransferResult failed = impl_->invalid(
        Clock::now() >= deadline ? ZstdSourceTransferStatus::DeadlineExceeded
                                 : ZstdSourceTransferStatus::Unavailable);
    failed.route_local_failure = true;
    co_return failed;
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
    if (impl_->completed_ledger_full())
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
