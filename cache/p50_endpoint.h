#pragma once

#include "p50_actions.h"
#include "p50_input_record.h"
#include "p50_profile.h"
#include "p50_sidecar_identity.h"
#include "p50_zstd.h"
#include "p50_slice0.h"
#include "p50_endpoint_run_cancel.h"
#include "services/p50_cache_session_wire.h"
#include "services/comm.h"

#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace icecc::p50 {

class GlobalResourceTrace;

/* A C-side P29V1 authority failed while reserving/initializing its permanent
   interner.  This exception is intentionally narrower than length_error:
   callers may disable only P29V1 for the current supervised READY lease, while
   ordinary per-TU size errors and transport failures remain retryable. */
class P29V1CapabilityUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace sidecar {
class P5coEndpointHandoff;
}

struct EndpointCaps {
    SessionLimits wire{};
    ZstdTuLimits zstd{uint64_t{64} << 20, uint64_t{2} << 30};
    ProfileId profile = ProfileId::ZSTD_TU;
    // C selects one profile for its preparation authority; F advertises the
    // runnable source profiles it can negotiate on each session.  Keeping
    // these distinct permits the READY capability mask to remain truthful
    // while each route still has one concrete profile.
    uint32_t supported_profiles = kOperationalProfileMask;
    auto operator<=>(const EndpointCaps&) const = default;
};

/* One immutable prepared source. Revision 1 has exactly one BODY component;
   interactive P29V1 NEED/FILL state remains owned by its route codec. */
struct PreparedInputEnvelope {
    TxBegin begin;
    std::vector<uint8_t> body;
};
using PreparedInputPtr = std::shared_ptr<const PreparedInputEnvelope>;

enum class AsyncOperationKind : uint8_t {
    Accept,
    Connect,
    ReadHeader,
    ReadPayload,
    WriteFragment,
    WaitPeerClose,
};

std::string_view async_operation_name(AsyncOperationKind operation);

struct CompletionStamp {
    ActorSide actor = ActorSide::C;
    AsyncOperationKind operation = AsyncOperationKind::Connect;
    // Local accounting classification only; it is not a wire or transaction
    // identity. R2 can share a route owner with R1 traffic.
    bool r2_traffic = false;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    std::optional<daemon::P50FSessionOperationId> cache_session_operation;
    std::optional<sidecar::AbsoluteMonotonicDeadline> absolute_deadline;
    uint64_t session_serial = 0;
    HistoryNonce history_nonce{};
    RelSeq rel_seq{};
    TuSeq tu_seq{};
    Digest128 transaction_digest{};
    Digest128 raw_digest{};
    bool transaction_bound = false;
    // R2 accounting identity is frozen with the stamp before async I/O. These
    // fields are diagnostic and never participate in wire validation.
    Id128 logical_link_id{};
    uint64_t relationship_epoch = 0;
    uint64_t physical_link_generation = 0;
    bool replay_attempt = false;
    auto operator<=>(const CompletionStamp&) const = default;
};

struct R2WireAccountingKey {
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    Id128 logical_link_id{};
    TuSeq tu_seq{};
    Digest128 raw_digest{};
    auto operator<=>(const R2WireAccountingKey&) const = default;
};

struct R2WireLinkIdentity {
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    Id128 logical_link_id{};
    uint64_t relationship_epoch = 0;
    uint64_t physical_link_generation = 0;
    auto operator<=>(const R2WireLinkIdentity&) const = default;
};

struct R2WireAccountingSnapshot {
    uint64_t c_to_f_bundle_bytes = 0;
    uint64_t f_to_c_receipt_bytes = 0;
    uint32_t bundle_attempts = 0;
    uint32_t replay_attempts = 0;
    bool valid = true;
};

enum class R2WireIntervalEnd : uint8_t {
    DrainedAckCheckpoint,
    RecoveryConfirmed,
    PhysicalLinkRetired,
    WindowPressure,
};

struct R2WireJobInterval {
    R2WireAccountingKey key{};
    R2WireLinkIdentity link{};
    uint64_t c_to_f_bundle_bytes = 0;
    uint64_t f_to_c_receipt_bytes = 0;
    uint32_t bundle_attempts = 0;
    uint32_t replay_attempts = 0;
    bool valid = true;
};

struct R2WireControlSnapshot {
    R2WireLinkIdentity link{};
    // Deltas since the preceding emitted checkpoint for this link identity.
    // Job rows and shared totals are disjoint classifications of `total_*`.
    uint64_t interval_sequence = 0;
    // Highest cumulative ACK prefix whose write completed before this
    // checkpoint; zero for non-ACK boundaries such as WindowPressure.
    uint64_t drained_ack_prefix = 0;
    // F-confirmed recovery prefix at a RESET_CONFIRM boundary. This is not a
    // COMMIT_ACK write and must never be merged into drained_ack_prefix.
    uint64_t recovery_confirmed_prefix = 0;
    R2WireIntervalEnd end = R2WireIntervalEnd::DrainedAckCheckpoint;
    uint64_t total_c_to_f_bytes = 0;
    uint64_t total_f_to_c_bytes = 0;
    uint64_t shared_c_to_f_bytes = 0;
    uint64_t shared_f_to_c_bytes = 0;
    std::vector<R2WireJobInterval> jobs;
    bool valid = true;
};

// The endpoint identity derived from current product state after an asynchronous
// operation completes.  It intentionally excludes the operation kind: that is
// frozen in CompletionStamp and checked on the observed-completion path.
struct CompletionLiveIdentity {
    ActorSide actor = ActorSide::C;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    std::optional<daemon::P50FSessionOperationId> cache_session_operation;
    std::optional<sidecar::AbsoluteMonotonicDeadline> absolute_deadline;
    uint64_t session_serial = 0;
    HistoryNonce history_nonce{};
    RelSeq rel_seq{};
    TuSeq tu_seq{};
    Digest128 transaction_digest{};
    Digest128 raw_digest{};
    bool transaction_bound = false;
    auto operator<=>(const CompletionLiveIdentity&) const = default;
};

