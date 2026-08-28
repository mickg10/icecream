// Focused unit test for the FSession control envelope/codec (S2 vertical
// foundation). Proves exact round-trip, direction legality as a type property,
// full-identity + sequence + no-trailing-byte validation (issue-16 5444410383).

#include "cache/p50_fsession_control.h"

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

FSessionOperationIdentity make_identity() {
    FSessionOperationIdentity id;
    id.daemon_launch_generation = 7;
    id.control_connection_generation = 3;
    id.operation.sidecar_launch.generation = 11;
    id.operation.sidecar_launch.attempt = 13;
    id.operation.role = icecc::p50::daemon::P50SessionOperationRole::FSession;
    id.operation.operation_sequence = 42;
    id.c_store_guid.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    id.f_store_guid.bytes = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    id.assignment_job = 100;
    id.assignment_epoch = 200;
    id.assignment_nonce = 300;
    id.arm_observation = 400;
    id.deadline.expires_at_ns = 123456789;
    id.deadline.clock_domain_id = 5;
    id.deadline.time_namespace_id = 9;
    return id;
}

// Build an encoded Daemon->Sidecar frame for the inbound sequencer tests.
std::pair<FSessionControlEnvelope, std::optional<std::vector<uint8_t>>>
daemon_frame(const FSessionOperationIdentity& id, DaemonToSidecarType type,
             uint64_t seq, std::vector<uint8_t> payload) {
    FSessionControlEnvelope e;
    e.identity = id;
    e.direction = FSessionControlDirection::DaemonToSidecar;
    e.message_type = static_cast<uint16_t>(type);
    e.sequence = seq;
    e.payload = std::move(payload);
    return {e, encode_fsession_control(e)};
}

void test_inbound_sequencer() {
    const auto id = make_identity();
    FSessionInboundControl in;

    // Blocker 1: an OpCancel before any accepted OperationOffer mints no row.
    auto [cancel_e, cancel_b] = daemon_frame(id, DaemonToSidecarType::OpCancel, 1, {});
    check(cancel_b.has_value(), "encode early cancel");
    check(in.classify(cancel_e, *cancel_b) == InboundDisposition::PhaseInvalidNoRow,
          "early OpCancel -> PhaseInvalidNoRow");
    check(!in.row_created(), "no row created by early cancel");

    // OperationOffer(seq==1) is the sole row-creation frame.
    auto [offer_e, offer_b] =
        daemon_frame(id, DaemonToSidecarType::OperationOffer, 1, {1, 2, 3});
    check(offer_b.has_value(), "encode offer");
    check(in.classify(offer_e, *offer_b) == InboundDisposition::AcceptedNew,
          "OperationOffer(1) -> AcceptedNew");
    check(in.row_created(), "row created by offer");

    // Blocker 2: a byte-identical re-offer replays; a changed re-offer conflicts;
    // neither resets the row.
    check(in.classify(offer_e, *offer_b) == InboundDisposition::ExactReplay,
          "identical re-offer -> ExactReplay (no reset)");
    check(in.next_expected() == 2, "replay did not advance/reset sequence");
    auto [offer2_e, offer2_b] =
        daemon_frame(id, DaemonToSidecarType::OperationOffer, 1, {9, 9, 9});
    check(in.classify(offer2_e, *offer2_b) == InboundDisposition::DuplicateConflict,
          "changed re-offer -> DuplicateConflict");

    // In-order advance, then a gap.
    auto [f2_e, f2_b] = daemon_frame(id, DaemonToSidecarType::PublicFdOffer, 2, {});
    check(in.classify(f2_e, *f2_b) == InboundDisposition::AcceptedNew, "seq 2 -> AcceptedNew");
    auto [f5_e, f5_b] = daemon_frame(id, DaemonToSidecarType::OpCancel, 5, {});
    check(in.classify(f5_e, *f5_b) == InboundDisposition::Gap, "seq 5 (next 3) -> Gap");

    // Wrong operation identity is stale-only.
    auto other = make_identity();
    other.assignment_nonce = 999;
    auto [wrong_e, wrong_b] = daemon_frame(other, DaemonToSidecarType::OpCancel, 3, {});
    check(in.classify(wrong_e, *wrong_b) == InboundDisposition::StaleWrongIdentity,
          "wrong identity -> StaleWrongIdentity");

    // Blocker 4: a retired operation is never revived under the same identity.
    in.retire();
    auto [f3_e, f3_b] = daemon_frame(id, DaemonToSidecarType::OpCancel, 3, {});
    check(in.classify(f3_e, *f3_b) == InboundDisposition::StaleWrongIdentity,
          "retired operation never revived");
}

