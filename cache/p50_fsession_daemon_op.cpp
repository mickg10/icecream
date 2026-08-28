#include "p50_fsession_daemon_op.h"

#include "p50_fsession_payloads.h"

namespace icecc::p50::fsession {
bool DaemonFSessionOperation::retiredish_replay_probe(
    const FSessionControlEnvelope& e, std::span<const uint8_t> bytes) const {
    (void)e;
    (void)bytes;
    // Replays are recognized by the acceptor itself; the phase gate only needs
    // to let already-consumed sequences through. A frame below the frontier is
    // either an exact replay (acceptor: ExactReplay, zero mutation) or a
    // conflict (acceptor: DuplicateConflict) -- both safe to classify.
    return e.sequence < inbound_.next_expected();
}

std::optional<DaemonFSessionOperation>
DaemonFSessionOperation::mint(const FSessionOperationIdentity& identity,
                              DaemonWaitLease&& lease,
                              const RoutePredecessor& predecessor,
                              size_t outbound_slots) {
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

    if (!predecessor.valid())
        return std::nullopt;
    DaemonFSessionOperation op;
    op.identity_ = identity;
    op.lease_ = std::move(lease);
    op.predecessor_ = predecessor;
    op.inbound_ = FSessionInboundControl::daemon_bound(identity);
    op.outbound_ = FSessionOutboundControl(outbound_slots);
    OperationOfferPayload offer;
    offer.identity = identity;
    offer.client_connection_generation =
        op.lease_.facts().client_connection_generation;
    offer.compile_file_lease = op.lease_.facts().compile_file_lease;
    offer.wait_reservation = op.lease_.facts().wait_reservation;
    offer.consumed_claim_capability =
        op.lease_.facts().consumed_claim_capability;
    offer.predecessor = predecessor;
    const auto body = encode_OperationOffer(offer);
    if (!body.has_value())
        return std::nullopt;
    const uint64_t seq = op.outbound_.stage_frame(
        identity, FSessionControlDirection::DaemonToSidecar,
        static_cast<uint16_t>(DaemonToSidecarType::OperationOffer), *body);
    if (seq == 0)
        return std::nullopt;
    op.phase_ = DaemonOpPhase::Minted;
    return op;
}

namespace {
// Phase-legality table: a direction-legal frame that is illegal in the current
// daemon phase must NOT advance the inbound frontier (root probe control 3).
bool daemon_phase_legal(DaemonOpPhase phase, uint16_t type) noexcept {
    switch (static_cast<SidecarToDaemonType>(type)) {
    case SidecarToDaemonType::OperationAccepted:
        return phase == DaemonOpPhase::Minted;
    case SidecarToDaemonType::PublicFdAdoptedReceipt:
        return phase == DaemonOpPhase::PublicFdOffered;
    case SidecarToDaemonType::EndpointObservation:
    case SidecarToDaemonType::InputCommitted:
    case SidecarToDaemonType::InputAbortedPreDurable:
    case SidecarToDaemonType::InputCancelledAfterCommit:
    case SidecarToDaemonType::DeliveryOffer:
        return phase == DaemonOpPhase::FdAdoptedObserved ||
               phase == DaemonOpPhase::SourceAccepted ||
               phase == DaemonOpPhase::CancelRequested;
    case SidecarToDaemonType::TerminalObservation:
        return phase != DaemonOpPhase::Retired;
    }
    return false;
}
} // namespace

InboundDisposition
DaemonFSessionOperation::consume_inbound(const FSessionControlEnvelope& e,
                                         std::span<const uint8_t> bytes) {
    // Atomic phase gate BEFORE any sequence/frontier advance: a fresh frame
    // that is phase-illegal is refused without consuming its sequence (an
    // exact replay of an already-retained frame still classifies normally).
    if (!retiredish_replay_probe(e, bytes) &&
        !daemon_phase_legal(phase_, e.message_type))
        return InboundDisposition::PhaseInvalidNoRow;
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
        // Settle at most once. Response capacity is part of the transition's
        // authority: the exact TerminalAck is staged FIRST, and only a
        // successful stage permits the settlement transition (root probe
        // control 4; 5444539259 sec.3).
        if (phase_ != DaemonOpPhase::Settled && phase_ != DaemonOpPhase::Retired) {
            TerminalAckPayload ack;
            ack.identity = identity_;
            ack.ack_of_sidecar_sequence = e.sequence;
            ack.terminal_class_echo = observation->terminal_class;
            ack.settlement_id = e.sequence; // daemon settlement/tombstone id
            const auto body = encode_TerminalAck(ack);
            const uint64_t staged =
                body.has_value()
                    ? outbound_.stage_frame(
                          identity_, FSessionControlDirection::DaemonToSidecar,
                          static_cast<uint16_t>(DaemonToSidecarType::TerminalAck),
                          *body)
                    : 0;
            if (staged == 0)
                break; // no capacity: no settlement; the peer replays later
            terminal_ack_seq_ = staged;
            ++settlement_count_;
            phase_ = DaemonOpPhase::Settled;
        }
        break;
    }
    case SidecarToDaemonType::EndpointObservation:
    case SidecarToDaemonType::InputCommitted:
    case SidecarToDaemonType::InputAbortedPreDurable:
    case SidecarToDaemonType::InputCancelledAfterCommit:
        break; // recorded by the wiring; no phase change at this layer
    case SidecarToDaemonType::DeliveryOffer: {
        // Delivery acceptance is authorized only by the exact typed offer
        // previously consumed for this operation.  The decoder re-binds the
        // payload identity to the envelope identity and validates every offer
        // field before the id is retained.
        const auto offer = decode_DeliveryOffer(e);
        if (offer.has_value() && offer->identity == identity_)
            delivery_offer_ = *offer;
        break;
    }
    }
    return disposition;
}

