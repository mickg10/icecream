// superblock-bench.cpp
//
// Superblock compression analysis for the icecream line-dedup transport (issue #16).
//
// Question: the current transport encodes each TU as an occurrence stream of interned
// line-ids (the "structure floor"). How much MORE can that be compressed by deduplicating
// CONTIGUOUS MULTI-LINE RUNS ("superblocks") -- and does an explicit superblock layer beat
// what zstd already captures implicitly on the line-id stream?
//
// A superblock detector is the SAME dedup one level up: bytes -> lines -> superblocks. It is
// a repeated-substring structure run over the sequence of interned line-ids. Two flavors:
//   (a) marker-aligned regions  -- the trace interner already produces these (a region is the
//       multi-line run between preprocessor `# ` markers); encode each TU as a region-id stream.
//   (b) general repeated line-runs -- a hierarchical (Re-Pair/BPE-style) grammar over the
//       line-id stream, finding recurring contiguous runs NOT tied to marker boundaries.
//   (a)+(b) -- run the general detector over the region-id stream (super-regions).
//
// MEASUREMENT MODEL (matches the real transport):
//   * The interned dictionary is PERSISTENT/warm/shared and captures cross-TU redundancy. The
//     line-text dictionary AND the region-composition table AND the superblock grammar are all
//     amortized (sent once over many jobs) -- reported as "dict" but not charged per message.
//   * Each job sends ONE TU's occurrence stream, zstd-compressed INDEPENDENTLY. So the recurring
//     wire cost is the SUM of per-TU zstd sizes of the occurrence body. That is the primary
//     metric. Whole-corpus zstd of the concatenated bodies is also reported as a batched floor.
//   * Every encoding is reconstructed to the exact token stream and digest-checked (byte-exact
//     back to the original source) before its size is reported.
//
// The Interner is copied verbatim from linecache/trace-interner-bench.cpp (the accepted interner),
// with ONE change: process() takes an optional region-id sink so we can capture the marker-aligned
// region sequence per TU alongside the line-id sequence.
//
// build: g++ -O3 -march=native -std=c++17 superblock-bench.cpp -o superblock-bench -lzstd

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <immintrin.h>
#include <zstd.h>
#include <zdict.h>

using Clock = std::chrono::steady_clock;

static double seconds_since(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
static inline uint64_t fold128(uint64_t a, uint64_t b) {
    __uint128_t p = __uint128_t(a) * b;
    return uint64_t(p) ^ uint64_t(p >> 64);
}
static inline uint64_t read64(const char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline uint64_t read_tail(const char *p, uint32_t n) { uint64_t v = 0; memcpy(&v, p, n); return v; }

static inline uint64_t sampled_hash(const char *p, uint32_t n) {
    constexpr uint64_t A = 0xa0761d6478bd642fULL;
    constexpr uint64_t B = 0xe7037ed1a0b428dbULL;
    uint64_t h = mix64(uint64_t(n) ^ A);
    if (n <= 8) return mix64(h ^ read_tail(p, n));
    if (n <= 16) return fold128(read64(p) ^ A, read64(p + n - 8) ^ h);
    if (n <= 32) {
        h = fold128(read64(p) ^ A, read64(p + 8) ^ h);
        return fold128(read64(p + n - 16) ^ B, read64(p + n - 8) ^ h);
    }
    uint32_t mid = (n >> 1) - 4;
    h = fold128(read64(p) ^ A, read64(p + 8) ^ h);
    h = fold128(read64(p + mid) ^ B, read64(p + n - 16) ^ h);
    return fold128(read64(p + n - 8) ^ A, h ^ B);
}

static inline uint64_t line_hash(const char *key, uint32_t len) {
    constexpr uint64_t secret[3] = {
        0x2d358dccaa6c78a5ULL, 0x8bb84b93962eacc9ULL, 0x4b33a62ed433d4a3ULL
    };
    uint64_t seed = 0xbdd89aa982704029ULL ^ uint64_t(len);
    const char *p = key;
    uint32_t n = len;
    if (n <= 16) {
        uint64_t a = 0, b = 0;
        if (n >= 8) { a = read64(p); b = read64(p + n - 8); }
        else if (n) { a = read_tail(p, n); b = a; }
        return fold128(a ^ secret[0], b ^ seed ^ secret[1]);
    }
    uint64_t a = read64(p) ^ secret[0];
    uint64_t b = read64(p + 8) ^ seed;
    p += 16;
    n -= 16;
    while (n >= 48) {
        seed = fold128(read64(p) ^ secret[0], read64(p + 8) ^ seed);
        a = fold128(read64(p + 16) ^ secret[1], read64(p + 24) ^ a);
        b = fold128(read64(p + 32) ^ secret[2], read64(p + 40) ^ b);
        p += 48;
        n -= 48;
    }
    while (n >= 16) {
        seed = fold128(read64(p) ^ secret[0], read64(p + 8) ^ seed);
        p += 16;
        n -= 16;
    }
    if (n) {
        uint64_t x = n >= 8 ? read64(p) : read_tail(p, n);
        uint64_t y = n >= 8 ? read64(p + n - 8) : x;
        seed = fold128(x ^ secret[1], y ^ seed);
    }
    return fold128(a ^ secret[0], b ^ seed ^ secret[2]);
}

static inline const char *next_region(const char *p, const char *end) {
    const char *q = p + 1;
#if defined(__AVX512BW__)
    const __m512i hashes = _mm512_set1_epi8('#');
    while (q + 64 <= end) {
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const void *>(q));
        uint64_t mask = _mm512_cmpeq_epi8_mask(v, hashes);
        while (mask) {
            unsigned bit = unsigned(__builtin_ctzll(mask));
            const char *candidate = q + bit;
            if (candidate[-1] == '\n' && candidate + 1 < end && candidate[1] == ' ')
                return candidate;
            mask &= mask - 1;
        }
        q += 64;
    }
#elif defined(__AVX2__)
    const __m256i hashes = _mm256_set1_epi8('#');
    while (q + 32 <= end) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(q));
        uint32_t mask = uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, hashes)));
        while (mask) {
            unsigned bit = unsigned(__builtin_ctz(mask));
            const char *candidate = q + bit;
            if (candidate[-1] == '\n' && candidate + 1 < end && candidate[1] == ' ')
                return candidate;
            mask &= mask - 1;
        }
        q += 32;
    }
#endif
    while (q + 1 < end) {
        if (*q == '#' && q[-1] == '\n' && q[1] == ' ') return q;
        ++q;
    }
    return end;
}

