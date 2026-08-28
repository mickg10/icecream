#pragma once

#include "services/digest128.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace icecc::p50 {

struct Id128 {
    std::array<uint8_t, 16> bytes{};

    static Id128 from_u64(uint64_t value);
    auto operator<=>(const Id128&) const = default;
};

struct Id128Hash {
    size_t operator()(const Id128& value) const noexcept;
};

using CStoreGuid = Id128;
using FStoreGuid = Id128;
using Digest128 = icecc::Digest128;

struct HistoryNonce {
    uint64_t value = 0;
    auto operator<=>(const HistoryNonce&) const = default;
};

struct TuSeq {
    uint64_t value = 0;
    auto operator<=>(const TuSeq&) const = default;
};

struct RelSeq {
    uint64_t value = 0;
    auto operator<=>(const RelSeq&) const = default;
};

enum class ObjectType : uint8_t {
    Atom = 1,
    Line = 2,
    Region = 3,
    Block = 4,
    Material = 5,
    Path = 6,
    Blob = 7,
};

struct KeyLayoutV1 {
    static constexpr unsigned type_bits = 5;
    static constexpr unsigned generation_bits = 10;
    static constexpr unsigned ordinal_bits = 49;
    static_assert(type_bits + generation_bits + ordinal_bits == 64);

    static constexpr unsigned generation_shift = ordinal_bits;
    static constexpr unsigned type_shift = generation_bits + ordinal_bits;
    static constexpr uint64_t ordinal_mask = (uint64_t{1} << ordinal_bits) - 1;
    static constexpr uint64_t generation_value_mask =
        (uint64_t{1} << generation_bits) - 1;
    static constexpr uint64_t type_value_mask = (uint64_t{1} << type_bits) - 1;
    static constexpr uint64_t generation_mask = generation_value_mask << generation_shift;
    static constexpr uint64_t type_mask = type_value_mask << type_shift;
};

class Key64 {
public:
    Key64() = default;

    static std::optional<Key64> make(ObjectType type, uint16_t generation,
                                     uint64_t ordinal);
    static std::optional<Key64> from_wire(uint64_t value);

    [[nodiscard]] bool valid() const;
    [[nodiscard]] uint64_t wire_value() const { return value_; }
    [[nodiscard]] ObjectType type() const;
    [[nodiscard]] uint16_t generation() const;
    [[nodiscard]] uint64_t ordinal() const;

    auto operator<=>(const Key64&) const = default;

private:
    explicit Key64(uint64_t value) : value_(value) {}
    uint64_t value_ = 0;
};

struct Key64Hash {
    size_t operator()(Key64 value) const noexcept;
};

constexpr uint16_t kProtocolVersion = 50;
constexpr uint32_t kInitialMaxFramePayload = 1U << 20;
// TX_BEGIN is the largest fixed-size mandatory V1 control payload.
constexpr uint32_t kMandatoryControlFramePayload = 152;
constexpr uint64_t kInitialMaxFillRecordBytes = uint64_t{1} << 32;

enum class MessageType : uint8_t {
    SESSION_HELLO = 1,
    SESSION_STATE = 2,
    HISTORY_RESET = 3,
    ERROR = 4,
    TX_BEGIN = 5,
    DICT = 6,
    BODY = 7,
    NEED = 8,
    FILL = 9,
    TX_COMMIT = 10,
};

struct ComponentDescriptor {
    uint16_t encoding = 0;
    uint64_t encoded_bytes = 0;
    uint64_t decoded_bytes = 0;
    Digest128 digest{};
    auto operator<=>(const ComponentDescriptor&) const = default;
};

enum class ProfileId : uint16_t {
    P29 = 1,
    ZSTD_TU = 2,
    GRZ = 3,
    Z3_LONG = 4,
    Z3_SHARED_LONG = 5,
};

std::string_view profile_name(ProfileId profile);

constexpr uint32_t profile_bit(ProfileId profile) {
    const uint16_t value = static_cast<uint16_t>(profile);
    return value >= 1 && value <= 32 ? uint32_t{1} << (value - 1) : 0;
}

