// GROUP-RLZ -- fast whole-program long-range LZ codec for preprocessed C++ (.ii) corpora.
//
// Single in-memory pass over the whole file with a content-defined anchor index over the
// *entire* history (no window bound, so cross-TU redundancy is never fragmented). Emits
// COPY(src,len) + ADD(literals) into four separate streams, each handed to its own entropy
// backend (libbsc BWT for the literals, libzstd for the token streams).
//
// Bytes covered by a COPY are never hashed, so on highly redundant corpora the rolling hash
// is only paid on novel content and the parse runs at memcmp speed.
//
// build: g++ -O3 -march=native -std=c++23 -o grz grz.cpp libbsc.a -lzstd
//
//   grz enc <in> <out.grz> [K sbits tbits litbe tokbe mt]
//   grz dec <in.grz> <out>

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
static void spit(const char* path, const void* p, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) die("fopen for write");
    if (n && fwrite(p, 1, n, f) != n) die("fwrite");
    fclose(f);
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
// Backend ids are stored per stream in the container. 1..7 pick a family and a strength;
// only the family matters on the decode side.
enum { BE_STORE = 0, BE_Z19 = 1, BE_BSC_E2 = 2, BE_BSC_E1 = 3, BE_BSC_E0 = 4,
       BE_Z12 = 5, BE_Z16 = 6, BE_Z22 = 7 };
static int g_bsc_features = LIBBSC_FEATURE_FASTMODE;

static bool be_is_bsc(int be) { return be >= BE_BSC_E2 && be <= BE_BSC_E0; }
static int be_zlevel(int be) {
    switch (be) { case BE_Z12: return 12; case BE_Z16: return 16; case BE_Z22: return 22; default: return 19; }
}
static int be_coder(int be) {
    switch (be) { case BE_BSC_E1: return LIBBSC_CODER_QLFC_STATIC;
                  case BE_BSC_E0: return LIBBSC_CODER_QLFC_FAST;
                  default: return LIBBSC_CODER_QLFC_ADAPTIVE; }
}

// LZP is bsc's cheap long-ish-range prefilter ahead of the BWT. Shrinking the BWT input
// speeds up the sort as well as improving the ratio, so these are tuned, not left default.
static int g_lzp_hash = LIBBSC_DEFAULT_LZPHASHSIZE;
static int g_lzp_min = LIBBSC_DEFAULT_LZPMINLEN;