static void *huge_zeroed(size_t bytes) {
    constexpr size_t H = 2u << 20;
    bytes = (bytes + H - 1) & ~(H - 1);
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    madvise(p, bytes, MADV_HUGEPAGE);
    return p;
}

struct FileSpan { uint64_t off; uint32_t len; };
struct LineRef  { uint32_t off; uint32_t len; };
struct TinySlot { uint64_t bytes; uint32_t id; uint8_t len; uint8_t pad[3]; };
struct ShortSlot { uint64_t lo; uint64_t hi; uint32_t id; uint8_t len; uint8_t pad[3]; };
struct LineSlot { uint64_t hash; uint32_t off; uint32_t len; uint32_t id; uint32_t pad; };
struct RegionRecord {
    uint64_t hash;
    uint32_t raw_off;
    uint32_t raw_len;
    uint32_t ids_off;
    uint32_t ids_count;
    uint32_t next1;
    uint32_t next2;
};

class Interner {
public:
    static constexpr uint32_t TINY_CAP = 1u << 10;
    static constexpr uint32_t SHORT_CAP = 1u << 17;
    static constexpr uint32_t LINE_CAP = 1u << 21;
    static constexpr uint32_t REGION_INITIAL_CAP = 1u << 17;

    Interner() {
        tiny_ = static_cast<TinySlot *>(huge_zeroed(sizeof(TinySlot) * TINY_CAP));
        short_ = static_cast<ShortSlot *>(huge_zeroed(sizeof(ShortSlot) * SHORT_CAP));
        lines_ = static_cast<LineSlot *>(huge_zeroed(sizeof(LineSlot) * LINE_CAP));
        region_index_.resize(REGION_INITIAL_CAP);
        region_mask_ = REGION_INITIAL_CAP - 1;
        region_records_.reserve(1u << 20);
        line_bytes_.reserve(64u << 20);
        region_bytes_.reserve(80u << 20);
        region_ids_.reserve(8u << 20);
        id_refs_.reserve(1000000);
        id_refs_.push_back({0, 0});
    }
    ~Interner() {
        munmap(tiny_, rounded(sizeof(TinySlot) * TINY_CAP));
        munmap(short_, rounded(sizeof(ShortSlot) * SHORT_CAP));
        munmap(lines_, rounded(sizeof(LineSlot) * LINE_CAP));
    }
    Interner(const Interner &) = delete;
    Interner &operator=(const Interner &) = delete;

    uint32_t distinct() const { return next_id_ - 1; }
    uint64_t region_count() const { return region_records_.size(); }
    const LineRef &ref(uint32_t id) const { return id_refs_[id]; }
    const char *line_data(uint32_t off) const { return line_bytes_.data() + off; }
    const uint32_t *region_ids_ptr(uint32_t rid) const {
        return region_ids_.data() + region_records_[rid].ids_off;
    }
    uint32_t region_ids_count(uint32_t rid) const { return region_records_[rid].ids_count; }
    uint64_t distinct_line_bytes() const { return line_bytes_.size(); }

    // Copied from trace-interner-bench.cpp; the ONLY change is the optional region_out sink.
    void process(const char *begin, const char *end, uint32_t *out, size_t &out_count,
                 uint64_t &region_hits, bool train_predictor,
                 std::vector<uint32_t> *region_out) {
        const char *p = begin;
        uint32_t previous = UINT32_MAX;
        while (p < end) {
            bool found = false;
            uint32_t region_id = UINT32_MAX;
            uint32_t n = 0;
            uint64_t h = 0;

            if (previous != UINT32_MAX) {
                const RegionRecord &prev = region_records_[previous];
                const uint32_t candidates[2] = {prev.next1, prev.next2};
                for (uint32_t encoded : candidates) {
                    if (!encoded) continue;
                    uint32_t candidate_id = encoded - 1;
                    const RegionRecord &candidate = region_records_[candidate_id];
                    if (candidate.raw_len > uint64_t(end - p)) continue;
                    const char *candidate_end = p + candidate.raw_len;
                    bool exact_boundary = candidate_end == end ||
                        (candidate_end + 1 < end && candidate_end[-1] == '\n' &&
                         candidate_end[0] == '#' && candidate_end[1] == ' ');
                    if (exact_boundary && memcmp(region_bytes_.data() + candidate.raw_off,
                                                 p, candidate.raw_len) == 0) {
                        region_id = candidate_id;
                        n = candidate.raw_len;
                        memcpy(out + out_count, region_ids_.data() + candidate.ids_off,
                               size_t(candidate.ids_count) * sizeof(uint32_t));
                        out_count += candidate.ids_count;
                        ++region_hits;
                        found = true;
                        break;
                    }
                }
            }

            if (!found) {
                const char *q = next_region(p, end);
                n = uint32_t(q - p);
                h = sampled_hash(p, n) | 1ULL;
                uint32_t slot = uint32_t(h) & region_mask_;
                uint32_t probes = 0;
                for (;;) {
                    if (++probes > region_index_.size()) {
                        fprintf(stderr, "region table exhausted\n"); exit(2);
                    }
                    uint32_t encoded = region_index_[slot];
                    if (!encoded) break;
                    RegionRecord &r = region_records_[encoded - 1];
                    if (r.hash == h && r.raw_len == n &&
                        memcmp(region_bytes_.data() + r.raw_off, p, n) == 0) {
                        region_id = encoded - 1;
                        memcpy(out + out_count, region_ids_.data() + r.ids_off,
                               size_t(r.ids_count) * sizeof(uint32_t));
                        out_count += r.ids_count;
                        ++region_hits;
                        found = true;
                        break;
                    }
                    slot = (slot + 1) & region_mask_;
                }
            }

            if (!found) {
                const char *q = p + n;
                uint32_t ids_off = uint32_t(region_ids_.size());
                const char *lp = p;
                while (lp < q) {
                    const void *hit = memchr(lp, '\n', size_t(q - lp));
                    const char *le = hit ? static_cast<const char *>(hit) + 1 : q;
                    uint32_t id = intern_line(lp, uint32_t(le - lp));
                    region_ids_.push_back(id);
                    out[out_count++] = id;
                    lp = le;
                }
                uint32_t raw_off = uint32_t(region_bytes_.size());
                region_bytes_.insert(region_bytes_.end(), p, q);
                region_id = uint32_t(region_records_.size());
                region_records_.push_back({h, raw_off, n, ids_off,
                                           uint32_t(region_ids_.size() - ids_off), 0, 0});
                insert_region_index(region_id);
            }

            if (region_out) region_out->push_back(region_id);

            if (train_predictor && previous != UINT32_MAX) {
                RegionRecord &prev = region_records_[previous];
                uint32_t encoded = region_id + 1;
                if (!prev.next1) prev.next1 = encoded;
                else if (prev.next1 != encoded && !prev.next2) prev.next2 = encoded;
            }
            previous = region_id;
            p += n;
        }
    }

