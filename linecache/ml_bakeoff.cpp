// ml_bakeoff.cpp -- issue #16 exact definition-coding ML capability harness.
//
// This is deliberately a codec benchmark, not a model-score benchmark.  It walks .ii files in
// chronological TU order, derives the same marker-delimited region stream used by Protocol 50,
// factors that stream into causal superblocks, and presents each first-seen exact Line with a
// bounded set of already-installed bases.  Several encoder-only selectors may rank those bases;
// the selected record is nevertheless an ordinary self-describing Literal, PrefixSuffix, or
// bounded COPY/ADD/RUN program.  A separate decoder sees only the framed bytes and its installed
// Line store, then reconstructs and byte-compares every complete TU.
//
// The first implementation row is the hashed sparse FTRL-Proximal ranker requested in BigOracle
// comment 5298035898.  The stable event export at the end of this file is shared with the PyTorch
// ranker/retriever and residual-model experiments.
//
// Build:
//   g++ -O3 -march=native -std=c++17 ml_bakeoff.cpp -o ml_bakeoff -lzstd -pthread


#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zstd.h>

using Clock = std::chrono::steady_clock;

static double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

[[noreturn]] static void fail(const char* what) {
    std::perror(what);
    std::exit(2);
}

[[noreturn]] static void fail_msg(const std::string& what) {
    std::fprintf(stderr, "%s\n", what.c_str());
    std::exit(2);
}

static inline uint64_t load64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

static inline uint64_t load_tail(const uint8_t* p, uint32_t n) {
    uint64_t v = 0;
    std::memcpy(&v, p, n);
    return v;
}

static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static uint64_t hash_bytes(const uint8_t* p, uint32_t n, uint64_t seed = 0) {
    constexpr uint64_t kMul = 0x9ddfea08eb382d69ULL;
    uint64_t h = mix64(seed ^ (uint64_t(n) * 0xa0761d6478bd642fULL));
    while (n >= 16) {
        h = mix64((h ^ load64(p)) * kMul);
        h = mix64((h ^ load64(p + 8)) * kMul);
        p += 16;
        n -= 16;
    }
    if (n >= 8) {
        h = mix64((h ^ load64(p)) * kMul);
        h = mix64((h ^ load64(p + n - 8)) * kMul);
    } else if (n) {
        h = mix64((h ^ load_tail(p, n)) * kMul);
    }
    return h | 1ULL;
}

static inline size_t varint_size(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

static inline void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(uint8_t(v) | 0x80);
        v >>= 7;
    }
    out.push_back(uint8_t(v));
}

static bool get_varint(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
    out = 0;
    unsigned shift = 0;
    while (p < end && shift <= 63) {
        const uint8_t b = *p++;
        out |= uint64_t(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
    }
    return false;
}

static std::vector<std::string> read_manifest(const char* manifest, size_t max_files) {
    FILE* f = std::fopen(manifest, "r");
    if (!f) fail(manifest);
    std::vector<std::string> paths;
    char line[16384];
    while (std::fgets(line, sizeof(line), f)) {
        size_t n = std::strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        struct stat st {};
        if (::stat(line, &st) != 0) fail(line);
        if (st.st_size < 0 || uint64_t(st.st_size) > UINT32_MAX)
            fail_msg(std::string("unsupported TU size: ") + line);
        paths.emplace_back(line);
        if (paths.size() == max_files) break;
    }
    std::fclose(f);
    return paths;
}

static std::vector<uint8_t> read_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) fail(path.c_str());
    struct stat st {};
    if (::fstat(::fileno(f), &st) != 0) fail(path.c_str());
    std::vector<uint8_t> data(size_t(st.st_size));
    if (!data.empty() && std::fread(data.data(), 1, data.size(), f) != data.size())
        fail_msg(std::string("short read: ") + path);
    std::fclose(f);
    return data;
}

struct Marker {
    std::string path;
    uint32_t logical_line = 0;
    uint32_t flags = 0;
};

// Parse only the exact conventional preprocessor marker form.  Everything else remains a literal.
static bool parse_marker(const uint8_t* s, uint32_t len, Marker& out) {
    if (len < 5 || s[0] != '#' || s[1] != ' ') return false;
    const uint8_t* p = s + 2;
    const uint8_t* end = s + len;
    if (p == end || *p < '0' || *p > '9') return false;
    uint64_t line = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        line = line * 10 + (*p++ - '0');
        if (line > UINT32_MAX) return false;
    }
    if (end - p < 3 || p[0] != ' ' || p[1] != '"') return false;
    p += 2;
    const uint8_t* path_begin = p;
    while (p < end && *p != '"') ++p;
    if (p == end) return false;
    out.path.assign(reinterpret_cast<const char*>(path_begin), size_t(p - path_begin));
    ++p;
    uint32_t flags = 0;
    unsigned slot = 0;
    while (p < end && *p == ' ') {
        ++p;
        if (p == end || *p < '0' || *p > '9' || slot >= 8) return false;
        uint32_t value = 0;
        while (p < end && *p >= '0' && *p <= '9') value = value * 10 + (*p++ - '0');
        flags ^= uint32_t(mix64(uint64_t(value) + 0x9e3779b97f4a7c15ULL * (++slot)));
    }
    if (p == end || *p != '\n' || p + 1 != end) return false;
    out.logical_line = uint32_t(line);
    out.flags = flags;
    return true;
}

static uint64_t skeleton_hash(const uint8_t* p, uint32_t n) {
    // Exact literal runs are retained.  Identifier/number runs become typed one-byte slots.
    uint64_t h = 0x6a09e667f3bcc909ULL;
    uint32_t i = 0;
    while (i < n) {
        const uint8_t c = p[i];
        const bool ident = (c == '_') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = c >= '0' && c <= '9';
        if (ident || digit) {
            const uint8_t tag = ident ? 0xf1 : 0xf2;
            h = mix64(h ^ tag);
            ++i;
            while (i < n) {
                const uint8_t d = p[i];
                const bool same = ident
                    ? ((d == '_') || (d >= 'A' && d <= 'Z') || (d >= 'a' && d <= 'z') ||
                       (d >= '0' && d <= '9'))
                    : (d >= '0' && d <= '9');
                if (!same) break;
                ++i;
            }
        } else {
            h = mix64(h ^ c);
            ++i;
        }
    }
    return h | 1ULL;
}

static uint64_t signature_hash(const uint8_t* p, uint32_t n) {
    uint64_t a = n <= 8 ? load_tail(p, n) : load64(p);
    uint64_t b = n <= 8 ? a : load64(p + n - 8);
    uint64_t q = 0;
    if (n >= 4) {
        q = UINT64_MAX;
        for (uint32_t i = 0; i + 4 <= n; i += std::max<uint32_t>(1, n / 8)) {
            uint32_t x = 0;
            std::memcpy(&x, p + i, 4);
            q = std::min(q, mix64(x));
        }
    }
    return mix64((uint64_t(n / 8) << 56) ^ a ^ (b * 0x9e3779b97f4a7c15ULL) ^ q) | 1ULL;
}

static uint64_t qgram_sketch(const uint8_t* p, uint32_t n) {
    uint64_t sketch = 0;
    if (n < 4) return uint64_t(1) << (hash_bytes(p, n) & 63);
    const uint32_t step = std::max<uint32_t>(1, n / 16);
    for (uint32_t i = 0; i + 4 <= n; i += step) {
        uint32_t gram = 0;
        std::memcpy(&gram, p + i, 4);
        sketch |= uint64_t(1) << (mix64(gram) & 63);
    }
    return sketch;
}