// Build an encoded Sidecar->Daemon frame for the daemon-side acceptor tests.
std::pair<FSessionControlEnvelope, std::optional<std::vector<uint8_t>>>
sidecar_frame(const FSessionOperationIdentity& id, SidecarToDaemonType type,
              uint64_t seq, std::vector<uint8_t> payload) {
    FSessionControlEnvelope e;
    e.identity = id;
    e.direction = FSessionControlDirection::SidecarToDaemon;
    e.message_type = static_cast<uint16_t>(type);
    e.sequence = seq;
    e.payload = std::move(payload);
    return {e, encode_fsession_control(e)};
}

void test_direction_enforced_per_frame() {
    // Boundary validation: after row creation, a wrong-direction envelope with a
    // matching identity and the expected sequence must still be rejected.
    const auto id = make_identity();
    FSessionInboundControl in; // sidecar-side: expects DaemonToSidecar
    auto [offer_e, offer_b] =
        daemon_frame(id, DaemonToSidecarType::OperationOffer, 1, {1});
    check(in.classify(offer_e, *offer_b) == InboundDisposition::AcceptedNew,
          "offer accepted (direction test setup)");
    auto [sd_e, sd_b] =
        sidecar_frame(id, SidecarToDaemonType::OperationAccepted, 2, {});
    check(sd_b.has_value(), "encode wrong-direction frame");
    check(in.classify(sd_e, *sd_b) == InboundDisposition::StaleWrongIdentity,
          "wrong-direction frame rejected after row creation");
    check(in.next_expected() == 2, "wrong-direction frame consumed no sequence");
}

void test_daemon_bound_acceptor() {
    const auto id = make_identity();
    auto in = FSessionInboundControl::daemon_bound(id);
    check(in.row_created(), "daemon-bound row pre-exists");

    auto [acc_e, acc_b] =
        sidecar_frame(id, SidecarToDaemonType::OperationAccepted, 1, {7});
    check(acc_b.has_value(), "encode OperationAccepted");
    check(in.classify(acc_e, *acc_b) == InboundDisposition::AcceptedNew,
          "OperationAccepted(1) accepted");
    check(in.classify(acc_e, *acc_b) == InboundDisposition::ExactReplay,
          "identical OperationAccepted replays");

    auto [gap_e, gap_b] =
        sidecar_frame(id, SidecarToDaemonType::TerminalObservation, 4, {});
    check(in.classify(gap_e, *gap_b) == InboundDisposition::Gap,
          "seq 4 (next 2) -> Gap on daemon side");

    auto other = make_identity();
    other.assignment_epoch = 777;
    auto [wr_e, wr_b] =
        sidecar_frame(other, SidecarToDaemonType::InputCommitted, 2, {});
    check(in.classify(wr_e, *wr_b) == InboundDisposition::StaleWrongIdentity,
          "wrong identity stale on daemon side");

    auto [dd_e, dd_b] = daemon_frame(id, DaemonToSidecarType::OpCancel, 2, {});
    check(in.classify(dd_e, *dd_b) == InboundDisposition::StaleWrongIdentity,
          "daemon-direction frame rejected by daemon-side acceptor");
}

