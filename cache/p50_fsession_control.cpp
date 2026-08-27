#include "p50_fsession_control.h"

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

} // namespace icecc::p50::fsession