class ExactLineStore {
public:
    struct Ref { uint32_t off = 0, len = 0; };
    struct Result { uint32_t id = 0; bool first = false; };

    ExactLineStore() { rehash(1u << 20); refs_.push_back({}); }

    Result intern(const uint8_t* p, uint32_t n) {
        if (count_ * 10 > table_.size() * 7) rehash(table_.size() * 2);
        const uint64_t h = hash_bytes(p, n);
        uint32_t slot = uint32_t(h) & mask_;
        for (;;) {
            Cell& c = table_[slot];
            if (!c.hash) {
                if (arena_.size() + n > UINT32_MAX) fail_msg("Line arena exceeds 4 GiB");
                const uint32_t id = uint32_t(refs_.size());
                const uint32_t off = uint32_t(arena_.size());
                arena_.insert(arena_.end(), p, p + n);
                refs_.push_back({off, n});
                c = {h, id};
                ++count_;
                return {id, true};
            }
            const Ref& r = refs_[c.id];
            if (c.hash == h && r.len == n && std::memcmp(arena_.data() + r.off, p, n) == 0)
                return {c.id, false};
            slot = (slot + 1) & mask_;
        }
    }

    const uint8_t* data(uint32_t id) const { return arena_.data() + refs_.at(id).off; }
    uint32_t len(uint32_t id) const { return refs_.at(id).len; }
    const Ref& ref(uint32_t id) const { return refs_.at(id); }
    const std::vector<uint8_t>& arena() const { return arena_; }
    size_t size() const { return refs_.size() - 1; }

private:
    struct Cell { uint64_t hash = 0; uint32_t id = 0; };
    void rehash(size_t n) {
        std::vector<Cell> next(n);
        const uint32_t mask = uint32_t(n - 1);
        for (const Cell& c : table_) if (c.hash) {
            uint32_t slot = uint32_t(c.hash) & mask;
            while (next[slot].hash) slot = (slot + 1) & mask;
            next[slot] = c;
        }
        table_.swap(next);
        mask_ = mask;
    }
    std::vector<Cell> table_;
    std::vector<Ref> refs_;
    std::vector<uint8_t> arena_;
    uint32_t mask_ = 0;
    size_t count_ = 0;
};

class StringIds {
public:
    uint32_t intern(const std::string& s) {
        auto [it, inserted] = ids_.emplace(s, uint32_t(ids_.size()));
        return it->second;
    }
private:
    std::unordered_map<std::string, uint32_t> ids_;
};

struct RegionOccurrence {
    const uint8_t* begin = nullptr;
    uint32_t len = 0;
    uint64_t hash = 0;
    uint64_t hash2 = 0;
    uint32_t ordinal = 0;
    uint64_t block_context = 0;
};

// Causal LZ factorization over exact region content hashes.  The current TU is queried against
// prior TUs, then published only after its complete factorization is fixed.
class SuperblockContext {
public:
    std::vector<uint64_t> factor(const std::vector<uint64_t>& root) {
        constexpr uint32_t kMin = 3;
        constexpr uint32_t kMaxChain = 64;
        std::vector<uint64_t> contexts(root.size());
        size_t i = 0;
        while (i < root.size()) {
            size_t best_len = 0;
            if (i + kMin <= root.size()) {
                const uint64_t key = kgram(root.data() + i, kMin);
                auto it = heads_.find(key);
                if (it != heads_.end()) {
                    uint32_t pos = it->second;
                    uint32_t chain = 0;
                    while (pos != UINT32_MAX && chain++ < kMaxChain) {
                        size_t n = 0;
                        while (i + n < root.size() && size_t(pos) + n < history_.size() &&
                               root[i + n] == history_[pos + n]) ++n;
                        if (n > best_len) best_len = n;
                        pos = previous_[pos];
                    }
                }
            }
            if (best_len >= kMin) {
                uint64_t h = mix64(best_len ^ 0x5355504552424c4bULL);
                for (size_t j = 0; j < best_len; ++j) h = mix64(h ^ root[i + j]);
                for (size_t j = 0; j < best_len; ++j) contexts[i + j] = h | 1ULL;
                i += best_len;
            } else {
                contexts[i] = mix64(root[i] ^ 0x524547494f4eULL) | 1ULL;
                ++i;
            }
        }

        const uint32_t base = uint32_t(history_.size());
        history_.insert(history_.end(), root.begin(), root.end());
        previous_.resize(history_.size(), UINT32_MAX);
        for (uint32_t j = 0; j < root.size(); ++j) {
            const uint32_t pos = base + j;
            if (j + kMin > root.size()) continue;
            const uint64_t key = kgram(root.data() + j, kMin);
            auto [it, inserted] = heads_.emplace(key, pos);
            if (!inserted) {
                previous_[pos] = it->second;
                it->second = pos;
            }
        }
        return contexts;
    }

private:
    static uint64_t kgram(const uint64_t* p, uint32_t n) {
        uint64_t h = 0xcbf29ce484222325ULL;
        for (uint32_t i = 0; i < n; ++i) h = mix64(h ^ p[i]);
        return h | 1ULL;
    }
    std::vector<uint64_t> history_;
    std::vector<uint32_t> previous_;
    std::unordered_map<uint64_t, uint32_t> heads_;
};

enum CandidateSource : uint8_t {
    kSourceLocation = 1u << 0,
    kSourceSkeleton = 1u << 1,
    kSourceContext  = 1u << 2,
    kSourceSignature = 1u << 3,
    kSourceRecent = 1u << 4,
    kSourceStatic = 1u << 5,
};

struct LineContext {
    uint32_t tu = 0;
    uint32_t line_id = 0;
    uint32_t path_id = 0;
    uint32_t logical_line = 0;
    uint32_t marker_flags = 0;
    uint32_t previous_line = 0;
    uint32_t previous2_line = 0;
    uint32_t region_ordinal = 0;
    uint64_t region_hash = 0;
    uint64_t block_context = 0;
    uint64_t skeleton = 0;
    uint64_t signature = 0;
    uint64_t qgram_sketch = 0;
};

struct Candidate {
    uint32_t base_id = 0;
    uint8_t sources = 0;
    uint32_t age = 0;
    uint32_t prefix = 0;
    uint32_t suffix = 0;
    uint32_t qgram_hits = 0;
    uint32_t prefix_record_bytes = UINT32_MAX;
    uint32_t program_record_bytes = UINT32_MAX;
    int32_t reward = 0;
};

template <size_t N>
struct RecentIds {
    std::array<uint32_t, N> ids{};
    uint8_t used = 0;
    void push(uint32_t id) {
        for (uint8_t i = 0; i < used; ++i) if (ids[i] == id) return;
        if (used < N) ++used;
        for (uint8_t i = used - 1; i > 0; --i) ids[i] = ids[i - 1];
        ids[0] = id;
    }
};

static uint64_t source_key(const LineContext& c) {
    uint64_t h = mix64((uint64_t(c.path_id) << 32) | c.logical_line);
    return mix64(h ^ c.marker_flags) | 1ULL;
}

static uint64_t context_key(const LineContext& c) {
    uint64_t h = mix64((uint64_t(c.previous2_line) << 32) | c.previous_line);
    return mix64(h ^ c.block_context) | 1ULL;
}

