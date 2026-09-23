#pragma once

// Production ownership for one retained Protocol-50 compiler input.
//
// Cache identity remains exactly (C_STORE_GUID, TU_SEQ).  The fields below
// are daemon/job observations used to ensure that cancellation, replacement,
// and terminal job closure mutate only their exact logical owner.

#include "p50_input_record.h"
#include "p50_local_transport.h"
#include "p50_sidecar_identity.h"

#include <compare>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace icecc::p50 {

struct InputLeaseOwner {
    uint64_t logical_job = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;
    auto operator<=>(const InputLeaseOwner&) const = default;
};

[[nodiscard]] inline bool input_lease_owner_valid(
    InputLeaseOwner owner) noexcept {
    return owner.logical_job != 0 && owner.assignment_epoch != 0 &&
           owner.assignment_nonce != 0;
}

enum class InputLifecycleAction : uint16_t {
    None = 0,
    CancelAttempt = 1,
    CloseAcceptedJob = 2,
    CancelJob = 3,
    // Attempt-scoped replacement is deliberately distinct from logical-job
    // close.  PREPARE quiesces the old sidecar attempt while retaining the
    // canonical InputRecord; COMMIT is admitted only after the caller has
    // supplied a complete local retirement proof.
    PrepareAttemptRetirement = 4,
    CommitAttemptReplacement = 5,
    CloseLogicalInputLease = 6,
    // Wire-spelling aliases used by the protocol census and external reducer.
    PREPARE_ATTEMPT_RETIREMENT = PrepareAttemptRetirement,
    COMMIT_ATTEMPT_REPLACEMENT = CommitAttemptReplacement,
    CLOSE_LOGICAL_INPUT_LEASE = CloseLogicalInputLease,
};

[[nodiscard]] inline bool input_lifecycle_action_valid(
    InputLifecycleAction action) noexcept {
    return action == InputLifecycleAction::CancelAttempt ||
           action == InputLifecycleAction::CloseAcceptedJob ||
           action == InputLifecycleAction::CancelJob ||
           action == InputLifecycleAction::PrepareAttemptRetirement ||
           action == InputLifecycleAction::CommitAttemptReplacement ||
           action == InputLifecycleAction::CloseLogicalInputLease;
}

struct InputLifecycleRequest {
    local::Identity identity{};
    InputRecordKey key{};
    InputLeaseOwner owner{};
    uint64_t operation_id = 0;
    InputLifecycleAction action = InputLifecycleAction::None;
    // Extended attempt-retirement binding.  These fields are zero for the
    // original CancelAttempt/CloseAcceptedJob/CancelJob envelope, preserving
    // its 88-byte compatibility wire.  Extended actions require every field
    // below (and COMMIT additionally requires replacement_owner).
    uint64_t f_store_generation = 0;
    FStoreGuid f_store_guid{};
    uint64_t immutable_size = 0;
    Digest128 immutable_digest{};
    uint64_t retirement_id = 0;
    std::optional<InputLeaseOwner> replacement_owner;
    // Local scheduler metadata; it is never encoded in the control wire.  A
    // queued outer-loop request retains the caller's original absolute
    // deadline instead of silently restarting a timeout on each retry.
    std::chrono::steady_clock::time_point deadline{};
    // Extended operations additionally carry the original CLOCK_MONOTONIC
    // value and both clock identities.  The steady timepoint above remains a
    // local compatibility representation; it must be an exact conversion of
    // this value whenever absolute_deadline is valid.
    sidecar::AbsoluteMonotonicDeadline absolute_deadline{};
    auto operator<=>(const InputLifecycleRequest&) const = default;
};

