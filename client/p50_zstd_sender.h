#pragma once

#include "cache/p50_endpoint.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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
    std::optional<InputRecordKey> committed_input;
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    uint8_t attempts = 0;
    std::optional<ErrorMessage> terminal_error;
};

struct ZstdSourceTransferConfig {
    EndpointCaps endpoint_caps{};
    PreparationAuthorityLimits authority_limits{};
    // This is an absolute deadline.  The sender refuses an unbounded or stale
    // deadline and never extends it for the one permitted retry.
    std::chrono::steady_clock::time_point deadline{};
    std::chrono::steady_clock::duration maximum_duration =
        std::chrono::seconds(300);
    int compression_level = 1;
};

// One-shot C-side source transfer.  A sender instance is intentionally not
// reusable: this keeps request identity, prepared bytes, and retry state in
// one immutable transaction scope.
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

private:
    boost::asio::awaitable<ZstdSourceTransferResult> transfer_bytes(
        boost::asio::ip::tcp::endpoint remote,
        std::shared_ptr<const std::vector<uint8_t>> source);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace icecc::p50
