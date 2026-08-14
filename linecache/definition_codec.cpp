// definition_codec.cpp -- see definition_codec.h for the design rationale.
//
// Unbounded-window relative-LZ over the growing distinct-line store. LDM-style sparse
// insertion (one anchor every `stride` bytes) keeps the encoder near LZ4-fast speed while
// still catching long-range cross-line redundancy; z3 then entropy-codes the residual.

#include "definition_codec.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr uint32_t kAnchor = 8;                 // we hash a full 8-byte load
constexpr uint64_t kHashMul = 0x9E3779B185EBCA87ULL;

static inline uint64_t load64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

// 8-byte anchor -> (table index of `bits` width, 1-byte confirmation tag) from one multiply.
// Index uses the top bits (best-mixed); tag uses independent middle bits.
static inline void hash_split(uint64_t x, uint32_t bits, uint32_t& idx, uint8_t& tag) {
    const uint64_t hp = x * kHashMul;
    idx = uint32_t(hp >> (64 - bits));
    tag = uint8_t(hp >> 24);
}

static inline void put_varint(std::vector<uint8_t>& o, uint64_t v) {
    while (v >= 0x80) { o.push_back(uint8_t(v) | 0x80); v >>= 7; }
    o.push_back(uint8_t(v));
}
// Read a LEB128 varint; advances *p. Returns false on truncation.
static inline bool get_varint(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
    uint64_t v = 0; uint32_t shift = 0;
    while (p < end) {
        uint8_t b = *p++;
        v |= uint64_t(b & 0x7F) << shift;
        if (!(b & 0x80)) { out = v; return true; }
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

// ---- output sinks: both receive full (literals, optional match) "sequences" ----

// One self-describing concatenated seg blob (the integration format).
struct SegSink {
    std::vector<uint8_t>& seg;
    uint32_t min_match;
    inline void sequence(const uint8_t* lit, uint32_t litlen, bool has_match,
                         uint32_t mlen, uint64_t off_code) {
        put_varint(seg, (uint64_t(litlen) << 1) | (has_match ? 1u : 0u));
        seg.insert(seg.end(), lit, lit + litlen);
        if (has_match) { put_varint(seg, mlen - min_match); put_varint(seg, off_code); }
    }
};

// Split layout (bench-only ratio probe): homogeneous literal / control / offset streams.
struct SplitSink {
    std::vector<uint8_t>& lit;
    std::vector<uint8_t>& ctrl;
    std::vector<uint8_t>& off;
    uint32_t min_match;
    inline void sequence(const uint8_t* l, uint32_t litlen, bool has_match,
                         uint32_t mlen, uint64_t off_code) {
        put_varint(ctrl, (uint64_t(litlen) << 1) | (has_match ? 1u : 0u));
        lit.insert(lit.end(), l, l + litlen);
        if (has_match) { put_varint(ctrl, mlen - min_match); put_varint(off, off_code); }
    }
};

} // namespace

// ---------------------------- FrontCodec (recommended dict codec) ----------------------------
FrontCodec::Encoded FrontCodec::encode_set(const uint8_t* blob, const uint32_t* off,
                                           const uint32_t* len, size_t N) {
    Encoded e;
    e.rank.resize(N);
    for (size_t i = 0; i < N; ++i) e.rank[i] = uint32_t(i);
    std::sort(e.rank.begin(), e.rank.end(), [&](uint32_t a, uint32_t b) {
        uint32_t la = len[a], lb = len[b], m = la < lb ? la : lb;
        int c = std::memcmp(blob + off[a], blob + off[b], m);
        return c != 0 ? c < 0 : la < lb;
    });
    e.lcp.reserve(N * 2);
    const uint8_t* prev = nullptr; uint32_t prev_len = 0;
    for (size_t k = 0; k < N; ++k) {
        const uint8_t* p = blob + off[e.rank[k]];
        uint32_t L = len[e.rank[k]];
        uint32_t l = 0, mm = prev_len < L ? prev_len : L;
        while (l < mm && prev[l] == p[l]) ++l;
        put_varint(e.lcp, l);
        e.suf.insert(e.suf.end(), p + l, p + L);   // suffix carries the trailing '\n' => self-delimiting
        prev = p; prev_len = L;
    }
    return e;
}

bool FrontCodec::decode_set(const uint8_t* lcp, size_t lcp_n, const uint8_t* suf, size_t suf_n,
                            size_t N, std::vector<uint8_t>& out_blob,
                            std::vector<uint32_t>& out_off, std::vector<uint32_t>& out_len) {
    out_blob.clear(); out_off.clear(); out_len.clear();
    out_off.reserve(N); out_len.reserve(N); out_blob.reserve(suf_n + suf_n / 2 + 64);
    const uint8_t* lp = lcp; const uint8_t* le = lcp + lcp_n;
    const uint8_t* sp = suf; const uint8_t* se = suf + suf_n;
    std::vector<uint8_t> prevline, cur;                       // separate buffer avoids self-aliasing
    for (size_t k = 0; k < N; ++k) {
        uint64_t l;
        if (!get_varint(lp, le, l)) return false;
        if (l > prevline.size()) return false;                // LCP cannot exceed previous line
        // suffix = bytes up to and including the next '\n' (lines are '\n'-terminated)
        const uint8_t* nl = (const uint8_t*)std::memchr(sp, '\n', size_t(se - sp));
        const uint8_t* send = nl ? nl + 1 : se;               // tolerate a final line without '\n'
        cur.assign(prevline.begin(), prevline.begin() + l);
        cur.insert(cur.end(), sp, send);
        out_off.push_back(uint32_t(out_blob.size()));
        out_len.push_back(uint32_t(cur.size()));
        out_blob.insert(out_blob.end(), cur.begin(), cur.end());
        prevline.swap(cur);
        sp = send;
    }
    return true;
}

void DefCodec::reset() {
    store_.clear();  head_.clear();  tag_.clear();  prev_.clear();  insert_cursor_ = 0;  rep0_ = 0;
    split_store_.clear(); split_head_.clear(); split_tag_.clear(); split_prev_.clear();
    split_insert_cursor_ = 0; split_rep0_ = 0;
}

template <class Sink>
void DefCodec::encode_core(const uint8_t* line, uint32_t len, Sink& sink,
                           const std::vector<uint8_t>& store,
                           const std::vector<uint32_t>& head, const std::vector<uint8_t>& tag,
                           const std::vector<uint32_t>& prev, uint32_t& rep0) {
    const uint32_t A = kAnchor;
    const uint32_t MM = params_.min_match;
    const uint32_t bits = params_.table_bits;
    const int max_chain = int(params_.chain);
    const bool use_chain = params_.chain > 1;
    const uint64_t S = store.size();
    const uint8_t* st = store.data();

    uint32_t lrep = params_.cross_line_rep ? rep0 : 0;
    uint32_t ip = 0, anchor = 0, miss = 0;
    const uint32_t skip_log = params_.skip_log;
    const uint32_t limit = (len >= A) ? (len - A) : 0;

    while (len >= A && ip <= limit) {
        const uint64_t probe = load64(line + ip);
        uint32_t hi; uint8_t ht; hash_split(probe, bits, hi, ht);
        uint32_t cand;
        if (!use_chain) {
            // tag gate: skip the cache-missing verify unless the 1-byte tag also matches.
            if (tag[hi] != ht) { ip += 1 + (miss++ >> skip_log); continue; }
            cand = head[hi];
        } else {
            cand = head[hi];   // chain walk verifies every link explicitly
        }

        uint32_t best_len = 0, best_mip = ip, best_mc = 0;
        int depth = max_chain;
        while (cand != kEmpty && depth-- > 0) {
            // exact 8-byte anchor check (no hash trust); cand+A<=S guaranteed by insertion.
            if (load64(st + cand) == probe) {
                uint32_t mip = ip, mc = cand;
                while (mip > anchor && mc > 0 && st[mc - 1] == line[mip - 1]) { --mip; --mc; }
                uint32_t mlen = (ip - mip) + A;                 // [mip,ip+A) already verified
                while (mc + mlen < S && mip + mlen < len && st[mc + mlen] == line[mip + mlen])
                    ++mlen;
                if (mlen > best_len) { best_len = mlen; best_mip = mip; best_mc = mc; }
            }
            if (!use_chain) break;
            cand = prev[cand];
        }

        if (best_len >= MM) {
            const uint64_t dist = (S + best_mip) - best_mc;     // write pos - source pos
            uint64_t off_code;
            if (dist == lrep) off_code = 0;                     // rep0 hit -> 1-byte token
            else { off_code = dist; lrep = uint32_t(dist); }    // literal distance (>=1)
            sink.sequence(line + anchor, best_mip - anchor, true, best_len, off_code);
            ip = best_mip + best_len;
            anchor = ip;
            miss = 0;                                           // reset skip accel on a hit
        } else {
            ip += 1 + (miss++ >> skip_log);                     // tag hit but too short: skip-advance
        }
    }

    sink.sequence(line + anchor, len - anchor, false, 0, 0);    // final literal run
    if (params_.cross_line_rep) rep0 = lrep;
}

void DefCodec::index_append(std::vector<uint8_t>& store, std::vector<uint32_t>& head,
                            std::vector<uint8_t>& tag, std::vector<uint32_t>& prev,
                            uint64_t& insert_cursor, const uint8_t* line, uint32_t len) {
    store.insert(store.end(), line, line + len);
    const uint64_t newS = store.size();
    const uint8_t* st = store.data();
    const uint32_t A = kAnchor, R = params_.stride, bits = params_.table_bits;
    const bool use_chain = params_.chain > 1;
    if (use_chain && prev.size() < newS) prev.resize(newS, kEmpty);

    uint64_t p = insert_cursor;
    while (p + A <= newS) {
        uint32_t h; uint8_t t; hash_split(load64(st + p), bits, h, t);
        if (use_chain) prev[p] = head[h];
        head[h] = uint32_t(p);
        tag[h] = t;
        p += R;
    }
    insert_cursor = p;
}

void DefCodec::encode(const uint8_t* line, uint32_t len, std::vector<uint8_t>& seg) {
    ensure_index();
    seg.clear();
    SegSink sink{seg, params_.min_match};
    encode_core(line, len, sink, store_, head_, tag_, prev_, rep0_);
    index_append(store_, head_, tag_, prev_, insert_cursor_, line, len);
}

bool DefCodec::decode(const uint8_t* seg, size_t n, std::vector<uint8_t>& out_line) {
    out_line.clear();
    const uint8_t* p = seg;
    const uint8_t* end = seg + n;
    const uint64_t S = store_.size();
    const uint32_t MM = params_.min_match;
    uint32_t rep = params_.cross_line_rep ? rep0_ : 0;

    while (p < end) {
        uint64_t ctrl;
        if (!get_varint(p, end, ctrl)) return false;
        const uint32_t litlen = uint32_t(ctrl >> 1);
        const bool has_match = ctrl & 1;
        if (uint64_t(end - p) < litlen) return false;
        out_line.insert(out_line.end(), p, p + litlen);
        p += litlen;

        if (!has_match) break;                     // final literal run (only place has_match==0)

        uint64_t mcode, off_code;
        if (!get_varint(p, end, mcode)) return false;
        if (!get_varint(p, end, off_code)) return false;
        const uint32_t mlen = uint32_t(mcode) + MM;
        uint32_t dist;
        if (off_code == 0) dist = rep;
        else { dist = uint32_t(off_code); rep = dist; }

        const uint64_t write_pos = S + out_line.size();
        if (dist == 0 || dist > write_pos) return false;
        const uint64_t src = write_pos - dist;
        if (src + mlen > S) return false;          // source must lie fully in the frozen store
        const uint8_t* sp = store_.data() + src;
        out_line.insert(out_line.end(), sp, sp + mlen);
    }

    store_.insert(store_.end(), out_line.begin(), out_line.end());
    if (params_.cross_line_rep) rep0_ = rep;
    return true;
}

void DefCodec::encode_split(const uint8_t* line, uint32_t len,
                            std::vector<uint8_t>& lit_stream,
                            std::vector<uint8_t>& ctrl_stream,
                            std::vector<uint8_t>& off_stream) {
    ensure_split_index();
    SplitSink sink{lit_stream, ctrl_stream, off_stream, params_.min_match};
    encode_core(line, len, sink, split_store_, split_head_, split_tag_, split_prev_, split_rep0_);
    index_append(split_store_, split_head_, split_tag_, split_prev_, split_insert_cursor_, line, len);
}
