#pragma once

#include "p50_actions.h"
#include "p50_input_record.h"
#include "p50_profile.h"
#include "p50_sidecar_identity.h"
#include "p50_zstd.h"
#include "p50_slice0.h"
#include "p50_endpoint_run_cancel.h"
#include "services/p50_store_identity_wire.h"

#include <utility>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace icecc::p50 {

class CRoute;
class GlobalResourceTrace;

/* A C-side P29V1 authority failed while reserving/initializing its permanent
   interner.  This exception is intentionally narrower than length_error:
   callers may disable only P29V1 for the current supervised READY lease, while
   ordinary per-TU size errors and transport failures remain retryable. */
class P29V1CapabilityUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    uint64_t session_serial = 0;
    HistoryNonce history_nonce{};
    RelSeq rel_seq{};
    TuSeq tu_seq{};
    Digest128 transaction_digest{};
    Digest128 raw_digest{};
    bool transaction_bound = false;
    auto operator<=>(const CompletionStamp&) const = default;
};

// The endpoint identity derived from current product state after an asynchronous
// operation completes.  It intentionally excludes the operation kind: that is
// frozen in CompletionStamp and checked on the observed-completion path.
struct CompletionLiveIdentity {
    ActorSide actor = ActorSide::C;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
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
    explicit CompletionLog(size_t max_records = std::numeric_limits<size_t>::max())
        : max_records_(max_records) {}

