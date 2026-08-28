#include "p50_fsession_sidecar_op.h"

namespace icecc::p50::fsession {
namespace {

std::vector<uint8_t> placeholder_payload(uint8_t tag) {
    // Phase-specific payloads (exact receipts/observation bodies) are encoded
    // by the production wiring; the reducer stages a canonical typed frame so
    // slot/flush/replay law is exercised end to end.
    return {tag};
}

} // namespace

bool SidecarFSessionOperation::deadline_live(int64_t now_ns) const noexcept {
    const auto& identity = inbound_.identity();
    return identity.deadline.valid() && now_ns < identity.deadline.expires_at_ns;
}

InboundDisposition
SidecarFSessionOperation::consume_inbound(const FSessionControlEnvelope& e,
                                          std::span<const uint8_t> bytes,
                                          int64_t now_ns) {
    const InboundDisposition disposition = inbound_.classify(e, bytes);
    if (disposition != InboundDisposition::AcceptedNew)
        return disposition;

    if (phase_ == SidecarOpPhase::AwaitOffer &&
        e.message_type ==
            static_cast<uint16_t>(DaemonToSidecarType::OperationOffer)) {
        // Refuse an offer whose original deadline is already dead: the row was
        // created by the acceptor, but the operation terminates immediately
        // with a terminal observation rather than progressing.
        if (!deadline_live(now_ns)) {
            phase_ = SidecarOpPhase::AbortedPreDurable;
            return disposition;
        }
        const uint64_t seq = outbound_.stage_frame(
            inbound_.identity(), FSessionControlDirection::SidecarToDaemon,
            static_cast<uint16_t>(SidecarToDaemonType::OperationAccepted),
            placeholder_payload(1));
        if (seq != 0)
            phase_ = SidecarOpPhase::Accepted;
        return disposition;
    }

    if (e.message_type == static_cast<uint16_t>(DaemonToSidecarType::OpCancel))
        apply_cancel();
    if (e.message_type ==
        static_cast<uint16_t>(DaemonToSidecarType::TerminalAck)) {
        // Direction-sequence acceptance is not semantic settlement: the payload
        // must name this operation and the retained observation exactly
        // (5448121766 sec.1). A fresh-sequence wrong-ack_of frame leaves the
        // operation unsettled.
        const auto ack = decode_TerminalAck(e);
        if (ack.has_value() && terminal_ack_semantically_valid(*ack))
            (void)consume_terminal_ack();
    }
    return disposition;
}

uint64_t SidecarFSessionOperation::adopt_public_fd(
    RouteSessionLease&& route_lease, const PublicFdValidator& socket_valid,
    int64_t now_ns) {
    // Preconditions (5444123889 sec.4), each refusing with zero partial
    // authority: registered operation; live original deadline; valid consumed
    // route lease naming this exact operation; injected socket-level checks.
    if (phase_ != SidecarOpPhase::Accepted)
        return 0;
    if (!deadline_live(now_ns))
        return 0;
    if (!route_lease.valid() ||
        !(route_lease.operation() == inbound_.identity()))
        return 0;
    if (!socket_valid || !socket_valid())
        return 0;

    const uint64_t seq = outbound_.stage_frame(
        inbound_.identity(), FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::PublicFdAdoptedReceipt),
        placeholder_payload(2));
    if (seq == 0)
        return 0;
    route_lease_ = std::move(route_lease);
    phase_ = SidecarOpPhase::PublicFdAdopted;
    return seq;
}

bool SidecarFSessionOperation::endpoint_started() {
    if (phase_ != SidecarOpPhase::PublicFdAdopted)
        return false;
    phase_ = SidecarOpPhase::EndpointRunning;
    return true;
}

bool SidecarFSessionOperation::prepared_input_ready() {
    if (phase_ != SidecarOpPhase::EndpointRunning)
        return false;
    phase_ = SidecarOpPhase::PreparedAwaitPermit;
    return true;
}

bool SidecarFSessionOperation::select_commit() {
    // The endpoint may not autonomously commit: only the owner selects, and
    // only from PreparedAwaitPermit (a cancel that already linearized wins the
    // race and this call refuses). Selection is STICKY: cancellation can no
    // longer win; failure resolves by actual durable state.
    if (phase_ != SidecarOpPhase::PreparedAwaitPermit)
        return false;
    phase_ = SidecarOpPhase::PermitSelected;
    return true;
}

uint64_t SidecarFSessionOperation::commit_durable(
    const CanonicalCommitFn& store_commit) {
    if (phase_ != SidecarOpPhase::PermitSelected)
        return 0;
    // The canonical store installs the COMPLETE bundle before the reducer may
    // report Committed. A failure after selection is reconciliation per actual
    // durable state -- never AbortedPreDurable (the selection already
    // linearized the owner decision).
    if (!store_commit || !store_commit()) {
        reconcile_required_ = true;
        return 0;
    }
    const uint64_t seq = outbound_.stage_frame(
        inbound_.identity(), FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::InputCommitted),
        placeholder_payload(3));
    if (seq == 0) {
        // Durable bundle exists but the evidence frame cannot stage: preserve
        // the durable facts under reconciliation (never a rollback).
        reconcile_required_ = true;
        phase_ = SidecarOpPhase::Committed;
        return 0;
    }
    phase_ = SidecarOpPhase::Committed;
    return seq;
}

uint64_t SidecarFSessionOperation::grant_commit_permit_and_commit() {
    if (!select_commit())
        return 0;
    return commit_durable([] { return true; });
}

