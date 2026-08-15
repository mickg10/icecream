// test_defcodec.cpp -- standalone correctness + ratio + throughput bench for DefCodec.
//
// Produces the number that decides the icecream #16 bake-off: for the DISTINCT-LINE
// dictionary of a corpus, how much smaller is (DefCodec relative-LZ segs -> z3) than the
// baseline (raw distinct-line blob -> z3), at z<=3 and >=1 GB/s.
//
// Pipeline:
//   1. load a manifest of .ii files (same loader shape as superblock-online-bench.cpp);
//   2. extract DISTINCT line texts in first-appearance order (own minimal exact-equality
//      interner -- a line is bytes up to and including the next '\n', matching the harness);
//   3. baseline:  raw distinct-line blob            -> {none, z1, z3};
//      defcodec:  per-line COPY/LITERAL segs concat -> {none, z1, z3};
//      split:     homogeneous lit/ctrl/off streams  -> z3 each (experimental Pareto probe);
//   4. round-trip: decode every seg with a fresh DefCodec fed the identical sequence, assert
//      byte-exact;
//   5. throughput: encode + decode GB/s over distinct-line bytes.
//
// NO corpus-name special-casing anywhere -- it is a general codec.
//
// build: g++ -O3 -march=native -std=c++17 definition_codec.cpp test_defcodec.cpp -o test_defcodec -lzstd

#include "definition_codec.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>
#include <zstd.h>
#include <zdict.h>

using Clock = std::chrono::steady_clock;
static double secs(Clock::time_point b) { return std::chrono::duration<double>(Clock::now() - b).count(); }

// ---------------- corpus loader (manifest of .ii files) ----------------
struct FileSpan { uint64_t off; uint32_t len; };
struct Corpus { std::vector<uint8_t> bytes; std::vector<FileSpan> files; uint64_t raw = 0; };

static Corpus load_corpus(const char* manifest, size_t max_files) {
    FILE* mf = fopen(manifest, "r");
    if (!mf) { perror(manifest); exit(2); }
    std::vector<std::string> paths; char path[8192]; uint64_t total = 0;
    while (fgets(path, sizeof path, mf)) {
        size_t n = strlen(path);
        while (n && (path[n - 1] == '\n' || path[n - 1] == '\r')) path[--n] = 0;
        if (!n) continue;
        struct stat st{};
        if (stat(path, &st) != 0) { perror(path); exit(2); }
        if (st.st_size < 0 || uint64_t(st.st_size) > UINT32_MAX) { fprintf(stderr, "bad size %s\n", path); exit(2); }
        paths.emplace_back(path); total += uint64_t(st.st_size);
        if (paths.size() == max_files) break;
    }
    fclose(mf);
    Corpus c; c.bytes.resize(size_t(total) + 8); c.files.reserve(paths.size());
    uint64_t off = 0;
    for (auto& p : paths) {
        FILE* f = fopen(p.c_str(), "rb");
        if (!f) { perror(p.c_str()); exit(2); }
        struct stat st{}; fstat(fileno(f), &st); size_t n = size_t(st.st_size);
        if (n && fread(c.bytes.data() + off, 1, n, f) != n) { fprintf(stderr, "short read %s\n", p.c_str()); exit(2); }
        fclose(f); c.files.push_back({off, uint32_t(n)}); off += n;
    }
    c.raw = off; return c;
}

// ---------------- distinct-line extractor (first-appearance, exact equality) ----------------
static inline uint64_t load64u(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint64_t rd_tail(const uint8_t* p, uint32_t n) { uint64_t v = 0; memcpy(&v, p, n); return v; }
// fast word-wise 64-bit hash (wyhash-ish; quality is ample for dedup bucketing)
static inline uint64_t fast_hash(const uint8_t* p, uint32_t n) {
    constexpr uint64_t A = 0xa0761d6478bd642fULL, B = 0xe7037ed1a0b428dbULL, C = 0x8ebc6af09c88c6e3ULL;
    uint64_t h = A ^ (uint64_t(n) * B);
    while (n >= 16) { h = (h ^ load64u(p)) * C; h = (h ^ load64u(p + 8)) * C; p += 16; n -= 16; }
    if (n >= 8) { h = (h ^ load64u(p)) * C; h = (h ^ load64u(p + n - 8)) * C; }
    else if (n) { h = (h ^ rd_tail(p, n)) * C; }
    h ^= h >> 32; h *= C; h ^= h >> 29;
    return h;
}

struct Distinct {
    std::vector<uint8_t> blob;                       // distinct lines concatenated, first-appearance order
    std::vector<uint32_t> off, len;                  // per distinct line
    uint64_t total_lines = 0;
    struct Slot { uint64_t h; uint32_t off; uint32_t len; };
    std::vector<Slot> tab; uint32_t mask; size_t count = 0;
    Distinct() { tab.assign(size_t(1) << 20, {0, 0, 0}); mask = (uint32_t(1) << 20) - 1; }
    void rehash(size_t n) {
        std::vector<Slot> t(n, {0, 0, 0}); uint32_t m = uint32_t(n - 1);
        for (auto& s : tab) if (s.h) { uint32_t i = uint32_t(s.h) & m; while (t[i].h) i = (i + 1) & m; t[i] = s; }
        tab.swap(t); mask = m;
    }
    inline void add(const uint8_t* p, uint32_t n) {
        ++total_lines;
        uint64_t h = fast_hash(p, n) | 1;
        uint32_t i = uint32_t(h) & mask;
        for (;;) {
            Slot& s = tab[i];
            if (!s.h) {
                uint32_t o = uint32_t(blob.size());
                blob.insert(blob.end(), p, p + n);
                s = {h, o, n}; off.push_back(o); len.push_back(n); ++count;
                if (count * 10 > size_t(mask + 1) * 7) rehash(size_t(mask + 1) * 2);
                return;
            }
            if (s.h == h && s.len == n && memcmp(blob.data() + s.off, p, n) == 0) return;
            i = (i + 1) & mask;
        }
    }
};

static Distinct extract_distinct(const Corpus& c) {
    Distinct d;
    for (const auto& f : c.files) {
        const uint8_t* p = c.bytes.data() + f.off;
        const uint8_t* e = p + f.len;
        while (p < e) {
            const void* nl = memchr(p, '\n', size_t(e - p));
            const uint8_t* le = nl ? (const uint8_t*)nl + 1 : e;
            d.add(p, uint32_t(le - p));
            p = le;
        }
    }
    return d;
}

// ---------------- generic string interner (returns first-appearance id) ----------------
struct StringInterner {
    std::vector<uint8_t> blob; std::vector<uint32_t> off, len;
    struct Slot { uint64_t h; uint32_t id; };
    std::vector<Slot> tab; uint32_t mask; size_t count = 0;
    StringInterner() { tab.assign(size_t(1) << 16, {0, 0}); mask = (uint32_t(1) << 16) - 1; }
    void rehash(size_t n) {
        std::vector<Slot> t(n, {0, 0}); uint32_t m = uint32_t(n - 1);
        for (auto& s : tab) if (s.h) { uint32_t i = uint32_t(s.h) & m; while (t[i].h) i = (i + 1) & m; t[i] = s; }
        tab.swap(t); mask = m;
    }
    uint32_t intern(const uint8_t* p, uint32_t n) {
        uint64_t h = fast_hash(p, n) | 1; uint32_t i = uint32_t(h) & mask;
        for (;;) {
            Slot& s = tab[i];
            if (!s.h) {
                uint32_t id = uint32_t(off.size()), o = uint32_t(blob.size());
                blob.insert(blob.end(), p, p + n); off.push_back(o); len.push_back(n);
                s = {h, id}; ++count;
                if (count * 10 > size_t(mask + 1) * 7) rehash(size_t(mask + 1) * 2);
                return id;
            }
            if (s.h == h && len[s.id] == n && memcmp(blob.data() + off[s.id], p, n) == 0) return s.id;
            i = (i + 1) & mask;
        }
    }
    bool contains(const uint8_t* p, uint32_t n) const { return find(p, n) != UINT32_MAX; }
    uint32_t find(const uint8_t* p, uint32_t n) const {   // interned id, or UINT32_MAX if absent
        uint64_t h = fast_hash(p, n) | 1; uint32_t i = uint32_t(h) & mask;
        for (;;) {
            const Slot& s = tab[i];
            if (!s.h) return UINT32_MAX;
            if (s.h == h && len[s.id] == n && memcmp(blob.data() + off[s.id], p, n) == 0) return s.id;
            i = (i + 1) & mask;
        }
    }
};

static inline bool is_slot_char(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
// Tokenize a distinct-line set into skeleton keys + slot tokens (populates the interners). If the
// per-line/per-slot out-arrays are non-null, also records line->skeleton and the slot-token stream
// (used for the target; passed null when only building the prior sets).
static void tokenize_corpus(const Distinct& d, StringInterner& skel, StringInterner& tok,
                            std::vector<uint32_t>* line_skel, std::vector<uint32_t>* slot_tok,
                            std::vector<uint32_t>* line_slot_start) {
    const size_t N = d.off.size();
    std::vector<uint8_t> key;
    if (line_slot_start) line_slot_start->push_back(0);
    for (size_t i = 0; i < N; ++i) {
        const uint8_t* p = d.blob.data() + d.off[i]; uint32_t L = d.len[i];
        key.clear();
        uint32_t k = 0; bool first = true;
        for (;;) {
            uint32_t ls = k; while (k < L && !is_slot_char(p[k])) ++k;
            if (!first) key.push_back(0x00);
            first = false;
            key.insert(key.end(), p + ls, p + k);
            if (k >= L) break;
            uint32_t ss = k; while (k < L && is_slot_char(p[k])) ++k;
            uint32_t tid = tok.intern(p + ss, k - ss);
            if (slot_tok) slot_tok->push_back(tid);
        }
        uint32_t sid = skel.intern(key.data(), uint32_t(key.size()));
        if (line_skel) line_skel->push_back(sid);
        if (line_slot_start) line_slot_start->push_back(uint32_t(slot_tok ? slot_tok->size() : 0));
    }
}

// ---------------- zstd ----------------
static size_t zstd_size(ZSTD_CCtx* c, const uint8_t* data, size_t n, int level, std::vector<uint8_t>& dst) {
    ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    size_t bound = ZSTD_compressBound(n); if (dst.size() < bound) dst.resize(bound);
    size_t r = ZSTD_compress2(c, dst.data(), dst.size(), data ? data : (const uint8_t*)"", n);
    if (ZSTD_isError(r)) { fprintf(stderr, "zstd err %s\n", ZSTD_getErrorName(r)); exit(2); }
    return r;
}
// level + optional long-distance-matching + explicit windowLog (diagnostic ceiling probe).
static size_t zstd_size_adv(ZSTD_CCtx* c, const uint8_t* data, size_t n, int level,
                            int ldm, int wlog, std::vector<uint8_t>& dst) {
    ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    if (ldm) ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1);
    if (wlog) ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, wlog);
    size_t bound = ZSTD_compressBound(n); if (dst.size() < bound) dst.resize(bound);
    size_t r = ZSTD_compress2(c, dst.data(), dst.size(), data ? data : (const uint8_t*)"", n);
    if (ZSTD_isError(r)) { fprintf(stderr, "zstd err %s\n", ZSTD_getErrorName(r)); exit(2); }
    return r;
}
static inline double MB(uint64_t b) { return double(b) / (1024.0 * 1024.0); }
static inline void bput_varint(std::vector<uint8_t>& o, uint64_t v) {
    while (v >= 0x80) { o.push_back(uint8_t(v) | 0x80); v >>= 7; } o.push_back(uint8_t(v));
}

