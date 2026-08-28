// Executable regression suite for the eight functional corrections in Root's
// probe of the 38e3ed archive (issue-16 5448233807 + follow-up 5448246071),
// ported to the current chain API. Each control asserts the DEFECT DOES NOT
// REPRODUCE: fixed behavior is the pass condition.

#include "cache/p50_fsession_daemon_op.h"
#include "cache/p50_fsession_payloads.h"
#include "cache/p50_fsession_route.h"
#include "cache/p50_fsession_service_owner.h"
#include "cache/p50_fsession_sidecar_op.h"

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
    r.identity = {11, 13};
    r.store_generation = 19;
    r.store_root.bytes[1] = 9;
    r.c_store_guid = icecc::p50::c_store_guid_for_root(r.store_root);
    r.f_store_guid = icecc::p50::f_store_guid_for_root(r.store_root);
    assert(r.valid());
    return r;
}

FSessionOperationIdentity operation(uint64_t op_seq,
                                    uint64_t control_generation = 22) {
    const auto inc = launch();
    FSessionOperationIdentity id;
    id.daemon_launch_generation = 21;
    id.control_connection_generation = control_generation;
    id.operation = {{inc.identity.generation, inc.identity.attempt},
                    icecc::p50::daemon::P50SessionOperationRole::FSession, op_seq};
    id.c_store_guid = {inc.c_store_guid.bytes};
    id.f_store_guid = {inc.f_store_guid.bytes};
    id.assignment_job = 100 + op_seq;
    id.assignment_epoch = 200;
    id.assignment_nonce = 300 + op_seq;
    id.arm_observation = 400 + op_seq;
    id.deadline = {1'000'000, 1, 1};
    assert(id.valid());
    return id;
}

DaemonWaitLease::Facts lease_facts(const FSessionOperationIdentity& id) {
    DaemonWaitLease::Facts f;
    f.client_connection_generation = 31;
    f.compile_file_lease = 32;
    f.assignment_job = id.assignment_job;
    f.assignment_epoch = id.assignment_epoch;
    f.assignment_nonce = id.assignment_nonce;
    f.arm_observation = id.arm_observation;
    f.wait_reservation = 33;
    f.consumed_claim_capability = 34;
    f.deadline = id.deadline;
    return f;
}

icecc::p50::EndpointRunIdentity endpoint_run(uint64_t run_seq) {
    const auto inc = launch();
    icecc::p50::EndpointRunIdentity r;
    r.sidecar_launch = inc;
    r.c_store_guid = inc.c_store_guid;
    r.f_store_guid = inc.f_store_guid;
    r.f_session_operation = {
        {inc.identity.generation, inc.identity.attempt},
        icecc::p50::daemon::P50SessionOperationRole::FSession, run_seq};
    r.endpoint_generation = 5;
    r.endpoint_session_serial = 6;
    r.run_sequence = run_seq;
    r.socket_ownership_generation = 7;
    assert(r.valid());
    return r;
}

RoutePredecessor established(uint64_t seq, uint8_t tag) {
    RoutePredecessor p;
    p.cold = false;
    p.rel_seq = seq;
    p.state_digest[0] = tag;
    return p;
}

std::vector<uint8_t> encode_frame(const FSessionOperationIdentity& id,
                                  FSessionControlDirection direction,
                                  uint16_t type, uint64_t seq,
                                  std::vector<uint8_t> payload) {
    FSessionControlEnvelope e;
    e.identity = id;
    e.direction = direction;
    e.message_type = type;
    e.sequence = seq;
    e.payload = std::move(payload);
    auto enc = encode_fsession_control(e);
    assert(enc.has_value());
    return *enc;
}

constexpr int64_t kNow = 1000;

// Control 1+2: duplicate binding is exact identity+direction+type+body; the
// duplicate path never bypasses direction legality.
void control_outbound_duplicate_binding() {
    const auto a = operation(1);
    const auto b = operation(2, 23);
    FSessionOutboundControl out(4);
    const std::vector<uint8_t> payload{4};
    const uint64_t first = out.stage_frame(
        a, FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation), payload);
    check(first != 0, "control1 setup: first frame stages");
    const uint64_t wrong_identity = out.stage_frame(
        b, FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation), payload);
    check(wrong_identity != 0 && wrong_identity != first,
          "control1: a different operation identity NEVER reuses a sequence");
    const uint64_t wrong_direction = out.stage_frame(
        a, FSessionControlDirection::DaemonToSidecar,
        static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation), payload);
    check(wrong_direction == 0,
          "control2: direction legality gates the duplicate path");
}

