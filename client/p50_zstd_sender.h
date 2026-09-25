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

// Local-only first-cause attribution for whole-route replacement. This enum
// is diagnostic metadata; it is never serialized on CacheWire.
enum class ReplacementTrigger : uint8_t {
    Unattributed,
    CompletedRequestCapacity,
    ExpiredUnresolvedWitness,
    RouteOwnerAdmissionException,
    EndpointIdentityRetirementCapacity,
    UnexpectedTransferException,
};

[[nodiscard]] constexpr const char* replacement_trigger_name(
    ReplacementTrigger trigger) noexcept {
    switch (trigger) {
    case ReplacementTrigger::CompletedRequestCapacity:
        return "completed_request_capacity";
    case ReplacementTrigger::ExpiredUnresolvedWitness:
        return "expired_unresolved_witness";
    case ReplacementTrigger::RouteOwnerAdmissionException:
        return "route_owner_admission_exception";
    case ReplacementTrigger::EndpointIdentityRetirementCapacity:
        return "endpoint_identity_retirement_capacity";
    case ReplacementTrigger::UnexpectedTransferException:
        return "unexpected_transfer_exception";
    case ReplacementTrigger::Unattributed:
    default:
        return "unattributed";
    }
}

// A validated R2_LINK_REJECT is a terminal result for one exact logical
// route offer. Keep the complete canonical offer so the route owner can retire
// only the matching relationship/incarnation; it is never a commit witness.
struct ZstdSourceRouteRejection {
    LinkRejectReason reason = LinkRejectReason::ReservationMissing;
    LinkHello offered{};
};

struct ZstdSourceTransferResult {
    ZstdSourceTransferStatus status = ZstdSourceTransferStatus::Unavailable;
    ProfileId profile = ProfileId::ZSTD_TU;
    std::optional<InputRecordKey> committed_input;
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    uint8_t attempts = 0;
    // Trace-only availability witnesses. R1 binds its serialized retry count
    // and byte totals; R2 must not present default zeroes as measured values.
    bool attempts_measured = false;
    bool wire_bytes_measured = false;
    // Present only for an exact R2 logical request that reached a terminal
    // result. The sender retains this bounded row across reset/replay and
    // removes it when this snapshot is taken.
    std::optional<R2WireAccountingSnapshot> r2_wire_accounting;
    // Exact immutable identity for the measured terminal snapshot, including
    // failed terminal results that have no committed_input.
    std::optional<R2WireAccountingKey> r2_wire_accounting_key;
    // Compatibility field: in observer mode interval deltas are delivered
    // only through r2_interval_observer and this vector remains empty.
    std::vector<R2WireControlSnapshot> r2_link_intervals;
    // False by default so an absent observer cannot look like measured data.
    bool r2_link_intervals_valid = false;
    // True when interval snapshots are delivered through the configured
    // observer instead of being copied into this result.
    bool r2_link_intervals_external = false;
    bool r2_accounting_reference = false;
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
    ReplacementTrigger replacement_trigger = ReplacementTrigger::Unattributed;
    // Transport exhaustion retains/quarantines only this relationship. It
    // must not retire the shared C owner or reject other F relationships.
    // Never set for typed preparation poison or uncertain local state.
    bool route_local_failure = false;
    // Set only after R2LinkRejected validated both the reason and the exact
    // LINK_HELLO offer. A generic EOF/timeout never populates it. It may
    // accompany a previously validated commit; the commit witness stays final.
    std::optional<ZstdSourceRouteRejection> r2_link_rejection;
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
    // Optional trace-only sink. When set by the diagnostics-enabled service,
    // every interval snapshot is delivered once here on the sender owner;
    // false/throw marks telemetry unavailable and never changes transfer I/O.
    std::function<bool(const R2WireControlSnapshot&)> r2_interval_observer;
    int compression_level = 1;
    // Deterministic unit-test seam for the typed route-poison boundary.
    // Product callers always leave this empty.
    std::function<void()> before_prepare_for_route_for_test;
    // Test-only observation after the complete R2 TU bundle is on the socket;
    // it does not participate in admission or receipt handling.
    std::function<void(uint64_t)> after_r2_bundle_sent_for_test;
    // Test-only observation after the independent reader validated the exact
    // cumulative receipt, before waking callers or starting ACK output.
    std::function<void(uint64_t)> after_r2_receipt_validated_for_test;
    // Test-only observation after a distinct R2 caller is parked waiting for
    // completed-ledger capacity; the callback does not affect admission.
    std::function<void(PrepareRequestKey)>
        after_r2_completed_capacity_waiter_registered_for_test;
    // Deterministic fault seam: after a complete bundle is on the wire, the
    // callback may request a transport close before the independent receipt
    // reader begins. Product callers leave this empty.
    std::function<bool(uint64_t)> disconnect_r2_after_bundle_for_test;
    // One-shot frame-boundary fault injection for the next C bundle write;
    // recovery rebuilds always use the normal framing path.
    EndpointIoControl r2_bundle_io_control_for_test{};
    // Asynchronous test gate that pauses only the receipt reader. Returning
    // true yields through a short timer, leaving the sole writer and F peer
    // independently runnable.
    std::function<bool()> hold_r2_receipt_reader_for_test;
    // Asynchronous test gate for the cumulative ACK pump. It is used to prove
    // that a replacement ARM waits for ACK-only tail work without retiring a
    // relationship that still has an unsettled bundle.
    std::function<bool()> hold_r2_ack_pump_for_test;
    // Test-only seam invoked after a recovery caller has registered on the
    // shared retry timer. Product callers leave this empty.
    std::function<void(std::chrono::steady_clock::duration)>
        after_r2_recovery_waiter_registered_for_test;
    // Request-scoped observation used to prove that a queued ARM reaches the
    // recovery-required branch before a shared reconnect completes.
    std::function<void(PrepareRequestKey)> before_r2_recovery_for_test;
    // Observes the request selected as the writer/coordinator immediately
    // before it attempts the shared recovery operation.
    std::function<void(PrepareRequestKey)> before_r2_recovery_attempt_for_test;
    // Observes an exact positive receipt settled from the RECOVER transcript.
    // Test-only; the callback cannot modify sender state.
    std::function<void(PrepareRequestKey, uint64_t)>
        after_r2_recovery_receipt_settled_for_test;
    // Deterministic disconnect immediately before one replay row is emitted.
    // This seam is used only to probe recovery-owner failure handling.
    std::function<bool(uint64_t, size_t, bool, bool)>
        disconnect_r2_before_replay_bundle_for_test;
    // Called as a detached R2 receipt/ACK pump completes. The route owner
    // uses this only to post an owner-affine deferred-retirement reap; it
    // must not mutate sender or route state inline from the coroutine.
    std::function<void()> on_r2_background_quiescent;
};

