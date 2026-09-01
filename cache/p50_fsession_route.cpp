#include "p50_fsession_route.h"

namespace icecc::p50::fsession {

std::optional<RouteSessionLease>
RouteAdmissionOwner::reserve(const FSessionOperationIdentity& operation,
                             const EndpointRunIdentity& endpoint_run,
                             const RoutePredecessor& predecessor,
                             RouteRefusal& refusal) {
    refusal = RouteRefusal::None;
    if (!operation.valid() || !endpoint_run.valid() || !predecessor.valid()) {
        refusal = RouteRefusal::InvalidRequest;
        return std::nullopt;
    }
    if (state_ != RouteAdmissionState::Idle) {
        // A live, committed-unsettled, or reconcile-required operation excludes
        // every other operation. A duplicate reservation for the SAME operation
        // cannot create a second run either (5443966435 sec.4).
        refusal = (live_operation_ && *live_operation_ == operation)
                      ? RouteRefusal::DuplicateOperation
                      : RouteRefusal::RouteBusy;
        return std::nullopt;
    }
    // Sequential-ABA fence: every settled/resolved operation of the current
    // sidecar launch is terminal for this route -- any operation at or below
    // the monotonic retirement frontier never re-reserves, regardless of how
    // many operations intervened.
    if (operation.operation.sidecar_launch == retired_launch_ &&
        operation.operation.operation_sequence <= retired_frontier_sequence_) {
        refusal = RouteRefusal::DuplicateOperation;
        return std::nullopt;
    }
    // The predecessor must match the route cursor exactly: tagged-cold on a
    // fresh route, or the committed successor after settlement.
    if (!(predecessor == cursor_)) {
        refusal = RouteRefusal::WrongPredecessor;
        return std::nullopt;
    }

    RouteSessionLease lease;
    lease.operation_ = operation;
    lease.endpoint_run_ = endpoint_run;
    lease.predecessor_ = predecessor;
    lease.route_owner_generation_ = generation_;
    lease.admission_sequence_ = next_admission_sequence_++;
    lease.valid_ = true;

    live_operation_ = operation;
    state_ = RouteAdmissionState::Reserved;
    return lease;
}

bool RouteAdmissionOwner::lease_current(
    const RouteSessionLease& lease) const noexcept {
    return lease.valid() && lease.route_owner_generation() == generation_ &&
           live_operation_ && *live_operation_ == lease.operation();
}

bool RouteAdmissionOwner::activate(const RouteSessionLease& lease) {
    if (state_ != RouteAdmissionState::Reserved || !lease_current(lease))
        return false;
    state_ = RouteAdmissionState::Active;
    return true;
}

bool RouteAdmissionOwner::commit(const RouteSessionLease& lease,
                                 const RoutePredecessor& successor) {
    // The committed successor names established route state; a cold/zero
    // successor is not a legal commit result.
    if (state_ != RouteAdmissionState::Active || !lease_current(lease) ||
        !successor.valid() || successor.cold)
        return false;
    cursor_ = successor;
    state_ = RouteAdmissionState::Committed;
    return true;
}

bool RouteAdmissionOwner::settle_and_release(const RouteSessionLease& lease) {
    if (state_ != RouteAdmissionState::Committed || !lease_current(lease))
        return false;
    // Release: the committed successor stays as the route cursor; the owner
    // generation advances so every late event naming the old generation is
    // stale-only (5443966435 sec.6). The settled operation raises the
    // monotonic retirement frontier (sequential-ABA fence at full depth).
    if (live_operation_) {
        if (!(live_operation_->operation.sidecar_launch == retired_launch_))
            retired_frontier_sequence_ = 0;
        retired_launch_ = live_operation_->operation.sidecar_launch;
        if (live_operation_->operation.operation_sequence >
            retired_frontier_sequence_)
            retired_frontier_sequence_ =
                live_operation_->operation.operation_sequence;
    }
    live_operation_.reset();
    state_ = RouteAdmissionState::Idle;
    ++generation_;
    return true;
}

bool RouteAdmissionOwner::mark_reconcile_required(const RouteSessionLease& lease) {
    if (state_ == RouteAdmissionState::Idle || !lease_current(lease))
        return false;
    state_ = RouteAdmissionState::ReconcileRequired;
    return true;
}

bool RouteAdmissionOwner::mark_reset_required(const RouteSessionLease& lease) {
    // A touched, pre-commit endpoint has an invalidated route context, not an
    // uncertain durable outcome.  Keep this transition distinct from
    // ReconcileRequired, which is reserved for post-commit uncertainty.
    if (state_ != RouteAdmissionState::Active || !lease_current(lease))
        return false;
    state_ = RouteAdmissionState::ResetRequired;
    return true;
}

void RouteAdmissionOwner::resolve_and_release() noexcept {
    if (state_ != RouteAdmissionState::ReconcileRequired &&
        state_ != RouteAdmissionState::ResetRequired)
        return;
    if (live_operation_) {
        if (!(live_operation_->operation.sidecar_launch == retired_launch_))
            retired_frontier_sequence_ = 0;
        retired_launch_ = live_operation_->operation.sidecar_launch;
        if (live_operation_->operation.operation_sequence >
            retired_frontier_sequence_)
            retired_frontier_sequence_ =
                live_operation_->operation.operation_sequence;
    }
    live_operation_.reset();
    state_ = RouteAdmissionState::Idle;
    ++generation_;
}

bool RouteAdmissionOwner::event_current(
    uint64_t route_owner_generation,
    const FSessionOperationIdentity& operation) const noexcept {
    return route_owner_generation == generation_ && live_operation_ &&
           *live_operation_ == operation;
}

} // namespace icecc::p50::fsession