struct AsyncCompletion {
    CompletionStamp stamp{};
    uint64_t transferred_bytes = 0;
    int error_value = 0;
    auto operator<=>(const AsyncCompletion&) const = default;
};

class CompletionLog {
public:
    enum class StorageMode : uint8_t { Detailed, ClientByteTotals };

    explicit CompletionLog(size_t max_records = std::numeric_limits<size_t>::max())
        : max_records_(max_records) {}
    explicit CompletionLog(StorageMode mode)
        : mode_(mode) {}

    void enable_r2_accounting() noexcept {
        r2_accounting_enabled_ = true;
    }

    bool r2_accounting_enabled() const noexcept {
        return r2_accounting_enabled_;
    }

    bool r2_accounting_available() const noexcept {
        return !r2_accounting_poisoned_.load(std::memory_order_acquire);
    }

    void record(AsyncCompletion completion) noexcept {
        if (r2_accounting_enabled_)
            record_r2_accounting(completion);
        if (!valid_)
            return;
        if (mode_ == StorageMode::ClientByteTotals) {
            if (completion.stamp.r2_traffic ||
                completion.stamp.actor != ActorSide::C)
                return;
            uint64_t* total = nullptr;
            if (completion.stamp.operation == AsyncOperationKind::WriteFragment)
                total = &c_to_f_bytes_;
            else if (completion.stamp.operation == AsyncOperationKind::ReadHeader ||
                     completion.stamp.operation == AsyncOperationKind::ReadPayload)
                total = &f_to_c_bytes_;
            if (total != nullptr) {
                if (completion.transferred_bytes >
                    std::numeric_limits<uint64_t>::max() - *total) {
                    valid_ = false;
                    return;
                }
                *total += completion.transferred_bytes;
            }
            return;
        }
        if (completions_.size() >= max_records_) {
            valid_ = false;
            return;
        }
        try {
            completions_.push_back(std::move(completion));
        } catch (...) {
            valid_ = false;
        }
    }
    [[nodiscard]] const std::vector<AsyncCompletion>& completions() const { return completions_; }
    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] uint64_t c_to_f_bytes() const { return c_to_f_bytes_; }
    [[nodiscard]] uint64_t f_to_c_bytes() const { return f_to_c_bytes_; }
    [[nodiscard]] size_t retained_record_count() const { return completions_.size(); }
    void clear() {
        completions_.clear();
        c_to_f_bytes_ = 0;
        f_to_c_bytes_ = 0;
        valid_ = true;
    }

    // R2 job rows are bounded by the negotiated window (maximum 30). A
    // transaction keeps the same key through physical reconnect/reindex.
    bool begin_r2_job(const R2WireAccountingKey& key) noexcept;
    void begin_r2_bundle_attempt(const CompletionStamp& stamp) noexcept;
    void begin_r2_receipt_read(const R2WireAccountingKey& key,
                               R2WireLinkIdentity link) noexcept;
    void confirm_r2_receipt(const R2WireAccountingKey& key) noexcept;
    void reject_r2_receipt(const R2WireAccountingKey& key) noexcept;
    R2WireAccountingSnapshot finish_r2_job(
        const R2WireAccountingKey& key) noexcept;
    void request_r2_interval_checkpoint(R2WireLinkIdentity link,
                                        R2WireIntervalEnd end,
                                        uint64_t drained_ack_prefix = 0,
                                        uint64_t recovery_confirmed_prefix = 0) noexcept;
    std::vector<R2WireControlSnapshot> drain_r2_interval_snapshots() noexcept;
    void mark_r2_accounting_unavailable() noexcept {
        r2_accounting_poisoned_.store(true, std::memory_order_release);
    }

private:
    struct R2ActiveJob {
        R2WireAccountingSnapshot totals{};
        R2WireJobInterval interval{};
        std::optional<R2WireLinkIdentity> interval_link;
        uint64_t provisional_receipt_bytes = 0;
        bool receipt_read_active = false;
        R2WireLinkIdentity provisional_link{};
    };
    struct R2PendingCheckpoint {
        R2WireLinkIdentity link{};
        R2WireIntervalEnd end = R2WireIntervalEnd::DrainedAckCheckpoint;
        uint64_t drained_ack_prefix = 0;
        uint64_t recovery_confirmed_prefix = 0;
    };
    void record_r2_accounting(const AsyncCompletion& completion) noexcept;
    void record_r2_unattributed_locked(uint64_t bytes, bool write,
                                      R2WireLinkIdentity link) noexcept;
    bool snapshot_r2_interval_locked(R2WireLinkIdentity link,
                                    R2WireIntervalEnd end,
                                    uint64_t drained_ack_prefix,
                                    uint64_t recovery_confirmed_prefix) noexcept;
    void service_r2_checkpoints_locked() noexcept;
    bool set_r2_interval_link_locked(R2WireLinkIdentity link) noexcept;
    void record_r2_interval_bytes_locked(uint64_t bytes, bool write,
                                        R2WireLinkIdentity link) noexcept;
    static bool add_r2_count(uint64_t amount, uint64_t& total) noexcept;
    mutable std::mutex r2_mutex_;
    std::map<R2WireAccountingKey, R2ActiveJob> r2_active_jobs_;
    uint64_t r2_unattributed_write_bytes_ = 0;
    uint64_t r2_unattributed_read_bytes_ = 0;
    uint64_t r2_interval_write_bytes_ = 0;
    uint64_t r2_interval_read_bytes_ = 0;
    bool r2_unattributed_valid_ = true;
    std::atomic<bool> r2_accounting_poisoned_{false};
    std::optional<R2WireLinkIdentity> r2_unattributed_link_;
    std::optional<R2WireLinkIdentity> r2_interval_link_;
    uint64_t r2_unattributed_sequence_ = 0;
    std::vector<R2WireJobInterval> r2_finished_interval_jobs_;
    std::deque<R2PendingCheckpoint> r2_pending_checkpoints_;
    std::deque<R2WireControlSnapshot> r2_ready_interval_snapshots_;
    StorageMode mode_ = StorageMode::Detailed;
    bool r2_accounting_enabled_ = false;
    size_t max_records_ = std::numeric_limits<size_t>::max();
    bool valid_ = true;
    uint64_t c_to_f_bytes_ = 0;
    uint64_t f_to_c_bytes_ = 0;
    std::vector<AsyncCompletion> completions_;
};