constexpr uint32_t kM1SupportedProfiles = profile_bit(ProfileId::P29);
constexpr uint32_t kKnownProfileMask = profile_bit(ProfileId::P29) |
                                       profile_bit(ProfileId::ZSTD_TU) |
                                       profile_bit(ProfileId::GRZ) |
                                       profile_bit(ProfileId::Z3_LONG);
constexpr uint32_t kDeclaredProfileMask = kKnownProfileMask |
                                          profile_bit(ProfileId::Z3_SHARED_LONG);
// Z3_LONG is the operational name of the first route profile. The legacy
// profile census remains separate; session negotiation admits only codecs
// built into this executable (GRZ requires the scoped libbsc option).
constexpr uint32_t kOperationalProfileMask =
    profile_bit(ProfileId::ZSTD_TU) | profile_bit(ProfileId::Z3_LONG)
#if defined(ICECC_P50_WITH_LIBBSC)
    | profile_bit(ProfileId::GRZ)
#endif
    ;

enum class P29RootMode : uint16_t {
    NotApplicable = 0,
    RouteHistory = 1,
    HistoryIndependent = 2,
};

struct SessionLimits {
    uint32_t max_frame_payload = kInitialMaxFramePayload;
    uint64_t max_fill_record_bytes = kInitialMaxFillRecordBytes;
    auto operator<=>(const SessionLimits&) const = default;
};

struct SessionSelection {
    uint16_t protocol = kProtocolVersion;
    uint32_t negotiated_profiles = kM1SupportedProfiles;
    SessionLimits limits{};
    auto operator<=>(const SessionSelection&) const = default;
};

struct TxCommit {
    HistoryNonce history_nonce{};
    RelSeq rel_seq{};
    TuSeq tu_seq{};
    Digest128 transaction_digest{};
    Digest128 raw_digest{};
    Digest128 post_state_digest{};
    auto operator<=>(const TxCommit&) const = default;
};

struct SessionHello {
    uint16_t min_protocol = kProtocolVersion;
    uint16_t max_protocol = kProtocolVersion;
    CStoreGuid c_store_guid{};
    uint32_t supported_profiles = kM1SupportedProfiles;
    SessionLimits limits{};
    auto operator<=>(const SessionHello&) const = default;
};

struct SessionState {
    uint16_t selected_protocol = kProtocolVersion;
    uint32_t negotiated_profiles = kM1SupportedProfiles;
    SessionLimits limits{};
    FStoreGuid f_store_guid{};
    bool namespace_present = false;
    bool route_present = false;
    HistoryNonce history_nonce{};
    RelSeq next_rel_seq{};
    Digest128 state_digest{};
    std::optional<TxCommit> last_commit;
    auto operator<=>(const SessionState&) const = default;
};

struct HistoryReset {
    HistoryNonce history_nonce{};
    Digest128 initial_state_digest{};
    auto operator<=>(const HistoryReset&) const = default;
};

struct ErrorMessage {
    uint16_t code = 0;
    std::string detail;
    auto operator<=>(const ErrorMessage&) const = default;
};

struct TxBegin {
    HistoryNonce history_nonce{};
    RelSeq rel_seq{};
    TuSeq tu_seq{};
    ProfileId profile = ProfileId::P29;
    P29RootMode p29_root_mode = P29RootMode::RouteHistory;
    Digest128 pre_state_digest{};
    ComponentDescriptor dict{};
    ComponentDescriptor body{};
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    Digest128 transaction_digest{};
    auto operator<=>(const TxBegin&) const = default;
};

// Client receive gate for a SESSION_STATE negotiated from the original offer.
void validate_session_state(const SessionHello& hello,
                            const SessionState& received_state);

SessionSelection negotiate_session(
    const SessionHello& hello,
    uint16_t server_min_protocol = kProtocolVersion,
    uint16_t server_max_protocol = kProtocolVersion,
    uint32_t server_profiles = kM1SupportedProfiles,
    SessionLimits server_limits = SessionLimits{});

