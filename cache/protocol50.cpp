#include "protocol50.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace icecc::p50 {

std::string_view profile_name(ProfileId profile) {
    switch (profile) {
    case ProfileId::P29:
        return "p29";
    case ProfileId::ZSTD_TU:
        return "zstd_tu";
    case ProfileId::GRZ:
        return "grz";
    case ProfileId::Z3_LONG:
        return "z3_long";
    case ProfileId::Z3_SHARED_LONG:
        return "z3_shared_long";
    }
    return "unknown";
}

namespace {

bool known_object_type(ObjectType type) {
    const uint8_t value = static_cast<uint8_t>(type);
    return value >= static_cast<uint8_t>(ObjectType::Atom) &&
           value <= static_cast<uint8_t>(ObjectType::Blob);
}

bool known_message_type(MessageType type) {
    const uint8_t value = static_cast<uint8_t>(type);
    return value >= static_cast<uint8_t>(MessageType::SESSION_HELLO) &&
           value <= static_cast<uint8_t>(MessageType::TX_COMMIT);
}

bool known_profile(ProfileId profile) {
    return profile == ProfileId::P29 || profile == ProfileId::ZSTD_TU ||
           profile == ProfileId::GRZ;
}

bool valid_root_mode(ProfileId profile, P29RootMode mode) {
    if (profile == ProfileId::P29)
        return mode == P29RootMode::RouteHistory ||
               mode == P29RootMode::HistoryIndependent;
    return known_profile(profile) && mode == P29RootMode::NotApplicable;
}

void validate_limits(const SessionLimits& limits) {
    if (limits.max_frame_payload < kMandatoryControlFramePayload ||
        limits.max_frame_payload > kInitialMaxFramePayload)
        throw std::invalid_argument(
            "session frame cap cannot carry every mandatory V1 control frame");
    if (limits.max_fill_record_bytes < 32)
        throw std::invalid_argument("session FILL-record cap is smaller than its prefix");
}

void validate_hello(const SessionHello& hello) {
    if (hello.min_protocol == 0 || hello.min_protocol > hello.max_protocol)
        throw std::invalid_argument("session protocol range is invalid");
    if (hello.c_store_guid == CStoreGuid{})
        throw std::invalid_argument("SESSION_HELLO C_STORE_GUID zero is reserved");
    if (hello.supported_profiles == 0)
        throw std::invalid_argument("session has no supported profile");
    validate_limits(hello.limits);
}

void validate_commit(const TxCommit& commit) {
    if (commit.history_nonce.value == 0)
        throw std::invalid_argument("TX_COMMIT HISTORY_NONCE zero is reserved");
}

void validate_tx_begin_intrinsic(const TxBegin& begin) {
    if (begin.history_nonce.value == 0)
        throw std::invalid_argument("TX_BEGIN HISTORY_NONCE zero is reserved");
    if (!valid_root_mode(begin.profile, begin.p29_root_mode))
        throw std::invalid_argument("TX_BEGIN profile/root mode is invalid");
}

void validate_session_state_intrinsic(const SessionState& state) {
    if (state.selected_protocol != kProtocolVersion || state.negotiated_profiles == 0 ||
        (state.negotiated_profiles & ~kKnownProfileMask) != 0)
        throw std::invalid_argument(
            "SESSION_STATE selected an unimplemented protocol or profile mask");
    if (state.f_store_guid == FStoreGuid{})
        throw std::invalid_argument("SESSION_STATE F_STORE_GUID zero is reserved");
    validate_limits(state.limits);
    if (state.route_present && !state.namespace_present)
        throw std::invalid_argument("SESSION_STATE route lacks its C namespace");
    if (state.route_present && state.history_nonce.value == 0)
        throw std::invalid_argument(
            "SESSION_STATE present route has reserved zero HISTORY_NONCE");
    if (!state.route_present &&
        (state.history_nonce.value != 0 || state.next_rel_seq.value != 0 ||
         state.state_digest != Digest128{} || state.last_commit))
        throw std::invalid_argument(
            "SESSION_STATE absent route did not use the canonical zero form");
    if (state.last_commit && !state.route_present)
        throw std::invalid_argument("SESSION_STATE commit lacks route state");
    if (state.last_commit) {
        const TxCommit& commit = *state.last_commit;
        validate_commit(commit);
        if (commit.history_nonce != state.history_nonce)
            throw std::invalid_argument(
                "SESSION_STATE retained commit nonce differs from its route");
        if (state.next_rel_seq.value == 0 ||
            commit.rel_seq.value != state.next_rel_seq.value - 1)
            throw std::invalid_argument(
                "SESSION_STATE retained commit does not precede next REL_SEQ");
        if (commit.post_state_digest != state.state_digest)
            throw std::invalid_argument(
                "SESSION_STATE retained commit differs from its route digest");
    } else if (state.route_present && state.next_rel_seq.value != 0) {
        throw std::invalid_argument(
            "SESSION_STATE advanced route lacks its latest commit");
    }
}

void validate_history_reset(const HistoryReset& reset) {
    if (reset.history_nonce.value == 0)
        throw std::invalid_argument("HISTORY_RESET nonce zero is reserved");
}

void validate_error(const ErrorMessage& error) {
    if (error.code == 0)
        throw std::invalid_argument("ERROR code zero is reserved");
}

class Encoder {
public:
    void u8(uint8_t value) { bytes_.push_back(value); }
    void u16(uint16_t value) {
        u8(static_cast<uint8_t>(value >> 8));
        u8(static_cast<uint8_t>(value));
    }
    void u32(uint32_t value) {
        u8(static_cast<uint8_t>(value >> 24));
        u8(static_cast<uint8_t>(value >> 16));
        u8(static_cast<uint8_t>(value >> 8));
        u8(static_cast<uint8_t>(value));
    }
    void u64(uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            u8(static_cast<uint8_t>(value >> shift));
    }
    void bytes(std::span<const uint8_t> value) {
        if (value.size() > bytes_.max_size() - bytes_.size())
            throw std::overflow_error("encoded message exceeds addressable size");
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void id(const Id128& value) { bytes(value.bytes); }
    void digest(const Digest128& value) { bytes(value.bytes); }
    void string(const std::string& value) {
        if (value.size() > std::numeric_limits<uint32_t>::max())
            throw std::overflow_error("wire string exceeds u32");
        u32(static_cast<uint32_t>(value.size()));
        bytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()),
                                       value.size()));
    }
    std::vector<uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<uint8_t> bytes_;
};