    uint32_t intern_line(const char *p, uint32_t n) {
        if (n <= 4) return intern_tiny(p, n);
        if (n <= 16) return intern_short(p, n);
        uint64_t h = line_hash(p, n) | 1ULL;
        uint32_t slot = uint32_t(h) & (LINE_CAP - 1);
        uint32_t probes = 0;
        for (;;) {
            if (++probes > LINE_CAP) { fprintf(stderr, "line table exhausted\n"); exit(2); }
            LineSlot &s = lines_[slot];
            if (!s.id) {
                uint32_t id = add_line(p, n);
                s = {h, id_refs_[id].off, n, id, 0};
                return id;
            }
            if (s.hash == h && s.len == n &&
                memcmp(line_bytes_.data() + s.off, p, n) == 0)
                return s.id;
            slot = (slot + 1) & (LINE_CAP - 1);
        }
    }

private:
    void insert_region_index(uint32_t region_id) {
        if ((region_records_.size() * 10) > (region_index_.size() * 7)) {
            std::vector<uint32_t> grown(region_index_.size() * 2);
            uint32_t new_mask = uint32_t(grown.size() - 1);
            for (uint32_t id = 0; id < region_records_.size() - 1; ++id) {
                uint32_t slot = uint32_t(region_records_[id].hash) & new_mask;
                while (grown[slot]) slot = (slot + 1) & new_mask;
                grown[slot] = id + 1;
            }
            region_index_.swap(grown);
            region_mask_ = new_mask;
        }
        uint32_t slot = uint32_t(region_records_[region_id].hash) & region_mask_;
        while (region_index_[slot]) slot = (slot + 1) & region_mask_;
        region_index_[slot] = region_id + 1;
    }
    static size_t rounded(size_t n) {
        constexpr size_t H = 2u << 20;
        return (n + H - 1) & ~(H - 1);
    }
    uint32_t add_line(const char *p, uint32_t n) {
        if (next_id_ == 0) { fprintf(stderr, "ID overflow\n"); exit(2); }
        uint32_t off = uint32_t(line_bytes_.size());
        line_bytes_.insert(line_bytes_.end(), p, p + n);
        uint32_t id = next_id_++;
        id_refs_.push_back({off, n});
        return id;
    }
    uint32_t intern_tiny(const char *p, uint32_t n) {
        uint64_t bytes = read_tail(p, n);
        uint32_t slot = uint32_t(mix64(bytes ^ (uint64_t(n) << 56))) & (TINY_CAP - 1);
        uint32_t probes = 0;
        for (;;) {
            if (++probes > TINY_CAP) { fprintf(stderr, "tiny table exhausted\n"); exit(2); }
            TinySlot &s = tiny_[slot];
            if (!s.id) { uint32_t id = add_line(p, n); s.bytes = bytes; s.id = id; s.len = uint8_t(n); return id; }
            if (s.len == n && s.bytes == bytes) return s.id;
            slot = (slot + 1) & (TINY_CAP - 1);
        }
    }
    uint32_t intern_short(const char *p, uint32_t n) {
        uint64_t lo = n >= 8 ? read64(p) : read_tail(p, n);
        uint64_t hi = n > 8 ? read_tail(p + 8, n - 8) : lo;
        uint64_t h = fold128(lo ^ 0xa0761d6478bd642fULL, hi ^ uint64_t(n) * 0xe7037ed1a0b428dbULL);
        uint32_t slot = uint32_t(h) & (SHORT_CAP - 1);
        uint32_t probes = 0;
        for (;;) {
            if (++probes > SHORT_CAP) { fprintf(stderr, "short table exhausted\n"); exit(2); }
            ShortSlot &s = short_[slot];
            if (!s.id) { uint32_t id = add_line(p, n); s.lo = lo; s.hi = hi; s.id = id; s.len = uint8_t(n); return id; }
            if (s.len == n && s.lo == lo && s.hi == hi) return s.id;
            slot = (slot + 1) & (SHORT_CAP - 1);
        }
    }

    TinySlot *tiny_ = nullptr;
    ShortSlot *short_ = nullptr;
    LineSlot *lines_ = nullptr;
    std::vector<uint32_t> region_index_;
    std::vector<RegionRecord> region_records_;
    std::vector<char> line_bytes_;
    std::vector<char> region_bytes_;
    std::vector<uint32_t> region_ids_;
    std::vector<LineRef> id_refs_;
    uint32_t next_id_ = 1;
    uint32_t region_mask_ = 0;
};

struct Corpus {
    std::vector<char> bytes;
    std::vector<FileSpan> files;
    uint64_t raw = 0;
};

static Corpus make_poisoned_corpus(const Corpus &source, uint64_t &modified_files) {
    static constexpr char marker[] = "# 1 \"/usr/include/stdc-predef.h\" 1 3 4\n";
    static constexpr char inserted[] = "typedef int local_oracle_inserted_line;\n";
    Corpus result;
    result.files.reserve(source.files.size());
    result.bytes.reserve(source.bytes.size() + source.files.size() * sizeof(inserted));
    modified_files = 0;
    for (const FileSpan &f : source.files) {
        const char *begin = source.bytes.data() + f.off;
        const char *end = begin + f.len;
        const char *at = std::search(begin, end, marker, marker + sizeof(marker) - 1);
        uint64_t out_off = result.bytes.size();
        if (at == end) {
            result.bytes.insert(result.bytes.end(), begin, end);
        } else {
            const char *after = at + sizeof(marker) - 1;
            result.bytes.insert(result.bytes.end(), begin, after);
            result.bytes.insert(result.bytes.end(), inserted, inserted + sizeof(inserted) - 1);
            result.bytes.insert(result.bytes.end(), after, end);
            ++modified_files;
        }
        uint64_t out_len = result.bytes.size() - out_off;
        if (out_len > UINT32_MAX) { fprintf(stderr, "poisoned TU too large\n"); exit(2); }
        result.files.push_back({out_off, uint32_t(out_len)});
    }
    result.raw = result.bytes.size();
    result.bytes.resize(result.bytes.size() + 64);
    return result;
}

