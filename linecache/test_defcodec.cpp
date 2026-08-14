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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>
#include <zstd.h>

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
};
static inline bool is_slot_char(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
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

int main(int argc, char** argv) {
    const char* manifest = nullptr;
    const char* name = nullptr;
    size_t max_files = SIZE_MAX;
    bool sweep = false;
    bool ceiling = false;
    bool reorder = false;
    bool skeleton = false;
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
        else if (eat("--stride")) P.stride = uint32_t(atoi(argv[++i]));
        else if (eat("--min-match")) P.min_match = uint32_t(atoi(argv[++i]));
        else if (eat("--table-bits")) P.table_bits = uint32_t(atoi(argv[++i]));
        else if (eat("--chain")) P.chain = uint32_t(atoi(argv[++i]));
        else if (eat("--skip-log")) P.skip_log = uint32_t(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--no-cross-rep")) P.cross_line_rep = false;
        else { fprintf(stderr, "unknown arg %s\n", argv[i]); return 2; }
    }
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