class CandidateIndex {
public:
    std::vector<Candidate> query(const LineContext& c, uint32_t next_line_id) const {
        std::vector<Candidate> out;
        out.reserve(20);
        auto add = [&](uint32_t id, uint8_t source) {
            if (!id || id >= next_line_id) return;
            for (Candidate& x : out) if (x.base_id == id) { x.sources |= source; return; }
            if (out.size() < 24) out.push_back({id, source, next_line_id - id});
        };
        auto take = [&](const auto& map, uint64_t key, uint8_t source) {
            auto it = map.find(key);
            if (it == map.end()) return;
            for (uint8_t i = 0; i < it->second.used; ++i) add(it->second.ids[i], source);
        };
        take(by_source_, source_key(c), kSourceLocation);
        take(by_skeleton_, c.skeleton, kSourceSkeleton);
        take(by_context_, context_key(c), kSourceContext);
        take(by_signature_, c.signature, kSourceSignature);
        for (uint8_t i = 0; i < recent_.used; ++i) add(recent_.ids[i], kSourceRecent);
        return out;
    }

    void publish(const LineContext& c) {
        by_source_[source_key(c)].push(c.line_id);
        by_skeleton_[c.skeleton].push(c.line_id);
        by_context_[context_key(c)].push(c.line_id);
        by_signature_[c.signature].push(c.line_id);
        recent_.push(c.line_id);
    }

private:
    std::unordered_map<uint64_t, RecentIds<4>> by_source_;
    std::unordered_map<uint64_t, RecentIds<4>> by_skeleton_;
    std::unordered_map<uint64_t, RecentIds<4>> by_context_;
    std::unordered_map<uint64_t, RecentIds<4>> by_signature_;
    RecentIds<4> recent_;
};

