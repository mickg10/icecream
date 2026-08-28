#pragma once

#include "p50_zstd.h"

namespace icecc::p50 {

constexpr uint16_t kGrzResidualNoDictionaryEncoding = 0;
constexpr uint16_t kGrzResidualBodyEncoding = 1;

using GrzResidualEnvelope = ZstdTuEnvelope;

inline constexpr bool grz_residual_available() noexcept {
#if defined(ICECC_P50_WITH_LIBBSC)
    return true;
#else
    return false;
#endif
}

class GrzResidualCodec {
public:
    GrzResidualCodec();
    ~GrzResidualCodec();
    GrzResidualCodec(const GrzResidualCodec&) = delete;
    GrzResidualCodec& operator=(const GrzResidualCodec&) = delete;

    GrzResidualEnvelope encode(HistoryNonce history_nonce, RelSeq rel_seq,
                               TuSeq tu_seq, Digest128 pre_state_digest,
                               std::span<const uint8_t> exact_input,
                               ZstdTuLimits limits);
    std::vector<uint8_t> decode(const TxBegin& begin,
                                std::span<const uint8_t> encoded_body,
                                ZstdTuLimits limits);

private:
    struct State;
    std::unique_ptr<State> state_;
};

GrzResidualEnvelope encode_grz_residual(
    HistoryNonce history_nonce, RelSeq rel_seq, TuSeq tu_seq,
    Digest128 pre_state_digest, std::span<const uint8_t> exact_input,
    ZstdTuLimits limits = {uint64_t{64} << 20, uint64_t{2} << 30});

void validate_grz_residual_begin(const TxBegin& begin, ZstdTuLimits limits);
std::vector<uint8_t> decode_grz_residual(const TxBegin& begin,
                                          std::span<const uint8_t> encoded_body,
                                          ZstdTuLimits limits);

class GrzResidualDialogue {
public:
    enum class State { Idle, ReceivingBody, BodyClosed, Materialized, Terminal };

    GrzResidualDialogue(uint32_t negotiated_profiles, ZstdTuLimits limits);
    void begin(const TxBegin& begin);
    void append_dict(const DictMessage& message);
    void append_body(const BodyMessage& message);
    void receive_need(const NeedMessage& message);
    void receive_fill(const FillMessage& message);
    std::vector<uint8_t> materialize();
    void commit_visible(const TxCommit& commit);
    void discard_tentative() noexcept;
    void disconnect();
    void reset();

    [[nodiscard]] uint64_t window_limit_bytes() const noexcept;
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool terminal() const { return state_ == State::Terminal; }
    [[nodiscard]] size_t pending_body_bytes() const { return body_.size(); }
    [[nodiscard]] const std::optional<TxBegin>& active_begin() const { return active_; }

private:
    [[noreturn]] void protocol_error(const char* message);
    void clear_active();
    uint32_t negotiated_profiles_ = 0;
    ZstdTuLimits limits_{};
    GrzResidualCodec codec_;
    State state_ = State::Idle;
    std::optional<TxBegin> active_;
    std::vector<uint8_t> body_;
};

} // namespace icecc::p50
