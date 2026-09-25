#include "p50_input_lifecycle.h"
#include "p50_control_operation.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace icecc::p50 {

namespace {

bool attempt_retirement_action(InputLifecycleAction action) noexcept {
    return action == InputLifecycleAction::PrepareAttemptRetirement ||
           action == InputLifecycleAction::CommitAttemptReplacement ||
           action == InputLifecycleAction::CloseLogicalInputLease;
}

bool nonzero_digest(const Digest128& digest) noexcept {
    return digest != Digest128{};
}

InputLifecycleResult lifecycle_rejected(InputLifecycleStatus status,
                                        InputLifecycleRequest request) noexcept {
    return InputLifecycleResult{status, request};
}

InputLifecycleStatus lifecycle_status_for_local(local::Status status) noexcept {
    switch (status) {
    case local::Status::Timeout:
        return InputLifecycleStatus::Timeout;
    case local::Status::StaleGeneration:
    case local::Status::IdentityMismatch:
        return InputLifecycleStatus::StaleIdentity;
    case local::Status::CleanEof:
    case local::Status::Truncated:
    case local::Status::IoError:
        return InputLifecycleStatus::Disconnected;
    case local::Status::PeerCredentialUnavailable:
    case local::Status::PeerCredentialMismatch:
        return InputLifecycleStatus::PeerUnauthenticated;
    case local::Status::InvalidArgument:
    case local::Status::InvalidPath:
        return InputLifecycleStatus::InvalidArgument;
    default:
        return InputLifecycleStatus::HandshakeFailed;
    }
}

InputLifecycleStatus lifecycle_status_for_apply(
    InputLifecycleApplyStatus status) noexcept {
    switch (status) {
    case InputLifecycleApplyStatus::Applied:
        return InputLifecycleStatus::Applied;
    case InputLifecycleApplyStatus::AlreadyApplied:
        return InputLifecycleStatus::AlreadyApplied;
    case InputLifecycleApplyStatus::InvalidArgument:
        return InputLifecycleStatus::InvalidArgument;
    case InputLifecycleApplyStatus::UnknownRecord:
        return InputLifecycleStatus::UnknownRecord;
    case InputLifecycleApplyStatus::StaleOwner:
        return InputLifecycleStatus::StaleOwner;
    case InputLifecycleApplyStatus::ConflictingReplay:
        return InputLifecycleStatus::ConflictingReplay;
    case InputLifecycleApplyStatus::CapacityExceeded:
        return InputLifecycleStatus::CapacityExceeded;
    case InputLifecycleApplyStatus::AttemptQuiescedRecordRetained:
        return InputLifecycleStatus::AttemptQuiescedRecordRetained;
    case InputLifecycleApplyStatus::ReplacementInstalledRecordRetained:
        return InputLifecycleStatus::ReplacementInstalledRecordRetained;
    case InputLifecycleApplyStatus::JobClosedRecordRetained:
        return InputLifecycleStatus::JobClosedRecordRetained;
    case InputLifecycleApplyStatus::JobClosedRecordReclaimed:
        return InputLifecycleStatus::JobClosedRecordReclaimed;
    case InputLifecycleApplyStatus::RetirementProofRequired:
        return InputLifecycleStatus::RetirementProofRequired;
    case InputLifecycleApplyStatus::GenerationMismatch:
        return InputLifecycleStatus::GenerationMismatch;
    }
    return InputLifecycleStatus::Rejected;
}

}  // namespace

InputLifecycleRegistry::InputLifecycleRegistry(size_t max_owners,
                                               size_t max_replays)
    : max_owners_(max_owners), max_replays_(max_replays) {
    if (max_owners_ == 0 || max_replays_ == 0)
        throw std::invalid_argument(
            "input lifecycle owner and replay limits must be nonzero");
}

void InputLifecycleRegistry::bind_store_identity(
    uint64_t f_store_generation, FStoreGuid f_store_guid) noexcept {
    // Binding is immutable for one runtime.  Refusing an attempted rebinding
    // keeps a stale sidecar operation from changing the owner identity under
    // an already-retained InputRecord.
    if (f_store_generation == 0 || f_store_guid == FStoreGuid{})
        return;
    if (f_store_generation_ == 0 && f_store_guid_ == FStoreGuid{}) {
        f_store_generation_ = f_store_generation;
        f_store_guid_ = f_store_guid;
    }
}

