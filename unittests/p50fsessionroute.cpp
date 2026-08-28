// Boundary-validation / regression test for F-session route admission
// (issue-16 5443966435 + tagged cold-route rule 5444410383 sec.7).
//
// Unit-level rows covered (socket-level rows land with the endpoint wiring):
//  - B refused while A is live (RouteBusy), including Committed-unsettled and
//    ReconcileRequired.
//  - a duplicate reservation for the same operation cannot create a second run.
//  - a legal cold route (tagged REL_SEQ 0 / zero digest) is admissible.
//  - a wrong predecessor is refused; after settlement the next TU must present
//    the exact committed successor.
//  - late events naming the old route-owner generation are stale-only.

#include "cache/p50_fsession_route.h"

#include <cassert>
#include <cstdio>

using namespace icecc::p50::fsession;
using icecc::p50::EndpointRunIdentity;
using icecc::p50::SidecarLaunchIdentity;

namespace {
int g_fail = 0;
void check(bool c, const char* m) {
    if (!c) {
        std::fprintf(stderr, "FAIL: %s\n", m);
        ++g_fail;
    }
}

SidecarLaunchIdentity launch(uint64_t generation, uint64_t attempt,
                             uint64_t root_byte) {
    SidecarLaunchIdentity result;
    result.identity = {generation, attempt};
    result.store_generation = root_byte + 10;
    result.store_root.bytes[1] = static_cast<uint8_t>(root_byte);
    result.c_store_guid = icecc::p50::c_store_guid_for_root(result.store_root);
    result.f_store_guid = icecc::p50::f_store_guid_for_root(result.store_root);
    assert(result.valid());
    return result;
}

EndpointRunIdentity run_identity(const SidecarLaunchIdentity& incarnation,
                                 uint64_t run) {
    EndpointRunIdentity result;
    result.sidecar_launch = incarnation;
    result.c_store_guid = incarnation.c_store_guid;
    result.f_store_guid = incarnation.f_store_guid;
    result.f_session_operation = {
        {incarnation.identity.generation, incarnation.identity.attempt},
        icecc::p50::daemon::P50SessionOperationRole::FSession, run};
    result.endpoint_generation = 5;
    result.endpoint_session_serial = 6;
    result.run_sequence = run;
    result.socket_ownership_generation = 7;
    assert(result.valid());
    return result;
}

FSessionOperationIdentity operation_identity(const SidecarLaunchIdentity& inc,
                                             uint64_t op_seq) {
    FSessionOperationIdentity id;
    id.daemon_launch_generation = 21;
    id.control_connection_generation = 22;
    id.operation = {{inc.identity.generation, inc.identity.attempt},
                    icecc::p50::daemon::P50SessionOperationRole::FSession,
                    op_seq};
    id.c_store_guid = {inc.c_store_guid.bytes};
    id.f_store_guid = {inc.f_store_guid.bytes};
    id.assignment_job = 100 + op_seq;
    id.assignment_epoch = 200;
    id.assignment_nonce = 300;
    id.arm_observation = 400;
    id.deadline = {123456789, 1, 1};
    assert(id.valid());
    return id;
}

RoutePredecessor cold() { return RoutePredecessor{}; }

RoutePredecessor established(uint64_t rel_seq, uint8_t tag) {
    RoutePredecessor p;
    p.cold = false;
    p.rel_seq = rel_seq;
    p.state_digest[0] = tag;
    return p;
}
} // namespace

int main() {
    const auto inc = launch(3, 4, 9);
    const auto op_a = operation_identity(inc, 1);
    const auto op_b = operation_identity(inc, 2);
    const auto run_a = run_identity(inc, 1);
    const auto run_b = run_identity(inc, 2);

    RouteAdmissionOwner route;
    RouteRefusal refusal = RouteRefusal::None;

    // Cold route: tagged REL_SEQ 0 / zero digest is legal.
    auto lease_a = route.reserve(op_a, run_a, cold(), refusal);
    check(lease_a.has_value() && lease_a->valid(),
          "cold-route reservation admissible (tagged predecessor)");

    // Exclusion: B refused while A is Reserved; duplicate A refused too.
    check(!route.reserve(op_b, run_b, cold(), refusal).has_value() &&
              refusal == RouteRefusal::RouteBusy,
          "B refused while A reserved (RouteBusy)");
    check(!route.reserve(op_a, run_a, cold(), refusal).has_value() &&
              refusal == RouteRefusal::DuplicateOperation,
          "duplicate A cannot create a second run");

    // Active + committed-unsettled still exclude B.
    check(route.activate(*lease_a), "A activates");
    check(!route.reserve(op_b, run_b, cold(), refusal).has_value(),
          "B refused while A active");
    check(!route.commit(*lease_a, cold()), "cold successor is not a legal commit");
    check(route.commit(*lease_a, established(11, 0xAB)), "A commits successor");
    check(!route.reserve(op_b, run_b, cold(), refusal).has_value() &&
              refusal == RouteRefusal::RouteBusy,
          "B refused while A committed-unsettled");

    // Settlement releases the route; generation advances -> old events stale.
    const uint64_t gen_a = lease_a->route_owner_generation();
    check(route.settle_and_release(*lease_a), "A settles and releases");
    check(!route.event_current(gen_a, op_a),
          "late A event names old generation -> stale-only");

    // Next TU must present the exact committed successor.
    check(!route.reserve(op_b, run_b, cold(), refusal).has_value() &&
              refusal == RouteRefusal::WrongPredecessor,
          "cold predecessor refused after commit (cursor advanced)");
    check(!route.reserve(op_b, run_b, established(11, 0xCD), refusal).has_value() &&
              refusal == RouteRefusal::WrongPredecessor,
          "wrong-digest predecessor refused");
    // Sequential-ABA fence: settled A can never re-reserve, even presenting
    // the exact committed successor (a consumed rendezvous never reopens).
    check(!route.reserve(op_a, run_a, established(11, 0xAB), refusal)
               .has_value() &&
              refusal == RouteRefusal::DuplicateOperation,
          "settled A cannot re-reserve (sequential-ABA fence)");

    auto lease_b = route.reserve(op_b, run_b, established(11, 0xAB), refusal);
    check(lease_b.has_value(), "B admits with the exact committed successor");

    // ReconcileRequired keeps the route inadmissible until resolved.
    check(route.activate(*lease_b), "B activates");
    check(route.mark_reconcile_required(*lease_b), "B enters ReconcileRequired");
    const auto op_c = operation_identity(inc, 3);
    check(!route.reserve(op_c, run_identity(inc, 3), established(11, 0xAB),
                         refusal)
               .has_value() &&
              refusal == RouteRefusal::RouteBusy,
          "next-TU C inadmissible while B is ReconcileRequired");
    const uint64_t gen_b = lease_b->route_owner_generation();
    route.resolve_and_release();
    check(!route.event_current(gen_b, op_b),
          "late B event stale after resolve (generation advanced)");
    auto lease_c = route.reserve(op_c, run_identity(inc, 3),
                                 established(11, 0xAB), refusal);
    check(lease_c.has_value(), "C admits after reconcile resolution");

    // A moved-from lease loses authority.
    RouteSessionLease moved = std::move(*lease_c);
    check(moved.valid() && !lease_c->valid(), "lease authority is move-only");

    if (g_fail != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS p50fsessionroute\n");
    return 0;
}