// Owner-affine handle for one outer-loop lifecycle dialogue.  This is an
// identity/cancellation lease, not an input-record owner or an endpoint
// materializer: the sidecar remains the sole owner of record bytes.  Every
// field which can distinguish a queued operation from a later reused role
// slot participates in the match, including the unchanged absolute deadline
// and allocator-bound F-store tuple.  A stale timer/completion can therefore
// only fail closed instead of mutating a replacement attempt.
struct InputLifecycleOperationLease {
    local::Identity identity{};
    InputRecordKey key{};
    InputLeaseOwner owner{};
    InputLifecycleAction action = InputLifecycleAction::None;
    uint64_t operation_id = 0;
    uint64_t f_store_generation = 0;
    FStoreGuid f_store_guid{};
    uint64_t immutable_size = 0;
    Digest128 immutable_digest{};
    uint64_t retirement_id = 0;
    std::optional<InputLeaseOwner> replacement_owner;
    sidecar::AbsoluteMonotonicDeadline absolute_deadline{};

    [[nodiscard]] bool valid() const noexcept {
        return identity.generation != 0 && identity.attempt != 0 &&
               key.c_store_guid != CStoreGuid{} &&
               input_lease_owner_valid(owner) && operation_id != 0 &&
               input_lifecycle_action_valid(action) &&
               ((action == InputLifecycleAction::PrepareAttemptRetirement ||
                 action == InputLifecycleAction::CommitAttemptReplacement ||
                 action == InputLifecycleAction::CloseLogicalInputLease)
                    ? (f_store_generation != 0 && f_store_guid != FStoreGuid{} &&
                       immutable_digest != Digest128{} && retirement_id != 0 &&
                       absolute_deadline.valid() &&
                       (action == InputLifecycleAction::CommitAttemptReplacement
                            ? replacement_owner.has_value() &&
                                  input_lease_owner_valid(*replacement_owner) &&
                                  replacement_owner->logical_job == owner.logical_job &&
                                  *replacement_owner != owner
                            : !replacement_owner.has_value()))
                    : f_store_generation == 0 && f_store_guid == FStoreGuid{} &&
                       immutable_size == 0 && immutable_digest == Digest128{} &&
                       retirement_id == 0 && !replacement_owner.has_value() &&
                       absolute_deadline == sidecar::AbsoluteMonotonicDeadline{});
    }

    [[nodiscard]] bool matches(const InputLifecycleRequest& request) const noexcept {
        const bool deadline_matches = absolute_deadline.valid()
                                           ? request.absolute_deadline == absolute_deadline &&
                                                 request.deadline ==
                                                     absolute_deadline.as_steady_time_point()
                                           : request.absolute_deadline ==
                                                 sidecar::AbsoluteMonotonicDeadline{};
        return valid() && request.identity == identity && request.key == key &&
               request.owner == owner && request.action == action &&
               request.operation_id == operation_id &&
               request.f_store_generation == f_store_generation &&
               request.f_store_guid == f_store_guid &&
               request.immutable_size == immutable_size &&
               request.immutable_digest == immutable_digest &&
               request.retirement_id == retirement_id &&
               request.replacement_owner == replacement_owner &&
               deadline_matches;
    }

    friend bool operator==(const InputLifecycleOperationLease&,
                           const InputLifecycleOperationLease&) = default;
};

[[nodiscard]] inline InputLifecycleOperationLease
make_input_lifecycle_operation_lease(
    const InputLifecycleRequest& request) noexcept {
    return InputLifecycleOperationLease{
        request.identity,
        request.key,
        request.owner,
        request.action,
        request.operation_id,
        request.f_store_generation,
        request.f_store_guid,
        request.immutable_size,
        request.immutable_digest,
        request.retirement_id,
        request.replacement_owner,
        request.absolute_deadline};
}

// Narrow refinement seam for an F-owner decoder/commit reducer.  This ledger
// carries operation identity only; it does not own InputRecord bytes or
// instantiate a second endpoint/store owner.  ObservePrepared() mints an
// owner-local observation id, CancelOrExpire() is the cancel-first
// profile-reset/no-durability branch, and SelectCommit() mints a move-only
// one-time permit for the commit-first branch.
enum class InputLifecycleRefinementState : uint8_t {
    None = 0,
    Prepared,
    CancelledNoDurability,
    CommitSelected,
    DurableCommitted,
    DeliverySuppressed,
};