// This control is a deterministic test seam around message boundaries and write
// fragmentation. Normal product callers use the default value.
struct EndpointIoControl {
    size_t max_write_fragment = std::numeric_limits<size_t>::max();
    std::optional<MessageType> close_before_write;
    std::optional<MessageType> close_after_write;
    std::optional<AsyncOperationKind> wrong_digest_completion;
    std::optional<AsyncOperationKind> wrong_raw_digest_completion;
    // Receives only the observed test copy; the endpoint retains and checks
    // its independently frozen pre-await completion stamp.
    std::function<void(CompletionStamp&)> before_completion_check;
    // Receives only a derived live-identity copy. Product endpoint state stays
    // private, and both endpoints use the same field-by-field validator.
    std::function<void(const CompletionStamp&, CompletionLiveIdentity&)>
        before_live_identity_check;
    // Test-only copied-begin transform: applied to a COPY of the outbound
    // TX_BEGIN after the client coroutine constructs it and before the
    // outbound-admission validation/send, so a test can drive the production
    // validator AND its production callsite with a wire-valid but
    // unnegotiated begin. The retained prepared record is never mutated.
    // Product callers leave it unset.
    std::function<TxBegin(const TxBegin&, std::span<const uint8_t>)>
        outbound_begin_transform;
    // Test-only cancellation linearization hook immediately before the first
    // CacheWire write becomes possible.  It receives no endpoint state;
    // product callers leave it unset.  This lets the deletion gate prove the
    // narrow no-remote-transmission cancellation outcome deterministically.
    std::function<void()> before_first_remote_write;
    // Test-only worker hook.  It executes after the immutable complete-BODY
    // materialization job leaves the endpoint owner and before codec work.
    // Product callers leave it unset; it proves the owner timer cannot be
    // starved by a blocked codec worker.
    std::function<void()> before_materialize_on_worker;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    // Exact parsed-P51 target at the same worker boundary. The bundle has
    // reached BODY closure, but no input publication or R2 receipt exists yet.
    // Carries immutable identities only; never copies source payload bytes.
    std::function<void(const JobBind&, const TxBegin&, const TxCommit&)>
        before_materialize_identified_for_test;
    // Inbound strict-prefix witness for a parsed P51 job. Called after the
    // complete frame header is read and before its payload read begins.
    std::function<void(const JobBind&, const TxBegin&, MessageType, size_t)>
        after_r2_component_header_for_test;
    // Outbound fragmentation seam. The callback runs after each successful
    // fragment and may asynchronously gate the sender in deterministic tests.
    std::function<boost::asio::awaitable<void>(const Message&, size_t, size_t)>
        after_write_fragment_for_test;
#endif
    // Simulator/test-only observation after one complete message has been
    // written successfully. It receives an immutable message copy and cannot
    // change product framing or endpoint state.
    std::function<void(ActorSide, const Message&)> outbound_message_observer;
    // Optional R2 diagnostics callback. Fired once, after the accepted socket
    // has been closed and all awaited socket I/O in this link coroutine has
    // completed. Prefixes are F's final per-link K/Q snapshot; this is not a
    // wire message and does not participate in protocol state.
    std::function<void(const LinkHello&, uint64_t committed_prefix_k,
                       uint64_t acknowledged_prefix_q)>
        r2_link_io_quiesced_observer;
    // Test-only copy transform for an F endpoint's RESET_CONFIRM echo. The
    // server applies and retains the received confirmation before this echo
    // copy is transformed; product callers leave it unset.
    std::function<ResetConfirm(const ResetConfirm&)>
        r2_reset_confirm_echo_transform_for_test;
    // Test-only observation fired after an R2 setup socket is registered for
    // incarnation cancellation and before the first protocol read starts.
    // Product callers leave it unset.
    std::function<void()> after_r2_setup_registered;
};

struct PrepareRequestKey {
    uint64_t producer_session = 0;
    uint64_t request_token = 0;
    auto operator<=>(const PrepareRequestKey&) const = default;
};

// C-wide preparation is shared, but route-profile planning is relationship
// scoped.  The F generation fences a replacement route from its predecessor.
struct PreparationRouteKey {
    FStoreGuid f_store_guid{};
    uint64_t f_store_generation = 0;
    ProfileId profile = ProfileId::ZSTD_TU;
    auto operator<=>(const PreparationRouteKey&) const = default;
};

