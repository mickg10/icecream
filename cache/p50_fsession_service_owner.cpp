#include "p50_fsession_service_owner.h"

namespace icecc::p50::fsession {

FSessionServiceOwner::Slot*
FSessionServiceOwner::find(uint64_t connection_id) noexcept {
    for (auto& slot : slots_)
        if (slot.in_use && slot.connection_id == connection_id)
            return &slot;
    return nullptr;
}

size_t FSessionServiceOwner::live_operations() const noexcept {
    size_t n = 0;
    for (const auto& slot : slots_)
        if (slot.in_use)
            ++n;
    return n;
}

uint64_t FSessionServiceOwner::connection_opened() {
    if (!admission_enabled_)
        return 0; // admission closed until exact payload codecs are installed
    if (live_operations() >= max_operations_)
        return 0; // bounded: refuse; no partial row
    const uint64_t connection_id = next_connection_id_++;
    for (auto& slot : slots_) {
        if (!slot.in_use) {
            slot = Slot{};
            slot.connection_id = connection_id;
            slot.connection_open = true;
            slot.in_use = true;
            slot.op = std::make_unique<SidecarFSessionOperation>();
            return connection_id;
        }
    }
    Slot slot;
    slot.connection_id = connection_id;
    slot.connection_open = true;
    slot.in_use = true;
    slot.op = std::make_unique<SidecarFSessionOperation>();
    slots_.push_back(std::move(slot));
    return connection_id;
}

ServiceIngestStatus
FSessionServiceOwner::on_bytes(uint64_t connection_id,
                               std::span<const uint8_t> bytes, int64_t now_ns) {
    Slot* slot = find(connection_id);
    if (slot == nullptr || !slot->connection_open)
        return ServiceIngestStatus::UnknownConnection;
    if (slot->reader.feed(bytes) == FSessionFrameReader::Status::Error)
        return ServiceIngestStatus::ConnectionError;
    while (auto frame = slot->reader.next_frame()) {
        const auto envelope = decode_fsession_control(*frame);
        if (!envelope.has_value())
            return ServiceIngestStatus::ConnectionError;
        // The reducer classifies stale/duplicate/gap itself; a Gap or a
        // non-identical duplicate is operation-local protocol error and
        // terminal for this dedicated connection.
        const InboundDisposition disposition =
            slot->op->consume_inbound(*envelope, *frame, now_ns);
        if (disposition == InboundDisposition::Gap ||
            disposition == InboundDisposition::DuplicateConflict)
            return ServiceIngestStatus::ConnectionError;
    }
    return ServiceIngestStatus::Progress;
}

bool FSessionServiceOwner::drain_outbound(uint64_t connection_id,
                                          const ServiceWriteFn& write_fn,
                                          size_t max_bytes) {
    Slot* slot = find(connection_id);
    if (slot == nullptr || !slot->connection_open || !write_fn)
        return false;
    size_t budget = max_bytes;
    FSessionOutboundControl& out = slot->op->outbound();
    // Walk slots in sequence order; write Queued/Writing frames until the
    // quantum is exhausted or the transport would block.
    for (uint64_t seq = 1; budget > 0 && seq <= 1024; ++seq) {
        const OutboundSemanticSlot* frame = out.find(seq);
        if (frame == nullptr)
            continue;
        if (frame->state != OutboundSlotState::Queued &&
            frame->state != OutboundSlotState::Writing)
            continue;
        while (budget > 0) {
            const size_t remaining =
                frame->canonical_bytes.size() - frame->write_offset;
            if (remaining == 0)
                break;
            const size_t chunk = remaining < budget ? remaining : budget;
            const long wrote = write_fn(
                {frame->canonical_bytes.data() + frame->write_offset, chunk});
            if (wrote < 0)
                return false; // terminal write error
            if (wrote == 0)
                return true; // would-block: resume on next writability event
            budget -= static_cast<size_t>(wrote);
            if (!out.record_written(seq, static_cast<size_t>(wrote)))
                return false;
            frame = out.find(seq); // state may have advanced to FullyFlushed
            if (frame->state == OutboundSlotState::FullyFlushed)
                break;
        }
    }
    return true;
}

bool FSessionServiceOwner::has_pending_outbound(uint64_t connection_id) {
    Slot* slot = find(connection_id);
    if (slot == nullptr)
        return false;
    FSessionOutboundControl& out = slot->op->outbound();
    for (uint64_t seq = 1; seq <= 1024; ++seq) {
        const OutboundSemanticSlot* frame = out.find(seq);
        if (frame != nullptr && (frame->state == OutboundSlotState::Queued ||
                                 frame->state == OutboundSlotState::Writing))
            return true;
    }
    return false;
}

SidecarFSessionOperation*
FSessionServiceOwner::operation(uint64_t connection_id) {
    Slot* slot = find(connection_id);
    return slot != nullptr ? slot->op.get() : nullptr;
}

void FSessionServiceOwner::connection_closed(uint64_t connection_id) {
    Slot* slot = find(connection_id);
    if (slot == nullptr || !slot->connection_open)
        return;
    slot->connection_open = false;
    slot->op->control_lost();
}

bool FSessionServiceOwner::reclaim(uint64_t connection_id) {
    Slot* slot = find(connection_id);
    if (slot == nullptr)
        return false;
    // Reclaim only a closed connection whose operation reached a terminal or
    // reconciliation-preserved state; a live operation is never evicted.
    if (slot->connection_open)
        return false;
    // An unresolved reconciliation retains the operation's local observation
    // facts; reclaiming it would destroy the evidence the reset/replacement
    // path needs. Reclaim requires either a cleanly terminal operation or an
    // owner-resolved reconciliation.
    const SidecarOpPhase phase = slot->op->phase();
    const bool cleanly_terminal =
        (phase == SidecarOpPhase::Retired ||
         phase == SidecarOpPhase::AbortedPreDurable ||
         phase == SidecarOpPhase::TerminalStaged) &&
        !slot->op->reconcile_required();
    const bool resolved =
        slot->op->reconcile_required() && slot->op->reconciled();
    if (!cleanly_terminal && !resolved)
        return false;
    slot->in_use = false;
    slot->op.reset();
    slot->reader = FSessionFrameReader{};
    return true;
}

} // namespace icecc::p50::fsession