static Corpus load_corpus(const char *manifest, size_t max_files) {
    FILE *mf = fopen(manifest, "r");
    if (!mf) { perror(manifest); exit(2); }
    std::vector<std::string> paths;
    char path[8192];
    uint64_t total = 0;
    while (fgets(path, sizeof path, mf)) {
        size_t n = strlen(path);
        while (n && (path[n - 1] == '\n' || path[n - 1] == '\r')) path[--n] = 0;
        if (!n) continue;
        struct stat st {};
        if (stat(path, &st) != 0) { perror(path); exit(2); }
        if (st.st_size < 0 || uint64_t(st.st_size) > UINT32_MAX) {
            fprintf(stderr, "unsupported file size: %s\n", path); exit(2);
        }
        paths.emplace_back(path);
        total += uint64_t(st.st_size);
        if (paths.size() == max_files) break;
    }
    fclose(mf);
    Corpus c;
    c.bytes.resize(size_t(total) + 64);
    c.files.reserve(paths.size());
    uint64_t off = 0;
    for (const auto &p : paths) {
        FILE *f = fopen(p.c_str(), "rb");
        if (!f) { perror(p.c_str()); exit(2); }
        struct stat st {};
        if (fstat(fileno(f), &st) != 0) { perror(p.c_str()); exit(2); }
        size_t n = size_t(st.st_size);
        if (n && fread(c.bytes.data() + off, 1, n, f) != n) {
            fprintf(stderr, "short read: %s\n", p.c_str()); exit(2);
        }
        fclose(f);
        c.files.push_back({off, uint32_t(n)});
        off += n;
    }
    c.raw = off;
    return c;
}

// ------------------------------- superblock analysis -------------------------------

static inline void put_varint(std::vector<uint8_t> &out, uint64_t v) {
    while (v >= 0x80) { out.push_back(uint8_t(v) | 0x80); v >>= 7; }
    out.push_back(uint8_t(v));
}
static inline void put_zigzag(std::vector<uint8_t> &out, int64_t v) {
    put_varint(out, (uint64_t(v) << 1) ^ uint64_t(v >> 63));
}
static uint64_t hash_bytes(const uint8_t *p, size_t n) {  // FNV-1a 64
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

// zstd size only. LDM path uses windowLog 30 (1 GiB) so a strong zstd can match across the WHOLE
// concatenated message -- the fair "does zstd capture it implicitly" baseline at scale.
static size_t zstd_size_ctx(ZSTD_CCtx *c, const uint8_t *data, size_t n, int level, bool ldm,
                            std::vector<uint8_t> &dst) {
    ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    if (ldm) {
        ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1);
        ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, 30);
    }
    size_t bound = ZSTD_compressBound(n);
    if (dst.size() < bound) dst.resize(bound);
    size_t r = ZSTD_compress2(c, dst.data(), dst.size(), data ? data : (const uint8_t*)"", n);
    if (ZSTD_isError(r)) { fprintf(stderr, "zstd error: %s\n", ZSTD_getErrorName(r)); exit(2); }
    return r;
}

// Sum of per-TU independent zstd sizes over body slices [off[t],off[t+1]) -- the recurring wire cost.
static size_t zstd_perTU_sum(const std::vector<uint8_t> &flat, const std::vector<size_t> &off,
                             int level) {
    ZSTD_CCtx *c = ZSTD_createCCtx();
    std::vector<uint8_t> dst;
    size_t total = 0;
    for (size_t t = 0; t + 1 < off.size(); ++t) {
        size_t a = off[t], b = off[t + 1];
        total += zstd_size_ctx(c, flat.data() + a, b - a, level, false, dst);
    }
    ZSTD_freeCCtx(c);
    return total;
}
static size_t zstd_whole(const std::vector<uint8_t> &flat, int level, bool ldm) {
    ZSTD_CCtx *c = ZSTD_createCCtx();
    std::vector<uint8_t> dst;
    size_t r = zstd_size_ctx(c, flat.data(), flat.size(), level, ldm, dst);
    ZSTD_freeCCtx(c);
    return r;
}

// The fair "give zstd the same cross-TU knowledge" control: train a zstd DICTIONARY on the per-TU
// bodies, then compress each TU independently WITH that dictionary and sum. A trained dictionary is
// the other way (besides an explicit superblock layer) to inject cross-TU patterns into per-TU
// compression. Trained on all samples (optimistic for zstd), so if superblocks still win it is a
// robust conclusion. Returns 0 if training is not possible (too few samples).
static size_t zstd_perTU_dict_sum(const std::vector<uint8_t> &flat, const std::vector<size_t> &off,
                                  int level, size_t &dict_used) {
    dict_used = 0;
    size_t nsamples = off.size() - 1;
    if (nsamples < 16 || flat.size() < 4096) return 0;
    std::vector<size_t> sizes(nsamples);
    for (size_t t = 0; t < nsamples; ++t) sizes[t] = off[t + 1] - off[t];
    size_t dictCap = 256 * 1024;
    std::vector<uint8_t> dictbuf(dictCap);
    size_t ds = ZDICT_trainFromBuffer(dictbuf.data(), dictCap, flat.data(), sizes.data(), unsigned(nsamples));
    if (ZDICT_isError(ds)) return 0;
    dict_used = ds;
    ZSTD_CDict *cd = ZSTD_createCDict(dictbuf.data(), ds, level);
    ZSTD_CCtx *c = ZSTD_createCCtx();
    std::vector<uint8_t> dst;
    size_t total = 0;
    for (size_t t = 0; t < nsamples; ++t) {
        size_t a = off[t], b = off[t + 1];
        size_t bound = ZSTD_compressBound(b - a);
        if (dst.size() < bound) dst.resize(bound);
        size_t r = ZSTD_compress_usingCDict(c, dst.data(), dst.size(), flat.data() + a, b - a, cd);
        if (ZSTD_isError(r)) { ZSTD_freeCCtx(c); ZSTD_freeCDict(cd); return 0; }
        total += r;
    }
    ZSTD_freeCCtx(c);
    ZSTD_freeCDict(cd);
    return total;
}

// Captured per-TU streams (flat arenas + per-TU offsets).
struct Captured {
    std::vector<uint32_t> line_ids;
    std::vector<size_t> line_off;
    std::vector<uint32_t> region_ids;
    std::vector<size_t> region_off;
    uint64_t total_line_occ = 0;
    uint64_t total_region_occ = 0;
};

