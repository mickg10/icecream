#pragma once

#include "protocol50.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace icecc::p50 {

// Component encodings are interpreted within the selected profile.
constexpr uint16_t kZstdTuNoDictionaryEncoding = 0;
constexpr uint16_t kZstdTuBodyEncoding = 1;
// ZSTD_ROUTE uses the same empty descriptor and component encoding as
// ZSTD_TU. Its body is an independently terminated frame with a bounded
// exact-raw route prefix.
constexpr uint16_t kZstdRouteNoDictionaryEncoding = kZstdTuNoDictionaryEncoding;
constexpr uint16_t kZstdRouteBodyEncoding = kZstdTuBodyEncoding;

struct ZstdTuLimits {
    uint64_t max_encoded_body_bytes = 0;
    uint64_t max_raw_bytes = 0;

    // Streaming decode retains a history window independently of the encoded
    // and exact-output buffers. Keep that allocation explicit and locally
    // configurable; 27 is Zstd's conventional 128 MiB default.
    int max_window_log = 27;
    // Relationship history is retained as a bounded suffix of exact raw
    // predecessor bytes. This is the route's rolling prefix window.
    uint64_t max_history_bytes = uint64_t{128} << 20;

    auto operator<=>(const ZstdTuLimits&) const = default;
};

struct ZstdTuEnvelope {
    TxBegin begin;
    std::vector<uint8_t> body;
    auto operator<=>(const ZstdTuEnvelope&) const = default;
};

// One validation authority for every object that owns ZSTD_TU limits. Keep
// endpoint construction and codec execution on the same accepted range.
void validate_zstd_tu_limits(ZstdTuLimits limits);

class ZstdTuCodec {
public:
    explicit ZstdTuCodec(int compression_level = 1);
    ~ZstdTuCodec();
    ZstdTuCodec(const ZstdTuCodec&) = delete;
    ZstdTuCodec& operator=(const ZstdTuCodec&) = delete;

    ZstdTuEnvelope encode(HistoryNonce history_nonce, RelSeq rel_seq,
                          TuSeq tu_seq, Digest128 pre_state_digest,
                          std::span<const uint8_t> exact_input,
                          ZstdTuLimits limits);
    std::vector<uint8_t> decode(const TxBegin& begin,
                                std::span<const uint8_t> encoded_body,
                                ZstdTuLimits limits);

private:
    struct Contexts;
    int compression_level_ = 1;
    std::unique_ptr<Contexts> contexts_;
};

ZstdTuEnvelope encode_zstd_tu(HistoryNonce history_nonce, RelSeq rel_seq,
                              TuSeq tu_seq, Digest128 pre_state_digest,
                              std::span<const uint8_t> exact_input,
                              int compression_level = 1,
                              ZstdTuLimits limits = {
                                  std::numeric_limits<uint64_t>::max(),
                                  std::numeric_limits<uint64_t>::max()});

void validate_zstd_tu_begin(const TxBegin& begin, ZstdTuLimits limits);

std::vector<uint8_t> decode_zstd_tu(const TxBegin& begin,
                                    std::span<const uint8_t> encoded_body,
                                    ZstdTuLimits limits);

using ZstdRouteEnvelope = ZstdTuEnvelope;

// A route uses a bounded exact-raw suffix as the single-use prefix for each
// independently terminated level-3 frame. Keeping the prefix explicit makes
// retry byte identity independent of a live CCtx and bounds predecessor work.
class ZstdRouteCodec {
public:
    explicit ZstdRouteCodec(int compression_level = 3);
    ~ZstdRouteCodec();
    ZstdRouteCodec(const ZstdRouteCodec&) = delete;
    ZstdRouteCodec& operator=(const ZstdRouteCodec&) = delete;

    ZstdRouteEnvelope encode(HistoryNonce history_nonce, RelSeq rel_seq,
                             TuSeq tu_seq, Digest128 pre_state_digest,
                             std::span<const uint8_t> predecessor_history,
                             std::span<const uint8_t> exact_input,
                             ZstdTuLimits limits);
    ZstdRouteEnvelope encode(HistoryNonce history_nonce, RelSeq rel_seq,
                             TuSeq tu_seq, Digest128 pre_state_digest,
                             std::span<const uint8_t> exact_input,
                             ZstdTuLimits limits);
    std::vector<uint8_t> decode(
        const std::vector<ZstdRouteEnvelope>& committed,
        const TxBegin& begin, std::span<const uint8_t> encoded_body,
        ZstdTuLimits limits);
    std::vector<uint8_t> decode(std::span<const uint8_t> predecessor_history,
                                const TxBegin& begin,
                                std::span<const uint8_t> encoded_body,
                                ZstdTuLimits limits);

private:
    struct Contexts;
    int compression_level_ = 3;
    std::unique_ptr<Contexts> contexts_;
};