struct PreparationAuthorityLimits {
    size_t max_live_entries = 4096;
    uint64_t max_retained_encoded_bytes = uint64_t{512} << 20;
    uint64_t max_interner_reserved_bytes = UINT64_C(2463121408);
    uint64_t max_route_state_bytes = uint64_t{1} << 30;
    uint32_t max_speculative_tus = 1;
    uint64_t max_speculative_raw_bytes = uint64_t{512} << 20;
    auto operator<=>(const PreparationAuthorityLimits&) const = default;
};

class PreparedTuHandle {
public:
    PreparedTuHandle() = default;

    [[nodiscard]] explicit operator bool() const {
        return entry_id_ != 0 && !authority_.expired();
    }

    friend bool operator==(const PreparedTuHandle& left, const PreparedTuHandle& right) {
        const bool same_authority = !left.authority_.owner_before(right.authority_) &&
                                    !right.authority_.owner_before(left.authority_);
        return left.entry_id_ == right.entry_id_ && same_authority;
    }

private:
    PreparedTuHandle(std::weak_ptr<const void> authority, uint64_t entry_id)
        : authority_(std::move(authority)), entry_id_(entry_id) {}

    std::weak_ptr<const void> authority_;
    uint64_t entry_id_ = 0;

    friend class P50PreparationAuthority;
};

class P50PreparationAuthority {
public:
    explicit P50PreparationAuthority(
        CStoreGuid c_store_guid,
        ZstdTuLimits zstd_limits = {uint64_t{64} << 20, uint64_t{2} << 30},
                                     PreparationAuthorityLimits authority_limits = {},
                                     int compression_level = 1,
                                     ProfileId profile = ProfileId::ZSTD_TU,
                                     TuSeq first_tu_seq = {});
    P50PreparationAuthority(
        CStoreGuid c_store_guid, ZstdTuLimits zstd_limits,
        PreparationAuthorityLimits authority_limits, int compression_level,
        ProfileId profile, TuSeq first_tu_seq,
        P29InternerFaultInjection fault_injection);
    ~P50PreparationAuthority();
    P50PreparationAuthority(const P50PreparationAuthority&) = delete;
    P50PreparationAuthority& operator=(const P50PreparationAuthority&) = delete;

    PreparedTuHandle prepare(PrepareRequestKey request,
                             std::span<const uint8_t> exact_input);
    PreparedTuHandle prepare_for_route(PreparationRouteKey route,
                                       PrepareRequestKey request,
                                       std::span<const uint8_t> exact_input);
    std::span<const uint8_t> answer_p29v1_need(
        PreparedTuHandle handle, std::span<const uint8_t> inner_need);
    [[nodiscard]] std::vector<uint8_t> predicted_p29v1_need(
        PreparedTuHandle handle);
    void advance_p29v1_speculative(PreparedTuHandle handle);
    // Advances/stages in relationship order; this does not assert that any
    // bytes reached the socket. P29V1 must use the explicit NEED/FILL path
    // above before advancing its continuing codec state.
    void advance_speculative(PreparedTuHandle handle);
    // Coordinated R2 transport recovery resets only relationship codec
    // history. C-wide TU/job identity and immutable source records survive.
    // retired_suffix contains exact handles proved not replayable after the
    // confirmed reset (F-unavailable or locally expired after the old-link
    // fence); the caller retains those distinct terminal dispositions.
    void reset_r2_route_for_recovery(PreparationRouteKey route,
                                     FStoreGuid f_store_guid,
                                     HistoryNonce history_nonce,
                                     std::span<const PreparedTuHandle>
                                         retired_suffix = {});
    void rebuild_r2_entry_for_recovery(PreparedTuHandle handle,
                                       Digest128 f_system_source_fingerprint);
    [[nodiscard]] TxBegin r2_staged_begin(
        PreparedTuHandle handle, HistoryNonce history_nonce,
        RelSeq rel_seq, Digest128 pre_state_digest) const;
    // R2 callers must pass the actual receiver receipt. The legacy commit()
    // overload remains for the R1 single-active-TU path only.
    void accept_commit(PreparedTuHandle handle, const TxCommit& receipt);
    void accept_commit(PreparedTuHandle handle, const TxBegin& sent_begin,
                       const TxCommit& receipt);
    void accept_p29v1_commit(PreparedTuHandle handle, const TxCommit& receipt);
    [[nodiscard]] Digest128 p29v1_system_source_fingerprint(
        PreparedTuHandle handle) const;
    [[nodiscard]] std::optional<bool> p29v1_system_source_reuse(
        PreparedTuHandle handle) const;
    void pin_p29v1_system_source_reuse(PreparedTuHandle handle,
                                       Digest128 f_fingerprint);
    void restart_p29v1_transport_retry(PreparedTuHandle handle);
    PreparedInputPtr reset_p29v1_route(PreparedTuHandle handle,
                                       FStoreGuid f_store_guid,
                                       HistoryNonce history_nonce);
    uint64_t retain(PreparedTuHandle handle);
    uint64_t release(PreparedTuHandle handle);
    // Remove precisely one unadvanced tail preparation after the caller has
    // established that no R2 JOB_BIND write began. P29 additionally requires
    // the exact active pre-FILL TU; ordinary release() stays strict.
    void cancel_unwritten_tail(PreparedTuHandle handle);
    void commit(PreparedTuHandle handle);
    [[nodiscard]] CStoreGuid c_store_guid() const;
    [[nodiscard]] ZstdTuLimits zstd_limits() const;
    [[nodiscard]] bool contains(PreparedTuHandle handle) const;
    // Exposes the authenticated C-wide identity carried by a prepared route
    // view; all forks of one request must report the same value.
    [[nodiscard]] TuSeq prepared_tu_seq(PreparedTuHandle handle) const;
    [[nodiscard]] ProfileId prepared_profile(PreparedTuHandle handle) const;
    [[nodiscard]] size_t live_entry_count() const;
    [[nodiscard]] uint64_t retained_encoded_bytes() const;
    [[nodiscard]] uint64_t p29v1_interner_reserved_bytes() const;
    [[nodiscard]] uint64_t p29v1_interner_committed_bytes() const;
    [[nodiscard]] uint64_t p29v1_route_state_bytes(
        PreparationRouteKey route) const;
    [[nodiscard]] size_t route_history_bytes() const;
    [[nodiscard]] size_t route_history_bytes(PreparationRouteKey route) const;
    [[nodiscard]] Digest128 route_history_digest() const;
    [[nodiscard]] Digest128 route_history_digest(PreparationRouteKey route) const;
    [[nodiscard]] size_t route_history_entries() const;
    [[nodiscard]] size_t route_history_entries(PreparationRouteKey route) const;
    [[nodiscard]] ProfileId profile() const;

