#include "p50_fsession_control.h"

#include <algorithm>
#include <cstring>

namespace icecc::p50::fsession {
namespace {

// Big-endian scalar helpers ------------------------------------------------
void put_u16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 3; i >= 0; --i)
        b.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
void put_u64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 7; i >= 0; --i)
        b.push_back(static_cast<uint8_t>(v >> (i * 8)));
}
void put_bytes(std::vector<uint8_t>& b, std::span<const uint8_t> s) {
    b.insert(b.end(), s.begin(), s.end());
}

struct Reader {
    std::span<const uint8_t> data;
    size_t off = 0;
    bool ok = true;
    bool need(size_t n) noexcept {
        if (!ok || off + n > data.size()) {
            ok = false;
            return false;
        }
        return true;
    }
    uint16_t u16() noexcept {
        if (!need(2)) return 0;
        uint16_t v = static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
        off += 2;
        return v;
    }
    uint32_t u32() noexcept {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | data[off + i];
        off += 4;
        return v;
    }
    uint64_t u64() noexcept {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | data[off + i];
        off += 8;
        return v;
    }
    void bytes16(std::array<uint8_t, 16>& out) noexcept {
        if (!need(16)) return;
        for (int i = 0; i < 16; ++i) out[i] = data[off + i];
        off += 16;
    }
};

// Canonical identity (de)serialization ------------------------------------
void put_identity(std::vector<uint8_t>& b, const FSessionOperationIdentity& id) {
    put_u64(b, id.daemon_launch_generation);
    put_u64(b, id.control_connection_generation);
    put_u64(b, id.operation.sidecar_launch.generation);
    put_u64(b, id.operation.sidecar_launch.attempt);
    put_u16(b, static_cast<uint16_t>(id.operation.role));
    put_u64(b, id.operation.operation_sequence);
    put_bytes(b, id.c_store_guid.bytes);
    put_bytes(b, id.f_store_guid.bytes);
    put_u64(b, id.assignment_job);
    put_u64(b, id.assignment_epoch);
    put_u64(b, id.assignment_nonce);
    put_u64(b, id.arm_observation);
    put_u64(b, static_cast<uint64_t>(id.deadline.expires_at_ns));
    put_u64(b, id.deadline.clock_domain_id);
    put_u64(b, id.deadline.time_namespace_id);
}

FSessionOperationIdentity get_identity(Reader& r) {
    FSessionOperationIdentity id;
    id.daemon_launch_generation = r.u64();
    id.control_connection_generation = r.u64();
    id.operation.sidecar_launch.generation = r.u64();
    id.operation.sidecar_launch.attempt = r.u64();
    id.operation.role = static_cast<daemon::P50SessionOperationRole>(r.u16());
    id.operation.operation_sequence = r.u64();
    r.bytes16(id.c_store_guid.bytes);
    r.bytes16(id.f_store_guid.bytes);
    id.assignment_job = r.u64();
    id.assignment_epoch = r.u64();
    id.assignment_nonce = r.u64();
    id.arm_observation = r.u64();
    id.deadline.expires_at_ns = static_cast<int64_t>(r.u64());
    id.deadline.clock_domain_id = r.u64();
    id.deadline.time_namespace_id = r.u64();
    return id;
}

} // namespace

void encode_fsession_identity(std::vector<uint8_t>& out,
                              const FSessionOperationIdentity& identity) {
    put_identity(out, identity);
}

bool decode_fsession_identity(std::span<const uint8_t> bytes, size_t& offset,
                              FSessionOperationIdentity& identity) {
    Reader r{bytes, offset, true};
    identity = get_identity(r);
    if (!r.ok)
        return false;
    offset = r.off;
    return true;
}

bool direction_legal_daemon(DaemonToSidecarType type) noexcept {
    switch (type) {
    case DaemonToSidecarType::OperationOffer:
    case DaemonToSidecarType::PublicFdOffer:
    case DaemonToSidecarType::OpCancel:
    case DaemonToSidecarType::DaemonFdAccepted:
    case DaemonToSidecarType::DaemonFdRejected:
    case DaemonToSidecarType::TerminalAck:
        return true;
    }
    return false;
}

bool direction_legal_sidecar(SidecarToDaemonType type) noexcept {
    switch (type) {
    case SidecarToDaemonType::OperationAccepted:
    case SidecarToDaemonType::PublicFdAdoptedReceipt:
    case SidecarToDaemonType::EndpointObservation:
    case SidecarToDaemonType::InputCommitted:
    case SidecarToDaemonType::InputAbortedPreDurable:
    case SidecarToDaemonType::InputCancelledAfterCommit:
    case SidecarToDaemonType::DeliveryOffer:
    case SidecarToDaemonType::TerminalObservation:
        return true;
    }
    return false;
}