ZstdRouteEnvelope encode_zstd_route(HistoryNonce history_nonce, RelSeq rel_seq,
                                    TuSeq tu_seq, Digest128 pre_state_digest,
                                    std::span<const uint8_t> exact_input,
                                    ZstdTuLimits limits = {
                                        uint64_t{64} << 20,
                                        uint64_t{2} << 30});

// One relationship-scoped dialogue. It retains one bounded exact-raw suffix
// and reconstructs a tentative decoder from that prefix for each candidate
// TU. Rejection/disconnect never mutates the committed predecessor.
class ZstdRouteDialogue {
public:
    enum class State {
        Idle,
        ReceivingBody,
        BodyClosed,
        Materialized,
        Terminal,
    };

    ZstdRouteDialogue(uint32_t negotiated_profiles, ZstdTuLimits limits);
    void begin(const TxBegin& begin_value);
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
    [[nodiscard]] size_t retained_history_bytes() const { return history_.size(); }
    [[nodiscard]] size_t retained_history_entries() const {
        return history_.empty() ? 0 : 1;
    }
    [[nodiscard]] const std::optional<TxBegin>& active_begin() const {
        return active_;
    }

private:
    [[noreturn]] void protocol_error(const char* message);
    void clear_active();

    uint32_t negotiated_profiles_ = 0;
    ZstdTuLimits limits_{};
    ZstdRouteCodec codec_{};
    State state_ = State::Idle;
    std::optional<TxBegin> active_;
    std::vector<uint8_t> body_;
    std::vector<uint8_t> active_raw_;
    std::vector<uint8_t> history_;
    std::optional<HistoryNonce> history_nonce_;
    std::optional<RelSeq> last_rel_;
    std::optional<Digest128> committed_state_;
};

// Receiver-side logical transaction core for the first M2 profile. Networking,
// reconnect classification, route commit and InputRecord publication remain
// outside this class. One instance belongs to one installed message session.
class ZstdTuDialogue {
public:
    enum class State {
        Idle,
        ReceivingBody,
        BodyClosed,
        Materialized,
        Terminal,
    };

    ZstdTuDialogue(uint32_t negotiated_profiles, ZstdTuLimits limits);

    // Any second TX_BEGIN on the same live session is a terminal protocol error.
    // Whole-TU replay uses a newly reconciled dialogue/session instance.
    void begin(const TxBegin& begin);

    // ZSTD_TU has a descriptor-complete empty DICT and no Need/Fill exchange.
    // Receiving any of those messages is terminal for the session.
    void append_dict(const DictMessage& message);
    void append_body(const BodyMessage& message);
    void receive_need(const NeedMessage& message);
    void receive_fill(const FillMessage& message);

    // Returns exact verified bytes. The caller must publish a restartable
    // InputRecord and route state before calling commit_visible().
    std::vector<uint8_t> materialize();
    void commit_visible(const TxCommit& commit);
    void discard_tentative() noexcept;

    // A disconnected dialogue is never reused. A replacement session creates a
    // new instance and replays the complete immutable transaction.
    void disconnect();
    void reset();

    [[nodiscard]] uint64_t window_limit_bytes() const noexcept;

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool terminal() const { return state_ == State::Terminal; }
    [[nodiscard]] size_t pending_body_bytes() const { return body_.size(); }
    [[nodiscard]] const std::optional<TxBegin>& active_begin() const {
        return active_;
    }

private:
    [[noreturn]] void protocol_error(const char* message);
    void clear_active();

    uint32_t negotiated_profiles_ = 0;
    ZstdTuLimits limits_{};
    ZstdTuCodec codec_{};
    State state_ = State::Idle;
    std::optional<TxBegin> active_;
    std::vector<uint8_t> body_;
};

}  // namespace icecc::p50
