#include "p50_zstd_sender.h"

#include <boost/asio/this_coro.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
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

bool valid_deadline(const ZstdSourceTransferConfig& config, Clock::time_point now) {
    return config.deadline > now && config.maximum_duration > Clock::duration::zero() &&
           config.deadline - now <= config.maximum_duration;
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
        if (config.deadline == Clock::time_point{})
            throw std::invalid_argument("sender requires an absolute deadline");
        if (config.endpoint_caps.zstd.max_raw_bytes > SIZE_MAX)
            throw std::invalid_argument("ZSTD_TU raw limit does not fit this process");
        authority = std::make_shared<P50PreparationAuthority>(
            c_guid, config.endpoint_caps.zstd, config.authority_limits,
            config.compression_level, config.endpoint_caps.profile);
        endpoint = std::make_unique<P50ClientEndpoint>(
            authority, config.endpoint_caps, HistoryNonce{1});
    }

    ZstdSourceTransferResult invalid(ZstdSourceTransferStatus status) const {
        ZstdSourceTransferResult result;
        result.status = status;
        return result;
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
        result.committed_input = run.committed_input;
        result.raw_bytes = raw_bytes;
        result.raw_digest = raw_digest;
        result.attempts = attempts;
        return result;
    }

    CStoreGuid c_guid{};
    PrepareRequestKey request{};
    ZstdSourceTransferConfig config{};
    std::shared_ptr<P50PreparationAuthority> authority;
    std::unique_ptr<P50ClientEndpoint> endpoint;
    bool used = false;
};

P50ZstdSourceSender::P50ZstdSourceSender(CStoreGuid c_store_guid,
                                         PrepareRequestKey request,
                                         ZstdSourceTransferConfig config)
    : impl_(std::make_unique<Impl>(c_store_guid, request, std::move(config))) {}

P50ZstdSourceSender::~P50ZstdSourceSender() = default;

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer(boost::asio::ip::tcp::endpoint remote,
                               OwnedSourceFd source) {
    if (impl_->used) throw std::logic_error("sender is one-shot");
    impl_->used = true;
    const auto bytes = read_complete_fd(source.get(),
                                        impl_->config.endpoint_caps.zstd.max_raw_bytes);
    if (!bytes) return transfer_bytes(ConnectionTarget{remote}, {});
    return transfer_bytes(ConnectionTarget{remote},
                          std::make_shared<const std::vector<uint8_t>>(*bytes));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer(boost::asio::ip::tcp::endpoint remote,
                               std::span<const uint8_t> source) {
    if (impl_->used) throw std::logic_error("sender is one-shot");
    impl_->used = true;
    if (source.size() > impl_->config.endpoint_caps.zstd.max_raw_bytes)
        return transfer_bytes(ConnectionTarget{remote}, {});
    return transfer_bytes(ConnectionTarget{remote},
                          std::make_shared<const std::vector<uint8_t>>(source.begin(),
                                                                         source.end()));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer(ConnectedFdFactory connection,
                              OwnedSourceFd source) {
    if (impl_->used) throw std::logic_error("sender is one-shot");
    impl_->used = true;
    const auto bytes = read_complete_fd(source.get(),
                                        impl_->config.endpoint_caps.zstd.max_raw_bytes);
    if (!bytes)
        return transfer_bytes(ConnectionTarget{std::move(connection)}, {});
    return transfer_bytes(
        ConnectionTarget{std::move(connection)},
        std::make_shared<const std::vector<uint8_t>>(*bytes));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer(ConnectedFdFactory connection,
                              std::span<const uint8_t> source) {
    if (impl_->used) throw std::logic_error("sender is one-shot");
    impl_->used = true;
    if (source.size() > impl_->config.endpoint_caps.zstd.max_raw_bytes)
        return transfer_bytes(ConnectionTarget{std::move(connection)}, {});
    return transfer_bytes(
        ConnectionTarget{std::move(connection)},
        std::make_shared<const std::vector<uint8_t>>(source.begin(), source.end()));
}

boost::asio::awaitable<ZstdSourceTransferResult>
P50ZstdSourceSender::transfer_bytes(
    ConnectionTarget target,
    std::shared_ptr<const std::vector<uint8_t>> source) {
    if (!valid_deadline(impl_->config, Clock::now()))
        co_return impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
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
    PreparedTuHandle prepared;
    try {
        prepared = impl_->authority->prepare(impl_->request, *source);
    } catch (const std::invalid_argument&) {
        co_return impl_->invalid(ZstdSourceTransferStatus::InvalidRequest);
    } catch (const std::length_error&) {
        co_return impl_->invalid(ZstdSourceTransferStatus::SourceError);
    }
    for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
        if (Clock::now() >= impl_->config.deadline)
            co_return impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
        ClientRunResult run;
        try {
            if (std::holds_alternative<boost::asio::ip::tcp::endpoint>(target)) {
                run = co_await impl_->endpoint->run(
                    std::get<boost::asio::ip::tcp::endpoint>(target), prepared, {},
                    impl_->config.deadline);
            } else {
                int connected_fd = -1;
                try {
                    connected_fd = std::get<ConnectedFdFactory>(target)(
                        impl_->config.deadline);
                } catch (...) {
                    co_return impl_->invalid(ZstdSourceTransferStatus::TerminalError);
                }
                if (connected_fd < 0) {
                    run.status = Clock::now() >= impl_->config.deadline
                                     ? ClientRunStatus::DeadlineExceeded
                                     : ClientRunStatus::Disconnected;
                } else {
                    run = co_await impl_->endpoint->run_adopted_fd(
                        connected_fd, prepared, {}, impl_->config.deadline);
                }
            }
        } catch (...) {
            co_return impl_->invalid(ZstdSourceTransferStatus::TerminalError);
        }
        // A validated commit is authoritative even when its completion races
        // the deadline boundary.  The endpoint freezes this witness before
        // its timer may close the owned socket, so a later clock sample must
        // never discard it.
        if (run.status == ClientRunStatus::Committed)
            co_return impl_->committed_from_witness(
                run, source->size(), raw_digest, attempt);
        if (run.status == ClientRunStatus::DeadlineExceeded) {
            ZstdSourceTransferResult result =
                impl_->invalid(ZstdSourceTransferStatus::DeadlineExceeded);
            result.attempts = attempt;
            co_return result;
        }
        if (run.status == ClientRunStatus::TerminalError) {
            ZstdSourceTransferResult result =
                impl_->invalid(ZstdSourceTransferStatus::TerminalError);
            result.attempts = attempt;
            result.terminal_error = run.terminal_error;
            co_return result;
        }
        if (attempt == 2) {
            ZstdSourceTransferResult result =
                impl_->invalid(ZstdSourceTransferStatus::RetryExhausted);
            result.attempts = attempt;
            co_return result;
        }
    }
    co_return impl_->invalid(ZstdSourceTransferStatus::RetryExhausted);
}

}  // namespace icecc::p50
