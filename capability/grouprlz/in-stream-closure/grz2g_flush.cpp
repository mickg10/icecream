// GRZ2_GROUPED -- bounded persistent-history long-range LZ for preprocessed C++ corpora.
//
// The entropy frame restarts per group; the MATCHER AND ITS HISTORY PERSIST across groups.
// Resetting the matcher at a group boundary is what recreates the fragmentation this codec
// exists to avoid, so it is never done.
//
// Modes:
//   G0  ungrouped baseline          -- one group covering the whole program
//   G1  FULL_COMMITTED_HISTORY      -- grouped; every committed byte stays referenceable
//   G2  ROLLING_COMMITTED_HISTORY   -- grouped; history_base advances, older bytes are
//                                      released by BOTH sides. This is the bounded row.
//
// Chronology invariants (all enforced, all tested):
//  * the parse of a group never reads a byte at or beyond that group's end -- forward match
//    extension is bounded by the group end, not by the input length;
//  * the stream header carries only frozen policy, never a future-derived total, so a live
//    encoder can emit group 0 without knowing what follows;
//  * anchors planted while a group is provisional are recorded in an undo log and are
//    restored exactly if the group is retried, so a retry re-parses from identical state
//    (anchors planted earlier in the ACTIVE group remain visible to later bytes of it --
//    that is required, not a violation);
//  * every COPY satisfies history_base_absolute <= source < current_output_absolute;
//  * the anchor table stores absolute_position+1 so byte 0 is representable, and is sized
//    from the configured retained-history budget, never from the total input length.
//
// build: g++ -O3 -march=native -std=c++17 -I<libbsc> -o grz3 grz3.cpp libbsc.a -lzstd -lpthread

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <thread>
#include <nmmintrin.h>
#include <cerrno>
#include <zstd.h>
#include "libbsc/libbsc.h"

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i64 = int64_t;

static double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void die(const char* m) { fprintf(stderr, "grz3: %s\n", m); exit(1); }
static u64 peak_rss_bytes() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (u64)ru.ru_maxrss * 1024ull;
}

static inline u64 rotl(u64 x, int r) { return (x << r) | (x >> ((64 - r) & 63)); }
static inline void putv(std::vector<u8>& v, u64 x) {
    while (x >= 0x80) { v.push_back((u8)(x | 0x80)); x >>= 7; }
    v.push_back((u8)x);
}
// Bounded varint read: never walks past the end of its own stream.
static inline u64 getv(const u8*& p, const u8* end) {
    u64 x = 0; int s = 0;
    for (;;) {
        if (p >= end) die("varint overruns stream");
        u8 c = *p++;
        x |= (u64)(c & 0x7f) << s;
        if (!(c & 0x80)) break;
        s += 7;
        if (s > 63) die("varint too long");
    }
    return x;
}
static inline u64 zz(i64 x) { return ((u64)x << 1) ^ (u64)(x >> 63); }
static inline i64 unzz(u64 x) { return (i64)(x >> 1) ^ -(i64)(x & 1); }

// Hardware CRC32C folded to 64 bits. It must be a STREAMING digest: the decoder sees a
// group as one or two spans (the retained-history ring can wrap mid-group) while the encoder
// sees it as one, so a chunk-boundary-dependent hash would disagree with itself. Carrying the
// partial 8-byte lane across calls makes the result a pure function of the byte sequence.
static const u64 DIG_SEED = 0x9E3779B97F4A7C15ull;
struct Dig {
    u64 h = DIG_SEED;
    u8 buf[8];
    int nbuf = 0;
    void add(const u8* p, size_t n) {
        if (nbuf) {
            while (n && nbuf < 8) { buf[nbuf++] = *p++; n--; }
            if (nbuf == 8) {
                u64 w; memcpy(&w, buf, 8);
                h = _mm_crc32_u64(h, w);
                nbuf = 0;
            }
        }
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            u64 w; memcpy(&w, p + i, 8);
            h = _mm_crc32_u64(h, w);
        }
        for (; i < n; i++) buf[nbuf++] = p[i];
    }
    u64 fin() const {
        u64 x = h;
        for (int i = 0; i < nbuf; i++) x = _mm_crc32_u8((u32)x, buf[i]);
        return x;
    }
};

// ------------------------------------------------------------- explicit LE frame I/O
struct Wr {
    FILE* f;
    u64 n = 0;
    void raw(const void* p, size_t k) { if (k && fwrite(p, 1, k, f) != k) die("fwrite"); n += k; }
    void u8v(u8 v) { raw(&v, 1); }
    void u32v(u32 v) { u8 b[4]; for (int i = 0; i < 4; i++) b[i] = (u8)(v >> (8 * i)); raw(b, 4); }
    void u64v(u64 v) { u8 b[8]; for (int i = 0; i < 8; i++) b[i] = (u8)(v >> (8 * i)); raw(b, 8); }
};
struct Rd {
    const u8* p;
    const u8* e;
    bool have(size_t k) const { return (size_t)(e - p) >= k; }
    u8 u8v() { if (!have(1)) die("truncated frame"); return *p++; }
    u32 u32v() { if (!have(4)) die("truncated frame"); u32 v = 0; for (int i = 0; i < 4; i++) v |= (u32)p[i] << (8 * i); p += 4; return v; }
    u64 u64v() { if (!have(8)) die("truncated frame"); u64 v = 0; for (int i = 0; i < 8; i++) v |= (u64)p[i] << (8 * i); p += 8; return v; }
};

static const u32 MAGIC_STREAM = 0x335A5247;  // "GRZ3"
static const u32 MAGIC_GROUP  = 0x46505247;  // "GRPF"
static const u32 MAGIC_END    = 0x444E4547;  // "GEND"