enum class InputLifecycleRefinementResult : uint8_t {
    InvalidArgument = 0,
    Prepared,
    AlreadyPrepared,
    CancelledNoDurability,
    AlreadyCancelled,
    CommitWon,
    CommitSelected,
    DurableCommitted,
    AlreadyDurable,
    DeliverySuppressed,
    StaleOperation,
    ConflictingOperation,
    CapacityExceeded,
};

// Immutable owner observation returned by ObservePrepared(op, prepared_id).
// `decoder_touched` is deliberately true: once this boundary is crossed,
// cancel-first must reset the transient profile and may not persist a durable
// result.  Repeating the exact call returns the same observation id.
struct InputLifecyclePreparedObservation {
    InputLifecycleOperationLease operation{};
    uint64_t prepared_id = 0;
    uint64_t observation_id = 0;
    bool decoder_touched = false;

    [[nodiscard]] bool valid() const noexcept {
        return operation.valid() &&
               operation.action == InputLifecycleAction::PrepareAttemptRetirement &&
               prepared_id != 0 && observation_id != 0 && decoder_touched;
    }
    [[nodiscard]] bool matches(
        const InputLifecycleOperationLease& candidate,
        uint64_t candidate_prepared_id) const noexcept {
        return valid() && operation == candidate &&
               prepared_id == candidate_prepared_id;
    }
    friend bool operator==(const InputLifecyclePreparedObservation&,
                           const InputLifecyclePreparedObservation&) = default;
};

// A commit permit is linearized by SelectCommit and consumed exactly once by
// CommitDurable.  It is move-only so a caller cannot duplicate the positive
// commit authority into two reducer branches.
class InputLifecycleCommitPermit {
public:
    InputLifecycleCommitPermit() = default;
    InputLifecycleCommitPermit(const InputLifecycleCommitPermit&) = delete;
    InputLifecycleCommitPermit& operator=(const InputLifecycleCommitPermit&) = delete;
    InputLifecycleCommitPermit(InputLifecycleCommitPermit&& other) noexcept
        : operation_(std::move(other.operation_)),
          observation_id_(other.observation_id_), permit_id_(other.permit_id_),
          valid_(other.valid_) {
        other.operation_ = {};
        other.observation_id_ = 0;
        other.permit_id_ = 0;
        other.valid_ = false;
    }
    InputLifecycleCommitPermit& operator=(InputLifecycleCommitPermit&& other) noexcept {
        if (this != &other) {
            operation_ = std::move(other.operation_);
            observation_id_ = other.observation_id_;
            permit_id_ = other.permit_id_;
            valid_ = other.valid_;
            other.operation_ = {};
            other.observation_id_ = 0;
            other.permit_id_ = 0;
            other.valid_ = false;
        }
        return *this;
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const InputLifecycleOperationLease& operation() const noexcept {
        return operation_;
    }
    [[nodiscard]] uint64_t observation_id() const noexcept { return observation_id_; }
    [[nodiscard]] uint64_t permit_id() const noexcept { return permit_id_; }

private:
    InputLifecycleCommitPermit(InputLifecycleOperationLease operation,
                               uint64_t observation_id,
                               uint64_t permit_id) noexcept
        : operation_(std::move(operation)), observation_id_(observation_id),
          permit_id_(permit_id), valid_(true) {}
    void consume() noexcept {
        operation_ = {};
        observation_id_ = 0;
        permit_id_ = 0;
        valid_ = false;
    }

    InputLifecycleOperationLease operation_{};
    uint64_t observation_id_ = 0;
    uint64_t permit_id_ = 0;
    bool valid_ = false;
    friend class InputLifecycleRegistry;
};

// Metadata bundle supplied by the same F owner after the exact permit was
// selected.  The digest binds the already-owned durable transcript without
// moving/copying InputRecord bytes through this seam.  CommitDurable stores the
// bundle atomically with the state transition; delivery suppression is a
// separate exact operation and never rolls the durable bundle back.
struct InputLifecycleDurableBundle {
    InputLifecycleOperationLease operation{};
    uint64_t prepared_id = 0;
    uint64_t observation_id = 0;
    uint64_t permit_id = 0;
    uint64_t durable_sequence = 0;
    Digest128 durable_digest{};

