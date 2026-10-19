#include "p50_zstd_sender.h"

#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <chrono>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace icecc::p50 {
namespace {

// Diagnostic only: name the exception a catch-all is about to turn into an
// untyped TerminalError, so a farm leg can tell which local invariant fired.
void report_swallowed_exception(const char* site, unsigned attempt) noexcept {
    const char* what = "non-standard exception";
    try {
        throw;
    } catch (const std::exception& error) {
        what = error.what();
    } catch (...) {
    }
    std::fprintf(stderr,
                 "P50_SENDER_EXCEPTION site=%s attempt=%u what=%s\n", site,
                 attempt, what);
    std::fflush(stderr);
}

using Clock = std::chrono::steady_clock;

bool nonzero(CStoreGuid guid) {
    return std::any_of(guid.bytes.begin(), guid.bytes.end(),
                       [](uint8_t byte) { return byte != 0; });
}

bool nonzero_request(PrepareRequestKey request) {
    return request.producer_session != 0 && request.request_token != 0;
}

bool valid_deadline(Clock::time_point deadline, Clock::duration maximum_duration,
                    Clock::time_point now) {
    return deadline > now && maximum_duration > Clock::duration::zero() &&
           deadline - now <= maximum_duration;
}

// Counts one up for the lifetime of the coroutine frame that constructs
// it, regardless of how that frame exits (normal co_return, exception, or
// cancellation-driven unwind): the local object is destroyed exactly once
// when the frame itself is destroyed. Used to give transfer_in_flight() an
// accurate view of whether any transfer_bytes() is currently running --
// ANY, not "this one": with route pipelining (K>1,
// P50_ROUTE_PIPELINING.md step 4) the per-route transfer lock releases at
// the head's write-complete (4c), so a successor transfer_bytes() overlaps
// the still-running head's on the SAME sender. A bool cleared by whichever
// frame exits first would hide the still-live other transfer and let
// P50CRouteOwner::reset_f_store_exact erase the sender out from under it.
struct InFlightGuard {
    explicit InFlightGuard(size_t* count) noexcept : count_(count) { ++*count_; }
    ~InFlightGuard() { --*count_; }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;

private:
    size_t* count_;
};

bool route_history_profile(ProfileId profile) noexcept {
    return profile == ProfileId::P29V1 || profile == ProfileId::ZSTD_ROUTE;
}

// Waits between reopens of an attempt that F refused for capacity.
constexpr Clock::duration kCapacityWaitMin = std::chrono::milliseconds(10);
constexpr Clock::duration kCapacityWaitMax = std::chrono::milliseconds(200);

// One factory call.  A reopen may block for a whole connect/arm/session round
// trip, so it runs on the reopen executor when one is configured.  The caller
// resumes by the deadline even if that open has not returned; the open keeps
// its thread and its credit until it does and closes a descriptor nobody is
// waiting for.
boost::asio::awaitable<int> open_connection(
    const ConnectedFdFactory& factory, Clock::time_point deadline,
    const std::optional<ReopenExecutor>& reopen) {
    if (!reopen)
        co_return factory(deadline);
    // Released when the posted open is destroyed: after it ran, or unrun.
    struct Credit {
        std::shared_ptr<std::atomic<size_t>> outstanding;
        ~Credit() {
            if (outstanding)
                outstanding->fetch_sub(1);
        }
    };
    auto credit = std::make_shared<Credit>();
    for (size_t held = reopen->outstanding->load();;) {
        if (held >= reopen->limit)
            co_return -1;
        if (reopen->outstanding->compare_exchange_weak(held, held + 1))
            break;
    }
    credit->outstanding = reopen->outstanding;
    struct Open {
        explicit Open(const boost::asio::any_io_executor& executor) : wake(executor) {}
        ~Open() {
            if (fd >= 0)
                (void)::close(fd);
        }
        std::mutex mutex;
        bool abandoned = false;
        int fd = -1;
        std::exception_ptr error;
        boost::asio::steady_timer wake;
    };
    const auto executor = co_await boost::asio::this_coro::executor;
    auto open = std::make_shared<Open>(executor);
    open->wake.expires_at(deadline);
    boost::asio::post(reopen->executor, [open, executor, factory, deadline,
                                         credit = std::move(credit)] {
        int fd = -1;
        std::exception_ptr error;
        try {
            if (Clock::now() < deadline)
                fd = factory(deadline);
        } catch (...) {
            error = std::current_exception();
        }
        std::lock_guard lock(open->mutex);
        if (open->abandoned) {
            if (fd >= 0)
                (void)::close(fd);
            return;
        }
        open->fd = fd;
        open->error = error;
        try {
            boost::asio::post(executor, [open] { open->wake.cancel(); });
        } catch (...) {
            // The deadline still wakes the caller.
        }
    });
    boost::system::error_code ignored;
    co_await open->wake.async_wait(
        boost::asio::redirect_error(boost::asio::use_awaitable, ignored));
    std::lock_guard lock(open->mutex);
    open->abandoned = true;
    if (open->error)
        std::rethrow_exception(open->error);
    int fd = std::exchange(open->fd, -1);
    if (fd >= 0 && Clock::now() >= deadline) {
        (void)::close(fd);
        fd = -1;
    }
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
        // route_value.c_store_guid carries a ROUTE_LANES lane GUID (or the
        // base GUID at lane 0, in which case this is identical to the
        // authority's own GUID). Member order below declares c_guid before
        // route, so reading route_value here -- before it is moved into
        // route -- is well-defined.
        : c_guid(route_value.c_store_guid != CStoreGuid{}
                     ? route_value.c_store_guid
                     : (authority_value ? authority_value->c_store_guid()
                                        : CStoreGuid{})),
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
        // A route-bound sender belongs to one C/F-incarnation relationship.
        endpoint->enable_pipelined_begin();
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
        uint8_t attempts, uint64_t c_prepare_route_ns = 0) const {
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
        // Diagnostic-only; see ZstdSourceTransferResult's field comments.
        result.c_prepare_route_ns = c_prepare_route_ns;
        result.c_fill_ns = run.c_fill_ns;
        result.c_write_ns = run.c_write_ns;
        result.c_state_wait_ns = run.c_state_wait_ns;
        result.c_state_to_commit_ns = run.c_state_to_commit_ns;
        result.c_post_commit_ns = run.c_post_commit_ns;
        result.c_begin_write_wall_ns = run.c_begin_write_wall_ns;
        result.c_commit_read_wall_ns = run.c_commit_read_wall_ns;
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

    // A request cannot be replayed once maximum_duration has passed since it
    // completed: its absolute deadline is already behind it.
    void expire_completed() {
        const auto horizon = std::chrono::steady_clock::now() - config.maximum_duration;
        while (!completed_order.empty() && completed_order.front().first < horizon) {
            completed.erase(completed_order.front().second);
            completed_order.pop_front();
        }
    }

    void remember_completed(PrepareRequestKey key, std::span<const uint8_t> source,
                            Digest128 raw_digest,
                            const ZstdSourceTransferResult& result) {
        expire_completed();
        if (completed.size() >= config.max_completed_requests)
            throw std::length_error("sender completed-request ledger is full");
        const auto [position, inserted] = completed.emplace(
            key, CompletedRequest{static_cast<uint64_t>(source.size()), raw_digest, result});
        if (!inserted)
            throw std::logic_error("sender completed request was admitted twice");
        (void)position;
        completed_order.emplace_back(std::chrono::steady_clock::now(), key);
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
    std::map<PrepareRequestKey, CompletedRequest> completed;
    std::deque<std::pair<std::chrono::steady_clock::time_point, PrepareRequestKey>>
        completed_order;
    bool used = false;
    bool route_replacement_required = false;
    bool route_transport_quarantined = false;
    // Backstop for the route-local retirement of witness-class terminals
    // (te=3: F refused this chain-abandoned encoding's STATE/TX_COMMIT
    // witness; a route reset re-encodes and cures it). The first few are
    // route-local by design, but a route that keeps producing them has a
    // C-side problem a reset cannot cure: after kWitnessRetireBackstop
    // consecutive witness-class retirements without an intervening commit,
    // escalate to the existing sticky whole-C replacement path.
    static constexpr size_t kWitnessRetireBackstop = 8;
    size_t witness_class_retirements = 0;
    // Set (nonzero) for the entire duration of each transfer_bytes()
    // coroutine call, across every co_await suspension; see InFlightGuard
    // above and P50ZstdSourceSender::transfer_in_flight(). A counter, not
    // a bool: overlapped pipelined transfers (K>1) each hold one count, so
    // the flag reads "any transfer in flight".
    size_t transfers_in_flight = 0;
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

bool P50ZstdSourceSender::transfer_in_flight() const noexcept {
    return impl_->transfers_in_flight != 0;
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
                          false, p50::P50SourceView::own(*bytes));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer(boost::asio::ip::tcp::endpoint remote,
                               std::span<const uint8_t> source) {
    const PrepareRequestKey request = impl_->begin_transfer();
    if (source.size() > impl_->config.endpoint_caps.zstd.max_raw_bytes)
        return transfer_bytes(ConnectionTarget{remote}, request,
                              impl_->config.deadline, false, {});
    return transfer_bytes(ConnectionTarget{remote}, request, impl_->config.deadline,
                          false, p50::P50SourceView::own(
                              std::vector<uint8_t>(source.begin(), source.end())));
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
        false, p50::P50SourceView::own(*bytes));
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
        false, p50::P50SourceView::own(
            std::vector<uint8_t>(source.begin(), source.end())));
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
                          p50::P50SourceView::own(*bytes));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_route(boost::asio::ip::tcp::endpoint remote,
                                    PrepareRequestKey request,
                                    Clock::time_point deadline,
                                    std::span<const uint8_t> source) {
    if (source.size() > impl_->config.endpoint_caps.zstd.max_raw_bytes)
        return transfer_bytes(ConnectionTarget{remote}, request, deadline, true, {});
    return transfer_bytes(ConnectionTarget{remote}, request, deadline, true,
                          p50::P50SourceView::own(std::vector<uint8_t>(
                              source.begin(), source.end())));
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
        p50::P50SourceView::own(*bytes));
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
        p50::P50SourceView::own(std::vector<uint8_t>(source.begin(), source.end())));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_route(ConnectedFdFactory connection,
                                    PrepareRequestKey request,
                                    Clock::time_point deadline,
                                    std::shared_ptr<const p50::P50SourceView> source,
                                    std::optional<Digest128> raw_digest,
                                    std::shared_ptr<std::atomic<ClientStallStage>> stall_stage,
                                    std::function<void()> write_complete) {
    if (source && source->size() > impl_->config.endpoint_caps.zstd.max_raw_bytes)
        source.reset();
    return transfer_bytes(ConnectionTarget{std::move(connection)}, request,
                          deadline, true, std::move(source), raw_digest,
                          std::move(stall_stage), std::move(write_complete));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_bytes(
    ConnectionTarget target,
    PrepareRequestKey request,
    Clock::time_point deadline,
    bool explicit_route,
    std::shared_ptr<const p50::P50SourceView> source,
    std::optional<Digest128> known_digest,
    std::shared_ptr<std::atomic<ClientStallStage>> stall_stage,
    std::function<void()> write_complete) {
    InFlightGuard in_flight_guard(&impl_->transfers_in_flight);
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
    } else if (!std::get<ConnectedFdFactory>(target)) {
        co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    }
    if (!source)
        co_return impl_->invalid(ZstdSourceTransferStatus::SourceError);

    // The view's digest was fused into its read (P50SourceView); a caller
    // that did not pass one (tests use this path) reuses it instead of
    // re-hashing the whole input.
    const Digest128 raw_digest =
        known_digest ? *known_digest : source->digest();
    try {
        if (const auto completed = impl_->completed_for(request, source->span(), raw_digest))
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
    impl_->expire_completed();
    if (impl_->completed.size() >= impl_->config.max_completed_requests)
        co_return impl_->replacement(ZstdSourceTransferStatus::Unavailable,
                                     explicit_route);

    // One route owner serializes its transfers. Retain only this operation's
    // completions so byte accounting is bounded and includes either retry.
    impl_->wire_completions.clear();

    // Step 4d core (research/designs/P50_ROUTE_PIPELINING.md section 1.1,
    // 2026-10-08 errata item 5): wait for a free outstanding-dialogue slot
    // BEFORE prepare_for_route, so a (K+1)-th concurrent transfer never
    // reaches the authority's admission throw
    // (cache/p50_endpoint.cpp:1107-1110) which would escalate to the sticky
    // cold-replacement path. At the default depth of 1 this returns true
    // without suspending. On timeout the transfer takes its ordinary
    // route-local deadline failure, never the sticky path.
    if (impl_->route_bound) {
        const bool slot = co_await impl_->endpoint->wait_for_window_slot(
            std::optional<Clock::time_point>(deadline));
        if (!slot) {
            ZstdSourceTransferResult result =
                impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
            impl_->quarantine_transport(result, explicit_route);
            co_return result;
        }
    }

    PreparedTuHandle prepared;
    uint64_t prepare_route_ns = 0;
    try {
        if (impl_->config.before_prepare_for_route_for_test)
            impl_->config.before_prepare_for_route_for_test();
        const auto prepare_start = Clock::now();
        prepared = impl_->route_bound
            ? impl_->authority->prepare_for_route(impl_->route, request, source->span())
            : impl_->authority->prepare(request, source->span());
        prepare_route_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - prepare_start)
                .count());
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
        // Across BOTH attempts: whether any transactional byte of this
        // transfer (its TX_BEGIN write phase) ever reached the wire. A run
        // that never wrote BEGIN changed nothing about the route's F-side
        // state, so its exhaustion is route-local -- see the attempt==2
        // branch below.
        bool wrote_begin = false;
        try {
            EndpointIoControl io_control;
            io_control.stall_stage = stall_stage.get();
            io_control.write_complete = write_complete;
            if (std::holds_alternative<boost::asio::ip::tcp::endpoint>(target)) {
                run = co_await impl_->endpoint->run(
                    std::get<boost::asio::ip::tcp::endpoint>(target), prepared,
                    io_control, deadline);
                wrote_begin = run.wrote_begin;
                if (impl_->endpoint->has_pipeline_poison()) {
                    // Step 4d: retire this handle's own uncommitted P29V1
                    // route state before anything below treats this attempt
                    // as terminal, while the transfer still counts as in
                    // flight.
                    impl_->authority->abandon_route(prepared);
                    impl_->endpoint->reset_after_pipeline_poison();
                }
            } else {
                int connected_fd = -1;
                // BUSY is capacity only: this attempt has not started and no
                // route or commit state changed.  Wait (bounded, under the
                // original deadline) and reopen the same attempt.
                auto capacity_wait = kCapacityWaitMin;
                for (bool reopen = attempt > 1;; reopen = true) {
                    try {
                        // gcc 11 destroys a prvalue bound to a coroutine's
                        // reference parameter twice; name it first.
                        const std::optional<ReopenExecutor> reopen_executor =
                            reopen ? impl_->config.reopen : std::nullopt;
                        connected_fd = co_await open_connection(
                            std::get<ConnectedFdFactory>(target), deadline,
                            reopen_executor);
                    } catch (...) {
                        report_swallowed_exception("open", attempt);
                        ZstdSourceTransferResult result =
                            impl_->invalid(ZstdSourceTransferStatus::TerminalError);
                        result.attempts = attempt;
                        impl_->require_replacement(result, explicit_route);
                        co_return result;
                    }
                    const auto now = Clock::now();
                    if (connected_fd != kConnectedFdCapacityBusy || now >= deadline)
                        break;
                    boost::asio::steady_timer timer(
                        co_await boost::asio::this_coro::executor,
                        std::min(capacity_wait, deadline - now));
                    boost::system::error_code wait_error;
                    co_await timer.async_wait(
                        boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
                    if (wait_error)
                        break;
                    capacity_wait = std::min(2 * capacity_wait, kCapacityWaitMax);
                }
                if (connected_fd < 0) {
                    run.status = Clock::now() >= deadline
                                     ? ClientRunStatus::DeadlineExceeded
                                     : ClientRunStatus::Disconnected;
                    // The connect/arm round trip itself failed: no endpoint
                    // code ran, so nothing of this attempt reached the wire.
                    wrote_begin = false;
                } else {
                    run = co_await impl_->endpoint->run_adopted_fd(
                        connected_fd, prepared, io_control, deadline);
                    wrote_begin = run.wrote_begin;
                    if (impl_->endpoint->has_pipeline_poison()) {
                        // Step 4d: same retirement as the other branch above.
                        impl_->authority->abandon_route(prepared);
                        impl_->endpoint->reset_after_pipeline_poison();
                    }
                }
            }
        } catch (const P29V1CapabilityUnavailable&) {
            // A HISTORY_RESET found the interner non-runnable: the capability
            // is gone for this READY lease, which is not a route ambiguity.
            throw;
        } catch (...) {
            report_swallowed_exception("run", attempt);
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
            // A healthy commit clears the witness-class retirement streak:
            // the backstop escalates only on REPEATED witness failures
            // without an intervening commit.
            impl_->witness_class_retirements = 0;
            ZstdSourceTransferResult result = impl_->committed_from_witness(
                run, source->size(), raw_digest, attempt, prepare_route_ns);
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
                impl_->remember_completed(request, source->span(), raw_digest, result);
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
        // A failed run -- ANY non-committed status, checked BEFORE the
        // deadline and terminal branches -- whose prepared handle was
        // abandoned with its chain (another member's deviation tore the
        // outstanding chain down and cold-reset the route) must not take
        // the sticky path and must not retry on that handle: the fresh route
        // re-encodes from its own cursor, and a retry dispatched on the
        // stale admitted rel/pre-state fails the F witness (K>2 farm legs,
        // 2026-10-09: te=3 "pipelined successor STATE differs from the
        // predicted route witness" / "TX_COMMIT does not close C's active
        // transaction", or the authority identification logic_error), and
        // the DeadlineExceeded/TerminalError branches would have latched a
        // whole-C sticky replacement for exactly this route-local
        // condition (K4-1 farm leg: every cluster ORIGINATOR was a
        // chain-abandoned handle whose F answer arrived as a te=3
        // terminal). Retire the entry and fail ROUTE-LOCALLY instead: no
        // sticky replacement, no retry on the dead encoding -- the client
        // takes its existing fresh-assignment path and re-prepares under a
        // new request key on this same sender.
        if (impl_->authority->prepared_chain_abandoned(prepared)) {
            try {
                (void)impl_->authority->release(prepared);
            } catch (...) {
            }
            ZstdSourceTransferResult result =
                impl_->invalid(ZstdSourceTransferStatus::RetryExhausted);
            result.attempts = attempt;
            if (run.status == ClientRunStatus::TerminalError)
                result.terminal_error = run.terminal_error;
            impl_->bind_wire_evidence(result);
            // Route-local, deliberately NOT the sticky path: the route
            // encoding this handle was prepared under is gone, nothing about
            // the C sidecar is broken, and the fresh assignment re-prepares
            // under a new request key on this same sender -- EXCEPT under
            // the backstop: a route that keeps producing witness-class
            // terminals (te=3) without an intervening commit has a C-side
            // problem a route reset cannot cure, and escalates to the
            // existing sticky whole-C replacement path.
            if (run.status == ClientRunStatus::TerminalError &&
                run.terminal_error.has_value() &&
                run.terminal_error->code == 3 &&
                ++impl_->witness_class_retirements >=
                    impl_->kWitnessRetireBackstop) {
                impl_->require_replacement(result, explicit_route);
                co_return result;
            }
            result.route_local_failure = true;
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
            // Exhaustion of the single permitted retry is transport-local --
            // EXCEPT when a transactional byte of this transfer reached F:
            // then the route's F-side state may be mid-transaction and the
            // relationship must be quarantined exactly as before (the retry
            // could not reconcile it). Neither attempt having written a
            // BEGIN means F never saw this transfer at all -- a refused
            // connect/arm on both rounds (the k45 farm leg: one f-arm
            // refusal exhausted the retry and latched
            // route_replacement_required, after which every later
            // assignment to that route was refused 0x5002 -- 471 refusals,
            // the rest of the leg) -- so the failure is route-local: no
            // quarantine, no sticky latch. The prepared entry must still be
            // RELEASED: its uncommitted successor slot stays occupied
            // otherwise, and the route's next prepare_for_route hits the
            // depth admission throw, which escalates to exactly the sticky
            // replacement the retirement just avoided (the same 0x5002
            // storm, one step later). release() on a non-tail entry
            // invalidates the whole outstanding chain uniformly, so any
            // live sibling retires through the chain-abandoned branch.
            if (wrote_begin) {
                impl_->quarantine_transport(result, explicit_route);
                co_return result;
            }
            // Nothing on the wire: drop the endpoint's retained retry
            // bookkeeping for this handle (a later attempt's dispatch would
            // otherwise identify against it and throw) and release the
            // authority entry, freeing the route's admission slot. Both are
            // best-effort against typed throws: the handle's structures were
            // marked abandoned uniformly by release()'s own non-tail path if
            // any sibling was outstanding.
            try {
                impl_->endpoint->discard_retained_route_work(prepared);
            } catch (...) {
            }
            try {
                (void)impl_->authority->release(prepared);
            } catch (...) {
            }
            result.route_local_failure = true;
            co_return result;
        }
    }
    co_return impl_->replacement(ZstdSourceTransferStatus::RetryExhausted,
                                 explicit_route);
}

}  // namespace icecc::p50