// ------------------------------------------------------------- entropy backends
enum { BE_STORE = 0, BE_Z19 = 1, BE_BSC_E2 = 2, BE_BSC_E1 = 3, BE_BSC_E0 = 4,
       BE_Z12 = 5, BE_Z16 = 6 };
static int g_bsc_features = LIBBSC_FEATURE_FASTMODE;
static bool be_is_bsc(int be) { return be >= BE_BSC_E2 && be <= BE_BSC_E0; }
static int be_zlevel(int be) { switch (be) { case BE_Z12: return 12; case BE_Z16: return 16; default: return 19; } }
static int be_coder(int be) {
    switch (be) { case BE_BSC_E1: return LIBBSC_CODER_QLFC_STATIC;
                  case BE_BSC_E0: return LIBBSC_CODER_QLFC_FAST;
                  default: return LIBBSC_CODER_QLFC_ADAPTIVE; }
}
static std::vector<u8> be_compress(int be, const u8* p, size_t n, u8* used) {
    std::vector<u8> out;
    if (!n) { *used = BE_STORE; return out; }
    if (be_is_bsc(be)) {
        if (n > (size_t)INT32_MAX - 64) die("stream too large for libbsc block");
        out.resize(n + LIBBSC_HEADER_SIZE);
        int r = bsc_compress(p, out.data(), (int)n, LIBBSC_DEFAULT_LZPHASHSIZE,
                             LIBBSC_DEFAULT_LZPMINLEN, LIBBSC_BLOCKSORTER_BWT,
                             be_coder(be), g_bsc_features);
        if (r > 0 && (size_t)r < n) { out.resize(r); *used = (u8)be; return out; }
    } else if (be != BE_STORE) {
        out.resize(ZSTD_compressBound(n));
        size_t r = ZSTD_compress(out.data(), out.size(), p, n, be_zlevel(be));
        if (!ZSTD_isError(r) && r < n) { out.resize(r); *used = (u8)be; return out; }
    }
    out.assign(p, p + n);
    *used = BE_STORE;
    return out;
}
static void be_decompress(int be, const u8* p, size_t n, u8* out, size_t rawn) {
    if (!rawn) return;
    if (be == BE_STORE) { memcpy(out, p, rawn); return; }
    if (be_is_bsc(be)) {
        if (bsc_decompress(p, (int)n, out, (int)rawn, g_bsc_features) != LIBBSC_NO_ERROR)
            die("bsc_decompress failed");
        return;
    }
    size_t r = ZSTD_decompress(out, rawn, p, n);
    if (ZSTD_isError(r) || r != rawn) die("ZSTD_decompress failed");
}

// Per-group, on ACTUAL bytes: no program-level hindsight is allowed in a binding row, so the
// choice is made from this group's own streams only.
static std::vector<u8> pick_group_backend(const u8* p, size_t n, const int* cands, int ncand, u8* used) {
    std::vector<u8> best;
    u8 bu = BE_STORE;
    for (int i = 0; i < ncand; i++) {
        u8 u;
        std::vector<u8> c = be_compress(cands[i], p, n, &u);
        if (best.empty() || c.size() < best.size()) { best.swap(c); bu = u; }
    }
    *used = bu;
    return best;
}

// ------------------------------------------------------------- config
enum { MODE_G0 = 0, MODE_G1 = 1, MODE_G2 = 2 };
struct Cfg {
    int mode = MODE_G1;
    u32 K = 256, sbits = 5, tbits = 0;
    int litbe = BE_BSC_E2, tokbe = BE_Z19, threads = 1;
    size_t blkraw = 8u << 20;
    u64 gtu = 112;                       // group close: TU count
    u64 graw = 512ull << 20;             // group close: raw output bytes
    u64 gadd = 32ull << 20;              // group close: ADD (literal) bytes
    u64 hist = 2048ull << 20;            // G2 retained history extent
    u64 anchor_budget = 2048ull << 20;   // index sizing budget (never the input length)
    u32 tbits_cap = 26;
    int retry_test = 0;                  // parse every Nth group twice, assert identical
    int select = 0;                      // 1 = per-group actual-byte backend selection
    u64 build_tus = 0;                   // force a group close at every multiple of this
};

struct Buf { u8* p; size_t n; };
static Buf map_file(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open input");
    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat");
    if (st.st_size == 0) { close(fd); return {nullptr, 0}; }
    void* m = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) die("mmap");
    close(fd);
    return {(u8*)m, (size_t)st.st_size};
}
static std::vector<u64> load_tu(const char* path) {
    std::vector<u64> v;
    if (!path) return v;
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open tu map");
    struct stat st; fstat(fd, &st);
    v.resize(st.st_size / 8);
    if (read(fd, v.data(), v.size() * 8) != (ssize_t)(v.size() * 8)) die("read tu map");
    close(fd);
    return v;
}
static void init_gear(u64* T) {
    u64 s = 0x243F6A8885A308D3ull;
    for (int i = 0; i < 256; i++) {
        s += 0x9E3779B97F4A7C15ull;
        u64 z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        T[i] = z ^ (z >> 31);
    }
}

// ------------------------------------------------------------- encoder
struct GroupStat {
    u64 idx, tu_lo, tu_hi, out_bytes, add_bytes, comp_bytes, hist_base, hist_extent;
    const char* closed_by;
    u64 end_offset;      // absolute byte offset in THIS stream after the group is written
};