std::optional<InputLifecyclePreparedObservation>
InputLifecycleRegistry::ObservePrepared(
    const InputLifecycleOperationLease& operation,
    uint64_t prepared_id) noexcept {
    if (!operation.valid() ||
        operation.action != InputLifecycleAction::PrepareAttemptRetirement ||
        prepared_id == 0)
        return std::nullopt;

    const auto existing = refinements_.find(operation.operation_id);
    if (existing != refinements_.end()) {
        // Operation ids are never a lookup-only authority.  An exact replay
        // returns the original observation, while reuse for a different
        // operation/prepared id fails closed against role-slot reuse.
        if (!existing->second.prepared.matches(operation, prepared_id))
            return std::nullopt;
        return existing->second.prepared;
    }
    if (refinements_.size() >= max_replays_ ||
        next_refinement_observation_id_ == 0)
        return std::nullopt;

    const InputLifecyclePreparedObservation prepared{
        operation, prepared_id, next_refinement_observation_id_, true};
    try {
        refinements_.emplace(
            operation.operation_id,
            Refinement{prepared, InputLifecycleRefinementState::Prepared, 0,
                       std::nullopt});
    } catch (...) {
        return std::nullopt;
    }
    if (next_refinement_observation_id_ == std::numeric_limits<uint64_t>::max())
        next_refinement_observation_id_ = 0;
    else
        ++next_refinement_observation_id_;
    return prepared;
}

InputLifecycleRefinementResult InputLifecycleRegistry::CancelOrExpire(
    const InputLifecycleOperationLease& operation,
    uint64_t observation_id) noexcept {
    if (!operation.valid() ||
        operation.action != InputLifecycleAction::PrepareAttemptRetirement ||
        observation_id == 0)
        return InputLifecycleRefinementResult::InvalidArgument;
    const auto position = refinements_.find(operation.operation_id);
    if (position == refinements_.end())
        return InputLifecycleRefinementResult::StaleOperation;
    Refinement& refinement = position->second;
    if (refinement.prepared.operation != operation ||
        refinement.prepared.observation_id != observation_id)
        return InputLifecycleRefinementResult::ConflictingOperation;
    switch (refinement.state) {
    case InputLifecycleRefinementState::Prepared:
        // This is the cancel-first linearization point.  The transient decoder
        // profile is reset by the owning reducer; no durable bundle is
        // retained or made visible from this state.
        refinement.state = InputLifecycleRefinementState::CancelledNoDurability;
        refinement.permit_id = 0;
        refinement.durable.reset();
        return InputLifecycleRefinementResult::CancelledNoDurability;
    case InputLifecycleRefinementState::CancelledNoDurability:
        return InputLifecycleRefinementResult::AlreadyCancelled;
    case InputLifecycleRefinementState::CommitSelected:
    case InputLifecycleRefinementState::DurableCommitted:
    case InputLifecycleRefinementState::DeliverySuppressed:
        // Commit selection is the commit-wins linearization point.  A late
        // cancellation/expiry cannot erase a durable bundle or lastCommit.
        return InputLifecycleRefinementResult::CommitWon;
    case InputLifecycleRefinementState::None:
        break;
    }
    return InputLifecycleRefinementResult::StaleOperation;
}

std::optional<InputLifecycleCommitPermit>
InputLifecycleRegistry::SelectCommit(
    const InputLifecycleOperationLease& operation,
    uint64_t observation_id) noexcept {
    if (!operation.valid() ||
        operation.action != InputLifecycleAction::PrepareAttemptRetirement ||
        observation_id == 0)
        return std::nullopt;
    const auto position = refinements_.find(operation.operation_id);
    if (position == refinements_.end() ||
        position->second.prepared.operation != operation ||
        position->second.prepared.observation_id != observation_id ||
        position->second.state != InputLifecycleRefinementState::Prepared ||
        next_refinement_permit_id_ == 0)
        return std::nullopt;
    const uint64_t permit_id = next_refinement_permit_id_;
    position->second.state = InputLifecycleRefinementState::CommitSelected;
    position->second.permit_id = permit_id;
    if (next_refinement_permit_id_ == std::numeric_limits<uint64_t>::max())
        next_refinement_permit_id_ = 0;
    else
        ++next_refinement_permit_id_;
    return InputLifecycleCommitPermit(operation, observation_id, permit_id);
}