static uint32_t common_prefix(const uint8_t* a, uint32_t an, const uint8_t* b, uint32_t bn) {
    uint32_t i = 0;
    const uint32_t n = std::min(an, bn);
    while (i + 8 <= n && load64(a + i) == load64(b + i)) i += 8;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

static uint32_t common_suffix(const uint8_t* a, uint32_t an, const uint8_t* b, uint32_t bn,
                              uint32_t prefix) {
    uint32_t i = 0;
    const uint32_t n = std::min(an - std::min(an, prefix), bn - std::min(bn, prefix));
    while (i < n && a[an - 1 - i] == b[bn - 1 - i]) ++i;
    return i;
}

static uint32_t qgram_hits(uint64_t target, uint64_t base) {
    return uint32_t(__builtin_popcountll(target & base));
}

enum class RecordMode : uint8_t { Literal = 0, PrefixSuffix = 1, Program = 2 };
enum class ProgramOp : uint8_t { Add = 0, Copy = 1, Run = 2 };

struct Op {
    ProgramOp kind = ProgramOp::Add;
    uint32_t a = 0;
    uint32_t b = 0;
};

struct Encoding {
    RecordMode mode = RecordMode::Literal;
    uint32_t base_id = 0;
    uint32_t prefix = 0;
    uint32_t suffix = 0;
    std::vector<Op> ops;
    uint32_t serialized_bytes = 0;
};

static uint32_t literal_size(uint32_t line_id, uint32_t len) {
    return uint32_t(1 + varint_size(line_id) + varint_size(len) + len);
}

static uint32_t prefix_size(uint32_t line_id, uint32_t base_id, uint32_t len,
                            uint32_t prefix, uint32_t suffix) {
    const uint32_t middle = len - prefix - suffix;
    return uint32_t(1 + varint_size(line_id) + varint_size(base_id) + varint_size(len) +
                    varint_size(prefix) + varint_size(suffix) + varint_size(middle) + middle);
}

static uint32_t op_size(const Op& op) {
    switch (op.kind) {
        case ProgramOp::Add:  return uint32_t(1 + varint_size(op.b) + op.b);
        case ProgramOp::Copy: return uint32_t(1 + varint_size(op.a) + varint_size(op.b));
        case ProgramOp::Run:  return uint32_t(2 + varint_size(op.b));
    }
    return UINT32_MAX;
}

// A bounded shortest path. COPY edges retain up to four exact base offsets per target position.
// Literal bytes advance one vertex at a time and consecutive literal steps are merged into one ADD
// during reconstruction.  The DP therefore uses a one-byte approximation for ADD setup, but the
// final competition always compares the actual serialized record.  This keeps candidate labeling
// linear in target length times bounded base probes instead of quadratic in every possible ADD.
static bool make_program(const uint8_t* target, uint32_t tn, const uint8_t* base, uint32_t bn,
                         uint32_t line_id, uint32_t base_id, Encoding& out) {
    constexpr uint32_t kMaxLine = 512;
    constexpr uint32_t kMaxOps = 12;
    constexpr uint32_t kMinCopy = 4;
    if (!tn || tn > kMaxLine || bn < kMinCopy || bn > 65536) return false;
    const uint32_t inf = UINT32_MAX / 4;
    std::vector<uint32_t> dp(tn + 1, inf);
    struct Back { uint16_t pos = 0; Op op{}; bool set = false; };
    std::vector<Back> back(tn + 1);
    dp[0] = 0;
    auto relax = [&](uint32_t pos, uint32_t end, const Op& op, uint32_t edge_cost) {
        const uint32_t cur = dp[pos];
        if (cur == inf || end > tn || end <= pos) return;
        const uint32_t cost = cur + edge_cost;
        if (cost < dp[end]) {
            dp[end] = cost;
            back[end] = {uint16_t(pos), op, true};
        }
    };

    for (uint32_t i = 0; i < tn; ++i) {
        if (dp[i] == inf) continue;
        relax(i, i + 1, {ProgramOp::Add, i, 1}, 1);

        uint32_t run = 1;
        while (i + run < tn && target[i + run] == target[i]) ++run;
        if (run >= 4) {
            const Op op{ProgramOp::Run, target[i], run};
            relax(i, i + run, op, op_size(op));
        }

        uint32_t kept = 0;
        for (uint32_t j = 0; j + kMinCopy <= bn && kept < 4; ++j) {
            if (i + kMinCopy > tn || std::memcmp(target + i, base + j, kMinCopy) != 0) continue;
            uint32_t n = kMinCopy;
            while (i + n < tn && j + n < bn && target[i + n] == base[j + n]) ++n;
            const Op op{ProgramOp::Copy, j, n};
            relax(i, i + n, op, op_size(op));
            ++kept;
        }
    }

    if (dp[tn] == inf) return false;
    std::vector<Op> rev;
    uint32_t pos = tn;
    while (pos) {
        const Back& b = back[pos];
        if (!b.set) return false;
        if (b.op.kind == ProgramOp::Add && !rev.empty() && rev.back().kind == ProgramOp::Add &&
            b.op.a + b.op.b == rev.back().a) {
            rev.back().a = b.op.a;
            rev.back().b += b.op.b;
        } else {
            rev.push_back(b.op);
        }
        pos = b.pos;
    }
    std::reverse(rev.begin(), rev.end());
    if (rev.size() > kMaxOps) return false;
    uint32_t bytes = uint32_t(1 + varint_size(line_id) + varint_size(base_id) +
                              varint_size(tn) + varint_size(rev.size()));
    for (const Op& op : rev) bytes += op_size(op);
    out.mode = RecordMode::Program;
    out.base_id = base_id;
    out.ops = std::move(rev);
    out.serialized_bytes = bytes;
    return true;
}

static Encoding best_against(const ExactLineStore& store, uint32_t line_id, uint32_t base_id,
                             bool run_program) {
    const uint8_t* target = store.data(line_id);
    const uint32_t tn = store.len(line_id);
    const uint8_t* base = store.data(base_id);
    const uint32_t bn = store.len(base_id);
    const uint32_t prefix = common_prefix(target, tn, base, bn);
    const uint32_t suffix = common_suffix(target, tn, base, bn, prefix);
    Encoding best;
    best.mode = RecordMode::PrefixSuffix;
    best.base_id = base_id;
    best.prefix = prefix;
    best.suffix = suffix;
    best.serialized_bytes = prefix_size(line_id, base_id, tn, prefix, suffix);
    if (run_program) {
        Encoding program;
        if (make_program(target, tn, base, bn, line_id, base_id, program) &&
            program.serialized_bytes < best.serialized_bytes) best = std::move(program);
    }
    return best;
}

static void serialize_record(const ExactLineStore& store, uint32_t line_id, const Encoding& enc,
                             std::vector<uint8_t>& out) {
    const uint8_t* target = store.data(line_id);
    const uint32_t len = store.len(line_id);
    out.push_back(uint8_t(enc.mode));
    put_varint(out, line_id);
    if (enc.mode == RecordMode::Literal) {
        put_varint(out, len);
        out.insert(out.end(), target, target + len);
        return;
    }
    put_varint(out, enc.base_id);
    put_varint(out, len);
    if (enc.mode == RecordMode::PrefixSuffix) {
        put_varint(out, enc.prefix);
        put_varint(out, enc.suffix);
        const uint32_t middle = len - enc.prefix - enc.suffix;
        put_varint(out, middle);
        out.insert(out.end(), target + enc.prefix, target + enc.prefix + middle);
        return;
    }
    put_varint(out, enc.ops.size());
    for (const Op& op : enc.ops) {
        out.push_back(uint8_t(op.kind));
        if (op.kind == ProgramOp::Add) {
            put_varint(out, op.b);
            out.insert(out.end(), target + op.a, target + op.a + op.b);
        } else if (op.kind == ProgramOp::Copy) {
            put_varint(out, op.a);
            put_varint(out, op.b);
        } else {
            out.push_back(uint8_t(op.a));
            put_varint(out, op.b);
        }
    }
}

class DecoderLineStore {
public:
    DecoderLineStore() { refs_.push_back({}); }

    bool decode_frame(const uint8_t* p, size_t n) {
        const uint8_t* end = p + n;
        uint64_t count = 0;
        if (!get_varint(p, end, count) || count > (1u << 24)) return false;
        for (uint64_t i = 0; i < count; ++i) if (!decode_record(p, end)) return false;
        return p == end;
    }

    bool verify_tu(const std::vector<uint32_t>& ids, const uint8_t* original, size_t n) const {
        size_t off = 0;
        for (uint32_t id : ids) {
            if (!id || id >= refs_.size()) return false;
            const Ref& r = refs_[id];
            if (off + r.len > n || std::memcmp(arena_.data() + r.off, original + off, r.len) != 0)
                return false;
            off += r.len;
        }
        return off == n;
    }

    const uint8_t* data(uint32_t id) const {
        return id < refs_.size() ? arena_.data() + refs_[id].off : nullptr;
    }
    uint32_t len(uint32_t id) const { return id < refs_.size() ? refs_[id].len : 0; }
    size_t size() const { return refs_.size() - 1; }
    size_t bytes() const { return arena_.size(); }

private:
    struct Ref { uint32_t off = 0, len = 0; };

    bool append(uint32_t id, const std::vector<uint8_t>& line) {
        if (!id || id > refs_.size()) return false;
        if (id < refs_.size()) {
            const Ref& r = refs_[id];
            return r.len == line.size() &&
                   std::memcmp(arena_.data() + r.off, line.data(), line.size()) == 0;
        }
        if (arena_.size() + line.size() > UINT32_MAX) return false;
        const uint32_t off = uint32_t(arena_.size());
        arena_.insert(arena_.end(), line.begin(), line.end());
        refs_.push_back({off, uint32_t(line.size())});
        return true;
    }

    bool decode_record(const uint8_t*& p, const uint8_t* end) {
        if (p == end) return false;
        const uint8_t raw_mode = *p++;
        if (raw_mode > uint8_t(RecordMode::Program)) return false;
        const RecordMode mode = RecordMode(raw_mode);
        uint64_t id64 = 0, len64 = 0;
        if (!get_varint(p, end, id64) || !id64 || id64 > UINT32_MAX) return false;
        const uint32_t id = uint32_t(id64);
        std::vector<uint8_t> line;
        if (mode == RecordMode::Literal) {
            if (!get_varint(p, end, len64) || len64 > uint64_t(end - p) || len64 > UINT32_MAX)
                return false;
            line.assign(p, p + len64);
            p += len64;
            return append(id, line);
        }

        uint64_t base64 = 0;
        if (!get_varint(p, end, base64) || !base64 || base64 >= refs_.size() ||
            !get_varint(p, end, len64) || len64 > UINT32_MAX) return false;
        const uint32_t base_id = uint32_t(base64);
        const Ref br = refs_[base_id];
        const uint8_t* base = arena_.data() + br.off;
        line.reserve(size_t(len64));
        if (mode == RecordMode::PrefixSuffix) {
            uint64_t prefix = 0, suffix = 0, middle = 0;
            if (!get_varint(p, end, prefix) || !get_varint(p, end, suffix) ||
                !get_varint(p, end, middle) || prefix > br.len || suffix > br.len - prefix ||
                prefix + suffix > len64 || middle != len64 - prefix - suffix ||
                middle > uint64_t(end - p)) return false;
            line.insert(line.end(), base, base + prefix);
            line.insert(line.end(), p, p + middle);
            p += middle;
            line.insert(line.end(), base + br.len - suffix, base + br.len);
            return line.size() == len64 && append(id, line);
        }

        uint64_t count = 0;
        if (!get_varint(p, end, count) || count > 16) return false;
        for (uint64_t k = 0; k < count; ++k) {
            if (p == end) return false;
            const uint8_t raw_op = *p++;
            if (raw_op > uint8_t(ProgramOp::Run)) return false;
            const ProgramOp op = ProgramOp(raw_op);
            if (op == ProgramOp::Add) {
                uint64_t add = 0;
                if (!get_varint(p, end, add) || add > uint64_t(end - p) ||
                    line.size() + add > len64) return false;
                line.insert(line.end(), p, p + add);
                p += add;
            } else if (op == ProgramOp::Copy) {
                uint64_t off = 0, copy = 0;
                if (!get_varint(p, end, off) || !get_varint(p, end, copy) || off > br.len ||
                    copy > br.len - off || line.size() + copy > len64) return false;
                line.insert(line.end(), base + off, base + off + copy);
            } else {
                if (p == end) return false;
                const uint8_t byte = *p++;
                uint64_t run = 0;
                if (!get_varint(p, end, run) || line.size() + run > len64) return false;
                line.insert(line.end(), size_t(run), byte);
            }
        }
        return line.size() == len64 && append(id, line);
    }

    std::vector<Ref> refs_;
    std::vector<uint8_t> arena_;
};

class ZstdFrames {
public:
    explicit ZstdFrames(int level) : level_(level) {
        cctx_ = ZSTD_createCCtx();
        dctx_ = ZSTD_createDCtx();
        if (!cctx_ || !dctx_) fail_msg("cannot create zstd contexts");
    }
    ~ZstdFrames() {
        ZSTD_freeCCtx(cctx_);
        ZSTD_freeDCtx(dctx_);
    }
    ZstdFrames(const ZstdFrames&) = delete;
    ZstdFrames& operator=(const ZstdFrames&) = delete;
    ZstdFrames(ZstdFrames&& other) noexcept
        : level_(other.level_), cctx_(other.cctx_), dctx_(other.dctx_) {
        other.cctx_ = nullptr;
        other.dctx_ = nullptr;
    }
    ZstdFrames& operator=(ZstdFrames&& other) noexcept {
        if (this == &other) return *this;
        ZSTD_freeCCtx(cctx_);
        ZSTD_freeDCtx(dctx_);
        level_ = other.level_;
        cctx_ = other.cctx_;
        dctx_ = other.dctx_;
        other.cctx_ = nullptr;
        other.dctx_ = nullptr;
        return *this;
    }

    bool compress(const std::vector<uint8_t>& raw, std::vector<uint8_t>& compressed) {
        ZSTD_CCtx_reset(cctx_, ZSTD_reset_session_and_parameters);
        ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, level_);
        compressed.resize(ZSTD_compressBound(raw.size()));
        const size_t cn = ZSTD_compress2(cctx_, compressed.data(), compressed.size(),
                                         raw.empty() ? nullptr : raw.data(), raw.size());
        if (ZSTD_isError(cn)) return false;
        compressed.resize(cn);
        return true;
    }

    bool decompress(const std::vector<uint8_t>& compressed, size_t raw_size,
                    std::vector<uint8_t>& decoded) {
        decoded.resize(raw_size);
        ZSTD_DCtx_reset(dctx_, ZSTD_reset_session_only);
        const size_t dn = ZSTD_decompressDCtx(dctx_, decoded.data(), decoded.size(),
                                              compressed.data(), compressed.size());
        return !ZSTD_isError(dn) && dn == raw_size;
    }

private:
    int level_ = 1;
    ZSTD_CCtx* cctx_ = nullptr;
    ZSTD_DCtx* dctx_ = nullptr;
};