static void encode(const char* in, const char* outp, const Cfg& cfg, const char* tupath,
                   const char* curve) {
    double T0 = now();
    Buf b = map_file(in);
    const u8* d = b.p;
    const u64 n = b.n;
    std::vector<u64> tu = load_tu(tupath);
    if (!tu.empty() && tu.back() != n) die("TU map does not match input size");
    if (cfg.mode != MODE_G0 && tu.size() < 2) die("grouped modes require -u tu.map");

    u64 T[256], Tout[256];
    init_gear(T);
    for (int i = 0; i < 256; i++) Tout[i] = rotl(T[i], (int)(cfg.K & 63));

    // Index sized from the retained-history/anchor budget and an explicit cap. Deliberately
    // NOT a function of n: G2 must not grow its index with total program size.
    u32 tbits = cfg.tbits;
    if (!tbits) {
        u64 budget = (cfg.mode == MODE_G2) ? cfg.hist : cfg.anchor_budget;
        tbits = 16;
        while (tbits < cfg.tbits_cap && ((u64)1 << tbits) < (budget >> (cfg.sbits + 3))) tbits++;
    }
    const size_t tslots = (size_t)1 << tbits;
    const size_t tbytes = tslots * sizeof(u64);
    u64* tbl = (u64*)mmap(nullptr, tbytes, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (tbl == MAP_FAILED) die("mmap anchor index");
    madvise(tbl, tbytes, MADV_HUGEPAGE);

    FILE* f = fopen(outp, "wb");
    if (!f) die("fopen container");
    Wr w{f};
    // STREAM_HEADER: frozen policy only. No raw/nseg/ntu -- nothing future-derived, so the
    // first group can be emitted by a live encoder that has not seen the rest of the build.
    w.u32v(MAGIC_STREAM);
    w.u32v(3);
    w.u8v((u8)cfg.mode);
    w.u32v(cfg.K); w.u32v(cfg.sbits); w.u32v(tbits);
    w.u8v((u8)cfg.litbe); w.u8v((u8)cfg.tokbe); w.u8v((u8)cfg.select);
    w.u64v(cfg.blkraw);
    w.u64v(cfg.gtu); w.u64v(cfg.graw); w.u64v(cfg.gadd);
    w.u64v(cfg.hist); w.u64v(cfg.anchor_budget);

    std::vector<u8> lits, ll, sd, sd2, ml;
    std::vector<std::pair<u32, u64>> undo;    // (slot, previous value) -- exact rollback
    std::vector<GroupStat> curves;
    const u64 amask = (1ull << cfg.sbits) - 1;
    const u32 K = cfg.K;

    u64 nmatch = 0, ins = 0, stale = 0, verify_fail = 0, src0 = 0;
    Dig wdig_acc;
    u64 gpos = 0, gidx = 0, tu_lo = 0;
    double tmatch = 0, tent = 0;
    u64 max_group_out = 0, released_upto = 0, oversize_tu = 0;

    while (gpos < n) {
        // Group window: TU cap and raw cap are known before parsing; the ADD cap can only be
        // discovered by parsing, so exceeding it triggers a real rollback + retry on a
        // smaller TU span. Every boundary stays TU-aligned.
        const u64 ntu = tu.empty() ? 0 : tu.size() - 1;
        u64 tu_hi = ntu;
        u64 s1 = n;
        const char* closed = "eof";
        if (cfg.mode != MODE_G0) {
            u64 lim = std::min(tu_lo + cfg.gtu, ntu);
            // A build boundary is a hard close: the group must end there so the build is
            // complete and independently decodable at its own last TU.  Only the GROUP
            // closes; matcher and dictionary state carry forward untouched.
            bool at_build = false;
            if (cfg.build_tus) {
                u64 nb = (tu_lo / cfg.build_tus + 1) * cfg.build_tus;
                if (nb < lim) { lim = nb; at_build = true; }
                else if (nb == lim) at_build = true;
            }
            tu_hi = tu_lo + 1;
            while (tu_hi < lim && tu[tu_hi + 1] - gpos <= cfg.graw) tu_hi++;
            closed = (tu_hi == lim) ? (at_build ? "build" : "tu") : "raw";
            s1 = tu[tu_hi];
            if (tu_hi == tu_lo + 1 && s1 - gpos > cfg.graw) { closed = "oversize_single_tu"; oversize_tu++; }
        }

        u64 hist_base = 0;
        if (cfg.mode == MODE_G2) hist_base = gpos > cfg.hist ? gpos - cfg.hist : 0;

        u64 gnm = 0;
        int retries = 0;
        std::vector<u8> probe_ll, probe_lit, probe_sd, probe_ml;
        u64 probe_end = 0, probe_nm = 0;
        bool probing = (cfg.retry_test && (gidx % (u64)cfg.retry_test) == 0);

        for (;;) {
            lits.clear(); ll.clear(); sd.clear(); sd2.clear(); ml.clear();
            undo.clear();
            gnm = 0;
            double a0 = now();
            u64 lit_start = gpos, prev_src_end = 0, prev_off = 0;
            const u64 end = s1;

            if (gpos + K <= end) {
                u64 wp = gpos, h = 0;
                for (u32 i = 0; i < K; i++) h = rotl(h, 1) ^ T[d[wp + i]];
                for (;;) {
                    bool did = false;
                    if ((h & amask) == 0) {
                        u32 slot = (u32)((h * 0x9E3779B97F4A7C15ull) >> (64 - tbits));
                        u64 rawv = tbl[slot];
                        u64 q = rawv ? rawv - 1 : 0;
                        bool usable = rawv && q < wp && q >= hist_base;
                        if (rawv && q < hist_base) stale++;   // released -- lazily replaced below
                        if (usable && memcmp(d + q, d + wp, K) == 0) {
                            // Forward extension bounded by the GROUP end: parsing this group
                            // never reads a byte at or past its own end.
                            u64 a = wp + K, c = q + K;
                            while (a + 8 <= end) {
                                u64 x, y;
                                memcpy(&x, d + a, 8);
                                memcpy(&y, d + c, 8);
                                if (x != y) { a += (u64)(__builtin_ctzll(x ^ y) >> 3); goto fwd; }
                                a += 8; c += 8;
                            }
                            while (a < end && d[a] == d[c]) { a++; c++; }
                        fwd:;
                            u64 back = 0;
                            while (wp - back > lit_start && q - back > hist_base &&
                                   d[wp - back - 1] == d[q - back - 1]) back++;
                            u64 ms = wp - back, src = q - back, len = a - ms;
                            if (len >= K) {
                                if (src < hist_base || src >= ms) die("COPY violates history bound");
                                u64 litlen = ms - lit_start;
                                putv(ll, litlen);
                                if (litlen) lits.insert(lits.end(), d + lit_start, d + ms);
                                putv(sd, zz((i64)src - (i64)prev_src_end));
                                u64 off = ms - src;
                                putv(sd2, zz((i64)off - (i64)prev_off));
                                putv(ml, len);
                                prev_src_end = src + len;
                                prev_off = off;
                                lit_start = ms + len;
                                if (src == 0) src0++;      // absolute byte zero as a source
                                gnm++;
                                wp = lit_start;
                                if (wp + K > end) break;
                                h = 0;
                                for (u32 i = 0; i < K; i++) h = rotl(h, 1) ^ T[d[wp + i]];
                                did = true;
                            }
                        }
                        if (!did) {
                            // Provisional, exactly reversible: log the previous value before
                            // touching the slot so a retry re-parses from identical state.
                            undo.push_back({slot, tbl[slot]});
                            tbl[slot] = wp + 1;    // +1 so absolute position 0 is representable
                            ins++;
                        }
                    }
                    if (!did) {
                        if (wp + K >= end) break;
                        h = rotl(h, 1) ^ Tout[d[wp]] ^ T[d[wp + K]];
                        wp++;
                    }
                }
            }
            if (end > lit_start) {
                putv(ll, end - lit_start);
                lits.insert(lits.end(), d + lit_start, d + end);
            }
            tmatch += now() - a0;

            if (probing) {
                // Retry probe: roll back and re-parse the identical span; results must match.
                probe_ll = ll; probe_lit = lits; probe_sd = sd; probe_ml = ml;
                probe_end = end; probe_nm = gnm;
                for (size_t i = undo.size(); i-- > 0;) tbl[undo[i].first] = undo[i].second;
                probing = false;
                continue;
            }
            if (probe_end) {
                if (probe_end != end || probe_ll != ll || probe_lit != lits ||
                    probe_sd != sd || probe_ml != ml || probe_nm != gnm) {
                    verify_fail++;
                    fprintf(stderr, "RETRY MISMATCH at group %llu\n", (unsigned long long)gidx);
                }
                probe_end = 0;
            }
            // ADD cap: close earlier (fewer TUs) and re-parse from restored state.
            if (cfg.mode != MODE_G0 && lits.size() > cfg.gadd && tu_hi > tu_lo + 1) {
                for (size_t i = undo.size(); i-- > 0;) tbl[undo[i].first] = undo[i].second;
                u64 span = tu_hi - tu_lo;
                tu_hi = tu_lo + std::max<u64>(1, span / 2);
                s1 = tu[tu_hi];
                closed = "add";
                retries++;
                continue;
            }
            nmatch += gnm;
            break;
        }
        // Group closes: the provisional anchors become committed
        // (the undo log is dropped -- nothing to roll back).
        undo.clear();

        double e0 = now();
        int offmode;
        {
            size_t cap = ZSTD_compressBound(sd.size() > sd2.size() ? sd.size() : sd2.size());
            std::vector<u8> t(cap ? cap : 1);
            size_t ra = sd.empty() ? 0 : ZSTD_compress(t.data(), t.size(), sd.data(), sd.size(), 1);
            size_t rb = sd2.empty() ? 0 : ZSTD_compress(t.data(), t.size(), sd2.data(), sd2.size(), 1);
            offmode = (!sd2.empty() && rb < ra) ? 1 : 0;
        }
        const std::vector<u8>& srcs = offmode ? sd2 : sd;

        u8 be_ll, be_sd, be_ml;
        std::vector<u8> c_ll, c_sd, c_ml;
        static const int tok_cands[] = {BE_Z19, BE_BSC_E1};
        static const int lit_cands[] = {BE_BSC_E2, BE_BSC_E1, BE_Z19};
        if (cfg.select) {
            c_ll = pick_group_backend(ll.data(), ll.size(), tok_cands, 2, &be_ll);
            c_sd = pick_group_backend(srcs.data(), srcs.size(), tok_cands, 2, &be_sd);
            c_ml = pick_group_backend(ml.data(), ml.size(), tok_cands, 2, &be_ml);
        } else {
            c_ll = be_compress(cfg.tokbe, ll.data(), ll.size(), &be_ll);
            c_sd = be_compress(cfg.tokbe, srcs.data(), srcs.size(), &be_sd);
            c_ml = be_compress(cfg.tokbe, ml.data(), ml.size(), &be_ml);
        }
        // literal blocks
        std::vector<std::vector<u8>> lb;
        std::vector<u8> lbe;
        {
            size_t nb = lits.empty() ? 0 : (lits.size() + cfg.blkraw - 1) / cfg.blkraw;
            lb.resize(nb); lbe.assign(nb, BE_STORE);
            auto job = [&](size_t lo, size_t hi) {
                for (size_t i = lo; i < hi; i++) {
                    size_t off = i * cfg.blkraw;
                    size_t len = lits.size() - off < cfg.blkraw ? lits.size() - off : cfg.blkraw;
                    if (cfg.select) lb[i] = pick_group_backend(lits.data() + off, len, lit_cands, 3, &lbe[i]);
                    else            lb[i] = be_compress(cfg.litbe, lits.data() + off, len, &lbe[i]);
                }
            };
            if (cfg.threads <= 1 || nb <= 1) job(0, nb);
            else {
                size_t nt = (size_t)cfg.threads < nb ? (size_t)cfg.threads : nb;
                size_t per = (nb + nt - 1) / nt;
                std::vector<std::thread> th;
                for (size_t t = 0; t * per < nb; t++)
                    th.emplace_back(job, t * per, std::min(nb, t * per + per));
                for (auto& x : th) x.join();
            }
        }

        u64 gout = s1 - gpos;
        Dig gd; gd.add(d + gpos, gout);
        u64 gdig = gd.fin();
        wdig_acc.add(d + gpos, gout);

        u64 before = w.n;
        w.u32v(MAGIC_GROUP);
        w.u64v(gidx);
        w.u64v(gpos);
        w.u64v(gout);
        w.u64v(hist_base);
        w.u64v(gpos - hist_base);
        w.u32v((u32)(tu.empty() ? 0 : tu_hi - tu_lo));
        for (u64 t = tu_lo; t < tu_hi && !tu.empty(); t++) w.u64v(tu[t + 1] - tu[t]);
        w.u8v((u8)offmode);
        w.u8v(be_ll); w.u8v(lb.empty() ? BE_STORE : lbe[0]); w.u8v(be_sd); w.u8v(be_ml);
        w.u64v(ll.size()); w.u64v(lits.size()); w.u64v(srcs.size()); w.u64v(ml.size());
        w.u64v(c_ll.size());
        u64 clit = 0; for (auto& x : lb) clit += x.size();
        w.u64v(clit); w.u64v(c_sd.size()); w.u64v(c_ml.size());
        w.u32v((u32)lb.size());
        for (size_t i = 0; i < lb.size(); i++) { w.u64v(lb[i].size()); w.u8v(lbe[i]); }
        w.u64v(gdig);
        w.raw(c_ll.data(), c_ll.size());
        for (auto& x : lb) w.raw(x.data(), x.size());
        w.raw(c_sd.data(), c_sd.size());
        w.raw(c_ml.data(), c_ml.size());
        tent += now() - e0;

        curves.push_back({gidx, tu_lo, tu_hi, gout, (u64)lits.size(), w.n - before,
                          hist_base, gpos - hist_base, closed, w.n});
        if (gout > max_group_out) max_group_out = gout;

        // G2: release everything below the NEXT group's history base on the encoder side too,
        // so the encoder's resident set is bounded, not just the decoder's.
        gpos = s1;
        gidx++;
        if (!tu.empty()) tu_lo = tu_hi;
        if (cfg.mode == MODE_G2) {
            // Release only the interval that expired since the last group; re-advising the
            // whole prefix every group cost measurable encode throughput.
            u64 nb2 = gpos > cfg.hist ? gpos - cfg.hist : 0;
            u64 pg = nb2 & ~(u64)4095;
            if (pg > released_upto) {
                madvise((void*)(b.p + released_upto), pg - released_upto, MADV_DONTNEED);
                released_upto = pg;
            }
        }
    }

    w.u32v(MAGIC_END);
    w.u64v(n);
    w.u64v(gidx);
    w.u64v(nmatch);
    w.u64v(wdig_acc.fin());
    u64 total = w.n;
    fclose(f);
    double T1 = now();

    if (curve) {
        FILE* cf = fopen(curve, "w");
        if (cf) {
            fprintf(cf, "group\ttu_lo\ttu_hi\tout_bytes\tadd_bytes\tcomp_bytes\tratio\thist_base\thist_extent\tclosed_by\tend_offset\n");
            for (auto& g : curves)
                fprintf(cf, "%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%.2f\t%llu\t%llu\t%s\t%llu\n",
                        (unsigned long long)g.idx, (unsigned long long)g.tu_lo, (unsigned long long)g.tu_hi,
                        (unsigned long long)g.out_bytes, (unsigned long long)g.add_bytes,
                        (unsigned long long)g.comp_bytes,
                        g.comp_bytes ? (double)g.out_bytes / g.comp_bytes : 0.0,
                        (unsigned long long)g.hist_base, (unsigned long long)g.hist_extent, g.closed_by,
                        (unsigned long long)g.end_offset);
            fclose(cf);
        }
    }

    double sec = T1 - T0;
    fprintf(stderr, "ENC %s mode=G%d raw=%llu out=%llu groups=%llu idx_bytes=%zu "
                    "match=%.2f entropy=%.2f total=%.2f  C=%.0f B/s (%.1f MiB/s)  peakRSS=%.2f GiB retry_fail=%llu oversize_tu=%llu src0=%llu\n",
            in, cfg.mode, (unsigned long long)n, (unsigned long long)total,
            (unsigned long long)gidx, tbytes, tmatch, tent, sec,
            n / sec, n / sec / 1048576.0, peak_rss_bytes() / 1073741824.0,
            (unsigned long long)verify_fail, (unsigned long long)oversize_tu,
            (unsigned long long)src0);
    printf("%llu\t%llu\t%llu\t%.4f\t%.4f\t%.4f\t%zu\t%llu\t%llu\t%llu\t%llu\n",
           (unsigned long long)n, (unsigned long long)total, (unsigned long long)gidx,
           tmatch, tent, sec, tbytes, peak_rss_bytes(),
           (unsigned long long)max_group_out, (unsigned long long)stale,
           (unsigned long long)(verify_fail + oversize_tu));
    munmap(tbl, tbytes);
}

// ------------------------------------------------------------- decoder
// Retains only [history_base, current) in a ring, so G2's decoder memory is bounded by the
// declared extent plus one group -- the whole point of the bounded row.
struct Hist {
    std::vector<u8> b;
    u64 mask = 0;
    bool bounded = false;
    void init(bool bnd, u64 cap) {
        bounded = bnd;
        if (bnd) {
            u64 c = 1;
            while (c < cap) c <<= 1;
            b.assign(c, 0);
            mask = c - 1;
        } else {
            b.reserve(cap ? cap : (1u << 20));
        }
    }
    // Unbounded (G0/G1) retains everything, so it grows; the total is never known up front
    // because the stream header carries no future totals.
    void ensure(u64 upto) { if (!bounded && b.size() < upto) b.resize(upto); }
    inline u8* at(u64 a) { return bounded ? b.data() + (a & mask) : b.data() + a; }
    void put(u64 dst, const u8* src, u64 len) {
        if (!bounded) { memcpy(b.data() + dst, src, len); return; }
        while (len) {
            u64 off = dst & mask;
            u64 k = std::min<u64>(len, b.size() - off);
            memcpy(b.data() + off, src, k);
            src += k; dst += k; len -= k;
        }
    }
    void copy(u64 dst, u64 src, u64 len) {
        u64 off = dst - src;
        if (!bounded) {
            if (off >= len) memcpy(b.data() + dst, b.data() + src, len);
            else { u8* D = b.data() + dst; const u8* S = b.data() + src;
                   for (u64 i = 0; i < len; i++) D[i] = S[i]; }
            return;
        }
        if (off >= len) {
            while (len) {
                u64 dO = dst & mask, sO = src & mask;
                u64 k = std::min(len, std::min(b.size() - dO, b.size() - sO));
                memcpy(b.data() + dO, b.data() + sO, k);
                dst += k; src += k; len -= k;
            }
        } else {
            for (u64 i = 0; i < len; i++) *at(dst + i) = *at(src + i);
        }
    }
    // Digest in place: a copy of every group just to hash it doubled the decode cost.
    void feed(Dig& dg, u64 from, u64 len) {
        while (len) {
            u64 off = bounded ? (from & mask) : from;
            u64 k = bounded ? std::min<u64>(len, b.size() - off) : len;
            dg.add(b.data() + off, k);
            from += k; len -= k;
        }
    }
    // A group must stay TU-aligned, so a single oversized TU can exceed the raw cap. Each
    // group frame declares its own output length before it is decoded, so the ring is sized
    // from history + THIS group rather than from a future-derived maximum.
    void grow(u64 need, u64 from, u64 len) {
        u64 c = 1;
        while (c < need) c <<= 1;
        if (c <= b.size()) return;
        std::vector<u8> nb(c, 0);
        u64 nmask = c - 1;
        for (u64 i = 0; i < len; i++) nb[(from + i) & nmask] = *at(from + i);
        b.swap(nb);
        mask = nmask;
    }
    void emit(FILE* f, u64 from, u64 len) {
        if (!bounded) { if (len && fwrite(b.data() + from, 1, len, f) != len) die("fwrite output"); return; }
        while (len) {
            u64 off = from & mask;
            u64 k = std::min<u64>(len, b.size() - off);
            if (fwrite(b.data() + off, 1, k, f) != k) die("fwrite output");
            from += k; len -= k;
        }
    }
};

static void decode(const char* inp, const char* outp, bool prefix_mode, int threads, u64 maxg) {
    double T0 = now();
    int fd = open(inp, O_RDONLY);
    if (fd < 0) die("open container");
    struct stat st; fstat(fd, &st);
    void* m = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) die("mmap container");
    close(fd);
    Rd r{(const u8*)m, (const u8*)m + st.st_size};

    if (r.u32v() != MAGIC_STREAM) die("bad stream magic");
    if (r.u32v() != 3) die("bad version");
    int mode = r.u8v();
    u32 K = r.u32v(); (void)K;
    r.u32v(); r.u32v();
    r.u8v(); r.u8v(); r.u8v();
    u64 blkraw = r.u64v();
    r.u64v();
    u64 graw = r.u64v();
    r.u64v();
    u64 hist = r.u64v();
    r.u64v();

    FILE* of = fopen(outp, "wb");
    if (!of) die("fopen output");

    Hist ring;
    bool ring_ready = false;
    u64 pos = 0, groups = 0, nmatch_seen = 0;
    Dig wdig_acc;
    bool saw_end = false;
    u64 end_raw = 0, end_groups = 0, end_dig = 0, end_match = 0;
    double tent = 0;

    for (;;) {
        // A prefix is valid ONLY at an exact completed-frame boundary. Anything else --
        // stray trailing bytes, a short header, a short payload -- is a hard failure in
        // both modes; it is never silently accepted as "a prefix".
        if (r.p == r.e) {
            if (prefix_mode) break;
            die("full decode requires END_FRAME");
        }
        if (!r.have(4)) die("trailing bytes past the last complete frame");
        u32 mg = 0;
        for (int i = 0; i < 4; i++) mg |= (u32)r.p[i] << (8 * i);
        if (mg == MAGIC_END) {
            r.u32v();
            end_raw = r.u64v(); end_groups = r.u64v(); end_match = r.u64v(); end_dig = r.u64v();
            saw_end = true;
            break;
        }
        if (mg != MAGIC_GROUP) die("bad frame magic");
        if (prefix_mode && maxg && groups >= maxg) break;
        r.u32v();
        u64 gi = r.u64v();
        u64 gstart = r.u64v();
        u64 gout = r.u64v();
        u64 hbase = r.u64v();
        u64 hext = r.u64v();
        u32 ntu = r.u32v();
        u64 tusum = 0;
        for (u32 i = 0; i < ntu; i++) tusum += r.u64v();
        u8 offmode = r.u8v();
        u8 be0 = r.u8v(); r.u8v(); u8 be2 = r.u8v(); u8 be3 = r.u8v();
        u64 raw0 = r.u64v(), raw1 = r.u64v(), raw2 = r.u64v(), raw3 = r.u64v();
        u64 c0 = r.u64v(), c1 = r.u64v(), c2 = r.u64v(), c3 = r.u64v();
        u32 nlb = r.u32v();
        std::vector<u64> lz(nlb);
        std::vector<u8> lbe(nlb);
        for (u32 i = 0; i < nlb; i++) { lz[i] = r.u64v(); lbe[i] = r.u8v(); }
        u64 gdig = r.u64v();
        if (!r.have(c0 + c1 + c2 + c3)) die("incomplete group payload");
        // Frame/state closure: every declared field is checked, not decoration.
        if (gi != groups) die("group index out of sequence");
        if (gstart != pos) die("group start does not continue the stream");
        if (hext != gstart - hbase) die("history extent disagrees with base");
        if (mode == MODE_G2) {
            u64 want = gstart > hist ? gstart - hist : 0;
            if (hbase != want) die("history base disagrees with the frozen window");
        } else if (hbase != 0) die("full-history mode declared a nonzero history base");
        if (ntu && tusum != gout) die("TU lengths do not sum to the group output");
        { u64 lsum = 0; for (u32 i = 0; i < nlb; i++) lsum += lz[i];
          if (lsum != c1) die("literal block sizes do not sum to the stream size"); }

        if (!ring_ready) {
            u64 gcap = std::max(graw, gout);
            if (mode == MODE_G2) ring.init(true, hist + gcap);
            else                 ring.init(false, 4 * gout + (64u << 20));
            ring_ready = true;
        }
        ring.ensure(pos + gout);
        if (mode == MODE_G2) {
            if (hist + gout > ring.b.size()) ring.grow(hist + gout, hbase, hext);
            if (hext + gout > ring.b.size())
                die("retained history + group exceeds the ring capacity");
        }

        double e0 = now();
        std::vector<u8> s0(raw0), s1v(raw1), s2(raw2), s3(raw3);
        const u8* q = r.p;
        be_decompress(be0, q, c0, s0.data(), raw0); q += c0;
        {
            struct J { const u8* src; u64 cs; u8* dst; size_t len; u8 be; };
            std::vector<J> jobs(nlb);
            size_t off = 0;
            for (u32 i = 0; i < nlb; i++) {
                size_t len = raw1 - off < blkraw ? raw1 - off : (size_t)blkraw;
                jobs[i] = {q, lz[i], s1v.data() + off, len, lbe[i]};
                q += lz[i]; off += len;
            }
            auto run = [&](size_t lo, size_t hi) {
                for (size_t i = lo; i < hi; i++)
                    be_decompress(jobs[i].be, jobs[i].src, jobs[i].cs, jobs[i].dst, jobs[i].len);
            };
            if (threads <= 1 || nlb <= 1) run(0, nlb);
            else {
                size_t nt = (size_t)threads < nlb ? (size_t)threads : nlb;
                size_t per = (nlb + nt - 1) / nt;
                std::vector<std::thread> th;
                for (size_t t = 0; t * per < nlb; t++)
                    th.emplace_back(run, t * per, std::min<size_t>(nlb, t * per + per));
                for (auto& x : th) x.join();
            }
        }
        be_decompress(be2, q, c2, s2.data(), raw2); q += c2;
        be_decompress(be3, q, c3, s3.data(), raw3); q += c3;
        tent += now() - e0;

        const u8* pll = s0.data(); const u8* pll_e = pll + s0.size();
        const u8* plit = s1v.data(); const u8* plit_e = plit + s1v.size();
        const u8* psd = s2.data(); const u8* psd_e = psd + s2.size();
        const u8* pml = s3.data(); const u8* pml_e = pml + s3.size();
        u64 stop = pos + gout, prev_src_end = 0, prev_off = 0;
        while (pos < stop) {
            u64 litlen = getv(pll, pll_e);
            if (litlen) {
                if ((u64)(plit_e - plit) < litlen) die("literal stream exhausted");
                ring.put(pos, plit, litlen);
                plit += litlen; pos += litlen;
            }
            if (pos >= stop) break;
            u64 src, off;
            if (offmode) { off = (u64)((i64)prev_off + unzz(getv(psd, psd_e))); src = pos - off; }
            else { src = (u64)((i64)prev_src_end + unzz(getv(psd, psd_e))); off = pos - src; }
            u64 len = getv(pml, pml_e);
            if (src < hbase || src >= pos || pos + len > stop) die("COPY out of bounds");
            ring.copy(pos, src, len);
            prev_src_end = src + len; prev_off = off;
            pos += len;
            nmatch_seen++;
        }
        if (pos != stop) die("group short decode");
        // A group is not closed until every one of its streams is exactly consumed.
        if (pll != pll_e || plit != plit_e || psd != psd_e || pml != pml_e)
            die("group left bytes unconsumed in a token stream");
        // verify the group reproduced exactly what the encoder digested
        { Dig gd; ring.feed(gd, gstart, gout);
          if (gd.fin() != gdig) die("group digest mismatch"); }
        ring.feed(wdig_acc, gstart, gout);
        ring.emit(of, gstart, gout);            // streaming output: group is done, ship it
        r.p = q;
        groups++;
    }
    fclose(of);
    double T1 = now();

    // END closure is common to BOTH modes: if an END frame is present its totals and digest
    // must hold and nothing may follow it, whether or not this was a deliberate prefix read.
    // A prefix WITHOUT an END is legitimate, but only at an exact group-frame boundary.
    if (saw_end) {
        if (pos != end_raw) die("decoded byte count != END_FRAME total");
        if (groups != end_groups) die("group count != END_FRAME total");
        if (end_match != nmatch_seen) die("match count != END_FRAME total");
        if (wdig_acc.fin() != end_dig) die("whole-stream digest mismatch");
        if (r.p != r.e) die("trailing bytes after END_FRAME");
    } else if (!prefix_mode) {
        die("full decode requires END_FRAME");
    }
    double sec = T1 - T0;
    fprintf(stderr, "DEC %s groups=%llu bytes=%llu entropy=%.2f total=%.2f  F=%.0f B/s (%.1f MiB/s) "
                    " ring=%.2f GiB peakRSS=%.2f GiB %s\n",
            inp, (unsigned long long)groups, (unsigned long long)pos, tent, sec,
            pos / sec, pos / sec / 1048576.0, ring.b.size() / 1073741824.0,
            peak_rss_bytes() / 1073741824.0, prefix_mode ? "[PREFIX]" : "[FULL,VERIFIED]");
    printf("%llu\t%llu\t%.4f\t%.4f\t%llu\t%llu\n", (unsigned long long)pos,
           (unsigned long long)groups, tent, sec, (u64)ring.b.size(), peak_rss_bytes());
}

