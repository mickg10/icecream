// Functional-correctness test for the daemon F-session operation phase
// reducer: lease-bound mint, stable PublicFdOfferId across retry, product
// receipt (not transport ACK) gating adoption, mint-once InputFdAcceptance
// with duplicate-delivery replay, at-most-once terminal settlement with a
// replayable tombstone, and typed reconciliation on control loss
// (issue-16 5444123889 sec.5 / 5444285506 sec.1 / 5443811178 sec.6 /
// 5444410383 sec.5).

#include "cache/p50_fsession_daemon_op.h"

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

FSessionOperationIdentity op_identity() {
    FSessionOperationIdentity id;
    id.daemon_launch_generation = 21;
    id.control_connection_generation = 22;
    id.operation = {{11, 13},
                    icecc::p50::daemon::P50SessionOperationRole::FSession, 42};
    id.c_store_guid.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    id.f_store_guid.bytes = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    id.assignment_job = 100;
    id.assignment_epoch = 200;
    id.assignment_nonce = 300;
    id.arm_observation = 400;
    id.deadline = {123456789, 1, 1};
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
    f.deadline = {123456789, 1, 1};
    assert(f.valid());
    return f;
}

std::pair<FSessionControlEnvelope, std::vector<uint8_t>>
sidecar_frame(const FSessionOperationIdentity& id, SidecarToDaemonType type,
              uint64_t seq) {
    FSessionControlEnvelope e;
    e.identity = id;
    e.direction = FSessionControlDirection::SidecarToDaemon;
    e.message_type = static_cast<uint16_t>(type);
    e.sequence = seq;
    auto enc = encode_fsession_control(e);
    assert(enc.has_value());
    return {e, *enc};
}
} // namespace

int main() {
    const auto id = op_identity();

    // Mint refuses a lease whose facts disagree with the identity binding.
    {
        auto bad = lease_facts();
        bad.assignment_epoch = 999;
        check(!DaemonFSessionOperation::mint(id, DaemonWaitLease(bad)).has_value(),
              "mint refuses mismatched lease facts");
    }

    auto minted = DaemonFSessionOperation::mint(id, DaemonWaitLease(lease_facts()));
    check(minted.has_value(), "mint succeeds with exact lease facts");
    auto& op = *minted;
    check(op.phase() == DaemonOpPhase::Minted, "phase Minted; offer staged");

    // Public offer is illegal before the peer accepts.
    check(op.offer_public_fd() == 0, "public offer refused before acceptance");

    auto [acc_e, acc_b] = sidecar_frame(id, SidecarToDaemonType::OperationAccepted, 1);
    (void)op.consume_inbound(acc_e, acc_b);
    check(op.phase() == DaemonOpPhase::AcceptedByPeer, "peer accepted");

    // One PublicFdOfferId; a retry reuses it exactly (no second claim).
    const uint64_t offer_id = op.offer_public_fd();
    check(offer_id != 0, "public offer minted");
    check(op.phase() == DaemonOpPhase::PublicFdOffered, "phase PublicFdOffered");
    check(op.offer_public_fd() == offer_id, "retry reuses the same offer id");

    // A delivery cannot be accepted while only Offered: the product receipt --
    // never a transport-level ACK -- moves the daemon past Offered.
    check(!op.accept_delivery(77, true).has_value(),
          "delivery refused before the exact adopted receipt");

    auto [adopt_e, adopt_b] =
        sidecar_frame(id, SidecarToDaemonType::PublicFdAdoptedReceipt, 2);
    (void)op.consume_inbound(adopt_e, adopt_b);
    check(op.phase() == DaemonOpPhase::FdAdoptedObserved, "receipt consumed");

    // Mint-once acceptance; duplicate delivery replays the same receipt.
    auto first = op.accept_delivery(77, true);
    check(first.has_value() && !first->replay && first->receipt.valid(),
          "delivery accepted; receipt minted");
    check(op.tocompile_transitions() == 1, "exactly one TOCOMPILE transition");
    auto dup = op.accept_delivery(77, true);
    check(dup.has_value() && dup->replay &&
              dup->receipt.receipt_id == first->receipt.receipt_id,
          "duplicate delivery replays the same receipt (close the dup fd)");
    check(op.tocompile_transitions() == 1,
          "no second TOCOMPILE transition on replay");
    check(!op.accept_delivery(0, true).has_value(), "zero delivery id refused");

    // Terminal settlement: at most once; ack tombstone replayable; retire only
    // after the ack is fully flushed.
    auto [term_e, term_b] =
        sidecar_frame(id, SidecarToDaemonType::TerminalObservation, 3);
    (void)op.consume_inbound(term_e, term_b);
    check(op.phase() == DaemonOpPhase::Settled, "settled");
    check(op.settlement_count() == 1, "one settlement");
    (void)op.consume_inbound(term_e, term_b); // exact observation replay
    check(op.settlement_count() == 1, "replay applies no second settlement");
    check(op.request_cancel() == 0, "no cancel authority after settlement");
    check(!op.retire(), "retire refused before the ack is flushed");
    // Flush the staged TerminalAck, then retire.
    bool flushed = false;
    for (uint64_t seq = 1; seq <= 8; ++seq) {
        const auto* slot = op.outbound().find(seq);
        if (slot != nullptr &&
            slot->message_type ==
                static_cast<uint16_t>(DaemonToSidecarType::TerminalAck)) {
            flushed = op.outbound().record_written(seq, slot->canonical_bytes.size());
            break;
        }
    }
    check(flushed, "terminal ack flushed");
    check(op.retire(), "retire after flushed ack");
    check(op.phase() == DaemonOpPhase::Retired, "retired");

    // A second operation: cancel path and reconciliation.
    auto second = DaemonFSessionOperation::mint(id, DaemonWaitLease(lease_facts()));
    check(second.has_value(), "second op mints");
    auto [acc2_e, acc2_b] =
        sidecar_frame(id, SidecarToDaemonType::OperationAccepted, 1);
    (void)second->consume_inbound(acc2_e, acc2_b);
    check(second->request_cancel() != 0, "cancel staged from live phase");
    check(second->phase() == DaemonOpPhase::CancelRequested, "cancel requested");
    second->control_lost();
    check(second->phase() == DaemonOpPhase::ReconcileRequired,
          "control loss -> typed reconciliation (no WAIT rollback)");

    if (g_fail != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS p50fsessiondaemonop\n");
    return 0;
}