// ---------------- skeleton-grouped columnar coding (bigoracle §4.2) ----------------
// Conservatively tokenize each distinct line into a SKELETON (punctuation/keyword/whitespace
// literal fragments) and SLOTS (maximal [A-Za-z0-9_] runs = identifiers/numbers/paths). Group
// lines by identical skeleton, ship each skeleton once, and code the slots columnar (all values
// of one skeleton-column together) so template/generated lines -- globally unique but sharing a
// skeleton and differing only in low-entropy slots -- dedup structurally, which byte-LZ/entropy
// (z3..z19, LDM) cannot capture. Measures the CEILING (whole-corpus grouping = hindsight); a
// causal streaming/TU-local variant would follow only if this clears.
static void run_skeleton(const Distinct& d, uint64_t base_z3, uint64_t front_z3, uint64_t raw, ZSTD_CCtx* cc) {
    const size_t N = d.off.size();
    StringInterner skel, slottok;
    std::vector<uint32_t> line_skel(N), skel_nslots;
    std::vector<uint32_t> slot_goff, slot_glen, slot_tok;   // all slots, line order
    std::vector<uint32_t> line_slot_start; line_slot_start.reserve(N + 1); line_slot_start.push_back(0);
    std::vector<uint8_t> key;

    Clock::time_point t0 = Clock::now();
    for (size_t i = 0; i < N; ++i) {
        const uint8_t* p = d.blob.data() + d.off[i]; uint32_t L = d.len[i];
        key.clear();
        uint32_t k = 0; bool first = true;
        for (;;) {
            uint32_t ls = k; while (k < L && !is_slot_char(p[k])) ++k;
            if (!first) key.push_back(0x00);                 // separator before every lit but lit0
            first = false;
            key.insert(key.end(), p + ls, p + k);
            if (k >= L) break;
            uint32_t ss = k; while (k < L && is_slot_char(p[k])) ++k;
            slot_goff.push_back(d.off[i] + ss); slot_glen.push_back(k - ss);
            slot_tok.push_back(slottok.intern(p + ss, k - ss));
        }
        uint32_t sid = skel.intern(key.data(), uint32_t(key.size()));
        line_skel[i] = sid;
        line_slot_start.push_back(uint32_t(slot_goff.size()));
        uint32_t ns = line_slot_start[i + 1] - line_slot_start[i];
        if (sid >= skel_nslots.size()) skel_nslots.resize(sid + 1, 0);
        skel_nslots[sid] = ns;
    }
    double tok_t = secs(t0);
    const size_t num_skel = skel.off.size(), num_tok = slottok.off.size(), num_slot = slot_goff.size();

    // skeleton dict + id stream (first-appearance ids)
    std::vector<uint8_t> skel_len, id_s, tok_len;
    for (size_t s = 0; s < num_skel; ++s) bput_varint(skel_len, skel.len[s]);
    for (size_t i = 0; i < N; ++i) bput_varint(id_s, line_skel[i]);
    for (size_t t = 0; t < num_tok; ++t) bput_varint(tok_len, slottok.len[t]);

    // columnar order: stable counting-sort of lines by skeleton (first-appearance within a skel)
    std::vector<uint32_t> gstart(num_skel + 1, 0), order(N);
    for (size_t i = 0; i < N; ++i) ++gstart[line_skel[i] + 1];
    for (size_t s = 0; s < num_skel; ++s) gstart[s + 1] += gstart[s];
    { std::vector<uint32_t> cur(gstart.begin(), gstart.end());
      for (size_t i = 0; i < N; ++i) order[cur[line_skel[i]]++] = uint32_t(i); }

    std::vector<uint8_t> val_col, len_col, tid_col, val_row, len_row, tid_row;
    // columnar
    for (size_t s = 0; s < num_skel; ++s) {
        uint32_t ns = skel_nslots[s];
        for (uint32_t j = 0; j < ns; ++j)
            for (uint32_t idx = gstart[s]; idx < gstart[s + 1]; ++idx) {
                uint32_t line = order[idx], si = line_slot_start[line] + j;
                val_col.insert(val_col.end(), d.blob.data() + slot_goff[si], d.blob.data() + slot_goff[si] + slot_glen[si]);
                bput_varint(len_col, slot_glen[si]); bput_varint(tid_col, slot_tok[si]);
            }
    }
    // row-wise (causal reference)
    for (size_t i = 0; i < N; ++i)
        for (uint32_t si = line_slot_start[i]; si < line_slot_start[i + 1]; ++si) {
            val_row.insert(val_row.end(), d.blob.data() + slot_goff[si], d.blob.data() + slot_goff[si] + slot_glen[si]);
            bput_varint(len_row, slot_glen[si]); bput_varint(tid_row, slot_tok[si]);
        }

    std::vector<uint8_t> z;
    auto Z = [&](const std::vector<uint8_t>& v) { return zstd_size(cc, v.data(), v.size(), 3, z); };
    uint64_t skeldict = Z(skel.blob) + Z(skel_len);
    uint64_t idz = Z(id_s);
    uint64_t tokdict = Z(slottok.blob) + Z(tok_len);
    uint64_t vc = Z(val_col), lc = Z(len_col), tc = Z(tid_col);
    uint64_t vr = Z(val_row), lr = Z(len_row), tr = Z(tid_row);
    uint64_t col_raw = skeldict + idz + vc + lc;
    uint64_t col_int = skeldict + idz + tokdict + tc;
    uint64_t row_raw = skeldict + idz + vr + lr;
    uint64_t row_int = skeldict + idz + tokdict + tr;

    // z19 ceiling of the interned decomposition (does deeper modeling of the token-placement
    // stream break the floor?) -- level 19 + ldm + full window on each interned stream.
    auto Z19 = [&](const std::vector<uint8_t>& v) { return zstd_size_adv(cc, v.data(), v.size(), 19, 1, 27, z); };
    uint64_t skeldict19 = Z19(skel.blob) + Z19(skel_len);
    uint64_t idz19 = Z19(id_s);
    uint64_t tokdict19 = Z19(slottok.blob) + Z19(tok_len);
    uint64_t col_int19 = skeldict19 + idz19 + tokdict19 + Z19(tid_col);
    uint64_t row_int19 = skeldict19 + idz19 + tokdict19 + Z19(tid_row);

    // ---- CAUSAL CONTEXTUAL MODEL code-length simulation on the token-placement stream ----
    // Exact code length a table-driven adaptive rANS/range coder realizes (within <1%), computed
    // without building the coder yet. PPM-C: order-2 context (skeleton_id, slot_column) -> order-0
    // global -> novel (token-id implicit = next first-appearance id, so novel costs escape only;
    // the token TEXT is the separately-counted tok_dict). Processed in columnar order (all
    // occurrences of a context are consecutive => decoder reproduces the order from the skel-id
    // stream). No escape-exclusion, so these are a slight OVER-estimate (a real coder does better).
    // Slot processing order == columnar tid_col order; contexts are runs of cnt[s] within each (s,j).
    double bits_o0 = 0, bits_o2 = 0;   // order-0-only, order-2 PPM-C code lengths
    {
        std::unordered_map<uint32_t, uint32_t> g0; g0.reserve(num_tok * 2); uint64_t g0tot = 0;
        std::unordered_map<uint32_t, uint32_t> g2; g2.reserve(num_tok * 2); uint64_t g2tot = 0;
        std::unordered_map<uint32_t, uint32_t> cm; uint64_t cmtot = 0;
        auto code_o0 = [&](uint32_t t) {                 // global order-0 with novel escape
            uint32_t gd = uint32_t(g0.size()); auto it = g0.find(t);
            if (it != g0.end()) bits_o0 += -std::log2(double(it->second) / double(g0tot + gd));
            else if (gd > 0) bits_o0 += -std::log2(double(gd) / double(g0tot + gd));  // + novel(implicit id)=0
            ++g0[t]; ++g0tot;
        };
        for (size_t s = 0; s < num_skel; ++s) {
            uint32_t ns = skel_nslots[s];
            for (uint32_t j = 0; j < ns; ++j) {
                cm.clear(); cmtot = 0;
                for (uint32_t idx = gstart[s]; idx < gstart[s + 1]; ++idx) {
                    uint32_t t = slot_tok[line_slot_start[order[idx]] + j];
                    code_o0(t);
                    // order-2 -> order-0 backoff (PPM-C, full update)
                    uint32_t cd = uint32_t(cm.size()); auto it = cm.find(t);
                    if (it != cm.end()) bits_o2 += -std::log2(double(it->second) / double(cmtot + cd));
                    else {
                        if (cd > 0) bits_o2 += -std::log2(double(cd) / double(cmtot + cd));       // ctx escape
                        uint32_t gd = uint32_t(g2.size()); auto git = g2.find(t);
                        if (git != g2.end()) bits_o2 += -std::log2(double(git->second) / double(g2tot + gd));
                        else if (gd > 0) bits_o2 += -std::log2(double(gd) / double(g2tot + gd));   // + novel=0
                    }
                    ++cm[t]; ++cmtot; ++g2[t]; ++g2tot;
                }
            }
        }
    }
    uint64_t plc_o0 = uint64_t(bits_o0 / 8.0), plc_o2 = uint64_t(bits_o2 / 8.0);
    uint64_t tid_col_z19 = Z19(tid_col), tid_row_z19 = Z19(tid_row);

    // per-(skel,col) MTF-rank transform (contextual prediction) then z3/z19 (strong entropy backend).
    // rank 0 == same token as the most-recent occurrence in this context (the ~47% prev-value hit).
    // A rank == list-size means "not yet seen in this context" (escape): the rank alone can't say
    // WHICH token, so we ALSO emit an escape-identity stream (globally-novel token -> implicit next
    // id, 0 cost; already-seen token -> its global id). True MTF placement = ranks + escape ids.
    std::vector<uint8_t> mtf_col, esc_ids;
    { std::vector<uint32_t> lst; lst.reserve(256);
      std::vector<uint8_t> gseen(num_tok, 0); std::vector<uint32_t> gid(num_tok, 0); uint32_t gnext = 0;
      for (size_t s = 0; s < num_skel; ++s) { uint32_t ns = skel_nslots[s];
        for (uint32_t j = 0; j < ns; ++j) { lst.clear();
          for (uint32_t idx = gstart[s]; idx < gstart[s + 1]; ++idx) {
            uint32_t t = slot_tok[line_slot_start[order[idx]] + j];
            uint32_t r = 0; while (r < lst.size() && lst[r] != t) ++r;
            bput_varint(mtf_col, r);                              // r == lst.size() marks an escape
            if (r == lst.size()) {                               // escape: identify the token
              if (gseen[t]) bput_varint(esc_ids, gid[t]);        // known token -> global id
              else { gseen[t] = 1; gid[t] = gnext++; }           // globally novel -> implicit next id
              lst.push_back(t);
            }
            for (uint32_t m = r; m > 0; --m) lst[m] = lst[m - 1]; // move-to-front
            lst[0] = t;
          } } }
    }
    uint64_t mtf_rank_z3 = zstd_size(cc, mtf_col.data(), mtf_col.size(), 3, z);
    uint64_t esc_z3 = zstd_size(cc, esc_ids.data(), esc_ids.size(), 3, z);
    uint64_t esc_z19 = Z19(esc_ids);
    uint64_t mtf_col_z3 = mtf_rank_z3 + esc_z3;                   // full decodable MTF placement (z3)
    uint64_t mtf_col_z19 = Z19(mtf_col) + esc_z19;               // full decodable MTF placement (z19)
    uint64_t plc_best = std::min({tid_col_z19, tid_row_z19, plc_o2, mtf_col_z3, mtf_col_z19});
    uint64_t dict_ctx = skeldict + idz + tokdict + plc_best;     // full dict with best placement coder

    uint64_t skel_raw_bytes = skel.blob.size(), slot_raw_bytes = 0, tok_raw_bytes = slottok.blob.size();
    for (size_t si = 0; si < num_slot; ++si) slot_raw_bytes += slot_glen[si];

    printf("  ---- SKELETON-GROUPED COLUMNAR coding (bigoracle 4.2) [tokenize=%.2fs] ----\n", tok_t);
    printf("    distinct skeletons=%zu (%.1f lines/skel)  slot occurrences=%zu (%.2f/line)  distinct slot tokens=%zu\n",
           num_skel, double(N) / double(num_skel ? num_skel : 1), num_slot, double(num_slot) / double(N), num_tok);
    printf("    raw bytes: skeleton-keys(deduped)=%.3f  all-slot-occurrences=%.3f  distinct-slot-tokens=%.3f MiB\n",
           MB(skel_raw_bytes), MB(slot_raw_bytes), MB(tok_raw_bytes));
    printf("    z3 components (MiB): skel_dict=%.3f  id=%.3f  tok_dict=%.3f | val_col=%.3f len_col=%.3f tid_col=%.3f\n",
           MB(skeldict), MB(idz), MB(tokdict), MB(vc), MB(lc), MB(tc));
    printf("    TOTAL columnar   raw-slots = %.3f MiB   (%.3fx vs z3-plain, %.3fx vs front-code)  raw/comp %.1fx\n",
           MB(col_raw), double(base_z3) / double(col_raw), double(front_z3) / double(col_raw), double(raw) / double(col_raw));
    printf("    TOTAL columnar   interned  = %.3f MiB   (%.3fx vs z3-plain, %.3fx vs front-code)  raw/comp %.1fx\n",
           MB(col_int), double(base_z3) / double(col_int), double(front_z3) / double(col_int), double(raw) / double(col_int));
    printf("    TOTAL row-wise   raw/int   = %.3f / %.3f MiB  (causal ref; %.3fx / %.3fx vs z3-plain)\n",
           MB(row_raw), MB(row_int), double(base_z3) / double(row_raw), double(base_z3) / double(row_int));
    printf("    z19 ceiling: columnar interned=%.3f  row-wise interned=%.3f MiB  (%.3fx / %.3fx vs z3-plain; front-code+z19 was ~5.05 on DuckDB)\n",
           MB(col_int19), MB(row_int19), double(base_z3) / double(col_int19), double(base_z3) / double(row_int19));
    printf("    --- CONTEXTUAL entropy coder on the token-PLACEMENT stream (which token fills each slot) ---\n");
    printf("    placement floor:  columnar z3=%.3f z19=%.3f | row-wise z3=%.3f z19=%.3f MiB\n",
           MB(tc), MB(tid_col_z19), MB(tr), MB(tid_row_z19));
    printf("    contextual models: order0-only=%.3f  order2(skel,col)PPM-C=%.3f MiB\n", MB(plc_o0), MB(plc_o2));
    printf("    MTF(skel,col) DECODABLE = ranks %.3f + escape-ids %.3f = %.3f (z3) / %.3f (z19) MiB\n",
           MB(mtf_rank_z3), MB(esc_z3), MB(mtf_col_z3), MB(mtf_col_z19));
    { uint64_t best_ctx = std::min({plc_o2, mtf_col_z3, mtf_col_z19});   // best CONTEXTUAL model
      uint64_t best_z19 = std::min(tid_col_z19, tid_row_z19);            // best plain z19 (LZ+FSE)
      printf("    BEST placement = %.3f MiB (%.3fx vs placement-z3 %.3f); best CONTEXTUAL model %.3f vs best plain-z19 %.3f -> contextual beats z19? %s\n",
             MB(plc_best), double(tc) / double(plc_best), MB(tc), MB(best_ctx), MB(best_z19),
             best_ctx < best_z19 ? "YES" : "NO (z19 LZ+FSE already near-optimal)"); }
    printf("    FULL DICT with BEST placement = %.3f MiB (skel %.3f + id %.3f + tok %.3f + plc %.3f)  raw/comp %.1fx  (%.3fx vs z3-plain, %.3fx vs front-code)\n",
           MB(dict_ctx), MB(skeldict), MB(idz), MB(tokdict), MB(plc_best), double(raw) / double(dict_ctx),
           double(base_z3) / double(dict_ctx), double(front_z3) / double(dict_ctx));
    printf("    400x budget = raw/400 = %.3f MiB   => %s (placement floor %.3f + vocab %.3f = %.3f MiB)\n",
           double(raw) / 400.0 / (1024.0 * 1024.0),
           double(dict_ctx) <= double(raw) / 400.0 ? "UNDER budget (400x cleared)" : "OVER budget (400x NOT cleared)",
           MB(plc_best), MB(skeldict + idz + tokdict), MB(dict_ctx));
    printf("    baseline z3-plain=%.3f  front-code=%.3f MiB\n", MB(base_z3), MB(front_z3));
}

