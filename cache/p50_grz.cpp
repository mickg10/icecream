#include "p50_grz.h"

#if defined(ICECC_P50_WITH_LIBBSC)

#include "p50_grz_residual_codec.h"

#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace icecc::p50 {
namespace {

// This is the live endpoint extraction of the reviewed GRZ2_GROUPED state:
// persistent absolute anchors across bounded groups, backward COPY phrases,
// and residual entropy coding only for ADD bytes.  The CLI/process ownership
// and mmap ring from capability/grouprlz/grz2g.cpp are intentionally absent.
constexpr uint32_t kGroupRlzMagic = UINT32_C(0x31505247); // GRP1
constexpr size_t kGroupBytes = 64U << 10;
constexpr size_t kAnchorBytes = 16;

struct GroupToken {
    bool copy = false;
    uint64_t value = 0;
    uint32_t length = 0;
};

struct GroupStage {
    std::vector<GroupToken> tokens;
    std::vector<uint8_t> residual;
    uint64_t references = 0;
    uint32_t groups = 0;
};

const std::array<uint64_t, 256>& gear_table() {
    static const std::array<uint64_t, 256> table = [] {
        std::array<uint64_t, 256> result{};
        uint64_t seed = UINT64_C(0x243F6A8885A308D3);
        for (uint64_t& value : result) {
            seed += UINT64_C(0x9E3779B97F4A7C15);
            uint64_t mixed = seed;
            mixed = (mixed ^ (mixed >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
            mixed = (mixed ^ (mixed >> 27)) * UINT64_C(0x94D049BB133111EB);
            value = mixed ^ (mixed >> 31);
        }
        return result;
    }();
    return table;
}

uint64_t gear_hash(const uint8_t* input, size_t size) {
    uint64_t hash = 0;
    for (size_t i = 0; i < size; ++i) {
        hash = (hash << 1) | (hash >> 63);
        hash ^= gear_table()[input[i]];
    }
    return hash;
}

void put_u32(std::vector<uint8_t>& output, uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        output.push_back(static_cast<uint8_t>(value >> shift));
}

void put_u64(std::vector<uint8_t>& output, uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        output.push_back(static_cast<uint8_t>(value >> shift));
}

uint32_t get_u32(const uint8_t*& cursor, const uint8_t* end) {
    if (static_cast<size_t>(end - cursor) < 4) throw std::invalid_argument("GRZ group header truncated");
    uint32_t value = 0;
    for (unsigned shift = 0; shift != 32; shift += 8) value |= uint32_t(*cursor++) << shift;
    return value;
}

uint64_t get_u64(const uint8_t*& cursor, const uint8_t* end) {
    if (static_cast<size_t>(end - cursor) < 8) throw std::invalid_argument("GRZ group token truncated");
    uint64_t value = 0;
    for (unsigned shift = 0; shift != 64; shift += 8) value |= uint64_t(*cursor++) << shift;
    return value;
}

// Encode one TU against the committed route prefix. The matcher is owned by
// the route codec; newly planted anchors remain provisional until commit and
// are removed in reverse order on retry/abort.
GroupStage group_encode(std::span<const uint8_t> history,
                        std::span<const uint8_t> input,
                        std::unordered_map<uint64_t, std::vector<uint64_t>>& anchors,
                        std::vector<std::pair<uint64_t, uint64_t>>& provisional_anchors) {
    GroupStage stage;
    std::vector<uint8_t> history_input;
    history_input.reserve(history.size() + input.size());
    history_input.insert(history_input.end(), history.begin(), history.end());
    history_input.insert(history_input.end(), input.begin(), input.end());
    const size_t history_bytes = history.size();
    for (size_t group_offset = 0; group_offset < input.size(); group_offset += kGroupBytes) {
        const size_t group_start = history_bytes + group_offset;
        const size_t group_end = std::min(history_bytes + input.size(), group_start + kGroupBytes);
        ++stage.groups;
        size_t position = group_start;
        size_t residual_start = stage.residual.size();
        while (position < group_end) {
            uint64_t source_position = 0;
            size_t length = 0;
            if (position + kAnchorBytes <= group_end) {
                const uint64_t key = gear_hash(history_input.data() + position, kAnchorBytes);
                const auto found = anchors.find(key);
                if (found != anchors.end()) {
                    for (auto candidate = found->second.rbegin(); candidate != found->second.rend(); ++candidate) {
                        const size_t q = static_cast<size_t>(*candidate);
                        if (q >= position || q + kAnchorBytes > position ||
                            std::memcmp(history_input.data() + q,
                                        history_input.data() + position, kAnchorBytes) != 0)
                            continue;
                        const size_t max_length = std::min(group_end - position, position - q);
                        size_t match = kAnchorBytes;
                        while (match < max_length && history_input[q + match] == history_input[position + match]) ++match;
                        if (match > length) { source_position = q; length = match; }
                        if (length == max_length) break;
                    }
                }
            }
            if (length >= kAnchorBytes) {
                if (position > history_input.size() - length || source_position + length > position)
                    throw std::logic_error("GRZ group reference escaped committed history");
                stage.tokens.push_back({true, source_position, static_cast<uint32_t>(length)});
                ++stage.references;
                position += length;
                residual_start = stage.residual.size();
                continue;
            }
            const size_t add_start = position;
            do {
                stage.residual.push_back(history_input[position++]);
                if (position + kAnchorBytes <= group_end) {
                    const uint64_t key = gear_hash(history_input.data() + position, kAnchorBytes);
                    anchors[key].push_back(position);
                    provisional_anchors.emplace_back(key, position);
                }
            } while (position < group_end &&
                     (position + kAnchorBytes > group_end || anchors[gear_hash(history_input.data() + position, kAnchorBytes)].empty()));
            stage.tokens.push_back({false, static_cast<uint64_t>(residual_start),
                                    static_cast<uint32_t>(stage.residual.size() - residual_start)});
            residual_start = stage.residual.size();
            (void)add_start;
        }
    }
    return stage;
}

std::vector<uint8_t> group_pack(const GroupStage& stage, uint64_t history_bytes,
                                std::span<const uint8_t> residual_wire) {
    std::vector<uint8_t> output;
    output.reserve(16 + stage.tokens.size() * 13 + residual_wire.size());
    put_u32(output, kGroupRlzMagic);
    put_u32(output, stage.groups);
    put_u64(output, stage.references);
    put_u64(output, stage.tokens.size());
    put_u64(output, history_bytes);
    for (const GroupToken& token : stage.tokens) {
        output.push_back(token.copy ? 1 : 0);
        put_u64(output, token.value);
        put_u32(output, token.length);
    }
    put_u64(output, residual_wire.size());
    output.insert(output.end(), residual_wire.begin(), residual_wire.end());
    return output;
}

struct GroupDecoded {
    std::vector<uint8_t> raw;
    uint64_t references = 0;
};

GroupDecoded group_unpack(const uint8_t* wire, size_t size,
                          std::span<const uint8_t> history,
                          residual_group::Codec& residual_codec) {
    const uint8_t* cursor = wire;
    const uint8_t* end = wire + size;
    if (get_u32(cursor, end) != kGroupRlzMagic) throw std::invalid_argument("GRZ group magic is invalid");
    const uint32_t groups = get_u32(cursor, end);
    const uint64_t references = get_u64(cursor, end);
    const uint64_t token_count = get_u64(cursor, end);
    const uint64_t history_bytes = get_u64(cursor, end);
    if (history_bytes != history.size())
        throw std::invalid_argument("GRZ group history does not match route prefix");
    if (groups == 0 || token_count > size / 13 + 1)
        throw std::invalid_argument("GRZ group counts are invalid");
    struct WireToken { bool copy; uint64_t value; uint32_t length; };
    std::vector<WireToken> tokens;
    tokens.reserve(static_cast<size_t>(token_count));
    for (uint64_t i = 0; i < token_count; ++i) {
        if (cursor >= end) throw std::invalid_argument("GRZ group token truncated");
        const uint8_t kind = *cursor++;
        if (kind > 1) throw std::invalid_argument("GRZ group token kind is invalid");
        tokens.push_back({kind == 1, get_u64(cursor, end), get_u32(cursor, end)});
    }
    const uint64_t residual_size = get_u64(cursor, end);
    if (residual_size > static_cast<uint64_t>(end - cursor))
        throw std::invalid_argument("GRZ residual payload truncated");
    std::vector<uint8_t> empty_residual;
    const std::vector<uint8_t>* residual = &empty_residual;
    if (residual_size != 0) {
        const auto residual_frame = residual_codec.decode(cursor, static_cast<size_t>(residual_size));
        if (residual_frame.wire_bytes != residual_size)
            throw std::invalid_argument("GRZ residual selector has trailing bytes");
        empty_residual = residual_frame.raw;
        residual = &empty_residual;
    }
    cursor += residual_size;
    if (cursor != end) throw std::invalid_argument("GRZ group container has trailing bytes");
    GroupDecoded result;
    result.raw.assign(history.begin(), history.end());
    size_t residual_offset = 0;
    for (const WireToken token : tokens) {
        if (token.length == 0) throw std::invalid_argument("GRZ group token has zero length");
        if (token.copy) {
            if (token.value >= result.raw.size() || token.length > result.raw.size() - token.value)
                throw std::invalid_argument("GRZ group COPY is outside committed history");
            const size_t start = result.raw.size();
            result.raw.resize(start + token.length);
            for (uint32_t i = 0; i < token.length; ++i) result.raw[start + i] = result.raw[token.value + i];
            ++result.references;
        } else {
            if (token.value != residual_offset || residual_offset > residual->size() ||
                token.length > residual->size() - residual_offset)
                throw std::invalid_argument("GRZ group ADD does not match residual stream");
            result.raw.insert(result.raw.end(), residual->begin() + residual_offset,
                              residual->begin() + residual_offset + token.length);
            residual_offset += token.length;
        }
    }
    if (residual_offset != residual->size() || result.references != references)
        throw std::invalid_argument("GRZ group reference/residual accounting differs");
    result.raw.erase(result.raw.begin(), result.raw.begin() + history.size());
    return result;
}

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
    std::vector<uint8_t> encode_history;
    std::vector<uint8_t> encode_pending;
    std::unordered_map<uint64_t, std::vector<uint64_t>> encode_anchors;
    std::vector<std::pair<uint64_t, uint64_t>> encode_provisional_anchors;
    std::vector<uint8_t> decode_history;
    std::vector<uint8_t> decode_pending;
};

void rollback_provisional_anchors(
    std::unordered_map<uint64_t, std::vector<uint64_t>>& anchors,
    std::vector<std::pair<uint64_t, uint64_t>>& provisional_anchors) noexcept {
    for (auto it = provisional_anchors.rbegin(); it != provisional_anchors.rend(); ++it) {
        auto found = anchors.find(it->first);
        if (found == anchors.end() || found->second.empty() ||
            found->second.back() != it->second)
            continue;
        found->second.pop_back();
        if (found->second.empty()) anchors.erase(found);
    }
    provisional_anchors.clear();
}

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

    if (!state_->encode_pending.empty())
        throw std::logic_error("GRZ_RESIDUAL route has an uncommitted TU");
    if (state_->encode_history.size() > limits.max_history_bytes ||
        exact_input.size() > limits.max_history_bytes - state_->encode_history.size())
        throw std::length_error("GRZ_RESIDUAL route history exceeds the local cap");
    try {
        const GroupStage stage = group_encode(state_->encode_history, exact_input,
                                              state_->encode_anchors,
                                              state_->encode_provisional_anchors);
        residual_group::Kind selected = residual_group::Kind::Zstd3;
        const std::vector<uint8_t> residual_wire = state_->codec.encode(
            stage.residual.data(), stage.residual.size(), &selected);
        std::vector<uint8_t> encoded = group_pack(stage, state_->encode_history.size(), residual_wire);
        if (encoded.empty() || encoded.size() > limits.max_encoded_body_bytes) {
            rollback_provisional_anchors(state_->encode_anchors,
                                         state_->encode_provisional_anchors);
            throw std::length_error("GRZ_RESIDUAL encoded BODY exceeds the local cap");
        }

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
        state_->encode_pending.assign(exact_input.begin(), exact_input.end());
        return result;
    } catch (...) {
        rollback_provisional_anchors(state_->encode_anchors,
                                     state_->encode_provisional_anchors);
        throw;
    }
}

void GrzResidualCodec::commit() {
    if (!state_->encode_pending.empty()) {
        state_->encode_history.insert(state_->encode_history.end(),
                                      state_->encode_pending.begin(),
                                      state_->encode_pending.end());
        state_->encode_pending.clear();
        state_->encode_provisional_anchors.clear();
    }
    if (!state_->decode_pending.empty()) {
        state_->decode_history.insert(state_->decode_history.end(),
                                      state_->decode_pending.begin(),
                                      state_->decode_pending.end());
        state_->decode_pending.clear();
    }
}

void GrzResidualCodec::discard() noexcept {
    rollback_provisional_anchors(state_->encode_anchors,
                                 state_->encode_provisional_anchors);
    state_->encode_pending.clear();
    state_->decode_pending.clear();
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
    if (!state_->decode_pending.empty())
        throw std::logic_error("GRZ_RESIDUAL route has an uncommitted decoded TU");
    const GroupDecoded decoded = group_unpack(encoded_body.data(), encoded_body.size(),
                                              state_->decode_history, state_->codec);
    if (decoded.raw.size() != begin.raw_bytes)
        throw std::invalid_argument("GRZ_RESIDUAL decoded size differs");
    if (icecc::digest128(decoded.raw) != begin.raw_digest)
        throw std::invalid_argument("GRZ_RESIDUAL raw input digest differs");
    state_->decode_pending = decoded.raw;
    return decoded.raw;
}

std::vector<uint8_t> decode_grz_residual(
    const TxBegin& begin, std::span<const uint8_t> encoded_body,
    ZstdTuLimits limits) {
    GrzResidualCodec codec;
    return codec.decode(begin, encoded_body, limits);
}

uint64_t grz_residual_group_reference_count(std::span<const uint8_t> body) noexcept {
    if (body.size() < 24) return 0;
    const uint8_t* cursor = body.data();
    const uint8_t* end = cursor + body.size();
    try {
        if (get_u32(cursor, end) != kGroupRlzMagic) return 0;
        (void)get_u32(cursor, end);
        return get_u64(cursor, end);
    } catch (...) {
        return 0;
    }
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
    codec_.commit();
    clear_active(); state_ = State::Idle;
}

void GrzResidualDialogue::discard_tentative() noexcept {
    codec_.discard();
    if (state_ != State::Idle && state_ != State::Terminal) { clear_active(); state_ = State::Idle; }
}
void GrzResidualDialogue::disconnect() { codec_.discard(); clear_active(); state_ = State::Terminal; }
void GrzResidualDialogue::reset() {
    if (state_ != State::Terminal) throw std::logic_error("GRZ_RESIDUAL reset requires a terminal dialogue");
    clear_active(); state_ = State::Idle;
}
uint64_t GrzResidualDialogue::window_limit_bytes() const noexcept {
    return uint64_t{1} << limits_.max_window_log;
}
[[noreturn]] void GrzResidualDialogue::protocol_error(const char* message) {
    codec_.discard(); clear_active(); state_ = State::Terminal; throw std::invalid_argument(message);
}
void GrzResidualDialogue::clear_active() { active_.reset(); std::vector<uint8_t>().swap(body_); }

} // namespace icecc::p50

#endif