class Decoder {
public:
    explicit Decoder(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    uint8_t u8() {
        require(1);
        return bytes_[offset_++];
    }
    uint16_t u16() {
        const uint16_t a = u8();
        return static_cast<uint16_t>((a << 8) | u8());
    }
    uint32_t u32() {
        uint32_t result = 0;
        for (unsigned i = 0; i != 4; ++i) result = (result << 8) | u8();
        return result;
    }
    uint64_t u64() {
        uint64_t result = 0;
        for (unsigned i = 0; i != 8; ++i) result = (result << 8) | u8();
        return result;
    }
    std::vector<uint8_t> bytes(size_t count) {
        require(count);
        std::vector<uint8_t> result(bytes_.begin() + offset_,
                                    bytes_.begin() + offset_ + count);
        offset_ += count;
        return result;
    }
    Id128 id() {
        Id128 result;
        require(result.bytes.size());
        std::copy_n(bytes_.begin() + offset_, result.bytes.size(), result.bytes.begin());
        offset_ += result.bytes.size();
        return result;
    }
    Digest128 digest() {
        Digest128 result;
        require(result.bytes.size());
        std::copy_n(bytes_.begin() + offset_, result.bytes.size(), result.bytes.begin());
        offset_ += result.bytes.size();
        return result;
    }
    std::string string() {
        const uint32_t count = u32();
        require(count);
        std::string result(reinterpret_cast<const char*>(bytes_.data() + offset_), count);
        offset_ += count;
        return result;
    }
    [[nodiscard]] size_t remaining() const { return bytes_.size() - offset_; }
    void exact_end() const {
        if (remaining() != 0) throw std::invalid_argument("wire payload has trailing bytes");
    }

private:
    void require(size_t count) const {
        if (count > bytes_.size() - offset_)
            throw std::invalid_argument("wire payload ended early");
    }

    std::span<const uint8_t> bytes_;
    size_t offset_ = 0;
};

void encode_descriptor(Encoder& out, const ComponentDescriptor& descriptor) {
    out.u16(descriptor.encoding);
    out.u64(descriptor.encoded_bytes);
    out.u64(descriptor.decoded_bytes);
    out.digest(descriptor.digest);
}

ComponentDescriptor decode_descriptor(Decoder& in) {
    ComponentDescriptor result;
    result.encoding = in.u16();
    result.encoded_bytes = in.u64();
    result.decoded_bytes = in.u64();
    result.digest = in.digest();
    return result;
}

void encode_commit(Encoder& out, const TxCommit& commit) {
    validate_commit(commit);
    out.u64(commit.history_nonce.value);
    out.u64(commit.rel_seq.value);
    out.u64(commit.tu_seq.value);
    out.digest(commit.transaction_digest);
    out.digest(commit.raw_digest);
    out.digest(commit.post_state_digest);
}

TxCommit decode_commit(Decoder& in) {
    TxCommit result;
    result.history_nonce.value = in.u64();
    result.rel_seq.value = in.u64();
    result.tu_seq.value = in.u64();
    result.transaction_digest = in.digest();
    result.raw_digest = in.digest();
    result.post_state_digest = in.digest();
    validate_commit(result);
    return result;
}

void append_varint(std::vector<uint8_t>& output, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        if (value) byte |= 0x80;
        output.push_back(byte);
    } while (value);
}

