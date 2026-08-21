// GROUP-RLZ -- fast whole-program long-range LZ codec for preprocessed C++ (.ii) corpora.
//
// One left-to-right pass over the whole file with a content-defined anchor index over the
// *entire* history (no window bound by default, so cross-TU redundancy is never fragmented).
// Emits COPY(src,len) + ADD(literals) into four separate streams, each handed to its own
// entropy backend (libbsc BWT for the literals, libzstd for the token streams).
//
// Bytes covered by a COPY are never hashed, so on highly redundant corpora the rolling hash
// is only paid on novel content and the parse runs at memcmp speed.
//
// The parse is strictly backward-referencing, so with a TU map it also runs in a *causal*
// mode: -c C cuts the output into self-framed segments every C TUs, and truncating the
// container at any segment boundary yields a wire that decodes standalone to exactly that
// prefix of the corpus. -w W additionally bounds references to the last W TUs.
//
// build: g++ -O3 -march=native -std=c++23 -I<libbsc> -o grz grz.cpp libbsc.a -lzstd -lpthread

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <thread>
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
static void die(const char* m) { fprintf(stderr, "grz: %s\n", m); exit(1); }

static inline u64 rotl(u64 x, int r) { return (x << r) | (x >> ((64 - r) & 63)); }
static inline void putv(std::vector<u8>& v, u64 x) {
    while (x >= 0x80) { v.push_back((u8)(x | 0x80)); x >>= 7; }
    v.push_back((u8)x);
}
static inline u64 getv(const u8*& p) {
    u64 x = 0; int s = 0;
    for (;;) { u8 c = *p++; x |= (u64)(c & 0x7f) << s; if (!(c & 0x80)) break; s += 7; }
    return x;
}
static inline u64 zz(i64 x) { return ((u64)x << 1) ^ (u64)(x >> 63); }
static inline i64 unzz(u64 x) { return (i64)(x >> 1) ^ -(i64)(x & 1); }

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
static Buf slurp(const char* path) {
    Buf b = map_file(path);
    if (!b.n) return b;
    u8* c = (u8*)malloc(b.n);
    if (!c) die("malloc");
    memcpy(c, b.p, b.n);
    munmap(b.p, b.n);
    return {c, b.n};
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

// ------------------------------------------------------------- entropy backends
enum { BE_STORE = 0, BE_Z19 = 1, BE_BSC_E2 = 2, BE_BSC_E1 = 3, BE_BSC_E0 = 4,
       BE_Z12 = 5, BE_Z16 = 6, BE_Z22 = 7 };
static int g_bsc_features = LIBBSC_FEATURE_FASTMODE;
static int g_lzp_hash = LIBBSC_DEFAULT_LZPHASHSIZE;
static int g_lzp_min = LIBBSC_DEFAULT_LZPMINLEN;

static bool be_is_bsc(int be) { return be >= BE_BSC_E2 && be <= BE_BSC_E0; }
static int be_zlevel(int be) {
    switch (be) { case BE_Z12: return 12; case BE_Z16: return 16; case BE_Z22: return 22; default: return 19; }
}
static int be_coder(int be) {
    switch (be) { case BE_BSC_E1: return LIBBSC_CODER_QLFC_STATIC;
                  case BE_BSC_E0: return LIBBSC_CODER_QLFC_FAST;
                  default: return LIBBSC_CODER_QLFC_ADAPTIVE; }
}

static std::vector<u8> be_compress(int be, const u8* p, size_t n, u8* used) {
    std::vector<u8> out;
    if (!n) { *used = BE_STORE; return out; }
    if (be_is_bsc(be)) {
        if (n > (size_t)INT32_MAX - 64) die("stream too large for libbsc");
        out.resize(n + LIBBSC_HEADER_SIZE);
        int r = bsc_compress(p, out.data(), (int)n, g_lzp_hash, g_lzp_min,
                             LIBBSC_BLOCKSORTER_BWT, be_coder(be), g_bsc_features);
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
        int r = bsc_decompress(p, (int)n, out, (int)rawn, g_bsc_features);
        if (r != LIBBSC_NO_ERROR) die("bsc_decompress failed");
        return;
    }
    size_t r = ZSTD_decompress(out, rawn, p, n);
    if (ZSTD_isError(r) || r != rawn) die("ZSTD_decompress failed");
}

// Cheap proxy for "which source encoding entropy-codes smaller": a zstd level-1 pass on each
// candidate, ~100x faster than running the real backend twice.
static int pick_offmode(const std::vector<u8>& a, const std::vector<u8>& b) {
    if (a.empty() || b.empty()) return 0;
    size_t cap = ZSTD_compressBound(a.size() > b.size() ? a.size() : b.size());
    std::vector<u8> t(cap);
    size_t ra = ZSTD_compress(t.data(), t.size(), a.data(), a.size(), 1);
    size_t rb = ZSTD_compress(t.data(), t.size(), b.data(), b.size(), 1);
    if (ZSTD_isError(ra) || ZSTD_isError(rb)) return 0;
    return rb < ra ? 1 : 0;
}

static void compress_literals(int be, const u8* p, size_t n, size_t blkraw, int threads,
                              std::vector<std::vector<u8>>* out, std::vector<u8>* used) {
    size_t nblk = n ? (n + blkraw - 1) / blkraw : 0;
    out->resize(nblk);
    used->assign(nblk, BE_STORE);
    if (!nblk) return;
    auto do_range = [&](size_t lo, size_t hi) {
        for (size_t i = lo; i < hi; i++) {
            size_t off = i * blkraw, len = n - off < blkraw ? n - off : blkraw;
            (*out)[i] = be_compress(be, p + off, len, &(*used)[i]);
        }
    };
    if (threads <= 1 || nblk == 1) { do_range(0, nblk); return; }
    size_t nt = (size_t)threads < nblk ? (size_t)threads : nblk;
    std::vector<std::thread> th;
    size_t per = (nblk + nt - 1) / nt;
    for (size_t t = 0; t < nt; t++) {
        size_t lo = t * per, hi = lo + per < nblk ? lo + per : nblk;
        if (lo >= hi) break;
        th.emplace_back(do_range, lo, hi);
    }
    for (auto& x : th) x.join();
}

// ------------------------------------------------------------- container
// Each segment is fully self-framed and the payloads are written in segment order, so
// truncating the file at a segment boundary leaves a wire that still decodes.
#pragma pack(push, 1)
struct FileHdr {
    char magic[4];      // "GRZ2"
    u32 K, sbits;
    u64 raw;            // full-corpus size (a truncated wire decodes a prefix of this)
    u64 nmatch;
    u32 nseg, ntu;
    u64 blkraw;
    u64 tus_per_seg, win_tus;
};
struct SegHdr {
    u64 out_bytes;      // decoded output bytes contributed by this segment
    u64 tu_end;         // TU index one past the last TU in this segment
    u64 rawsz[4], csz[4];
    u8  be[4], offmode;
    u32 nlitblk;        // followed by nlitblk * { u64 csz; u8 be; }
};
#pragma pack(pop)

// ------------------------------------------------------------- encode
struct EncCfg {
    u32 K = 256, sbits = 5, tbits = 0;
    int litbe = BE_BSC_E2, tokbe = BE_Z19, threads = 1;
    size_t blkraw = 25u << 20;
    u64 tus_per_seg = 0, win_tus = 0;
};

// POS is the hash-table position width: u32 below 4 GiB (half the cache footprint), u64 above.
template <typename POS>
static void encode_impl(const u8* d, u64 n, const std::vector<u64>& tu, const EncCfg& cfg,
                        FILE* f, u64* out_nmatch, double* t_match, double* t_ent) {
    const u32 K = cfg.K, sbits = cfg.sbits;
    u32 tbits = cfg.tbits;
    u64 T[256], Tout[256];
    init_gear(T);
    for (int i = 0; i < 256; i++) Tout[i] = rotl(T[i], (int)(K & 63));

    // Size the anchor index to the expected *anchor* count, not the input: anchors are only
    // planted on novel content, and an oversized table pays a page fault per cold bucket --
    // on the small corpora that cost more than the entire parse.
    if (!tbits) {
        tbits = 16;
        while (tbits < 26 && ((u64)1 << tbits) < (n >> (sbits + 3))) tbits++;
    }
    size_t tsz = ((size_t)1 << tbits) * sizeof(POS);
    POS* tbl = (POS*)mmap(nullptr, tsz, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (tbl == MAP_FAILED) die("mmap table");
    madvise(tbl, tsz, MADV_HUGEPAGE);

    // Segment boundaries in output-byte space. Without a TU map the whole corpus is one segment.
    std::vector<u64> segb;
    segb.push_back(0);
    if (cfg.tus_per_seg && tu.size() > 1) {
        for (u64 t = cfg.tus_per_seg; t < tu.size() - 1; t += cfg.tus_per_seg) segb.push_back(tu[t]);
    }
    segb.push_back(n);
    const size_t nseg = segb.size() - 1;

    std::vector<u8> lits, ll, sd, sd2, ml;
    const u64 amask = (1ull << sbits) - 1;
    u64 nmatch = 0;
    double tm = 0, te = 0;
    u64 cur_tu = 0;

    for (size_t seg = 0; seg < nseg; seg++) {
        const u64 s0 = segb[seg], s1 = segb[seg + 1];
        double a0 = now();
        lits.clear(); ll.clear(); sd.clear(); sd2.clear(); ml.clear();
        u64 lit_start = s0, prev_src_end = 0, prev_off = 0;

        // Reference floor: with -w W, matches may only reach back W TUs.
        u64 win_lo = 0;
        auto refresh_win = [&](u64 p) {
            if (!cfg.win_tus || tu.size() < 2) return;
            while (cur_tu + 1 < tu.size() - 1 && tu[cur_tu + 1] <= p) cur_tu++;
            win_lo = cur_tu >= cfg.win_tus ? tu[cur_tu - cfg.win_tus + 1] : 0;
        };
        refresh_win(s0);

        if (s0 + K <= s1) {
            u64 wp = s0, h = 0;
            for (u32 i = 0; i < K; i++) h = rotl(h, 1) ^ T[d[wp + i]];
            for (;;) {
                bool did_match = false;
                if ((h & amask) == 0) {
                    u32 idx = (u32)((h * 0x9E3779B97F4A7C15ull) >> (64 - tbits));
                    u64 q = (u64)tbl[idx];
                    if (q && q < wp && q >= win_lo && memcmp(d + q, d + wp, K) == 0) {
                        u64 a = wp + K, c = q + K;
                        while (a + 8 <= n) {
                            u64 x, y;
                            memcpy(&x, d + a, 8);
                            memcpy(&y, d + c, 8);
                            if (x != y) { a += (u64)(__builtin_ctzll(x ^ y) >> 3); goto fwd_done; }
                            a += 8; c += 8;
                        }
                        while (a < n && d[a] == d[c]) { a++; c++; }
                    fwd_done:;
                        u64 back = 0;
                        while (wp - back > lit_start && q - back > 0 &&
                               d[wp - back - 1] == d[q - back - 1]) back++;
                        u64 ms = wp - back, src = q - back, len = a - ms;
                        if (len >= K) {
                            // A COPY may not cross a segment boundary, or the segment would
                            // not decode to exactly its own TU range.
                            if (ms + len > s1) len = s1 - ms;
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
                            nmatch++;
                            wp = lit_start;
                            refresh_win(wp);
                            if (wp + K > s1) break;
                            h = 0;
                            for (u32 i = 0; i < K; i++) h = rotl(h, 1) ^ T[d[wp + i]];
                            did_match = true;
                        }
                    }
                    if (!did_match) tbl[idx] = (POS)wp;
                }
                if (!did_match) {
                    if (wp + K >= s1 || wp + K >= n) break;
                    h = rotl(h, 1) ^ Tout[d[wp]] ^ T[d[wp + K]];
                    wp++;
                    if (cfg.win_tus && (wp & 0xFFFF) == 0) refresh_win(wp);
                }
            }
        }
        if (s1 > lit_start) {
            putv(ll, s1 - lit_start);
            lits.insert(lits.end(), d + lit_start, d + s1);
        }
        double a1 = now();
        tm += a1 - a0;

        int offmode = pick_offmode(sd, sd2);
        const std::vector<u8>& srcs = offmode ? sd2 : sd;
        SegHdr sh{};
        sh.out_bytes = s1 - s0;
        sh.tu_end = cfg.tus_per_seg ? (seg + 1 == nseg ? (tu.empty() ? 0 : tu.size() - 1)
                                                       : (seg + 1) * cfg.tus_per_seg) : 0;
        sh.offmode = (u8)offmode;
        std::vector<u8> c0, c2, c3;
        std::vector<std::vector<u8>> lb;
        std::vector<u8> lbe;
        c0 = be_compress(cfg.tokbe, ll.data(), ll.size(), &sh.be[0]);
        compress_literals(cfg.litbe, lits.data(), lits.size(), cfg.blkraw, cfg.threads, &lb, &lbe);
        c2 = be_compress(cfg.tokbe, srcs.data(), srcs.size(), &sh.be[2]);
        c3 = be_compress(cfg.tokbe, ml.data(), ml.size(), &sh.be[3]);
        sh.be[1] = lb.empty() ? BE_STORE : lbe[0];
        sh.nlitblk = (u32)lb.size();
        sh.rawsz[0] = ll.size(); sh.rawsz[1] = lits.size();
        sh.rawsz[2] = srcs.size(); sh.rawsz[3] = ml.size();
        sh.csz[0] = c0.size();
        sh.csz[1] = 0;
        for (auto& x : lb) sh.csz[1] += x.size();
        sh.csz[2] = c2.size(); sh.csz[3] = c3.size();

        fwrite(&sh, sizeof(sh), 1, f);
        for (size_t i = 0; i < lb.size(); i++) {
            u64 z = lb[i].size();
            fwrite(&z, 8, 1, f);
            fwrite(&lbe[i], 1, 1, f);
        }
        if (sh.csz[0]) fwrite(c0.data(), 1, sh.csz[0], f);
        for (auto& x : lb) if (!x.empty()) fwrite(x.data(), 1, x.size(), f);
        if (sh.csz[2]) fwrite(c2.data(), 1, sh.csz[2], f);
        if (sh.csz[3]) fwrite(c3.data(), 1, sh.csz[3], f);
        te += now() - a1;
    }
    munmap(tbl, tsz);
    *out_nmatch = nmatch;
    *t_match = tm;
    *t_ent = te;
}

static std::vector<u64> load_tu(const char* path) {
    std::vector<u64> v;
    if (!path) return v;
    Buf b = slurp(path);
    v.resize(b.n / 8);
    memcpy(v.data(), b.p, v.size() * 8);
    free(b.p);
    return v;
}

static void encode(const char* in, const char* outp, const EncCfg& cfg, const char* tupath) {
    double T0 = now();
    Buf b = map_file(in);
    const u64 n = b.n;
    std::vector<u64> tu = load_tu(tupath);
    if (!tu.empty() && tu.back() != n) die("TU map does not match input size");

    std::vector<u64> segb;
    size_t nseg = 1;
    if (cfg.tus_per_seg && tu.size() > 1)
        nseg = (tu.size() - 1 + cfg.tus_per_seg - 1) / cfg.tus_per_seg;

    FILE* f = fopen(outp, "wb");
    if (!f) die("fopen container");
    FileHdr h{};
    memcpy(h.magic, "GRZ2", 4);
    h.K = cfg.K; h.sbits = cfg.sbits; h.raw = n;
    h.nseg = (u32)nseg; h.ntu = tu.empty() ? 0 : (u32)(tu.size() - 1);
    h.blkraw = cfg.blkraw;
    h.tus_per_seg = cfg.tus_per_seg; h.win_tus = cfg.win_tus;
    fwrite(&h, sizeof(h), 1, f);

    u64 nmatch = 0;
    double tm = 0, te = 0;
    if (n < (1ull << 32)) encode_impl<u32>(b.p, n, tu, cfg, f, &nmatch, &tm, &te);
    else                  encode_impl<u64>(b.p, n, tu, cfg, f, &nmatch, &tm, &te);

    long total = ftell(f);
    h.nmatch = nmatch;
    fseek(f, 0, SEEK_SET);
    fwrite(&h, sizeof(h), 1, f);
    fclose(f);
    double T1 = now();
    double mb = n / 1048576.0;
    fprintf(stderr, "ENC %s raw=%llu out=%ld nseg=%zu matches=%llu match=%.2f(%.0f MB/s) "
                    "entropy=%.2f total=%.2f -> %.1f MB/s\n",
            in, (unsigned long long)n, total, nseg, (unsigned long long)nmatch,
            tm, mb / tm, te, T1 - T0, mb / (T1 - T0));
    printf("%llu\t%ld\t%llu\t%.4f\t%.4f\t%.4f\t%zu\n",
           (unsigned long long)n, total, (unsigned long long)nmatch, tm, te, T1 - T0, nseg);
}

// ------------------------------------------------------------- decode
// Stops cleanly at EOF, so a container truncated at a segment boundary decodes to exactly
// that prefix of the corpus.
static void decode(const char* inp, const char* outp, int threads) {
    double T0 = now();
    Buf cb = slurp(inp);
    if (cb.n < sizeof(FileHdr)) die("truncated container");
    FileHdr h;
    memcpy(&h, cb.p, sizeof(h));
    if (memcmp(h.magic, "GRZ2", 4) != 0) die("bad magic");

    u8* o = (u8*)malloc(h.raw ? h.raw : 1);
    if (!o) die("malloc output");
    const u8* p = cb.p + sizeof(FileHdr);
    const u8* end = cb.p + cb.n;
    u64 pos = 0;
    u32 segs = 0;
    double tent = 0;

    while (p + sizeof(SegHdr) <= end) {
        SegHdr sh;
        memcpy(&sh, p, sizeof(sh));
        const u8* q = p + sizeof(SegHdr);
        std::vector<u64> lz(sh.nlitblk);
        std::vector<u8> lbe(sh.nlitblk);
        if (q + (size_t)sh.nlitblk * 9 > end) break;
        for (u32 i = 0; i < sh.nlitblk; i++) { memcpy(&lz[i], q, 8); q += 8; lbe[i] = *q++; }
        u64 need = sh.csz[0] + sh.csz[1] + sh.csz[2] + sh.csz[3];
        if (q + need > end) break;            // partial trailing segment: stop here
        if (pos + sh.out_bytes > h.raw) die("segment overruns declared size");

        double e0 = now();
        std::vector<u8> s[4];
        for (int i = 0; i < 4; i++) s[i].resize(sh.rawsz[i]);
        be_decompress(sh.be[0], q, sh.csz[0], s[0].data(), sh.rawsz[0]);
        q += sh.csz[0];
        // Literal blocks are independent, so the BWT inverse -- the dominant decode cost --
        // parallelizes even though the COPY replay that follows is strictly sequential.
        struct BlkJob { const u8* src; u64 csz; u8* dst; size_t len; u8 be; };
        std::vector<BlkJob> jobs(sh.nlitblk);
        {
            size_t off = 0;
            for (u32 i = 0; i < sh.nlitblk; i++) {
                size_t len = sh.rawsz[1] - off < h.blkraw ? sh.rawsz[1] - off : (size_t)h.blkraw;
                jobs[i] = {q, lz[i], s[1].data() + off, len, lbe[i]};
                q += lz[i]; off += len;
            }
        }
        auto run_blocks = [&](size_t lo, size_t hi) {
            for (size_t i = lo; i < hi; i++)
                be_decompress(jobs[i].be, jobs[i].src, jobs[i].csz, jobs[i].dst, jobs[i].len);
        };
        if (threads <= 1 || jobs.size() <= 1) {
            run_blocks(0, jobs.size());
        } else {
            size_t nt = (size_t)threads < jobs.size() ? (size_t)threads : jobs.size();
            size_t per = (jobs.size() + nt - 1) / nt;
            std::vector<std::thread> th;
            for (size_t t = 0; t * per < jobs.size(); t++) {
                size_t lo = t * per, hi = lo + per < jobs.size() ? lo + per : jobs.size();
                th.emplace_back(run_blocks, lo, hi);
            }
            for (auto& x : th) x.join();
        }
        be_decompress(sh.be[2], q, sh.csz[2], s[2].data(), sh.rawsz[2]);
        q += sh.csz[2];
        be_decompress(sh.be[3], q, sh.csz[3], s[3].data(), sh.rawsz[3]);
        q += sh.csz[3];
        tent += now() - e0;

        const u8* pll = s[0].data();
        const u8* plit = s[1].data();
        const u8* psd = s[2].data();
        const u8* pml = s[3].data();
        const u64 stop = pos + sh.out_bytes;
        u64 prev_src_end = 0, prev_off = 0;
        while (pos < stop) {
            u64 litlen = getv(pll);
            if (litlen) { memcpy(o + pos, plit, litlen); plit += litlen; pos += litlen; }
            if (pos >= stop) break;
            u64 src, offs;
            if (sh.offmode) { offs = (u64)((i64)prev_off + unzz(getv(psd))); src = pos - offs; }
            else { src = (u64)((i64)prev_src_end + unzz(getv(psd))); offs = pos - src; }
            u64 len = getv(pml);
            if (src >= pos || pos + len > stop) die("corrupt stream");
            if (offs >= len) memcpy(o + pos, o + src, len);
            else { u8* dst = o + pos; const u8* sp = o + src; for (u64 i = 0; i < len; i++) dst[i] = sp[i]; }
            prev_src_end = src + len;
            prev_off = offs;
            pos += len;
        }
        if (pos != stop) die("segment short decode");
        p = q;
        segs++;
    }
    double T1 = now();

    FILE* f = fopen(outp, "wb");
    if (!f) die("fopen output");
    if (pos && fwrite(o, 1, pos, f) != pos) die("fwrite");
    fclose(f);
    double T2 = now();
    double mb = pos / 1048576.0;
    fprintf(stderr, "DEC %s segs=%u/%u bytes=%llu (%.1f%% of full) entropy=%.2f total=%.2f -> %.1f MB/s\n",
            inp, segs, h.nseg, (unsigned long long)pos, h.raw ? 100.0 * pos / h.raw : 0.0,
            tent, T1 - T0, mb / (T1 - T0));
    printf("%llu\t%u\t%.4f\t%.4f\t%.4f\n", (unsigned long long)pos, segs, tent, T1 - T0, T2 - T0);
}

// ------------------------------------------------------------- segment index
// Prints, per segment, the container byte offset just past it and the corpus prefix it
// covers -- i.e. exactly where the wire may be cut and still decode.
static void index_segs(const char* inp) {
    Buf cb = slurp(inp);
    if (cb.n < sizeof(FileHdr)) die("truncated container");
    FileHdr h;
    memcpy(&h, cb.p, sizeof(h));
    if (memcmp(h.magic, "GRZ2", 4) != 0) die("bad magic");
    const u8* p = cb.p + sizeof(FileHdr);
    const u8* end = cb.p + cb.n;
    u64 pos = 0;
    u32 seg = 0;
    printf("seg\tcut_bytes\ttu_end\tout_bytes\tcum_out\n");
    while (p + sizeof(SegHdr) <= end) {
        SegHdr sh;
        memcpy(&sh, p, sizeof(sh));
        const u8* q = p + sizeof(SegHdr) + (size_t)sh.nlitblk * 9;
        u64 need = sh.csz[0] + sh.csz[1] + sh.csz[2] + sh.csz[3];
        if (q + need > end) break;
        q += need;
        pos += sh.out_bytes;
        printf("%u\t%lld\t%llu\t%llu\t%llu\n", seg, (long long)(q - cb.p),
               (unsigned long long)sh.tu_end, (unsigned long long)sh.out_bytes,
               (unsigned long long)pos);
        p = q;
        seg++;
    }
}

// ------------------------------------------------------------- TU map
static void make_tu(const char* manifest, const char* outp) {
    FILE* m = fopen(manifest, "r");
    if (!m) die("open manifest");
    std::vector<u64> off;
    off.push_back(0);
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

// ------------------------------------------------------------- main
int main(int argc, char** argv) {
    if (argc < 3 || (argc < 4 && strcmp(argv[1], "idx"))) {
        fprintf(stderr,
            "usage: grz enc <in> <out.grz> [-K n] [-s sbits] [-t tbits] [-l litbe] [-k tokbe]\n"
            "                              [-b blkMB] [-j threads] [-u tu.map] [-c TUsPerSeg] [-w winTUs]\n"
            "       grz dec <in.grz> <out> [-j threads]   (truncated containers decode to a prefix)\n"
            "       grz tu  <manifest> <out.tu>\n"
            "       grz idx <in.grz>                  (segment cut points)\n"
            "       backends: 1=zstd19 2=bsc-e2 3=bsc-e1 4=bsc-e0 5=zstd12 6=zstd16\n");
        return 1;
    }
    if (!strcmp(argv[1], "tu")) { make_tu(argv[2], argv[3]); return 0; }
    if (!strcmp(argv[1], "idx")) { index_segs(argv[2]); return 0; }
    if (!strcmp(argv[1], "dec")) {
        int jt = 1;
        for (int i = 4; i + 1 < argc; i += 2)
            if (!strcmp(argv[i], "-j")) jt = atoi(argv[i + 1]);
        if (bsc_init(g_bsc_features) != LIBBSC_NO_ERROR) die("bsc_init");
        decode(argv[2], argv[3], jt);
        return 0;
    }
    if (strcmp(argv[1], "enc")) die("unknown mode");

    EncCfg cfg;
    const char* tupath = nullptr;
    for (int i = 4; i + 1 < argc; i += 2) {
        const char* o = argv[i];
        const char* v = argv[i + 1];
        if (!strcmp(o, "-K")) cfg.K = (u32)atoi(v);
        else if (!strcmp(o, "-s")) cfg.sbits = (u32)atoi(v);
        else if (!strcmp(o, "-t")) cfg.tbits = (u32)atoi(v);
        else if (!strcmp(o, "-l")) cfg.litbe = atoi(v);
        else if (!strcmp(o, "-k")) cfg.tokbe = atoi(v);
        else if (!strcmp(o, "-b")) cfg.blkraw = (size_t)atoll(v) << 20;
        else if (!strcmp(o, "-j")) cfg.threads = atoi(v);
        else if (!strcmp(o, "-u")) tupath = v;
        else if (!strcmp(o, "-c")) cfg.tus_per_seg = (u64)atoll(v);
        else if (!strcmp(o, "-w")) cfg.win_tus = (u64)atoll(v);
        else if (!strcmp(o, "-H")) g_lzp_hash = atoi(v);
        else if (!strcmp(o, "-M")) g_lzp_min = atoi(v);
        else if (!strcmp(o, "-m")) { if (atoi(v)) g_bsc_features |= LIBBSC_FEATURE_MULTITHREADING; }
        else die("unknown option");
    }
    if ((cfg.tus_per_seg || cfg.win_tus) && !tupath) die("-c/-w require -u tu.map");
    if (bsc_init(g_bsc_features) != LIBBSC_NO_ERROR) die("bsc_init");
    encode(argv[2], argv[3], cfg, tupath);
    return 0;
}