struct DictMessage {
    std::vector<uint8_t> bytes;
    auto operator<=>(const DictMessage&) const = default;
};

struct BodyMessage {
    std::vector<uint8_t> bytes;
    auto operator<=>(const BodyMessage&) const = default;
};

struct NeedMessage {
    std::vector<uint8_t> bytes;
    auto operator<=>(const NeedMessage&) const = default;
};

struct FillMessage {
    std::vector<uint8_t> bytes;
    auto operator<=>(const FillMessage&) const = default;
};

using Message = std::variant<SessionHello, SessionState, HistoryReset, ErrorMessage,
                             TxBegin, DictMessage, BodyMessage, NeedMessage,
                             FillMessage, TxCommit>;

struct Frame {
    MessageType type = MessageType::ERROR;
    std::vector<uint8_t> payload;
    auto operator<=>(const Frame&) const = default;
};

struct FrameHeader {
    MessageType type = MessageType::ERROR;
    uint32_t payload_bytes = 0;
    auto operator<=>(const FrameHeader&) const = default;
};

MessageType message_type(const Message& message);
std::vector<uint8_t> encode_payload(const Message& message);
Message decode_payload(MessageType type, std::span<const uint8_t> payload);
std::array<uint8_t, 4> encode_frame_header(MessageType type, uint32_t payload_bytes);
FrameHeader decode_frame_header(std::span<const uint8_t> header,
                                uint32_t max_payload = kInitialMaxFramePayload);
std::vector<uint8_t> encode_frame(const Message& message);

class FrameParser {
public:
    explicit FrameParser(uint32_t max_payload = kInitialMaxFramePayload);
    std::vector<Frame> feed(std::span<const uint8_t> bytes);
    void finish() const;
    [[nodiscard]] size_t buffered_bytes() const { return buffer_.size(); }

private:
    uint32_t max_payload_;
    std::vector<uint8_t> buffer_;
};

class MessageSessionGate {
public:
    void observe(MessageType type);
    [[nodiscard]] bool terminal() const { return terminal_; }

private:
    bool terminal_ = false;
};

std::vector<NeedMessage> encode_need_messages(std::span<const Key64> sorted_unique,
                                               size_t max_payload);

class NeedStreamDecoder {
public:
    void push(const NeedMessage& message);
    [[nodiscard]] bool complete() const;
    [[nodiscard]] const std::vector<Key64>& keys() const;
    void finish() const;

private:
    bool started_ = false;
    uint64_t expected_keys_ = 0;
    uint64_t expected_bytes_ = 0;
    std::vector<uint8_t> encoded_;
    mutable std::optional<std::vector<Key64>> decoded_;
};

struct FillRecord {
    Key64 key{};
    Digest128 content_digest{};
    std::vector<uint8_t> object_bytes;
    auto operator<=>(const FillRecord&) const = default;
};

std::vector<FillMessage> encode_fill_messages(std::span<const FillRecord> records,
                                               size_t max_payload);

class FillStreamDecoder {
public:
    explicit FillStreamDecoder(
        uint64_t max_record_bytes = kInitialMaxFillRecordBytes);
    std::vector<FillRecord> push(const FillMessage& message);
    void finish() const;

private:
    uint64_t max_record_bytes_;
    std::vector<uint8_t> buffer_;
};

ComponentDescriptor describe_component(uint16_t encoding,
                                       std::span<const uint8_t> encoded,
                                       uint64_t decoded_bytes);
Digest128 compute_transaction_digest(const TxBegin& begin,
                                     std::span<const uint8_t> dict,
                                     std::span<const uint8_t> body);
Digest128 compute_post_state_digest(Digest128 pre_state, HistoryNonce history_nonce,
                                    RelSeq rel_seq, TuSeq tu_seq,
                                    Digest128 transaction_digest);
Digest128 initial_route_digest(CStoreGuid c_store_guid, HistoryNonce history_nonce);

}  // namespace icecc::p50
