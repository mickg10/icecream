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

uint64_t SidecarFSessionOperation::grant_commit_permit_and_commit() {
    // The endpoint may not autonomously commit: only the owner grants the
    // permit, and only from PreparedAwaitPermit (a cancel that already
    // linearized wins the race and this call refuses).
    if (phase_ != SidecarOpPhase::PreparedAwaitPermit)
        return 0;
    const uint64_t seq = outbound_.stage_frame(
        inbound_.identity(), FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::InputCommitted),
        placeholder_payload(3));
    if (seq == 0)
        return 0;
    phase_ = SidecarOpPhase::Committed;
    return seq;
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
    const uint64_t seq = outbound_.stage_frame(
        inbound_.identity(), FSessionControlDirection::SidecarToDaemon,
        static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation),
        placeholder_payload(4));
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
    case SidecarOpPhase::Committed:
    case SidecarOpPhase::CancelledAfterCommit:
        // Durable facts retained; delivery suppressed pending reconciliation.
        delivery_suppressed_ = true;
        reconcile_required_ = true;
        break;
    case SidecarOpPhase::AbortedPreDurable:
    case SidecarOpPhase::TerminalStaged:
    case SidecarOpPhase::Retired:
        break; // already terminal-capable/terminal
    }
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