// ---------------- one codec configuration ----------------
struct Metrics {
    uint64_t seg_bytes = 0, seg_z1 = 0, seg_z3 = 0;
    uint64_t split_lit_z3 = 0, split_ctrl_z3 = 0, split_off_z3 = 0;
    uint64_t split_lit_raw = 0, split_ctrl_raw = 0, split_off_raw = 0;
    double enc_gbs = 0, dec_gbs = 0;
    bool roundtrip_ok = false;
    uint64_t store_bytes = 0;
};

static Metrics run_config(const Distinct& d, const DefCodec::Params& P, ZSTD_CCtx* cc) {
    Metrics m;
    const size_t N = d.off.size();
    const uint64_t dbytes = d.blob.size();

    // ---- encode (concatenated segs) : timed ----
    std::vector<uint8_t> seg;
    std::vector<uint8_t> segblob; segblob.reserve(dbytes / 2 + 64);
    std::vector<uint32_t> seg_off; seg_off.reserve(N + 1); seg_off.push_back(0);
    DefCodec enc(P);
    Clock::time_point t0 = Clock::now();
    for (size_t i = 0; i < N; ++i) {
        enc.encode(d.blob.data() + d.off[i], d.len[i], seg);
        segblob.insert(segblob.end(), seg.begin(), seg.end());
        seg_off.push_back(uint32_t(segblob.size()));
    }
    double enc_t = secs(t0);
    m.seg_bytes = segblob.size();
    m.store_bytes = enc.store_bytes();
    m.enc_gbs = double(dbytes) / enc_t / 1e9;

    // ---- decode round-trip : timed (fresh decoder, identical sequence) ----
    DefCodec dec(P);
    std::vector<uint8_t> out; bool ok = true;
    t0 = Clock::now();
    for (size_t i = 0; i < N; ++i) {
        if (!dec.decode(segblob.data() + seg_off[i], seg_off[i + 1] - seg_off[i], out)) { ok = false; break; }
        if (out.size() != d.len[i] || memcmp(out.data(), d.blob.data() + d.off[i], d.len[i]) != 0) { ok = false; break; }
    }
    double dec_t = secs(t0);
    m.roundtrip_ok = ok;
    m.dec_gbs = ok ? double(dbytes) / dec_t / 1e9 : 0.0;

    // ---- z-compress the seg blob ----
    std::vector<uint8_t> z;
    m.seg_z1 = zstd_size(cc, segblob.data(), segblob.size(), 1, z);
    m.seg_z3 = zstd_size(cc, segblob.data(), segblob.size(), 3, z);

    // ---- split-stream probe (experimental; ratio only) ----
    std::vector<uint8_t> lit, ctrl, off;
    lit.reserve(dbytes / 2 + 64); ctrl.reserve(N * 2 + 64); off.reserve(N * 2 + 64);
    DefCodec sp(P);
    for (size_t i = 0; i < N; ++i) sp.encode_split(d.blob.data() + d.off[i], d.len[i], lit, ctrl, off);
    m.split_lit_raw = lit.size(); m.split_ctrl_raw = ctrl.size(); m.split_off_raw = off.size();
    m.split_lit_z3 = zstd_size(cc, lit.data(), lit.size(), 3, z);
    m.split_ctrl_z3 = zstd_size(cc, ctrl.data(), ctrl.size(), 3, z);
    m.split_off_z3 = zstd_size(cc, off.data(), off.size(), 3, z);
    return m;
}