InputLifecycleRefinementResult InputLifecycleRegistry::CommitDurable(
    InputLifecycleCommitPermit permit,
    const InputLifecycleDurableBundle& bundle) noexcept {
    if (!permit.valid())
        return InputLifecycleRefinementResult::InvalidArgument;
    const InputLifecycleOperationLease operation = permit.operation();
    const uint64_t observation_id = permit.observation_id();
    const uint64_t permit_id = permit.permit_id();
    const auto position = refinements_.find(operation.operation_id);
    const bool exact = position != refinements_.end() &&
                       position->second.prepared.operation == operation &&
                       position->second.prepared.observation_id == observation_id &&
                       position->second.state ==
                           InputLifecycleRefinementState::CommitSelected &&
                       position->second.permit_id == permit_id &&
                       bundle.matches(permit) &&
                       bundle.prepared_id ==
                           position->second.prepared.prepared_id;
    // A permit is linearized once at this boundary even when the supplied
    // bundle is malformed.  The caller cannot retry with a second copy.
    permit.consume();
    if (!exact)
        return position == refinements_.end()
                   ? InputLifecycleRefinementResult::StaleOperation
                   : InputLifecycleRefinementResult::ConflictingOperation;
    position->second.durable = bundle;
    position->second.state = InputLifecycleRefinementState::DurableCommitted;
    return InputLifecycleRefinementResult::DurableCommitted;
}

bool InputLifecycleRegistry::SuppressDeliveryAfterCommit(
    const InputLifecycleOperationLease& operation,
    uint64_t observation_id) noexcept {
    if (!operation.valid() ||
        operation.action != InputLifecycleAction::PrepareAttemptRetirement ||
        observation_id == 0)
        return false;
    const auto position = refinements_.find(operation.operation_id);
    if (position == refinements_.end() ||
        position->second.prepared.operation != operation ||
        position->second.prepared.observation_id != observation_id)
        return false;
    if (position->second.state == InputLifecycleRefinementState::DeliverySuppressed)
        return true;
    if (position->second.state != InputLifecycleRefinementState::DurableCommitted ||
        !position->second.durable.has_value())
        return false;
    position->second.state = InputLifecycleRefinementState::DeliverySuppressed;
    return true;
}

bool InputLifecycleRegistry::SuppressDeliveryAfterCommit(
    const InputLifecycleDurableBundle& bundle) noexcept {
    if (!bundle.valid())
        return false;
    const auto position = refinements_.find(bundle.operation.operation_id);
    if (position == refinements_.end() ||
        !position->second.durable.has_value() ||
        position->second.durable.value() != bundle)
        return false;
    return SuppressDeliveryAfterCommit(bundle.operation, bundle.observation_id);
}

std::optional<InputLifecycleRefinementState>
InputLifecycleRegistry::refinement_state(
    const InputLifecycleOperationLease& operation) const noexcept {
    if (!operation.valid())
        return std::nullopt;
    const auto position = refinements_.find(operation.operation_id);
    if (position == refinements_.end() ||
        position->second.prepared.operation != operation)
        return std::nullopt;
    return position->second.state;
}

std::optional<InputLifecycleDurableBundle>
InputLifecycleRegistry::durable_bundle(
    const InputLifecycleOperationLease& operation) const noexcept {
    if (!operation.valid())
        return std::nullopt;
    const auto position = refinements_.find(operation.operation_id);
    if (position == refinements_.end() ||
        position->second.prepared.operation != operation ||
        !position->second.durable.has_value())
        return std::nullopt;
    return position->second.durable;
}

bool InputLifecycleRegistry::key_valid(InputRecordKey key) noexcept {
    return key.c_store_guid != CStoreGuid{};
}

InputLifecycleCommitDecision InputLifecycleRegistry::prepare_route_commit(
    InputRecordKey key) noexcept {
    if (!key_valid(key))
        return InputLifecycleCommitDecision::CapacityExceeded;
    auto position = leases_.find(key);
    if (position == leases_.end()) {
        if (leases_.size() >= max_owners_)
            return InputLifecycleCommitDecision::CapacityExceeded;
        try {
            position = leases_.try_emplace(key).first;
        } catch (...) {
            return InputLifecycleCommitDecision::CapacityExceeded;
        }
    }
    Lease& lease = position->second;
    lease.route_commit_pending = true;
    return lease.job_closed ? InputLifecycleCommitDecision::Closed
                            : InputLifecycleCommitDecision::Open;
}

void InputLifecycleRegistry::abort_route_commit(InputRecordKey key) noexcept {
    const auto position = leases_.find(key);
    if (position == leases_.end())
        return;
    Lease& lease = position->second;
    lease.route_commit_pending = false;
    if (!lease.committed && !lease.route_commit_observed &&
        !lease.job_closed && !lease.owner.has_value())
        erase_lease(position);
}