    void record(AsyncCompletion completion) noexcept {
        if (!valid_)
            return;
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
    void clear() {
        completions_.clear();
        valid_ = true;
    }

private:
    size_t max_records_ = std::numeric_limits<size_t>::max();
    bool valid_ = true;
    std::vector<AsyncCompletion> completions_;
};

// Coarse last-reached stage on a single client-side route transfer, tracked so
// that if the owner coroutine ever stops making progress and the blocking
// caller in P50CacheServiceImpl::transfer_source hits its wait_limit and
// retires the sidecar (see fail_stop()/std::_Exit(125) there), the
// ICECC_P50_OWNER_LAG_TRACE probe can report which step the stuck coroutine
// last completed. Written only via relaxed atomic stores from the owner
// executor; read (also relaxed) from the blocking caller's thread only in
// that already-fatal shutdown path, so a torn/stale read changes nothing
// about product behavior -- this is diagnostic-only, same as the rest of the
// owner-lag probe.
enum class ClientStallStage {
    Connecting,
    Armed,
    SessionReady,
    BeginWritten,
    StateRead,
    CommitRead,
};

// This control is a deterministic test seam around message boundaries and write
// fragmentation. Normal product callers use the default value.
struct EndpointIoControl {
    size_t max_write_fragment = std::numeric_limits<size_t>::max();
    // Diagnostic-only, non-null only when ICECC_P50_OWNER_LAG_TRACE is set.
    // See ClientStallStage above. Never read on any non-fail-stop path.
    std::atomic<ClientStallStage>* stall_stage = nullptr;
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
    // Simulator/test-only observation after one complete message has been
    // written successfully. It receives an immutable message copy and cannot
    // change product framing or endpoint state.
    std::function<void(ActorSide, const Message&)> outbound_message_observer;
    // Step 4 sub-step 4c (research/designs/P50_ROUTE_PIPELINING.md): fires
    // exactly once per run_connected() dialogue, immediately after this TX's
    // last FILL byte (or, for non-P29V1 profiles, its BODY) has been written
    // to the wire and before this endpoint starts waiting on a peer reply it
    // can no longer influence by writing more. Null by default: every
    // existing caller is unaffected.
    // Step 4d contract: this fires from INSIDE the endpoint's own coroutine
    // on the owner executor. A callback must never block, suspend, or run
    // another endpoint operation inline (a prepare, a run, a future wait):
    // production only RELEASES the route transfer lock here (the next
    // transfer then runs in its own coroutine), and anything heavier can
    // self-deadlock the single-threaded owner.
    std::function<void()> write_complete;
};

struct PrepareRequestKey {
    uint64_t producer_session = 0;
    uint64_t request_token = 0;
    auto operator<=>(const PrepareRequestKey&) const = default;
};

// C-wide preparation is shared, but route-profile planning is relationship
// scoped.  The F generation fences a replacement route from its predecessor.
// c_store_guid is the base C-store GUID for the whole relationship unless
// ROUTE_LANES (ICECC_P50_ROUTE_LANES) is enabled, in which case it carries a
// lane-derived GUID so each lane gets its own "one uncommitted successor"
// slot inside P50PreparationAuthority.  Lane 0 always reuses the base GUID,
// so a default (K=1) build's key is byte-identical to a build with no lane
// field at all once the base GUID is in the same position.
struct PreparationRouteKey {
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    uint64_t f_store_generation = 0;
    ProfileId profile = ProfileId::ZSTD_TU;
    auto operator<=>(const PreparationRouteKey&) const = default;
};

struct PreparationAuthorityLimits {
    size_t max_live_entries = 4096;
    uint64_t max_retained_encoded_bytes = uint64_t{512} << 20;
    // The interner reserves sidecar() (10 GiB, mostly untouched arenas) if
    // it fits under this, else probe(); its hash tables grow into the rest.
    uint64_t max_interner_reserved_bytes = uint64_t{24} << 30;
    uint64_t max_route_state_bytes = uint64_t{1} << 30;
    // How many successive TUs on one route may have an uncommitted
    // successor prepared at once (research/designs/P50_ROUTE_PIPELINING.md,
    // "Step 4"). 1 reproduces every pre-pipelining call site byte-for-byte:
    // a route may not begin TU n+1 until TU n commits. The production value
    // is sourced once at sidecar construction from
    // SidecarRuntime::RouteGate::route_pipeline_depth(), which itself stays
    // clamped to 1 outside this step's own tests until the F-side sequencer
    // (design doc Step 5) exists.
    uint32_t route_pipeline_depth = 1;
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
    // The route-independent half of a P29V1 prepare_for_route (TU_SEQ and
    // interning).  Unlike every other member it may run on any thread, so a
    // route's serialized dialogue on the owner need not wait for it.  The
    // owner installs the result; prepare_for_route then reuses it for the
    // same request, and release_source drops it if no route admitted it.
    class PreparedSource;
    // Optional per-call timings for the stage trace (all zero when the
    // caller does not ask): mutex_wait_ns is the time blocked acquiring the
    // C-wide source mutex (both acquisitions), mutex_hold_ns the time it
    // was HELD (reserve+intern+commit, the serialized term), plan_ns the
    // lock-free planning walk.  steady_clock deltas; nanoseconds.
    struct PrepareSourceTiming {
        uint64_t mutex_wait_ns = 0;
        uint64_t mutex_hold_ns = 0;
        uint64_t plan_ns = 0;
    };
    std::shared_ptr<const PreparedSource> prepare_source(
        PrepareRequestKey request,
        std::shared_ptr<const std::vector<uint8_t>> exact_input,
        Digest128 raw_digest,
        PrepareSourceTiming* timing = nullptr);
    // The same preparation over a read-only view of the input (the
    // sidecar's mapped source read, cache/p50_source_view.h).
    // keepalive owns exact_input's bytes for the whole request lifetime
    // (the vector overload above delegates here with itself as the
    // keepalive); every internal reader of the raw bytes uses the span, so
    // a mapped and a copied input are indistinguishable.
    std::shared_ptr<const PreparedSource> prepare_source(
        PrepareRequestKey request,
        std::span<const uint8_t> exact_input,
        std::shared_ptr<const void> keepalive,
        Digest128 raw_digest,
        PrepareSourceTiming* timing = nullptr);
    void install_source(PrepareRequestKey request,
                        std::shared_ptr<const PreparedSource> prepared);
    void release_source(PrepareRequestKey request) noexcept;
    std::span<const uint8_t> answer_p29v1_need(
        PreparedTuHandle handle, std::span<const uint8_t> inner_need);
    // FILL for a pipelined TX_BEGIN, sent before NEED; nullopt when NEED could
    // outgrow a socket buffer.  confirm_p29v1_need() must check NEED later.
    std::optional<std::span<const uint8_t>> fill_p29v1_before_need(
        PreparedTuHandle handle);
    // Read-only probe of the same bound: true exactly when
    // fill_p29v1_before_need() would return a FILL, without building one.
    // Route pipelining, P50_ROUTE_PIPELINING.md section 1.1, 2026-10-08
    // errata: the endpoint dispatch uses this to keep a would-be successor
    // that must answer NEED interactively on the head path instead.
    [[nodiscard]] bool p29v1_fill_before_need_possible(PreparedTuHandle handle);
    void confirm_p29v1_need(PreparedTuHandle handle,
                            std::span<const uint8_t> inner_need);
    // Step 4d core (research/designs/P50_ROUTE_PIPELINING.md section 1.1,
    // 2026-10-08 errata): run this handle's P29V1 serializer commit locally,
    // ahead of F's TX_COMMIT ack, recording its predicted post-state digest
    // and freeing the route's serializer for the next successor's begin.
    // Only valid at route_pipeline_depth > 1; nothing calls it at the
    // default depth of 1, where the legacy confirm-only commit inside
    // commit() stays byte-identical.
    void commit_predicted_p29v1(PreparedTuHandle handle);
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
    // Release-isolated query (k415 item 5): true when release(handle) would
    // abandon NO other outstanding successor -- the entry is already absent
    // from its route's uncommitted deque (a uniform abandonment from another
    // member's path consumed it) or it is that deque's sole member.  A
    // zero-wire attempt-2 retirement may release route-locally ONLY in that
    // shape: with a live sibling outstanding, release() clears the deque
    // and marks every sibling chain_abandoned, so the sibling's own healthy
    // TX_COMMIT accept ("is not its prepared successor") escapes its run to
    // the sender catch-all and one dead transfer becomes a sticky whole-C
    // replacement -- and NOT releasing is no better, because an
    // uncommittable junior left in the FIFO blocks every later commit the
    // same way.  The isolated shapes are the only ones where the route stays
    // healthy through the release.
    [[nodiscard]] bool prepared_route_release_isolated(
        PreparedTuHandle handle) const;
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
    // Route pipelining (research/designs/P50_ROUTE_PIPELINING.md "Step 4"):
    // the per-route outstanding-successor bound this authority admits.
    // 1 is the byte-identical default.
    [[nodiscard]] uint32_t route_pipeline_depth() const;

