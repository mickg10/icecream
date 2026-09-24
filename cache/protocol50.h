#pragma once

#include "services/digest128.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
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
    // F-side accounting identity for one committed P29V1 receiver segment.
    // This key never appears in the P29 inner stream and is deliberately
    // distinct from the Blob key used by the compiler InputRecord.
    P29Segment = 8,
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

struct Digest128Hash {
    size_t operator()(const Digest128& value) const noexcept;
};

// CacheWire has its own revision space.  The ordinary Icecream connection
// remains protocol 50; revision 1 is the first deployable CacheWire shape.
constexpr uint16_t kP50WireRevision = 1;
constexpr uint32_t kInitialMaxFramePayload = 1U << 20;
// TX_BEGIN is the largest fixed-size mandatory V1 control payload.
constexpr uint32_t kMandatoryControlFramePayload = 116;
constexpr uint32_t kR2LinkHelloPayloadBytes = 181;
constexpr uint32_t kR2LinkStatePayloadBytes = 212;
constexpr uint32_t kR2MandatoryControlFramePayload =
    kR2LinkStatePayloadBytes;
constexpr uint64_t kInitialMaxFillRecordBytes = uint64_t{1} << 32;

enum class MessageType : uint8_t {
    SESSION_HELLO = 1,
    SESSION_STATE = 2,
    HISTORY_RESET = 3,
    ERROR = 4,
    TX_BEGIN = 5,
    BODY = 6,
    NEED = 7,
    FILL = 8,
    TX_COMMIT = 9,
    LINK_HELLO = 10,
    LINK_STATE = 11,
    JOB_BIND = 12,
    TU_BEGIN = 13,
    R2_BODY = 14,
    R2_FILL = 15,
    TU_END = 16,
    R2_TX_COMMIT = 17,
    COMMIT_ACK = 18,
    CLOSE = 24,
};

enum class LinkStartMode : uint8_t { Initial = 0, Reconnect = 1 };

enum class ErrorCode : uint16_t {
    WIRE_REVISION_MISMATCH = 4,
};

