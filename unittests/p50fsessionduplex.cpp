// Dual-reducer integration test: DaemonFSessionOperation and
// SidecarFSessionOperation joined through the real codec bytes -- each side's
// staged-and-flushed outbound frames are decoded and consumed by the peer,
// exactly as the dedicated control connection will carry them. Proves the two
// process-local halves realize ONE logical distributed operation
// (issue-16 5443811178): the complete settlement path, and the commit-vs-
// cancel race in both owner orders, end to end.

#include "cache/p50_fsession_daemon_op.h"
#include "cache/p50_fsession_route.h"
#include "cache/p50_fsession_sidecar_op.h"

#include <cassert>
#include <cstdio>
#include <deque>

using namespace icecc::p50::fsession;

namespace {
int g_fail = 0;
void check(bool c, const char* m) {
    if (!c) {
        std::fprintf(stderr, "FAIL: %s\n", m);
        ++g_fail;
    }
}

icecc::p50::SidecarLaunchIdentity launch() {
    icecc::p50::SidecarLaunchIdentity r;
    r.identity = {11, 13};
    r.store_generation = 19;
    r.store_root.bytes[1] = 9;
    r.c_store_guid = icecc::p50::c_store_guid_for_root(r.store_root);
    r.f_store_guid = icecc::p50::f_store_guid_for_root(r.store_root);
    assert(r.valid());
    return r;
}

FSessionOperationIdentity op_identity() {
    const auto inc = launch();
    FSessionOperationIdentity id;
    id.daemon_launch_generation = 21;
    id.control_connection_generation = 22;
    id.operation = {{inc.identity.generation, inc.identity.attempt},
                    icecc::p50::daemon::P50SessionOperationRole::FSession, 42};
    id.c_store_guid = {inc.c_store_guid.bytes};
    id.f_store_guid = {inc.f_store_guid.bytes};
    id.assignment_job = 100;
    id.assignment_epoch = 200;
    id.assignment_nonce = 300;
    id.arm_observation = 400;
    id.deadline = {1'000'000, 1, 1};
    assert(id.valid());
    return id;
}

DaemonWaitLease::Facts lease_facts() {
    DaemonWaitLease::Facts f;
    f.client_connection_generation = 31;
    f.compile_file_lease = 32;
    f.assignment_job = 100;
    f.assignment_epoch = 200;
    f.assignment_nonce = 300;
    f.arm_observation = 400;
    f.wait_reservation = 33;
    f.consumed_claim_capability = 34;
    f.deadline = {1'000'000, 1, 1};
    return f;
}

icecc::p50::EndpointRunIdentity run_identity() {
    const auto inc = launch();
    icecc::p50::EndpointRunIdentity r;
    r.sidecar_launch = inc;
    r.c_store_guid = inc.c_store_guid;
    r.f_store_guid = inc.f_store_guid;
    r.f_session_operation = {
        {inc.identity.generation, inc.identity.attempt},
        icecc::p50::daemon::P50SessionOperationRole::FSession, 42};
    r.endpoint_generation = 5;
    r.endpoint_session_serial = 6;
    r.run_sequence = 42;
    r.socket_ownership_generation = 7;
    assert(r.valid());
    return r;
}

constexpr int64_t kNow = 1000;

// One direction of the simulated dedicated control connection: pump every
// staged frame in `from` through full flush, decode the canonical bytes, and
// deliver them to `consume`. Returns the number of frames delivered.
template <typename Consume>
size_t pump(FSessionOutboundControl& from, Consume&& consume) {
    size_t delivered = 0;
    for (uint64_t seq = 1; seq <= 64; ++seq) {
        const OutboundSemanticSlot* slot = from.find(seq);
        if (slot == nullptr)
            continue;
        if (slot->state == OutboundSlotState::Queued ||
            slot->state == OutboundSlotState::Writing) {
            if (!from.record_written(seq, slot->canonical_bytes.size() -
                                              slot->write_offset))
                continue;
        }
        slot = from.find(seq);
        if (slot->state != OutboundSlotState::FullyFlushed)
            continue;
        const auto decoded = decode_fsession_control(slot->canonical_bytes);
        check(decoded.has_value(), "pumped frame decodes");
        if (decoded.has_value()) {
            consume(*decoded, slot->canonical_bytes);
            ++delivered;
        }
    }
    return delivered;
}

struct Duplex {
    FSessionOperationIdentity id = op_identity();
    RouteAdmissionOwner route;
    std::optional<DaemonFSessionOperation> daemon;
    SidecarFSessionOperation sidecar;

    Duplex() {
        daemon = DaemonFSessionOperation::mint(id, DaemonWaitLease(lease_facts()));
        assert(daemon.has_value());
    }

    // Deliver every flushed daemon frame to the sidecar and vice versa until
    // quiescent (bounded rounds; the protocol is finite).
    void settle_wire() {
        for (int round = 0; round < 8; ++round) {
            size_t moved = 0;
            moved += pump(daemon->outbound(),
                          [&](const FSessionControlEnvelope& e,
                              const std::vector<uint8_t>& b) {
                              (void)sidecar.consume_inbound(e, b, kNow);
                          });
            moved += pump(sidecar.outbound(),
                          [&](const FSessionControlEnvelope& e,
                              const std::vector<uint8_t>& b) {
                              (void)daemon->consume_inbound(e, b);
                          });
            if (moved == 0)
                return;
        }
    }
};

void stage_delivery_offer(Duplex& d, uint64_t delivery_id) {
    DeliveryOfferPayload offer;
    offer.identity = d.id;
    offer.attachment_delivery_id = delivery_id;
    offer.attachment_admission_id = 1;
    offer.ready_event_id = 1;
    offer.ancillary_attempt_id = 1;
    const auto body = encode_DeliveryOffer(offer);
    check(body.has_value(), "delivery offer encodes");
    if (!body.has_value())
        return;
    const uint64_t sequence = d.sidecar.outbound().stage_frame(
        d.id, FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::DeliveryOffer), *body);
    check(sequence != 0, "delivery offer staged");
    d.settle_wire();
}
} // namespace