    // Route owners call this only after all handles for the relationship have
    // been released.  It drops matcher/history state without touching the
    // C-wide immutable catalogue or TU allocator.
    [[nodiscard]] bool reset_route(PreparationRouteKey route) noexcept;

    // Force variant for a route whose F incarnation is provably dead: used
    // only when the caller has independently proven (via f_store_generation)
    // that this route's F incarnation can never be observed again, so any
    // still-live Entry here was orphaned by a sender that already reached a
    // terminal transfer outcome without releasing its handle. Unconditionally
    // erases every live Entry for this route plus the route state itself,
    // bypassing the live-reference check reset_route() performs. Callers
    // must not call this while that sender's transfer is still in flight; see
    // P50CRouteOwner::reset_f_store_exact and P50ZstdSourceSender::
    // transfer_in_flight for the full argument.
    [[nodiscard]] bool discard_route(PreparationRouteKey route) noexcept;

    // Step 4d (research/designs/P50_ROUTE_PIPELINING.md): a pipelined
    // successor's own uncommitted P29V1 route state must be retired when the
    // client endpoint observes its predecessor deviated from the predicted
    // witness, before the handle itself is released. Unlike reset_route(),
    // this acts on one still-live handle, not a whole relationship; the
    // caller must not use `handle` again afterward. Throws
    // std::invalid_argument if `handle` does not belong to this authority,
    // has already been released, or is not an active P29V1 successor; throws
    // std::logic_error if `handle` is not the route's identified successor.
    void abandon_route(PreparedTuHandle handle);

