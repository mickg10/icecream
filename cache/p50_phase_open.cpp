#include "p50_phase_open.h"

#include <algorithm>
#include <array>
#include <limits>

namespace icecc::p50 {
namespace {

constexpr std::array<uint8_t, 4> kMagic{'P', '5', '0', 'H'};
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

bool take_u16(std::span<const uint8_t> bytes, size_t& offset, uint16_t& value) {
    if (bytes.size() - offset < 2)
        return false;
    value = static_cast<uint16_t>(bytes[offset]) << 8 |
            static_cast<uint16_t>(bytes[offset + 1]);
    offset += 2;
    return true;
}

bool take_u32(std::span<const uint8_t> bytes, size_t& offset, uint32_t& value) {
    if (bytes.size() - offset < 4)
        return false;
    value = static_cast<uint32_t>(bytes[offset]) << 24 |
            static_cast<uint32_t>(bytes[offset + 1]) << 16 |
            static_cast<uint32_t>(bytes[offset + 2]) << 8 |
            static_cast<uint32_t>(bytes[offset + 3]);
    offset += 4;
    return true;
}

bool take_u64(std::span<const uint8_t> bytes, size_t& offset, uint64_t& value) {
    if (bytes.size() - offset < 8)
        return false;
    value = 0;
    for (unsigned index = 0; index != 8; ++index)
        value = (value << 8) | bytes[offset + index];
    offset += 8;
    return true;
}

std::vector<uint8_t> finish(PhaseOpenWirePhase phase, std::vector<uint8_t> body) {
    if (body.size() > std::numeric_limits<uint32_t>::max() ||
        body.size() > kMaxWireSize - kHeaderSize)
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

bool start(std::span<const uint8_t> wire, PhaseOpenWirePhase phase, size_t& offset) {
    if (wire.size() < kHeaderSize ||
        !std::equal(kMagic.begin(), kMagic.end(), wire.begin()))
        return false;
    offset = kMagic.size();
    uint16_t version = 0;
    uint16_t actual_phase = 0;
    uint32_t body_size = 0;
    if (!take_u16(wire, offset, version) || !take_u16(wire, offset, actual_phase) ||
        !take_u32(wire, offset, body_size) || version != kWireVersion ||
        actual_phase != static_cast<uint16_t>(phase) ||
        body_size != wire.size() - kHeaderSize || body_size > kMaxWireSize - kHeaderSize)
        return false;
    return true;
}

bool take_arm(std::span<const uint8_t> wire, size_t& offset, P50SourceArm& arm) {
    uint32_t size = 0;
    if (!take_u32(wire, offset, size) || size > wire.size() - offset)
        return false;
    const auto encoded = wire.subspan(offset, size);
    offset += size;
    const auto decoded = decode_source_arm(encoded);
    if (!decoded.has_value())
        return false;
    arm = *decoded;
    return true;
}

void put_arm(std::vector<uint8_t>& body, const P50SourceArm& arm) {
    const auto encoded = encode_source_arm(arm);
    put_u32(body, static_cast<uint32_t>(encoded.size()));
    body.insert(body.end(), encoded.begin(), encoded.end());
}

}  // namespace

bool HandoffOffer::valid() const noexcept {
    return request_id != 0 && source_arm.valid() &&
           request_id == source_arm.source_request_id &&
           cache_profile != 0 && cache_profile == source_arm.cache_profile;
}

bool AttachmentPhaseOpen::valid() const noexcept {
    return request_id != 0 && source_arm.valid() &&
           request_id == source_arm.source_request_id;
}

std::vector<uint8_t> encode_handoff_offer(const HandoffOffer& offer) {
    if (!offer.valid())
        return {};
    std::vector<uint8_t> body;
    put_u64(body, offer.request_id);
    put_u32(body, offer.cache_profile);
    put_arm(body, offer.source_arm);
    return finish(PhaseOpenWirePhase::HandoffOffer, std::move(body));
}

std::vector<uint8_t> encode_attachment_phase_open(const AttachmentPhaseOpen& open) {
    if (!open.valid())
        return {};
    std::vector<uint8_t> body;
    put_u64(body, open.request_id);
    put_arm(body, open.source_arm);
    return finish(PhaseOpenWirePhase::AttachmentPhaseOpen, std::move(body));
}

std::optional<HandoffOffer> decode_handoff_offer(std::span<const uint8_t> wire) {
    size_t offset = 0;
    if (!start(wire, PhaseOpenWirePhase::HandoffOffer, offset))
        return std::nullopt;
    HandoffOffer offer;
    if (!take_u64(wire, offset, offer.request_id) ||
        !take_u32(wire, offset, offer.cache_profile) ||
        !take_arm(wire, offset, offer.source_arm) || offset != wire.size() ||
        !offer.valid())
        return std::nullopt;
    return offer;
}

std::optional<AttachmentPhaseOpen> decode_attachment_phase_open(
    std::span<const uint8_t> wire) {
    size_t offset = 0;
    if (!start(wire, PhaseOpenWirePhase::AttachmentPhaseOpen, offset))
        return std::nullopt;
    AttachmentPhaseOpen open;
    if (!take_u64(wire, offset, open.request_id) ||
        !take_arm(wire, offset, open.source_arm) || offset != wire.size() ||
        !open.valid())
        return std::nullopt;
    return open;
}

OfferDecision P50HandoffAuthority::offer(const HandoffOffer& value) noexcept {
    if (!value.valid())
        return OfferDecision::Invalid;
    if (current_offer_.has_value() && value.request_id == current_offer_->request_id) {
        return value == *current_offer_ ? OfferDecision::ExactReplay
                                        : OfferDecision::Conflict;
    }
    if (value.request_id <= high_water_)
        return OfferDecision::StaleRequest;
    high_water_ = value.request_id;
    current_offer_ = value;
    current_phase_open_.reset();
    phase_opened_ = false;
    return OfferDecision::Accepted;
}

OfferDecision P50HandoffAuthority::phase_open(const AttachmentPhaseOpen& value) noexcept {
    if (!value.valid())
        return OfferDecision::Invalid;
    if (!current_offer_.has_value())
        return OfferDecision::NoOffer;
    if (value.request_id != current_offer_->request_id)
        return value.request_id <= high_water_ ? OfferDecision::StaleRequest
                                               : OfferDecision::NoOffer;
    if (value.source_arm != current_offer_->source_arm)
        return OfferDecision::Conflict;
    if (current_phase_open_.has_value())
        return value == *current_phase_open_ ? OfferDecision::ExactReplay
                                             : OfferDecision::Conflict;
    current_phase_open_ = value;
    phase_opened_ = true;
    return OfferDecision::Accepted;
}

PhaseOpenResult P50PhaseOpenState::install_offer(
    const HandoffOffer& offer, TimePoint now, TimePoint phase_open_deadline,
    TimePoint source_arrival_deadline) noexcept {
    if (state_ != PhaseOpenState::Idle || !offer.valid() || phase_open_deadline < now ||
        source_arrival_deadline < phase_open_deadline)
        return PhaseOpenResult::Invalid;
    offer_ = offer;
    ready_.reset();
    phase_open_deadline_ = phase_open_deadline;
    source_arrival_deadline_ = source_arrival_deadline;
    state_ = PhaseOpenState::Offered;
    return PhaseOpenResult::Accepted;
}

PhaseOpenResult P50PhaseOpenState::accept_phase_open(
    const AttachmentPhaseOpen& open, TimePoint now) noexcept {
    if (state_ != PhaseOpenState::Offered)
        return PhaseOpenResult::PhaseViolation;
    if (now > phase_open_deadline_) {
        state_ = PhaseOpenState::Expired;
        return PhaseOpenResult::DeadlineExpired;
    }
    if (!open.valid() || open.request_id != offer_.request_id)
        return PhaseOpenResult::WrongRequest;
    if (open.source_arm != offer_.source_arm)
        return PhaseOpenResult::WrongArm;
    state_ = PhaseOpenState::Established;
    return PhaseOpenResult::Accepted;
}

PhaseOpenResult P50PhaseOpenState::accept_source_arrival(
    const P50InputReady& ready, TimePoint now) noexcept {
    if (state_ != PhaseOpenState::Established)
        return PhaseOpenResult::PhaseViolation;
    if (now > source_arrival_deadline_) {
        state_ = PhaseOpenState::Expired;
        return PhaseOpenResult::DeadlineExpired;
    }
    if (!ready.valid() || ready.attachment_request_id != offer_.request_id)
        return PhaseOpenResult::WrongRequest;
    if (!ready.matches_arm(offer_.source_arm))
        return PhaseOpenResult::WrongArm;
    ready_ = ready;
    state_ = PhaseOpenState::SourceArrived;
    return PhaseOpenResult::Accepted;
}

void P50PhaseOpenState::close() noexcept {
    ready_.reset();
    if (state_ != PhaseOpenState::Closed)
        state_ = PhaseOpenState::Closed;
}

}  // namespace icecc::p50