// FrontCodec (recommended offset-free dict codec): encode/decode, round-trip, throughput, z3.
static bool run_frontcodec(const Distinct& d, uint64_t base_z3, uint64_t raw, ZSTD_CCtx* cc) {
    const size_t N = d.off.size();
    const uint64_t dbytes = d.blob.size();

    // audit: the self-delimiting '\n' claim (no length side-stream)
    size_t no_nl = 0;
    for (size_t i = 0; i < N; ++i)
        if (d.len[i] == 0 || d.blob[d.off[i] + d.len[i] - 1] != '\n') ++no_nl;

    Clock::time_point t0 = Clock::now();
    FrontCodec::Encoded e = FrontCodec::encode_set(d.blob.data(), d.off.data(), d.len.data(), N);
    double enc_t = secs(t0);

    std::vector<uint8_t> ob; std::vector<uint32_t> oo, ol;
    t0 = Clock::now();
    bool ok = FrontCodec::decode_set(e.lcp.data(), e.lcp.size(), e.suf.data(), e.suf.size(), N, ob, oo, ol);
    double dec_t = secs(t0);
    if (ok) {
        for (size_t k = 0; k < N; ++k) {                      // decoded[k] must equal k-th sorted line
            uint32_t r = e.rank[k];
            if (ol[k] != d.len[r] || memcmp(ob.data() + oo[k], d.blob.data() + d.off[r], d.len[r]) != 0) { ok = false; break; }
        }
    }

    std::vector<uint8_t> z;
    uint64_t lcp_z3 = zstd_size(cc, e.lcp.data(), e.lcp.size(), 3, z);
    uint64_t suf_z3 = zstd_size(cc, e.suf.data(), e.suf.size(), 3, z);
    uint64_t tot_z3 = lcp_z3 + suf_z3;

    printf("  ---- FrontCodec (RECOMMENDED: sort->rank-id + front-code, offset-free) ----\n");
    printf("    streams: lcp raw=%.3f suf raw=%.3f MiB   ->  z3 lcp=%.3f + suf=%.3f = %.3f MiB\n",
           MB(e.lcp.size()), MB(e.suf.size()), MB(lcp_z3), MB(suf_z3), MB(tot_z3));
    printf("    REDUCTION vs plain-z3 = %.3fx     dict-only ceiling ratio raw/z3: plain=%.1fx  front=%.1fx\n",
           double(base_z3) / double(tot_z3), double(raw) / double(base_z3), double(raw) / double(tot_z3));
    printf("    throughput:  encode=%.3f GB/s  decode=%.3f GB/s   roundtrip=%s   (lines without trailing '\\n': %zu)\n",
           dbytes / enc_t / 1e9, dbytes / dec_t / 1e9, ok ? "OK" : "*** FAIL ***", no_nl);
    return ok;
}

