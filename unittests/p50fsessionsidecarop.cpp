// Functional-correctness test for the sidecar F-session operation phase
// reducer: offer/accept, public-FD adoption preconditions, the commit-vs-cancel
// race in both owner orders, and the terminal observation/ACK handshake with
// flush-gated retirement and replay (issue-16 5443811178 / 5444123889 /
// 5444410383 sec.5).

#include "cache/p50_fsession_sidecar_op.h"
#include "cache/p50_fsession_payloads.h"

#include <cassert>
#include <cstdio>

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
    r.identity = {3, 4};
    r.store_generation = 19;
    r.store_root.bytes[1] = 9;
    r.c_store_guid = icecc::p50::c_store_guid_for_root(r.store_root);
    r.f_store_guid = icecc::p50::f_store_guid_for_root(r.store_root);
    assert(r.valid());
    return r;
}

FSessionOperationIdentity op_identity(int64_t deadline_ns) {
    const auto inc = launch();
    FSessionOperationIdentity id;
    id.daemon_launch_generation = 21;
    id.control_connection_generation = 22;
    id.operation = {{inc.identity.generation, inc.identity.attempt},
                    icecc::p50::daemon::P50SessionOperationRole::FSession, 1};
    id.c_store_guid = {inc.c_store_guid.bytes};
    id.f_store_guid = {inc.f_store_guid.bytes};
    id.assignment_job = 100;
    id.assignment_epoch = 200;
    id.assignment_nonce = 300;
    id.arm_observation = 400;
    id.deadline = {deadline_ns, 1, 1};
    assert(id.valid());
    return id;
}

icecc::p50::EndpointRunIdentity run_identity() {
    const auto inc = launch();
    icecc::p50::EndpointRunIdentity r;
    r.sidecar_launch = inc;
    r.c_store_guid = inc.c_store_guid;
    r.f_store_guid = inc.f_store_guid;
    r.f_session_operation = {
        {inc.identity.generation, inc.identity.attempt},
        icecc::p50::daemon::P50SessionOperationRole::FSession, 1};
    r.endpoint_generation = 5;
    r.endpoint_session_serial = 6;
    r.run_sequence = 1;
    r.socket_ownership_generation = 7;
    assert(r.valid());
    return r;
}

std::pair<FSessionControlEnvelope, std::vector<uint8_t>>
frame_with_payload(const FSessionOperationIdentity& id, DaemonToSidecarType type,
                   uint64_t seq, std::vector<uint8_t> payload) {
    FSessionControlEnvelope e;
    e.identity = id;
    e.direction = FSessionControlDirection::DaemonToSidecar;
    e.message_type = static_cast<uint16_t>(type);
    e.sequence = seq;
    e.payload = std::move(payload);
    auto enc = encode_fsession_control(e);
    assert(enc.has_value());
    return {e, *enc};
}

std::pair<FSessionControlEnvelope, std::vector<uint8_t>>
frame(const FSessionOperationIdentity& id, DaemonToSidecarType type,
      uint64_t seq) {
    FSessionControlEnvelope e;
    e.identity = id;
    e.direction = FSessionControlDirection::DaemonToSidecar;
    e.message_type = static_cast<uint16_t>(type);
    e.sequence = seq;
    auto enc = encode_fsession_control(e);
    assert(enc.has_value());
    return {e, *enc};
}

RouteSessionLease make_lease(RouteAdmissionOwner& route,
                             const FSessionOperationIdentity& id) {
    RouteRefusal refusal = RouteRefusal::None;
    auto lease = route.reserve(id, run_identity(), RoutePredecessor{}, refusal);
    assert(lease.has_value());
    return std::move(*lease);
}

constexpr int64_t kNow = 1000;
constexpr int64_t kLiveDeadline = 5000;

std::vector<uint8_t> public_fd_offer_body(const FSessionOperationIdentity& id) {
    PublicFdOfferPayload p;
    p.identity = id;
    p.public_fd_offer_id = 0x2A01;
    p.ancillary_attempt_id = 1;
    p.socket_cookie = 0xC00C1E;
    auto body = encode_PublicFdOffer(p);
    assert(body.has_value());
    return *body;
}