namespace {
bool message_type_legal(FSessionControlDirection dir, uint16_t type) noexcept {
    if (dir == FSessionControlDirection::DaemonToSidecar)
        return direction_legal_daemon(static_cast<DaemonToSidecarType>(type));
    if (dir == FSessionControlDirection::SidecarToDaemon)
        return direction_legal_sidecar(static_cast<SidecarToDaemonType>(type));
    return false;
}
} // namespace

bool FSessionControlEnvelope::header_valid() const noexcept {
    return identity.valid() && sequence != 0 &&
           (direction == FSessionControlDirection::DaemonToSidecar ||
            direction == FSessionControlDirection::SidecarToDaemon) &&
           message_type_legal(direction, message_type) &&
           payload.size() <= kFSessionControlMaxPayload;
}

std::optional<std::vector<uint8_t>>
encode_fsession_control(const FSessionControlEnvelope& e) {
    if (!e.header_valid())
        return std::nullopt;
    std::vector<uint8_t> b;
    put_u32(b, kFSessionControlMagic);
    put_u16(b, kFSessionControlVersion);
    b.push_back(static_cast<uint8_t>(e.direction));
    b.push_back(0); // reserved
    put_u16(b, e.message_type);
    put_u64(b, e.sequence);
    put_identity(b, e.identity);
    put_u32(b, static_cast<uint32_t>(e.payload.size()));
    put_bytes(b, e.payload);
    return b;
}

std::optional<FSessionControlEnvelope>
decode_fsession_control(std::span<const uint8_t> bytes) {
    Reader r{bytes};
    if (r.u32() != kFSessionControlMagic)
        return std::nullopt;
    if (r.u16() != kFSessionControlVersion)
        return std::nullopt;
    FSessionControlEnvelope e;
    if (!r.need(2))
        return std::nullopt;
    e.direction = static_cast<FSessionControlDirection>(bytes[r.off]);
    const uint8_t reserved = bytes[r.off + 1];
    r.off += 2;
    if (reserved != 0)
        return std::nullopt;
    e.message_type = r.u16();
    e.sequence = r.u64();
    e.identity = get_identity(r);
    const uint32_t len = r.u32();
    if (!r.ok || len > kFSessionControlMaxPayload || !r.need(len))
        return std::nullopt;
    e.payload.assign(bytes.begin() + static_cast<long>(r.off),
                     bytes.begin() + static_cast<long>(r.off + len));
    r.off += len;
    if (!r.ok || r.off != bytes.size()) // no trailing bytes
        return std::nullopt;
    if (!e.header_valid())
        return std::nullopt;
    return e;
}

InboundDisposition FSessionInboundControl::classify(
    const FSessionControlEnvelope& e, std::span<const uint8_t> bytes) {
    // A retired operation is never revived under the same identity (5444410383
    // sec.6). A byte-identical replay of a retained frame is still recognized
    // (zero mutation) so a peer's lost-ACK replay finds the tombstone; any
    // other frame is stale-only.
    if (retired_) {
        if (e.sequence >= 1 && e.sequence <= retained_.size()) {
            const std::vector<uint8_t>& kept =
                retained_[static_cast<size_t>(e.sequence - 1)];
            if (kept.size() == bytes.size() &&
                std::equal(kept.begin(), kept.end(), bytes.begin()))
                return InboundDisposition::ExactReplay;
        }
        return InboundDisposition::StaleWrongIdentity;
    }

    // Direction legality is enforced on EVERY frame, not only at row creation:
    // a wrong-direction envelope is rejected before any sequence/row logic.
    if (e.direction != expected_direction_)
        return InboundDisposition::StaleWrongIdentity;

    if (!row_created_) {
        // OperationOffer(seq==1) is the SOLE row-creation frame. Anything else
        // (an early OpCancel, a mis-sequenced offer) is phase-invalid and mints
        // no row / no AbortedPreDurable (fixes rejected-probe blocker 1).
        if (e.direction == FSessionControlDirection::DaemonToSidecar &&
            e.message_type ==
                static_cast<uint16_t>(DaemonToSidecarType::OperationOffer) &&
            e.sequence == 1 && e.identity.valid()) {
            identity_ = e.identity;
            row_created_ = true;
            retained_.emplace_back(bytes.begin(), bytes.end());
            next_expected_ = 2;
            return InboundDisposition::AcceptedNew;
        }
        return InboundDisposition::PhaseInvalidNoRow;
    }

    // Row exists: the complete operation identity must match exactly. A frame
    // naming a different operation is stale-only and mutates nothing.
    if (!(e.identity == identity_))
        return InboundDisposition::StaleWrongIdentity;

    if (e.sequence == next_expected_) {
        retained_.emplace_back(bytes.begin(), bytes.end());
        ++next_expected_;
        return InboundDisposition::AcceptedNew;
    }

    if (e.sequence < next_expected_) {
        // Accept ONLY a byte-identical replay of the retained frame at that
        // sequence -> replay the prior response with zero owner mutation. A
        // changed re-offer / non-identical duplicate is an op-local protocol
        // error and cannot reset the row (fixes blocker 2).
        if (e.sequence >= 1 && e.sequence <= retained_.size()) {
            const std::vector<uint8_t>& kept =
                retained_[static_cast<size_t>(e.sequence - 1)];
            if (kept.size() == bytes.size() &&
                std::equal(kept.begin(), kept.end(), bytes.begin()))
                return InboundDisposition::ExactReplay;
        }
        return InboundDisposition::DuplicateConflict;
    }

    // seq > next_expected: a gap. No buffering that could hide a missing
    // authority transition.
    return InboundDisposition::Gap;
}

