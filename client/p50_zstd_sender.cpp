#include "p50_zstd_sender.h"

#include <boost/asio/this_coro.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <limits>
#include <deque>
#include <map>
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

bool valid_deadline(Clock::time_point deadline, Clock::duration maximum_duration,
                    Clock::time_point now) {
    return deadline > now && maximum_duration > Clock::duration::zero() &&
           deadline - now <= maximum_duration;
}

bool route_history_profile(ProfileId profile) noexcept {
    return profile == ProfileId::P29V1 || profile == ProfileId::ZSTD_ROUTE;
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
P50ZstdSourceSender::transfer_route(ConnectedFdFactory connection,
                                    PrepareRequestKey request,
                                    Clock::time_point deadline,
                                    std::shared_ptr<const std::vector<uint8_t>> source) {
    if (source && source->size() > impl_->config.endpoint_caps.zstd.max_raw_bytes)
        source.reset();
    return transfer_bytes(ConnectionTarget{std::move(connection)}, request,
                          deadline, true, std::move(source));
}

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
    } else if (!std::get<ConnectedFdFactory>(target)) {
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
    impl_->expire_completed();
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

}  // namespace icecc::p50