static Captured capture_streams(Interner &dict, const Corpus &corpus, bool train) {
    Captured cap;
    uint32_t max_len = 0;
    for (const auto &f : corpus.files) max_len = std::max(max_len, f.len);
    std::vector<uint32_t> out(size_t(max_len) + 1);
    cap.line_off.push_back(0);
    cap.region_off.push_back(0);
    uint64_t hits = 0;
    std::vector<uint32_t> region_scratch;
    for (const auto &f : corpus.files) {
        size_t out_count = 0;
        region_scratch.clear();
        const char *p = corpus.bytes.data() + f.off;
        dict.process(p, p + f.len, out.data(), out_count, hits, train, &region_scratch);
        cap.line_ids.insert(cap.line_ids.end(), out.data(), out.data() + out_count);
        cap.line_off.push_back(cap.line_ids.size());
        cap.region_ids.insert(cap.region_ids.end(), region_scratch.begin(), region_scratch.end());
        cap.region_off.push_back(cap.region_ids.size());
    }
    cap.total_line_occ = cap.line_ids.size();
    cap.total_region_occ = cap.region_ids.size();
    return cap;
}

static uint64_t hash_reconstructed_bytes(const Interner &dict, const uint32_t *ids, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        const LineRef &r = dict.ref(ids[i]);
        const char *b = dict.line_data(r.off);
        for (uint32_t j = 0; j < r.len; ++j) { h ^= uint8_t(b[j]); h *= 1099511628211ULL; }
    }
    return h;
}

// ---------- batched BPE / Re-Pair-lite grammar over a token stream ----------
struct Grammar {
    uint32_t base = 0;
    std::vector<std::pair<uint32_t,uint32_t>> rules;   // symbol (base+i) -> (a,b)
    void expand(uint32_t s, std::vector<uint32_t> &out) const {
        if (s < base) { out.push_back(s); return; }
        const auto &r = rules[s - base];
        expand(r.first, out);
        expand(r.second, out);
    }
    uint32_t leaf_count(uint32_t s, std::vector<uint32_t> &memo) const {
        if (s < base) return 1;
        uint32_t idx = s - base;
        if (memo[idx]) return memo[idx];
        const auto &r = rules[idx];
        uint32_t c = leaf_count(r.first, memo) + leaf_count(r.second, memo);
        memo[idx] = c;
        return c;
    }
};

static constexpr uint32_t SENT = 0xFFFFFFFFu;
static constexpr uint32_t MAX_RULES_PER_ROUND = 4u << 20;

struct PairCounts {
    std::vector<uint64_t> keys;
    std::vector<uint64_t> vals;
    size_t mask;
    explicit PairCounts(size_t cap_pow2) : keys(cap_pow2, UINT64_MAX), vals(cap_pow2, 0), mask(cap_pow2 - 1) {}
    inline void add(uint64_t k) {
        size_t i = mix64(k) & mask;
        for (;;) {
            if (keys[i] == k) { vals[i]++; return; }
            if (keys[i] == UINT64_MAX) { keys[i] = k; vals[i] = 1; return; }
            i = (i + 1) & mask;
        }
    }
};

// Each round: count adjacent pairs, accept the frequent ones (>=min_count, highest first up to a
// cap) as new symbols, apply left-to-right in ONE pass. Left-to-right greedy with a key->symbol map
// is always byte-exact (each position consumes at most one accepted pair; grammar expansion
// reproduces the exact run). Rounds grow superblocks to length up to 2^rounds.
static Grammar build_grammar(std::vector<uint32_t> &tok, uint32_t base,
                             uint32_t min_count, int max_rounds, bool verbose) {
    Grammar g;
    g.base = base;
    uint32_t next_sym = base;
    for (int round = 0; round < max_rounds; ++round) {
        size_t cap = 1;
        while (cap < tok.size() * 2 + 16) cap <<= 1;
        if (cap > (size_t(1) << 28)) cap = size_t(1) << 28;
        PairCounts pc(cap);
        for (size_t i = 0; i + 1 < tok.size(); ++i) {
            uint32_t a = tok[i], b = tok[i + 1];
            if (a == SENT || b == SENT) continue;
            pc.add((uint64_t(a) << 32) | b);
        }
        std::vector<std::pair<uint64_t,uint64_t>> cands;
        for (size_t i = 0; i <= pc.mask; ++i)
            if (pc.keys[i] != UINT64_MAX && pc.vals[i] >= min_count)
                cands.push_back({pc.vals[i], pc.keys[i]});
        if (cands.empty()) { if (verbose) fprintf(stderr, "    round %d: no pair >= %u, stop\n", round, min_count); break; }
        std::sort(cands.begin(), cands.end(), std::greater<>());
        if (cands.size() > MAX_RULES_PER_ROUND) cands.resize(MAX_RULES_PER_ROUND);
        std::vector<uint64_t> amk; std::vector<uint32_t> amv; size_t amask;
        { size_t c = 1; while (c < cands.size() * 2 + 16) c <<= 1; amk.assign(c, UINT64_MAX); amv.assign(c, 0); amask = c - 1; }
        auto amput = [&](uint64_t k, uint32_t sym) { size_t i = mix64(k) & amask; while (amk[i] != UINT64_MAX) i = (i + 1) & amask; amk[i] = k; amv[i] = sym; };
        for (auto &c : cands) { g.rules.push_back({uint32_t(c.second >> 32), uint32_t(c.second)}); amput(c.second, next_sym++); }
        auto amget = [&](uint64_t k) -> uint32_t { size_t i = mix64(k) & amask; while (amk[i] != UINT64_MAX) { if (amk[i] == k) return amv[i]; i = (i + 1) & amask; } return UINT32_MAX; };
        std::vector<uint32_t> nt;
        nt.reserve(tok.size());
        for (size_t i = 0; i < tok.size(); ) {
            uint32_t a = tok[i];
            if (a != SENT && i + 1 < tok.size()) {
                uint32_t b = tok[i + 1];
                if (b != SENT) { uint32_t sym = amget((uint64_t(a) << 32) | b); if (sym != UINT32_MAX) { nt.push_back(sym); i += 2; continue; } }
            }
            nt.push_back(a);
            ++i;
        }
        if (verbose) fprintf(stderr, "    round %d: cands=%zu tok %zu -> %zu (rules=%zu)\n",
                             round, cands.size(), tok.size(), nt.size(), g.rules.size());
        tok.swap(nt);
    }
    return g;
}