    // Route owners call this only after all handles for the relationship have
    // been released.  It drops matcher/history state without touching the
    // C-wide immutable catalogue or TU allocator.
    [[nodiscard]] bool reset_route(PreparationRouteKey route) noexcept;
    // R2 replacement-only retirement. The caller must first fence the exact
    // physical sender incarnation and wait until no reader/writer/caller can
    // still use its handles. This discards only that relationship's live
    // route views and history; C-wide TU allocation and views owned by other
    // routes are preserved. It never fabricates a receiver commit.
    [[nodiscard]] bool abandon_retired_route(
        PreparationRouteKey route) noexcept;

private:
    PreparedInputPtr resolve(PreparedTuHandle handle) const;
    void validate_begin(const TxBegin& begin) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class P50ClientEndpoint;
    friend struct P50PreparationAuthorityTestAccess;
};

enum class EndpointReconnectOutcome : uint8_t {
    ExactMatch,
    LostFinalAcknowledgement,
    ColdFStore,
    RouteHistoryReset,
    WrongAdoptedPeer,
};

enum class ClientRunStatus : uint8_t {
    Committed,
    Disconnected,
    DeadlineExceeded,
    TerminalError,
};

// Operation-scoped cancellation is intentionally distinct from an ordinary
// transport disconnect.  A caller may authorize a clean pre-durable abort
// only while the endpoint can prove that no CacheWire byte may have reached
// the peer and no previously active transaction is being reconciled.
enum class ClientCancellationDisposition : uint8_t {
    None,
    ReconcileRequired,
};

// A local endpoint observation never proves durability.  Only the owning
// FSession operation may later settle this observation as committed or
// pre-durable-aborted.
enum class ClientRunObservation : uint8_t {
    Unresolved,
    ExactCommitObserved,
    PeerTerminalFrame,
    ProtocolViolation,
    Disconnected,
    DeadlineExpired,
    Cancelled,
    WrongAdoptedPeer,
    ReconcileRequired,
    LocalFailure,
};

struct ClientRunResult {
    ClientRunStatus status = ClientRunStatus::Disconnected;
    EndpointReconnectOutcome reconnect = EndpointReconnectOutcome::ExactMatch;
    ClientRunObservation observation = ClientRunObservation::Unresolved;
    ClientCancellationDisposition cancellation =
        ClientCancellationDisposition::None;
    // These witnesses are populated only after the endpoint has accepted the
    // exact, fully validated TX_COMMIT.  They are deliberately independent of
    // ActionTrace (which is diagnostic, not an authority).
    std::optional<TxCommit> committed_commit;
    std::optional<InputRecordKey> committed_input;
    std::optional<ErrorMessage> terminal_error;
};

// A peer can ask the sender to retire only the exact LINK_HELLO offer. This
// exception is thrown only after the reject's digest has been checked against
// the canonical bytes sent on this socket; EOF and all other protocol errors
// remain ordinary transport/protocol failures.
class R2LinkRejected final : public std::runtime_error {
public:
    R2LinkRejected(LinkRejectMessage rejection, LinkHello offered)
        : std::runtime_error("peer rejected exact R2 LINK_HELLO"),
          rejection(std::move(rejection)), offered(std::move(offered)) {}

    LinkRejectMessage rejection;
    LinkHello offered;
};

// Immutable C-side witness for one completely written R2 TU bundle. It is
// produced by the sole writer and consumed independently by the receipt
// reader; creation does not imply F commit or advance the confirmed cursor.
struct R2SentBundle {
    JobBind binding{};
    TuBegin begin{};
    Digest128 binding_digest{};
    Digest128 transaction_digest{};
    PreparedTuHandle prepared{};
};

struct R2RecoveryResult {
    LinkState link_state{};
    std::vector<R2TxCommit> committed_receipts;
    ResetRequest reset_request{};
    ResetAck reset_ack{};
};

enum class ServerRunStatus : uint8_t {
    Completed,
    Disconnected,
    TerminalError,
    DeadlineExceeded,
};

struct ServerRunResult {
    ServerRunStatus status = ServerRunStatus::Disconnected;
    uint64_t session_serial = 0;
    // Codec work performed on F after receiving profile data: applying FILL
    // fragments plus exact materialization/verification. Queueing, network
    // waits, selector callbacks, and owner-side publication are excluded.
    uint64_t f_apply_materialize_ns = 0;
    std::optional<CStoreGuid> c_store_guid;
    // candidate_input is set before the job-state selector. completed_input is
    // set only after exact publication/closed-commit validation succeeds.
    // committed_input remains the narrower compiler-attachable witness.
    std::optional<InputRecordKey> candidate_input;
    std::optional<InputRecordKey> completed_input;
    std::optional<InputRecordKey> committed_input;
    std::optional<ErrorMessage> terminal_error;
};

enum class InputJobState : uint8_t {
    Open,
    Closed,
};

