#include "p50_fsession_daemon_op.h"

#include "p50_fsession_payloads.h"

namespace icecc::p50::fsession {
namespace {

std::vector<uint8_t> placeholder_payload(uint8_t tag) {
    // Exact frame bodies are encoded by the production wiring; the reducer
    // stages typed frames so the slot/flush/replay law is exercised end to end.
    return {tag};
}

} // namespace

std::optional<DaemonFSessionOperation>
DaemonFSessionOperation::mint(const FSessionOperationIdentity& identity,
                              DaemonWaitLease&& lease, size_t outbound_slots) {
    if (!identity.valid() || !lease.valid())
        return std::nullopt;
    // The operation identity's assignment binding must be the lease's exact
    // facts (no independently invented identity; 5444285506 sec.1).
    const auto& facts = lease.facts();
    if (identity.assignment_job != facts.assignment_job ||
        identity.assignment_epoch != facts.assignment_epoch ||
        identity.assignment_nonce != facts.assignment_nonce ||
        identity.arm_observation != facts.arm_observation ||
        !(identity.deadline == facts.deadline))
        return std::nullopt;

    DaemonFSessionOperation op;
    op.identity_ = identity;
    op.lease_ = std::move(lease);
    op.inbound_ = FSessionInboundControl::daemon_bound(identity);
    op.outbound_ = FSessionOutboundControl(outbound_slots);
    const uint64_t seq = op.outbound_.stage_frame(
        identity, FSessionControlDirection::DaemonToSidecar,
        static_cast<uint16_t>(DaemonToSidecarType::OperationOffer),
        placeholder_payload(1));
    if (seq == 0)
        return std::nullopt;
    op.phase_ = DaemonOpPhase::Minted;
    return op;
}

InboundDisposition
DaemonFSessionOperation::consume_inbound(const FSessionControlEnvelope& e,
                                         std::span<const uint8_t> bytes) {
    const InboundDisposition disposition = inbound_.classify(e, bytes);
    if (disposition == InboundDisposition::ExactReplay &&
        e.message_type ==
            static_cast<uint16_t>(SidecarToDaemonType::TerminalObservation)) {
        // Observation replay: no second settlement; the retained TerminalAck
        // is replayed by re-flushing its slot (same sequence, same bytes).
        return disposition;
    }
    if (disposition != InboundDisposition::AcceptedNew)
        return disposition;

    switch (static_cast<SidecarToDaemonType>(e.message_type)) {
    case SidecarToDaemonType::OperationAccepted:
        if (phase_ == DaemonOpPhase::Minted)
            phase_ = DaemonOpPhase::AcceptedByPeer;
        break;
    case SidecarToDaemonType::PublicFdAdoptedReceipt:
        // Only the exact product receipt moves the daemon past Offered
        // (a transport-level ACK never does; 5444123889 sec.3).
        if (phase_ == DaemonOpPhase::PublicFdOffered)
            phase_ = DaemonOpPhase::FdAdoptedObserved;
        break;
    case SidecarToDaemonType::TerminalObservation: {
        // Semantic gate: only a decodable identity-complete observation can
        // settle; a placeholder/invalid body leaves the daemon unsettled.
        const auto observation = decode_TerminalObservation(e);
        if (!observation.has_value())
            break;
        // Settle at most once; retain the tombstone; stage the one exact
        // TerminalAck naming the retained observation sequence.
        if (phase_ != DaemonOpPhase::Settled && phase_ != DaemonOpPhase::Retired) {
            ++settlement_count_;
            phase_ = DaemonOpPhase::Settled;
            TerminalAckPayload ack;
            ack.identity = identity_;
            ack.ack_of_sidecar_sequence = e.sequence;
            ack.terminal_class_echo = observation->terminal_class;
            ack.settlement_id = e.sequence; // daemon settlement/tombstone id
            const auto body = encode_TerminalAck(ack);
            if (body.has_value())
                terminal_ack_seq_ = outbound_.stage_frame(
                    identity_, FSessionControlDirection::DaemonToSidecar,
                    static_cast<uint16_t>(DaemonToSidecarType::TerminalAck),
                    *body);
        }
        break;
    }
    case SidecarToDaemonType::EndpointObservation:
    case SidecarToDaemonType::InputCommitted:
    case SidecarToDaemonType::InputAbortedPreDurable:
    case SidecarToDaemonType::InputCancelledAfterCommit:
    case SidecarToDaemonType::DeliveryOffer:
        break; // recorded by the wiring; no phase change at this layer
    }
    return disposition;
}

uint64_t DaemonFSessionOperation::offer_public_fd() {
    if (phase_ != DaemonOpPhase::AcceptedByPeer &&
        phase_ != DaemonOpPhase::PublicFdOffered)
        return 0;
    if (public_fd_offer_id_ == 0) {
        public_fd_offer_id_ = identity_.operation.operation_sequence << 8 | 1;
        const uint64_t seq = outbound_.stage_frame(
            identity_, FSessionControlDirection::DaemonToSidecar,
            static_cast<uint16_t>(DaemonToSidecarType::PublicFdOffer),
            placeholder_payload(2));
        if (seq == 0) {
            public_fd_offer_id_ = 0;
            return 0;
        }
    }
    // A retry reuses the exact same PublicFdOfferId (no second client claim).
    phase_ = DaemonOpPhase::PublicFdOffered;
    return public_fd_offer_id_;
}

std::optional<DeliveryAcceptance>
DaemonFSessionOperation::accept_delivery(uint64_t delivery_id,
                                         bool descriptor_valid) {
    if (delivery_id == 0)
        return std::nullopt;
    // Replay of an already accepted delivery: return the retained receipt;
    // the caller closes the duplicate descriptor; no second transition.
    for (const auto& receipt : acceptance_ledger_) {
        if (receipt.delivery_id == delivery_id)
            return DeliveryAcceptance{receipt, /*replay=*/true};
    }
    if (phase_ != DaemonOpPhase::FdAdoptedObserved || !descriptor_valid)
        return std::nullopt;

    InputFdAcceptanceReceipt receipt;
    receipt.receipt_id = next_receipt_id_++;
    receipt.delivery_id = delivery_id;
    receipt.operation_sequence = identity_.operation.operation_sequence;
    acceptance_ledger_.push_back(receipt);
    ++tocompile_transitions_; // exactly one WAITP50INPUT -> TOCOMPILE
    phase_ = DaemonOpPhase::SourceAccepted;
    const uint64_t seq = outbound_.stage_frame(
        identity_, FSessionControlDirection::DaemonToSidecar,
        static_cast<uint16_t>(DaemonToSidecarType::DaemonFdAccepted),
        placeholder_payload(4));
    (void)seq; // slot law verified by tests; wiring drives the flush
    return DeliveryAcceptance{receipt, /*replay=*/false};
}

uint64_t DaemonFSessionOperation::request_cancel() {
    switch (phase_) {
    case DaemonOpPhase::Minted:
    case DaemonOpPhase::AcceptedByPeer:
    case DaemonOpPhase::PublicFdOffered:
    case DaemonOpPhase::FdAdoptedObserved:
    case DaemonOpPhase::SourceAccepted:
        break;
    default:
        return 0; // settled/reconcile/retired: no cancel authority
    }
    const uint64_t seq = outbound_.stage_frame(
        identity_, FSessionControlDirection::DaemonToSidecar,
        static_cast<uint16_t>(DaemonToSidecarType::OpCancel),
        placeholder_payload(5));
    if (seq != 0)
        phase_ = DaemonOpPhase::CancelRequested;
    return seq;
}

void DaemonFSessionOperation::control_lost() noexcept {
    if (phase_ == DaemonOpPhase::Settled || phase_ == DaemonOpPhase::Retired)
        return;
    // After transfer may have occurred, loss is typed reconciliation: the WAIT
    // does not roll back and no second claim/offer opens.
    phase_ = DaemonOpPhase::ReconcileRequired;
}

bool DaemonFSessionOperation::retire() noexcept {
    if (phase_ != DaemonOpPhase::Settled || terminal_ack_seq_ == 0)
        return false;
    // The tombstone must survive until the ack is at least fully flushed so an
    // observation replay can find and replay it (5444410383 sec.5).
    const OutboundSemanticSlot* slot = outbound_.find(terminal_ack_seq_);
    if (slot == nullptr || (slot->state != OutboundSlotState::FullyFlushed &&
                            slot->state != OutboundSlotState::AckedRetained))
        return false;
    phase_ = DaemonOpPhase::Retired;
    inbound_.retire();
    return true;
}

} // namespace icecc::p50::fsession
