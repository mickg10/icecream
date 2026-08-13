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

static PassResult run_pass(Interner &dict, const Corpus &corpus, bool train_predictor) {
    PassResult r;
    r.raw = corpus.raw;
    std::vector<uint32_t> out;
    uint32_t max_len = 0;
    for (const auto &f : corpus.files) max_len = std::max(max_len, f.len);
    out.resize(size_t(max_len) + 1);
    uint64_t hit_count = 0;
    uint64_t regions_before = dict.region_count();
    dict.reset_counters();
    PerfCounters counters;
    counters.start();
    auto begin = Clock::now();
    for (const auto &f : corpus.files) {
        size_t out_count = 0;
        const char *p = corpus.bytes.data() + f.off;
#ifdef LINE_ONLY
        dict.process_linewise(p, p + f.len, out.data(), out_count);
#else
        dict.process(p, p + f.len, out.data(), out_count, hit_count, train_predictor);
#endif
        expose_output(out.data(), out_count);
        r.lines += out_count;
        r.sink += out_count;
        if (out_count) r.sink += out[0] + out[out_count - 1];
    }
    r.seconds = seconds_since(begin);
    counters.stop(r);
    r.region_hits = hit_count;
    r.regions = (dict.region_count() - regions_before) + hit_count;
    r.region_probes = dict.region_probes();
    r.line_probes = dict.line_probes();
    r.predictor_attempts = dict.predictor_attempts();
    r.predictor_hits = dict.predictor_hits();
    return r;
}

static bool validate_all(Interner &dict, const Corpus &corpus) {
    uint32_t max_len = 0;
    for (const auto &f : corpus.files) max_len = std::max(max_len, f.len);
    std::vector<uint32_t> out(size_t(max_len) + 1);
    uint64_t hits = 0, occurrences = 0;
    for (size_t fi = 0; fi < corpus.files.size(); ++fi) {
        const auto &f = corpus.files[fi];
        size_t out_count = 0;
        const char *src = corpus.bytes.data() + f.off;
#ifdef LINE_ONLY
        dict.process_linewise(src, src + f.len, out.data(), out_count);
#else
        dict.process(src, src + f.len, out.data(), out_count, hits, false);
#endif
        size_t pos = 0;
        for (size_t oi = 0; oi < out_count; ++oi) {
            uint32_t id = out[oi];
            if (!id || id > dict.distinct()) {
                fprintf(stderr, "validation invalid id at TU %zu\n", fi); return false;
            }
            const LineRef &ref = dict.ref(id);
            if (pos + ref.len > f.len ||
                memcmp(src + pos, dict.line_data(ref.off), ref.len) != 0) {
                fprintf(stderr, "validation byte mismatch at TU %zu offset %zu id %u\n",
                        fi, pos, id);
                return false;
            }
            pos += ref.len;
        }
        if (pos != f.len) {
            fprintf(stderr, "validation length mismatch at TU %zu: %zu != %u\n",
                    fi, pos, f.len);
            return false;
        }
        occurrences += out_count;
    }
    fprintf(stderr, "validation: exact TUs=%zu/%zu occurrences=%llu PASS\n",
            corpus.files.size(), corpus.files.size(), (unsigned long long)occurrences);
    return true;
}

static void report(const char *name, const PassResult &r) {
    double decimal_gbps = double(r.raw) / r.seconds / 1e9;
    double ns_per_line = r.lines ? r.seconds * 1e9 / double(r.lines) : 0;
    fprintf(stderr,
            "%s: %.6f s %.3f GB/s lines=%llu regions=%llu region_hits=%llu "
            "ns/line=%.3f region_probes/region=%.3f line_probes(new-region)=%.3f sink=%llu\n",
            name, r.seconds, decimal_gbps, (unsigned long long)r.lines,
            (unsigned long long)r.regions, (unsigned long long)r.region_hits,
            ns_per_line,
            r.regions ? double(r.region_probes) / r.regions : 0,
            r.lines ? double(r.line_probes) / r.lines : 0,
            (unsigned long long)r.sink);
    fprintf(stderr,
            "  predictor: hits=%llu attempts=%llu hit/region=%.2f%% hit/attempt=%.2f%%\n",
            (unsigned long long)r.predictor_hits,
            (unsigned long long)r.predictor_attempts,
            r.regions ? 100.0 * r.predictor_hits / r.regions : 0,
            r.predictor_attempts ? 100.0 * r.predictor_hits / r.predictor_attempts : 0);
    if (r.cycles) {
        fprintf(stderr,
                "  perf: cycles=%llu instructions=%llu branches=%llu branch_misses=%llu "
                "cache_misses=%llu cyc/byte=%.3f ins/byte=%.3f IPC=%.3f\n",
                (unsigned long long)r.cycles,
                (unsigned long long)r.instructions,
                (unsigned long long)r.branches,
                (unsigned long long)r.branch_misses,
                (unsigned long long)r.cache_misses,
                double(r.cycles) / r.raw, double(r.instructions) / r.raw,
                double(r.instructions) / r.cycles);
    } else {
        fprintf(stderr, "  perf: unavailable\n");
    }
}