// --- FSessionFrameReader ---------------------------------------------------

FSessionFrameReader::Status FSessionFrameReader::validate_prefix() noexcept {
    if (buffer_.size() >= 4) {
        uint32_t magic = 0;
        for (int i = 0; i < 4; ++i)
            magic = (magic << 8) | buffer_[static_cast<size_t>(i)];
        if (magic != kFSessionControlMagic) {
            error_ = true;
            return Status::Error;
        }
    }
    if (buffer_.size() >= 6) {
        const uint16_t version =
            static_cast<uint16_t>((buffer_[4] << 8) | buffer_[5]);
        if (version != kFSessionControlVersion) {
            error_ = true;
            return Status::Error;
        }
    }
    if (buffer_.size() >= kFSessionControlPrefixBytes) {
        uint32_t payload_len = 0;
        for (size_t i = kFSessionControlPrefixBytes - 4;
             i < kFSessionControlPrefixBytes; ++i)
            payload_len = (payload_len << 8) | buffer_[i];
        if (payload_len > kFSessionControlMaxPayload) {
            error_ = true;
            return Status::Error;
        }
        if (buffer_.size() >= kFSessionControlPrefixBytes + payload_len)
            return Status::FrameReady;
    }
    return Status::NeedMore;
}

FSessionFrameReader::Status
FSessionFrameReader::feed(std::span<const uint8_t> bytes) {
    if (error_)
        return Status::Error;
    // Bounded accumulation: one maximal frame plus one maximal read's worth.
    if (buffer_.size() + bytes.size() >
        2 * (kFSessionControlPrefixBytes + kFSessionControlMaxPayload)) {
        error_ = true;
        return Status::Error;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    return validate_prefix();
}

std::optional<std::vector<uint8_t>> FSessionFrameReader::next_frame() {
    if (error_ || validate_prefix() != Status::FrameReady)
        return std::nullopt;
    uint32_t payload_len = 0;
    for (size_t i = kFSessionControlPrefixBytes - 4;
         i < kFSessionControlPrefixBytes; ++i)
        payload_len = (payload_len << 8) | buffer_[i];
    const size_t total = kFSessionControlPrefixBytes + payload_len;
    std::vector<uint8_t> frame(buffer_.begin(),
                               buffer_.begin() + static_cast<long>(total));
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(total));
    return frame;
}

// --- FSessionOutboundControl ----------------------------------------------

size_t FSessionOutboundControl::live_slots() const noexcept {
    size_t n = 0;
    for (const auto& s : slots_)
        if (s.occupied())
            ++n;
    return n;
}

std::vector<uint64_t> FSessionOutboundControl::pending_sequences() const {
    std::vector<uint64_t> pending;
    for (const auto& s : slots_) {
        if (s.state == OutboundSlotState::Queued ||
            s.state == OutboundSlotState::Writing)
            pending.push_back(s.sequence);
    }
    std::sort(pending.begin(), pending.end());
    return pending;
}

const OutboundSemanticSlot*
FSessionOutboundControl::find(uint64_t sequence) const noexcept {
    for (const auto& s : slots_)
        if (s.sequence == sequence && s.state != OutboundSlotState::Empty)
            return &s;
    return nullptr;
}

OutboundSemanticSlot*
FSessionOutboundControl::mutable_find(uint64_t sequence) noexcept {
    for (auto& s : slots_)
        if (s.sequence == sequence && s.state != OutboundSlotState::Empty)
            return &s;
    return nullptr;
}

uint64_t FSessionOutboundControl::reserve(uint16_t message_type) {
    if (live_slots() >= max_live_)
        return 0; // fail closed: never grow an unbounded log
    const uint64_t sequence = next_sequence_++;
    // Reuse a retired/empty slot's storage so slots_ stays bounded by the live
    // cap even across long operations (no unbounded message log).
    for (auto& s : slots_) {
        if (s.state == OutboundSlotState::Retired ||
            s.state == OutboundSlotState::Empty) {
            s = OutboundSemanticSlot{};
            s.sequence = sequence;
            s.message_type = message_type;
            s.state = OutboundSlotState::Reserved;
            return sequence;
        }
    }
    OutboundSemanticSlot slot;
    slot.sequence = sequence;
    slot.message_type = message_type;
    slot.state = OutboundSlotState::Reserved;
    slots_.push_back(std::move(slot));
    return sequence;
}