void test_outbound_slots() {
    FSessionOutboundControl out(2); // bounded to 2 live slots

    const uint64_t s1 =
        out.reserve(static_cast<uint16_t>(SidecarToDaemonType::InputCommitted));
    check(s1 == 1, "reserve -> seq 1");
    const std::vector<uint8_t> f1 = {1, 2, 3};
    check(out.stage(s1, f1), "stage s1");
    check(out.find(s1) != nullptr &&
              out.find(s1)->state == OutboundSlotState::Queued,
          "s1 queued");

    const uint64_t s2 = out.reserve(
        static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation));
    check(s2 == 2, "reserve -> seq 2");
    check(out.reserve(static_cast<uint16_t>(SidecarToDaemonType::DeliveryOffer)) ==
              0,
          "reserve fails closed at bounded capacity");

    // A byte-identical live frame of the same type reuses its sequence.
    check(out.enqueue_idempotent(
              static_cast<uint16_t>(SidecarToDaemonType::InputCommitted), f1) == s1,
          "identical frame reuses sequence (one canonical frame per transition)");
    const std::vector<uint8_t> f1b = {9, 9};
    check(out.enqueue_idempotent(
              static_cast<uint16_t>(SidecarToDaemonType::InputCommitted), f1b) == 0,
          "genuinely new frame blocked at capacity");

    check(out.record_written(s1, 2), "write 2/3");
    check(out.find(s1)->state == OutboundSlotState::Writing, "s1 writing");
    check(out.record_written(s1, 9), "write remainder (clamped)");
    check(out.find(s1)->state == OutboundSlotState::FullyFlushed, "s1 fully flushed");
    check(out.mark_acked(s1), "s1 acked");
    check(out.find(s1)->state == OutboundSlotState::AckedRetained,
          "s1 retained for replay after ack");

    check(out.retire(s2), "retire s2");
    const uint64_t s3 =
        out.reserve(static_cast<uint16_t>(SidecarToDaemonType::DeliveryOffer));
    check(s3 == 3, "reserve after retire -> seq 3 (freed capacity)");
    check(out.live_slots() == 2, "two live slots (acked-retained s1 + reserved s3)");

    // No unbounded log: reserve+retire churn must reuse retired storage.
    FSessionOutboundControl churn(2);
    for (int i = 0; i < 200; ++i) {
        const uint64_t a = churn.reserve(1);
        check(a != 0, "churn reserve succeeds");
        check(churn.retire(a), "churn retire succeeds");
    }
    check(churn.slot_storage() <= 2,
          "outbound slot storage stays bounded across 200 reserve/retire cycles");
}
} // namespace

int main() {
    check(make_identity().valid(), "constructed identity is valid");

    FSessionControlEnvelope e;
    e.identity = make_identity();
    e.direction = FSessionControlDirection::DaemonToSidecar;
    e.message_type = static_cast<uint16_t>(DaemonToSidecarType::OperationOffer);
    e.sequence = 1;
    e.payload = {0xAA, 0xBB, 0xCC};

    const auto enc = encode_fsession_control(e);
    check(enc.has_value(), "encode valid envelope");
    if (enc.has_value()) {
        const auto dec = decode_fsession_control(*enc);
        check(dec.has_value(), "decode valid envelope");
        if (dec.has_value()) {
            check(dec->identity == e.identity, "identity round-trips exactly");
            check(dec->direction == e.direction &&
                      dec->message_type == e.message_type &&
                      dec->sequence == e.sequence,
                  "header round-trips");
            check(dec->payload == e.payload, "payload round-trips");
        }
        auto trailing = *enc;
        trailing.push_back(0);
        check(!decode_fsession_control(trailing).has_value(),
              "trailing byte rejected (no smuggled second frame)");
        auto bad_magic = *enc;
        bad_magic[0] ^= 0xFFu;
        check(!decode_fsession_control(bad_magic).has_value(),
              "corrupt magic rejected");
    }

    // Direction legality is a type property: a sidecar-only type (8) in a
    // Daemon->Sidecar envelope is illegal (TerminalAck stays reachable at 6).
    FSessionControlEnvelope wrong_dir = e;
    wrong_dir.message_type = 8;
    check(!encode_fsession_control(wrong_dir).has_value(),
          "sidecar-only type rejected in daemon direction");

    // TerminalAck (6) is a legal, reachable Daemon->Sidecar frame.
    FSessionControlEnvelope term_ack = e;
    term_ack.message_type = static_cast<uint16_t>(DaemonToSidecarType::TerminalAck);
    check(encode_fsession_control(term_ack).has_value(),
          "TerminalAck reachable in daemon direction");

    FSessionControlEnvelope zero_seq = e;
    zero_seq.sequence = 0;
    check(!encode_fsession_control(zero_seq).has_value(), "sequence 0 rejected");

    FSessionControlEnvelope bad_id = e;
    bad_id.identity.assignment_job = 0;
    check(!encode_fsession_control(bad_id).has_value(),
          "incomplete identity rejected");

    test_inbound_sequencer();
    test_direction_enforced_per_frame();
    test_daemon_bound_acceptor();
    test_outbound_slots();

    if (g_fail != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS p50fsessioncontrol\n");
    return 0;
}