int main(int argc, char **argv) {
    const char *manifest = nullptr;
    bool validate = false;
    bool poison = false;
    size_t max_files = SIZE_MAX;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
        else if (!strcmp(argv[i], "--validate")) validate = true;
        else if (!strcmp(argv[i], "--poison")) poison = true;
        else if (!strcmp(argv[i], "--max-files") && i + 1 < argc) {
            char *tail = nullptr;
            unsigned long long value = strtoull(argv[++i], &tail, 10);
            if (!tail || *tail || !value || value > SIZE_MAX) {
                fprintf(stderr, "invalid --max-files\n"); return 2;
            }
            max_files = size_t(value);
        }
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 2; }
    }
    if (!manifest) {
        fprintf(stderr,
                "usage: %s --manifest FILE [--validate] [--poison] [--max-files N]\n",
                argv[0]);
        return 2;
    }
    Corpus corpus = load_corpus(manifest, max_files);
    Interner dict;
    PassResult cold = run_pass(dict, corpus, true);
    PassResult hot = run_pass(dict, corpus, false);
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    fprintf(stderr, "mode=%s cpu=runtime-native TUs=%zu raw=%llu distinct=%u\n",
#ifdef LINE_ONLY
            "line-only",
#else
            "trace-replay",
#endif
            corpus.files.size(), (unsigned long long)corpus.raw, dict.distinct());
    report("COLD", cold);
    report("HOT", hot);
    uint64_t fixed_tables = sizeof(TinySlot) * Interner::TINY_CAP +
                            sizeof(ShortSlot) * Interner::SHORT_CAP +
                            sizeof(LineSlot) * Interner::LINE_CAP;
    uint64_t region_metadata_used = dict.region_index_bytes() + dict.region_record_bytes();
    uint64_t region_metadata_allocated =
        dict.region_index_bytes() + dict.region_record_capacity_bytes();
    uint64_t cache_used = fixed_tables + region_metadata_used + dict.line_bytes() +
                          dict.region_bytes() + dict.region_id_bytes();
    uint64_t cache_allocated = fixed_tables + region_metadata_allocated +
        dict.line_capacity_bytes() + dict.region_capacity_bytes() +
        dict.region_id_capacity_bytes() + dict.id_ref_capacity_bytes();
    fprintf(stderr,
            "memory: fixed_tables=%.2f MiB region_metadata_used=%.2f MiB line_arena_used=%.2f MiB "
            "region_arena_used=%.2f MiB region_ids_used=%.2f MiB cache_used=%.2f MiB "
            "cache_allocated=%.2f MiB peak_RSS_with_corpus=%.2f MiB\n",
            fixed_tables / 1048576.0, region_metadata_used / 1048576.0,
            dict.line_bytes() / 1048576.0,
            dict.region_bytes() / 1048576.0, dict.region_id_bytes() / 1048576.0,
            cache_used / 1048576.0, cache_allocated / 1048576.0,
            ru.ru_maxrss / 1024.0);
    bool is_llvm = corpus.files.size() == 1238 && corpus.raw == 3620271340ULL;
    bool shape_ok = !is_llvm || (cold.lines == 137913644ULL && dict.distinct() == 771055);
    fprintf(stderr, "corpus_gate: %s\n",
            is_llvm ? (shape_ok ? "PASS(LLVM)" : "FAIL(LLVM)") : "PASS(generic shape reported)");
    if (!shape_ok) return 1;
    if (validate && !validate_all(dict, corpus)) return 1;
    if (poison) {
        uint64_t modified_files = 0;
        Corpus changed = make_poisoned_corpus(corpus, modified_files);
        PassResult changed_pass = run_pass(dict, changed, true);
        fprintf(stderr, "POISON: inserted_one_line files=%llu added_bytes=%llu\n",
                (unsigned long long)modified_files,
                (unsigned long long)(changed.raw - corpus.raw));
        report("POISON", changed_pass);
        if (!validate_all(dict, changed)) return 1;
    }
    return 0;
}