// Called once per bounded attempt.  The callback returns ownership of one
// already-connected TCP descriptor that has crossed the ordinary
// CACHE_SESSION boundary, or -1 without leaking a descriptor.  It receives
// the unchanged absolute sender deadline and must not extend it.
using ConnectedFdFactory =
    std::function<int(std::chrono::steady_clock::time_point deadline)>;
using AsyncConnectedFdFactory = std::function<void(
    std::chrono::steady_clock::time_point deadline,
    std::function<void(int)> completion)>;

// C-side source transfer.  The historical class name is retained for source
// compatibility; endpoint_caps.profile selects the exact P29V1, ZSTD_TU, or
// ZSTD_ROUTE dialogue and records that selection.
// Direct ZSTD_TU transfer calls are one-shot.  The assignment-bound
// transfer_route overloads may retain the sender so one stable C/F relationship
// advances TU identity while each ZSTD_TU payload remains independently
// compressed.  P29V1 and ZSTD_ROUTE additionally retain profile-owned state
// for sequential transfers.
class P50ZstdSourceSender : public std::enable_shared_from_this<P50ZstdSourceSender> {
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
        AsyncConnectedFdFactory connection, PrepareRequestKey request,
        std::chrono::steady_clock::time_point deadline,
        std::span<const uint8_t> source);

    // Persistent P51 R2 relationship path. The connector is used once on
    // first use to obtain the clean post-P51_CACHE_LINK_SESSION TCP socket;
    // later exact-incarnation jobs reuse that socket and send bounded bundles
    // through the sole writer while the independent receipt reader advances
    // the cumulative-ACK window.
    boost::asio::awaitable<ZstdSourceTransferResult> transfer_p51_route(
        P51SourceArmedFields armed, uint64_t physical_link_generation,
        AsyncConnectedFdFactory connection, PrepareRequestKey request,
        std::chrono::steady_clock::time_point deadline,
        std::span<const uint8_t> source);

    // F-incarnation retirement fences the old physical link. Shared owner
    // references held by active calls/pumps keep this sender alive to drain.
    void retire_for_replacement() noexcept;
    [[nodiscard]] size_t retained_completion_records_for_test() const noexcept;
    // Owner-affine generation floor used when a strictly newer logical
    // relationship replaces an idle same-key relationship.
    [[nodiscard]] uint64_t current_r2_physical_generation() const noexcept;
    [[nodiscard]] bool can_rebind_r2_relationship() const noexcept;
    // True only while the link has no bundle awaiting a receipt/recovery and
    // any remaining activity is draining cumulative ACK work or its caller.
    [[nodiscard]] bool r2_rebind_waitable() const noexcept;

private:
    using ConnectionTarget =
        std::variant<boost::asio::ip::tcp::endpoint, ConnectedFdFactory,
                     AsyncConnectedFdFactory>;

    boost::asio::awaitable<ZstdSourceTransferResult> transfer_bytes(
        ConnectionTarget target,
        PrepareRequestKey request,
        std::chrono::steady_clock::time_point deadline,
        bool explicit_route,
        std::shared_ptr<const std::vector<uint8_t>> source);

    boost::asio::awaitable<void> run_r2_receipt_reader(
        uint64_t physical_link_generation);
    boost::asio::awaitable<void> run_r2_ack_pump(
        uint64_t physical_link_generation);
    boost::asio::awaitable<void> recover_r2_link(
        AsyncConnectedFdFactory connection, uint64_t requested_generation,
        std::chrono::steady_clock::time_point deadline);
    boost::asio::awaitable<bool> wait_for_r2_recovery_retry(
        std::chrono::steady_clock::time_point deadline);
    boost::asio::awaitable<bool> wait_for_r2_connect_retry(
        std::chrono::steady_clock::time_point deadline);
    boost::asio::awaitable<bool> wait_for_r2_retry_not_before(
        std::chrono::steady_clock::time_point deadline,
        bool require_recovery);
    boost::asio::awaitable<bool> acquire_r2_writer(
        std::chrono::steady_clock::time_point deadline);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace icecc::p50
