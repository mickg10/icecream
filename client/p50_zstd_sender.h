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
    [[nodiscard]] int release() noexcept {
        const int result = fd_;
        fd_ = -1;
        return result;
    }
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
    // Exact CacheWire bytes observed by the C endpoint, including frame
    // headers and bounded retries. These diagnostic witnesses do not grant
    // transfer authority.
    uint64_t c_to_f_bytes = 0;
    uint64_t f_to_c_bytes = 0;
    // Present only for P29V1 after the route pins its two fingerprints.
    std::optional<bool> system_source_reuse;
    // A long-lived sender sets this after any post-prepare outcome whose C/F
    // commit state is ambiguous, or when its bounded replay ledger is full.
    // It is sticky for the route: another distinct request may not open F.
    // Unless route_local_failure is set, the supervised C sidecar must be
    // replaced before any relationship accepts a new request.
    bool replacement_required = false;
    // Transport exhaustion retains/quarantines only this relationship. It
    // must not retire the shared C owner or reject other F relationships.
    // Never set for typed preparation poison or uncertain local state.
    bool route_local_failure = false;
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
    // Deterministic unit-test seam for the typed route-poison boundary.
    // Product callers always leave this empty.
    std::function<void()> before_prepare_for_route_for_test;
};

// Called once per bounded attempt.  The callback returns ownership of one
// already-connected TCP descriptor that has crossed the ordinary
// CACHE_SESSION boundary, or -1 without leaking a descriptor.  It receives
// the unchanged absolute sender deadline and must not extend it.
using ConnectedFdFactory =
    std::function<int(std::chrono::steady_clock::time_point deadline)>;

// C-side source transfer.  The historical class name is retained for source
// compatibility; endpoint_caps.profile selects the exact P29V1, ZSTD_TU, or
// ZSTD_ROUTE dialogue and records that selection.
// Direct ZSTD_TU transfer calls are one-shot.  The assignment-bound
// transfer_route overloads may retain the sender so one stable C/F relationship
// advances TU identity while each ZSTD_TU payload remains independently
// compressed.  P29V1 and ZSTD_ROUTE additionally retain profile-owned state
// for sequential transfers.
class P50ZstdSourceSender {
public:
    P50ZstdSourceSender(CStoreGuid c_store_guid, PrepareRequestKey request,
                        ZstdSourceTransferConfig config = {});
    P50ZstdSourceSender(std::shared_ptr<P50PreparationAuthority> authority,
                        PreparationRouteKey route, PrepareRequestKey request,
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

    // A long-lived route-profile owner must bind every operation to the exact
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

    boost::asio::awaitable<ZstdSourceTransferResult> transfer_route(
        ConnectedFdFactory connection, PrepareRequestKey request,
        std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<const std::vector<uint8_t>> source);

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