bool FSessionOutboundControl::stage(uint64_t sequence,
                                    std::span<const uint8_t> canonical_bytes) {
    OutboundSemanticSlot* slot = mutable_find(sequence);
    if (slot == nullptr || slot->state != OutboundSlotState::Reserved)
        return false;
    slot->canonical_bytes.assign(canonical_bytes.begin(), canonical_bytes.end());
    slot->state = OutboundSlotState::Queued;
    return true;
}

uint64_t
FSessionOutboundControl::enqueue_idempotent(uint16_t message_type,
                                            std::span<const uint8_t> bytes) {
    // At most one canonical frame per semantic transition: a byte-identical live
    // frame of this exact type reuses its sequence rather than appending another.
    for (const auto& s : slots_) {
        if (s.occupied() && s.state != OutboundSlotState::Reserved &&
            s.message_type == message_type &&
            s.canonical_bytes.size() == bytes.size() &&
            std::equal(s.canonical_bytes.begin(), s.canonical_bytes.end(),
                       bytes.begin()))
            return s.sequence;
    }
    const uint64_t sequence = reserve(message_type);
    if (sequence == 0)
        return 0;
    if (!stage(sequence, bytes))
        return 0;
    return sequence;
}

uint64_t
FSessionOutboundControl::stage_frame(const FSessionOperationIdentity& identity,
                                     FSessionControlDirection direction,
                                     uint16_t message_type,
                                     std::span<const uint8_t> payload) {
    // Direction legality and identity validity gate EVERY path, including the
    // duplicate fast path (root probe controls 1/2).
    if (!identity.valid())
        return 0;
    const bool legal =
        direction == FSessionControlDirection::DaemonToSidecar
            ? direction_legal_daemon(static_cast<DaemonToSidecarType>(message_type))
            : (direction == FSessionControlDirection::SidecarToDaemon &&
               direction_legal_sidecar(
                   static_cast<SidecarToDaemonType>(message_type)));
    if (!legal)
        return 0;
    // Semantic dup: the same transition is exact identity+direction+type+body.
    for (const auto& s : slots_) {
        if (s.occupied() && s.state != OutboundSlotState::Reserved &&
            s.message_type == message_type && s.direction == direction &&
            s.identity == identity &&
            s.semantic_payload.size() == payload.size() &&
            std::equal(s.semantic_payload.begin(), s.semantic_payload.end(),
                       payload.begin()))
            return s.sequence;
    }
    const uint64_t sequence = reserve(message_type);
    if (sequence == 0)
        return 0;
    FSessionControlEnvelope envelope;
    envelope.identity = identity;
    envelope.direction = direction;
    envelope.message_type = message_type;
    envelope.sequence = sequence;
    envelope.payload.assign(payload.begin(), payload.end());
    const auto encoded = encode_fsession_control(envelope);
    if (!encoded.has_value()) {
        (void)retire(sequence); // release the reservation; nothing escaped
        return 0;
    }
    OutboundSemanticSlot* slot = mutable_find(sequence);
    slot->canonical_bytes = *encoded;
    slot->semantic_payload.assign(payload.begin(), payload.end());
    slot->identity = identity;
    slot->direction = direction;
    slot->state = OutboundSlotState::Queued;
    return sequence;
}

bool FSessionOutboundControl::record_written(uint64_t sequence, size_t nbytes) {
    OutboundSemanticSlot* slot = mutable_find(sequence);
    if (slot == nullptr || (slot->state != OutboundSlotState::Queued &&
                            slot->state != OutboundSlotState::Writing))
        return false;
    slot->state = OutboundSlotState::Writing;
    slot->write_offset += nbytes;
    if (slot->write_offset >= slot->canonical_bytes.size()) {
        slot->write_offset = slot->canonical_bytes.size();
        slot->state = OutboundSlotState::FullyFlushed;
    }
    return true;
}

bool FSessionOutboundControl::mark_acked(uint64_t sequence) {
    OutboundSemanticSlot* slot = mutable_find(sequence);
    if (slot == nullptr || slot->state != OutboundSlotState::FullyFlushed)
        return false;
    slot->state = OutboundSlotState::AckedRetained;
    return true;
}

bool FSessionOutboundControl::retire(uint64_t sequence) {
    OutboundSemanticSlot* slot = mutable_find(sequence);
    if (slot == nullptr)
        return false;
    slot->state = OutboundSlotState::Retired;
    return true;
}

} // namespace icecc::p50::fsession