void SidecarFSessionOperation::apply_cancel() noexcept {
    switch (phase_) {
    case SidecarOpPhase::Accepted:
    case SidecarOpPhase::PublicFdAdopted:
    case SidecarOpPhase::EndpointRunning:
    case SidecarOpPhase::PreparedAwaitPermit:
        // Cancel linearized before any durable commit: AbortedPreDurable, and
        // no later permit grant can commit (the phase gate enforces it).
        phase_ = SidecarOpPhase::AbortedPreDurable;
        break;
    case SidecarOpPhase::PermitSelected:
        // Selection already linearized the owner decision: a cancel can no
        // longer steal it. The pending durable transition resolves by actual
        // durable state; record the cancel for the terminal classification.
        delivery_suppressed_ = true;
        break;
    case SidecarOpPhase::Committed:
        // Commit won the race: the durable bundle is retained; delivery is
        // suppressed/reconciled rather than rolled back.
        phase_ = SidecarOpPhase::CancelledAfterCommit;
        delivery_suppressed_ = true;
        break;
    case SidecarOpPhase::AwaitOffer:
    case SidecarOpPhase::AbortedPreDurable:
    case SidecarOpPhase::CancelledAfterCommit:
    case SidecarOpPhase::TerminalStaged:
    case SidecarOpPhase::Retired:
        break; // no-op / already terminal-capable
    }
}

uint64_t SidecarFSessionOperation::stage_terminal_observation() {
    switch (phase_) {
    case SidecarOpPhase::Committed:
    case SidecarOpPhase::AbortedPreDurable:
    case SidecarOpPhase::CancelledAfterCommit:
        break;
    case SidecarOpPhase::TerminalStaged:
        return terminal_observation_seq_; // replay reuses the same staged frame
    default:
        return 0;
    }
    TerminalObservationPayload observation;
    observation.identity = inbound_.identity();
    observation.terminal_class =
        phase_ == SidecarOpPhase::Committed
            ? 1
            : (phase_ == SidecarOpPhase::AbortedPreDurable ? 2 : 3);
    observation.ready_event_id = 0; // canonical-store binding lands with wiring
    observation.delivery_state = delivery_suppressed_ ? 1 : 0;
    observation.highest_accepted_daemon_sequence = inbound_.next_expected() - 1;
    const auto body = encode_TerminalObservation(observation);
    if (!body.has_value())
        return 0;
    const uint64_t seq = outbound_.stage_frame(
        inbound_.identity(), FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation), *body);
    if (seq == 0)
        return 0;
    terminal_observation_seq_ = seq;
    phase_ = SidecarOpPhase::TerminalStaged;
    return seq;
}

void SidecarFSessionOperation::control_lost() noexcept {
    switch (phase_) {
    case SidecarOpPhase::AwaitOffer:
    case SidecarOpPhase::Accepted:
    case SidecarOpPhase::PublicFdAdopted:
    case SidecarOpPhase::EndpointRunning:
    case SidecarOpPhase::PreparedAwaitPermit:
        // No commit linearized: the owner proof for AbortedPreDurable holds;
        // no late record/Ready/route advance may appear.
        phase_ = SidecarOpPhase::AbortedPreDurable;
        reconcile_required_ = true;
        break;
    case SidecarOpPhase::PermitSelected:
        // Selection linearized but durability is undetermined here: resolve
        // by actual durable state under reconciliation -- never claim
        // AbortedPreDurable by construction (5448121766 sec.4).
        reconcile_required_ = true;
        break;
    case SidecarOpPhase::Committed:
    case SidecarOpPhase::CancelledAfterCommit:
        // Durable facts retained; delivery suppressed pending reconciliation.
        delivery_suppressed_ = true;
        reconcile_required_ = true;
        break;
    case SidecarOpPhase::TerminalStaged:
        // The observation is staged but the exact TerminalAck was never
        // consumed: the bounded observation/tombstone MUST survive for
        // identical-sequence/bytes replay under reconciliation. Never a
        // cleanly-terminal state (5448067827 sec.1 row 1).
        reconcile_required_ = true;
        break;
    case SidecarOpPhase::AbortedPreDurable:
    case SidecarOpPhase::Retired:
        break; // already terminal-capable/terminal
    }
}

bool SidecarFSessionOperation::terminal_ack_semantically_valid(
    const TerminalAckPayload& ack) const noexcept {
    return ack.identity == inbound_.identity() &&
           terminal_observation_seq_ != 0 &&
           ack.ack_of_sidecar_sequence == terminal_observation_seq_ &&
           ack.settlement_id != 0;
}

bool SidecarFSessionOperation::consume_reconciliation(
    const TerminalReconciliationPermit& permit) noexcept {
    if (!reconcile_required_)
        return false;
    if (!(permit.identity == inbound_.identity()))
        return false; // stale permit for another operation retires nothing
    if (permit.retained_observation_sequence != terminal_observation_seq_)
        return false; // must name the exact retained observation (or 0=none)
    if (permit.outcome != ReconcileOutcome::DaemonSettlementProved &&
        permit.outcome !=
            ReconcileOutcome::ExactOperationAbandonedUnderIncarnationLoss &&
        permit.outcome != ReconcileOutcome::WholeIncarnationTerminated)
        return false;
    if (permit.supporting_receipt == 0)
        return false;
    reconciled_ = true;
    return true;
}

bool SidecarFSessionOperation::consume_terminal_ack() noexcept {
    if (phase_ != SidecarOpPhase::TerminalStaged ||
        terminal_observation_seq_ == 0)
        return false;
    // An ack can only follow a fully flushed observation; an ack for a
    // never-flushed observation is a protocol anomaly and does not retire.
    if (!outbound_.mark_acked(terminal_observation_seq_))
        return false;
    phase_ = SidecarOpPhase::Retired;
    inbound_.retire();
    return true;
}

} // namespace icecc::p50::fsession