// Runs on the endpoint state owner after exact materialization and before
// InputRecord publication or route visibility. Throwing leaves the exact
// transaction identity available for replay.
using InputJobStateSelector = std::function<InputJobState(
    CStoreGuid, const TxBegin&, const TxCommit&, std::span<const uint8_t>)>;

struct P50ServerOwnerLimits {
    size_t max_live_sessions = 64;
    size_t max_namespaces = 4096;
    uint64_t max_pending_encoded_bytes = uint64_t{512} << 20;
    uint64_t max_pending_raw_bytes = uint64_t{8} << 30;
    uint64_t max_decoder_window_bytes = uint64_t{8} << 30;
    size_t max_retained_input_records = 4096;
    uint64_t max_retained_input_bytes = uint64_t{8} << 30;
    auto operator<=>(const P50ServerOwnerLimits&) const = default;
};

struct P50ServerOwnerUsage {
    size_t live_sessions = 0;
    size_t namespaces = 0;
    size_t revisions = 0;
    uint64_t pending_encoded_bytes = 0;
    uint64_t pending_raw_bytes = 0;
    uint64_t decoder_window_bytes = 0;
    uint64_t detached_history_bytes = 0;
    uint64_t global_detached_resident_bytes = 0;
    size_t retained_input_records = 0;
    uint64_t retained_input_bytes = 0;
    auto operator<=>(const P50ServerOwnerUsage&) const = default;
};

// A JOB_BIND consumes the exact daemon-armed source lease. Keep the original
// same-host deadline and input lifecycle key with it; source_budget_msec is
// only an advertised bound and must never be used to mint a fresh deadline.
struct P51SourceJobLease {
    P51SourceArmedFields armed{};
    sidecar::AbsoluteMonotonicDeadline absolute_deadline{};
    JobBind binding{};
    Digest128 binding_digest{};
    InputRecordKey input_key{};
};

struct P51SourceLinkLease {
    P51SourceArmedFields initial_armed{};
    sidecar::AbsoluteMonotonicDeadline absolute_deadline{};
    bool reconnect = false;
    uint64_t relationship_epoch = 0;
    HistoryNonce history_nonce{};
    uint64_t committed_prefix_k = 0;
    uint64_t acknowledged_prefix_q = 0;
};

enum class P51SourceLinkLookupStatus : uint8_t {
    Found,
    ReservationMissing,
    Invalid,
};

struct P51SourceLinkLookupResult {
    P51SourceLinkLookupStatus status = P51SourceLinkLookupStatus::Invalid;
    std::optional<P51SourceLinkLease> lease;

    P51SourceLinkLookupResult() = default;
    // Compatibility for endpoint fixtures that return an optional lease:
    // an empty optional is ambiguous and therefore never means terminal
    // ReservationMissing.
    P51SourceLinkLookupResult(std::optional<P51SourceLinkLease> value)
        : status(value ? P51SourceLinkLookupStatus::Found
                       : P51SourceLinkLookupStatus::Invalid),
          lease(std::move(value)) {}
    P51SourceLinkLookupResult(P51SourceLinkLookupStatus lookup_status,
                              std::optional<P51SourceLinkLease> value)
        : status(lookup_status), lease(std::move(value)) {}
    [[nodiscard]] bool has_value() const noexcept {
        return status == P51SourceLinkLookupStatus::Found && lease.has_value();
    }
    operator std::optional<P51SourceLinkLease>() const {
        return has_value() ? lease : std::nullopt;
    }
};

struct P51RecoveryReceiptInterval {
    ReceiptsEnd end{};
    std::vector<ReceiptRow> rows;
};

struct P50ServerEndpointConfig {
    uint16_t protocol_error_code = 1;
    P50ServerOwnerLimits owner_limits{};
    InputJobStateSelector input_job_state;
    std::optional<SidecarLaunchIdentity> sidecar_launch;
    uint64_t endpoint_generation = 1;
    // Optional authoritative F-store generation. Standalone endpoint tests
    // may omit it; SidecarRuntime always supplies its allocator-issued value.
    // This lets R2 distinguish a replaced same-GUID incarnation from an
    // unrelated missing reservation without over-classifying null lookups.
    uint64_t f_store_generation = 0;
    // Optional owner-visible trace for the global resource binding. The
    // endpoint never owns this sink; callers keep it alive for the endpoint.
    GlobalResourceTrace* global_resource_trace = nullptr;
    // Called on the endpoint owner immediately after an input record is
    // published and before TX_COMMIT is exposed to the peer.  Sidecar
    // lifecycle ownership must observe this edge before a daemon can issue
    // the corresponding attachment.
    std::function<void(InputRecordKey, bool)> on_input_committed;
    std::function<void(EndpointCancelPermit)> on_run_admitted;
    std::function<void(EndpointCancelPermit, EndpointTerminalResult)> on_run_terminal;
    // SidecarRuntime installs these endpoint-owner callbacks for R2. HELLO
    // lookup is non-consuming; each JOB_BIND consumes one exact reservation
    // and returns the immutable input/deadline binding used through commit.
    // No control-worker route-map access is permitted.
    // Lookup preserves definite absence (terminal R2 reservation miss) from
    // present-but-invalid/stale offers. Empty optional conversion is Invalid.
    std::function<P51SourceLinkLookupResult(const LinkHello&)>
        lookup_p51_link_reservation;
    std::function<std::optional<P51SourceJobLease>(const LinkHello&, const JobBind&)>
        consume_p51_job_reservation;
    std::function<bool(const LinkHello&, const JobBind&)>
        authorize_p51_job_publication;
    std::function<void(const LinkHello&, const JobBind&)>
        settle_p51_cancelled_job;
    std::function<bool(const LinkHello&, const JobBind&, const R2TxCommit&)>
        record_p51_job_commit;
    std::function<bool(const LinkHello&, const CommitAck&)>
        acknowledge_p51_receipt;
    std::function<std::optional<P51RecoveryReceiptInterval>(
        const LinkHello&, const RecoverBegin&,
        std::span<const RecoverWitness>, const RecoverEnd&)>
        recover_p51_receipts;
    std::function<bool(const LinkHello&)> settle_p51_interrupted_job;
    // True only when the authoritative F reservation owner has permanently
    // cancelled/expired this exact retained job. Used to retire its bounded
    // uncommitted global-install tombstone; a failed consume is not proof.
    std::function<bool(const JobBind&)> p51_source_reservation_terminal;
    std::function<std::optional<ResetAck>(const LinkHello&, const ResetRequest&)>
        validate_p51_reset;
    std::function<bool(const LinkHello&, const ResetRequest&, const ResetAck&)>
        commit_p51_reset;
    std::function<bool(const LinkHello&, const ResetConfirm&)>
        confirm_p51_reset;
    std::function<void(const LinkHello&,
                       const std::optional<JobBind>& unpublished_binding)>
        on_p51_link_terminal;
};

