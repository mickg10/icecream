#include "p50_zstd.h"

#include <zstd.h>

#if ZSTD_VERSION_NUMBER < 10400
#error "Protocol-50 ZSTD_TU requires libzstd >= 1.4.0 for streaming window bounds"
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace icecc::p50 {
namespace {

ComponentDescriptor empty_dict_descriptor() {
    return describe_component(kZstdTuNoDictionaryEncoding,
                              std::span<const uint8_t>{}, 0);
}

void validate_begin_shape(const TxBegin& begin, ZstdTuLimits limits) {
    validate_zstd_tu_limits(limits);
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

void validate_zstd_tu_limits(ZstdTuLimits limits) {
    if (limits.max_encoded_body_bytes == 0 || limits.max_raw_bytes == 0)
        throw std::invalid_argument("ZSTD_TU byte limits must be nonzero");
    if (limits.max_window_log < 10 || limits.max_window_log > 31)
        throw std::invalid_argument("ZSTD_TU window-log cap is outside [10,31]");
    if (limits.max_history_bytes == 0 || limits.max_history_bytes > SIZE_MAX)
        throw std::invalid_argument("ZSTD route history cap is outside process bounds");
}

struct ZstdTuCodec::Contexts {
    Contexts() : compress(ZSTD_createCCtx()), decompress(ZSTD_createDCtx()) {
        if (!compress || !decompress) {
            ZSTD_freeCCtx(compress);
            ZSTD_freeDCtx(decompress);
            throw std::bad_alloc();
        }
    }

    ~Contexts() {
        ZSTD_freeCCtx(compress);
        ZSTD_freeDCtx(decompress);
    }

    ZSTD_CCtx* compress = nullptr;
    ZSTD_DCtx* decompress = nullptr;
};

ZstdTuCodec::ZstdTuCodec(int compression_level)
    : compression_level_(compression_level), contexts_(std::make_unique<Contexts>()) {
    if (compression_level < ZSTD_minCLevel() ||
        compression_level > ZSTD_maxCLevel())
        throw std::invalid_argument("ZSTD_TU compression level is outside Zstd's range");
}

ZstdTuCodec::~ZstdTuCodec() = default;

ZstdTuEnvelope ZstdTuCodec::encode(HistoryNonce history_nonce, RelSeq rel_seq,
                                   TuSeq tu_seq, Digest128 pre_state_digest,
                                   std::span<const uint8_t> exact_input,
                                   ZstdTuLimits limits) {
    validate_zstd_tu_limits(limits);
    if (rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("ZSTD_TU cannot encode terminal REL_SEQ");
    if (exact_input.size() > limits.max_raw_bytes)
        throw std::length_error("ZSTD_TU raw input exceeds the local cap");

    const size_t bound = ZSTD_compressBound(exact_input.size());
    if (ZSTD_isError(bound)) throw_zstd("ZSTD_compressBound", bound);

    const size_t capacity = std::min(
        bound, static_cast<size_t>(std::min<uint64_t>(
                   limits.max_encoded_body_bytes,
                   std::numeric_limits<size_t>::max())));
    std::vector<uint8_t> encoded(capacity);
    const uint8_t scratch = 0;
    const size_t reset = ZSTD_CCtx_reset(
        contexts_->compress, ZSTD_reset_session_and_parameters);
    if (ZSTD_isError(reset))
        throw_zstd("ZSTD_CCtx_reset", reset);
    const size_t level_result = ZSTD_CCtx_setParameter(
        contexts_->compress, ZSTD_c_compressionLevel, compression_level_);
    if (ZSTD_isError(level_result))
        throw_zstd("ZSTD_CCtx_setParameter(compressionLevel)", level_result);
    const size_t window_result = ZSTD_CCtx_setParameter(
        contexts_->compress, ZSTD_c_windowLog, limits.max_window_log);
    if (ZSTD_isError(window_result))
        throw_zstd("ZSTD_CCtx_setParameter(windowLog)", window_result);
    const size_t compressed = ZSTD_compress2(
        contexts_->compress,
        encoded.data(), encoded.size(), readable_data(exact_input, scratch),
        exact_input.size());
    if (ZSTD_isError(compressed)) {
        if (bound > limits.max_encoded_body_bytes)
            throw std::length_error("ZSTD_TU encoded BODY exceeds the local cap");
        throw_zstd("ZSTD_compress", compressed);
    }
    if (compressed > limits.max_encoded_body_bytes)
        throw std::length_error("ZSTD_TU encoded BODY exceeds the local cap");
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

ZstdTuEnvelope encode_zstd_tu(HistoryNonce history_nonce, RelSeq rel_seq,
                              TuSeq tu_seq, Digest128 pre_state_digest,
                              std::span<const uint8_t> exact_input,
                              int compression_level, ZstdTuLimits limits) {
    ZstdTuCodec codec(compression_level);
    return codec.encode(history_nonce, rel_seq, tu_seq, pre_state_digest,
                        exact_input, limits);
}

void validate_zstd_tu_begin(const TxBegin& begin, ZstdTuLimits limits) {
    validate_begin_shape(begin, limits);
}

std::vector<uint8_t> ZstdTuCodec::decode(const TxBegin& begin,
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

    const size_t initialized = ZSTD_initDStream(contexts_->decompress);
    if (ZSTD_isError(initialized))
        throw_zstd("ZSTD_initDStream", initialized);
    const size_t window_result = ZSTD_DCtx_setParameter(
        contexts_->decompress, ZSTD_d_windowLogMax, limits.max_window_log);
    if (ZSTD_isError(window_result))
        throw_zstd("ZSTD_DCtx_setParameter(windowLogMax)", window_result);

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
            contexts_->decompress, &output_buffer, &input_buffer);
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

std::vector<uint8_t> decode_zstd_tu(const TxBegin& begin,
                                    std::span<const uint8_t> encoded_body,
                                    ZstdTuLimits limits) {
    ZstdTuCodec codec;
    return codec.decode(begin, encoded_body, limits);
}

bool commit_matches(const TxCommit& commit, const TxBegin& begin);

namespace {

void validate_route_begin_shape(const TxBegin& begin, ZstdTuLimits limits) {
    validate_zstd_tu_limits(limits);
    if (begin.profile != ProfileId::Z3_LONG ||
        begin.p29_root_mode != P29RootMode::NotApplicable)
        throw std::invalid_argument("transaction is not a ZSTD_ROUTE profile");
    if (begin.rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("ZSTD_ROUTE begins at terminal REL_SEQ");
    if (begin.dict != empty_dict_descriptor())
        throw std::invalid_argument("ZSTD_ROUTE DICT is not canonically empty");
    if (begin.body.encoding != kZstdRouteBodyEncoding ||
        begin.body.encoded_bytes == 0)
        throw std::invalid_argument("ZSTD_ROUTE BODY encoding is invalid");
    if (begin.body.decoded_bytes != begin.raw_bytes)
        throw std::invalid_argument("ZSTD_ROUTE decoded and raw lengths differ");
    if (begin.body.encoded_bytes > limits.max_encoded_body_bytes)
        throw std::length_error("ZSTD_ROUTE encoded BODY exceeds the local cap");
    if (begin.raw_bytes > limits.max_raw_bytes)
        throw std::length_error("ZSTD_ROUTE raw input exceeds the local cap");
    if (begin.body.encoded_bytes > std::numeric_limits<size_t>::max() ||
        begin.raw_bytes > std::numeric_limits<size_t>::max())
        throw std::overflow_error("ZSTD_ROUTE lengths do not fit this process");
}

void set_route_compression_parameters(ZSTD_CCtx* context, int level,
                                      ZstdTuLimits limits) {
    size_t result = ZSTD_CCtx_reset(context, ZSTD_reset_session_and_parameters);
    if (ZSTD_isError(result)) throw_zstd("ZSTD_ROUTE CCtx reset", result);
    result = ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level);
    if (ZSTD_isError(result)) throw_zstd("ZSTD_ROUTE compression level", result);
    result = ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog,
                                    limits.max_window_log);
    if (ZSTD_isError(result)) throw_zstd("ZSTD_ROUTE window log", result);
}

uint64_t route_history_limit(ZstdTuLimits limits) {
    const uint64_t window = uint64_t{1} << limits.max_window_log;
    return std::min(limits.max_history_bytes, window);
}

void route_compress_frame(ZSTD_CCtx* context, std::span<const uint8_t> prefix,
                          std::span<const uint8_t> input,
                          std::vector<uint8_t>* encoded, uint64_t cap) {
    const uint8_t prefix_scratch = 0;
    size_t result = ZSTD_CCtx_refPrefix(
        context, readable_data(prefix, prefix_scratch), prefix.size());
    if (ZSTD_isError(result)) throw_zstd("ZSTD_ROUTE prefix", result);
    const size_t bound = ZSTD_compressBound(input.size());
    if (ZSTD_isError(bound)) throw_zstd("ZSTD_ROUTE compressBound", bound);
    encoded->clear();
    encoded->reserve(static_cast<size_t>(std::min<uint64_t>(bound, cap)));
    const uint8_t input_scratch = 0;
    ZSTD_inBuffer source{readable_data(input, input_scratch), input.size(), 0};
    std::array<uint8_t, 128U << 10> scratch{};
    size_t remaining = 1;
    while (remaining != 0) {
        if (encoded->size() >= cap)
            throw std::length_error("ZSTD_ROUTE encoded BODY exceeds the local cap");
        const size_t room = static_cast<size_t>(std::min<uint64_t>(
            cap - encoded->size(), scratch.size()));
        ZSTD_outBuffer destination{scratch.data(), room, 0};
        const size_t previous_input = source.pos;
        remaining = ZSTD_compressStream2(context, &destination, &source, ZSTD_e_end);
        if (ZSTD_isError(remaining)) throw_zstd("ZSTD_ROUTE stream end", remaining);
        encoded->insert(encoded->end(), scratch.begin(),
                        scratch.begin() + destination.pos);
        if (remaining != 0 && source.pos == previous_input && destination.pos == 0)
            throw std::invalid_argument("ZSTD_ROUTE encoder made no progress");
    }
}

} // namespace

struct ZstdRouteCodec::Contexts {
    Contexts() : compress(ZSTD_createCCtx()), decompress(ZSTD_createDCtx()) {
        if (!compress || !decompress) {
            ZSTD_freeCCtx(compress);
            ZSTD_freeDCtx(decompress);
            throw std::bad_alloc();
        }
    }
    ~Contexts() {
        ZSTD_freeCCtx(compress);
        ZSTD_freeDCtx(decompress);
    }
    ZSTD_CCtx* compress = nullptr;
    ZSTD_DCtx* decompress = nullptr;
};

ZstdRouteCodec::ZstdRouteCodec(int compression_level)
    : compression_level_(compression_level), contexts_(std::make_unique<Contexts>()) {
    if (compression_level != 3)
        throw std::invalid_argument("ZSTD_ROUTE requires compression level 3");
}

ZstdRouteCodec::~ZstdRouteCodec() = default;

ZstdRouteEnvelope ZstdRouteCodec::encode(
    HistoryNonce history_nonce, RelSeq rel_seq, TuSeq tu_seq,
    Digest128 pre_state_digest, std::span<const uint8_t> predecessor_history,
    std::span<const uint8_t> exact_input, ZstdTuLimits limits) {
    validate_zstd_tu_limits(limits);
    if (history_nonce.value == 0)
        throw std::invalid_argument("ZSTD_ROUTE HISTORY_NONCE zero is reserved");
    if (rel_seq.value == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("ZSTD_ROUTE cannot encode terminal REL_SEQ");
    if (exact_input.size() > limits.max_raw_bytes)
        throw std::length_error("ZSTD_ROUTE raw input exceeds the local cap");
    if (exact_input.empty())
        throw std::invalid_argument("ZSTD_ROUTE requires a non-empty TU");
    if (predecessor_history.size() > route_history_limit(limits))
        throw std::length_error("ZSTD_ROUTE predecessor history exceeds the local cap");
    set_route_compression_parameters(contexts_->compress, compression_level_, limits);
    std::vector<uint8_t> encoded;
    route_compress_frame(contexts_->compress, predecessor_history, exact_input,
                         &encoded, limits.max_encoded_body_bytes);
    if (encoded.empty())
        throw std::invalid_argument("ZSTD_ROUTE encoder produced an empty BODY");

    ZstdRouteEnvelope result;
    result.begin.history_nonce = history_nonce;
    result.begin.rel_seq = rel_seq;
    result.begin.tu_seq = tu_seq;
    result.begin.profile = ProfileId::Z3_LONG;
    result.begin.p29_root_mode = P29RootMode::NotApplicable;
    result.begin.pre_state_digest = pre_state_digest;
    result.begin.dict = empty_dict_descriptor();
    result.begin.body = describe_component(kZstdRouteBodyEncoding, encoded,
                                           exact_input.size());
    result.begin.raw_bytes = exact_input.size();
    result.begin.raw_digest = icecc::digest128(exact_input);
    result.body = std::move(encoded);
    result.begin.transaction_digest = compute_transaction_digest(
        result.begin, std::span<const uint8_t>{}, result.body);
    return result;
}

ZstdRouteEnvelope ZstdRouteCodec::encode(
    HistoryNonce history_nonce, RelSeq rel_seq, TuSeq tu_seq,
    Digest128 pre_state_digest, std::span<const uint8_t> exact_input,
    ZstdTuLimits limits) {
    return encode(history_nonce, rel_seq, tu_seq, pre_state_digest,
                  std::span<const uint8_t>{}, exact_input, limits);
}

ZstdRouteEnvelope encode_zstd_route(HistoryNonce history_nonce, RelSeq rel_seq,
                                    TuSeq tu_seq, Digest128 pre_state_digest,
                                    std::span<const uint8_t> exact_input,
                                    ZstdTuLimits limits) {
    ZstdRouteCodec codec;
    return codec.encode(history_nonce, rel_seq, tu_seq, pre_state_digest,
                        exact_input, limits);
}

std::vector<uint8_t> ZstdRouteCodec::decode(
    std::span<const uint8_t> predecessor_history, const TxBegin& begin,
    std::span<const uint8_t> encoded_body, ZstdTuLimits limits) {
    validate_route_begin_shape(begin, limits);
    if (encoded_body.size() != begin.body.encoded_bytes ||
        icecc::digest128(encoded_body) != begin.body.digest ||
        compute_transaction_digest(begin, std::span<const uint8_t>{}, encoded_body) !=
            begin.transaction_digest)
        throw std::invalid_argument("ZSTD_ROUTE BODY or transaction digest differs");
    if (predecessor_history.size() > route_history_limit(limits))
        throw std::length_error("ZSTD_ROUTE predecessor history exceeds the local cap");
    size_t initialized = ZSTD_initDStream(contexts_->decompress);
    if (ZSTD_isError(initialized)) throw_zstd("ZSTD_ROUTE init DStream", initialized);
    initialized = ZSTD_DCtx_setParameter(contexts_->decompress,
                                         ZSTD_d_windowLogMax,
                                         limits.max_window_log);
    if (ZSTD_isError(initialized)) throw_zstd("ZSTD_ROUTE decoder window", initialized);
    const uint8_t prefix_scratch = 0;
    initialized = ZSTD_DCtx_refPrefix(
        contexts_->decompress, readable_data(predecessor_history, prefix_scratch),
        predecessor_history.size());
    if (ZSTD_isError(initialized)) throw_zstd("ZSTD_ROUTE decoder prefix", initialized);
    std::vector<uint8_t> result(static_cast<size_t>(begin.raw_bytes));
    const uint8_t input_scratch = 0;
    ZSTD_inBuffer source{readable_data(encoded_body, input_scratch), encoded_body.size(), 0};
    ZSTD_outBuffer destination{result.empty() ? nullptr : result.data(), result.size(), 0};
    size_t remaining = 1;
    while (remaining != 0) {
        const size_t previous_input = source.pos;
        const size_t previous_output = destination.pos;
        remaining = ZSTD_decompressStream(contexts_->decompress, &destination, &source);
        if (ZSTD_isError(remaining)) throw_zstd("ZSTD_ROUTE frame decode", remaining);
        if (remaining != 0 && destination.pos == destination.size)
            throw std::invalid_argument("ZSTD_ROUTE decoder exceeded raw byte count");
        if (remaining != 0 && source.pos == previous_input &&
            destination.pos == previous_output)
            throw std::invalid_argument("ZSTD_ROUTE decoder made no progress");
    }
    if (source.pos != source.size || destination.pos != result.size())
        throw std::invalid_argument("ZSTD_ROUTE decoder consumed the wrong byte count");
    if (icecc::digest128(result) != begin.raw_digest)
        throw std::invalid_argument("ZSTD_ROUTE raw input digest differs");
    return result;
}

ZstdRouteDialogue::ZstdRouteDialogue(uint32_t negotiated_profiles,
                                     ZstdTuLimits limits)
    : negotiated_profiles_(negotiated_profiles), limits_(limits) {
    validate_zstd_tu_limits(limits_);
    if (negotiated_profiles_ == 0)
        throw std::invalid_argument("message session negotiated no profiles");
}

void ZstdRouteDialogue::begin(const TxBegin& begin_value) {
    if (state_ != State::Idle)
        protocol_error("second TX_BEGIN arrived on a live ZSTD_ROUTE dialogue");
    if ((negotiated_profiles_ & profile_bit(begin_value.profile)) == 0)
        protocol_error("TX_BEGIN selected an unnegotiated profile");
    try {
        validate_route_begin_shape(begin_value, limits_);
        if (history_nonce_ && begin_value.history_nonce != *history_nonce_)
            throw std::invalid_argument("ZSTD_ROUTE history nonce changed without reset");
        if (last_rel_ &&
            (last_rel_->value == std::numeric_limits<uint64_t>::max() ||
             begin_value.rel_seq.value != last_rel_->value + 1))
            throw std::invalid_argument("ZSTD_ROUTE REL_SEQ did not advance by one");
        if (committed_state_ && begin_value.pre_state_digest != *committed_state_)
            throw std::invalid_argument("ZSTD_ROUTE predecessor state digest differs");
    } catch (...) {
        clear_active();
        state_ = State::Terminal;
        throw;
    }
    active_ = begin_value;
    state_ = State::ReceivingBody;
}

void ZstdRouteDialogue::append_dict(const DictMessage&) {
    protocol_error("ZSTD_ROUTE received DICT after its empty stream was closed");
}

void ZstdRouteDialogue::append_body(const BodyMessage& message) {
    if (state_ != State::ReceivingBody || !active_)
        protocol_error("BODY arrived outside the open ZSTD_ROUTE BODY stream");
    if (message.bytes.empty()) protocol_error("ZSTD_ROUTE BODY made no progress");
    const uint64_t expected = active_->body.encoded_bytes;
    if (body_.size() > expected || message.bytes.size() > expected - body_.size() ||
        message.bytes.size() > body_.max_size() - body_.size())
        protocol_error("ZSTD_ROUTE BODY exceeds its declared encoded length");
    body_.insert(body_.end(), message.bytes.begin(), message.bytes.end());
    if (body_.size() == expected) state_ = State::BodyClosed;
}

void ZstdRouteDialogue::receive_need(const NeedMessage&) {
    protocol_error("ZSTD_ROUTE received an unexpected NEED message");
}

void ZstdRouteDialogue::receive_fill(const FillMessage&) {
    protocol_error("ZSTD_ROUTE received an unexpected FILL message");
}

std::vector<uint8_t> ZstdRouteDialogue::materialize() {
    if (state_ != State::BodyClosed || !active_)
        throw std::logic_error("ZSTD_ROUTE materialized before BODY closure");
    try {
        std::vector<uint8_t> result = codec_.decode(history_, *active_, body_, limits_);
        active_raw_ = result;
        state_ = State::Materialized;
        return result;
    } catch (...) {
        clear_active();
        state_ = State::Terminal;
        throw;
    }
}

void ZstdRouteDialogue::commit_visible(const TxCommit& commit) {
    if (state_ != State::Materialized)
        throw std::logic_error("ZSTD_ROUTE commit became visible before materialization");
    if (!active_ || !commit_matches(commit, *active_))
        throw std::invalid_argument("ZSTD_ROUTE terminal commit differs from TX_BEGIN");
    if (active_raw_.empty())
        throw std::logic_error("ZSTD_ROUTE commit has no materialized raw input");
    const size_t limit = static_cast<size_t>(route_history_limit(limits_));
    if (active_raw_.size() >= limit) {
        history_.assign(active_raw_.end() - static_cast<std::ptrdiff_t>(limit),
                        active_raw_.end());
    } else {
        const size_t excess = history_.size() + active_raw_.size() > limit
                                  ? history_.size() + active_raw_.size() - limit
                                  : 0;
        if (excess != 0)
            history_.erase(history_.begin(), history_.begin() + excess);
        history_.insert(history_.end(), active_raw_.begin(), active_raw_.end());
    }
    history_nonce_ = active_->history_nonce;
    last_rel_ = active_->rel_seq;
    committed_state_ = commit.post_state_digest;
    clear_active();
    state_ = State::Idle;
}

void ZstdRouteDialogue::discard_tentative() noexcept {
    if (state_ != State::Idle) {
        clear_active();
        state_ = State::Idle;
    }
}

void ZstdRouteDialogue::disconnect() {
    clear_active();
    history_.clear();
    history_nonce_.reset();
    last_rel_.reset();
    committed_state_.reset();
    state_ = State::Terminal;
}

void ZstdRouteDialogue::reset() {
    if (state_ != State::Terminal)
        throw std::logic_error("ZSTD_ROUTE reset requires a terminal dialogue");
    clear_active();
    history_.clear();
    history_nonce_.reset();
    last_rel_.reset();
    committed_state_.reset();
    state_ = State::Idle;
}

uint64_t ZstdRouteDialogue::window_limit_bytes() const noexcept {
    return uint64_t{1} << limits_.max_window_log;
}

[[noreturn]] void ZstdRouteDialogue::protocol_error(const char* message) {
    clear_active();
    state_ = State::Terminal;
    throw std::invalid_argument(message);
}

void ZstdRouteDialogue::clear_active() {
    active_.reset();
    std::vector<uint8_t>().swap(body_);
    std::vector<uint8_t>().swap(active_raw_);
}

bool commit_matches(const TxCommit& commit, const TxBegin& begin) {
    return commit.history_nonce == begin.history_nonce &&
           commit.rel_seq == begin.rel_seq && commit.tu_seq == begin.tu_seq &&
           commit.transaction_digest == begin.transaction_digest &&
           commit.raw_digest == begin.raw_digest &&
           commit.post_state_digest ==
               compute_post_state_digest(begin.pre_state_digest, begin.history_nonce,
                                          begin.rel_seq, begin.tu_seq,
                                          begin.transaction_digest);
}

ZstdTuDialogue::ZstdTuDialogue(uint32_t negotiated_profiles,
                               ZstdTuLimits limits)
    : negotiated_profiles_(negotiated_profiles), limits_(limits) {
    validate_zstd_tu_limits(limits_);
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
        std::vector<uint8_t> result = codec_.decode(*active_, body_, limits_);
        state_ = State::Materialized;
        return result;
    } catch (...) {
        clear_active();
        state_ = State::Terminal;
        throw;
    }
}

void ZstdTuDialogue::commit_visible(const TxCommit& commit) {
    if (state_ != State::Materialized)
        throw std::logic_error(
            "ZSTD_TU commit became visible before materialization");
    if (!active_ || !commit_matches(commit, *active_))
        throw std::invalid_argument("ZSTD_TU terminal commit differs from TX_BEGIN");
    clear_active();
    state_ = State::Idle;
}

void ZstdTuDialogue::discard_tentative() noexcept {
    if (state_ != State::Idle && state_ != State::Terminal) {
        clear_active();
        state_ = State::Idle;
    }
}

void ZstdTuDialogue::disconnect() {
    clear_active();
    state_ = State::Terminal;
}

void ZstdTuDialogue::reset() {
    if (state_ != State::Terminal)
        throw std::logic_error("ZSTD_TU reset requires a terminal dialogue");
    clear_active();
    state_ = State::Idle;
}

uint64_t ZstdTuDialogue::window_limit_bytes() const noexcept {
    return uint64_t{1} << limits_.max_window_log;
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