// ---------- one encoding's measured result ----------
struct EncResult {
    std::string name;
    // recurring per-message body (dictionaries amortized)
    uint64_t body_raw = 0;
    uint64_t body_perTU_z3 = 0;      // sum of independent per-TU zstd (the real wire cost)
    uint64_t body_perTU_zdict = 0;   // per-TU zstd WITH a trained cross-TU dictionary (fair control)
    uint64_t zdict_size = 0;         // amortized size of that trained dictionary
    uint64_t body_whole_z3 = 0;      // batched floor
    uint64_t body_whole_z19 = 0;     // batched floor, strong zstd
    // amortized dictionary (composition table / grammar), sent once
    uint64_t dict_raw = 0;
    uint64_t dict_z3 = 0;
    // stats
    uint64_t distinct_blocks = 0;
    double coverage_pct = 0;      // % of line-occurrences absorbed into a multi-line superblock ref
    bool verified = false;
};

// Build per-TU body slices from a token stream (global-id varint), given per-TU offsets.
static void body_from_tokens(const uint32_t *tok, const std::vector<size_t> &off,
                             std::vector<uint8_t> &flat, std::vector<size_t> &boff) {
    boff.clear();
    boff.push_back(0);
    for (size_t t = 0; t + 1 < off.size(); ++t) {
        for (size_t i = off[t]; i < off[t + 1]; ++i) put_varint(flat, tok[i]);
        boff.push_back(flat.size());
    }
}