struct Feature { uint32_t index = 0; float value = 0; };

class FtrlRanker {
public:
    struct Params {
        uint32_t bits = 20;
        float alpha = 0.08f;
        float beta = 1.0f;
        float l1 = 0.05f;
        float l2 = 1.0f;
    };

    FtrlRanker() : FtrlRanker(Params{}) {}
    explicit FtrlRanker(const Params& p) : params_(p) {
        const size_t n = size_t(1) << params_.bits;
        z_.assign(n, 0.0f);
        n_.assign(n, 0.0f);
        mask_ = uint32_t(n - 1);
    }

    float predict(const std::vector<Feature>& features) const {
        double score = 0;
        for (const Feature& f : features) score += weight(f.index) * f.value;
        return float(std::max(-1024.0, std::min(8192.0, score)));
    }

    void update(const std::vector<Feature>& features, float label, float prediction) {
        const float error = std::max(-4096.0f, std::min(4096.0f, prediction - label));
        for (const Feature& f : features) {
            const uint32_t i = f.index & mask_;
            const float g = error * f.value;
            const float w = weight(i);
            const float sigma = (std::sqrt(n_[i] + g * g) - std::sqrt(n_[i])) / params_.alpha;
            z_[i] += g - sigma * w;
            n_[i] += g * g;
        }
        ++updates_;
    }

    size_t bytes() const { return (z_.size() + n_.size()) * sizeof(float); }
    uint64_t updates() const { return updates_; }

private:
    float weight(uint32_t raw) const {
        const uint32_t i = raw & mask_;
        const float z = z_[i];
        if (std::fabs(z) <= params_.l1) return 0.0f;
        const float sign = z < 0 ? -1.0f : 1.0f;
        return -(z - sign * params_.l1) /
               ((params_.beta + std::sqrt(n_[i])) / params_.alpha + params_.l2);
    }
    Params params_;
    uint32_t mask_ = 0;
    std::vector<float> z_, n_;
    uint64_t updates_ = 0;
};

static inline uint32_t feature_index(uint64_t kind, uint64_t value) {
    return uint32_t(mix64((kind * 0x9e3779b97f4a7c15ULL) ^ value));
}

static std::vector<Feature> make_features(const LineContext& target, const LineContext& base,
                                          const Candidate& c, uint32_t target_len,
                                          uint32_t base_len) {
    std::vector<Feature> f;
    f.reserve(28);
    auto cat = [&](uint64_t kind, uint64_t value) { f.push_back({feature_index(kind, value), 1.0f}); };
    auto num = [&](uint64_t kind, float value) { f.push_back({feature_index(kind, 0), value}); };
    cat(1, 0); // bias
    for (unsigned bit = 0; bit < 8; ++bit) if (c.sources & (1u << bit)) cat(2, bit);
    cat(3, std::min<uint32_t>(15, 31u - uint32_t(__builtin_clz(std::max(1u, target_len)))));
    cat(4, std::min<uint32_t>(15, 31u - uint32_t(__builtin_clz(std::max(1u, base_len)))));
    cat(5, std::min<uint32_t>(15, 31u - uint32_t(__builtin_clz(std::max(1u, c.age)))));
    cat(6, std::min<uint32_t>(15, c.prefix / 4));
    cat(7, std::min<uint32_t>(15, c.suffix / 4));
    cat(8, std::min<uint32_t>(15, c.qgram_hits));
    cat(9, target.path_id == base.path_id);
    cat(10, target.logical_line == base.logical_line);
    cat(11, target.marker_flags == base.marker_flags);
    cat(12, target.skeleton == base.skeleton);
    cat(13, target.block_context == base.block_context);
    cat(14, target.region_hash == base.region_hash);
    cat(15, uint32_t(target.signature == base.signature));
    cat(16, (uint64_t(c.sources) << 8) | std::min<uint32_t>(15, c.prefix / 4));
    cat(17, (uint64_t(target.block_context) & 0xffff) ^ (uint64_t(c.sources) << 16));
    num(20, std::min(4.0f, float(target_len) / 128.0f));
    num(21, std::min(4.0f, float(base_len) / 128.0f));
    num(22, std::min(1.0f, float(c.prefix) / std::max(1u, target_len)));
    num(23, std::min(1.0f, float(c.suffix) / std::max(1u, target_len)));
    num(24, std::min(1.0f, float(c.qgram_hits) / 8.0f));
    num(25, std::max(-4.0f, std::min(4.0f, float(int64_t(target_len) - base_len) / 64.0f)));
    return f;
}

struct Event {
    LineContext context;
    std::vector<Candidate> candidates;
    std::vector<float> ftrl_scores;
    std::vector<Encoding> encodings;
    std::vector<uint8_t> encoding_ready;
};

enum class Selector { Literal, Cheap, Ftrl, Oracle };

struct Row {
    std::string name;
    Selector selector = Selector::Literal;
    uint32_t k = 0;
    DecoderLineStore decoder;
    ZstdFrames frames;
    uint64_t wire = 0;
    uint64_t raw_record_bytes = 0;
    uint64_t literal_records = 0;
    uint64_t prefix_records = 0;
    uint64_t program_records = 0;
    uint64_t literal_fallback_frames = 0;
    double encode_seconds = 0;
    double decode_seconds = 0;
    bool exact = true;

    Row(std::string n, Selector s, uint32_t topk, int zlevel)
        : name(std::move(n)), selector(s), k(topk), frames(zlevel) {}
};

static Encoding literal_encoding(const ExactLineStore& store, uint32_t line_id) {
    Encoding e;
    e.mode = RecordMode::Literal;
    e.serialized_bytes = literal_size(line_id, store.len(line_id));
    return e;
}