// Control 3: a direction-legal but phase-invalid frame does not advance the
// inbound frontier.
void control_phase_invalid_frontier() {
    const auto id = operation(3);
    SidecarFSessionOperation sidecar;
    const auto offer_body = [&] {
        OperationOfferPayload p;
        p.identity = id;
        p.client_connection_generation = 31;
        p.compile_file_lease = 32;
        p.wait_reservation = 33;
        p.consumed_claim_capability = 34;
        p.predecessor = RoutePredecessor{};
        auto body = encode_OperationOffer(p);
        assert(body.has_value());
        return *body;
    }();
    const auto offer = encode_frame(
        id, FSessionControlDirection::DaemonToSidecar,
        static_cast<uint16_t>(DaemonToSidecarType::OperationOffer), 1, offer_body);
    const auto offer_env = decode_fsession_control(offer);
    (void)sidecar.consume_inbound(*offer_env, offer, kNow);
    check(sidecar.phase() == SidecarOpPhase::Accepted, "control3 setup");

    const auto early = encode_frame(
        id, FSessionControlDirection::DaemonToSidecar,
        static_cast<uint16_t>(DaemonToSidecarType::DaemonFdAccepted), 2, {4});
    const auto early_env = decode_fsession_control(early);
    const auto disposition = sidecar.consume_inbound(*early_env, early, kNow);
    check(disposition == InboundDisposition::PhaseInvalidNoRow &&
              sidecar.inbound().next_expected() == 2,
          "control3: phase-invalid frame refused without frontier advance");
}

// Control 4: terminal settlement requires ack capacity FIRST.
void control_settlement_requires_ack_capacity() {
    const auto id = operation(4);
    auto daemon =
        DaemonFSessionOperation::mint(id, DaemonWaitLease(lease_facts(id)),
                                      RoutePredecessor{}, 1); // one slot: offer
    check(daemon.has_value(), "control4 setup: mint (slots=1)");
    TerminalObservationPayload obs;
    obs.identity = id;
    obs.terminal_class = 1;
    obs.highest_accepted_daemon_sequence = 1;
    auto body = encode_TerminalObservation(obs);
    const auto frame_bytes = encode_frame(
        id, FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation), 1, *body);
    const auto env = decode_fsession_control(frame_bytes);
    (void)daemon->consume_inbound(*env, frame_bytes);
    check(daemon->phase() != DaemonOpPhase::Settled &&
              daemon->settlement_count() == 0,
          "control4: no settlement without a stageable TerminalAck");
}

// Control 5: delivery acceptance requires response capacity FIRST.
void control_delivery_requires_response_capacity() {
    const auto id = operation(5);
    auto daemon = DaemonFSessionOperation::mint(
        id, DaemonWaitLease(lease_facts(id)), RoutePredecessor{}, 2);
    check(daemon.has_value(), "control5 setup: mint (slots=2)");
    // slot 1 = offer; accept peer; slot 2 = public-fd offer -> table full.
    OperationAcceptedPayload acc;
    acc.identity = id;
    acc.ack_of_daemon_sequence = 1;
    acc.sidecar_store_generation = 19;
    auto acc_body = encode_OperationAccepted(acc);
    const auto acc_frame = encode_frame(
        id, FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::OperationAccepted), 1,
        *acc_body);
    (void)daemon->consume_inbound(*decode_fsession_control(acc_frame), acc_frame);
    check(daemon->offer_public_fd(0xC00C1E) != 0, "control5 setup: offer");
    PublicFdAdoptedReceiptPayload receipt;
    receipt.identity = id;
    receipt.public_fd_offer_id = (id.operation.operation_sequence << 8) | 1;
    receipt.endpoint_generation = 5;
    receipt.endpoint_session_serial = 6;
    receipt.run_sequence = 5;
    receipt.socket_ownership_generation = 7;
    receipt.route_admission_sequence = 1;
    receipt.sidecar_owner_sequence = 1;
    auto receipt_body = encode_PublicFdAdoptedReceipt(receipt);
    const auto receipt_frame = encode_frame(
        id, FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::PublicFdAdoptedReceipt), 2,
        *receipt_body);
    (void)daemon->consume_inbound(*decode_fsession_control(receipt_frame),
                                  receipt_frame);
    check(daemon->phase() == DaemonOpPhase::FdAdoptedObserved,
          "control5 setup: adopted");
    DeliveryOfferPayload offer;
    offer.identity = id;
    offer.attachment_delivery_id = 77;
    offer.attachment_admission_id = 1;
    offer.ready_event_id = 1;
    offer.ancillary_attempt_id = 1;
    auto offer_body = encode_DeliveryOffer(offer);
    check(offer_body.has_value(), "control5 setup: delivery offer encodes");
    const auto offer_frame = encode_frame(
        id, FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::DeliveryOffer), 3,
        *offer_body);
    (void)daemon->consume_inbound(*decode_fsession_control(offer_frame),
                                  offer_frame);
    const auto result = daemon->accept_delivery(77, true);
    check(!result.has_value() && daemon->tocompile_transitions() == 0 &&
              daemon->phase() == DaemonOpPhase::FdAdoptedObserved,
          "control5: no TOCOMPILE transition without a response slot");
}