    [[nodiscard]] bool valid() const noexcept {
        return operation.valid() &&
               operation.action == InputLifecycleAction::PrepareAttemptRetirement &&
               prepared_id != 0 && observation_id != 0 && permit_id != 0 &&
               durable_sequence != 0 && durable_digest != Digest128{};
    }
    [[nodiscard]] bool matches(const InputLifecycleCommitPermit& permit) const noexcept {
        return valid() && permit.valid() && operation == permit.operation() &&
               observation_id == permit.observation_id() &&
               permit_id == permit.permit_id();
    }
    friend bool operator==(const InputLifecycleDurableBundle&,
                           const InputLifecycleDurableBundle&) = default;
};

struct RemoteInputLeaseBinding {
    local::Identity identity{};
    uint64_t f_store_generation = 0;
    FStoreGuid f_store_guid{};
    InputRecordKey key{};
    InputLeaseOwner owner{};
    uint64_t operation_id = 0;
    uint64_t retirement_id = 0;
    uint64_t immutable_size = 0;
    Digest128 immutable_digest{};

    auto operator<=>(const RemoteInputLeaseBinding&) const = default;

    [[nodiscard]] bool valid() const noexcept {
        return identity.generation != 0 && identity.attempt != 0 &&
               f_store_generation != 0 && f_store_guid != FStoreGuid{} &&
               key.c_store_guid != CStoreGuid{} && input_lease_owner_valid(owner) &&
               operation_id != 0 && retirement_id != 0 &&
               immutable_digest != Digest128{};
    }
};

enum class InputLifecycleApplyStatus : uint8_t {
    Applied = 0,
    AlreadyApplied,
    InvalidArgument,
    UnknownRecord,
    StaleOwner,
    ConflictingReplay,
    CapacityExceeded,
    AttemptQuiescedRecordRetained,
    ReplacementInstalledRecordRetained,
    JobClosedRecordRetained,
    JobClosedRecordReclaimed,
    RetirementProofRequired,
    GenerationMismatch,
};

struct InputLifecycleApplyResult {
    InputLifecycleApplyStatus status = InputLifecycleApplyStatus::InvalidArgument;
    bool close_record = false;
    bool collect_record = false;
};

enum class InputLifecycleCommitDecision : uint8_t {
    Open = 0,
    Closed,
    CapacityExceeded,
};

// Serialized beside P50ServerEndpoint on its one owner executor.  This table
// does not own exact input bytes: it owns the bounded logical-job/attempt
// observation needed to decide whether the endpoint's InputRecordStore may be
// closed.  A committed record can exist before its CompileFile owner arrives;
// first attachment binds that record to one exact logical owner.
class InputLifecycleRegistry {
public:
    InputLifecycleRegistry(size_t max_owners, size_t max_replays);

    // The endpoint's single allocator-bound FInputStoreOwner installs this
    // identity once at construction.  A zero identity is retained for the
    // historical registry unit fixtures and rejects all v4 retirement work.
    void bind_store_identity(uint64_t f_store_generation,
                             FStoreGuid f_store_guid) noexcept;

    // Refinement-compatible F-owner interface.  These methods are
    // owner-affine and side-effect only this bounded identity ledger; the
    // endpoint's one FInputStoreOwner remains the authority for bytes and
    // decoder state.  Exact operation identity, observation id, and one-time
    // permit fencing make stale timer/completion/cancel events fail closed.
    [[nodiscard]] std::optional<InputLifecyclePreparedObservation>
    ObservePrepared(const InputLifecycleOperationLease& operation,
                    uint64_t prepared_id) noexcept;
    [[nodiscard]] InputLifecycleRefinementResult
    CancelOrExpire(const InputLifecycleOperationLease& operation,
                  uint64_t observation_id) noexcept;
    [[nodiscard]] std::optional<InputLifecycleCommitPermit>
    SelectCommit(const InputLifecycleOperationLease& operation,
                 uint64_t observation_id) noexcept;
    [[nodiscard]] InputLifecycleRefinementResult
    CommitDurable(InputLifecycleCommitPermit permit,
                  const InputLifecycleDurableBundle& bundle) noexcept;
    [[nodiscard]] bool
    SuppressDeliveryAfterCommit(const InputLifecycleOperationLease& operation,
                                uint64_t observation_id) noexcept;
    [[nodiscard]] bool
    SuppressDeliveryAfterCommit(const InputLifecycleDurableBundle& bundle) noexcept;