uint64_t DaemonFSessionOperation::offer_public_fd(uint64_t socket_cookie) {
    if (socket_cookie == 0)
        return 0;
    if (phase_ != DaemonOpPhase::AcceptedByPeer &&
        phase_ != DaemonOpPhase::PublicFdOffered)
        return 0;
    // The product transfer identity is STABLE across retries; every rendezvous
    // attempt carries a FRESH AncillaryAttemptId (5448067827 sec.4).
    if (public_fd_offer_id_ == 0)
        public_fd_offer_id_ = identity_.operation.operation_sequence << 8 | 1;
    PublicFdOfferPayload offer;
    offer.identity = identity_;
    offer.public_fd_offer_id = public_fd_offer_id_;
    offer.ancillary_attempt_id = next_ancillary_attempt_++;
    offer.socket_cookie = socket_cookie;
    const auto body = encode_PublicFdOffer(offer);
    if (!body.has_value())
        return 0;
    const uint64_t seq = outbound_.stage_frame(
        identity_, FSessionControlDirection::DaemonToSidecar,
        static_cast<uint16_t>(DaemonToSidecarType::PublicFdOffer), *body);
    if (seq == 0)
        return 0;
    phase_ = DaemonOpPhase::PublicFdOffered;
    return public_fd_offer_id_;
}

std::optional<DeliveryAcceptance>
DaemonFSessionOperation::accept_delivery(uint64_t delivery_id,
                                         bool descriptor_valid) {
    if (delivery_id == 0)
        return std::nullopt;
    // A descriptor is admissible only when the exact delivery id was named by
    // a previously decoded DeliveryOffer for this operation.  In particular,
    // the phase alone is not an offer/claim authority.
    if (!delivery_offer_.has_value() ||
        !(delivery_offer_->identity == identity_) ||
        delivery_offer_->attachment_delivery_id != delivery_id)
        return std::nullopt;
    // Replay of an already accepted delivery: return the retained receipt;
    // the caller closes the duplicate descriptor; no second transition.
    for (const auto& receipt : acceptance_ledger_) {
        if (receipt.delivery_id == delivery_id)
            return DeliveryAcceptance{receipt, /*replay=*/true};
    }
    if (phase_ != DaemonOpPhase::FdAdoptedObserved || !descriptor_valid)
        return std::nullopt;

    // Response capacity is reserved BEFORE the irreversible acceptance: the
    // DaemonFdAccepted frame must stage or no ledger/TOCOMPILE transition
    // occurs (root probe control 5; 5444539259 sec.3).
    InputFdAcceptanceReceipt receipt;
    receipt.receipt_id = next_receipt_id_;
    receipt.delivery_id = delivery_id;
    receipt.operation_sequence = identity_.operation.operation_sequence;
    DaemonFdAcceptedPayload accepted;
    accepted.identity = identity_;
    accepted.attachment_delivery_id = delivery_id;
    accepted.acceptance_receipt_id = receipt.receipt_id;
    accepted.replay = 0;
    const auto body = encode_DaemonFdAccepted(accepted);
    if (!body.has_value())
        return std::nullopt;
    const uint64_t staged = outbound_.stage_frame(
        identity_, FSessionControlDirection::DaemonToSidecar,
        static_cast<uint16_t>(DaemonToSidecarType::DaemonFdAccepted), *body);
    if (staged == 0)
        return std::nullopt; // no capacity: refuse; the delivery retries
    ++next_receipt_id_;
    acceptance_ledger_.push_back(receipt);
    ++tocompile_transitions_; // exactly one WAITP50INPUT -> TOCOMPILE
    phase_ = DaemonOpPhase::SourceAccepted;
    return DeliveryAcceptance{receipt, /*replay=*/false};
}

uint64_t DaemonFSessionOperation::request_cancel(uint16_t reason) {
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
    OpCancelPayload cancel;
    cancel.identity = identity_;
    cancel.reason = reason;
    cancel.cancellation_observation = next_cancellation_observation_++;
    const auto body = encode_OpCancel(cancel);
    if (!body.has_value())
        return 0;
    const uint64_t seq = outbound_.stage_frame(
        identity_, FSessionControlDirection::DaemonToSidecar,
        static_cast<uint16_t>(DaemonToSidecarType::OpCancel), *body);
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
