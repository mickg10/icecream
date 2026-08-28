// Functional-correctness test for the sidecar service-owner composition layer:
// bounded operation table, one parser per connection, independent A/B
// progress, terminal framing errors scoped to one connection, bounded
// would-block-resumable outbound drain, control-loss law, and slot reclaim.

#include "cache/p50_fsession_service_owner.h"

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

FSessionOperationIdentity op_identity(uint64_t op_seq) {
    FSessionOperationIdentity id;
    id.daemon_launch_generation = 21;
    id.control_connection_generation = 22;
    id.operation = {{11, 13},
                    icecc::p50::daemon::P50SessionOperationRole::FSession,
                    op_seq};
    id.c_store_guid.bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    id.f_store_guid.bytes = {16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
    id.assignment_job = 100 + op_seq;
    id.assignment_epoch = 200;
    id.assignment_nonce = 300;
    id.arm_observation = 400;
    id.deadline = {1'000'000, 1, 1};
    assert(id.valid());
    return id;
}

std::vector<uint8_t> offer_frame(const FSessionOperationIdentity& id) {
    FSessionControlEnvelope e;
    e.identity = id;
    e.direction = FSessionControlDirection::DaemonToSidecar;
    e.message_type = static_cast<uint16_t>(DaemonToSidecarType::OperationOffer);
    e.sequence = 1;
    e.payload = {1};
    auto enc = encode_fsession_control(e);
    assert(enc.has_value());
    return *enc;
}

constexpr int64_t kNow = 1000;
} // namespace

int main() {
    // Positive admission is closed by default and opens only through the
    // exact owner-minted readiness authority.
    {
        FSessionServiceOwner closed(2);
        check(closed.connection_opened() == 0,
              "admission closed by default refuses connections");
        auto tampered = mint_fsession_admission_ready(1, 1);
        tampered.codec_registry_hash ^= 1;
        check(!closed.open_admission(tampered),
              "tampered codec-registry hash refused");
        auto wrong_gen = mint_fsession_admission_ready(2, 1);
        check(!closed.open_admission(wrong_gen),
              "stale/wrong service generation refused");
        check(closed.connection_opened() == 0, "still closed after refusals");
    }

    FSessionServiceOwner owner(2); // bounded to two live operations
    check(owner.open_admission(mint_fsession_admission_ready(
              owner.service_generation(), 1)),
          "exact readiness authority opens admission");

    // Two independent connections/operations.
    const uint64_t a = owner.connection_opened();
    const uint64_t b = owner.connection_opened();
    check(a != 0 && b != 0 && a != b, "two connections admitted");
    check(owner.connection_opened() == 0,
          "third connection refused at the bounded cap");

    const auto id_a = op_identity(1);
    const auto id_b = op_identity(2);
    check(owner.on_bytes(a, offer_frame(id_a), kNow) ==
              ServiceIngestStatus::Progress,
          "A offer ingested");
    check(owner.on_bytes(b, offer_frame(id_b), kNow) ==
              ServiceIngestStatus::Progress,
          "B offer ingested");
    check(owner.operation(a)->phase() == SidecarOpPhase::Accepted &&
              owner.operation(b)->phase() == SidecarOpPhase::Accepted,
          "A and B progress independently");

    // Framing garbage on A is terminal for A's connection only.
    const std::vector<uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF};
    check(owner.on_bytes(a, garbage, kNow) ==
              ServiceIngestStatus::ConnectionError,
          "garbage on A -> connection error");
    check(owner.operation(b)->phase() == SidecarOpPhase::Accepted,
          "B unaffected by A's framing error");

    // Bounded, would-block-resumable drain of B's staged OperationAccepted.
    std::vector<uint8_t> written;
    size_t block_after = 10; // deliver 10 bytes, then would-block once
    const ServiceWriteFn write_fn = [&](std::span<const uint8_t> bytes) -> long {
        if (written.size() >= block_after && block_after != 0) {
            block_after = 0; // block exactly once
            return 0;
        }
        const size_t take = bytes.size() < 7 ? bytes.size() : 7; // small chunks
        written.insert(written.end(), bytes.begin(), bytes.begin() + take);
        return static_cast<long>(take);
    };
    check(owner.drain_outbound(b, write_fn, 1024), "drain (hits would-block)");
    check(owner.drain_outbound(b, write_fn, 1024), "drain resumes and finishes");
    const OutboundSemanticSlot* accepted = owner.operation(b)->outbound().find(1);
    check(accepted != nullptr &&
              accepted->state == OutboundSlotState::FullyFlushed,
          "B's OperationAccepted fully flushed");
    check(written == accepted->canonical_bytes,
          "drained bytes are byte-exact");

    // Control loss: A was pre-commit -> AbortedPreDurable + reconcile flag.
    owner.connection_closed(a);
    check(owner.operation(a)->phase() == SidecarOpPhase::AbortedPreDurable &&
              owner.operation(a)->reconcile_required(),
          "A control loss pre-commit -> AbortedPreDurable + reconcile");

    // Reclaim: refused while open (B); refused for closed A until its
    // reconciliation is owner-resolved (local facts survive until then).
    check(!owner.reclaim(b), "live B is never evicted");
    check(!owner.reclaim(a),
          "closed A with unresolved reconciliation is NOT reclaimable");
    // Reconciliation retires only through the exact typed permit.
    SidecarFSessionOperation::TerminalReconciliationPermit wrong;
    wrong.identity = id_b; // names another operation
    wrong.retained_observation_sequence = 0;
    wrong.outcome = SidecarFSessionOperation::ReconcileOutcome::
        ExactOperationAbandonedUnderIncarnationLoss;
    wrong.supporting_receipt = 7;
    check(!owner.operation(a)->consume_reconciliation(wrong),
          "permit naming another operation retires nothing");
    SidecarFSessionOperation::TerminalReconciliationPermit permit = wrong;
    permit.identity = id_a;
    check(owner.operation(a)->consume_reconciliation(permit),
          "exact typed permit resolves reconciliation");
    check(owner.reclaim(a), "closed A reclaimed after reconciliation resolves");
    check(owner.live_operations() == 1, "capacity freed");
    check(owner.connection_opened() != 0, "new connection admitted after reclaim");

    if (g_fail != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("PASS p50fsessionserviceowner\n");
    return 0;
}
