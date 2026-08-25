#include "p50_source_identity.h"

#include <algorithm>
#include <array>
#include <limits>

namespace icecc::p50 {
namespace {

constexpr std::array<uint8_t, 4> kMagic{'P', '5', '0', 'A'};
constexpr uint16_t kWireVersion = 50;
constexpr size_t kHeaderSize = 12;
constexpr size_t kMaxWireSize = 64u * 1024u;

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void put_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned shift = 56; shift != static_cast<unsigned>(-8); shift -= 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

void put_bytes(std::vector<uint8_t>& out, std::span<const uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void put_id(std::vector<uint8_t>& out, const Id128& id) {
    put_bytes(out, id.bytes);
}

void put_string(std::vector<uint8_t>& out, const std::string& value) {
    put_u32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    bool take_u16(uint16_t& value) {
        if (remaining() < 2)
            return false;
        value = static_cast<uint16_t>(bytes_[offset_]) << 8 |
                static_cast<uint16_t>(bytes_[offset_ + 1]);
        offset_ += 2;
        return true;
    }
    bool take_u32(uint32_t& value) {
        if (remaining() < 4)
            return false;
        value = static_cast<uint32_t>(bytes_[offset_]) << 24 |
                static_cast<uint32_t>(bytes_[offset_ + 1]) << 16 |
                static_cast<uint32_t>(bytes_[offset_ + 2]) << 8 |
                static_cast<uint32_t>(bytes_[offset_ + 3]);
        offset_ += 4;
        return true;
    }
    bool take_u64(uint64_t& value) {
        if (remaining() < 8)
            return false;
        value = 0;
        for (unsigned i = 0; i != 8; ++i)
            value = (value << 8) | bytes_[offset_ + i];
        offset_ += 8;
        return true;
    }
    template <typename T>
    bool take_id(T& value) {
        if (remaining() < value.bytes.size())
            return false;
        std::copy_n(bytes_.begin() + static_cast<ptrdiff_t>(offset_),
                    value.bytes.size(), value.bytes.begin());
        offset_ += value.bytes.size();
        return true;
    }
    bool take_string(std::string& value) {
        uint32_t size = 0;
        if (!take_u32(size) || size > remaining())
            return false;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }
    size_t remaining() const noexcept { return bytes_.size() - offset_; }
    bool skip(size_t count) {
        if (count > remaining())
            return false;
        offset_ += count;
        return true;
    }

private:
    std::span<const uint8_t> bytes_;
    size_t offset_ = 0;
};

void append_arm_body(std::vector<uint8_t>& out, const P50SourceArm& arm) {
    put_u32(out, arm.wire_job_id);
    put_u64(out, arm.assignment_epoch);
    put_u64(out, arm.assignment_nonce);
    put_string(out, arm.selected_f_host);
    put_u32(out, arm.selected_f_ordinary_port);
    put_u32(out, arm.selected_f_cache_port);
    put_u32(out, arm.cache_protocol);
    put_u32(out, arm.cache_profile);
    put_u64(out, arm.logical_job);
    put_u64(out, arm.attempt_id);
    put_u64(out, arm.c_store_generation);
    put_id(out, arm.c_store_guid);
    put_u64(out, arm.source_request_id);
    put_u32(out, arm.source_mode);
}

bool take_arm_body(Reader& reader, P50SourceArm& arm) {
    return reader.take_u32(arm.wire_job_id) &&
           reader.take_u64(arm.assignment_epoch) &&
           reader.take_u64(arm.assignment_nonce) &&
           reader.take_string(arm.selected_f_host) &&
           reader.take_u32(arm.selected_f_ordinary_port) &&
           reader.take_u32(arm.selected_f_cache_port) &&
           reader.take_u32(arm.cache_protocol) &&
           reader.take_u32(arm.cache_profile) &&
           reader.take_u64(arm.logical_job) && reader.take_u64(arm.attempt_id) &&
           reader.take_u64(arm.c_store_generation) && reader.take_id(arm.c_store_guid) &&
           reader.take_u64(arm.source_request_id) && reader.take_u32(arm.source_mode);
}

std::vector<uint8_t> finish(P50SourceWirePhase phase,
                            std::vector<uint8_t> body) {
    if (body.size() > std::numeric_limits<uint32_t>::max() ||
        body.size() + kHeaderSize > kMaxWireSize)
        return {};
    std::vector<uint8_t> wire;
    wire.reserve(kHeaderSize + body.size());
    wire.insert(wire.end(), kMagic.begin(), kMagic.end());
    put_u16(wire, kWireVersion);
    put_u16(wire, static_cast<uint16_t>(phase));
    put_u32(wire, static_cast<uint32_t>(body.size()));
    wire.insert(wire.end(), body.begin(), body.end());
    return wire;
}

bool start(Reader& reader, P50SourceWirePhase phase,
           std::span<const uint8_t> wire, uint32_t& body_size) {
    if (wire.size() < kHeaderSize || !std::equal(kMagic.begin(), kMagic.end(), wire.begin()))
        return false;
    uint16_t version = 0;
    uint16_t actual_phase = 0;
    if (!reader.skip(kMagic.size()) || !reader.take_u16(version) ||
        !reader.take_u16(actual_phase) ||
        !reader.take_u32(body_size) || version != kWireVersion ||
        actual_phase != static_cast<uint16_t>(phase) ||
        body_size != wire.size() - kHeaderSize || body_size > kMaxWireSize - kHeaderSize)
        return false;
    return true;
}

}  // namespace

bool P50SourceArm::valid() const noexcept {
    return wire_job_id != 0 && assignment_epoch != 0 && assignment_nonce != 0 &&
           !selected_f_host.empty() && selected_f_host.size() <= UINT16_MAX &&
           selected_f_ordinary_port != 0 &&
           selected_f_ordinary_port <= UINT16_MAX && selected_f_cache_port != 0 &&
           selected_f_cache_port <= UINT16_MAX && cache_protocol == 50 &&
           cache_profile != 0 && logical_job != 0 && attempt_id != 0 &&
           c_store_generation != 0 && c_store_guid != CStoreGuid{} &&
           source_request_id != 0 && source_mode != 0;
}

bool P50InputReady::valid() const noexcept {
    return arm.valid() && tu_seq.value != 0 && raw_bytes != 0 &&
           f_store_guid != FStoreGuid{} && attachment_store_generation != 0 &&
           attachment_request_id != 0 && ready_event_id != 0;
}

bool P50InputReady::matches_arm(const P50SourceArm& expected) const noexcept {
    return arm == expected;
}

std::vector<uint8_t> encode_source_arm(const P50SourceArm& arm) {
    if (!arm.valid())
        return {};
    std::vector<uint8_t> body;
    append_arm_body(body, arm);
    return finish(P50SourceWirePhase::Arm, std::move(body));
}

std::vector<uint8_t> encode_input_ready(const P50InputReady& ready) {
    if (!ready.valid())
        return {};
    std::vector<uint8_t> body;
    append_arm_body(body, ready.arm);
    put_u64(body, ready.tu_seq.value);
    put_u64(body, ready.raw_bytes);
    put_bytes(body, ready.raw_digest.bytes);
    put_id(body, ready.f_store_guid);
    put_u64(body, ready.attachment_store_generation);
    put_u64(body, ready.attachment_request_id);
    put_u64(body, ready.ready_event_id);
    return finish(P50SourceWirePhase::InputReady, std::move(body));
}

std::optional<P50SourceArm> decode_source_arm(std::span<const uint8_t> wire) {
    Reader reader(wire);
    uint32_t body_size = 0;
    if (!start(reader, P50SourceWirePhase::Arm, wire, body_size))
        return std::nullopt;
    P50SourceArm arm;
    if (!take_arm_body(reader, arm) || reader.remaining() != 0 || !arm.valid())
        return std::nullopt;
    return arm;
}

std::optional<P50InputReady> decode_input_ready(std::span<const uint8_t> wire) {
    Reader reader(wire);
    uint32_t body_size = 0;
    if (!start(reader, P50SourceWirePhase::InputReady, wire, body_size))
        return std::nullopt;
    P50InputReady ready;
    if (!take_arm_body(reader, ready.arm) || !reader.take_u64(ready.tu_seq.value) ||
        !reader.take_u64(ready.raw_bytes) || !reader.take_id(ready.raw_digest) ||
        !reader.take_id(ready.f_store_guid) ||
        !reader.take_u64(ready.attachment_store_generation) ||
        !reader.take_u64(ready.attachment_request_id) ||
        !reader.take_u64(ready.ready_event_id) || reader.remaining() != 0 ||
        !ready.valid())
        return std::nullopt;
    return ready;
}

}  // namespace icecc::p50