// Outbound-admission law shared by the client's production send path and its
// tests: a C endpoint must refuse to place a TX_BEGIN whose profile lies
// outside the session's negotiated mask on the wire. Pure over its inputs --
// callers pass a COPY of the outbound begin and the session's negotiated
// mask, so exercising the law mutates no retained endpoint state.
void require_outbound_profile_negotiated(uint32_t negotiated_profiles,
                                         const TxBegin& begin);

class P50ClientEndpoint {
public:
    explicit P50ClientEndpoint(std::shared_ptr<P50PreparationAuthority> preparation,
                               EndpointCaps caps = {},
                               HistoryNonce first_history_nonce = HistoryNonce{1},
                               CompletionLog* completions = nullptr,
                               ActionTrace* actions = nullptr,
                               std::optional<EndpointRunIdentity> run_identity_seed =
                                   std::nullopt,
                               std::function<void(EndpointCancelPermit)>
                                   on_run_admitted = {},
                               std::function<void(EndpointCancelPermit,
                                                  EndpointTerminalResult)>
                                   on_run_terminal = {},
                               std::optional<PreparationRouteKey> route =
                                   std::nullopt);
    ~P50ClientEndpoint();
    P50ClientEndpoint(const P50ClientEndpoint&) = delete;
    P50ClientEndpoint& operator=(const P50ClientEndpoint&) = delete;

    // The deadline is an absolute steady-clock time.  Omitting it preserves
    // the historical unbounded behavior for callers that do not opt in.
    boost::asio::awaitable<ClientRunResult> run(boost::asio::ip::tcp::endpoint remote,
                                                PreparedTuHandle prepared = {},
                                                EndpointIoControl control = {},
                                                std::optional<std::chrono::steady_clock::time_point>
                                                    deadline = std::nullopt);