uint64_t read_varint(std::span<const uint8_t> input, size_t& offset) {
    uint64_t result = 0;
    unsigned shift = 0;
    while (offset < input.size() && shift < 64) {
        const uint8_t byte = input[offset++];
        if (shift == 63 && (byte & 0x7e) != 0)
            throw std::invalid_argument("delta varint exceeds u64");
        result |= uint64_t(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            if (shift != 0 && (byte & 0x7f) == 0)
                throw std::invalid_argument("delta varint is not minimally encoded");
            return result;
        }
        shift += 7;
    }
    throw std::invalid_argument("incomplete or oversized delta varint");
}

void append_digest_field(Digest128Builder& out, const Digest128& value) {
    out.append_digest(value);
}

void append_descriptor(Digest128Builder& out,
                       const ComponentDescriptor& descriptor) {
    out.append_u16(descriptor.encoding);
    out.append_u64(descriptor.encoded_bytes);
    out.append_u64(descriptor.decoded_bytes);
    append_digest_field(out, descriptor.digest);
}

}  // namespace

void validate_session_state(const SessionHello& hello,
                            const SessionState& received_state) {
    validate_hello(hello);
    validate_session_state_intrinsic(received_state);
    if (received_state.selected_protocol != kProtocolVersion ||
        hello.min_protocol > kProtocolVersion ||
        hello.max_protocol < kProtocolVersion)
        throw std::invalid_argument(
            "SESSION_STATE did not select implemented Protocol 50");
    if ((received_state.negotiated_profiles & hello.supported_profiles) !=
        received_state.negotiated_profiles)
        throw std::invalid_argument(
            "SESSION_STATE profiles were outside the client offer");
    if (received_state.limits.max_frame_payload >
        hello.limits.max_frame_payload)
        throw std::invalid_argument(
            "SESSION_STATE frame cap exceeded the client offer");
    if (received_state.limits.max_fill_record_bytes >
        hello.limits.max_fill_record_bytes)
        throw std::invalid_argument(
            "SESSION_STATE FILL-record cap exceeded the client offer");
}

SessionSelection negotiate_session(const SessionHello& hello,
                                   uint16_t server_min_protocol,
                                   uint16_t server_max_protocol,
                                   uint32_t server_profiles,
                                   SessionLimits server_limits) {
    validate_hello(hello);
    if (server_min_protocol == 0 || server_min_protocol > server_max_protocol)
        throw std::invalid_argument("session protocol range is reversed");
    validate_limits(server_limits);
    if (hello.min_protocol > kProtocolVersion ||
        hello.max_protocol < kProtocolVersion ||
        server_min_protocol > kProtocolVersion ||
        server_max_protocol < kProtocolVersion)
        throw std::invalid_argument(
            "peers do not both implement Protocol 50");

    const uint32_t common = hello.supported_profiles & server_profiles & kKnownProfileMask;
    if (common == 0) throw std::invalid_argument("session profiles do not overlap");
    return {kProtocolVersion, common,
            {std::min(hello.limits.max_frame_payload,
                      server_limits.max_frame_payload),
             std::min(hello.limits.max_fill_record_bytes,
                      server_limits.max_fill_record_bytes)}};
}