    // Lower-case aliases keep this seam consistent with the surrounding
    // registry API while retaining the ruling's exact refinement names.
    [[nodiscard]] std::optional<InputLifecyclePreparedObservation>
    observe_prepared(const InputLifecycleOperationLease& operation,
                     uint64_t prepared_id) noexcept {
        return ObservePrepared(operation, prepared_id);
    }
    [[nodiscard]] InputLifecycleRefinementResult
    cancel_or_expire(const InputLifecycleOperationLease& operation,
                     uint64_t observation_id) noexcept {
        return CancelOrExpire(operation, observation_id);
    }
    [[nodiscard]] std::optional<InputLifecycleCommitPermit>
    select_commit(const InputLifecycleOperationLease& operation,
                  uint64_t observation_id) noexcept {
        return SelectCommit(operation, observation_id);
    }
    [[nodiscard]] InputLifecycleRefinementResult
    commit_durable(InputLifecycleCommitPermit permit,
                   const InputLifecycleDurableBundle& bundle) noexcept {
        return CommitDurable(std::move(permit), bundle);
    }
    [[nodiscard]] bool
    suppress_delivery_after_commit(const InputLifecycleOperationLease& operation,
                                   uint64_t observation_id) noexcept {
        return SuppressDeliveryAfterCommit(operation, observation_id);
    }
    [[nodiscard]] bool
    suppress_delivery_after_commit(const InputLifecycleDurableBundle& bundle) noexcept {
        return SuppressDeliveryAfterCommit(bundle);
    }

    [[nodiscard]] std::optional<InputLifecycleRefinementState>
    refinement_state(const InputLifecycleOperationLease& operation) const noexcept;
    [[nodiscard]] std::optional<InputLifecycleDurableBundle>
    durable_bundle(const InputLifecycleOperationLease& operation) const noexcept;

    // Reserve lifecycle capacity before InputRecord publication.  This runs in
    // the endpoint's input_job_state selector, so capacity failure precedes
    // publication and cannot create an unowned retained record.  A terminal
    // pre-commit tombstone selects the endpoint's closed-commit path.
    [[nodiscard]] InputLifecycleCommitDecision prepare_route_commit(
        InputRecordKey key) noexcept;
    void abort_route_commit(InputRecordKey key) noexcept;

    // Completes the reservation after the endpoint has validated and committed
    // the exact route transaction.  retained is true only when an attachable
    // InputRecord was published; a closed commit is validated but not retained.
    [[nodiscard]] bool observe_route_commit(InputRecordKey key,
                                            bool retained) noexcept;

    // Reserve one compiler authorization.  A cancelled current attempt may be
    // replaced by another assignment for the same logical job without changing
    // cache identity.  A live or closed owner cannot be stolen.
    [[nodiscard]] bool begin_attachment(InputRecordKey key,
                                        InputLeaseOwner owner,
                                        uint64_t request_id);
    void finish_attachment(InputRecordKey key, InputLeaseOwner owner,
                           uint64_t request_id, bool authorized) noexcept;

    // Apply one exact replay-safe lifecycle command.  Callers perform the
    // indicated endpoint close/collection on the same executor, then call
    // finish_apply() with whether that endpoint mutation succeeded.
    [[nodiscard]] InputLifecycleApplyResult begin_apply(
        const InputLifecycleRequest& request) noexcept;
    [[nodiscard]] bool finish_apply(const InputLifecycleRequest& request,
                                    bool endpoint_mutation_succeeded) noexcept;

    // Consulted synchronously by P50ServerEndpoint::input_job_state before an
    // exact late commit is published.  A close-before-commit tombstone forces
    // the endpoint's non-attachable closed-commit path.
    [[nodiscard]] bool job_closed(InputRecordKey key) const noexcept;