// Control 6: after retirement, the exact lost-ACK observation replay is still
// recognized (ExactReplay), never stale.
void control_retirement_recognizes_replay() {
    const auto id = operation(6);
    auto daemon = DaemonFSessionOperation::mint(
        id, DaemonWaitLease(lease_facts(id)), RoutePredecessor{}, 4);
    check(daemon.has_value(), "control6 setup: mint");
    TerminalObservationPayload obs;
    obs.identity = id;
    obs.terminal_class = 1;
    obs.highest_accepted_daemon_sequence = 1;
    auto body = encode_TerminalObservation(obs);
    const auto frame_bytes = encode_frame(
        id, FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation), 1, *body);
    const auto env = decode_fsession_control(frame_bytes);
    (void)daemon->consume_inbound(*env, frame_bytes);
    check(daemon->phase() == DaemonOpPhase::Settled, "control6 setup: settled");
    // Flush the ack, retire, then replay the exact observation.
    bool flushed = false;
    for (uint64_t seq : daemon->outbound().pending_sequences()) {
        const auto* slot = daemon->outbound().find(seq);
        if (slot->message_type ==
            static_cast<uint16_t>(DaemonToSidecarType::TerminalAck))
            flushed = daemon->outbound().record_written(
                seq, slot->canonical_bytes.size());
    }
    check(flushed && daemon->retire(), "control6 setup: retired");
    const auto replay = daemon->consume_inbound(*env, frame_bytes);
    check(replay == InboundDisposition::ExactReplay,
          "control6: lost-ACK observation replay recognized after retirement");
}

// Control 7: the route ABA fence holds at full depth (A cannot reopen after
// any number of intervening settled operations).
void control_route_aba_full_depth() {
    RouteAdmissionOwner route;
    RouteRefusal refusal = RouteRefusal::None;
    const auto a = operation(10);
    const auto b = operation(11);
    auto lease_a = route.reserve(a, endpoint_run(10), RoutePredecessor{}, refusal);
    check(lease_a.has_value() && route.activate(*lease_a) &&
              route.commit(*lease_a, established(1, 0xA1)) &&
              route.settle_and_release(*lease_a),
          "control7 setup: A settles");
    auto lease_b = route.reserve(b, endpoint_run(11), established(1, 0xA1), refusal);
    check(lease_b.has_value() && route.activate(*lease_b) &&
              route.commit(*lease_b, established(2, 0xB2)) &&
              route.settle_and_release(*lease_b),
          "control7 setup: B settles");
    check(!route.reserve(a, endpoint_run(10), established(2, 0xB2), refusal)
               .has_value() &&
              refusal == RouteRefusal::DuplicateOperation,
          "control7: settled A cannot reopen after an intervening operation");
}

// Control 8: the writer drains the actual pending sequences (1025 included).
void control_writer_no_sequence_ceiling() {
    FSessionServiceOwner owner(1);
    check(owner.open_admission(mint_fsession_admission_ready(
              owner.service_generation(), 1)),
          "control8 setup: admission");
    const uint64_t connection = owner.connection_opened();
    check(connection != 0, "control8 setup: connection");
    auto& out = owner.operation(connection)->outbound();
    const std::vector<uint8_t> byte{0xAA};
    uint64_t seq = 0;
    for (size_t i = 0; i < 1025; ++i) {
        seq = out.reserve(1);
        check(seq != 0, "control8 setup: reserve");
        check(out.stage(seq, byte), "control8 setup: stage");
        if (i != 1024)
            check(out.retire(seq), "control8 setup: retire");
    }
    check(seq == 1025, "control8 setup: reached sequence 1025");
    size_t written = 0;
    check(owner.drain_outbound(
              connection,
              [&written](std::span<const uint8_t> bytes) -> long {
                  written += bytes.size();
                  return static_cast<long>(bytes.size());
              },
              16),
          "control8: drain succeeds");
    check(written == 1 &&
              out.find(seq)->state == OutboundSlotState::FullyFlushed,
          "control8: sequence 1025 drains (no fixed ceiling)");
}

} // namespace

int main() {
    control_outbound_duplicate_binding();
    control_phase_invalid_frontier();
    control_settlement_requires_ack_capacity();
    control_delivery_requires_response_capacity();
    control_retirement_recognizes_replay();
    control_route_aba_full_depth();
    control_writer_no_sequence_ceiling();

    if (g_fail != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS p50fsessionsemantics\n");
    return 0;
}