Id128 Id128::from_u64(uint64_t value) {
    Id128 result;
    static constexpr std::array<uint8_t, 8> prefix{'P', '5', '0', 'G', 'U', 'I', 'D', 1};
    std::copy(prefix.begin(), prefix.end(), result.bytes.begin());
    for (size_t i = 0; i != 8; ++i)
        result.bytes[8 + i] = static_cast<uint8_t>(value >> (56 - i * 8));
    return result;
}

size_t Id128Hash::operator()(const Id128& value) const noexcept {
    uint64_t first = 0;
    uint64_t second = 0;
    std::memcpy(&first, value.bytes.data(), sizeof(first));
    std::memcpy(&second, value.bytes.data() + sizeof(first), sizeof(second));
    const size_t a = std::hash<uint64_t>{}(first);
    const size_t b = std::hash<uint64_t>{}(second);
    return a ^ (b + size_t{0x9e3779b9} + (a << 6) + (a >> 2));
}

std::optional<Key64> Key64::make(ObjectType type, uint16_t generation,
                                 uint64_t ordinal) {
    if (!known_object_type(type) || generation > KeyLayoutV1::generation_value_mask ||
        ordinal == 0 || ordinal > KeyLayoutV1::ordinal_mask)
        return std::nullopt;
    const uint64_t value =
        (uint64_t(static_cast<uint8_t>(type)) << KeyLayoutV1::type_shift) |
        (uint64_t(generation) << KeyLayoutV1::generation_shift) | ordinal;
    return Key64(value);
}

std::optional<Key64> Key64::from_wire(uint64_t value) {
    const Key64 result(value);
    return result.valid() ? std::optional<Key64>(result) : std::nullopt;
}

bool Key64::valid() const {
    return known_object_type(type()) && ordinal() != 0;
}

ObjectType Key64::type() const {
    return static_cast<ObjectType>((value_ & KeyLayoutV1::type_mask) >>
                                   KeyLayoutV1::type_shift);
}

uint16_t Key64::generation() const {
    return static_cast<uint16_t>((value_ & KeyLayoutV1::generation_mask) >>
                                 KeyLayoutV1::generation_shift);
}

uint64_t Key64::ordinal() const { return value_ & KeyLayoutV1::ordinal_mask; }

size_t Key64Hash::operator()(Key64 value) const noexcept {
    return std::hash<uint64_t>{}(value.wire_value());
}

MessageType message_type(const Message& message) {
    return std::visit([](const auto& value) -> MessageType {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SessionHello>) return MessageType::SESSION_HELLO;
        if constexpr (std::is_same_v<T, SessionState>) return MessageType::SESSION_STATE;
        if constexpr (std::is_same_v<T, HistoryReset>) return MessageType::HISTORY_RESET;
        if constexpr (std::is_same_v<T, ErrorMessage>) return MessageType::ERROR;
        if constexpr (std::is_same_v<T, TxBegin>) return MessageType::TX_BEGIN;
        if constexpr (std::is_same_v<T, DictMessage>) return MessageType::DICT;
        if constexpr (std::is_same_v<T, BodyMessage>) return MessageType::BODY;
        if constexpr (std::is_same_v<T, NeedMessage>) return MessageType::NEED;
        if constexpr (std::is_same_v<T, FillMessage>) return MessageType::FILL;
        return MessageType::TX_COMMIT;
    }, message);
}

