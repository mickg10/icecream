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
    // Sequential-ABA fence: a settled/resolved operation is terminal for this
    // route; its exact identity never re-reserves, even with a matching cursor.
    if (retired_operation_ && *retired_operation_ == operation) {
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
    // stale-only (5443966435 sec.6). The settled operation is retired for this
    // route (sequential-ABA fence).
    retired_operation_ = live_operation_;
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

void RouteAdmissionOwner::resolve_and_release() noexcept {
    if (state_ != RouteAdmissionState::ReconcileRequired &&
        state_ != RouteAdmissionState::ResetRequired)
        return;
    retired_operation_ = live_operation_;
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