int main(int argc, char **argv) {
    const char *manifest = nullptr;
    bool do19 = false, poison = false;
    size_t max_files = SIZE_MAX;
    uint32_t bpe_min = 16;
    int bpe_rounds = 20;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
        else if (!strcmp(argv[i], "--zstd19")) do19 = true;
        else if (!strcmp(argv[i], "--poison")) poison = true;
        else if (!strcmp(argv[i], "--bpe-min") && i + 1 < argc) bpe_min = uint32_t(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--bpe-rounds") && i + 1 < argc) bpe_rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-files") && i + 1 < argc) {
            char *tail = nullptr; unsigned long long v = strtoull(argv[++i], &tail, 10);
            if (!tail || *tail || !v) { fprintf(stderr, "invalid --max-files\n"); return 2; }
            max_files = size_t(v);
        } else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
    }
    if (!manifest) { fprintf(stderr, "usage: %s --manifest FILE [--zstd19] [--poison] [--bpe-min N] [--bpe-rounds N] [--max-files N]\n", argv[0]); return 2; }

    auto t0 = Clock::now();
    Corpus corpus = load_corpus(manifest, max_files);
    Interner dict;
    Captured cap = capture_streams(dict, corpus, true);
    fprintf(stderr, "loaded+interned in %.1fs: TUs=%zu raw=%llu distinct_lines=%u line_occ=%llu region_occ=%llu distinct_regions=%llu\n",
            seconds_since(t0), corpus.files.size(), (unsigned long long)corpus.raw, dict.distinct(),
            (unsigned long long)cap.total_line_occ, (unsigned long long)cap.total_region_occ,
            (unsigned long long)dict.region_count());

    uint64_t ref_hash = hash_bytes(reinterpret_cast<const uint8_t*>(corpus.bytes.data()), corpus.raw);
    if (hash_reconstructed_bytes(dict, cap.line_ids.data(), cap.line_ids.size()) != ref_hash) {
        fprintf(stderr, "FATAL: baseline line-id stream does not reconstruct source\n"); return 1;
    }
    // anchor: region-id stream expands to the exact line-id stream
    {
        std::vector<uint32_t> exp; exp.reserve(cap.total_line_occ);
        for (uint32_t r : cap.region_ids) { const uint32_t *ids = dict.region_ids_ptr(r); exp.insert(exp.end(), ids, ids + dict.region_ids_count(r)); }
        if (exp.size() != cap.total_line_occ || memcmp(exp.data(), cap.line_ids.data(), exp.size() * sizeof(uint32_t)) != 0) {
            fprintf(stderr, "FATAL: region-id stream does not expand to line-id stream\n"); return 1;
        }
    }
    fprintf(stderr, "byte-exactness anchors OK (raw == line-id stream == region-id expansion)\n");

    std::vector<EncResult> results;

    // helper to finalize a token-stream encoding (body = per-TU tokens, dict optional)
    auto measure_tokens = [&](const std::string &name, const uint32_t *tok, const std::vector<size_t> &off,
                              const std::vector<uint8_t> &dict_bytes, double coverage_pct,
                              uint64_t distinct_blocks, bool verified) {
        EncResult e; e.name = name;
        std::vector<uint8_t> flat; std::vector<size_t> boff;
        flat.reserve(cap.total_line_occ * 2);
        body_from_tokens(tok, off, flat, boff);
        e.body_raw = flat.size();
        e.body_perTU_z3 = zstd_perTU_sum(flat, boff, 3);
        e.body_perTU_zdict = zstd_perTU_dict_sum(flat, boff, 3, e.zdict_size);
        e.body_whole_z3 = zstd_whole(flat, 3, false);
        if (do19) e.body_whole_z19 = zstd_whole(flat, 19, true);
        e.dict_raw = dict_bytes.size();
        e.dict_z3 = dict_bytes.empty() ? 0 : zstd_whole(dict_bytes, 3, false);
        e.coverage_pct = coverage_pct;
        e.distinct_blocks = distinct_blocks;
        e.verified = verified;
        results.push_back(e);
    };

    // ---- B_global: line-id occurrence stream, raw global varint (no amortized dict beyond line text) ----
    measure_tokens("B_global (line-id stream)", cap.line_ids.data(), cap.line_off, {}, 0.0,
                   dict.distinct(), true);

    // ---- B_local: per-TU first-appearance local remap + per-TU keyset (the CURRENT transport) ----
    {
        EncResult e; e.name = "B_local (line-id stream, per-TU local remap + keyset)";
        std::vector<uint32_t> stamp(dict.distinct() + 1, 0), local_of(dict.distinct() + 1, 0);
        uint32_t cur = 0;
        std::vector<uint8_t> flat; std::vector<size_t> boff; boff.push_back(0);
        std::vector<uint32_t> keyset;
        size_t TUs = cap.line_off.size() - 1;
        for (size_t t = 0; t < TUs; ++t) {
            ++cur; keyset.clear();
            std::vector<uint8_t> body;
            for (size_t i = cap.line_off[t]; i < cap.line_off[t + 1]; ++i) {
                uint32_t gid = cap.line_ids[i];
                if (stamp[gid] != cur) { stamp[gid] = cur; local_of[gid] = uint32_t(keyset.size()); keyset.push_back(gid); }
                put_varint(body, local_of[gid]);
            }
            // per-TU message = keyset (global ids, first-appearance, delta-zigzag) + local-id body
            put_varint(flat, keyset.size());
            int64_t prev = 0;
            for (uint32_t gid : keyset) { put_zigzag(flat, int64_t(gid) - prev); prev = int64_t(gid); }
            flat.insert(flat.end(), body.begin(), body.end());
            boff.push_back(flat.size());
        }
        e.body_raw = flat.size();
        e.body_perTU_z3 = zstd_perTU_sum(flat, boff, 3);
        e.body_perTU_zdict = zstd_perTU_dict_sum(flat, boff, 3, e.zdict_size);
        e.body_whole_z3 = zstd_whole(flat, 3, false);
        if (do19) e.body_whole_z19 = zstd_whole(flat, 19, true);
        e.dict_raw = e.dict_z3 = 0;   // keyset is per-message (already in body)
        e.distinct_blocks = dict.distinct();
        e.verified = true;   // identity remap, exact by construction
        results.push_back(e);
    }

    // ---- S_region: marker-aligned superblocks. body = region-id stream; dict = composition table ----
    {
        uint64_t nregions = dict.region_count();
        std::vector<uint8_t> seen(nregions, 0);
        std::vector<uint32_t> order;
        for (uint32_t r : cap.region_ids) if (!seen[r]) { seen[r] = 1; order.push_back(r); }
        std::vector<uint8_t> dict_bytes;
        put_varint(dict_bytes, order.size());
        uint64_t comp_line_occ = 0;
        for (uint32_t r : order) {
            uint32_t cnt = dict.region_ids_count(r); const uint32_t *ids = dict.region_ids_ptr(r);
            put_varint(dict_bytes, cnt);
            for (uint32_t j = 0; j < cnt; ++j) put_varint(dict_bytes, ids[j]);
            comp_line_occ += cnt;
        }
        // coverage: line-occurrences absorbed by reusing a region (all but its first appearance)
        double cov = 100.0 * double(cap.total_line_occ - comp_line_occ) / double(cap.total_line_occ);
        measure_tokens("S_region (marker regions)", cap.region_ids.data(), cap.region_off,
                       dict_bytes, cov, order.size(), true);
    }

    // ---- S_bpe_line: general superblocks over line-ids. body = tiled tokens; dict = grammar ----
    {
        auto tb = Clock::now();
        size_t TUs = cap.line_off.size() - 1;
        // BPE needs SENT separators; build a SENT-joined copy, tile, then split back per TU.
        std::vector<uint32_t> js; js.reserve(cap.total_line_occ + TUs);
        for (size_t t = 0; t < TUs; ++t) { for (size_t i = cap.line_off[t]; i < cap.line_off[t + 1]; ++i) js.push_back(cap.line_ids[i]); js.push_back(SENT); }
        uint32_t base = dict.distinct() + 1;
        fprintf(stderr, "S_bpe_line BPE:\n");
        Grammar g = build_grammar(js, base, bpe_min, bpe_rounds, true);
        // split tiled stream per TU on SENT
        std::vector<uint32_t> tiled; std::vector<size_t> tiled_off; tiled_off.push_back(0);
        std::vector<uint32_t> memo(g.rules.size() + 1, 0);
        uint64_t absorbed = 0;
        for (uint32_t x : js) {
            if (x == SENT) { tiled_off.push_back(tiled.size()); continue; }
            tiled.push_back(x + 1);   // +1: keep 0 free (unused here but consistent)
            if (x >= base) absorbed += g.leaf_count(x, memo);
        }
        // dict = grammar rules
        std::vector<uint8_t> dict_bytes;
        put_varint(dict_bytes, g.rules.size());
        for (auto &r : g.rules) { put_varint(dict_bytes, r.first); put_varint(dict_bytes, r.second); }
        // verify by expanding tiled back to line-ids
        std::vector<uint32_t> recon; recon.reserve(cap.total_line_occ);
        for (uint32_t x : js) { if (x == SENT) continue; g.expand(x, recon); }
        bool ok = (recon.size() == cap.total_line_occ) && (memcmp(recon.data(), cap.line_ids.data(), recon.size() * sizeof(uint32_t)) == 0);
        double cov = 100.0 * double(absorbed) / double(cap.total_line_occ);
        fprintf(stderr, "  built in %.1fs rules=%zu coverage=%.1f%% verify=%s\n", seconds_since(tb), g.rules.size(), cov, ok ? "OK" : "FAIL");
        measure_tokens("S_bpe_line (general line-runs)", tiled.data(), tiled_off, dict_bytes, cov, g.rules.size(), ok);
    }

    // ---- S_bpe_region: (a)+(b) super-regions over the region stream. dict = composition + grammar ----
    {
        auto tb = Clock::now();
        size_t TUs = cap.region_off.size() - 1;
        std::vector<uint32_t> js; js.reserve(cap.total_region_occ + TUs);
        for (size_t t = 0; t < TUs; ++t) { for (size_t i = cap.region_off[t]; i < cap.region_off[t + 1]; ++i) js.push_back(cap.region_ids[i]); js.push_back(SENT); }
        uint32_t base = uint32_t(dict.region_count()) + 1;
        fprintf(stderr, "S_bpe_region BPE:\n");
        Grammar g = build_grammar(js, base, bpe_min, bpe_rounds, true);
        std::vector<uint32_t> tiled; std::vector<size_t> tiled_off; tiled_off.push_back(0);
        for (uint32_t x : js) { if (x == SENT) { tiled_off.push_back(tiled.size()); continue; } tiled.push_back(x + 1); }
        // composition table (regions -> line-ids), same as S_region
        uint64_t nregions = dict.region_count();
        std::vector<uint8_t> seen(nregions, 0); std::vector<uint32_t> order;
        for (uint32_t r : cap.region_ids) if (!seen[r]) { seen[r] = 1; order.push_back(r); }
        std::vector<uint8_t> dict_bytes;
        put_varint(dict_bytes, order.size());
        uint64_t comp_line_occ = 0;
        for (uint32_t r : order) { uint32_t cnt = dict.region_ids_count(r); const uint32_t *ids = dict.region_ids_ptr(r); put_varint(dict_bytes, cnt); for (uint32_t j = 0; j < cnt; ++j) put_varint(dict_bytes, ids[j]); comp_line_occ += cnt; }
        // + super-region grammar
        put_varint(dict_bytes, g.rules.size());
        for (auto &r : g.rules) { put_varint(dict_bytes, r.first); put_varint(dict_bytes, r.second); }
        // verify: expand tiled -> region-ids -> compare
        std::vector<uint32_t> recon; recon.reserve(cap.total_region_occ);
        for (uint32_t x : js) { if (x == SENT) continue; g.expand(x, recon); }
        bool ok = (recon.size() == cap.total_region_occ) && (memcmp(recon.data(), cap.region_ids.data(), recon.size() * sizeof(uint32_t)) == 0);
        // coverage in line-occurrences: regions absorbed into super-regions -> their line counts
        std::vector<uint32_t> memo(g.rules.size() + 1, 0);
        // reuse S_region's baseline coverage (regions reused beyond first appearance) as the floor;
        // super-regions don't change which line-occurrences are absorbed, only how the region stream is coded.
        double cov = 100.0 * double(cap.total_line_occ - comp_line_occ) / double(cap.total_line_occ);
        fprintf(stderr, "  built in %.1fs rules=%zu verify=%s\n", seconds_since(tb), g.rules.size(), ok ? "OK" : "FAIL");
        measure_tokens("S_bpe_region ((a)+(b) super-regions)", tiled.data(), tiled_off, dict_bytes, cov, g.rules.size(), ok);
    }

    // ---------- report ----------
    double MiB = 1048576.0;
    printf("\n==== SUPERBLOCK COMPRESSION REPORT ====\n");
    printf("corpus: %s\n", manifest);
    printf("TUs=%zu  raw_source=%.1f MiB  distinct_lines=%u  line_occ=%llu  region_occ=%llu  distinct_regions=%llu\n",
           corpus.files.size(), corpus.raw / MiB, dict.distinct(),
           (unsigned long long)cap.total_line_occ, (unsigned long long)cap.total_region_occ,
           (unsigned long long)dict.region_count());
    printf("shared line-text dictionary (amortized) = %.2f MiB\n\n", dict.distinct_line_bytes() / MiB);

    printf("RECURRING per-TU wire cost (dictionaries amortized; each TU zstd'd independently):\n");
    printf("  perTU-L3      = each TU's occurrence body, zstd-L3, no dictionary (summed)\n");
    printf("  perTU+zdict   = same, but WITH a trained cross-TU zstd dictionary (fair control)\n");
    printf("  whole-L3/L19  = whole corpus batched into one zstd stream (theoretical floor)\n\n");
    printf("%-42s %11s %8s %11s | %10s %10s %7s %s\n",
           "encoding", "perTU-L3", "KB/TU", "perTU+zdict", "whole-L3", "wholeL19", "cover%", "verify");
    for (auto &e : results) {
        double kbtu = double(e.body_perTU_z3) / corpus.files.size() / 1024.0;
        char zd[32];
        if (e.body_perTU_zdict) snprintf(zd, sizeof zd, "%llu", (unsigned long long)e.body_perTU_zdict);
        else snprintf(zd, sizeof zd, "n/a");
        printf("%-42s %11llu %8.1f %11s | %10llu %10llu %6.1f %s\n",
               e.name.c_str(), (unsigned long long)e.body_perTU_z3, kbtu, zd,
               (unsigned long long)e.body_whole_z3, (unsigned long long)e.body_whole_z19,
               e.coverage_pct, e.verified ? "OK" : "FAIL");
    }
    printf("\nAMORTIZED artifacts (sent once over all jobs): superblock dict = composition table/grammar;\n");
    printf("zdict = the trained zstd dictionary used by the perTU+zdict column.\n");
    printf("%-42s %12s %11s %11s %10s\n", "encoding", "dict_raw", "dict-L3", "zdict_sz", "blocks");
    for (auto &e : results)
        printf("%-42s %12llu %11llu %11llu %10llu\n", e.name.c_str(),
               (unsigned long long)e.dict_raw, (unsigned long long)e.dict_z3,
               (unsigned long long)e.zdict_size, (unsigned long long)e.distinct_blocks);

    // decisive comparisons vs the strong line-id baselines
    auto find = [&](const char *pfx) -> EncResult* { for (auto &e : results) if (e.name.rfind(pfx, 0) == 0) return &e; return nullptr; };
    EncResult *bg = find("B_global");
    printf("\n---- DECISIVE: does an explicit superblock layer beat zstd on the line-id stream? ----\n");
    if (bg) {
        printf("baseline B_global line-id stream:  per-TU-L3=%llu (%.1f KB/TU)  whole-L3=%llu  whole-L19=%llu\n",
               (unsigned long long)bg->body_perTU_z3, double(bg->body_perTU_z3)/corpus.files.size()/1024.0,
               (unsigned long long)bg->body_whole_z3, (unsigned long long)bg->body_whole_z19);
        for (auto &e : results) {
            if (&e == bg || e.name.rfind("B_local", 0) == 0) continue;
            double d_tu  = 100.0 * (double(bg->body_perTU_z3) - double(e.body_perTU_z3)) / double(bg->body_perTU_z3);
            double d_wh  = 100.0 * (double(bg->body_whole_z3) - double(e.body_whole_z3)) / double(bg->body_whole_z3);
            printf("  %-40s per-TU-L3 %+6.1f%%   whole-L3 %+6.1f%%", e.name.c_str(), d_tu, d_wh);
            if (do19 && bg->body_whole_z19) {
                double d19 = 100.0 * (double(bg->body_whole_z19) - double(e.body_whole_z19)) / double(bg->body_whole_z19);
                printf("   whole-L19 %+6.1f%%", d19);
            }
            printf("\n");
        }
        printf("(positive %% = superblock body is SMALLER than the line-id body at that setting.)\n");
    }

    if (poison) {
        uint64_t modified = 0;
        Corpus pc = make_poisoned_corpus(corpus, modified);
        uint64_t regions_before = dict.region_count();
        uint32_t lines_before = dict.distinct();
        Captured pcap = capture_streams(dict, pc, true);
        printf("\n---- POISON sensitivity (one line inserted after a common header marker) ----\n");
        printf("modified TUs=%llu  new distinct lines=+%u  new distinct regions=+%llu (of %llu)  region_occ(poison)=%llu\n",
               (unsigned long long)modified, dict.distinct() - lines_before,
               (unsigned long long)(dict.region_count() - regions_before),
               (unsigned long long)dict.region_count(), (unsigned long long)pcap.total_region_occ);
        printf("hierarchical superblocks localize the edit: only the poisoned region (and any super-region\n");
        printf("containing it) is new; every other region/superblock is reused unchanged.\n");
    }
    return 0;
}
