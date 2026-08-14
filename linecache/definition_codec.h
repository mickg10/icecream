// definition_codec.h
//
// D2 "relative-LZ definition codec" for the icecream #16 distinct-line dictionary.
//
// PURPOSE. In the line-dedup transport (issue #16) the dominant cold cost is the
// DISTINCT-LINE-TEXT dictionary: every distinct preprocessed line must be shipped once.
// Plain zstd at level <=3 caps the achievable whole-corpus ratio because z3's small
// window (~2 MiB) cannot reach the LONG-RANGE cross-line redundancy in that dictionary
// (shared #line paths, repeated template instantiations, long qualified names, common
// declaration fragments). zstd-19+LDM would catch it but is far too slow for the >=1 GB/s
// hot path. So we do LDM-at-z3-speed EXPLICITLY: an unbounded-window relative-LZ pass over
// ALL previously-shipped distinct-line bytes emits COPY/LITERAL segments; the harness then
// z3-entropy-codes the (much smaller, redundancy-factored) segment stream.
//
// MODEL. A DefCodec instance owns a growing byte "store" = the concatenation of every line
// already encoded (or decoded), in the exact order lines were presented. encode() covers a
// NEW distinct line as a sequence of COPY(distance,len)/LITERAL(bytes) segments matched
// against the frozen store (prior lines only -- never the current line, so no overlapping
// copies), then appends the new line's raw bytes to the store. decode() replays a segment
// stream against its OWN identically-grown store and appends the reconstructed line. Feed
// the encoder and decoder the identical line sequence => byte-exact reconstruction.
//
// OFFSETS. Matches are encoded as a DISTANCE (current write position - source position),
// plus a single repeat-offset slot (rep0). Distance-based rep0 makes the common
// "near-duplicate line = prefix + substituted-middle + suffix" cost almost nothing: the
// suffix copy has the same distance as the prefix copy and codes as a 1-byte rep token,
// which z3 then packs hard. This is the key to beating plain-z3 on the dictionary.
//
// INTERFACE. This is the exact interface the codec harness (superblock, mixture-bench.cpp)
// #includes and integrates against; conform to it. Default construction is usable as-is
// (DefCodec d; d.reset(); d.encode(...); d.decode(...)). Params exist only for the
// standalone sweep in test_defcodec.cpp and never need to be set for integration.
//
// build: g++ -O3 -march=native -std=c++17 definition_codec.cpp test_defcodec.cpp -o ... -lzstd

#ifndef DEFINITION_CODEC_H
#define DEFINITION_CODEC_H

#include <cstdint>
#include <cstddef>
#include <vector>

// ============================================================================================
// FrontCodec -- the RECOMMENDED offset-free dictionary codec (see test_defcodec results).
//
// Measurement verdict: on the distinct-line dictionary, explicit relative-LZ (DefCodec below)
// LOSES to plain z3, because the redundancy z3's window "misses" is negligible (z3 + full
// window + LDM buys only +0.4..1.6% on every corpus) while an explicit offset stream costs
// more than it saves. The dict is entropy/effort-bound, not window-bound.
//
// What DOES help at z3 speed, offset-free: treat the dict as a SET of strings, sort it, assign
// canonical line-id := sorted rank, and FRONT-CODE (store each sorted line as LCP-with-previous
// + its suffix). No offset stream => z3 packs the two homogeneous streams far better. Measured
// dict reduction vs plain-z3: RocksDB 1.27x, LLVM 1.10x, OpenCV 1.10x, DuckDB 1.08x. Byte-exact.
//
// This is a BATCH codec (it needs the whole set to sort), so it fits a 2-pass COLD encoder --
// collect all distinct lines, then assign rank-ids -- not the streaming per-line DefCodec
// interface. Assigning ids by sorted rank is permutation-free (nothing extra shipped) but
// re-locates id space from temporal to lexical order; the effect on the per-TU id streams is
// for the transport owner to measure. Suffix streams are '\n'-self-delimiting exactly like the
// baseline distinct blob (every preprocessed line ends in '\n'), so no length side-stream.
// ============================================================================================
struct FrontCodec {
    struct Encoded {
        std::vector<uint32_t> rank;   // rank[k] = original index of the k-th line in sorted order (id := k)
        std::vector<uint8_t>  lcp;    // varint LCP(line k, line k-1) per sorted line
        std::vector<uint8_t>  suf;    // suffix bytes (line k minus its LCP prefix, incl. trailing '\n')
    };
    // Sort + front-code the set given as (blob, off[], len[], N); input order is irrelevant.
    static Encoded encode_set(const uint8_t* blob, const uint32_t* off, const uint32_t* len, size_t N);
    // Reconstruct the N sorted lines from the lcp+suf streams into out_blob/out_off/out_len (in
    // sorted/rank order); out_off[k]/out_len[k] index out_blob. Returns false on malformed input.
    static bool decode_set(const uint8_t* lcp, size_t lcp_n, const uint8_t* suf, size_t suf_n,
                           size_t N, std::vector<uint8_t>& out_blob,
                           std::vector<uint32_t>& out_off, std::vector<uint32_t>& out_len);
};