    void clear() noexcept;
    [[nodiscard]] size_t owner_count() const noexcept { return leases_.size(); }
    [[nodiscard]] size_t replay_count() const noexcept { return replays_.size(); }

private:
    struct Lease {
        std::optional<InputLeaseOwner> owner;
        std::vector<InputLeaseOwner> retired_owners;
        uint64_t attachment_request_id = 0;
        bool committed = false;
        bool route_commit_pending = false;
        bool route_commit_observed = false;
        bool attachment_pending = false;
        bool attachment_authorized = false;
        bool attempt_cancelled = false;
        bool attempt_retiring = false;
        uint64_t retirement_id = 0;
        uint64_t f_store_generation = 0;
        FStoreGuid f_store_guid{};
        uint64_t immutable_size = 0;
        Digest128 immutable_digest{};
        bool job_closed = false;
        bool apply_pending = false;
        bool apply_created = false;
    };

    struct Replay {
        InputLifecycleRequest request{};
        InputLifecycleApplyStatus status = InputLifecycleApplyStatus::Applied;
        bool complete = false;
    };

    struct Refinement {
        InputLifecyclePreparedObservation prepared{};
        InputLifecycleRefinementState state = InputLifecycleRefinementState::None;
        uint64_t permit_id = 0;
        std::optional<InputLifecycleDurableBundle> durable;
    };

    [[nodiscard]] static bool key_valid(InputRecordKey key) noexcept;
    [[nodiscard]] bool replay_conflicts(
        const InputLifecycleRequest& request,
        InputLifecycleApplyResult& result) const noexcept;
    [[nodiscard]] bool reserve_replay(const InputLifecycleRequest& request) noexcept;
    void complete_replay(const InputLifecycleRequest& request,
                         InputLifecycleApplyStatus status) noexcept;
    void forget_replay(uint64_t operation_id) noexcept;
    void collect_closed(InputRecordKey key) noexcept;
    void erase_lease(std::map<InputRecordKey, Lease>::iterator position) noexcept;

    size_t max_owners_ = 0;
    size_t max_replays_ = 0;
    std::map<InputRecordKey, Lease> leases_;
    std::map<uint64_t, Replay> replays_;
    std::deque<uint64_t> replay_order_;
    // Operation ids are retained for the lifetime of this F-store runtime so
    // a stale completion cannot mutate a later role-slot reuse.  Capacity is
    // bounded by max_replays_; replacement clears this whole ledger with the
    // endpoint's one FInputStoreOwner.
    std::map<uint64_t, Refinement> refinements_;
    uint64_t next_refinement_observation_id_ = 1;
    uint64_t next_refinement_permit_id_ = 1;
    size_t retired_owner_count_ = 0;
    uint64_t f_store_generation_ = 0;
    FStoreGuid f_store_guid_{};
};

enum class InputLifecycleStatus : uint8_t {
    Applied = 0,
    AlreadyApplied,
    StoreReplaced,
    InvalidArgument,
    PeerUnauthenticated,
    HandshakeFailed,
    MalformedResponse,
    StaleIdentity,
    UnknownRecord,
    StaleOwner,
    ConflictingReplay,
    CapacityExceeded,
    Rejected,
    Timeout,
    Disconnected,
    AttemptQuiescedRecordRetained,
    ReplacementInstalledRecordRetained,
    JobClosedRecordRetained,
    JobClosedRecordReclaimed,
    RetirementProofRequired,
    GenerationMismatch,
};

const char* input_lifecycle_status_name(InputLifecycleStatus status) noexcept;

struct InputLifecycleResult {
    InputLifecycleStatus status = InputLifecycleStatus::InvalidArgument;
    InputLifecycleRequest request{};
};

class InputLifecycleClient {
public:
    // Executes one exact lifecycle operation and accepts only an authenticated
    // byte-exact echo.  The sidecar's bounded replay table makes an identical
    // retry safe after a lost response; changed operation payload is rejected.
    [[nodiscard]] static InputLifecycleResult apply(
        const std::string& socket_path, InputLifecycleRequest request,
        const local::CredentialExpectation& expected_peer,
        std::chrono::steady_clock::time_point deadline) noexcept;
};

}  // namespace icecc::p50