std::vector<uint8_t> encode_payload(const Message& message) {
    Encoder out;
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SessionHello>) {
            validate_hello(value);
            out.u16(value.min_protocol);
            out.u16(value.max_protocol);
            out.id(value.c_store_guid);
            out.u32(value.supported_profiles);
            out.u32(value.limits.max_frame_payload);
            out.u64(value.limits.max_fill_record_bytes);
        } else if constexpr (std::is_same_v<T, SessionState>) {
            validate_session_state_intrinsic(value);
            out.u16(value.selected_protocol);
            out.u32(value.negotiated_profiles);
            out.u32(value.limits.max_frame_payload);
            out.u64(value.limits.max_fill_record_bytes);
            out.id(value.f_store_guid);
            out.u8((value.namespace_present ? 1 : 0) |
                   (value.route_present ? 2 : 0) |
                   (value.last_commit ? 4 : 0));
            out.u64(value.history_nonce.value);
            out.u64(value.next_rel_seq.value);
            out.digest(value.state_digest);
            if (value.last_commit) encode_commit(out, *value.last_commit);
        } else if constexpr (std::is_same_v<T, HistoryReset>) {
            validate_history_reset(value);
            out.u64(value.history_nonce.value);
            out.digest(value.initial_state_digest);
        } else if constexpr (std::is_same_v<T, ErrorMessage>) {
            validate_error(value);
            out.u16(value.code);
            out.string(value.detail);
        } else if constexpr (std::is_same_v<T, TxBegin>) {
            validate_tx_begin_intrinsic(value);
            out.u64(value.history_nonce.value);
            out.u64(value.rel_seq.value);
            out.u64(value.tu_seq.value);
            out.u16(static_cast<uint16_t>(value.profile));
            out.u16(static_cast<uint16_t>(value.p29_root_mode));
            out.digest(value.pre_state_digest);
            encode_descriptor(out, value.dict);
            encode_descriptor(out, value.body);
            out.u64(value.raw_bytes);
            out.digest(value.raw_digest);
            out.digest(value.transaction_digest);
        } else if constexpr (std::is_same_v<T, DictMessage> ||
                             std::is_same_v<T, BodyMessage> ||
                             std::is_same_v<T, NeedMessage> ||
                             std::is_same_v<T, FillMessage>) {
            out.bytes(value.bytes);
        } else {
            encode_commit(out, value);
        }
    }, message);
    return out.take();
}

Message decode_payload(MessageType type, std::span<const uint8_t> payload) {
    if (!known_message_type(type)) throw std::invalid_argument("unknown message type");
    Decoder in(payload);
    switch (type) {
    case MessageType::SESSION_HELLO: {
        SessionHello value;
        value.min_protocol = in.u16();
        value.max_protocol = in.u16();
        value.c_store_guid = in.id();
        value.supported_profiles = in.u32();
        value.limits.max_frame_payload = in.u32();
        value.limits.max_fill_record_bytes = in.u64();
        in.exact_end();
        validate_hello(value);
        return value;
    }
    case MessageType::SESSION_STATE: {
        SessionState value;
        value.selected_protocol = in.u16();
        value.negotiated_profiles = in.u32();
        value.limits.max_frame_payload = in.u32();
        value.limits.max_fill_record_bytes = in.u64();
        value.f_store_guid = in.id();
        const uint8_t flags = in.u8();
        if (flags & ~uint8_t{7}) throw std::invalid_argument("SESSION_STATE flags are invalid");
        value.namespace_present = flags & 1;
        value.route_present = flags & 2;
        value.history_nonce.value = in.u64();
        value.next_rel_seq.value = in.u64();
        value.state_digest = in.digest();
        if (flags & 4) value.last_commit = decode_commit(in);
        in.exact_end();
        validate_session_state_intrinsic(value);
        return value;
    }
    case MessageType::HISTORY_RESET: {
        HistoryReset value;
        value.history_nonce.value = in.u64();
        value.initial_state_digest = in.digest();
        in.exact_end();
        validate_history_reset(value);
        return value;
    }
    case MessageType::ERROR: {
        ErrorMessage value;
        value.code = in.u16();
        value.detail = in.string();
        in.exact_end();
        validate_error(value);
        return value;
    }
    case MessageType::TX_BEGIN: {
        TxBegin value;
        value.history_nonce.value = in.u64();
        value.rel_seq.value = in.u64();
        value.tu_seq.value = in.u64();
        value.profile = static_cast<ProfileId>(in.u16());
        value.p29_root_mode = static_cast<P29RootMode>(in.u16());
        value.pre_state_digest = in.digest();
        value.dict = decode_descriptor(in);
        value.body = decode_descriptor(in);
        value.raw_bytes = in.u64();
        value.raw_digest = in.digest();
        value.transaction_digest = in.digest();
        in.exact_end();
        validate_tx_begin_intrinsic(value);
        return value;
    }
    case MessageType::DICT: return DictMessage{in.bytes(in.remaining())};
    case MessageType::BODY: return BodyMessage{in.bytes(in.remaining())};
    case MessageType::NEED: return NeedMessage{in.bytes(in.remaining())};
    case MessageType::FILL: return FillMessage{in.bytes(in.remaining())};
    case MessageType::TX_COMMIT: {
        TxCommit value = decode_commit(in);
        in.exact_end();
        return value;
    }
    }
    throw std::invalid_argument("unknown message type");
}