// Cross-project PRETRAINING probe: can an out-of-band vocabulary prior (skeletons + slot tokens)
// trained on OTHER repos cover enough of the target's vocab to push its dict under the 400x budget?
// The placement (which token per slot) and line->skeleton id are corpus-specific hard floors.
static void run_pretrain(const Distinct& d, uint64_t raw, const std::vector<std::string>& trains, ZSTD_CCtx* cc) {
    StringInterner pline, pskel, ptok;                  // 1) prior sets from training corpora
    for (auto& m : trains) {
        Corpus tc = load_corpus(m.c_str(), SIZE_MAX);
        Distinct td = extract_distinct(tc);
        for (size_t k = 0; k < td.off.size(); ++k) pline.intern(td.blob.data() + td.off[k], td.len[k]);
        tokenize_corpus(td, pskel, ptok, nullptr, nullptr, nullptr);
        fprintf(stderr, "  [prior] %s: distinct=%zu  cumulative prior lines=%zu skeletons=%zu tokens=%zu\n",
                m.c_str(), td.off.size(), pline.off.size(), pskel.off.size(), ptok.off.size());
    }
    StringInterner tskel, ttok;                         // 2) tokenize target
    std::vector<uint32_t> line_skel, slot_tok, line_slot_start;
    tokenize_corpus(d, tskel, ttok, &line_skel, &slot_tok, &line_slot_start);
    const size_t N = d.off.size();
    std::vector<uint8_t> z;
    auto Z3 = [&](const std::vector<uint8_t>& v) { return zstd_size(cc, v.data(), v.size(), 3, z); };

    // LINE-LEVEL coverage: DuckDB distinct lines byte-identical to a prior (toolchain) line ship as a
    // reference, not text. Covered lines are FREE if the reference set is bundled with the environment
    // (icecream ships it) and referenced via a shared id space; only the residual (project-specific)
    // lines' text ships. Also price a per-line mapping variant (flags + covered->ref-id) if needed.
    std::vector<uint8_t> residual_blob, map_s, flag_s;
    size_t line_cov = 0; uint64_t line_cov_b = 0, line_tot_b = 0;
    // classify by kind: linemarker (# ...), trivial (<=2 bytes / all-ws), substantive code
    size_t sub_n = 0, sub_cov = 0, lm_n = 0, triv_n = 0; uint64_t sub_b = 0, sub_cov_b = 0;
    for (size_t i = 0; i < N; ++i) {
        const uint8_t* p = d.blob.data() + d.off[i]; uint32_t L = d.len[i]; line_tot_b += L;
        uint32_t rid = pline.find(p, L);
        bool cov = rid != UINT32_MAX;
        if (cov) { ++line_cov; line_cov_b += L; flag_s.push_back(1); bput_varint(map_s, rid); }
        else { flag_s.push_back(0); residual_blob.insert(residual_blob.end(), p, p + L); }
        uint32_t ws = 0; while (ws < L && (p[ws] == ' ' || p[ws] == '\t' || p[ws] == '\n')) ++ws;
        if (L && p[0] == '#') ++lm_n;
        else if (ws >= L - (L && p[L - 1] == '\n' ? 1 : 0) || L <= 2) ++triv_n;
        else { ++sub_n; sub_b += L; if (cov) { ++sub_cov; sub_cov_b += L; } }
    }
    uint64_t base_z3 = Z3(d.blob);
    uint64_t res_z3 = Z3(residual_blob), flag_z3 = Z3(flag_s), map_z3 = Z3(map_s);
    uint64_t cold_strict = res_z3;                       // covered lines free (bundled, shared id space)
    uint64_t cold_withmap = res_z3 + flag_z3 + map_z3;   // if a per-line dict->ref mapping is charged
    double budget = double(raw) / 400.0;
    printf("  ---- TOOLCHAIN/PRIOR LINE-LEVEL coverage (prior distinct lines=%zu) ----\n", pline.off.size());
    printf("    target distinct lines=%zu  covered=%zu (%.1f%% by count, %.1f%% by BYTES)  residual=%.3f MiB raw\n",
           N, line_cov, 100.0 * double(line_cov) / double(N), 100.0 * double(line_cov_b) / double(line_tot_b),
           MB(residual_blob.size()));
    printf("    kinds: linemarkers=%zu  trivial=%zu  substantive-code=%zu (%.3f MiB); SUBSTANTIVE coverage=%.1f%% by count, %.1f%% by bytes\n",
           lm_n, triv_n, sub_n, MB(sub_b), 100.0 * double(sub_cov) / double(sub_n ? sub_n : 1),
           100.0 * double(sub_cov_b) / double(sub_b ? sub_b : 1));
    printf("    full dict z3 (no prior) = %.3f MiB = %.1fx\n", MB(base_z3), double(raw) / double(base_z3));
    printf("    cold dict, covered-FREE = residual z3 %.3f MiB = %.1fx   %s 400x (budget %.3f MiB)\n",
           MB(cold_strict), double(raw) / double(cold_strict),
           double(cold_strict) <= budget ? "CLEARS" : "under", MB(uint64_t(budget)));
    printf("    cold dict, +per-line map = %.3f MiB (res %.3f + flags %.3f + map %.3f) = %.1fx   %s 400x\n",
           MB(cold_withmap), MB(res_z3), MB(flag_z3), MB(map_z3), double(raw) / double(cold_withmap),
           double(cold_withmap) <= budget ? "CLEARS" : "under");

    // TRAINED ZSTD DICTIONARY (bigoracle 6/7): whole lines don't share cross-project, but SUBSTRINGS
    // (common tokens, punctuation, type fragments) might. Does a dict trained on the prior beat plain z3?
    {
        size_t np = pline.off.size(), stride = np > 200000 ? np / 200000 : 1;
        std::vector<uint8_t> samples; std::vector<size_t> sizes;
        for (size_t k = 0; k < np; k += stride) {
            samples.insert(samples.end(), pline.blob.data() + pline.off[k], pline.blob.data() + pline.off[k] + pline.len[k]);
            sizes.push_back(pline.len[k]);
        }
        std::vector<uint8_t> dict(512 * 1024);
        size_t ds = ZDICT_trainFromBuffer(dict.data(), dict.size(), samples.data(), sizes.data(), unsigned(sizes.size()));
        if (ZDICT_isError(ds)) printf("    trained-dict: ZDICT failed (%s)\n", ZDICT_getErrorName(ds));
        else {
            auto zdictsz = [&](const std::vector<uint8_t>& v) {
                ZSTD_CCtx_reset(cc, ZSTD_reset_session_and_parameters);
                ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, 3);
                ZSTD_CCtx_loadDictionary(cc, dict.data(), ds);
                size_t bound = ZSTD_compressBound(v.size()); if (z.size() < bound) z.resize(bound);
                size_t r = ZSTD_compress2(cc, z.data(), z.size(), v.data(), v.size());
                return ZSTD_isError(r) ? uint64_t(0) : uint64_t(r);
            };
            uint64_t full_d = zdictsz(d.blob), res_d = zdictsz(residual_blob);
            printf("    TRAINED-DICT (%zuKB, %zu samples): full dict z3+dict=%.3f MiB=%.1fx (vs plain z3 %.3f=%.1fx); residual z3+dict=%.3f MiB=%.1fx  %s 400x\n",
                   ds / 1024, sizes.size(), MB(full_d), double(raw) / double(full_d), MB(base_z3), double(raw) / double(base_z3),
                   MB(res_d), double(raw) / double(res_d), double(res_d) <= budget ? "CLEARS" : "under");
        }
    }

    // 3) vocab coverage by the prior
    std::vector<uint8_t> inc_skel_blob, inc_skel_len, inc_tok_blob, inc_tok_len;
    size_t skel_cov = 0; uint64_t skel_cov_b = 0, skel_tot_b = 0;
    for (size_t s = 0; s < tskel.off.size(); ++s) {
        const uint8_t* p = tskel.blob.data() + tskel.off[s]; uint32_t L = tskel.len[s]; skel_tot_b += L;
        if (pskel.contains(p, L)) { ++skel_cov; skel_cov_b += L; }
        else { inc_skel_blob.insert(inc_skel_blob.end(), p, p + L); bput_varint(inc_skel_len, L); }
    }
    size_t tok_cov = 0; uint64_t tok_cov_b = 0, tok_tot_b = 0;
    for (size_t t = 0; t < ttok.off.size(); ++t) {
        const uint8_t* p = ttok.blob.data() + ttok.off[t]; uint32_t L = ttok.len[t]; tok_tot_b += L;
        if (ptok.contains(p, L)) { ++tok_cov; tok_cov_b += L; }
        else { inc_tok_blob.insert(inc_tok_blob.end(), p, p + L); bput_varint(inc_tok_len, L); }
    }

    // 4) target streams + costs (skeleton/slot-token VOCAB-level coverage, for comparison)
    std::vector<uint8_t> id_s, tid_row, full_skel_len, full_tok_len;
    for (size_t i = 0; i < N; ++i) bput_varint(id_s, line_skel[i]);
    for (size_t i = 0; i < N; ++i)
        for (uint32_t si = line_slot_start[i]; si < line_slot_start[i + 1]; ++si) bput_varint(tid_row, slot_tok[si]);
    for (size_t s = 0; s < tskel.off.size(); ++s) bput_varint(full_skel_len, tskel.len[s]);
    for (size_t t = 0; t < ttok.off.size(); ++t) bput_varint(full_tok_len, ttok.len[t]);
    uint64_t id_z3 = Z3(id_s);
    uint64_t plc_z19 = zstd_size_adv(cc, tid_row.data(), tid_row.size(), 19, 1, 27, z);
    uint64_t full_skel_z3 = Z3(tskel.blob) + Z3(full_skel_len);
    uint64_t full_tok_z3 = Z3(ttok.blob) + Z3(full_tok_len);
    uint64_t inc_skel_z3 = Z3(inc_skel_blob) + Z3(inc_skel_len);
    uint64_t inc_tok_z3 = Z3(inc_tok_blob) + Z3(inc_tok_len);

    uint64_t floor_hard = plc_z19 + id_z3;                                   // corpus-specific, un-pretrainable
    uint64_t dict_noprior = floor_hard + full_skel_z3 + full_tok_z3;
    uint64_t dict_prior = floor_hard + inc_skel_z3 + inc_tok_z3;

    printf("  ---- VOCAB-level coverage (skeleton + slot-token text) ----\n");
    printf("    prior vocab: skeletons=%zu tokens=%zu\n", pskel.off.size(), ptok.off.size());
    printf("    target skeletons=%zu covered=%zu (%.1f%% by count, %.1f%% by bytes)  incremental z3=%.3f MiB (full %.3f)\n",
           tskel.off.size(), skel_cov, 100.0 * double(skel_cov) / double(tskel.off.size()),
           100.0 * double(skel_cov_b) / double(skel_tot_b), MB(inc_skel_z3), MB(full_skel_z3));
    printf("    target tokens=%zu covered=%zu (%.1f%% by count, %.1f%% by bytes)  incremental z3=%.3f MiB (full %.3f)\n",
           ttok.off.size(), tok_cov, 100.0 * double(tok_cov) / double(ttok.off.size()),
           100.0 * double(tok_cov_b) / double(tok_tot_b), MB(inc_tok_z3), MB(full_tok_z3));
    printf("    hard floor (placement z19 %.3f + line->skel-id %.3f) = %.3f MiB = %.1fx (un-pretrainable)\n",
           MB(plc_z19), MB(id_z3), MB(floor_hard), double(raw) / double(floor_hard));
    printf("    dict WITHOUT prior = %.3f MiB = %.1fx\n", MB(dict_noprior), double(raw) / double(dict_noprior));
    printf("    dict WITH    prior = %.3f MiB = %.1fx   (incremental vocab %.3f MiB)\n",
           MB(dict_prior), double(raw) / double(dict_prior), MB(inc_skel_z3 + inc_tok_z3));
    printf("    400x budget = %.3f MiB  =>  %s\n", MB(uint64_t(budget)),
           double(dict_prior) <= budget ? "*** WITH-PRIOR dict CLEARS 400x ***" : "with-prior dict still OVER 400x");
}