// ------------------------------------------------------------- tu map
static void make_tu(const char* manifest, const char* outp) {
    FILE* m = fopen(manifest, "r");
    if (!m) die("open manifest");
    std::vector<u64> off{0};
    char line[8192];
    u64 acc = 0;
    while (fgets(line, sizeof(line), m)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (!L) continue;
        struct stat st;
        if (stat(line, &st) != 0) die("stat TU");
        acc += (u64)st.st_size;
        off.push_back(acc);
    }
    fclose(m);
    FILE* f = fopen(outp, "wb");
    if (!f) die("fopen tu");
    fwrite(off.data(), 8, off.size(), f);
    fclose(f);
    printf("%zu\t%llu\n", off.size() - 1, (unsigned long long)acc);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
            "usage: grz3 enc <in> <out> -u tu.map [-m g0|g1|g2] [-K n] [-s b] [-t b] [-l be] [-k be]\n"
            "                 [-b blkMB] [-j n] [--gtu n] [--graw MB] [--gadd MB] [--hist MB]\n"
            "                 [--anchor-budget MB] [--select 1] [--retry-test n] [--curve f.tsv]\n"
            "                 [--build-tus n]   (close a group exactly at every n TUs)\n"
            "       grz3 dec <in> <out> [-j n]            (requires END_FRAME; verifies digests)\n"
            "       grz3 decprefix <in> <out> [-g n] [-j n]\n"
            "       grz3 tu <manifest> <out.tu>\n");
        return 1;
    }
    if (!strcmp(argv[1], "tu")) { make_tu(argv[2], argv[3]); return 0; }
    if (bsc_init(g_bsc_features) != LIBBSC_NO_ERROR) die("bsc_init");
    if (!strcmp(argv[1], "dec") || !strcmp(argv[1], "decprefix")) {
        int jt = 1; u64 mg = 0;
        for (int i = 4; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "-j")) jt = atoi(argv[i + 1]);
            else if (!strcmp(argv[i], "-g")) mg = (u64)atoll(argv[i + 1]);
        }
        decode(argv[2], argv[3], !strcmp(argv[1], "decprefix"), jt, mg);
        return 0;
    }
    if (strcmp(argv[1], "enc")) die("unknown mode");
    Cfg c;
    const char* tup = nullptr;
    const char* curve = nullptr;
    for (int i = 4; i + 1 < argc; i += 2) {
        const char* o = argv[i]; const char* v = argv[i + 1];
        if (!strcmp(o, "-u")) tup = v;
        else if (!strcmp(o, "-m")) c.mode = v[1] == '0' ? MODE_G0 : (v[1] == '1' ? MODE_G1 : MODE_G2);
        else if (!strcmp(o, "-K")) c.K = atoi(v);
        else if (!strcmp(o, "-s")) c.sbits = atoi(v);
        else if (!strcmp(o, "-t")) c.tbits = atoi(v);
        else if (!strcmp(o, "-l")) c.litbe = atoi(v);
        else if (!strcmp(o, "-k")) c.tokbe = atoi(v);
        else if (!strcmp(o, "-b")) c.blkraw = (size_t)atoll(v) << 20;
        else if (!strcmp(o, "-j")) c.threads = atoi(v);
        else if (!strcmp(o, "--gtu")) c.gtu = atoll(v);
        else if (!strcmp(o, "--graw")) c.graw = (u64)atoll(v) << 20;
        else if (!strcmp(o, "--gadd")) c.gadd = (u64)atoll(v) << 20;
        else if (!strcmp(o, "--hist")) c.hist = (u64)atoll(v) << 20;
        else if (!strcmp(o, "--anchor-budget")) c.anchor_budget = (u64)atoll(v) << 20;
        else if (!strcmp(o, "--select")) c.select = atoi(v);
        else if (!strcmp(o, "--retry-test")) c.retry_test = atoi(v);
        else if (!strcmp(o, "--curve")) curve = v;
        else if (!strcmp(o, "--build-tus")) {
            // strictly positive integer: reject empty, sign, trailing junk and overflow
            if (!v || !*v) die("--build-tus requires a value");
            for (const char* q = v; *q; ++q)
                if (*q < '0' || *q > '9') die("--build-tus must be a positive integer");
            errno = 0;
            char* endp = nullptr;
            unsigned long long bt = strtoull(v, &endp, 10);
            if (errno == ERANGE || !endp || *endp) die("--build-tus out of range");
            if (bt == 0) die("--build-tus must be > 0");
            c.build_tus = (u64)bt;
        }
        else die("unknown option");
    }
    encode(argv[2], argv[3], c, tup, curve);
    return 0;
}