std::array<uint8_t, 4> encode_frame_header(MessageType type, uint32_t payload_bytes) {
    if (!known_message_type(type)) throw std::invalid_argument("unknown message type");
    if (payload_bytes > kInitialMaxFramePayload || payload_bytes > 0x00ffffffU)
        throw std::length_error("frame payload exceeds the initial Protocol-50 cap");
    const uint32_t word = (uint32_t(static_cast<uint8_t>(type)) << 24) | payload_bytes;
    return {static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
            static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
}

FrameHeader decode_frame_header(std::span<const uint8_t> header,
                                uint32_t max_payload) {
    if (header.size() != 4)
        throw std::invalid_argument("Protocol-50 frame header is not four bytes");
    if (max_payload == 0 || max_payload > kInitialMaxFramePayload)
        throw std::invalid_argument("frame cap is outside the initial Protocol-50 range");
    const uint32_t word = (uint32_t(header[0]) << 24) |
                          (uint32_t(header[1]) << 16) |
                          (uint32_t(header[2]) << 8) | uint32_t(header[3]);
    const MessageType type = static_cast<MessageType>(word >> 24);
    const uint32_t payload_bytes = word & 0x00ffffffU;
    if (!known_message_type(type)) throw std::invalid_argument("unknown frame type");
    if (payload_bytes > max_payload)
        throw std::length_error("frame payload exceeds configured cap");
    return {type, payload_bytes};
}

std::vector<uint8_t> encode_frame(const Message& message) {
    std::vector<uint8_t> payload = encode_payload(message);
    if (payload.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("frame payload does not fit u32");
    const auto header = encode_frame_header(message_type(message),
                                            static_cast<uint32_t>(payload.size()));
    std::vector<uint8_t> result(header.size() + payload.size());
    std::copy(header.begin(), header.end(), result.begin());
    std::copy(payload.begin(), payload.end(), result.begin() + header.size());
    return result;
}

FrameParser::FrameParser(uint32_t max_payload) : max_payload_(max_payload) {
    if (max_payload == 0 || max_payload > kInitialMaxFramePayload)
        throw std::invalid_argument("parser cap is outside the initial Protocol-50 range");
}

std::vector<Frame> FrameParser::feed(std::span<const uint8_t> bytes) {
    if (bytes.size() > buffer_.max_size() - buffer_.size())
        throw std::overflow_error("frame parser buffer exceeds addressable size");
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    std::vector<Frame> frames;
    size_t consumed = 0;
    while (buffer_.size() - consumed >= 4) {
        const FrameHeader header = decode_frame_header(
            std::span<const uint8_t>(buffer_).subspan(consumed, 4), max_payload_);
        const size_t frame_size = 4 + static_cast<size_t>(header.payload_bytes);
        if (buffer_.size() - consumed < frame_size) break;
        frames.push_back({header.type,
                          std::vector<uint8_t>(buffer_.begin() + consumed + 4,
                                               buffer_.begin() + consumed + frame_size)});
        consumed += frame_size;
    }
    if (consumed) buffer_.erase(buffer_.begin(), buffer_.begin() + consumed);
    return frames;
}

void FrameParser::finish() const {
    if (!buffer_.empty()) throw std::invalid_argument("framed stream ended mid-frame");
}

void MessageSessionGate::observe(MessageType type) {
    if (!known_message_type(type)) throw std::invalid_argument("unknown message type");
    if (terminal_) throw std::logic_error("message arrived after terminal ERROR");
    if (type == MessageType::ERROR) terminal_ = true;
}

std::vector<NeedMessage> encode_need_messages(std::span<const Key64> sorted_unique,
                                               size_t max_payload) {
    if (max_payload < 16 || max_payload > kInitialMaxFramePayload)
        throw std::invalid_argument("NEED payload cap must be in [16, 1 MiB]");
    std::vector<uint8_t> deltas;
    uint64_t previous = 0;
    for (Key64 key : sorted_unique) {
        if (!key.valid() || (previous && key.wire_value() <= previous))
            throw std::invalid_argument("NEED keys must be valid, sorted, and unique");
        const uint64_t delta = previous ? key.wire_value() - previous : key.wire_value();
        append_varint(deltas, delta);
        previous = key.wire_value();
    }
    Encoder first;
    first.u64(sorted_unique.size());
    first.u64(deltas.size());
    const size_t first_bytes = std::min(deltas.size(), max_payload - 16);
    first.bytes(std::span<const uint8_t>(deltas).first(first_bytes));
    std::vector<NeedMessage> result;
    result.push_back({first.take()});
    size_t offset = first_bytes;
    while (offset < deltas.size()) {
        const size_t chunk = std::min(max_payload, deltas.size() - offset);
        result.push_back({std::vector<uint8_t>(deltas.begin() + offset,
                                               deltas.begin() + offset + chunk)});
        offset += chunk;
    }
    return result;
}

void NeedStreamDecoder::push(const NeedMessage& message) {
    size_t offset = 0;
    if (!started_) {
        Decoder header(message.bytes);
        expected_keys_ = header.u64();
        expected_bytes_ = header.u64();
        offset = 16;
        started_ = true;
    }
    if (message.bytes.size() - offset > expected_bytes_ - encoded_.size())
        throw std::invalid_argument("NEED key stream exceeds declared length");
    encoded_.insert(encoded_.end(), message.bytes.begin() + offset, message.bytes.end());
    decoded_.reset();
}

bool NeedStreamDecoder::complete() const {
    return started_ && encoded_.size() == expected_bytes_;
}

const std::vector<Key64>& NeedStreamDecoder::keys() const {
    if (!complete()) throw std::logic_error("NEED stream is incomplete");
    if (decoded_) return *decoded_;
    std::vector<Key64> result;
    if (expected_keys_ > result.max_size())
        throw std::overflow_error("NEED key count exceeds addressable size");
    result.reserve(static_cast<size_t>(expected_keys_));
    size_t offset = 0;
    uint64_t previous = 0;
    while (offset < encoded_.size()) {
        const uint64_t delta = read_varint(encoded_, offset);
        if ((previous != 0 && delta == 0) || delta > std::numeric_limits<uint64_t>::max() - previous)
            throw std::invalid_argument("NEED delta sequence is not strictly increasing");
        const uint64_t value = previous + delta;
        const auto key = Key64::from_wire(value);
        if (!key) throw std::invalid_argument("NEED contains an invalid Key64");
        result.push_back(*key);
        previous = value;
    }
    if (result.size() != expected_keys_)
        throw std::invalid_argument("NEED key count does not match its declaration");
    decoded_ = std::move(result);
    return *decoded_;
}

void NeedStreamDecoder::finish() const { (void)keys(); }

std::vector<FillMessage> encode_fill_messages(std::span<const FillRecord> records,
                                               size_t max_payload) {
    if (max_payload == 0 || max_payload > kInitialMaxFramePayload)
        throw std::invalid_argument("FILL payload cap must be in [1, 1 MiB]");
    std::vector<uint8_t> stream;
    for (const FillRecord& record : records) {
        if (!record.key.valid()) throw std::invalid_argument("FILL record has invalid Key64");
        if (record.object_bytes.size() >
            std::numeric_limits<uint64_t>::max() - uint64_t{32})
            throw std::overflow_error("FILL object is too large for its record length");
        const uint64_t record_bytes = 8 + 16 + 8 + record.object_bytes.size();
        Encoder encoded;
        encoded.u64(record_bytes);
        encoded.u64(record.key.wire_value());
        encoded.digest(record.content_digest);
        encoded.u64(record.object_bytes.size());
        encoded.bytes(record.object_bytes);
        std::vector<uint8_t> bytes = encoded.take();
        if (bytes.size() > stream.max_size() - stream.size())
            throw std::overflow_error("FILL stream exceeds addressable size");
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    }
    std::vector<FillMessage> result;
    for (size_t offset = 0; offset < stream.size();) {
        const size_t chunk = std::min(max_payload, stream.size() - offset);
        result.push_back({std::vector<uint8_t>(stream.begin() + offset,
                                               stream.begin() + offset + chunk)});
        offset += chunk;
    }
    return result;
}

FillStreamDecoder::FillStreamDecoder(uint64_t max_record_bytes)
    : max_record_bytes_(max_record_bytes) {
    if (max_record_bytes < 32)
        throw std::invalid_argument("FILL-record cap is smaller than its prefix");
}

std::vector<FillRecord> FillStreamDecoder::push(const FillMessage& message) {
    if (message.bytes.size() > buffer_.max_size() - buffer_.size())
        throw std::overflow_error("FILL parser buffer exceeds addressable size");
    buffer_.insert(buffer_.end(), message.bytes.begin(), message.bytes.end());
    std::vector<FillRecord> result;
    size_t consumed = 0;
    while (buffer_.size() - consumed >= 8) {
        Decoder prefix(std::span<const uint8_t>(buffer_).subspan(consumed));
        const uint64_t record_bytes = prefix.u64();
        if (record_bytes < 32 || record_bytes > max_record_bytes_)
            throw std::length_error("FILL record length is invalid");
        if (record_bytes > std::numeric_limits<size_t>::max() - 8)
            throw std::overflow_error("FILL record does not fit address space");
        const size_t total = 8 + static_cast<size_t>(record_bytes);
        if (buffer_.size() - consumed < total) break;
        Decoder in(std::span<const uint8_t>(buffer_).subspan(consumed + 8,
                                                                 record_bytes));
        const auto key = Key64::from_wire(in.u64());
        if (!key) throw std::invalid_argument("FILL record has invalid Key64");
        FillRecord record;
        record.key = *key;
        record.content_digest = in.digest();
        const uint64_t object_bytes = in.u64();
        if (object_bytes != in.remaining())
            throw std::invalid_argument("FILL object length does not close its record");
        record.object_bytes = in.bytes(static_cast<size_t>(object_bytes));
        in.exact_end();
        result.push_back(std::move(record));
        consumed += total;
    }
    if (consumed) buffer_.erase(buffer_.begin(), buffer_.begin() + consumed);
    return result;
}

void FillStreamDecoder::finish() const {
    if (!buffer_.empty()) throw std::invalid_argument("FILL stream ended mid-record");
}

ComponentDescriptor describe_component(uint16_t encoding,
                                       std::span<const uint8_t> encoded,
                                       uint64_t decoded_bytes) {
    return {encoding, encoded.size(), decoded_bytes, digest128(encoded)};
}

Digest128 compute_transaction_digest(const TxBegin& begin,
                                     std::span<const uint8_t> dict,
                                     std::span<const uint8_t> body) {
    if (!valid_root_mode(begin.profile, begin.p29_root_mode))
        throw std::invalid_argument("transaction profile/root mode is invalid");
    if (dict.size() != begin.dict.encoded_bytes || digest128(dict) != begin.dict.digest ||
        body.size() != begin.body.encoded_bytes || digest128(body) != begin.body.digest)
        throw std::invalid_argument("transaction component does not match its descriptor");
    Digest128Builder out;
    out.append("ICECC-P50-TX-V1");
    out.append_u64(begin.history_nonce.value);
    out.append_u64(begin.rel_seq.value);
    out.append_u64(begin.tu_seq.value);
    out.append_u16(static_cast<uint16_t>(begin.profile));
    out.append_u16(static_cast<uint16_t>(begin.p29_root_mode));
    append_digest_field(out, begin.pre_state_digest);
    append_descriptor(out, begin.dict);
    append_descriptor(out, begin.body);
    out.append_u64(begin.raw_bytes);
    append_digest_field(out, begin.raw_digest);
    out.append_u64(dict.size());
    out.append(dict);
    out.append_u64(body.size());
    out.append(body);
    return out.finish();
}

Digest128 compute_post_state_digest(Digest128 pre_state, HistoryNonce history_nonce,
                                    RelSeq rel_seq, TuSeq tu_seq,
                                    Digest128 transaction_digest) {
    Digest128Builder out;
    out.append("ICECC-P50-POST-V1");
    out.append_digest(pre_state);
    out.append_u64(history_nonce.value);
    out.append_u64(rel_seq.value);
    out.append_u64(tu_seq.value);
    out.append_digest(transaction_digest);
    return out.finish();
}

Digest128 initial_route_digest(CStoreGuid c_store_guid, HistoryNonce history_nonce) {
    Digest128Builder out;
    out.append("ICECC-P50-ROUTE-V1");
    out.append(c_store_guid.bytes);
    out.append_u64(history_nonce.value);
    return out.finish();
}

}  // namespace icecc::p50