// LEAVE-ONE-OUT cross-project study (task #21): for each corpus, how much of its distinct-line /
// skeleton / slot-token vocab is covered by a prior of the OTHER corpora? Builds one combined interner
// per stream with a per-entry corpus bitmask, so coverage + learning curves are bitmask queries.
static void run_loo(const char* listfile) {
    std::vector<std::pair<std::string, std::string>> corp;   // (manifest, name)
    { FILE* f = fopen(listfile, "r"); if (!f) { perror(listfile); exit(2); }
      char line[8192];
      while (fgets(line, sizeof line, f)) { std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty()) continue;
        size_t sp = s.find(' '); if (sp == std::string::npos) sp = s.find('\t');
        if (sp == std::string::npos) corp.push_back({s, s});
        else corp.push_back({s.substr(0, sp), s.substr(s.find_first_not_of(" \t", sp))}); }
      fclose(f); }
    const size_t NC = corp.size();
    ZSTD_CCtx* cc = ZSTD_createCCtx(); std::vector<uint8_t> z;

    StringInterner L, S, Tk; std::vector<uint32_t> Lm, Sm, Tm;   // masks indexed by interner id
    struct CS { std::string name; size_t tus, dlines, dskel, dtok; uint64_t raw, lbytes, dict_z3, toolfree_b, proj_z3; };
    std::vector<CS> cs(NC);
    auto add = [](StringInterner& in, std::vector<uint32_t>& m, const uint8_t* p, uint32_t n, uint32_t bit) {
        uint32_t id = in.intern(p, n); if (id >= m.size()) m.resize(id + 1, 0); m[id] |= bit;
    };
    for (size_t c = 0; c < NC; ++c) {
        uint32_t bit = 1u << c;
        Corpus co = load_corpus(corp[c].first.c_str(), SIZE_MAX);
        Distinct d = extract_distinct(co);
        uint64_t dz = zstd_size(cc, d.blob.data(), d.blob.size(), 3, z);
        for (size_t k = 0; k < d.off.size(); ++k) add(L, Lm, d.blob.data() + d.off[k], d.len[k], bit);
        StringInterner ts, tt; tokenize_corpus(d, ts, tt, nullptr, nullptr, nullptr);
        for (size_t k = 0; k < ts.off.size(); ++k) add(S, Sm, ts.blob.data() + ts.off[k], ts.len[k], bit);
        for (size_t k = 0; k < tt.off.size(); ++k) add(Tk, Tm, tt.blob.data() + tt.off[k], tt.len[k], bit);
        // source-attribution: first-seen content lines (markers excluded=structure) by marker path class
        StringInterner seen; std::vector<uint8_t> proj_blob; uint64_t toolfree_b = 0;
        for (auto& fsp : co.files) {
            const uint8_t* p = co.bytes.data() + fsp.off; const uint8_t* e = p + fsp.len; bool cur_tool = false;
            while (p < e) {
                const uint8_t* nl = (const uint8_t*)memchr(p, '\n', size_t(e - p)); const uint8_t* le = nl ? nl + 1 : e;
                if (le - p > 2 && p[0] == '#' && p[1] == ' ') {
                    const uint8_t* q = p + 2; while (q < le && *q >= '0' && *q <= '9') ++q;
                    if (q < le && *q == ' ') { ++q; if (q < le && *q == '"') { ++q; const uint8_t* s = q;
                        while (q < le && *q != '"') ++q;
                        if (q > s) cur_tool = (q - s > 4 && (memcmp(s, "/usr", 4) == 0 || memcmp(s, "/lib", 4) == 0)); } }
                } else { uint32_t Ln = uint32_t(le - p);
                    if (!seen.contains(p, Ln)) { seen.intern(p, Ln);
                        if (cur_tool) toolfree_b += Ln; else proj_blob.insert(proj_blob.end(), p, le); } }
                p = le;
            }
        }
        uint64_t proj_z3 = zstd_size(cc, proj_blob.data(), proj_blob.size(), 3, z);
        cs[c] = {corp[c].second, co.files.size(), d.off.size(), ts.off.size(), tt.off.size(), co.raw, d.blob.size(), dz, toolfree_b, proj_z3};
        fprintf(stderr, "  [loo] %zu/%zu %s: TUs=%zu dlines=%zu dict_z3=%.2f toolfree=%.2f proj_z3=%.2f MiB\n",
                c + 1, NC, cs[c].name.c_str(), cs[c].tus, cs[c].dlines, MB(dz), MB(toolfree_b), MB(proj_z3));
    }

    // covered bytes/count of target T's entries by the union of all OTHER corpora
    auto cover = [](const StringInterner& in, const std::vector<uint32_t>& m, uint32_t T,
                    uint64_t& tot_b, uint64_t& cov_b, size_t& tot_n, size_t& cov_n) {
        uint32_t self = 1u << T; tot_b = cov_b = 0; tot_n = cov_n = 0;
        for (size_t id = 0; id < in.off.size(); ++id) if (m[id] & self) {
            ++tot_n; tot_b += in.len[id];
            if (m[id] & ~self) { ++cov_n; cov_b += in.len[id]; }
        }
    };
    auto isApp = [](const std::string& n) {
        return n == "llvm" || n == "rocksdb" || n == "duckdb" || n == "abseil-protobuf" || n == "opencv"; };
    printf("ATTR\tcorpus\tkind\ttus\traw_MiB\tdict_z3_MiB\tdict_ratio\ttoolchain_free_MiB\tproject_ship_z3_MiB\tsrccond_ratio\tline_cov_pctB\tline_cov_pctN\tskel_cov_pctB\ttok_cov_pctB\n");
    for (size_t T = 0; T < NC; ++T) {
        uint64_t ltb, lcb, stb, scb, ttb, tcb; size_t ltn, lcn, stn, scn, ttn, tcn;
        cover(L, Lm, uint32_t(T), ltb, lcb, ltn, lcn);
        cover(S, Sm, uint32_t(T), stb, scb, stn, scn);
        cover(Tk, Tm, uint32_t(T), ttb, tcb, ttn, tcn);
        double rawM = MB(cs[T].raw), dz3 = MB(cs[T].dict_z3), pz3 = MB(cs[T].proj_z3);
        printf("ATTR\t%s\t%s\t%zu\t%.2f\t%.3f\t%.1f\t%.3f\t%.3f\t%.1f\t%.2f\t%.2f\t%.2f\t%.2f\n",
               cs[T].name.c_str(), isApp(cs[T].name) ? "app" : "lib", cs[T].tus, rawM, dz3, dz3 > 0 ? rawM / dz3 : 0,
               MB(cs[T].toolfree_b), pz3, pz3 > 0 ? rawM / pz3 : 0,
               100.0 * double(lcb) / double(ltb ? ltb : 1), 100.0 * double(lcn) / double(ltn ? ltn : 1),
               100.0 * double(scb) / double(stb ? stb : 1), 100.0 * double(tcb) / double(ttb ? ttb : 1));
    }

    // LINE-coverage learning curve: prior grows by adding OTHER corpora in descending distinct_lines
    // order; coverage(k) = target bytes covered once the first k prior projects are included.
    printf("CURVE\tcorpus\tk_projects\tline_cov_pctB\n");
    for (size_t T = 0; T < NC; ++T) {
        std::vector<size_t> order;
        for (size_t c = 0; c < NC; ++c) if (c != T) order.push_back(c);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cs[a].dlines > cs[b].dlines; });
        std::vector<uint32_t> rank(NC, 0xFFFFFFFF);
        for (size_t r = 0; r < order.size(); ++r) rank[order[r]] = uint32_t(r);
        uint32_t self = 1u << T;
        std::vector<uint64_t> by_thr(order.size() + 1, 0); uint64_t tot = 0;
        for (size_t id = 0; id < L.off.size(); ++id) if (Lm[id] & self) {
            tot += L.len[id];
            uint32_t best = 0xFFFFFFFF, mm = Lm[id] & ~self;
            while (mm) { uint32_t c = uint32_t(__builtin_ctz(mm)); mm &= mm - 1; if (rank[c] < best) best = rank[c]; }
            if (best != 0xFFFFFFFF) by_thr[best] += L.len[id];
        }
        uint64_t cum = 0;
        for (size_t k = 0; k < order.size(); ++k) { cum += by_thr[k];
            printf("CURVE\t%s\t%zu\t%.2f\n", cs[T].name.c_str(), k + 1, 100.0 * double(cum) / double(tot ? tot : 1)); }
    }
    ZSTD_freeCCtx(cc);
}