bool InputLifecycleRegistry::observe_route_commit(InputRecordKey key,
                                                  bool retained) noexcept {
    const auto position = leases_.find(key);
    if (position == leases_.end())
        return false;
    Lease& lease = position->second;
    if (!lease.route_commit_pending && !lease.route_commit_observed)
        return false;
    if (retained && lease.job_closed)
        return false;
    lease.route_commit_pending = false;
    lease.route_commit_observed = true;
    lease.committed = retained;
    if (!retained)
        lease.job_closed = true;
    collect_closed(key);
    return true;
}

bool InputLifecycleRegistry::begin_attachment(InputRecordKey key,
                                              InputLeaseOwner owner,
                                              uint64_t request_id) {
    if (!key_valid(key) || !input_lease_owner_valid(owner) || request_id == 0)
        return false;
    const auto position = leases_.find(key);
    if (position == leases_.end())
        return false;
    Lease& lease = position->second;
    if (!lease.committed || lease.job_closed || lease.attachment_pending ||
        lease.attempt_retiring)
        return false;

    if (!lease.owner.has_value()) {
        lease.owner = owner;
    } else if (*lease.owner == owner) {
        // Attempt cancellation permanently revokes this exact assignment
        // owner, including when cancellation races a lost descriptor ACK.
        // Only the fresh-owner replacement arm below may reuse retained
        // bytes after CancelAttempt.
        if (lease.attempt_cancelled)
            return false;
    } else {
        // Replacement is legal only after the exact current attempt was
        // cancelled.  It keeps the same logical job and must carry a fresh
        // assignment nonce/ATTEMPT_ID.
        if (!lease.attempt_cancelled ||
            lease.owner->logical_job != owner.logical_job ||
            lease.owner->assignment_nonce == owner.assignment_nonce ||
            std::find(lease.retired_owners.begin(),
                      lease.retired_owners.end(), owner) !=
                lease.retired_owners.end() ||
            retired_owner_count_ >= max_replays_)
            return false;
        try {
            lease.retired_owners.push_back(*lease.owner);
        } catch (...) {
            return false;
        }
        ++retired_owner_count_;
        lease.owner = owner;
        lease.attachment_authorized = false;
        lease.attempt_cancelled = false;
        lease.attachment_request_id = 0;
    }

    if (lease.attachment_authorized || lease.attachment_request_id != 0)
        return false;
    lease.attachment_request_id = request_id;
    lease.attachment_pending = true;
    return true;
}

void InputLifecycleRegistry::finish_attachment(InputRecordKey key,
                                               InputLeaseOwner owner,
                                               uint64_t request_id,
                                               bool authorized) noexcept {
    const auto position = leases_.find(key);
    if (position == leases_.end())
        return;
    Lease& lease = position->second;
    if (!lease.owner.has_value() || *lease.owner != owner ||
        !lease.attachment_pending ||
        lease.attachment_request_id != request_id)
        return;
    lease.attachment_pending = false;
    lease.attachment_authorized = authorized;
    if (!authorized)
        lease.attachment_request_id = 0;
    collect_closed(key);
}

bool InputLifecycleRegistry::replay_conflicts(
    const InputLifecycleRequest& request,
    InputLifecycleApplyResult& result) const noexcept {
    const auto replay = replays_.find(request.operation_id);
    if (replay == replays_.end())
        return false;
    if (replay->second.request != request || !replay->second.complete) {
        result.status = InputLifecycleApplyStatus::ConflictingReplay;
    } else if (replay->second.status == InputLifecycleApplyStatus::Applied) {
        result.status = InputLifecycleApplyStatus::AlreadyApplied;
    } else {
        result.status = replay->second.status;
    }
    return true;
}

void InputLifecycleRegistry::complete_replay(
    const InputLifecycleRequest& request,
    InputLifecycleApplyStatus status) noexcept {
    const auto position = replays_.find(request.operation_id);
    if (position == replays_.end() || position->second.request != request)
        return;
    position->second.status = status;
    position->second.complete = true;
}

void InputLifecycleRegistry::erase_lease(
    std::map<InputRecordKey, Lease>::iterator position) noexcept {
    if (position == leases_.end())
        return;
    const size_t retired = position->second.retired_owners.size();
    retired_owner_count_ = retired > retired_owner_count_
                               ? 0
                               : retired_owner_count_ - retired;
    leases_.erase(position);
}