static std::vector<uint32_t> ranked_candidates(const Event& event, Selector selector) {
    std::vector<uint32_t> order(event.candidates.size());
    std::iota(order.begin(), order.end(), 0);
    if (selector == Selector::Ftrl) {
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return event.ftrl_scores[a] > event.ftrl_scores[b];
        });
    } else if (selector == Selector::Cheap) {
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            const Candidate& x = event.candidates[a];
            const Candidate& y = event.candidates[b];
            const int64_t xs = int64_t(x.prefix + x.suffix) * 16 + x.qgram_hits * 3 -
                               int64_t(std::min<uint32_t>(x.age, 65535)) / 1024;
            const int64_t ys = int64_t(y.prefix + y.suffix) * 16 + y.qgram_hits * 3 -
                               int64_t(std::min<uint32_t>(y.age, 65535)) / 1024;
            return xs > ys;
        });
    } else if (selector == Selector::Oracle) {
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            const Candidate& x = event.candidates[a];
            const Candidate& y = event.candidates[b];
            return std::min(x.prefix_record_bytes, x.program_record_bytes) <
                   std::min(y.prefix_record_bytes, y.program_record_bytes);
        });
    }
    return order;
}

static const Encoding& candidate_encoding(const ExactLineStore& store, Event& event, uint32_t index,
                                          bool run_program) {
    if (event.encodings.empty()) {
        event.encodings.resize(event.candidates.size());
        event.encoding_ready.assign(event.candidates.size(), 0);
    }
    const uint8_t required = run_program ? 2 : 1;
    if (event.encoding_ready[index] < required) {
        Encoding e = best_against(store, event.context.line_id, event.candidates[index].base_id,
                                  run_program);
        if (!event.encoding_ready[index] || e.serialized_bytes < event.encodings[index].serialized_bytes)
            event.encodings[index] = std::move(e);
        event.encoding_ready[index] = required;
        Candidate& c = event.candidates[index];
        c.program_record_bytes = event.encodings[index].serialized_bytes;
        const uint32_t literal = literal_size(event.context.line_id, store.len(event.context.line_id));
        c.reward = int32_t(literal) - int32_t(std::min(c.prefix_record_bytes, c.program_record_bytes));
    }
    return event.encodings[index];
}

static Encoding choose_encoding(const ExactLineStore& store, Event& event, const Row& row) {
    Encoding best = literal_encoding(store, event.context.line_id);
    if (row.selector == Selector::Literal || event.candidates.empty()) return best;
    std::vector<uint32_t> order = ranked_candidates(event, row.selector);
    const size_t limit = row.selector == Selector::Oracle ? order.size()
                                                          : std::min<size_t>(row.k, order.size());
    for (size_t i = 0; i < limit; ++i) {
        const Encoding& candidate = candidate_encoding(store, event, order[i], true);
        if (candidate.serialized_bytes < best.serialized_bytes) best = candidate;
    }
    return best;
}

class EventExporter {
public:
    explicit EventExporter(const std::string& path) {
        if (path.empty()) return;
        out_.open(path, std::ios::binary | std::ios::trunc);
        if (!out_) fail_msg("cannot open event export: " + path);
        const char magic[8] = {'I','C','M','L','D','S','2','\0'};
        out_.write(magic, sizeof(magic));
        write_u32(2); // schema version
    }

    bool enabled() const { return out_.is_open(); }

    void write_tu(uint32_t tu, uint64_t raw_bytes, const std::vector<RegionOccurrence>& regions) {
        if (!enabled()) return;
        write_u8(1);
        write_u32(tu);
        write_u64(raw_bytes);
        write_u32(uint32_t(regions.size()));
        for (const RegionOccurrence& r : regions) {
            write_u64(r.hash);
            write_u64(r.hash2);
            write_u64(r.block_context);
            write_u32(r.len);
        }
        ++tu_count_;
    }

    void write_event(const ExactLineStore& store, const Event& e) {
        if (!enabled()) return;
        write_u8(2);
        const LineContext& c = e.context;
        write_u32(c.tu); write_u32(c.line_id); write_u32(c.path_id);
        write_u32(c.logical_line); write_u32(c.marker_flags);
        write_u32(c.previous_line); write_u32(c.previous2_line); write_u32(c.region_ordinal);
        write_u64(c.region_hash); write_u64(c.block_context); write_u64(c.skeleton);
        write_u64(c.signature);
        const uint32_t len = store.len(c.line_id);
        write_u32(len);
        out_.write(reinterpret_cast<const char*>(store.data(c.line_id)), len);
        write_u32(uint32_t(e.candidates.size()));
        for (size_t i = 0; i < e.candidates.size(); ++i) {
            const Candidate& x = e.candidates[i];
            write_u32(x.base_id); write_u8(x.sources); write_u8(0); write_u16(0);
            write_u32(x.age); write_u32(x.prefix); write_u32(x.suffix); write_u32(x.qgram_hits);
            write_u32(x.prefix_record_bytes); write_u32(x.program_record_bytes);
            write_i32(x.reward);
            write_f32(i < e.ftrl_scores.size() ? e.ftrl_scores[i] : 0.0f);
        }
        ++event_count_;
        candidate_count_ += e.candidates.size();
    }