// Drive an op to EndpointRunning; returns the next daemon sequence to use.
uint64_t advance_to_running(SidecarFSessionOperation& op,
                            RouteAdmissionOwner& route,
                            const FSessionOperationIdentity& id) {
    auto [offer_e, offer_b] = frame(id, DaemonToSidecarType::OperationOffer, 1);
    check(op.consume_inbound(offer_e, offer_b, kNow) ==
              InboundDisposition::AcceptedNew,
          "offer accepted");
    check(op.phase() == SidecarOpPhase::Accepted, "phase Accepted");
    // Adoption without a consumed PublicFdOffer is refused (no inferred
    // transfer identity) and leaves the route lease unconsumed; the SAME lease
    // then succeeds after the exact offer arrives.
    auto lease = make_lease(route, id);
    check(op.adopt_public_fd(std::move(lease), [] { return true; }, kNow) == 0,
          "adoption refused before the exact PublicFdOffer");
    check(lease.valid(), "refusal left the route lease unconsumed");
    auto [fd_e, fd_b] = frame_with_payload(
        id, DaemonToSidecarType::PublicFdOffer, 2, public_fd_offer_body(id));
    (void)op.consume_inbound(fd_e, fd_b, kNow);
    const uint64_t receipt =
        op.adopt_public_fd(std::move(lease), [] { return true; }, kNow);
    check(receipt != 0, "public-FD receipt staged");
    check(op.endpoint_started(), "endpoint started");
    return 3;
}

void test_precondition_refusals() {
    const auto id = op_identity(kLiveDeadline);
    RouteAdmissionOwner route;

    // Socket validation failure refuses with zero partial authority: the phase
    // stays Accepted and the caller's route lease survives unconsumed.
    SidecarFSessionOperation op;
    auto [offer_e, offer_b] = frame(id, DaemonToSidecarType::OperationOffer, 1);
    (void)op.consume_inbound(offer_e, offer_b, kNow);
    auto lease = make_lease(route, id);
    check(op.adopt_public_fd(std::move(lease), [] { return false; }, kNow) == 0,
          "socket-invalid refuses receipt");
    check(op.phase() == SidecarOpPhase::Accepted, "refusal leaves phase");
    check(lease.valid(), "refusal leaves the route lease unconsumed");

    // Dead deadline refuses adoption.
    check(op.adopt_public_fd(std::move(lease), [] { return true; },
                             kLiveDeadline + 1) == 0,
          "dead deadline refuses receipt");

    // An offer that arrives already past its deadline terminates immediately.
    SidecarFSessionOperation late;
    const auto late_id = op_identity(kNow - 1);
    auto [late_e, late_b] = frame(late_id, DaemonToSidecarType::OperationOffer, 1);
    (void)late.consume_inbound(late_e, late_b, kNow);
    check(late.phase() == SidecarOpPhase::AbortedPreDurable,
          "expired offer -> AbortedPreDurable");
}

void test_cancel_before_commit() {
    const auto id = op_identity(kLiveDeadline);
    RouteAdmissionOwner route;
    SidecarFSessionOperation op;
    uint64_t seq = advance_to_running(op, route, id);

    check(op.prepared_input_ready(), "prepared input ready");
    auto [cancel_e, cancel_b] = frame(id, DaemonToSidecarType::OpCancel, seq);
    (void)op.consume_inbound(cancel_e, cancel_b, kNow);
    check(op.phase() == SidecarOpPhase::AbortedPreDurable,
          "cancel before permit -> AbortedPreDurable");
    check(op.grant_commit_permit_and_commit() == 0,
          "no commit after linearized cancel (exactly one outcome)");

    // Terminal handshake with flush gating and replay.
    const uint64_t term = op.stage_terminal_observation();
    check(term != 0, "terminal observation staged");
    check(op.stage_terminal_observation() == term,
          "terminal replay reuses the same staged frame");
    check(!op.consume_terminal_ack(),
          "ack before observation flush does not retire (anomaly)");
    const auto* slot = op.outbound().find(term);
    check(slot != nullptr, "terminal slot exists");
    check(op.outbound().record_written(term, slot->canonical_bytes.size()),
          "flush terminal observation");
    check(op.consume_terminal_ack(), "ack after flush retires");
    check(op.phase() == SidecarOpPhase::Retired, "row retired");
}

void test_commit_before_cancel() {
    const auto id = op_identity(kLiveDeadline);
    RouteAdmissionOwner route;
    SidecarFSessionOperation op;
    uint64_t seq = advance_to_running(op, route, id);

    check(op.prepared_input_ready(), "prepared input ready");
    check(op.grant_commit_permit_and_commit() != 0, "owner permit -> committed");
    check(op.phase() == SidecarOpPhase::Committed, "phase Committed");

    auto [cancel_e, cancel_b] = frame(id, DaemonToSidecarType::OpCancel, seq);
    (void)op.consume_inbound(cancel_e, cancel_b, kNow);
    check(op.phase() == SidecarOpPhase::CancelledAfterCommit,
          "cancel after commit -> CancelledAfterCommit");
    check(op.delivery_suppressed(), "delivery suppressed, bundle retained");
    check(op.stage_terminal_observation() != 0,
          "terminal observation available after commit+cancel");
}

void test_endpoint_cannot_autonomously_commit() {
    const auto id = op_identity(kLiveDeadline);
    RouteAdmissionOwner route;
    SidecarFSessionOperation op;
    (void)advance_to_running(op, route, id);
    // No PreparedInputReady yet: a commit without the prepared+permit boundary
    // must refuse (the endpoint may not bypass the owner).
    check(op.grant_commit_permit_and_commit() == 0,
          "commit refused outside PreparedAwaitPermit");
}