bool InputLifecycleRegistry::reserve_replay(
    const InputLifecycleRequest& request) noexcept {
    try {
        if (replays_.size() >= max_replays_) {
            if (replay_order_.empty())
                return false;
            const uint64_t oldest = replay_order_.front();
            const auto position = replays_.find(oldest);
            if (position == replays_.end() || !position->second.complete)
                return false;
            replays_.erase(position);
            replay_order_.pop_front();
        }
        const auto [position, inserted] = replays_.emplace(
            request.operation_id,
            Replay{request, InputLifecycleApplyStatus::Applied, false});
        (void)position;
        if (!inserted)
            return false;
        replay_order_.push_back(request.operation_id);
        return true;
    } catch (...) {
        replays_.erase(request.operation_id);
        if (!replay_order_.empty() && replay_order_.back() == request.operation_id)
            replay_order_.pop_back();
        return false;
    }
}

void InputLifecycleRegistry::forget_replay(uint64_t operation_id) noexcept {
    replays_.erase(operation_id);
    const auto position =
        std::find(replay_order_.begin(), replay_order_.end(), operation_id);
    if (position != replay_order_.end())
        replay_order_.erase(position);
}

InputLifecycleApplyResult InputLifecycleRegistry::begin_apply(
    const InputLifecycleRequest& request) noexcept {
    InputLifecycleApplyResult result;
    const bool retirement = attempt_retirement_action(request.action);
    if (request.identity.generation == 0 || request.identity.attempt == 0 ||
        request.operation_id == 0 || !key_valid(request.key) ||
        !input_lease_owner_valid(request.owner) ||
        !input_lifecycle_action_valid(request.action))
        return result;
    if (retirement &&
        (request.f_store_generation == 0 || request.f_store_guid == FStoreGuid{} ||
         !nonzero_digest(request.immutable_digest) || request.retirement_id == 0 ||
         !request.absolute_deadline.valid() ||
         request.deadline != request.absolute_deadline.as_steady_time_point())) {
        result.status = InputLifecycleApplyStatus::InvalidArgument;
        return result;
    }
    if (retirement &&
        (f_store_generation_ == 0 || f_store_guid_ == FStoreGuid{} ||
         request.f_store_generation != f_store_generation_ ||
         request.f_store_guid != f_store_guid_)) {
        result.status = InputLifecycleApplyStatus::GenerationMismatch;
        return result;
    }
    if (request.action == InputLifecycleAction::CommitAttemptReplacement &&
        (!request.replacement_owner.has_value() ||
         !input_lease_owner_valid(*request.replacement_owner) ||
         *request.replacement_owner == request.owner ||
         request.replacement_owner->logical_job != request.owner.logical_job)) {
        result.status = InputLifecycleApplyStatus::InvalidArgument;
        return result;
    }
    if (request.action != InputLifecycleAction::CommitAttemptReplacement &&
        request.replacement_owner.has_value()) {
        result.status = InputLifecycleApplyStatus::InvalidArgument;
        return result;
    }
    if (replay_conflicts(request, result))
        return result;
    if (!reserve_replay(request)) {
        result.status = InputLifecycleApplyStatus::CapacityExceeded;
        return result;
    }

    auto position = leases_.find(request.key);
    bool created = false;
    if (position == leases_.end()) {
        if (request.action == InputLifecycleAction::CancelAttempt || retirement) {
            result.status = InputLifecycleApplyStatus::UnknownRecord;
            complete_replay(request, result.status);
            return result;
        }
        if (leases_.size() >= max_owners_) {
            result.status = InputLifecycleApplyStatus::CapacityExceeded;
            complete_replay(request, result.status);
            return result;
        }
        try {
            position = leases_.try_emplace(request.key).first;
        } catch (...) {
            result.status = InputLifecycleApplyStatus::CapacityExceeded;
            complete_replay(request, result.status);
            return result;
        }
        position->second.owner = request.owner;
        created = true;
    }

    Lease& lease = position->second;
    if (!lease.owner.has_value())
        lease.owner = request.owner;
    if (*lease.owner != request.owner || lease.apply_pending) {
        if (created)
            erase_lease(position);
        result.status = InputLifecycleApplyStatus::StaleOwner;
        complete_replay(request, result.status);
        return result;
    }
    if (retirement && !lease.committed) {
        result.status = InputLifecycleApplyStatus::UnknownRecord;
        complete_replay(request, result.status);
        return result;
    }
    if (retirement && lease.f_store_generation != 0 &&
        (lease.f_store_generation != request.f_store_generation ||
         lease.f_store_guid != request.f_store_guid ||
         lease.immutable_size != request.immutable_size ||
         lease.immutable_digest != request.immutable_digest)) {
        result.status = InputLifecycleApplyStatus::GenerationMismatch;
        complete_replay(request, result.status);
        return result;
    }
    if (request.action == InputLifecycleAction::PrepareAttemptRetirement) {
        if (lease.attempt_retiring) {
            result.status = InputLifecycleApplyStatus::AlreadyApplied;
            complete_replay(request, result.status);
            return result;
        }
        if (lease.attachment_pending || lease.attempt_cancelled) {
            result.status = InputLifecycleApplyStatus::RetirementProofRequired;
            complete_replay(request, result.status);
            return result;
        }
    }
    if (request.action == InputLifecycleAction::CommitAttemptReplacement) {
        if (!lease.attempt_retiring || lease.retirement_id != request.retirement_id) {
            result.status = InputLifecycleApplyStatus::RetirementProofRequired;
            complete_replay(request, result.status);
            return result;
        }
        if (std::find(lease.retired_owners.begin(), lease.retired_owners.end(),
                      *request.replacement_owner) != lease.retired_owners.end() ||
            *request.replacement_owner == *lease.owner ||
            retired_owner_count_ >= max_replays_) {
            result.status = InputLifecycleApplyStatus::StaleOwner;
            complete_replay(request, result.status);
            return result;
        }
    }
    if ((lease.job_closed &&
         request.action != InputLifecycleAction::CancelAttempt) ||
        (request.action == InputLifecycleAction::CancelAttempt &&
         lease.attempt_cancelled)) {
        result.status = InputLifecycleApplyStatus::AlreadyApplied;
        complete_replay(request, result.status);
        return result;
    }
    if (lease.job_closed) {
        if (created)
            erase_lease(position);
        result.status = InputLifecycleApplyStatus::StaleOwner;
        complete_replay(request, result.status);
        return result;
    }

    lease.apply_pending = true;
    lease.apply_created = created;
    result.status = InputLifecycleApplyStatus::Applied;
    if (request.action != InputLifecycleAction::CancelAttempt &&
        request.action != InputLifecycleAction::PrepareAttemptRetirement &&
        request.action != InputLifecycleAction::CommitAttemptReplacement &&
        lease.committed) {
        result.close_record = true;
        result.collect_record = true;
    }
    return result;
}

