#pragma once

#include "cache/p50_endpoint.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace icecc::p50 {

// A sender owns the source descriptor.  It accepts only a regular, complete
// file so that a short read cannot silently turn into a different TU.
class OwnedSourceFd {
public:
    explicit OwnedSourceFd(int fd) noexcept : fd_(fd) {}
    ~OwnedSourceFd();
    OwnedSourceFd(const OwnedSourceFd&) = delete;
    OwnedSourceFd& operator=(const OwnedSourceFd&) = delete;
    OwnedSourceFd(OwnedSourceFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }
    OwnedSourceFd& operator=(OwnedSourceFd&& other) noexcept;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

enum class ZstdSourceTransferStatus : uint8_t {
    Committed,
    SourceError,
    InvalidRequest,
    Unavailable,
    TerminalError,
    RetryExhausted,
    DeadlineExceeded,
    CommittedIdentityUnavailable,
};

struct ZstdSourceTransferResult {
    ZstdSourceTransferStatus status = ZstdSourceTransferStatus::Unavailable;
    ProfileId profile = ProfileId::ZSTD_TU;
    std::optional<InputRecordKey> committed_input;
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    uint8_t attempts = 0;
    std::optional<ErrorMessage> terminal_error;
};

struct ZstdSourceTransferConfig {
    EndpointCaps endpoint_caps{};
    PreparationAuthorityLimits authority_limits{};
    // Completed request identities are retained independently from live
    // preparation entries so releasing a committed handle does not erase
    // replay idempotence.  This is deliberately bounded per sender.
    size_t max_completed_requests = 4096;
    // This is an absolute deadline.  The sender refuses an unbounded or stale
    // deadline and never extends it for the one permitted retry.
    std::chrono::steady_clock::time_point deadline{};
    std::chrono::steady_clock::duration maximum_duration =
        std::chrono::seconds(300);
    int compression_level = 1;
};

// Called once per bounded attempt.  The callback returns ownership of one
// already-connected TCP descriptor that has crossed the ordinary
// CACHE_SESSION boundary, or -1 without leaking a descriptor.  It receives
// the unchanged absolute sender deadline and must not extend it.
using ConnectedFdFactory =
    std::function<int(std::chrono::steady_clock::time_point deadline)>;

// C-side source transfer.  The historical class name is retained for source
// compatibility; endpoint_caps.profile selects the exact P29, ZSTD_TU, or
// ZSTD_ROUTE dialogue and the result records that selection. ZSTD_TU senders
// are one-shot because each transfer owns an independent namespace, while
// P29/ZSTD_ROUTE retain their relationship authority for sequential transfers.
class P50ZstdSourceSender {
public:
    P50ZstdSourceSender(CStoreGuid c_store_guid, PrepareRequestKey request,
                        ZstdSourceTransferConfig config = {});
    ~P50ZstdSourceSender();
    P50ZstdSourceSender(const P50ZstdSourceSender&) = delete;
    P50ZstdSourceSender& operator=(const P50ZstdSourceSender&) = delete;

    boost::asio::awaitable<ZstdSourceTransferResult> transfer(
        boost::asio::ip::tcp::endpoint remote, OwnedSourceFd source);

    boost::asio::awaitable<ZstdSourceTransferResult> transfer(
        boost::asio::ip::tcp::endpoint remote, std::span<const uint8_t> source);

    boost::asio::awaitable<ZstdSourceTransferResult> transfer(
        ConnectedFdFactory connection, OwnedSourceFd source);

    boost::asio::awaitable<ZstdSourceTransferResult> transfer(
        ConnectedFdFactory connection, std::span<const uint8_t> source);

    // A long-lived ZSTD_ROUTE owner must bind every operation to the exact
    // assignment which produced it and to that operation's current deadline.
    // Unlike the compatibility overloads above, these calls never synthesize
    // the next request token and never reuse the constructor deadline.
    boost::asio::awaitable<ZstdSourceTransferResult> transfer_route(
        boost::asio::ip::tcp::endpoint remote, PrepareRequestKey request,
        std::chrono::steady_clock::time_point deadline, OwnedSourceFd source);

    boost::asio::awaitable<ZstdSourceTransferResult> transfer_route(
        boost::asio::ip::tcp::endpoint remote, PrepareRequestKey request,
        std::chrono::steady_clock::time_point deadline,
        std::span<const uint8_t> source);

    boost::asio::awaitable<ZstdSourceTransferResult> transfer_route(
        ConnectedFdFactory connection, PrepareRequestKey request,
        std::chrono::steady_clock::time_point deadline, OwnedSourceFd source);

    boost::asio::awaitable<ZstdSourceTransferResult> transfer_route(
        ConnectedFdFactory connection, PrepareRequestKey request,
        std::chrono::steady_clock::time_point deadline,
        std::span<const uint8_t> source);

private:
    using ConnectionTarget =
        std::variant<boost::asio::ip::tcp::endpoint, ConnectedFdFactory>;

    boost::asio::awaitable<ZstdSourceTransferResult> transfer_bytes(
        ConnectionTarget target,
        PrepareRequestKey request,
        std::chrono::steady_clock::time_point deadline,
        bool explicit_route,
        std::shared_ptr<const std::vector<uint8_t>> source);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace icecc::p50