static std::vector<u8> be_compress(int be, const u8* p, size_t n, u8* used) {
    std::vector<u8> out;
    if (!n) { *used = BE_STORE; return out; }
    if (be_is_bsc(be)) {
        if (n > (size_t)INT32_MAX - 64) die("stream too large for libbsc");
        out.resize(n + LIBBSC_HEADER_SIZE);
        int r = bsc_compress(p, out.data(), (int)n, g_lzp_hash, g_lzp_min,
                             LIBBSC_BLOCKSORTER_BWT,
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
        int r = bsc_decompress(p, (int)n, out, (int)rawn, g_bsc_features);
        if (r != LIBBSC_NO_ERROR) die("bsc_decompress failed");
        return;
    }
    size_t r = ZSTD_decompress(out, rawn, p, n);
    if (ZSTD_isError(r) || r != rawn) die("ZSTD_decompress failed");
}

// Cheap proxy for "which source encoding entropy-codes smaller" -- a zstd level-1 pass on
// each candidate, which is ~100x faster than running the real backend twice.
static int pick_offmode(const std::vector<u8>& a, const std::vector<u8>& b) {
    if (a.empty() || b.empty()) return 0;
    std::vector<u8> t(ZSTD_compressBound(a.size() > b.size() ? a.size() : b.size()));
    size_t ra = ZSTD_compress(t.data(), t.size(), a.data(), a.size(), 1);
    size_t rb = ZSTD_compress(t.data(), t.size(), b.data(), b.size(), 1);
    if (ZSTD_isError(ra) || ZSTD_isError(rb)) return 0;
    return rb < ra ? 1 : 0;
}

// ------------------------------------------------------------- container
// The literal stream is cut into independent blocks: a BWT over a 25 MB block is ~1.35x
// faster per byte than over a 120 MB one (for ~3% of ratio), and independent blocks are
// what makes the optional threaded mode possible.
#pragma pack(push, 1)
struct Hdr {
    char magic[4];      // "GRZ1"
    u8 offmode;         // 0 = src-delta, 1 = offset-delta
    u8 be[4];           // backend id per stream: ll, lit, sd, ml
    u32 K, sbits;
    u64 raw, nmatch;
    u64 rawsz[4], csz[4];
    u32 nblk;           // literal blocks; per-block compressed sizes follow the header
    u64 blkraw;         // uncompressed bytes per literal block (last block may be short)
};
#pragma pack(pop)

// Compress one literal stream into nblk independently-coded blocks, optionally in parallel.
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

// ------------------------------------------------------------- encode
static void encode(const char* in, const char* outp, u32 K, u32 sbits, u32 tbits,
                   int litbe, int tokbe, size_t blkraw, int threads) {
    double T0 = now();
    Buf b = map_file(in);
    const u8* d = b.p;
    const u64 n = b.n;
    if (n >= (1ull << 32)) die("input >= 4GiB not supported");

    u64 T[256], Tout[256];
    init_gear(T);
    for (int i = 0; i < 256; i++) Tout[i] = rotl(T[i], (int)(K & 63));

    // Anchor index. Sized to the anchor budget (n >> sbits) rather than a fixed constant:
    // an oversized table costs a page fault per distinct bucket touched, which on the small
    // corpora dominated the whole parse. Huge pages cut the remaining fault count 512x.
    // Size to the expected *anchor* count, not the input: anchors are only planted on novel
    // content (a few percent of a redundant corpus), and an oversized table pays a page fault
    // per cold bucket -- which on the small corpora cost more than the whole parse.
    if (!tbits) {
        tbits = 16;
        while (tbits < 24 && ((u64)1 << tbits) < (n >> (sbits + 3))) tbits++;
    }
    size_t tsz = ((size_t)1 << tbits) * sizeof(u32);
    u32* tbl = (u32*)mmap(nullptr, tsz, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (tbl == MAP_FAILED) die("mmap table");
    madvise(tbl, tsz, MADV_HUGEPAGE);

    std::vector<u8> lits, ll, sd, sd2, ml;
    lits.reserve(n / 16 + 4096);

    const u64 amask = (1ull << sbits) - 1;
    u64 lit_start = 0, prev_src_end = 0, prev_off = 0, nmatch = 0, mbytes = 0;

    double T1 = now();
    if (n > K) {
        u64 wp = 0, h = 0;
        for (u32 i = 0; i < K; i++) h = rotl(h, 1) ^ T[d[i]];
        for (;;) {
            bool did_match = false;
            if ((h & amask) == 0) {
                u32 idx = (u32)((h * 0x9E3779B97F4A7C15ull) >> (64 - tbits));
                u32 q = tbl[idx];
                if (q && (u64)q < wp && memcmp(d + q, d + wp, K) == 0) {
                    u64 a = wp + K, c = (u64)q + K;
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
                    while (wp - back > lit_start && (u64)q - back > 0 &&
                           d[wp - back - 1] == d[(u64)q - back - 1]) back++;
                    u64 ms = wp - back, src = (u64)q - back, len = a - ms;
                    if (len >= K) {
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
                        nmatch++; mbytes += len;
                        wp = lit_start;
                        if (wp + K > n) break;
                        h = 0;
                        for (u32 i = 0; i < K; i++) h = rotl(h, 1) ^ T[d[wp + i]];
                        did_match = true;
                    }
                }
                if (!did_match) tbl[idx] = (u32)wp;
            }
            if (!did_match) {
                if (wp + K >= n) break;
                h = rotl(h, 1) ^ Tout[d[wp]] ^ T[d[wp + K]];
                wp++;
            }
        }
    }
    if (n > lit_start) {
        putv(ll, n - lit_start);
        lits.insert(lits.end(), d + lit_start, d + n);
    }
    double T2 = now();
    munmap(tbl, tsz);

    int offmode = pick_offmode(sd, sd2);
    const std::vector<u8>& src_stream = offmode ? sd2 : sd;

    Hdr h{};
    memcpy(h.magic, "GRZ1", 4);
    h.offmode = (u8)offmode;
    h.K = K; h.sbits = sbits; h.raw = n; h.nmatch = nmatch;

    std::vector<u8> c[4];
    std::vector<std::vector<u8>> litblk;
    std::vector<u8> litused;
    c[0] = be_compress(tokbe, ll.data(), ll.size(), &h.be[0]);
    compress_literals(litbe, lits.data(), lits.size(), blkraw, threads, &litblk, &litused);
    c[2] = be_compress(tokbe, src_stream.data(), src_stream.size(), &h.be[2]);
    c[3] = be_compress(tokbe, ml.data(), ml.size(), &h.be[3]);
    h.nblk = (u32)litblk.size();
    h.blkraw = blkraw;
    h.be[1] = litblk.empty() ? BE_STORE : litused[0];
    h.rawsz[0] = ll.size();
    h.rawsz[1] = lits.size();
    h.rawsz[2] = offmode ? sd2.size() : sd.size();
    h.rawsz[3] = ml.size();
    h.csz[0] = c[0].size();
    h.csz[1] = 0;
    for (auto& blk : litblk) h.csz[1] += blk.size();
    h.csz[2] = c[2].size();
    h.csz[3] = c[3].size();
    double T3 = now();

    FILE* f = fopen(outp, "wb");
    if (!f) die("fopen container");
    fwrite(&h, sizeof(h), 1, f);
    for (size_t i = 0; i < litblk.size(); i++) {
        u64 sz = litblk[i].size();
        u8 be = litused[i];
        fwrite(&sz, 8, 1, f);
        fwrite(&be, 1, 1, f);
    }
    fwrite(c[0].data(), 1, h.csz[0], f);
    for (auto& blk : litblk) if (!blk.empty()) fwrite(blk.data(), 1, blk.size(), f);
    fwrite(c[2].data(), 1, h.csz[2], f);
    fwrite(c[3].data(), 1, h.csz[3], f);
    long total = ftell(f);
    fclose(f);
    double T4 = now();

    double mb = n / 1048576.0;
    fprintf(stderr,
            "ENC %s raw=%llu out=%ld matches=%llu cov=%.3f%% lit=%zu(%.3f%%) "
            "map=%.2f match=%.2f(%.0f MB/s) entropy=%.2f write=%.2f total=%.2f -> %.1f MB/s\n",
            in, (unsigned long long)n, total, (unsigned long long)nmatch,
            n ? 100.0 * mbytes / n : 0.0, lits.size(), n ? 100.0 * lits.size() / n : 0.0,
            T1 - T0, T2 - T1, mb / (T2 - T1), T3 - T2, T4 - T3, T4 - T0, mb / (T4 - T0));
    printf("%llu\t%ld\t%llu\t%zu\t%.4f\t%.4f\t%.4f\t%.4f\t%llu\t%llu\t%llu\t%llu\t%d\n",
           (unsigned long long)n, total, (unsigned long long)nmatch, lits.size(),
           T1 - T0, T2 - T1, T3 - T2, T4 - T0,
           (unsigned long long)h.csz[0], (unsigned long long)h.csz[1],
           (unsigned long long)h.csz[2], (unsigned long long)h.csz[3], offmode);
}

// ------------------------------------------------------------- decode
static void decode(const char* inp, const char* outp) {
    double T0 = now();
    Buf cb = slurp(inp);
    if (cb.n < sizeof(Hdr)) die("truncated container");
    Hdr h;
    memcpy(&h, cb.p, sizeof(h));
    if (memcmp(h.magic, "GRZ1", 4) != 0) die("bad magic");

    const u8* p = cb.p + sizeof(Hdr);
    std::vector<u64> blksz(h.nblk);
    std::vector<u8> blkbe(h.nblk);
    for (u32 i = 0; i < h.nblk; i++) {
        memcpy(&blksz[i], p, 8); p += 8;
        blkbe[i] = *p++;
    }
    std::vector<u8> s[4];
    for (int i = 0; i < 4; i++) {
        if ((size_t)(p - cb.p) + h.csz[i] > cb.n) die("truncated payload");
        s[i].resize(h.rawsz[i]);
        if (i == 1) {
            size_t off = 0;
            for (u32 j = 0; j < h.nblk; j++) {
                size_t len = h.rawsz[1] - off < h.blkraw ? h.rawsz[1] - off : h.blkraw;
                be_decompress(blkbe[j], p, blksz[j], s[1].data() + off, len);
                p += blksz[j];
                off += len;
            }
            if (off != h.rawsz[1]) die("literal block size mismatch");
        } else {
            be_decompress(h.be[i], p, h.csz[i], s[i].data(), h.rawsz[i]);
            p += h.csz[i];
        }
    }
    double T1 = now();

    const u64 n = h.raw;
    u8* o = (u8*)malloc(n ? n : 1);
    if (!o) die("malloc output");
    const u8* pll = s[0].data();
    const u8* plit = s[1].data();
    const u8* psd = s[2].data();
    const u8* pml = s[3].data();

    u64 pos = 0, prev_src_end = 0, prev_off = 0;
    while (pos < n) {
        u64 litlen = getv(pll);
        if (litlen) { memcpy(o + pos, plit, litlen); plit += litlen; pos += litlen; }
        if (pos >= n) break;
        u64 src, off;
        if (h.offmode) {
            off = (u64)((i64)prev_off + unzz(getv(psd)));
            src = pos - off;
        } else {
            src = (u64)((i64)prev_src_end + unzz(getv(psd)));
            off = pos - src;
        }
        u64 len = getv(pml);
        if (src >= pos || pos + len > n) die("corrupt stream");
        if (off >= len) {
            memcpy(o + pos, o + src, len);
        } else {
            u8* dst = o + pos;
            const u8* sp = o + src;
            for (u64 i = 0; i < len; i++) dst[i] = sp[i];
        }
        prev_src_end = src + len;
        prev_off = off;
        pos += len;
    }
    if (pos != n) die("short decode");
    double T2 = now();
    spit(outp, o, n);
    double T3 = now();

    double mb = n / 1048576.0;
    fprintf(stderr, "DEC %s raw=%llu entropy=%.2f copy=%.2f(%.0f MB/s) write=%.2f total=%.2f -> %.1f MB/s\n",
            inp, (unsigned long long)n, T1 - T0, T2 - T1, mb / (T2 - T1), T3 - T2, T3 - T0, mb / (T3 - T0));
    printf("%.4f\t%.4f\t%.4f\t%.4f\n", T1 - T0, T2 - T1, T3 - T2, T3 - T0);
}

// ------------------------------------------------------------- main
int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: grz enc <in> <out.grz> [K sbits tbits litbe tokbe mt lzpH lzpM blkMB threads]\n"
                        "       grz dec <in.grz> <out>          (be: 1=zstd19 2=bsc)\n");
        return 1;
    }
    if (!strcmp(argv[1], "enc")) {
        u32 K = argc > 4 ? (u32)atoi(argv[4]) : 128;
        u32 sbits = argc > 5 ? (u32)atoi(argv[5]) : 8;
        u32 tbits = argc > 6 ? (u32)atoi(argv[6]) : 0;   // 0 = auto-size
        int litbe = argc > 7 ? atoi(argv[7]) : BE_BSC_E2;
        int tokbe = argc > 8 ? atoi(argv[8]) : BE_Z19;
        if (argc > 9 && atoi(argv[9])) g_bsc_features |= LIBBSC_FEATURE_MULTITHREADING;
        if (argc > 10) g_lzp_hash = atoi(argv[10]);
        if (argc > 11) g_lzp_min = atoi(argv[11]);
        if (bsc_init(g_bsc_features) != LIBBSC_NO_ERROR) die("bsc_init");
        size_t blkraw = (size_t)(argc > 12 ? atoi(argv[12]) : 25) * 1048576;
        int threads = argc > 13 ? atoi(argv[13]) : 1;
        encode(argv[2], argv[3], K, sbits, tbits, litbe, tokbe, blkraw, threads);
    } else if (!strcmp(argv[1], "dec")) {
        if (argc > 4 && atoi(argv[4])) g_bsc_features |= LIBBSC_FEATURE_MULTITHREADING;
        if (bsc_init(g_bsc_features) != LIBBSC_NO_ERROR) die("bsc_init");
        decode(argv[2], argv[3]);
    } else {
        die("unknown mode");
    }
    return 0;
}