bool InputLifecycleRegistry::finish_apply(
    const InputLifecycleRequest& request,
    bool endpoint_mutation_succeeded) noexcept {
    const auto replay = replays_.find(request.operation_id);
    const auto position = leases_.find(request.key);
    if (replay == replays_.end() || replay->second.request != request ||
        replay->second.complete || position == leases_.end())
        return false;
    Lease& lease = position->second;
    if (!lease.apply_pending || !lease.owner.has_value() ||
        *lease.owner != request.owner)
        return false;

    if (!endpoint_mutation_succeeded) {
        const bool erase = lease.apply_created;
        lease.apply_pending = false;
        lease.apply_created = false;
        forget_replay(request.operation_id);
        if (erase) {
            const auto created = leases_.find(request.key);
            if (created != leases_.end())
                erase_lease(created);
        }
        return false;
    }

    lease.apply_pending = false;
    lease.apply_created = false;
    if (request.action == InputLifecycleAction::CancelAttempt) {
        lease.attempt_cancelled = true;
    } else if (request.action == InputLifecycleAction::PrepareAttemptRetirement) {
        lease.attempt_retiring = true;
        lease.retirement_id = request.retirement_id;
        lease.f_store_generation = request.f_store_generation;
        lease.f_store_guid = request.f_store_guid;
        lease.immutable_size = request.immutable_size;
        lease.immutable_digest = request.immutable_digest;
    } else if (request.action == InputLifecycleAction::CommitAttemptReplacement) {
        // The old owner is retired only after the caller's complete local
        // proof has admitted COMMIT.  The retained InputRecord itself is not
        // closed: the endpoint remains the sole immutable-byte owner.
        try {
            lease.retired_owners.push_back(request.owner);
        } catch (...) {
            // A bounded append failure is fail-closed rather than a partial
            // owner substitution.
            lease.apply_pending = false;
            lease.apply_created = false;
            forget_replay(request.operation_id);
            return false;
        }
        ++retired_owner_count_;
        lease.owner = request.replacement_owner;
        lease.attempt_retiring = false;
        lease.retirement_id = 0;
        lease.attempt_cancelled = false;
        lease.attachment_request_id = 0;
        lease.attachment_pending = false;
        lease.attachment_authorized = false;
    } else {
        // Logical close is separate from attempt retirement and is the only
        // path that asks the endpoint owner to close/collect the InputRecord.
        lease.job_closed = true;
        lease.attempt_cancelled = true;
    }
    replay->second.status =
        request.action == InputLifecycleAction::PrepareAttemptRetirement
            ? InputLifecycleApplyStatus::AttemptQuiescedRecordRetained
            : request.action == InputLifecycleAction::CommitAttemptReplacement
                  ? InputLifecycleApplyStatus::ReplacementInstalledRecordRetained
                  : request.action == InputLifecycleAction::CloseLogicalInputLease
                        ? InputLifecycleApplyStatus::JobClosedRecordRetained
                        : InputLifecycleApplyStatus::Applied;
    replay->second.complete = true;
    collect_closed(request.key);
    return true;
}