    // True when a uniform chain invalidation (abandon_route, release's
    // non-tail path) removed this entry from its route's outstanding
    // successor deque while the entry itself is still alive -- i.e. the
    // route encoding this handle was prepared under no longer exists.
    // The C sender must not retry a transfer on such a handle: the fresh
    // route re-encodes from its own cursor, and a retry dispatched on the
    // stale admitted rel/pre-state fails the F witness (K>2 farm legs,
    // 2026-10-09: te=3 "STATE differs" / "TX_COMMIT does not close").
    // Total: false for unknown or released handles, never throws.
    [[nodiscard]] bool prepared_chain_abandoned(PreparedTuHandle handle) const;

private:
    PreparedInputPtr resolve(PreparedTuHandle handle) const;
    void validate_begin(const TxBegin& begin) const;
    CRoute& p29v1_successor(PreparedTuHandle handle);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class P50ClientEndpoint;
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
    // True the moment this run's TX_BEGIN write phase started (HELLO and any
    // later frame of the dialogue were excluded): at least one transactional
    // byte of THIS transaction was heading to F, so any subsequent ambiguity
    // must reconcile route state.  False for a run that died before that
    // point -- a refused connect/arm, a HELLO that never got a reply -- where
    // nothing about the route's F-side state can have changed and the
    // sender's retry branches may retire the transfer ROUTE-LOCALLY instead
    // of quarantining the relationship (the k45 farm leg's whole-route
    // 0x5002 latch: one f-arm refusal cost 471 refusals).
    bool wrote_begin = false;
    // Diagnostic-only per-run timing (steady_clock durations, ns). Populated
    // only along the paths named in each field's comment at the call site;
    // zero otherwise. These never gate behavior and exist solely for
    // ICECC_P50_SOURCE_RESULT_TRACE.
    uint64_t c_fill_ns = 0;
    uint64_t c_write_ns = 0;
    uint64_t c_state_wait_ns = 0;
    uint64_t c_state_to_commit_ns = 0;
    uint64_t c_post_commit_ns = 0;
    // Diagnostic-only absolute CLOCK_REALTIME wall-clock samples, taken at
    // the same two points as trace_write_phase_start/trace_commit_read_done
    // above (BEGIN write-phase start, TX_COMMIT frame read). Used offline to
    // derive C/F clock offsets via the classic four-timestamp NTP algorithm
    // against ServerRunResult::f_begin_recv_wall_ns/f_wall_clock_ns. Never
    // sent over the wire; populated only for ICECC_P50_SOURCE_RESULT_TRACE.
    uint64_t c_begin_write_wall_ns = 0;
    uint64_t c_commit_read_wall_ns = 0;
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
    // Diagnostic-only per-run timing (steady_clock durations, ns, plus one
    // CLOCK_REALTIME wall-clock sample) for ICECC_P50_F_RESULT_TRACE.
    // Populated only along the ordinary Completed path; zero otherwise.
    // These never gate behavior.
    RelSeq f_rel_seq{};
    uint64_t f_raw_bytes = 0;
    uint64_t f_hello_ns = 0;
    uint64_t f_hello_to_begin_ns = 0;
    uint64_t f_begin_to_body_complete_ns = 0;
    uint64_t f_commit_ns = 0;
    uint64_t f_commit_write_ns = 0;
    uint64_t f_wait_peer_close_ns = 0;
    uint64_t f_run_total_ns = 0;
    uint64_t f_begin_to_commit_written_ns = 0;
    uint64_t f_wall_clock_ns = 0;
    // Diagnostic-only absolute CLOCK_REALTIME wall-clock sample taken
    // immediately after the BEGIN frame read completes, before it is
    // decoded as TxBegin -- i.e. F's receipt time for the BEGIN bytes,
    // excluding any prepare/activate/install work that follows. Taking the
    // sample later (after that work) would bias the derived offset by
    // roughly +work/2; see tools/stagetrace/analyze.py. Paired with
    // f_wall_clock_ns (taken at TX_COMMIT write) and ClientRunResult's
    // c_begin_write_wall_ns/c_commit_read_wall_ns to derive C/F clock offsets
    // via the classic four-timestamp NTP algorithm. Never sent over the
    // wire; populated only for ICECC_P50_F_RESULT_TRACE.
    uint64_t f_begin_recv_wall_ns = 0;
    // Diagnostic-only. Set by step 5b's MSG_PEEK (taken immediately before
    // stage()/the SESSION_STATE write; see run_connected) when a TX_BEGIN
    // was already sitting in the socket's receive buffer at that point --
    // e.g. a client using enable_pipelined_begin(), or, in the future, any
    // outstanding leg of a K>1 pipelined route. Never gates behavior and
    // never travels over the wire; step 5c's per-route sequencer is what
    // turns this observation into an admission decision.
    std::optional<RelSeq> f_hello_time_peeked_begin_rel_seq;
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
    size_t retained_input_records = 0;
    uint64_t retained_input_bytes = 0;
    auto operator<=>(const P50ServerOwnerUsage&) const = default;
};

struct P50ServerEndpointConfig {
    uint16_t protocol_error_code = 1;
    P50ServerOwnerLimits owner_limits{};
    InputJobStateSelector input_job_state;
    std::optional<SidecarLaunchIdentity> sidecar_launch;
    uint64_t endpoint_generation = 1;
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
    // Step 5c pre-park grace bound (2026-10-09 admission-race fixes), in
    // milliseconds. Production keeps the 2000 ms default: a new session
    // arriving while another session's TX is in flight on the route waits
    // up to this bound for that TX to resolve (or for the new session's
    // first frame) before falling through to today's stage()/activate()
    // semantics. Test-only override so the grace path is reachable
    // deterministically (a shorter bound does not change which frames are
    // sent, only when the fall-through happens).
    uint32_t prepark_grace_bound_ms = 2000;
    // 2026-10-10 F-side retained route-history bound, in milliseconds of
    // namespace idleness. A C store that goes away leaves its namespace's
    // persistent P29V1 dialogue behind: that dialogue owns the route codec
    // whose P29ReceiverRouteState (cache/codec/p29_wire.h) appends one
    // region segment, one occurrence-stream run, and path/region index
    // entries per committed TU for the whole route life, and nothing else
    // bounds it -- namespace LRU eviction fires only under max_namespaces
    // or InputRecordStore byte pressure, and both require job-lease closure
    // this endpoint cannot force for a store that is gone.  After this much
    // continuous idleness (no live session, no active or interrupted
    // transaction, no parked waiter) collect_input_garbage() drops the
    // idle namespaces' codec history through the same
    // invalidate_p29v1_codec() path input-payload eviction already uses:
    // the authenticated Protocol-50 cursor and every retained InputRecord
    // survive untouched, and a reconnecting C of the same store reconciles
    // through HISTORY_RESET exactly as after an input eviction.  Zero
    // disables the sweep and keeps today's retention byte-for-byte.
    uint32_t route_history_idle_grace_ms = 120000;
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