void test_unacked_terminal_tombstone_survives_control_loss() {
    const auto id = op_identity(kLiveDeadline);
    RouteAdmissionOwner route;
    SidecarFSessionOperation op;
    (void)advance_to_running(op, route, id);
    check(op.prepared_input_ready(), "prepared (tombstone row)");
    check(op.grant_commit_permit_and_commit() != 0, "committed (tombstone row)");
    const uint64_t term = op.stage_terminal_observation();
    check(term != 0, "terminal staged (tombstone row)");
    // Control lost with the ACK never consumed: the observation/tombstone must
    // survive for identical replay -- reconciliation required, and a repeat
    // stage call still returns the same retained sequence.
    op.control_lost();
    check(op.reconcile_required(),
          "unacked TerminalStaged + control loss -> reconciliation required");
    check(op.phase() == SidecarOpPhase::TerminalStaged,
          "observation retained (not erased) after control loss");
    check(op.stage_terminal_observation() == term,
          "replay reuses the retained observation sequence");
}

void test_semantic_ack_and_commit_split() {
    const auto id = op_identity(kLiveDeadline);
    RouteAdmissionOwner route;
    SidecarFSessionOperation op;
    uint64_t seq = advance_to_running(op, route, id);
    check(op.prepared_input_ready(), "prepared (gate rows)");

    // Commit split: selection is sticky; a cancel cannot steal it; a store
    // failure after selection is reconciliation, never AbortedPreDurable.
    check(op.select_commit(), "owner selects commit");
    auto [cancel_e, cancel_b] = frame(id, DaemonToSidecarType::OpCancel, seq++);
    (void)op.consume_inbound(cancel_e, cancel_b, kNow);
    check(op.phase() == SidecarOpPhase::PermitSelected,
          "cancel after selection cannot steal the decision");
    check(op.commit_durable([]() -> std::optional<
                                SidecarFSessionOperation::CommitBundle> {
              return std::nullopt;
          }) == 0,
          "store failure after selection refuses commit");
    check(op.phase() == SidecarOpPhase::PermitSelected &&
              op.reconcile_required(),
          "store failure -> reconciliation, not AbortedPreDurable");
    check(op.commit_durable([]() -> std::optional<
                                SidecarFSessionOperation::CommitBundle> {
              SidecarFSessionOperation::CommitBundle bundle;
              bundle.ready_event_id = 51;
              bundle.backing_id = 52;
              bundle.successor_rel_seq = 11;
              bundle.successor_digest[0] = 0xAB;
              bundle.committed_receipt_id = 53;
              return bundle;
          }) != 0,
          "durable commit succeeds after transient store failure");
    check(op.phase() == SidecarOpPhase::Committed, "committed");

    // Terminal path with the SEMANTIC ack gate: a fresh-sequence TerminalAck
    // naming the wrong observation leaves the operation unsettled.
    const uint64_t term = op.stage_terminal_observation();
    check(term != 0, "terminal staged (gate rows)");
    const auto* slot = op.outbound().find(term);
    check(op.outbound().record_written(term, slot->canonical_bytes.size()),
          "terminal flushed (gate rows)");
    TerminalAckPayload wrong_ack;
    wrong_ack.identity = id;
    wrong_ack.ack_of_sidecar_sequence = term + 7; // wrong observation
    wrong_ack.terminal_class_echo = 1;
    wrong_ack.settlement_id = 5;
    auto wrong_body = encode_TerminalAck(wrong_ack);
    check(wrong_body.has_value(), "encode wrong-ack body");
    auto [wa_e, wa_b] =
        frame_with_payload(id, DaemonToSidecarType::TerminalAck, seq++, *wrong_body);
    (void)op.consume_inbound(wa_e, wa_b, kNow);
    check(op.phase() == SidecarOpPhase::TerminalStaged,
          "fresh-sequence wrong-ack_of leaves the operation unsettled");

    TerminalAckPayload ack = wrong_ack;
    ack.ack_of_sidecar_sequence = term;
    auto ack_body = encode_TerminalAck(ack);
    auto [ak_e, ak_b] =
        frame_with_payload(id, DaemonToSidecarType::TerminalAck, seq++, *ack_body);
    (void)op.consume_inbound(ak_e, ak_b, kNow);
    check(op.phase() == SidecarOpPhase::Retired,
          "exact semantic ack settles and retires");
}

} // namespace

int main() {
    test_precondition_refusals();
    test_cancel_before_commit();
    test_commit_before_cancel();
    test_endpoint_cannot_autonomously_commit();
    test_unacked_terminal_tombstone_survives_control_loss();
    test_semantic_ack_and_commit_split();

    if (g_fail != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS p50fsessionsidecarop\n");
    return 0;
}