// SOURCE-CONDITIONED REGION coverage (bigoracle §11): a preprocessed .ii is mostly VERBATIM its own
// source files (the preprocessor expands #include/#define/#if but does NOT instantiate templates).
// The `# N "path"` markers name the source. If the worker has the source (toolchain headers bundled;
// project source shipped ONCE, content-addressed), a distinct .ii line that is byte-verbatim a source
// line costs ~a copy reference, not its text. Residual = macro-expanded / generated lines -> z3.
// This is DIFFERENT from cross-corpus line pretraining: we match against each line's OWN source tree.
static void run_regioncov(const Corpus& c, const Distinct& d, uint64_t raw, ZSTD_CCtx* cc) {
    // 1) collect the distinct source paths named by the .ii markers
    StringInterner paths;
    for (auto& fsp : c.files) {
        const uint8_t* p = c.bytes.data() + fsp.off; const uint8_t* e = p + fsp.len;
        while (p < e) {
            const uint8_t* nl = (const uint8_t*)memchr(p, '\n', size_t(e - p)); const uint8_t* le = nl ? nl + 1 : e;
            if (le - p > 5 && p[0] == '#' && p[1] == ' ') {
                const uint8_t* q = p + 2; while (q < le && *q >= '0' && *q <= '9') ++q;
                if (q < le && *q == ' ') { ++q; if (q < le && *q == '"') { ++q; const uint8_t* s = q;
                    while (q < le && *q != '"') ++q;
                    if (q < le && *q == '"' && q > s && *s != '<') paths.intern(s, uint32_t(q - s)); } }
            }
            p = le;
        }
    }
    // 2) load each source file once; build verbatim-line sets (toolchain vs project)
    StringInterner tool_lines, proj_lines;
    std::vector<uint8_t> proj_src; uint64_t proj_raw = 0, tool_files = 0, proj_files = 0, miss = 0;
    std::vector<uint8_t> buf;
    for (size_t i = 0; i < paths.off.size(); ++i) {
        std::string path((const char*)paths.blob.data() + paths.off[i], paths.len[i]);
        bool tool = path.rfind("/usr", 0) == 0 || path.rfind("/lib", 0) == 0;
        FILE* f = fopen(path.c_str(), "rb"); if (!f) { ++miss; continue; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        buf.clear(); if (sz > 0) { buf.resize(size_t(sz)); if (fread(buf.data(), 1, size_t(sz), f) != size_t(sz)) { fclose(f); ++miss; continue; } }
        fclose(f);
        StringInterner& set = tool ? tool_lines : proj_lines;
        const uint8_t* p = buf.data(); const uint8_t* e = p + buf.size();
        while (p < e) { const uint8_t* nl = (const uint8_t*)memchr(p, '\n', size_t(e - p)); const uint8_t* le = nl ? nl + 1 : e; set.intern(p, uint32_t(le - p)); p = le; }
        if (tool) ++tool_files; else { ++proj_files; proj_raw += buf.size(); proj_src.insert(proj_src.end(), buf.begin(), buf.end()); }
    }
    // 3) classify each distinct .ii dict line
    std::vector<uint8_t> residual;
    uint64_t tb = 0, pb = 0, rb = 0; size_t tc_ = 0, pc_ = 0, rc_ = 0;
    for (size_t i = 0; i < d.off.size(); ++i) {
        const uint8_t* p = d.blob.data() + d.off[i]; uint32_t L = d.len[i];
        if (tool_lines.contains(p, L)) { tb += L; ++tc_; }
        else if (proj_lines.contains(p, L)) { pb += L; ++pc_; }
        else { rb += L; ++rc_; residual.insert(residual.end(), p, p + L); }
    }
    // 4) costs
    std::vector<uint8_t> z;
    uint64_t base_z3 = zstd_size(cc, d.blob.data(), d.blob.size(), 3, z);
    uint64_t res_z3 = zstd_size(cc, residual.data(), residual.size(), 3, z);
    uint64_t projsrc_z3 = zstd_size(cc, proj_src.data(), proj_src.size(), 3, z);
    uint64_t fill = res_z3 + projsrc_z3;               // toolchain copies free; residual + project-src-once
    double budget = double(raw) / 400.0, dtot = double(d.blob.size());

    printf("  ---- SOURCE-CONDITIONED REGION coverage (bigoracle 11) ----\n");
    printf("    marker source paths=%zu  loaded: toolchain files=%llu, project files=%llu, missing=%llu  project-src raw=%.1f MiB\n",
           paths.off.size(), (unsigned long long)tool_files, (unsigned long long)proj_files, (unsigned long long)miss, MB(proj_raw));
    printf("    distinct dict lines=%zu (%.3f MiB): toolchain-verbatim=%zu (%.1f%% bytes) project-verbatim=%zu (%.1f%% bytes) residual=%zu (%.1f%% bytes)\n",
           d.off.size(), MB(d.blob.size()), tc_, 100.0 * double(tb) / dtot, pc_, 100.0 * double(pb) / dtot, rc_, 100.0 * double(rb) / dtot);
    printf("    residual raw=%.3f MiB -> z3=%.3f;  project-src z3 (ship once)=%.3f MiB\n", MB(rb), MB(res_z3), MB(projsrc_z3));
    printf("    FILL (toolchain free) = residual z3 %.3f + project-src z3 %.3f = %.3f MiB = %.1fx   (base dict z3 %.3f = %.1fx)\n",
           MB(res_z3), MB(projsrc_z3), MB(fill), double(raw) / double(fill), MB(base_z3), double(raw) / double(base_z3));
    printf("    go/no-go: <=3.5MB REACHABLE, >4.15MB escalate. budget(400x)=%.3f MiB => %s   (residual-only, toolchain+project free = %.3f MiB = %.1fx)\n",
           MB(uint64_t(budget)), double(fill) <= budget ? "*** CLEARS 400x ***" : (double(fill) <= 4.15 * 1024 * 1024 ? "fragile zone" : "over"),
           MB(res_z3), double(raw) / double(res_z3));

    // SOURCE-ATTRIBUTION of FIRST-SEEN content lines (markers excluded = structure). This is the
    // CEILING for source-conditioning: even with perfect COPY+PATCH, only TOOLCHAIN-attributed lines
    // are free (worker has the toolchain source); PROJECT-attributed lines must ship (worker lacks the
    // project source, and shipping it costs more than the lines). FILL floor = z3(project-attributed).
    StringInterner seen;
    std::vector<uint8_t> tool_fs, proj_fs; uint64_t tfb = 0, pfb = 0; size_t tfn = 0, pfn = 0;
    for (auto& fsp : c.files) {
        const uint8_t* p = c.bytes.data() + fsp.off; const uint8_t* e = p + fsp.len; bool cur_tool = false;
        while (p < e) {
            const uint8_t* nl = (const uint8_t*)memchr(p, '\n', size_t(e - p)); const uint8_t* le = nl ? nl + 1 : e;
            if (le - p > 2 && p[0] == '#' && p[1] == ' ') {                    // marker: update class
                const uint8_t* q = p + 2; while (q < le && *q >= '0' && *q <= '9') ++q;
                if (q < le && *q == ' ') { ++q; if (q < le && *q == '"') { ++q; const uint8_t* s = q;
                    while (q < le && *q != '"') ++q;
                    if (q > s) cur_tool = (q - s > 4 && (memcmp(s, "/usr", 4) == 0 || memcmp(s, "/lib", 4) == 0)); } }
            } else {                                                          // content: first-seen -> attribute
                uint32_t L = uint32_t(le - p);
                if (!seen.contains(p, L)) { seen.intern(p, L);
                    if (cur_tool) { tool_fs.insert(tool_fs.end(), p, le); tfb += L; ++tfn; }
                    else { proj_fs.insert(proj_fs.end(), p, le); pfb += L; ++pfn; } }
            }
            p = le;
        }
    }
    uint64_t tool_fs_z3 = zstd_size(cc, tool_fs.data(), tool_fs.size(), 3, z);
    uint64_t proj_fs_z3 = zstd_size(cc, proj_fs.data(), proj_fs.size(), 3, z);
    printf("    --- source-attribution of first-seen content (markers excluded) ---\n");
    printf("    toolchain-attributed=%zu lines %.3f MiB -> z3 %.3f (FREE, worker has toolchain src)\n", tfn, MB(tfb), MB(tool_fs_z3));
    printf("    project-attributed  =%zu lines %.3f MiB -> z3 %.3f (must ship; worker lacks project src)\n", pfn, MB(pfb), MB(proj_fs_z3));
    printf("    BEST-CASE FILL (toolchain fully free via COPY+PATCH) = project z3 %.3f MiB = %.1fx   %s 400x (budget %.3f)\n",
           MB(proj_fs_z3), double(raw) / double(proj_fs_z3),
           double(proj_fs_z3) <= budget ? "*** CLEARS ***" : "under", MB(uint64_t(budget)));
}

int main(int argc, char** argv) {
    const char* manifest = nullptr;
    const char* name = nullptr;
    size_t max_files = SIZE_MAX;
    bool sweep = false;
    bool ceiling = false;
    bool reorder = false;
    bool skeleton = false;
    bool pretrain = false;
    bool regioncov = false;
    const char* loofile = nullptr;
    std::vector<std::string> trains;
    DefCodec::Params P;
    for (int i = 1; i < argc; ++i) {
        auto eat = [&](const char* f) { return !strcmp(argv[i], f) && i + 1 < argc; };
        if (eat("--manifest")) manifest = argv[++i];
        else if (eat("--name")) name = argv[++i];
        else if (eat("--max-files")) max_files = strtoull(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--sweep")) sweep = true;
        else if (!strcmp(argv[i], "--ceiling")) ceiling = true;
        else if (!strcmp(argv[i], "--reorder")) reorder = true;
        else if (!strcmp(argv[i], "--skeleton")) skeleton = true;
        else if (!strcmp(argv[i], "--pretrain")) pretrain = true;
        else if (!strcmp(argv[i], "--regioncov")) regioncov = true;
        else if (eat("--loo")) loofile = argv[++i];
        else if (eat("--train")) trains.emplace_back(argv[++i]);
        else if (eat("--stride")) P.stride = uint32_t(atoi(argv[++i]));
        else if (eat("--min-match")) P.min_match = uint32_t(atoi(argv[++i]));
        else if (eat("--table-bits")) P.table_bits = uint32_t(atoi(argv[++i]));
        else if (eat("--chain")) P.chain = uint32_t(atoi(argv[++i]));
        else if (eat("--skip-log")) P.skip_log = uint32_t(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-cross-rep")) P.cross_line_rep = false;
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
    if (loofile) { run_loo(loofile); return 0; }   // leave-one-out study loads its own corpora
    if (!manifest) {
        fprintf(stderr, "usage: %s --manifest F [--name N] [--sweep] "
                        "[--stride N] [--min-match N] [--table-bits N] [--chain N] [--no-cross-rep] [--max-files N]\n", argv[0]);
        return 2;
    }
    if (!name) name = manifest;

    Clock::time_point t0 = Clock::now();
    Corpus c = load_corpus(manifest, max_files);
    double load_t = secs(t0);
    t0 = Clock::now();
    Distinct d = extract_distinct(c);
    double ext_t = secs(t0);
    const uint64_t dbytes = d.blob.size();

    ZSTD_CCtx* cc = ZSTD_createCCtx();
    std::vector<uint8_t> z;
    uint64_t base_z1 = zstd_size(cc, d.blob.data(), dbytes, 1, z);
    uint64_t base_z3 = zstd_size(cc, d.blob.data(), dbytes, 3, z);

    printf("================================================================\n");
    printf("corpus: %s  (%s)\n", name, manifest);
    printf("  TUs=%zu  raw=%.1f MiB  total_lines=%llu  distinct_lines=%zu  distinct_bytes=%.3f MiB\n",
           c.files.size(), MB(c.raw), (unsigned long long)d.total_lines, d.off.size(), MB(dbytes));
    printf("  load=%.2fs  extract=%.2fs\n", load_t, ext_t);
    printf("  [baseline] distinct blob -> none=%.3f MiB  z1=%.3f MiB  z3=%.3f MiB   (raw/z3 = %.1fx)\n",
           MB(dbytes), MB(base_z1), MB(base_z3), double(dbytes) / double(base_z3));

    if (ceiling) {
        // Compressibility ceiling of the SAME distinct blob: does long-distance matching or a
        // bigger window beat plain z3? If not, no explicit relative-LZ codec can help.
        struct Cfg { const char* tag; int lvl, ldm, wlog; };
        Cfg cfgs[] = {
            {"z1          ", 1, 0, 0}, {"z3          ", 3, 0, 0},
            {"z3+ldm+w27  ", 3, 1, 27}, {"z6          ", 6, 0, 0},
            {"z9          ", 9, 0, 0}, {"z12+ldm+w27 ", 12, 1, 27},
            {"z19         ", 19, 0, 0}, {"z19+ldm+w27 ", 19, 1, 27},
        };
        printf("  ---- compressibility ceiling of the distinct blob (%.3f MiB) ----\n", MB(dbytes));
        for (auto& g : cfgs) {
            Clock::time_point tt = Clock::now();
            uint64_t s = zstd_size_adv(cc, d.blob.data(), dbytes, g.lvl, g.ldm, g.wlog, z);
            double dt = secs(tt);
            printf("    %s -> %.3f MiB  (dict raw/comp=%.2fx  raw/comp=%.1fx  %.3f GB/s)\n",
                   g.tag, MB(s), double(dbytes) / double(s), double(c.raw) / double(s),
                   double(dbytes) / dt / 1e9);
        }
        ZSTD_freeCCtx(cc);
        return 0;
    }

    if (regioncov) {
        run_regioncov(c, d, c.raw, cc);
        ZSTD_freeCCtx(cc);
        return 0;
    }

    if (pretrain) {
        if (trains.empty()) { fprintf(stderr, "--pretrain needs at least one --train MANIFEST\n"); return 2; }
        run_pretrain(d, c.raw, trains, cc);
        ZSTD_freeCCtx(cc);
        return 0;
    }

    if (skeleton) {
        FrontCodec::Encoded e = FrontCodec::encode_set(d.blob.data(), d.off.data(), d.len.data(), d.off.size());
        std::vector<uint8_t> zz;
        uint64_t front_z3 = zstd_size(cc, e.lcp.data(), e.lcp.size(), 3, zz)
                          + zstd_size(cc, e.suf.data(), e.suf.size(), 3, zz);
        run_skeleton(d, base_z3, front_z3, c.raw, cc);
        ZSTD_freeCCtx(cc);
        return 0;
    }

    if (reorder) {
        // Offset-free structural alternative to relative-LZ: treat the dict as a SET of strings,
        // assign canonical line-id = sorted rank (no permutation to ship -- ids are our choice),
        // and front-code (store each sorted line as LCP-with-previous + suffix). This is the
        // classic compressed-string-dictionary transform; unlike LZ it adds no offset stream.
        const size_t N = d.off.size();
        std::vector<uint32_t> idx(N);
        for (size_t i = 0; i < N; ++i) idx[i] = uint32_t(i);
        const uint8_t* B = d.blob.data();
        Clock::time_point ts = Clock::now();
        std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) {
            uint32_t la = d.len[a], lb = d.len[b], m = la < lb ? la : lb;
            int c = memcmp(B + d.off[a], B + d.off[b], m);
            return c != 0 ? c < 0 : la < lb;
        });
        double sort_t = secs(ts);

        std::vector<uint8_t> sorted; sorted.reserve(dbytes);
        std::vector<uint8_t> lcp_s, suf_s; lcp_s.reserve(N * 2); suf_s.reserve(dbytes);
        const uint8_t* prev = nullptr; uint32_t prev_len = 0;
        for (size_t i = 0; i < N; ++i) {
            const uint8_t* p = B + d.off[idx[i]]; uint32_t L = d.len[idx[i]];
            sorted.insert(sorted.end(), p, p + L);
            uint32_t lcp = 0, mm = prev_len < L ? prev_len : L;
            while (lcp < mm && prev[lcp] == p[lcp]) ++lcp;
            // varint LCP
            uint64_t v = lcp; while (v >= 0x80) { lcp_s.push_back(uint8_t(v) | 0x80); v >>= 7; } lcp_s.push_back(uint8_t(v));
            suf_s.insert(suf_s.end(), p + lcp, p + L);
            prev = p; prev_len = L;
        }

        printf("  ---- offset-free reorder transform (sort=%.2fs) ----\n", sort_t);
        uint64_t sorted_z3  = zstd_size_adv(cc, sorted.data(), sorted.size(), 3, 0, 0, z);
        uint64_t sorted_z19 = zstd_size_adv(cc, sorted.data(), sorted.size(), 19, 1, 27, z);
        uint64_t lcp_z3 = zstd_size_adv(cc, lcp_s.data(), lcp_s.size(), 3, 0, 0, z);
        uint64_t suf_z3 = zstd_size_adv(cc, suf_s.data(), suf_s.size(), 3, 0, 0, z);
        uint64_t suf_z19 = zstd_size_adv(cc, suf_s.data(), suf_s.size(), 19, 1, 27, z);
        uint64_t lcp_z19 = zstd_size_adv(cc, lcp_s.data(), lcp_s.size(), 19, 0, 0, z);
        uint64_t fc_z3  = lcp_z3 + suf_z3;
        uint64_t fc_z19 = lcp_z19 + suf_z19;
        printf("    baseline (first-appearance order) z3 = %.3f MiB   (raw/comp %.1fx)\n",
               MB(base_z3), double(c.raw) / double(base_z3));
        printf("    sorted blob            z3 = %.3f MiB (%.3fx vs base)   raw/comp %.1fx\n",
               MB(sorted_z3), double(base_z3) / double(sorted_z3), double(c.raw) / double(sorted_z3));
        printf("    sorted blob      z19+ldm = %.3f MiB (%.3fx vs base)   raw/comp %.1fx\n",
               MB(sorted_z19), double(base_z3) / double(sorted_z19), double(c.raw) / double(sorted_z19));
        printf("    front-code (lcp+suf)   z3 = %.3f MiB (lcp %.3f + suf %.3f) (%.3fx vs base)  raw/comp %.1fx\n",
               MB(fc_z3), MB(lcp_z3), MB(suf_z3), double(base_z3) / double(fc_z3), double(c.raw) / double(fc_z3));
        printf("    front-code (lcp+suf) z19+ = %.3f MiB (lcp %.3f + suf %.3f) (%.3fx vs base)  raw/comp %.1fx\n",
               MB(fc_z19), MB(lcp_z19), MB(suf_z19), double(base_z3) / double(fc_z19), double(c.raw) / double(fc_z19));
        printf("    (raw distinct = %.3f MiB; lcp raw = %.3f MiB; suf raw = %.3f MiB)\n",
               MB(dbytes), MB(lcp_s.size()), MB(suf_s.size()));
        ZSTD_freeCCtx(cc);
        return 0;
    }

    auto report = [&](const DefCodec::Params& pp, const Metrics& m) {
        double red_seg = double(base_z3) / double(m.seg_z3);
        double red_split = double(base_z3) / double(m.split_lit_z3 + m.split_ctrl_z3 + m.split_off_z3);
        uint64_t split_tot = m.split_lit_z3 + m.split_ctrl_z3 + m.split_off_z3;
        printf("  ---- cfg stride=%u min_match=%u table_bits=%u chain=%u skip_log=%u cross_rep=%d ----\n",
               pp.stride, pp.min_match, pp.table_bits, pp.chain, pp.skip_log, pp.cross_line_rep ? 1 : 0);
        printf("    [defcodec] seg concat -> none=%.3f  z1=%.3f  z3=%.3f MiB   store=%.3f MiB\n",
               MB(m.seg_bytes), MB(m.seg_z1), MB(m.seg_z3), MB(m.store_bytes));
        printf("    [defcodec] split z3   -> lit=%.3f  ctrl=%.3f  off=%.3f  total=%.3f MiB "
               "(raw lit=%.3f ctrl=%.3f off=%.3f)\n",
               MB(m.split_lit_z3), MB(m.split_ctrl_z3), MB(m.split_off_z3), MB(split_tot),
               MB(m.split_lit_raw), MB(m.split_ctrl_raw), MB(m.split_off_raw));
        printf("    REDUCTION vs plain-z3:  seg-concat=%.3fx   split=%.3fx\n", red_seg, red_split);
        printf("    dict-only ceiling ratio (raw/dict-z3):  plain=%.1fx  seg=%.1fx  split=%.1fx\n",
               double(c.raw) / double(base_z3), double(c.raw) / double(m.seg_z3), double(c.raw) / double(split_tot));
        printf("    throughput:  encode=%.3f GB/s  decode=%.3f GB/s   roundtrip=%s\n",
               m.enc_gbs, m.dec_gbs, m.roundtrip_ok ? "OK" : "*** FAIL ***");
        return red_seg;
    };

    if (!sweep) {
        Metrics m = run_config(d, P, cc);
        report(P, m);
        bool fc_ok = run_frontcodec(d, base_z3, c.raw, cc);
        if (!m.roundtrip_ok || !fc_ok) { fprintf(stderr, "ROUND-TRIP FAILED\n"); return 1; }
    } else {
        // small grid to find the ratio/speed Pareto (no corpus-name branches).
        uint32_t strides[] = {4, 8, 16, 32};
        uint32_t chains[] = {1, 4};
        double best = 0; DefCodec::Params bestP;
        for (uint32_t ch : chains) for (uint32_t s : strides) {
            DefCodec::Params pp = P; pp.stride = s; pp.min_match = s + 8; pp.chain = ch;
            Metrics m = run_config(d, pp, cc);
            double r = report(pp, m);
            if (m.roundtrip_ok && r > best) { best = r; bestP = pp; }
        }
        printf("  >>> best seg reduction %.3fx at stride=%u min_match=%u chain=%u\n",
               best, bestP.stride, bestP.min_match, bestP.chain);
    }
    ZSTD_freeCCtx(cc);
    return 0;
}
