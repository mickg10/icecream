#include "p50_zstd.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace icecc::p50 {
namespace {

void validate_limits(ZstdTuLimits limits) {
    if (limits.max_encoded_body_bytes == 0 || limits.max_raw_bytes == 0)
        throw std::invalid_argument("ZSTD_TU limits must be nonzero");
}

ComponentDescriptor empty_dict_descriptor() {
    return describe_component(kZstdTuNoDictionaryEncoding,
                              std::span<const uint8_t>{}, 0);
}

void validate_begin_shape(const TxBegin& begin, ZstdTuLimits limits) {
    validate_limits(limits);
    if (begin.profile != ProfileId::ZSTD_TU ||
        begin.p29_root_mode != P29RootMode::NotApplicable)
        throw std::invalid_argument("transaction is not a ZSTD_TU profile");
    if (begin.rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("ZSTD_TU begins at terminal REL_SEQ");
    if (begin.dict != empty_dict_descriptor())
        throw std::invalid_argument("ZSTD_TU DICT is not canonically empty");
    if (begin.body.encoding != kZstdTuBodyEncoding)
        throw std::invalid_argument("ZSTD_TU BODY encoding is invalid");
    if (begin.body.encoded_bytes == 0)
        throw std::invalid_argument("ZSTD_TU BODY has no Zstd frame");
    if (begin.body.decoded_bytes != begin.raw_bytes)
        throw std::invalid_argument("ZSTD_TU decoded and raw lengths differ");
    if (begin.body.encoded_bytes > limits.max_encoded_body_bytes)
        throw std::length_error("ZSTD_TU encoded BODY exceeds the local cap");
    if (begin.raw_bytes > limits.max_raw_bytes)
        throw std::length_error("ZSTD_TU raw input exceeds the local cap");
    if (begin.body.encoded_bytes > std::numeric_limits<size_t>::max() ||
        begin.raw_bytes > std::numeric_limits<size_t>::max())
        throw std::overflow_error("ZSTD_TU lengths do not fit this process");
}

[[noreturn]] void throw_zstd(const char* operation, size_t result) {
    throw std::invalid_argument(std::string(operation) + ": " +
                                ZSTD_getErrorName(result));
}

const void* readable_data(std::span<const uint8_t> bytes,
                          const uint8_t& scratch) {
    return bytes.empty() ? static_cast<const void*>(&scratch)
                         : static_cast<const void*>(bytes.data());
}

}  // namespace

ZstdTuEnvelope encode_zstd_tu(HistoryNonce history_nonce, RelSeq rel_seq,
                              TuSeq tu_seq, Digest128 pre_state_digest,
                              std::span<const uint8_t> exact_input,
                              int compression_level) {
    if (rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("ZSTD_TU cannot encode terminal REL_SEQ");

    const size_t bound = ZSTD_compressBound(exact_input.size());
    if (ZSTD_isError(bound)) throw_zstd("ZSTD_compressBound", bound);

    std::vector<uint8_t> encoded(bound);
    const uint8_t scratch = 0;
    const size_t compressed = ZSTD_compress(
        encoded.data(), encoded.size(), readable_data(exact_input, scratch),
        exact_input.size(), compression_level);
    if (ZSTD_isError(compressed)) throw_zstd("ZSTD_compress", compressed);
    encoded.resize(compressed);

    ZstdTuEnvelope result;
    result.begin.history_nonce = history_nonce;
    result.begin.rel_seq = rel_seq;
    result.begin.tu_seq = tu_seq;
    result.begin.profile = ProfileId::ZSTD_TU;
    result.begin.p29_root_mode = P29RootMode::NotApplicable;
    result.begin.pre_state_digest = pre_state_digest;
    result.begin.dict = empty_dict_descriptor();
    result.begin.body = describe_component(kZstdTuBodyEncoding, encoded,
                                           exact_input.size());
    result.begin.raw_bytes = exact_input.size();
    result.begin.raw_digest = icecc::digest128(exact_input);
    result.body = std::move(encoded);
    result.begin.transaction_digest = compute_transaction_digest(
        result.begin, std::span<const uint8_t>{}, result.body);
    return result;
}

std::vector<uint8_t> decode_zstd_tu(const TxBegin& begin,
                                    std::span<const uint8_t> encoded_body,
                                    ZstdTuLimits limits) {
    validate_begin_shape(begin, limits);
    if (encoded_body.size() != begin.body.encoded_bytes)
        throw std::invalid_argument("ZSTD_TU BODY length differs from TX_BEGIN");
    if (icecc::digest128(encoded_body) != begin.body.digest)
        throw std::invalid_argument("ZSTD_TU encoded BODY digest differs");
    if (compute_transaction_digest(begin, std::span<const uint8_t>{},
                                   encoded_body) != begin.transaction_digest)
        throw std::invalid_argument("ZSTD_TU transaction digest differs");

    using DctxPtr = std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)>;
    DctxPtr dctx(ZSTD_createDCtx(), &ZSTD_freeDCtx);
    if (!dctx) throw std::bad_alloc();

    std::vector<uint8_t> output(static_cast<size_t>(begin.raw_bytes));
    uint8_t output_scratch = 0;
    void* destination = output.empty() ? static_cast<void*>(&output_scratch)
                                       : static_cast<void*>(output.data());
    const uint8_t input_scratch = 0;
    ZSTD_inBuffer input_buffer{
        readable_data(encoded_body, input_scratch), encoded_body.size(), 0};
    ZSTD_outBuffer output_buffer{destination, output.size(), 0};

    size_t remaining = 1;
    while (remaining != 0) {
        const size_t previous_input = input_buffer.pos;
        const size_t previous_output = output_buffer.pos;
        remaining = ZSTD_decompressStream(
            dctx.get(), &output_buffer, &input_buffer);
        if (ZSTD_isError(remaining))
            throw_zstd("ZSTD_decompressStream", remaining);
        if (remaining != 0 && output_buffer.pos == output_buffer.size)
            throw std::invalid_argument(
                "ZSTD_TU decoder exceeded the declared raw byte count");
        if (remaining != 0 && input_buffer.pos == previous_input &&
            output_buffer.pos == previous_output)
            throw std::invalid_argument("ZSTD_TU decoder made no progress");
    }
    if (input_buffer.pos != input_buffer.size)
        throw std::invalid_argument(
            "ZSTD_TU BODY contains trailing or concatenated frame data");
    if (output_buffer.pos != output.size())
        throw std::invalid_argument(
            "ZSTD_TU decoder produced the wrong byte count");
    if (icecc::digest128(output) != begin.raw_digest)
        throw std::invalid_argument("ZSTD_TU raw input digest differs");
    return output;
}

ZstdTuDialogue::ZstdTuDialogue(uint32_t negotiated_profiles,
                               ZstdTuLimits limits)
    : negotiated_profiles_(negotiated_profiles), limits_(limits) {
    validate_limits(limits_);
    if (negotiated_profiles_ == 0)
        throw std::invalid_argument("message session negotiated no profiles");
}

void ZstdTuDialogue::begin(const TxBegin& begin_value) {
    if (state_ != State::Idle)
        protocol_error("second TX_BEGIN arrived on a live dialogue");
    if ((negotiated_profiles_ & profile_bit(begin_value.profile)) == 0)
        protocol_error("TX_BEGIN selected an unnegotiated profile");
    try {
        validate_begin_shape(begin_value, limits_);
    } catch (...) {
        clear_active();
        state_ = State::Terminal;
        throw;
    }
    active_ = begin_value;
    state_ = State::ReceivingBody;
}

void ZstdTuDialogue::append_dict(const DictMessage& message) {
    (void)message;
    protocol_error("ZSTD_TU received DICT after its empty stream was closed");
}

void ZstdTuDialogue::append_body(const BodyMessage& message) {
    if (state_ != State::ReceivingBody || !active_)
        protocol_error("BODY arrived outside the open ZSTD_TU BODY stream");
    if (message.bytes.empty())
        protocol_error("ZSTD_TU BODY continuation made no progress");

    const uint64_t expected = active_->body.encoded_bytes;
    if (body_.size() > expected ||
        message.bytes.size() > expected - body_.size())
        protocol_error("ZSTD_TU BODY exceeds its declared encoded length");
    if (message.bytes.size() > body_.max_size() - body_.size())
        protocol_error("ZSTD_TU BODY exceeds addressable memory");

    body_.insert(body_.end(), message.bytes.begin(), message.bytes.end());
    if (body_.size() == expected) state_ = State::BodyClosed;
}

void ZstdTuDialogue::receive_need(const NeedMessage& message) {
    (void)message;
    protocol_error("ZSTD_TU received an unexpected NEED message");
}

void ZstdTuDialogue::receive_fill(const FillMessage& message) {
    (void)message;
    protocol_error("ZSTD_TU received an unexpected FILL message");
}

std::vector<uint8_t> ZstdTuDialogue::materialize() {
    if (state_ != State::BodyClosed || !active_)
        throw std::logic_error("ZSTD_TU materialized before BODY closure");
    try {
        std::vector<uint8_t> result = decode_zstd_tu(*active_, body_, limits_);
        state_ = State::Materialized;
        return result;
    } catch (...) {
        clear_active();
        state_ = State::Terminal;
        throw;
    }
}

void ZstdTuDialogue::commit_visible() {
    if (state_ != State::Materialized)
        throw std::logic_error(
            "ZSTD_TU commit became visible before materialization");
    clear_active();
    state_ = State::Idle;
}

void ZstdTuDialogue::disconnect() {
    clear_active();
    state_ = State::Terminal;
}

[[noreturn]] void ZstdTuDialogue::protocol_error(const char* message) {
    clear_active();
    state_ = State::Terminal;
    throw std::invalid_argument(message);
}

void ZstdTuDialogue::clear_active() {
    active_.reset();
    std::vector<uint8_t>().swap(body_);
}

}  // namespace icecc::p50