    // For callers that only ever reach one F incarnation: with an exact
    // retained P29V1 route, TX_BEGIN and BODY follow HELLO without waiting
    // for STATE, which is checked afterwards.
    void enable_pipelined_begin() noexcept;

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
    // Step 4d core test hooks (P50_ROUTE_PIPELINING.md section 5.1): the
    // outstanding-successor window and both waiter structures must stay
    // empty at depth 1 (T6b's K=1 no-op assertion) and be observable from
    // the endpoint tests while a K>1 window is open.
    [[nodiscard]] size_t pipeline_window_size_for_test() const;
    [[nodiscard]] size_t pipeline_retire_waiter_count_for_test() const;
    [[nodiscard]] size_t pipeline_window_slot_waiter_count_for_test() const;
    [[nodiscard]] uint64_t pipeline_epoch_for_test() const;
#endif

    [[nodiscard]] CStoreGuid c_store_guid() const;
    [[nodiscard]] std::optional<FStoreGuid> f_store_guid() const;
    [[nodiscard]] bool has_active_transaction() const;
    [[nodiscard]] bool has_reconciliation_work() const;
    [[nodiscard]] RelSeq next_rel_seq() const;
    [[nodiscard]] Digest128 state_digest() const;

    // Step 4d (research/designs/P50_ROUTE_PIPELINING.md): true from the
    // moment a pipelined successor observes its predecessor deviate from the
    // predicted witness until reset_after_pipeline_poison() clears it. The
    // predicted-cursor/pipeline-member bookkeeping that is meant to set this
    // flag is the remaining Step 4d/5 work and does not exist yet, so this
    // is always false today; the accessor and its paired reset below are
    // being landed first as the cross-layer bridge the sender side already
    // calls.
    [[nodiscard]] bool has_pipeline_poison() const noexcept;