    // Run over a socket that is already connected by the ordinary listener
    // negotiation path.  Ownership is consumed on every success and failure
    // path; the endpoint validates that it is a connected IPv4/IPv6 TCP socket.
    boost::asio::awaitable<ClientRunResult> run(
        boost::asio::ip::tcp::socket socket, PreparedTuHandle prepared = {},
        EndpointIoControl control = {},
        std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

    boost::asio::awaitable<ClientRunResult> run_adopted_fd(
        int fd, PreparedTuHandle prepared = {}, EndpointIoControl control = {},
        std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

    // Persistent R2 link primitives. LINK_HELLO/LINK_STATE happen exactly
    // once per connection; the writer and reader are separate so later
    // windowed senders can overlap socket writes with receipt waits.
    boost::asio::awaitable<LinkState> open_r2_link(
        boost::asio::ip::tcp::socket& socket, LinkHello hello,
        std::chrono::steady_clock::time_point deadline);
    boost::asio::awaitable<R2RecoveryResult> recover_r2_link(
        boost::asio::ip::tcp::socket& socket, LinkHello hello,
        std::span<const R2SentBundle> witnesses, uint64_t verified_floor_a,
        Id128 operation_id, uint64_t new_relationship_epoch,
        HistoryNonce new_history_nonce,
        std::chrono::steady_clock::time_point deadline);
    boost::asio::awaitable<R2SentBundle> write_r2_bundle(
        boost::asio::ip::tcp::socket& socket, JobBind binding,
        PreparedTuHandle prepared,
        std::chrono::steady_clock::time_point deadline,
        EndpointIoControl control = {}, bool replay_attempt = false);
    boost::asio::awaitable<ClientRunResult> read_r2_receipt(
        boost::asio::ip::tcp::socket& socket, const R2SentBundle& sent,
        std::chrono::steady_clock::time_point deadline);
    boost::asio::awaitable<void> write_r2_ack(
        boost::asio::ip::tcp::socket& socket, uint64_t cumulative_ordinal,
        std::chrono::steady_clock::time_point deadline);
    boost::asio::awaitable<void> flush_r2_ack(
        boost::asio::ip::tcp::socket& socket,
        std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool r2_window_available() const noexcept;
    [[nodiscard]] uint64_t r2_confirmed_prefix() const noexcept;
    [[nodiscard]] uint64_t r2_ack_written_prefix() const noexcept;
    [[nodiscard]] std::vector<R2SentBundle> r2_pending_witnesses(
        uint64_t after_ordinal) const;

    static std::optional<boost::asio::ip::tcp::socket> adopt_connected_fd(
        boost::asio::any_io_executor executor, int fd,
        boost::system::error_code& error);

    // Cancel only the currently active C-role dialogue.  The caller must post
    // this method onto the endpoint's owner executor.  It never resets route
    // state or authorizes fallback: run() reports an observation and the
    // owning FSession operation performs any distributed settlement.
    EndpointCancelResult request_cancel(const EndpointCancelPermit& permit) noexcept;
    size_t cancel_all_for_incarnation(const SidecarLaunchIdentity& incarnation) noexcept;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    void request_cancel_for_test() noexcept;
#endif

    [[nodiscard]] CStoreGuid c_store_guid() const;
    [[nodiscard]] std::optional<FStoreGuid> f_store_guid() const;
    [[nodiscard]] bool has_active_transaction() const;
    [[nodiscard]] bool has_reconciliation_work() const;
    [[nodiscard]] RelSeq next_rel_seq() const;
    [[nodiscard]] Digest128 state_digest() const;

private:
    boost::asio::awaitable<ClientRunResult> run_connected(
        std::optional<boost::asio::ip::tcp::endpoint> remote,
        std::optional<boost::asio::ip::tcp::socket> socket,
        PreparedTuHandle prepared, EndpointIoControl control,
        std::optional<std::chrono::steady_clock::time_point> deadline);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class P50ServerEndpoint {
public:
    // Takes ownership of fd on every path.  Success returns one move-only,
    // CLOEXEC, connected TCP socket; failure closes fd and returns nullopt.
    static std::optional<boost::asio::ip::tcp::socket> adopt_connected_fd(
        boost::asio::any_io_executor executor, int fd,
        boost::system::error_code& error);

    explicit P50ServerEndpoint(FStoreGuid f_store_guid, EndpointCaps caps = {},
                               CompletionLog* completions = nullptr,
                               ActionTrace* actions = nullptr,
                               P50ServerEndpointConfig config = {});
    ~P50ServerEndpoint();
    P50ServerEndpoint(const P50ServerEndpoint&) = delete;
    P50ServerEndpoint& operator=(const P50ServerEndpoint&) = delete;

    boost::asio::awaitable<ServerRunResult> accept_one(boost::asio::ip::tcp::acceptor& acceptor,
                                                       EndpointIoControl control = {});

    // Consumes exactly one already-connected socket.  No accept/listen or read
    // occurs before ownership reaches the shared server transaction reducer.
    boost::asio::awaitable<ServerRunResult> run_adopted(
        boost::asio::ip::tcp::socket socket, EndpointIoControl control = {});

    // Consumes the complete post-P5CO authority bundle.  The retained socket
    // descriptor is endpoint-private and cannot be detached independently of
    // the exact adopted outcome and original absolute deadline.
    boost::asio::awaitable<ServerRunResult> run_adopted(
        sidecar::P5coEndpointHandoff handoff,
        EndpointIoControl control = {});

    // Explicit CacheWire R2 entry. The initial reservation is retained from
    // F's already-completed ARM/ARMED exchange; the session then remains live
    // across ordered JOB_BIND/TU bundles until CLOSE or transport loss.
    boost::asio::awaitable<ServerRunResult> run_adopted_r2(
        boost::asio::ip::tcp::socket socket,
        EndpointIoControl control = {});

    // Owner-affine terminal cleanup for one exact cancelled/expired R2 job.
    // Removes only that route's uncommitted Absent+crashed install tombstones;
    // committed residents and unrelated recovery rows are untouched.
    bool retire_p51_recovery_install(const JobBind& binding);
    bool retire_p51_recovery_install(Id128 reservation_id);
    // Retires only the quiescent codec-history row owned by an exact idle R2
    // relationship. The C namespace, committed input records, global resident
    // objects, and other profile histories remain in place.
    bool retire_idle_p51_route_history(CStoreGuid c_store_guid,
                                       ProfileId profile,
                                       Id128 relationship_id,
                                       uint64_t relationship_epoch) noexcept;

    // Cancels the active socket on the endpoint's owner executor. The caller
    // must arrange that affinity (SidecarRuntime posts this method); it never
    // changes listener or store ownership and is a no-op between dialogues.
    EndpointCancelResult request_cancel(const EndpointCancelPermit& permit) noexcept;
    size_t cancel_all_for_incarnation(const SidecarLaunchIdentity& incarnation) noexcept;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    void request_cancel_for_test() noexcept;
    [[nodiscard]] std::optional<std::string>
    global_resource_invariant_for_test() const;
#endif

    void reset_store(FStoreGuid new_guid);
    [[nodiscard]] InputCursor attach_input(InputRecordKey key) const;
    void close_input_job(InputRecordKey key);
    void collect_input_garbage();
    [[nodiscard]] FStoreGuid f_store_guid() const;
    [[nodiscard]] size_t namespace_count() const;
    [[nodiscard]] size_t revision_count() const;
    [[nodiscard]] size_t live_session_count() const;
    [[nodiscard]] P50ServerOwnerUsage owner_usage() const;
    [[nodiscard]] std::optional<InputRecordKey>
    last_committed_input(CStoreGuid c_store_guid) const;

private:
    class SessionRegistration;

    boost::asio::awaitable<ServerRunResult> run_connected(
        boost::asio::ip::tcp::socket socket, SessionRegistration registration,
        EndpointIoControl control, boost::asio::ip::tcp::acceptor* acceptor,
        std::optional<sidecar::AbsoluteMonotonicDeadline> deadline,
        std::optional<CStoreGuid> expected_c_store_guid);
    boost::asio::awaitable<ServerRunResult> run_r2_connected(
        boost::asio::ip::tcp::socket socket, SessionRegistration registration,
        EndpointIoControl control);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace icecc::p50
