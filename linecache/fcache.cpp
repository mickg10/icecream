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
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/perf_event.h>
#include <vector>
#include <immintrin.h>

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

static inline uint64_t read64(const char *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static inline uint64_t read_tail(const char *p, uint32_t n) {
    uint64_t v = 0;
    memcpy(&v, p, n);
    return v;
}

// A table-routing fingerprint. Equality is always established by exact length/bytes.
static inline uint64_t sampled_hash(const char *p, uint32_t n) {
#ifdef FORCE_SELECTOR_COLLISIONS
    (void)p;
    (void)n;
    return 1;
#else
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
#endif
}

// Full-byte line hash used only when a region has not previously been interned.
static inline uint64_t line_hash(const char *key, uint32_t len) {
#ifdef FORCE_SELECTOR_COLLISIONS
    (void)key;
    (void)len;
    return 1;
#else
    constexpr uint64_t secret[3] = {
        0x2d358dccaa6c78a5ULL, 0x8bb84b93962eacc9ULL, 0x4b33a62ed433d4a3ULL
    };
    uint64_t seed = 0xbdd89aa982704029ULL ^ uint64_t(len);
    const char *p = key;
    uint32_t n = len;
    if (n <= 16) {
        uint64_t a = 0, b = 0;
        if (n >= 8) {
            a = read64(p);
            b = read64(p + n - 8);
        } else if (n) {
            a = read_tail(p, n);
            b = a;
        }
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
#endif
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

struct FileSpan {
    uint64_t off;
    uint32_t len;
};

struct LineRef {
    uint32_t off;
    uint32_t len;
};

struct TinySlot {
    uint64_t bytes;
    uint32_t id;
    uint8_t len;
    uint8_t pad[3];
};

struct ShortSlot {
    uint64_t lo;
    uint64_t hi;
    uint32_t id;
    uint8_t len;
    uint8_t pad[3];
};

struct LineSlot {
    uint64_t hash;
    uint32_t off;
    uint32_t len;
    uint32_t id;
    uint32_t pad;
};

struct RegionRecord {
    uint64_t hash;
    uint32_t raw_off;
    uint32_t raw_len;
    uint32_t ids_off;
    uint32_t ids_count;
    uint32_t next1;
    uint32_t next2;
};

struct PassResult {
    double seconds = 0;
    uint64_t raw = 0;
    uint64_t lines = 0;
    uint64_t regions = 0;
    uint64_t region_hits = 0;
    uint64_t sink = 0;
    uint64_t region_probes = 0;
    uint64_t line_probes = 0;
    uint64_t predictor_attempts = 0;
    uint64_t predictor_hits = 0;
    uint64_t cycles = 0;
    uint64_t instructions = 0;
    uint64_t branches = 0;
    uint64_t branch_misses = 0;
    uint64_t cache_misses = 0;
};

class PerfCounters {
public:
    PerfCounters() {
        leader_ = open(PERF_COUNT_HW_CPU_CYCLES, -1);
        if (leader_ < 0) return;
        fds_[0] = leader_;
        fds_[1] = open(PERF_COUNT_HW_INSTRUCTIONS, leader_);
        fds_[2] = open(PERF_COUNT_HW_BRANCH_INSTRUCTIONS, leader_);
        fds_[3] = open(PERF_COUNT_HW_BRANCH_MISSES, leader_);
        fds_[4] = open(PERF_COUNT_HW_CACHE_MISSES, leader_);
        for (int fd : fds_) if (fd < 0) { close_all(); return; }
        available_ = true;
    }

    ~PerfCounters() { close_all(); }

    void start() {
        if (!available_) return;
        ioctl(leader_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(leader_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }

    void stop(PassResult &r) {
        if (!available_) return;
        ioctl(leader_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        uint64_t v[5] = {};
        for (unsigned i = 0; i < 5; ++i) {
            if (::read(fds_[i], &v[i], sizeof(v[i])) != sizeof(v[i])) return;
        }
        r.cycles = v[0];
        r.instructions = v[1];
        r.branches = v[2];
        r.branch_misses = v[3];
        r.cache_misses = v[4];
    }

private:
    static int open(uint64_t config, int group_fd) {
        perf_event_attr a {};
        a.type = PERF_TYPE_HARDWARE;
        a.size = sizeof(a);
        a.config = config;
        a.disabled = group_fd < 0;
        a.exclude_kernel = 1;
        a.exclude_hv = 1;
        return int(syscall(SYS_perf_event_open, &a, 0, -1, group_fd, 0));
    }

    void close_all() {
        for (int &fd : fds_) {
            if (fd >= 0) ::close(fd);
            fd = -1;
        }
        leader_ = -1;
        available_ = false;
    }

    int leader_ = -1;
    int fds_[5] = {-1, -1, -1, -1, -1};
    bool available_ = false;
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
    uint64_t region_probes() const { return region_probes_; }
    uint64_t line_probes() const { return line_probes_; }
    uint64_t predictor_attempts() const { return predictor_attempts_; }
    uint64_t predictor_hits() const { return predictor_hits_; }
    uint64_t line_bytes() const { return line_bytes_.size(); }
    uint64_t line_capacity_bytes() const { return line_bytes_.capacity(); }
    uint64_t region_bytes() const { return region_bytes_.size(); }
    uint64_t region_capacity_bytes() const { return region_bytes_.capacity(); }
    uint64_t region_id_bytes() const { return region_ids_.size() * sizeof(uint32_t); }
    uint64_t region_id_capacity_bytes() const {
        return region_ids_.capacity() * sizeof(uint32_t);
    }
    uint64_t region_index_bytes() const { return region_index_.size() * sizeof(uint32_t); }
    uint64_t region_record_bytes() const { return region_records_.size() * sizeof(RegionRecord); }
    uint64_t region_record_capacity_bytes() const {
        return region_records_.capacity() * sizeof(RegionRecord);
    }
    uint64_t id_ref_capacity_bytes() const { return id_refs_.capacity() * sizeof(LineRef); }
    const LineRef &ref(uint32_t id) const { return id_refs_[id]; }
    const char *line_data(uint32_t off) const { return line_bytes_.data() + off; }

    void reset_counters() {
        region_probes_ = line_probes_ = predictor_attempts_ = predictor_hits_ = 0;
    }

    uint32_t intern_line(const char *p, uint32_t n) {
        if (n <= 4) return intern_tiny(p, n);
        if (n <= 16) return intern_short(p, n);
        uint64_t h = line_hash(p, n) | 1ULL;
        uint32_t slot = uint32_t(h) & (LINE_CAP - 1);
        uint32_t probes = 0;
        for (;;) {
            if (++probes > LINE_CAP) {
                fprintf(stderr, "line table exhausted\n"); exit(2);
            }
            ++line_probes_;
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

    void process(const char *begin, const char *end, uint32_t *out,
                 size_t &out_count, uint64_t &region_hits, bool train_predictor) {
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
                    ++predictor_attempts_;
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
                        memcpy(out + out_count,
                               region_ids_.data() + candidate.ids_off,
                               size_t(candidate.ids_count) * sizeof(uint32_t));
                        out_count += candidate.ids_count;
                        ++predictor_hits_;
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
                    ++region_probes_;
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

#ifdef LINE_ONLY
    void process_linewise(const char *begin, const char *end, uint32_t *out,
                          size_t &out_count) {
        const char *p = begin;
        while (p < end) {
            const void *hit = memchr(p, '\n', size_t(end - p));
            const char *q = hit ? static_cast<const char *>(hit) + 1 : end;
            out[out_count++] = intern_line(p, uint32_t(q - p));
            p = q;
        }
    }
#endif

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
            if (++probes > TINY_CAP) {
                fprintf(stderr, "tiny table exhausted\n"); exit(2);
            }
            ++line_probes_;
            TinySlot &s = tiny_[slot];
            if (!s.id) {
                uint32_t id = add_line(p, n);
                s.bytes = bytes; s.id = id; s.len = uint8_t(n);
                return id;
            }
            if (s.len == n && s.bytes == bytes) return s.id;
            slot = (slot + 1) & (TINY_CAP - 1);
        }
    }

    uint32_t intern_short(const char *p, uint32_t n) {
        uint64_t lo = n >= 8 ? read64(p) : read_tail(p, n);
        uint64_t hi = n > 8 ? read_tail(p + 8, n - 8) : lo;
        uint64_t h = fold128(lo ^ 0xa0761d6478bd642fULL,
                             hi ^ uint64_t(n) * 0xe7037ed1a0b428dbULL);
        uint32_t slot = uint32_t(h) & (SHORT_CAP - 1);
        uint32_t probes = 0;
        for (;;) {
            if (++probes > SHORT_CAP) {
                fprintf(stderr, "short table exhausted\n"); exit(2);
            }
            ++line_probes_;
            ShortSlot &s = short_[slot];
            if (!s.id) {
                uint32_t id = add_line(p, n);
                s.lo = lo; s.hi = hi; s.id = id; s.len = uint8_t(n);
                return id;
            }
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
    uint64_t region_probes_ = 0;
    uint64_t line_probes_ = 0;
    uint64_t predictor_attempts_ = 0;
    uint64_t predictor_hits_ = 0;
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

__attribute__((noinline)) static void expose_output(const uint32_t *p, size_t n) {
    asm volatile("" : : "r"(p), "r"(n) : "memory");
}
// ===== PRODUCTION_FUSED: F cache-server + ephemeral fork-per-job (the real shape) =====
// Correct model (owner-specified):
//   * F cache SERVER  = ONE persistent process, sole owner of the per-GUID key->text store.
//   * iceccd per-job FORK = ephemeral: handles ONE job then DIES. It holds NONE of the
//     cache; it TALKS to the cache server over IPC to get the text it needs. NO COW, NO
//     shared memory of the cache. Each job = fork -> connect -> ask server -> reconstruct
//     -> exit. This measures whether ONE cache owner saturates as concurrency rises (-> shard).
// Roles:
//   --role cacheserver --sock PATH        (persistent; serves KEYSET-> text)
//   --role driver --sock PATH --jobs N    (fork-per-TU, N concurrent, over the whole corpus)
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <csignal>
#include <thread>

struct FStore {
    std::vector<uint8_t> has; std::vector<uint32_t> foff, flen; std::vector<char> fb;
    void ensure(uint32_t d){ size_t n=size_t(d)+1; if(has.size()<n){has.resize(n,0);foff.resize(n,0);flen.resize(n,0);} }
    void add(uint32_t g,const char*p,uint32_t n){ ensure(g); if(has[g])return; uint32_t o=(uint32_t)fb.size(); fb.insert(fb.end(),p,p+n); foff[g]=o; flen[g]=n; has[g]=1; }
};
static inline uint64_t digest(const char*p,size_t n){ uint64_t h=0x100000001b3ULL^n; size_t i=0; for(;i+8<=n;i+=8){uint64_t w;memcpy(&w,p+i,8);h=(h^w)*0x9E3779B97F4A7C15ULL;h^=h>>29;} uint64_t t=0; if(i<n)memcpy(&t,p+i,n-i); h=(h^t)*0x9E3779B97F4A7C15ULL; return h^(h>>31); }
static bool wr(int fd,const void*p,size_t n){ const char*b=(const char*)p; while(n){ ssize_t w=write(fd,b,n); if(w<=0){ if(w<0&&errno==EINTR)continue; return false;} b+=w; n-=size_t(w);} return true; }
static bool rd(int fd,void*p,size_t n){ char*b=(char*)p; while(n){ ssize_t r=read(fd,b,n); if(r<=0){ if(r<0&&errno==EINTR)continue; return false;} b+=r; n-=size_t(r);} return true; }
static bool wrf(int fd,const void*p,uint32_t n){ return wr(fd,&n,4)&&wr(fd,p,n); }
static bool rdf(int fd,std::vector<uint8_t>&b){ uint32_t n; if(!rd(fd,&n,4))return false; b.resize(n); return rd(fd,b.data(),n); }
static Interner* build_dict(const Corpus& c){ Interner* d=new Interner(); uint32_t ml=0; for(auto&f:c.files) ml=std::max(ml,f.len);
    std::vector<uint32_t> out(size_t(ml)+1); for(auto&f:c.files){ size_t oc=0; uint64_t h=0; d->process(c.bytes.data()+f.off,c.bytes.data()+f.off+f.len,out.data(),oc,h,true); expose_output(out.data(),oc);} return d; }
static int usock_listen(const char* path){ unlink(path); int s=socket(AF_UNIX,SOCK_STREAM,0); sockaddr_un a{}; a.sun_family=AF_UNIX; strncpy(a.sun_path,path,sizeof(a.sun_path)-1);
    if(bind(s,(sockaddr*)&a,sizeof a)){ perror("bind"); exit(2);} listen(s,256); return s; }
static int usock_conn(const char* path){ int s=socket(AF_UNIX,SOCK_STREAM,0); sockaddr_un a{}; a.sun_family=AF_UNIX; strncpy(a.sun_path,path,sizeof(a.sun_path)-1);
    for(int t=0; connect(s,(sockaddr*)&a,sizeof a) && t<6000; ++t) usleep(2000); return s; }

// cache-server: serve one connection. Request = [u32 nk][u32 global_key ...].
// Response = nk x ([u32 len][text bytes]) for the keys, from the shared read-only warm store.
static void serve(int c, const FStore* F){
    std::vector<uint8_t> ks, resp;
    for(;;){ if(!rdf(c,ks)) break; const uint8_t* p=ks.data(); uint32_t nk; memcpy(&nk,p,4); p+=4;
        resp.clear();
        for(uint32_t i=0;i<nk;i++){ uint32_t g; memcpy(&g,p,4); p+=4; uint32_t n=F->flen[g]; const char* t=F->fb.data()+F->foff[g];
            uint8_t lb[4]; memcpy(lb,&n,4); resp.insert(resp.end(),lb,lb+4); resp.insert(resp.end(),(const uint8_t*)t,(const uint8_t*)t+n); }
        if(!wrf(c,resp.data(),(uint32_t)resp.size())) break;
    }
    close(c);
}

int main(int argc,char**argv){
    const char* manifest=nullptr; const char* role=nullptr; const char* sock="/tmp/fcache.sock"; int jobs=8; size_t maxf=SIZE_MAX;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--manifest")&&i+1<argc)manifest=argv[++i];
        else if(!strcmp(argv[i],"--role")&&i+1<argc)role=argv[++i];
        else if(!strcmp(argv[i],"--sock")&&i+1<argc)sock=argv[++i];
        else if(!strcmp(argv[i],"--jobs")&&i+1<argc)jobs=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--max-files")&&i+1<argc)maxf=strtoull(argv[++i],0,10); }
    if(!manifest||!role){ fprintf(stderr,"usage: fcache --role cacheserver|driver --manifest F --sock P [--jobs N]\n"); return 2; }
    Corpus corpus=load_corpus(manifest,maxf);
    Interner* dict=build_dict(corpus); uint32_t distinct=dict->distinct();

    if(!strcmp(role,"cacheserver")){
        FStore* F=new FStore(); F->ensure(distinct); for(uint32_t g=1;g<=distinct;g++){ const LineRef&r=dict->ref(g); F->add(g,dict->line_data(r.off),r.len);} // warm per-GUID store
        int s=usock_listen(sock);
        fprintf(stderr,"F cache-server ready: sock=%s distinct=%u store=%.1fMB (single owner; forks talk to it over IPC)\n",sock,distinct,F->fb.size()/1048576.0);
        signal(SIGCHLD,SIG_IGN);
        for(;;){ int c=accept(s,nullptr,nullptr); if(c<0){ if(errno==EINTR)continue; break; } std::thread(serve,c,F).detach(); }
        return 0;
    }
    // driver = iceccd: precompute each TU's message (keyset + occ local ids), then run the
    // corpus through a pool of N concurrent EPHEMERAL forks (one job per fork, then it dies).
    uint32_t ml=0; for(auto&f:corpus.files) ml=std::max(ml,f.len);
    std::vector<uint32_t> ks_flat, occ_flat; std::vector<size_t> ks_off(corpus.files.size()+1,0), occ_off(corpus.files.size()+1,0);
    { std::vector<uint32_t> out(size_t(ml)+1), used_g, local_of(size_t(distinct)+1,0), stamp(size_t(distinct)+1,0); uint32_t epoch=0;
      for(size_t fi=0; fi<corpus.files.size(); ++fi){ const auto&f=corpus.files[fi]; size_t occ=0; uint64_t h=0;
        dict->process(corpus.bytes.data()+f.off,corpus.bytes.data()+f.off+f.len,out.data(),occ,h,false);
        ++epoch; used_g.clear(); for(size_t i=0;i<occ;i++){ uint32_t g=out[i]; if(stamp[g]!=epoch){ stamp[g]=epoch; local_of[g]=(uint32_t)used_g.size(); used_g.push_back(g);} }
        ks_flat.insert(ks_flat.end(),used_g.begin(),used_g.end()); ks_off[fi+1]=ks_flat.size();
        for(size_t i=0;i<occ;i++) occ_flat.push_back(local_of[out[i]]); occ_off[fi+1]=occ_flat.size(); } }
    fprintf(stderr,"F driver: TUs=%zu raw=%llu distinct=%u sock=%s\n",corpus.files.size(),(unsigned long long)corpus.raw,distinct,sock);
    std::vector<uint64_t> tudig(corpus.files.size());   // per-TU digest so the child verifies WITHOUT the corpus
    for(size_t fi=0; fi<corpus.files.size(); ++fi){ const auto&f=corpus.files[fi]; tudig[fi]=digest(corpus.bytes.data()+f.off,f.len); }

    // one job (ephemeral fork): connect -> send keyset -> recv text -> expand occ -> verify -> exit.
    auto do_job=[&](size_t fi)->int{
        int s=usock_conn(sock); if(s<0) return 5;
        uint32_t nk=(uint32_t)(ks_off[fi+1]-ks_off[fi]); const uint32_t* keys=ks_flat.data()+ks_off[fi];
        std::vector<uint8_t> req(4+size_t(nk)*4); memcpy(req.data(),&nk,4); if(nk)memcpy(req.data()+4,keys,size_t(nk)*4);
        if(!wrf(s,req.data(),(uint32_t)req.size())) return 4;
        std::vector<uint8_t> resp; if(!rdf(s,resp)) return 4; close(s);
        std::vector<const char*> tptr(nk); std::vector<uint32_t> tlen(nk); const uint8_t* p=resp.data();
        for(uint32_t i=0;i<nk;i++){ uint32_t n; memcpy(&n,p,4); p+=4; tlen[i]=n; tptr[i]=(const char*)p; p+=n; }
        std::string recon; const uint32_t* occ=occ_flat.data()+occ_off[fi]; size_t no=occ_off[fi+1]-occ_off[fi];
        for(size_t i=0;i<no;i++){ uint32_t lid=occ[i]; recon.append(tptr[lid],tlen[lid]); }
        return (recon.size()==corpus.files[fi].len && digest(recon.data(),recon.size())==tudig[fi])?0:1;
    };
    // children are ephemeral one-job forks and never touch the corpus -> exclude it from fork COW.
    { uintptr_t a=(uintptr_t)corpus.bytes.data(), pg=4096, aa=(a+pg-1)&~(pg-1);
      if(corpus.bytes.size()>pg) madvise((void*)aa, corpus.bytes.size()-(aa-a), MADV_DONTFORK); }

    // --- pure DECODE measurement: one process, ONE persistent connection. Separates the
    // server round-trip (get the text) from the actual reconstruction (expand occ->bytes). ---
    { int s=usock_conn(sock); double t_query=0,t_decode=0; uint64_t rb=0; bool ok=true;
      std::string recon; std::vector<const char*> tptr; std::vector<uint32_t> tlen; std::vector<uint8_t> req,resp;
      for(size_t fi=0; fi<corpus.files.size(); ++fi){
        uint32_t nk=(uint32_t)(ks_off[fi+1]-ks_off[fi]); const uint32_t* keys=ks_flat.data()+ks_off[fi];
        req.resize(4+size_t(nk)*4); memcpy(req.data(),&nk,4); if(nk)memcpy(req.data()+4,keys,size_t(nk)*4);
        auto q0=Clock::now(); if(!wrf(s,req.data(),(uint32_t)req.size())||!rdf(s,resp)){ok=false;break;} t_query+=seconds_since(q0);
        tptr.assign(nk,nullptr); tlen.assign(nk,0); const uint8_t* p=resp.data();
        for(uint32_t i=0;i<nk;i++){ uint32_t n; memcpy(&n,p,4); p+=4; tlen[i]=n; tptr[i]=(const char*)p; p+=n; }
        auto d0=Clock::now(); recon.clear(); const uint32_t* occ=occ_flat.data()+occ_off[fi]; size_t no=occ_off[fi+1]-occ_off[fi];
        for(size_t i=0;i<no;i++){ uint32_t lid=occ[i]; recon.append(tptr[lid],tlen[lid]); } t_decode+=seconds_since(d0); rb+=recon.size();
        const auto&f=corpus.files[fi]; if(recon.size()!=f.len||memcmp(recon.data(),corpus.bytes.data()+f.off,f.len)!=0){ok=false;break;}
      }
      close(s); double raw=corpus.raw/1e9;
      fprintf(stderr,"  DECODE-only(1 proc,persistent conn): reconstruct %.2f GB/s | server-roundtrip %.2f GB/s | verify=%s\n",
        raw/t_decode, raw/t_query, ok?"PASS":"FAIL"); }

    (void)do_job;
    // Cache-server saturation test: N concurrent handlers, each one persistent connection over
    // its own TU partition (query the SINGLE cache server for text + decode). Isolates whether
    // one cache owner scales; fork cost is amortized (N forks, not 1238).
    std::vector<uint64_t> cum(corpus.files.size()+1,0); for(size_t i=0;i<corpus.files.size();i++) cum[i+1]=cum[i]+corpus.files[i].len;
    auto do_part=[&](size_t lo,size_t hi)->int{
        int s=usock_conn(sock); if(s<0) return 5;
        std::vector<const char*> tptr; std::vector<uint32_t> tlen; std::vector<uint8_t> req,resp; std::string recon;
        for(size_t fi=lo; fi<hi; ++fi){
            uint32_t nk=(uint32_t)(ks_off[fi+1]-ks_off[fi]); const uint32_t* keys=ks_flat.data()+ks_off[fi];
            req.resize(4+size_t(nk)*4); memcpy(req.data(),&nk,4); if(nk)memcpy(req.data()+4,keys,size_t(nk)*4);
            if(!wrf(s,req.data(),(uint32_t)req.size())||!rdf(s,resp)){ close(s); return 4; }
            tptr.assign(nk,nullptr); tlen.assign(nk,0); const uint8_t* p=resp.data();
            for(uint32_t i=0;i<nk;i++){ uint32_t n; memcpy(&n,p,4); p+=4; tlen[i]=n; tptr[i]=(const char*)p; p+=n; }
            recon.clear(); const uint32_t* occ=occ_flat.data()+occ_off[fi]; size_t no=occ_off[fi+1]-occ_off[fi];
            for(size_t i=0;i<no;i++){ uint32_t lid=occ[i]; recon.append(tptr[lid],tlen[lid]); }
            if(recon.size()!=corpus.files[fi].len || digest(recon.data(),recon.size())!=tudig[fi]){ close(s); return 1; }
        }
        close(s); return 0;
    };
    for(int trial=0; ; trial++){ int Ns[6]={1,4,8,16,jobs>16?jobs:0,0}; int N=Ns[trial]; if(N<=0) break; if(trial>0&&N==Ns[trial-1]) continue;
        std::vector<pid_t> pids; auto t0=Clock::now();
        for(int w=0; w<N; w++){ uint64_t a=corpus.raw*uint64_t(w)/N,b=corpus.raw*uint64_t(w+1)/N;
            size_t lo=std::lower_bound(cum.begin(),cum.end(),a)-cum.begin(), hi=std::lower_bound(cum.begin(),cum.end(),b)-cum.begin();
            pid_t pid=fork(); if(pid==0){ _exit(do_part(lo,hi)); } pids.push_back(pid); }
        bool ok=true; for(pid_t p:pids){ int st=0; waitpid(p,&st,0); if(!(WIFEXITED(st)&&WEXITSTATUS(st)==0)) ok=false; }
        double dt=seconds_since(t0), raw=corpus.raw/1e9;
        fprintf(stderr,"  handlers=%-3d: %.3f s  aggregate %.2f GB/s  (query single cache-server + decode)  verify=%s\n",N,dt,raw/dt,ok?"PASS":"FAIL");
    }
    return 0;
}