    // Cold local reset for the route this endpoint owns, performed by the
    // caller while the poisoning transfer still counts as in flight. Leaves
    // f_guid untouched so the next head dialogue's same_f check stays true
    // and takes the existing, unmodified RouteHistoryReset wire branch.
    // Throws std::logic_error if called without an outstanding pipeline
    // poison.
    void reset_after_pipeline_poison();

    // Local abandon of a route-local zero-wire retirement whose prepared
    // handle is NOT release-isolated (a live sibling is outstanding on the
    // same route).  Thin wrapper over Impl::poison_pipeline(): closes every
    // outstanding successor socket and wakes both waiter populations so the
    // sibling coroutines observe their own teardown -- the exact same first
    // half the mid-write has_pipeline_poison() flow performs before
    // authority abandon_route + reset_after_pipeline_poison().  The caller
    // MUST follow with abandon_route(prepared) and
    // reset_after_pipeline_poison(); neither release() nor
    // discard_retained_route_work() alone can free this shape's slot.
    void poison_pipeline_for_local_abandon();

    // Discards this endpoint's retained retry bookkeeping (the active
    // transaction or the queued successor) when it references exactly this
    // prepared handle -- the sender's route-local retirement of a transfer
    // whose attempts never wrote a TX_BEGIN (refused connect/arm), so no
    // route reconciliation can resolve it later and the retained reference
    // would strand the route's admission slot (the next distinct request's
    // dispatch would throw "already has different queued work" and latch a
    // sticky whole-C replacement). Cold route fields (history_nonce,
    // next_rel, state) are NOT touched: nothing reached F, so the retained
    // cursor is still exactly what F holds. Throws std::invalid_argument if
    // the handle belongs to another authority and std::logic_error if this
    // endpoint retains no bookkeeping for it.
    void discard_retained_route_work(PreparedTuHandle handle);

    // Step 4d core (research/designs/P50_ROUTE_PIPELINING.md section 1.1,
    // 2026-10-08 errata): suspend this coroutine until the endpoint's
    // outstanding-dialogue window has a free slot (fewer than
    // route_pipeline_depth dialogues: head plus successors). Level
    // triggered; woken by accept() retirement and by poison/epoch changes.
    // Bounded by deadline: a timeout resolves to false and the caller takes
    // its ordinary route-local deadline failure. At the default depth of 1
    // this returns true immediately without suspending, so the sender's
    // pre-prepare wait is invisible to every existing caller.
    boost::asio::awaitable<bool> wait_for_window_slot(
        std::optional<std::chrono::steady_clock::time_point> deadline);

    // C2 (2026-10-09 admission-race fix): level-triggered wait for the
    // window to be completely EMPTY (!active && successors.empty() &&
    // active_session == 0) -- the T7 (no eager FILL) fallback waits on
    // this before taking the unchanged head path directly. Same deadline,
    // poison, and window_slot_waiters wake mechanics as
    // wait_for_window_slot; unreachable at depth 1.
    boost::asio::awaitable<bool> wait_for_empty_window(
        std::optional<std::chrono::steady_clock::time_point> deadline);

private:
    boost::asio::awaitable<ClientRunResult> run_connected(
        std::optional<boost::asio::ip::tcp::endpoint> remote,
        std::optional<boost::asio::ip::tcp::socket> socket,
        PreparedTuHandle prepared, EndpointIoControl control,
        std::optional<std::chrono::steady_clock::time_point> deadline);

    // Step 4d core: one successor dialogue at depth > 1. Never touches the
    // confirmed cursor fields until promoted; opens its own connection;
    // predict-commits its own serializer right after its eager FILL write
    // (symmetric with the head's Site A) so the next successor can begin;
    // waits level-triggered for its predecessor to retire before reading
    // its own COMMIT through the shared accept() path.
    boost::asio::awaitable<ClientRunResult> run_connected_successor(
        std::optional<boost::asio::ip::tcp::endpoint> remote,
        std::optional<boost::asio::ip::tcp::socket> adopted,
        PreparedTuHandle prepared, PreparedInputPtr admitted,
        EndpointIoControl control,
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
        EndpointIoControl control, boost::asio::ip::tcp::acceptor* acceptor);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace icecc::p50