void InputLifecycleRegistry::collect_closed(InputRecordKey key) noexcept {
    const auto position = leases_.find(key);
    if (position == leases_.end())
        return;
    const Lease& lease = position->second;
    // A pre-commit terminal tombstone must remain for input_job_state().  Once
    // a committed record has been closed/collected, exact operation replays are
    // answered by the separately bounded replay table and the owner row can go.
    if (lease.job_closed && lease.route_commit_observed &&
        !lease.attachment_pending)
        erase_lease(position);
}

bool InputLifecycleRegistry::job_closed(InputRecordKey key) const noexcept {
    const auto position = leases_.find(key);
    return position != leases_.end() && position->second.job_closed;
}

void InputLifecycleRegistry::clear() noexcept {
    leases_.clear();
    replays_.clear();
    replay_order_.clear();
    refinements_.clear();
    next_refinement_observation_id_ = 1;
    next_refinement_permit_id_ = 1;
    retired_owner_count_ = 0;
}

const char* input_lifecycle_status_name(InputLifecycleStatus status) noexcept {
    switch (status) {
    case InputLifecycleStatus::Applied: return "applied";
    case InputLifecycleStatus::AlreadyApplied: return "already-applied";
    case InputLifecycleStatus::StoreReplaced: return "store-replaced";
    case InputLifecycleStatus::InvalidArgument: return "invalid-argument";
    case InputLifecycleStatus::PeerUnauthenticated: return "peer-unauthenticated";
    case InputLifecycleStatus::HandshakeFailed: return "handshake-failed";
    case InputLifecycleStatus::MalformedResponse: return "malformed-response";
    case InputLifecycleStatus::StaleIdentity: return "stale-identity";
    case InputLifecycleStatus::UnknownRecord: return "unknown-record";
    case InputLifecycleStatus::StaleOwner: return "stale-owner";
    case InputLifecycleStatus::ConflictingReplay: return "conflicting-replay";
    case InputLifecycleStatus::CapacityExceeded: return "capacity-exceeded";
    case InputLifecycleStatus::Rejected: return "rejected";
    case InputLifecycleStatus::Timeout: return "timeout";
    case InputLifecycleStatus::Disconnected: return "disconnected";
    case InputLifecycleStatus::AttemptQuiescedRecordRetained:
        return "attempt-quiesced-record-retained";
    case InputLifecycleStatus::ReplacementInstalledRecordRetained:
        return "replacement-installed-record-retained";
    case InputLifecycleStatus::JobClosedRecordRetained:
        return "job-closed-record-retained";
    case InputLifecycleStatus::JobClosedRecordReclaimed:
        return "job-closed-record-reclaimed";
    case InputLifecycleStatus::RetirementProofRequired:
        return "retirement-proof-required";
    case InputLifecycleStatus::GenerationMismatch:
        return "generation-mismatch";
    }
    return "unknown";
}