class ProtocolError : public std::invalid_argument {
public:
    ProtocolError(ErrorCode code, std::string message)
        : std::invalid_argument(std::move(message)), code_(code) {}
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

struct ComponentDescriptor {
    uint16_t encoding = 0;
    uint64_t encoded_bytes = 0;
    uint64_t decoded_bytes = 0;
    Digest128 digest{};
    auto operator<=>(const ComponentDescriptor&) const = default;
};

enum class ProfileId : uint16_t {
    P29V1 = 1,
    ZSTD_TU = 2,
    ZSTD_ROUTE = 3,
};

std::string_view profile_name(ProfileId profile);

constexpr uint32_t profile_bit(ProfileId profile) {
    const uint16_t value = static_cast<uint16_t>(profile);
    return value >= 1 && value <= 32 ? uint32_t{1} << (value - 1) : 0;
}

constexpr uint32_t kDefaultSupportedProfiles =
    profile_bit(ProfileId::P29V1) | profile_bit(ProfileId::ZSTD_TU) |
    profile_bit(ProfileId::ZSTD_ROUTE);
constexpr uint32_t kKnownProfileMask = kDefaultSupportedProfiles;
constexpr uint32_t kDeclaredProfileMask = kKnownProfileMask;
constexpr uint32_t kOperationalProfileMask = kKnownProfileMask;

struct SessionLimits {
    uint32_t max_frame_payload = kInitialMaxFramePayload;
    uint64_t max_fill_record_bytes = kInitialMaxFillRecordBytes;
    auto operator<=>(const SessionLimits&) const = default;
};

struct SessionSelection {
    uint16_t wire_revision = kP50WireRevision;
    uint32_t negotiated_profiles = kDefaultSupportedProfiles;
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
    uint16_t wire_revision = kP50WireRevision;
    CStoreGuid c_store_guid{};
    Digest128 system_source_fingerprint{};
    uint32_t supported_profiles = kDefaultSupportedProfiles;
    SessionLimits limits{};
    auto operator<=>(const SessionHello&) const = default;
};

struct SessionState {
    uint16_t wire_revision = kP50WireRevision;
    uint32_t negotiated_profiles = kDefaultSupportedProfiles;
    SessionLimits limits{};
    FStoreGuid f_store_guid{};
    Digest128 system_source_fingerprint{};
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
    ProfileId profile = ProfileId::P29V1;
    Digest128 pre_state_digest{};
    ComponentDescriptor body{};
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    Digest128 transaction_digest{};
    auto operator<=>(const TxBegin&) const = default;
};

// CacheWire R2 W1 link binding. These records are codecs only until the
// persistent endpoint and recovery path are qualified; ordinary R1 records
// retain their original byte layouts.
struct LinkHello {
    uint16_t revision = 2;
    ProfileId profile = ProfileId::P29V1;
    uint32_t window = 1;
    uint32_t max_frame_payload = kInitialMaxFramePayload;
    uint64_t max_raw_bytes = 0;
    uint64_t max_encoded_bytes = 0;
    uint64_t max_output_bytes = 0;
    Id128 reservation_id{};
    Id128 relationship_id{};
    uint64_t relationship_epoch = 0;
    uint64_t physical_link_generation = 0;
    CStoreGuid c_store_guid{};
    uint64_t c_store_generation = 0;
    FStoreGuid f_store_guid{};
    uint64_t f_store_generation = 0;
    uint64_t c_control_generation = 0;
    uint64_t c_control_attempt = 0;
    Digest128 system_source_fingerprint{};
    HistoryNonce history_nonce{};
    uint64_t verified_receipt_floor = 0;
    LinkStartMode start_mode = LinkStartMode::Initial;
    auto operator<=>(const LinkHello&) const = default;
};

struct LinkState {
    uint16_t revision = 2;
    ProfileId profile = ProfileId::P29V1;
    uint32_t window = 1;
    Id128 reservation_id{};
    Id128 relationship_id{};
    uint64_t relationship_epoch = 0;
    uint64_t physical_link_generation = 0;
    CStoreGuid c_store_guid{};
    uint64_t c_store_generation = 0;
    FStoreGuid f_store_guid{};
    uint64_t f_store_generation = 0;
    uint64_t c_control_generation = 0;
    uint64_t c_control_attempt = 0;
    uint32_t selected_max_frame_payload = 0;
    uint64_t selected_max_raw_bytes = 0;
    uint64_t selected_max_encoded_bytes = 0;
    uint64_t selected_max_output_bytes = 0;
    Digest128 f_system_source_fingerprint{};
    HistoryNonce history_nonce{};
    RelSeq next_rel_seq{};
    Digest128 state_digest{};
    uint64_t committed_prefix_k = 0;
    uint64_t acknowledged_prefix_q = 0;
    auto operator<=>(const LinkState&) const = default;
};

struct JobBind {
    Id128 reservation_id{};
    uint64_t physical_link_generation = 0;
    uint64_t relationship_ordinal = 0;
    uint32_t wire_job_id = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;
    uint64_t logical_job = 0;
    uint64_t compiler_attempt = 0;
    uint64_t source_request_id = 0;
    TuSeq tu_seq{};
    ProfileId profile = ProfileId::P29V1;
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    auto operator<=>(const JobBind&) const = default;
};

struct TuBegin {
    uint64_t relationship_ordinal = 0;
    TxBegin inner{};
    auto operator<=>(const TuBegin&) const = default;
};

struct TuEnd {
    uint64_t relationship_ordinal = 0;
    Digest128 binding_digest{};
    Digest128 transaction_digest{};
    auto operator<=>(const TuEnd&) const = default;
};

struct R2TxCommit {
    uint64_t relationship_ordinal = 0;
    Digest128 binding_digest{};
    Digest128 transaction_digest{};
    TxCommit inner{};
    auto operator<=>(const R2TxCommit&) const = default;
};

struct CommitAck {
    Id128 relationship_id{};
    uint64_t relationship_epoch = 0;
    uint64_t physical_link_generation = 0;
    uint64_t contiguous_verified_ordinal = 0;
    auto operator<=>(const CommitAck&) const = default;
};

struct CloseMessage {
    auto operator<=>(const CloseMessage&) const = default;
};

// Client receive gate for a SESSION_STATE negotiated from the original offer.
void validate_session_state(const SessionHello& hello,
                            const SessionState& received_state);

SessionSelection negotiate_session(
    const SessionHello& hello,
    uint16_t server_wire_revision = kP50WireRevision,
    uint32_t server_profiles = kDefaultSupportedProfiles,
    SessionLimits server_limits = SessionLimits{});

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

struct R2BodyMessage {
    std::vector<uint8_t> bytes;
    auto operator<=>(const R2BodyMessage&) const = default;
};

struct R2FillMessage {
    std::vector<uint8_t> bytes;
    auto operator<=>(const R2FillMessage&) const = default;
};

using Message = std::variant<SessionHello, SessionState, HistoryReset, ErrorMessage,
                             TxBegin, BodyMessage, NeedMessage, FillMessage,
                             TxCommit, LinkHello, LinkState, JobBind, TuBegin,
                             R2TxCommit, CommitAck, TuEnd,
                             R2BodyMessage, R2FillMessage, CloseMessage>;

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
[[nodiscard]] Digest128 compute_r2_binding_digest(const JobBind& binding);
[[nodiscard]] Digest128 compute_r2_transaction_digest(
    const JobBind& binding, const TuBegin& begin,
    std::span<const R2BodyMessage> bodies,
    std::span<const R2FillMessage> fills);
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

// P29V1 carries its already-framed inner NEED stream through one or more
// ordinary Protocol-50 NEED messages. The first outer payload prefixes the
// exact inner length; continuations are raw bytes. System-source reuse is
// pinned by SESSION_HELLO/SESSION_STATE rather than repeated per transaction.

std::vector<NeedMessage> encode_p29v1_need_messages(
    std::span<const uint8_t> inner_frames, size_t max_payload,
    uint64_t max_inner_bytes);

class P29V1NeedStreamDecoder {
public:
    explicit P29V1NeedStreamDecoder(uint64_t max_inner_bytes);
    void push(const NeedMessage& message);
    [[nodiscard]] bool complete() const;
    [[nodiscard]] const std::vector<uint8_t>& inner_frames() const;
    void finish() const;

private:
    uint64_t max_inner_bytes_ = 0;
    bool started_ = false;
    uint64_t expected_bytes_ = 0;
    std::vector<uint8_t> inner_;
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

// P29V1 FILL uses the same bounded continuation pattern, without flags.
std::vector<FillMessage> encode_p29v1_fill_messages(
    std::span<const uint8_t> inner_frames, size_t max_payload,
    uint64_t max_inner_bytes);

class P29V1FillStreamDecoder {
public:
    explicit P29V1FillStreamDecoder(uint64_t max_inner_bytes);
    void push(const FillMessage& message);
    [[nodiscard]] bool complete() const;
    [[nodiscard]] const std::vector<uint8_t>& inner_frames() const;
    [[nodiscard]] std::vector<uint8_t> take_inner_frames();
    void finish() const;

private:
    uint64_t max_inner_bytes_ = 0;
    bool started_ = false;
    uint64_t expected_bytes_ = 0;
    std::vector<uint8_t> inner_;
};

ComponentDescriptor describe_component(uint16_t encoding,
                                       std::span<const uint8_t> encoded,
                                       uint64_t decoded_bytes);
Digest128 compute_transaction_digest(const TxBegin& begin,
                                     std::span<const uint8_t> body);
Digest128 compute_post_state_digest(Digest128 pre_state, HistoryNonce history_nonce,
                                    RelSeq rel_seq, TuSeq tu_seq,
                                    Digest128 transaction_digest);
Digest128 initial_route_digest(CStoreGuid c_store_guid, HistoryNonce history_nonce);

}  // namespace icecc::p50