struct DefCodec {
    // --- required interface (superblock integrates against exactly this) ---
    void reset();

    // Encode one NEW distinct line's raw bytes as a self-describing COPY/LITERAL segment
    // stream over the growing byte-index of ALL previously-encoded line bytes; append this
    // line to the internal store. Overwrites `seg` with the raw segment bytes.
    void encode(const uint8_t* line, uint32_t len, std::vector<uint8_t>& seg);

    // Reconstruct the exact line from `seg` against an IDENTICAL growing store; append it.
    // Returns false only on a malformed/inconsistent segment stream (never for our own
    // encoder output). `out_line` is overwritten with the reconstructed line.
    bool decode(const uint8_t* seg, size_t n, std::vector<uint8_t>& out_line);

    uint64_t store_bytes() const { return store_.size(); }   // accounting / audit

    // --- tuning knobs (defaults are the integration defaults; sweep-only) ---
    struct Params {
        uint32_t table_bits = 22;   // hash-table size = 1<<table_bits single cells
        uint32_t stride     = 8;    // insert one store anchor every `stride` bytes (LDM-style)
        uint32_t min_match  = 16;   // shortest COPY emitted (>= stride+8 keeps long matches findable)
        uint32_t chain      = 1;    // hash-chain depth searched at match time (1 = single cell)
        uint32_t skip_log   = 31;   // LZ4-style literal skip accel: after 2^skip_log misses, step+1.
                                    // 31 = probe every byte (max ratio); ~6 = fast. Speed knob only.
        bool     cross_line_rep = true;  // carry rep0 across line boundaries
    };
    // Construct with defaults (integration) or with a swept Params (bench). Changing Params
    // is only valid before the first encode()/decode() since reset().
    DefCodec() = default;
    explicit DefCodec(const Params& p) : params_(p) {}
    void set_params(const Params& p) { params_ = p; }        // call before first encode
    const Params& params() const { return params_; }

    // --- experimental split-stream encoder (bench-only ratio probe; NOT part of the
    // integration contract). Routes the SAME matcher's literals / control tokens / offset
    // tokens into three separate caller streams so the bench can measure the upside of a
    // homogeneous split layout (each stream z3'd on its own) versus one concatenated seg
    // blob. Appends; does not touch the store used by encode()/decode(). See .cpp. ---
    void encode_split(const uint8_t* line, uint32_t len,
                      std::vector<uint8_t>& lit_stream,
                      std::vector<uint8_t>& ctrl_stream,
                      std::vector<uint8_t>& off_stream);

    uint64_t split_store_bytes() const { return split_store_.size(); }

private:
    // core greedy matcher; both encode() and encode_split() drive it through a sink.
    // Matches `line` against the frozen `store` (positions < store.size()) via `head`/`tag`/
    // `prev`; updates the repeat-offset `rep0`. Does NOT modify the store (caller appends).
    // A 1-byte tag per cell is checked before the (cache-missing) 8-byte verify, so only true
    // anchor matches touch the store -- this is what keeps the encoder near LZ4-fast speed.
    template <class Sink>
    void encode_core(const uint8_t* line, uint32_t len, Sink& sink,
                     const std::vector<uint8_t>& store, const std::vector<uint32_t>& head,
                     const std::vector<uint8_t>& tag, const std::vector<uint32_t>& prev,
                     uint32_t& rep0);

    // Append `line` to `store` and index its stride-aligned anchors into head/tag/prev.
    void index_append(std::vector<uint8_t>& store, std::vector<uint32_t>& head,
                      std::vector<uint8_t>& tag, std::vector<uint32_t>& prev,
                      uint64_t& insert_cursor, const uint8_t* line, uint32_t len);

    void ensure_index() {
        if (head_.empty()) {
            head_.assign(size_t(1) << params_.table_bits, kEmpty);
            tag_.assign(size_t(1) << params_.table_bits, 0);
        }
    }
    void ensure_split_index() {
        if (split_head_.empty()) {
            split_head_.assign(size_t(1) << params_.table_bits, kEmpty);
            split_tag_.assign(size_t(1) << params_.table_bits, 0);
        }
    }

    static constexpr uint32_t kEmpty = 0xFFFFFFFFu;

    Params params_{};

    // encode()/decode() state
    std::vector<uint8_t>  store_;          // concatenation of all encoded/decoded lines
    std::vector<uint32_t> head_;           // hash -> most-recent store position (+ chain via prev_)
    std::vector<uint8_t>  tag_;            // 1-byte confirmation tag per head cell (skip false verifies)
    std::vector<uint32_t> prev_;           // chain links, indexed by store position (only if chain>1)
    uint64_t insert_cursor_ = 0;           // next store position to index (stride-aligned to 0)
    uint32_t rep0_ = 0;                    // repeat-offset slot for encode()

    // encode_split() state (independent store/index so both can run in one bench process)
    std::vector<uint8_t>  split_store_;
    std::vector<uint32_t> split_head_;
    std::vector<uint8_t>  split_tag_;
    std::vector<uint32_t> split_prev_;
    uint64_t split_insert_cursor_ = 0;
    uint32_t split_rep0_ = 0;
};

#endif // DEFINITION_CODEC_H