InputLifecycleResult InputLifecycleClient::apply(
    const std::string& socket_path, InputLifecycleRequest request,
    const local::CredentialExpectation& expected_peer,
    std::chrono::steady_clock::time_point deadline) noexcept {
    const bool retirement = attempt_retirement_action(request.action);
    const sidecar::MonotonicClockIdentity clock_identity =
        sidecar::process_monotonic_clock_identity();
    if (request.identity.generation == 0 || request.identity.attempt == 0 ||
        request.operation_id == 0 || request.key.c_store_guid == CStoreGuid{} ||
        !input_lease_owner_valid(request.owner) ||
        !input_lifecycle_action_valid(request.action) || !expected_peer.specified())
        return lifecycle_rejected(InputLifecycleStatus::InvalidArgument, request);
    // A retirement dialogue may only consume the caller's original absolute
    // CLOCK_MONOTONIC deadline.  The compatibility helper remains available
    // for old callers, but it cannot reconstruct a fresh deadline from the
    // receive time or cross into another time namespace.
    if (retirement &&
        (!request.absolute_deadline.valid() ||
         !request.absolute_deadline.matches_clock(clock_identity) ||
         request.absolute_deadline.as_steady_time_point() != deadline))
        return lifecycle_rejected(InputLifecycleStatus::InvalidArgument, request);

    local::Status io = local::Status::InvalidArgument;
    local::Connection connection =
        local::connect_unix_until(socket_path, deadline, &io);
    if (!connection.valid())
        return lifecycle_rejected(lifecycle_status_for_local(io), request);
    io = connection.verify_peer_credentials(expected_peer);
    if (io != local::Status::Ok)
        return lifecycle_rejected(InputLifecycleStatus::PeerUnauthenticated, request);
    io = connection.send_until(
        local::make_hello(local::PeerRole::Daemon, request.identity), deadline);
    if (io != local::Status::Ok)
        return lifecycle_rejected(lifecycle_status_for_local(io), request);

    local::Frame reply;
    io = connection.receive_until(reply, deadline);
    if (io != local::Status::Ok)
        return lifecycle_rejected(lifecycle_status_for_local(io), request);
    io = local::validate_handshake(reply, local::MessageType::HelloAck,
                                   local::PeerRole::Sidecar,
                                   request.identity);
    if (io != local::Status::Ok)
        return lifecycle_rejected(lifecycle_status_for_local(io), request);

    const local::ControlOperation expected =
        local::make_input_lifecycle_operation(request);
    const std::vector<uint8_t> payload = local::encode_control_operation(expected);
    if (payload.empty())
        return lifecycle_rejected(InputLifecycleStatus::InvalidArgument, request);
    local::Frame operation_frame{local::kProtocolVersion,
                                 local::MessageType::Data,
                                 request.identity, payload};
    io = connection.send_until(operation_frame, deadline);
    if (io != local::Status::Ok)
        return lifecycle_rejected(lifecycle_status_for_local(io), request);

    local::Frame response;
    io = connection.receive_until(response, deadline);
    if (io != local::Status::Ok)
        return lifecycle_rejected(lifecycle_status_for_local(io), request);
    if (response.type != local::MessageType::Data ||
        local::validate_identity(response, request.identity) != local::Status::Ok)
        return lifecycle_rejected(InputLifecycleStatus::MalformedResponse, request);
    local::ControlOperation observed;
    if (!local::decode_control_operation(response.payload, observed))
        return lifecycle_rejected(InputLifecycleStatus::MalformedResponse, request);

    // Keep the responder from closing while unread response bytes can still
    // produce POLLIN|POLLHUP.  The sidecar waits for this exact final ACK; a
    // lost ACK is harmless because its replay table already owns the result.
    const local::Frame acknowledgement{local::kProtocolVersion,
                                       local::MessageType::Goodbye,
                                       request.identity, {}};
    (void)connection.send_until(acknowledgement, deadline);

    if (observed.kind != expected.kind || observed.identity != expected.identity ||
        observed.request_id != expected.request_id || observed.input != expected.input ||
        observed.owner != expected.owner ||
        observed.lifecycle_action != expected.lifecycle_action ||
        observed.f_store_generation != expected.f_store_generation ||
        observed.f_store_guid != expected.f_store_guid ||
        observed.immutable_size != expected.immutable_size ||
        observed.immutable_digest != expected.immutable_digest ||
        observed.retirement_id != expected.retirement_id ||
        observed.replacement_owner != expected.replacement_owner ||
        observed.absolute_deadline != expected.absolute_deadline ||
        !observed.lifecycle_result.has_value())
        return lifecycle_rejected(InputLifecycleStatus::MalformedResponse, request);
    return InputLifecycleResult{
        lifecycle_status_for_apply(*observed.lifecycle_result), request};
}

}  // namespace icecc::p50
