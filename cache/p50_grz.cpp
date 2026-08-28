#include "p50_grz.h"

#if defined(ICECC_P50_WITH_LIBBSC)

#include "p50_grz_residual_codec.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace icecc::p50 {
namespace {

ComponentDescriptor empty_dict_descriptor() {
    return describe_component(kGrzResidualNoDictionaryEncoding,
                              std::span<const uint8_t>{}, 0);
}

void validate_shape(const TxBegin& begin, ZstdTuLimits limits) {
    validate_zstd_tu_limits(limits);
    if (begin.profile != ProfileId::GRZ ||
        begin.p29_root_mode != P29RootMode::NotApplicable)
        throw std::invalid_argument("transaction is not a GRZ_RESIDUAL profile");
    if (begin.rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("GRZ_RESIDUAL begins at terminal REL_SEQ");
    if (begin.dict != empty_dict_descriptor())
        throw std::invalid_argument("GRZ_RESIDUAL DICT is not canonically empty");
    if (begin.body.encoding != kGrzResidualBodyEncoding ||
        begin.body.encoded_bytes == 0)
        throw std::invalid_argument("GRZ_RESIDUAL BODY encoding is invalid");
    if (begin.body.decoded_bytes != begin.raw_bytes)
        throw std::invalid_argument("GRZ_RESIDUAL decoded and raw lengths differ");
    if (begin.body.encoded_bytes > limits.max_encoded_body_bytes)
        throw std::length_error("GRZ_RESIDUAL encoded BODY exceeds the local cap");
    if (begin.raw_bytes > limits.max_raw_bytes)
        throw std::length_error("GRZ_RESIDUAL raw input exceeds the local cap");
    if (begin.body.encoded_bytes > std::numeric_limits<size_t>::max() ||
        begin.raw_bytes > std::numeric_limits<size_t>::max())
        throw std::overflow_error("GRZ_RESIDUAL lengths do not fit this process");
}

bool commit_matches_grz(const TxCommit& commit, const TxBegin& begin) {
    return commit.history_nonce == begin.history_nonce &&
           commit.rel_seq == begin.rel_seq && commit.tu_seq == begin.tu_seq &&
           commit.transaction_digest == begin.transaction_digest &&
           commit.raw_digest == begin.raw_digest &&
           commit.post_state_digest == compute_post_state_digest(
               begin.pre_state_digest, begin.history_nonce, begin.rel_seq,
               begin.tu_seq, begin.transaction_digest);
}

} // namespace

struct GrzResidualCodec::State {
    residual_group::Codec codec;
};

GrzResidualCodec::GrzResidualCodec() : state_(std::make_unique<State>()) {}
GrzResidualCodec::~GrzResidualCodec() = default;

GrzResidualEnvelope GrzResidualCodec::encode(
    HistoryNonce history_nonce, RelSeq rel_seq, TuSeq tu_seq,
    Digest128 pre_state_digest, std::span<const uint8_t> exact_input,
    ZstdTuLimits limits) {
    validate_zstd_tu_limits(limits);
    if (rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("GRZ_RESIDUAL cannot encode terminal REL_SEQ");
    if (exact_input.empty())
        throw std::invalid_argument("GRZ_RESIDUAL cannot encode an empty TU");
    if (exact_input.size() > limits.max_raw_bytes)
        throw std::length_error("GRZ_RESIDUAL raw input exceeds the local cap");

    residual_group::Kind selected = residual_group::Kind::Zstd3;
    std::vector<uint8_t> encoded = state_->codec.encode(
        exact_input.data(), exact_input.size(), &selected);
    (void)selected;
    if (encoded.empty() || encoded.size() > limits.max_encoded_body_bytes)
        throw std::length_error("GRZ_RESIDUAL encoded BODY exceeds the local cap");

    GrzResidualEnvelope result;
    result.begin.history_nonce = history_nonce;
    result.begin.rel_seq = rel_seq;
    result.begin.tu_seq = tu_seq;
    result.begin.profile = ProfileId::GRZ;
    result.begin.p29_root_mode = P29RootMode::NotApplicable;
    result.begin.pre_state_digest = pre_state_digest;
    result.begin.dict = empty_dict_descriptor();
    result.begin.body = describe_component(kGrzResidualBodyEncoding, encoded,
                                           exact_input.size());
    result.begin.raw_bytes = exact_input.size();
    result.begin.raw_digest = icecc::digest128(exact_input);
    result.body = std::move(encoded);
    result.begin.transaction_digest = compute_transaction_digest(
        result.begin, std::span<const uint8_t>{}, result.body);
    return result;
}

GrzResidualEnvelope encode_grz_residual(
    HistoryNonce history_nonce, RelSeq rel_seq, TuSeq tu_seq,
    Digest128 pre_state_digest, std::span<const uint8_t> exact_input,
    ZstdTuLimits limits) {
    GrzResidualCodec codec;
    return codec.encode(history_nonce, rel_seq, tu_seq, pre_state_digest,
                        exact_input, limits);
}

void validate_grz_residual_begin(const TxBegin& begin, ZstdTuLimits limits) {
    validate_shape(begin, limits);
}

std::vector<uint8_t> GrzResidualCodec::decode(
    const TxBegin& begin, std::span<const uint8_t> encoded_body,
    ZstdTuLimits limits) {
    validate_shape(begin, limits);
    if (encoded_body.size() != begin.body.encoded_bytes)
        throw std::invalid_argument("GRZ_RESIDUAL BODY length differs from TX_BEGIN");
    if (icecc::digest128(encoded_body) != begin.body.digest)
        throw std::invalid_argument("GRZ_RESIDUAL encoded BODY digest differs");
    if (compute_transaction_digest(begin, std::span<const uint8_t>{},
                                   encoded_body) != begin.transaction_digest)
        throw std::invalid_argument("GRZ_RESIDUAL transaction digest differs");
    const residual_group::DecodedFrame frame = state_->codec.decode(
        encoded_body.data(), encoded_body.size());
    if (frame.wire_bytes != encoded_body.size())
        throw std::invalid_argument("GRZ_RESIDUAL BODY contains trailing frame data");
    if (frame.raw.size() != begin.raw_bytes)
        throw std::invalid_argument("GRZ_RESIDUAL decoded size differs");
    if (icecc::digest128(frame.raw) != begin.raw_digest)
        throw std::invalid_argument("GRZ_RESIDUAL raw input digest differs");
    return frame.raw;
}

std::vector<uint8_t> decode_grz_residual(
    const TxBegin& begin, std::span<const uint8_t> encoded_body,
    ZstdTuLimits limits) {
    GrzResidualCodec codec;
    return codec.decode(begin, encoded_body, limits);
}

GrzResidualDialogue::GrzResidualDialogue(uint32_t negotiated_profiles,
                                         ZstdTuLimits limits)
    : negotiated_profiles_(negotiated_profiles), limits_(limits) {
    validate_zstd_tu_limits(limits_);
    if (negotiated_profiles_ == 0)
        throw std::invalid_argument("message session negotiated no profiles");
}

void GrzResidualDialogue::begin(const TxBegin& begin_value) {
    if (state_ != State::Idle) protocol_error("second TX_BEGIN arrived on a live dialogue");
    if ((negotiated_profiles_ & profile_bit(begin_value.profile)) == 0)
        protocol_error("TX_BEGIN selected an unnegotiated profile");
    try { validate_shape(begin_value, limits_); }
    catch (...) { clear_active(); state_ = State::Terminal; throw; }
    active_ = begin_value;
    state_ = State::ReceivingBody;
}

void GrzResidualDialogue::append_dict(const DictMessage&) {
    protocol_error("GRZ_RESIDUAL received DICT after its empty stream was closed");
}

void GrzResidualDialogue::append_body(const BodyMessage& message) {
    if (state_ != State::ReceivingBody || !active_)
        protocol_error("BODY arrived outside the open GRZ_RESIDUAL BODY stream");
    if (message.bytes.empty()) protocol_error("GRZ_RESIDUAL BODY continuation made no progress");
    const uint64_t expected = active_->body.encoded_bytes;
    if (body_.size() > expected || message.bytes.size() > expected - body_.size())
        protocol_error("GRZ_RESIDUAL BODY exceeds its declared encoded length");
    if (message.bytes.size() > body_.max_size() - body_.size())
        protocol_error("GRZ_RESIDUAL BODY exceeds addressable memory");
    body_.insert(body_.end(), message.bytes.begin(), message.bytes.end());
    if (body_.size() == expected) state_ = State::BodyClosed;
}

void GrzResidualDialogue::receive_need(const NeedMessage&) {
    protocol_error("GRZ_RESIDUAL received an unexpected NEED message");
}
void GrzResidualDialogue::receive_fill(const FillMessage&) {
    protocol_error("GRZ_RESIDUAL received an unexpected FILL message");
}

std::vector<uint8_t> GrzResidualDialogue::materialize() {
    if (state_ != State::BodyClosed || !active_)
        throw std::logic_error("GRZ_RESIDUAL materialized before BODY closure");
    try {
        std::vector<uint8_t> result = codec_.decode(*active_, body_, limits_);
        state_ = State::Materialized;
        return result;
    } catch (...) { clear_active(); state_ = State::Terminal; throw; }
}

void GrzResidualDialogue::commit_visible(const TxCommit& commit) {
    if (state_ != State::Materialized)
        throw std::logic_error("GRZ_RESIDUAL commit became visible before materialization");
    if (!active_ || !commit_matches_grz(commit, *active_))
        throw std::invalid_argument("GRZ_RESIDUAL terminal commit differs from TX_BEGIN");
    clear_active(); state_ = State::Idle;
}

void GrzResidualDialogue::discard_tentative() noexcept {
    if (state_ != State::Idle && state_ != State::Terminal) { clear_active(); state_ = State::Idle; }
}
void GrzResidualDialogue::disconnect() { clear_active(); state_ = State::Terminal; }
void GrzResidualDialogue::reset() {
    if (state_ != State::Terminal) throw std::logic_error("GRZ_RESIDUAL reset requires a terminal dialogue");
    clear_active(); state_ = State::Idle;
}
uint64_t GrzResidualDialogue::window_limit_bytes() const noexcept {
    return uint64_t{1} << limits_.max_window_log;
}
[[noreturn]] void GrzResidualDialogue::protocol_error(const char* message) {
    clear_active(); state_ = State::Terminal; throw std::invalid_argument(message);
}
void GrzResidualDialogue::clear_active() { active_.reset(); std::vector<uint8_t>().swap(body_); }

} // namespace icecc::p50

#endif