static void test_full_settlement_path() {
    Duplex d;
    d.settle_wire(); // offer -> accepted (both directions quiesce)
    check(d.sidecar.phase() == SidecarOpPhase::Accepted, "sidecar accepted");
    check(d.daemon->phase() == DaemonOpPhase::AcceptedByPeer, "daemon sees accept");

    check(d.daemon->offer_public_fd(0xC00C1E) != 0, "daemon offers public fd");
    d.settle_wire();

    RouteRefusal refusal = RouteRefusal::None;
    auto lease = d.route.reserve(d.id, run_identity(), RoutePredecessor{}, refusal);
    check(lease.has_value(), "route lease reserved");
    check(d.sidecar.adopt_public_fd(std::move(*lease), [] { return true; },
                                    kNow) != 0,
          "sidecar adopts public fd");
    d.settle_wire();
    check(d.daemon->phase() == DaemonOpPhase::FdAdoptedObserved,
          "daemon consumed the exact adopted receipt");

    check(d.sidecar.endpoint_started(), "endpoint started");
    check(d.sidecar.prepared_input_ready(), "prepared input ready");
    check(d.sidecar.grant_commit_permit_and_commit() != 0, "owner commits");
    d.settle_wire();

    stage_delivery_offer(d, 77);
    auto accept = d.daemon->accept_delivery(77, true);
    check(accept.has_value() && !accept->replay, "daemon accepts delivery");
    check(d.daemon->tocompile_transitions() == 1, "one TOCOMPILE transition");
    d.settle_wire();

    check(d.sidecar.stage_terminal_observation() != 0, "terminal staged");
    d.settle_wire(); // observation -> daemon settles -> ack -> sidecar retires
    check(d.daemon->settlement_count() == 1, "daemon settled once");
    check(d.sidecar.phase() == SidecarOpPhase::Retired, "sidecar retired");
    check(d.daemon->retire(), "daemon retires after flushed ack");
}

static void test_cancel_before_commit_duplex() {
    Duplex d;
    d.settle_wire();
    check(d.daemon->offer_public_fd(0xC00C1E) != 0, "offer");
    d.settle_wire();
    RouteRefusal refusal = RouteRefusal::None;
    auto lease = d.route.reserve(d.id, run_identity(), RoutePredecessor{}, refusal);
    check(d.sidecar.adopt_public_fd(std::move(*lease), [] { return true; },
                                    kNow) != 0,
          "adopt");
    d.settle_wire();
    check(d.sidecar.endpoint_started() && d.sidecar.prepared_input_ready(),
          "prepared");

    // Daemon cancels; the cancel frame reaches the sidecar BEFORE any permit.
    check(d.daemon->request_cancel() != 0, "daemon stages cancel");
    d.settle_wire();
    check(d.sidecar.phase() == SidecarOpPhase::AbortedPreDurable,
          "cancel-before-permit -> AbortedPreDurable");
    check(d.sidecar.grant_commit_permit_and_commit() == 0,
          "no commit after linearized cancel");

    check(d.sidecar.stage_terminal_observation() != 0, "terminal staged");
    d.settle_wire();
    check(d.daemon->settlement_count() == 1, "daemon settled once");
    check(d.sidecar.phase() == SidecarOpPhase::Retired, "sidecar retired");
}

static void test_commit_before_cancel_duplex() {
    Duplex d;
    d.settle_wire();
    check(d.daemon->offer_public_fd(0xC00C1E) != 0, "offer");
    d.settle_wire();
    RouteRefusal refusal = RouteRefusal::None;
    auto lease = d.route.reserve(d.id, run_identity(), RoutePredecessor{}, refusal);
    check(d.sidecar.adopt_public_fd(std::move(*lease), [] { return true; },
                                    kNow) != 0,
          "adopt");
    d.settle_wire();
    check(d.sidecar.endpoint_started() && d.sidecar.prepared_input_ready(),
          "prepared");

    // Commit linearizes on the sidecar owner BEFORE the cancel frame arrives.
    check(d.sidecar.grant_commit_permit_and_commit() != 0, "commit wins");
    check(d.daemon->request_cancel() != 0, "daemon cancel staged late");
    d.settle_wire();
    check(d.sidecar.phase() == SidecarOpPhase::CancelledAfterCommit,
          "cancel-after-commit retains the bundle");
    check(d.sidecar.delivery_suppressed(), "delivery suppressed");

    check(d.sidecar.stage_terminal_observation() != 0, "terminal staged");
    d.settle_wire();
    check(d.daemon->settlement_count() == 1, "settled once");
}

int main() {
    test_full_settlement_path();
    test_cancel_before_commit_duplex();
    test_commit_before_cancel_duplex();

    if (g_fail != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS p50fsessionduplex\n");
    return 0;
}