    void finish(uint64_t raw_bytes, uint64_t line_bytes) {
        if (!enabled()) return;
        write_u8(0);
        write_u64(raw_bytes); write_u64(line_bytes); write_u64(tu_count_);
        write_u64(event_count_); write_u64(candidate_count_);
        out_.flush();
        if (!out_) fail_msg("event export write failed");
    }

private:
    template <class T> void pod(T v) {
        out_.write(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    void write_u8(uint8_t v) { pod(v); }
    void write_u16(uint16_t v) { pod(v); }
    void write_u32(uint32_t v) { pod(v); }
    void write_u64(uint64_t v) { pod(v); }
    void write_i32(int32_t v) { pod(v); }
    void write_f32(float v) { pod(v); }
    std::ofstream out_;
    uint64_t tu_count_ = 0, event_count_ = 0, candidate_count_ = 0;
};

static std::vector<RegionOccurrence> split_regions(const std::vector<uint8_t>& data) {
    std::vector<uint32_t> starts;
    if (!data.empty()) starts.push_back(0);
    for (uint32_t i = 1; i + 1 < data.size(); ++i)
        if (data[i] == '#' && data[i - 1] == '\n' && data[i + 1] == ' ') starts.push_back(i);
    std::vector<RegionOccurrence> out;
    out.reserve(starts.size());
    for (uint32_t i = 0; i < starts.size(); ++i) {
        const uint32_t begin = starts[i];
        const uint32_t end = i + 1 < starts.size() ? starts[i + 1] : uint32_t(data.size());
        out.push_back({data.data() + begin, end - begin,
                       hash_bytes(data.data() + begin, end - begin, 0x123456789abcdef0ULL),
                       hash_bytes(data.data() + begin, end - begin, 0xfedcba9876543210ULL),
                       i, 0});
    }
    return out;
}

struct RankMetrics {
    std::array<uint64_t, 4> recall{};
    std::array<uint64_t, 4> regret{};
    uint64_t events_with_candidates = 0;
    uint64_t oracle_saving = 0;
};

static const std::array<uint32_t, 4> kValues{{1, 2, 4, 8}};

static void score_ranker(const Event& event, RankMetrics& metrics) {
    if (event.candidates.empty()) return;
    ++metrics.events_with_candidates;
    uint32_t best_index = 0, best_cost = UINT32_MAX;
    for (uint32_t i = 0; i < event.candidates.size(); ++i) {
        const Candidate& c = event.candidates[i];
        const uint32_t cost = std::min(c.prefix_record_bytes, c.program_record_bytes);
        if (cost < best_cost) { best_cost = cost; best_index = i; }
    }
    metrics.oracle_saving += std::max<int64_t>(0, event.candidates[best_index].reward);
    std::vector<uint32_t> order = ranked_candidates(event, Selector::Ftrl);
    for (size_t row = 0; row < kValues.size(); ++row) {
        const size_t k = std::min<size_t>(kValues[row], order.size());
        uint32_t selected_cost = UINT32_MAX;
        bool hit = false;
        for (size_t i = 0; i < k; ++i) {
            hit |= order[i] == best_index;
            const Candidate& c = event.candidates[order[i]];
            selected_cost = std::min(selected_cost,
                                     std::min(c.prefix_record_bytes, c.program_record_bytes));
        }
        metrics.recall[row] += hit;
        metrics.regret[row] += selected_cost - best_cost;
    }
}

struct Config {
    const char* manifest = nullptr;
    std::string name;
    std::string export_path;
    size_t max_files = SIZE_MAX;
    int zlevel = 1;
    uint32_t label_program_k = 8;
    bool full_oracle = true;
    bool export_only = false;
};

struct Totals {
    uint64_t raw_bytes = 0;
    uint64_t total_lines = 0;
    uint64_t first_lines = 0;
    uint64_t candidate_rows = 0;
    uint64_t regions = 0;
    double load_seconds = 0;
    double parse_seconds = 0;
    double label_seconds = 0;
    double update_seconds = 0;
};

static std::vector<Row> make_rows(int zlevel, bool full_oracle) {
    std::vector<Row> rows;
    rows.emplace_back("literal", Selector::Literal, 0, zlevel);
    rows.emplace_back("cheap-k4", Selector::Cheap, 4, zlevel);
    for (uint32_t k : kValues) rows.emplace_back("ftrl-k" + std::to_string(k), Selector::Ftrl, k, zlevel);
    if (full_oracle) rows.emplace_back("all-candidate-oracle", Selector::Oracle, UINT32_MAX, zlevel);
    return rows;
}

static void label_candidates(const ExactLineStore& store, Event& event, uint32_t program_k) {
    std::vector<uint32_t> cheap = ranked_candidates(event, Selector::Cheap);
    std::vector<uint8_t> use_program(event.candidates.size(), 0);
    for (size_t i = 0; i < std::min<size_t>(program_k, cheap.size()); ++i) use_program[cheap[i]] = 1;
    for (uint32_t i = 0; i < event.candidates.size(); ++i) {
        Candidate& c = event.candidates[i];
        (void)c;
        candidate_encoding(store, event, i, use_program[i] != 0);
    }
}

static bool process_row(Row& row, const ExactLineStore& store, std::vector<Event>& events,
                        const std::vector<uint32_t>& occurrences, const std::vector<uint8_t>& original) {
    if (events.empty()) return row.decoder.verify_tu(occurrences, original.data(), original.size());
    std::vector<uint8_t> frame;
    put_varint(frame, events.size());
    const Clock::time_point enc_begin = Clock::now();
    uint64_t selected_literal = 0, selected_prefix = 0, selected_program = 0;
    for (Event& event : events) {
        const Encoding encoding = choose_encoding(store, event, row);
        serialize_record(store, event.context.line_id, encoding, frame);
        if (encoding.mode == RecordMode::Literal) ++selected_literal;
        else if (encoding.mode == RecordMode::PrefixSuffix) ++selected_prefix;
        else ++selected_program;
    }
    std::vector<uint8_t> compressed;
    if (!row.frames.compress(frame, compressed)) return false;
    if (row.selector != Selector::Literal) {
        std::vector<uint8_t> literal_frame;
        put_varint(literal_frame, events.size());
        for (const Event& event : events)
            serialize_record(store, event.context.line_id,
                             literal_encoding(store, event.context.line_id), literal_frame);
        std::vector<uint8_t> literal_compressed;
        if (!row.frames.compress(literal_frame, literal_compressed)) return false;
        if (literal_compressed.size() < compressed.size()) {
            frame.swap(literal_frame);
            compressed.swap(literal_compressed);
            selected_literal = events.size(); selected_prefix = selected_program = 0;
            ++row.literal_fallback_frames;
        }
    }
    row.raw_record_bytes += frame.size();
    row.literal_records += selected_literal;
    row.prefix_records += selected_prefix;
    row.program_records += selected_program;
    row.wire += compressed.size() + 4;
    row.encode_seconds += elapsed(enc_begin);

    const Clock::time_point dec_begin = Clock::now();
    std::vector<uint8_t> decoded;
    if (!row.frames.decompress(compressed, frame.size(), decoded) || decoded != frame ||
        !row.decoder.decode_frame(decoded.data(), decoded.size())) return false;
    const bool exact = row.decoder.verify_tu(occurrences, original.data(), original.size());
    row.decode_seconds += elapsed(dec_begin);
    row.exact &= exact;
    return exact;
}

static int run(const Config& cfg) {
    const Clock::time_point all_begin = Clock::now();
    const std::vector<std::string> paths = read_manifest(cfg.manifest, cfg.max_files);
    if (paths.empty()) fail_msg("empty manifest");

    ExactLineStore store;
    StringIds path_ids;
    CandidateIndex candidate_index;
    SuperblockContext superblocks;
    FtrlRanker ranker;
    EventExporter exporter(cfg.export_path);
    std::vector<LineContext> metadata(1);
    std::vector<Row> rows = cfg.export_only ? std::vector<Row>{}
                                            : make_rows(cfg.zlevel, cfg.full_oracle);
    RankMetrics rank_metrics;
    Totals totals;

    for (uint32_t tu = 0; tu < paths.size(); ++tu) {
        Clock::time_point phase = Clock::now();
        std::vector<uint8_t> data = read_file(paths[tu]);
        totals.load_seconds += elapsed(phase);
        totals.raw_bytes += data.size();

        phase = Clock::now();
        std::vector<RegionOccurrence> regions = split_regions(data);
        std::vector<uint64_t> root;
        root.reserve(regions.size());
        for (const RegionOccurrence& r : regions) root.push_back(r.hash);
        const std::vector<uint64_t> block_contexts = superblocks.factor(root);
        for (size_t i = 0; i < regions.size(); ++i) regions[i].block_context = block_contexts[i];
        exporter.write_tu(tu, data.size(), regions);
        totals.regions += regions.size();

        std::vector<Event> events;
        std::vector<uint32_t> occurrences;
        occurrences.reserve(data.size() / 24 + 1);
        uint32_t previous = 0, previous2 = 0;
        uint32_t current_path = 0, current_line = 0, current_flags = 0;
        Marker marker;
        for (const RegionOccurrence& region : regions) {
            const uint8_t* p = region.begin;
            const uint8_t* end = p + region.len;
            while (p < end) {
                const void* nl = std::memchr(p, '\n', size_t(end - p));
                const uint8_t* line_end = nl ? static_cast<const uint8_t*>(nl) + 1 : end;
                const uint32_t len = uint32_t(line_end - p);
                if (parse_marker(p, len, marker)) {
                    current_path = path_ids.intern(marker.path);
                    current_line = marker.logical_line;
                    current_flags = marker.flags;
                }
                const ExactLineStore::Result ir = store.intern(p, len);
                LineContext context;
                context.tu = tu; context.line_id = ir.id; context.path_id = current_path;
                context.logical_line = current_line; context.marker_flags = current_flags;
                context.previous_line = previous; context.previous2_line = previous2;
                context.region_ordinal = region.ordinal; context.region_hash = region.hash;
                context.block_context = region.block_context;
                if (ir.first) {
                    context.skeleton = skeleton_hash(p, len);
                    context.signature = signature_hash(p, len);
                    context.qgram_sketch = qgram_sketch(p, len);
                } else {
                    context.skeleton = metadata[ir.id].skeleton;
                    context.signature = metadata[ir.id].signature;
                    context.qgram_sketch = metadata[ir.id].qgram_sketch;
                }

                if (ir.first) {
                    if (metadata.size() != ir.id) fail_msg("non-sequential Line ID");
                    metadata.push_back(context);
                    Event event;
                    event.context = context;
                    event.candidates = candidate_index.query(context, ir.id);
                    event.ftrl_scores.reserve(event.candidates.size());
                    for (Candidate& candidate : event.candidates) {
                        const uint8_t* base = store.data(candidate.base_id);
                        const uint32_t base_len = store.len(candidate.base_id);
                        candidate.prefix = common_prefix(p, len, base, base_len);
                        candidate.suffix = common_suffix(p, len, base, base_len, candidate.prefix);
                        candidate.qgram_hits = qgram_hits(context.qgram_sketch,
                                                         metadata[candidate.base_id].qgram_sketch);
                        candidate.prefix_record_bytes = prefix_size(ir.id, candidate.base_id, len,
                                                                    candidate.prefix, candidate.suffix);
                        const std::vector<Feature> features = make_features(context, metadata[candidate.base_id],
                                                                            candidate, len, base_len);
                        event.ftrl_scores.push_back(ranker.predict(features));
                    }
                    totals.candidate_rows += event.candidates.size();
                    events.push_back(std::move(event));
                    ++totals.first_lines;
                }

                candidate_index.publish(context);
                occurrences.push_back(ir.id);
                ++totals.total_lines;
                previous2 = previous;
                previous = ir.id;
                if (!parse_marker(p, len, marker)) ++current_line;
                p = line_end;
            }
        }
        totals.parse_seconds += elapsed(phase);

        phase = Clock::now();
        for (Event& event : events) label_candidates(store, event, cfg.label_program_k);
        totals.label_seconds += elapsed(phase);

        for (Row& row : rows) {
            if (!process_row(row, store, events, occurrences, data)) {
                row.exact = false;
                std::fprintf(stderr, "exact replay failed: TU=%u row=%s\n", tu, row.name.c_str());
                return 1;
            }
        }

        phase = Clock::now();
        for (Event& event : events) {
            score_ranker(event, rank_metrics);
            for (size_t i = 0; i < event.candidates.size(); ++i) {
                const Candidate& candidate = event.candidates[i];
                const std::vector<Feature> features = make_features(event.context,
                    metadata[candidate.base_id], candidate, store.len(event.context.line_id),
                    store.len(candidate.base_id));
                ranker.update(features, float(candidate.reward), event.ftrl_scores[i]);
            }
            exporter.write_event(store, event);
        }
        totals.update_seconds += elapsed(phase);

        if ((tu + 1) % 100 == 0 || tu + 1 == paths.size()) {
            std::fprintf(stderr,
                "progress %u/%zu raw=%.2f GiB distinct=%zu candidates=%llu elapsed=%.1fs\n",
                tu + 1, paths.size(), totals.raw_bytes / double(1ULL << 30), store.size(),
                (unsigned long long)totals.candidate_rows, elapsed(all_begin));
        }
    }

    exporter.finish(totals.raw_bytes, store.arena().size());
    const double total_seconds = elapsed(all_begin);
    std::printf("==== ISSUE16 ML DEFINITION BAKEOFF: %s ====\n", cfg.name.c_str());
    std::printf("manifest=%s TUs=%zu raw=%llu total_lines=%llu distinct_lines=%llu "
                "distinct_bytes=%zu regions=%llu candidate_rows=%llu\n",
                cfg.manifest, paths.size(), (unsigned long long)totals.raw_bytes,
                (unsigned long long)totals.total_lines, (unsigned long long)totals.first_lines,
                store.arena().size(), (unsigned long long)totals.regions,
                (unsigned long long)totals.candidate_rows);
    std::printf("phases seconds: load=%.3f parse=%.3f label-oracle=%.3f model-update=%.3f total=%.3f\n",
                totals.load_seconds, totals.parse_seconds, totals.label_seconds,
                totals.update_seconds, total_seconds);
    std::printf("FTRL model bytes=%zu updates=%llu\n", ranker.bytes(),
                (unsigned long long)ranker.updates());
    for (size_t i = 0; i < kValues.size(); ++i) {
        const double recall = rank_metrics.events_with_candidates
            ? double(rank_metrics.recall[i]) / rank_metrics.events_with_candidates : 0;
        const double regret = rank_metrics.events_with_candidates
            ? double(rank_metrics.regret[i]) / rank_metrics.events_with_candidates : 0;
        std::printf("FTRL K=%u recall_best=%.6f byte_regret/event=%.4f total_regret=%llu\n",
                    kValues[i], recall, regret, (unsigned long long)rank_metrics.regret[i]);
    }
    std::printf("codec rows (actual per-TU zstd-%d frames; four-byte framing; independent exact decoder):\n",
                cfg.zlevel);
    for (const Row& row : rows) {
        const double enc_gbs = row.encode_seconds ? totals.raw_bytes / row.encode_seconds / 1e9 : 0;
        const double dec_gbs = row.decode_seconds ? totals.raw_bytes / row.decode_seconds / 1e9 : 0;
        const double ratio = row.wire ? double(totals.raw_bytes) / row.wire : 0;
        std::printf("ROW %-22s wire=%llu raw_records=%llu raw/wire=%.3fx "
                    "enc=%.3fGB/s dec=%.3fGB/s modes[L/P/G]=%llu/%llu/%llu "
                    "literal_frame_fallbacks=%llu exact=%s\n",
                    row.name.c_str(), (unsigned long long)row.wire,
                    (unsigned long long)row.raw_record_bytes, ratio, enc_gbs, dec_gbs,
                    (unsigned long long)row.literal_records,
                    (unsigned long long)row.prefix_records,
                    (unsigned long long)row.program_records,
                    (unsigned long long)row.literal_fallback_frames,
                    row.exact ? "PASS" : "FAIL");
    }
    struct rusage usage {};
    ::getrusage(RUSAGE_SELF, &usage);
    std::printf("peak_RSS_MiB=%.1f complete_harness_raw_GBps=%.3f export=%s\n",
                usage.ru_maxrss / 1024.0, totals.raw_bytes / total_seconds / 1e9,
                cfg.export_path.empty() ? "none" : cfg.export_path.c_str());
    return 0;
}

int main(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        auto value = [&](const char* flag) -> const char* {
            if (std::strcmp(argv[i], flag) || i + 1 == argc) return nullptr;
            return argv[++i];
        };
        if (const char* v = value("--manifest")) cfg.manifest = v;
        else if (const char* v = value("--name")) cfg.name = v;
        else if (const char* v = value("--export")) cfg.export_path = v;
        else if (const char* v = value("--max-files")) cfg.max_files = std::strtoull(v, nullptr, 10);
        else if (const char* v = value("--z")) cfg.zlevel = std::atoi(v);
        else if (const char* v = value("--label-program-k")) cfg.label_program_k = std::atoi(v);
        else if (!std::strcmp(argv[i], "--no-full-oracle")) cfg.full_oracle = false;
        else if (!std::strcmp(argv[i], "--export-only")) cfg.export_only = true;
        else {
            std::fprintf(stderr, "unknown/incomplete argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!cfg.manifest || cfg.max_files == 0 || cfg.zlevel < 0 || cfg.zlevel > 3 ||
        cfg.label_program_k > 24) {
        std::fprintf(stderr,
            "usage: %s --manifest FILE [--name NAME] [--export FILE] [--max-files N] "
            "[--z 0..3] [--label-program-k 0..24] [--no-full-oracle] [--export-only]\n", argv[0]);
        return 2;
    }
    if (cfg.name.empty()) cfg.name = cfg.manifest;
    return run(cfg);
}
