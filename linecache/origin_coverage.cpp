// origin_coverage.cpp -- issue #16 R0/R3/R4 Region/source capability probe.
//
// Builds immutable exact Region/Line model assets from whole held-out training repositories, then
// scans a target repository chronologically.  Every reported static hit is routed by two 64-bit
// digests and confirmed by an exact byte comparison.  Source coverage is likewise an exact compare
// against the marker-named source/header file.  This is the coverage ledger requested by BigOracle
// comment 5299969154; it intentionally precedes the Region-program serializer.
//
// Build:
//   g++ -O3 -march=native -std=c++17 origin_coverage.cpp -o origin_coverage

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using Clock = std::chrono::steady_clock;

static double seconds(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

[[noreturn]] static void die(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(2);
}

static inline uint64_t load64(const uint8_t* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, 8);
    return v;
}

static inline uint64_t tail64(const uint8_t* p, uint32_t n) {
    uint64_t v = 0;
    std::memcpy(&v, p, n);
    return v;
}

static inline uint64_t mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static uint64_t hash_bytes(const uint8_t* p, uint32_t n, uint64_t seed) {
    uint64_t h = mix64(seed ^ (uint64_t(n) * 0xa0761d6478bd642fULL));
    while (n >= 16) {
        h = mix64(h ^ load64(p));
        h = mix64(h ^ load64(p + 8));
        p += 16; n -= 16;
    }
    if (n >= 8) {
        h = mix64(h ^ load64(p));
        h = mix64(h ^ load64(p + n - 8));
    } else if (n) h = mix64(h ^ tail64(p, n));
    return h | 1ULL;
}

struct Key {
    uint64_t h1 = 0, h2 = 0;
    uint32_t len = 0;
    uint8_t kind = 0; // 1 Region, 2 Line
    bool operator==(const Key& other) const {
        return h1 == other.h1 && h2 == other.h2 && len == other.len && kind == other.kind;
    }
};

struct KeyHash {
    size_t operator()(const Key& key) const {
        return size_t(mix64(key.h1 ^ mix64(key.h2) ^ (uint64_t(key.len) << 8) ^ key.kind));
    }
};

static Key make_key(const uint8_t* p, uint32_t n, uint8_t kind) {
    return {hash_bytes(p, n, 0x243f6a8885a308d3ULL),
            hash_bytes(p, n, 0x13198a2e03707344ULL), n, kind};
}

static std::vector<std::string> manifest_paths(const std::string& manifest, size_t max_files) {
    FILE* f = std::fopen(manifest.c_str(), "r");
    if (!f) die("cannot open manifest " + manifest);
    std::vector<std::string> paths;
    char line[16384];
    while (std::fgets(line, sizeof(line), f)) {
        size_t n = std::strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        paths.emplace_back(line);
        if (paths.size() == max_files) break;
    }
    std::fclose(f);
    return paths;
}

static std::vector<uint8_t> read_file(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) die("cannot open " + path);
    struct stat st {};
    if (::fstat(::fileno(f), &st) != 0 || st.st_size < 0 || uint64_t(st.st_size) > UINT32_MAX)
        die("bad file " + path);
    std::vector<uint8_t> data(size_t(st.st_size));
    if (!data.empty() && std::fread(data.data(), 1, data.size(), f) != data.size())
        die("short read " + path);
    std::fclose(f);
    return data;
}

struct Span { uint32_t off = 0, len = 0; };

static std::vector<Span> regions(const std::vector<uint8_t>& data) {
    std::vector<uint32_t> starts;
    if (!data.empty()) starts.push_back(0);
    for (uint32_t i = 1; i + 1 < data.size(); ++i)
        if (data[i] == '#' && data[i - 1] == '\n' && data[i + 1] == ' ') starts.push_back(i);
    std::vector<Span> out;
    out.reserve(starts.size());
    for (uint32_t i = 0; i < starts.size(); ++i) {
        const uint32_t end = i + 1 < starts.size() ? starts[i + 1] : uint32_t(data.size());
        out.push_back({starts[i], end - starts[i]});
    }
    return out;
}

static std::vector<Span> lines(const uint8_t* data, uint32_t n) {
    std::vector<Span> out;
    uint32_t off = 0;
    while (off < n) {
        const void* nl = std::memchr(data + off, '\n', n - off);
        const uint32_t end = nl ? uint32_t(static_cast<const uint8_t*>(nl) - data + 1) : n;
        out.push_back({off, end - off});
        off = end;
    }
    return out;
}

struct Marker {
    std::string path;
    uint32_t line = 0;
    uint32_t flags = 0;
};

static bool parse_marker(const uint8_t* s, uint32_t n, Marker& marker) {
    if (n < 5 || s[0] != '#' || s[1] != ' ') return false;
    const uint8_t* p = s + 2;
    const uint8_t* end = s + n;
    if (p == end || *p < '0' || *p > '9') return false;
    uint64_t line = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        line = line * 10 + (*p++ - '0');
        if (line > UINT32_MAX) return false;
    }
    if (end - p < 3 || p[0] != ' ' || p[1] != '"') return false;
    p += 2;
    const uint8_t* begin = p;
    while (p < end && *p != '"') ++p;
    if (p == end) return false;
    marker.path.assign(reinterpret_cast<const char*>(begin), size_t(p - begin));
    ++p;
    uint32_t flags = 0, ordinal = 0;
    while (p < end && *p == ' ') {
        ++p;
        if (p == end || *p < '0' || *p > '9') return false;
        uint32_t value = 0;
        while (p < end && *p >= '0' && *p <= '9') value = value * 10 + (*p++ - '0');
        flags ^= uint32_t(mix64(value + 0x9e3779b97f4a7c15ULL * ++ordinal));
    }
    if (p == end || *p != '\n' || p + 1 != end) return false;
    marker.line = uint32_t(line); marker.flags = flags;
    return true;
}

static bool system_path(const std::string& path) {
    return path.rfind("/usr/include/", 0) == 0 || path.rfind("/usr/lib/gcc/", 0) == 0 ||
           path.rfind("/usr/local/include/", 0) == 0;
}

struct Locator { uint32_t file = 0, off = 0; };
struct Stat {
    uint64_t occurrences = 0;
    uint64_t bytes_seen = 0;
    uint64_t corpus_mask = 0;
    Locator first{};
    bool toolchain = false;
};

struct TrainingIndex {
    std::vector<std::string> files;
    std::unordered_map<Key, Stat, KeyHash> stats;
    std::unordered_map<std::string, uint64_t> source_path_counts;
    uint64_t raw_bytes = 0, region_occurrences = 0, line_occurrences = 0;
};

static void update_stat(TrainingIndex& index, const Key& key, uint32_t file, uint32_t off,
                        uint64_t corpus_bit, bool toolchain) {
    auto [it, inserted] = index.stats.try_emplace(key);
    Stat& stat = it->second;
    if (inserted) stat.first = {file, off};
    ++stat.occurrences;
    stat.bytes_seen += key.len;
    stat.corpus_mask |= corpus_bit;
    stat.toolchain |= toolchain;
}

static void scan_training(const std::vector<std::string>& manifests, size_t max_files,
                          TrainingIndex& index) {
    for (uint32_t corpus = 0; corpus < manifests.size(); ++corpus) {
        const std::vector<std::string> paths = manifest_paths(manifests[corpus], max_files);
        for (const std::string& path : paths) {
            const uint32_t file_id = uint32_t(index.files.size());
            index.files.push_back(path);
            std::vector<uint8_t> data = read_file(path);
            index.raw_bytes += data.size();
            for (const Span& region : regions(data)) {
                const uint8_t* r = data.data() + region.off;
                const std::vector<Span> rlines = lines(r, region.len);
                Marker marker;
                const bool parsed = !rlines.empty() && parse_marker(r + rlines[0].off, rlines[0].len, marker);
                const bool toolchain = parsed && system_path(marker.path);
                if (parsed) ++index.source_path_counts[marker.path];
                update_stat(index, make_key(r, region.len, 1), file_id, region.off,
                            uint64_t(1) << corpus, toolchain);
                ++index.region_occurrences;
                for (const Span& line : rlines) {
                    update_stat(index, make_key(r + line.off, line.len, 2), file_id,
                                region.off + line.off, uint64_t(1) << corpus, toolchain);
                    ++index.line_occurrences;
                }
            }
        }
        std::fprintf(stderr, "training corpus %u/%zu files=%zu unique-assets=%zu raw=%.2f GiB\n",
                     corpus + 1, manifests.size(), paths.size(), index.stats.size(),
                     index.raw_bytes / double(1ULL << 30));
    }
}

struct Asset {
    Key key{};
    uint64_t occurrences = 0, corpus_mask = 0, cumulative = 0;
    Locator locator{};
    std::vector<uint8_t> bytes;
    bool toolchain = false;
};

static unsigned populations(uint64_t x) { return unsigned(__builtin_popcountll(x)); }

static std::vector<Asset> select_assets(const TrainingIndex& index, bool toolchain_only,
                                        uint64_t maximum_bytes, uint8_t kind) {
    std::vector<Asset> candidates;
    for (const auto& [key, stat] : index.stats) {
        if (key.kind != kind || (toolchain_only && !stat.toolchain)) continue;
        // GENERIC_II assets need evidence from at least two held-out projects.  The matched
        // toolchain track may use one-project observations because identity comes from the explicit
        // compiler/header package.
        if (!toolchain_only && populations(stat.corpus_mask) < 2) continue;
        candidates.push_back({key, stat.occurrences, stat.corpus_mask, 0, stat.first, {}, stat.toolchain});
    }
    std::sort(candidates.begin(), candidates.end(), [&](const Asset& a, const Asset& b) {
        const unsigned ap = populations(a.corpus_mask), bp = populations(b.corpus_mask);
        if (ap != bp) return ap > bp;
        // Region assets receive a modest priority because a hit also removes composition metadata.
        const __uint128_t av = __uint128_t(a.occurrences) * (a.key.kind == 1 ? 5 : 4);
        const __uint128_t bv = __uint128_t(b.occurrences) * (b.key.kind == 1 ? 5 : 4);
        if (av != bv) return av > bv;
        if (a.key.len != b.key.len) return a.key.len > b.key.len;
        if (a.key.h1 != b.key.h1) return a.key.h1 < b.key.h1;
        return a.key.h2 < b.key.h2;
    });

    uint64_t selected = 0;
    size_t keep = 0;
    for (; keep < candidates.size(); ++keep) {
        const uint64_t charged = candidates[keep].key.len + 24;
        if (selected + charged > maximum_bytes) break;
        selected += charged;
        candidates[keep].cumulative = selected;
    }
    candidates.resize(keep);

    std::vector<uint32_t> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        if (candidates[a].locator.file != candidates[b].locator.file)
            return candidates[a].locator.file < candidates[b].locator.file;
        return candidates[a].locator.off < candidates[b].locator.off;
    });
    uint32_t active_file = UINT32_MAX;
    std::vector<uint8_t> data;
    for (uint32_t id : order) {
        Asset& asset = candidates[id];
        if (asset.locator.file != active_file) {
            active_file = asset.locator.file;
            data = read_file(index.files[active_file]);
        }
        if (uint64_t(asset.locator.off) + asset.key.len > data.size()) die("asset locator overflow");
        asset.bytes.assign(data.begin() + asset.locator.off,
                           data.begin() + asset.locator.off + asset.key.len);
        const Key check = make_key(asset.bytes.data(), asset.key.len, asset.key.kind);
        if (!(check == asset.key)) die("asset locator identity mismatch");
    }
    return candidates;
}

class AssetLookup {
public:
    explicit AssetLookup(std::vector<Asset> assets) : assets_(std::move(assets)) {
        for (uint32_t i = 0; i < assets_.size(); ++i) route_[assets_[i].key].push_back(i);
    }

    uint64_t required_bytes(const uint8_t* p, uint32_t n, uint8_t kind) const {
        const Key key = make_key(p, n, kind);
        auto it = route_.find(key);
        if (it == route_.end()) return UINT64_MAX;
        for (uint32_t id : it->second) {
            const Asset& asset = assets_[id];
            if (asset.bytes.size() == n && std::memcmp(asset.bytes.data(), p, n) == 0)
                return asset.cumulative;
        }
        return UINT64_MAX;
    }

    uint32_t find_id(const uint8_t* p, uint32_t n, uint8_t kind, uint64_t budget) const {
        const Key key = make_key(p, n, kind);
        auto it = route_.find(key);
        if (it == route_.end()) return UINT32_MAX;
        for (uint32_t id : it->second) {
            const Asset& asset = assets_[id];
            if (asset.cumulative <= budget && asset.bytes.size() == n &&
                std::memcmp(asset.bytes.data(), p, n) == 0) return id;
        }
        return UINT32_MAX;
    }

    const Asset& asset(uint32_t id) const { return assets_.at(id); }

    size_t size() const { return assets_.size(); }
    uint64_t bytes() const { return assets_.empty() ? 0 : assets_.back().cumulative; }

private:
    std::vector<Asset> assets_;
    std::unordered_map<Key, std::vector<uint32_t>, KeyHash> route_;
};

class SeenExact {
public:
    bool first(const uint8_t* p, uint32_t n, uint8_t kind) {
        const Key key = make_key(p, n, kind);
        return keys_.insert(key).second;
    }
private:
    // The first-seen ledger uses a 128-bit route key to bound memory. Model hits themselves are
    // still exact-compared against stored bytes before they are credited or would be referenced.
    std::unordered_set<Key, KeyHash> keys_;
};

struct SourceFile {
    bool available = false;
    bool system = false;
    uint64_t bytes = 0;
    std::vector<uint8_t> data;
    std::vector<Span> lines;
};

class SourceCache {
public:
    SourceFile& get(const std::string& path) {
        auto [it, inserted] = files_.try_emplace(path);
        if (!inserted) return it->second;
        SourceFile& source = it->second;
        source.system = system_path(path);
        struct stat st {};
        if (path.empty() || path[0] == '<' || ::stat(path.c_str(), &st) != 0 || st.st_size < 0 ||
            uint64_t(st.st_size) > UINT32_MAX) return source;
        source.data = read_file(path);
        source.bytes = source.data.size();
        source.lines = ::lines(source.data.data(), uint32_t(source.data.size()));
        source.available = true;
        return source;
    }
    uint64_t available_bytes(bool system) const {
        uint64_t total = 0;
        for (const auto& [_, source] : files_)
            if (source.available && source.system == system) total += source.bytes;
        return total;
    }
    size_t available_files(bool system) const {
        size_t total = 0;
        for (const auto& [_, source] : files_)
            total += source.available && source.system == system;
        return total;
    }
private:
    std::unordered_map<std::string, SourceFile> files_;
};

static const std::array<uint64_t, 6> kBudgets{{0, 256ULL << 10, 1ULL << 20, 4ULL << 20,
                                               16ULL << 20, 64ULL << 20}};

struct Coverage {
    uint64_t first_objects = 0, first_bytes = 0;
    std::array<uint64_t, kBudgets.size()> static_hits{}, static_bytes{};
};

struct TargetTotals {
    uint64_t raw = 0, tus = 0, region_occ = 0, line_occ = 0;
    Coverage generic_region, generic_line, toolchain_region, toolchain_line;
    uint64_t source_regions = 0, source_region_bytes = 0;
    uint64_t source_body_lines = 0, source_body_bytes = 0;
    uint64_t source_exact_lines = 0, source_exact_bytes = 0;
    uint64_t system_source_exact_lines = 0, system_source_exact_bytes = 0;
};

static void account_hit(Coverage& coverage, uint64_t required, uint32_t bytes) {
    ++coverage.first_objects;
    coverage.first_bytes += bytes;
    for (size_t i = 0; i < kBudgets.size(); ++i) if (required <= kBudgets[i]) {
        ++coverage.static_hits[i]; coverage.static_bytes[i] += bytes;
    }
}

[[maybe_unused]] static void scan_target(const std::string& manifest, size_t max_files,
                        const AssetLookup& generic_regions, const AssetLookup& generic_lines,
                        const AssetLookup& toolchain_regions, const AssetLookup& toolchain_lines,
                        TargetTotals& totals, SourceCache& sources) {
    const std::vector<std::string> paths = manifest_paths(manifest, max_files);
    SeenExact seen_regions, seen_lines;
    for (uint32_t tu = 0; tu < paths.size(); ++tu) {
        std::vector<uint8_t> data = read_file(paths[tu]);
        totals.raw += data.size(); ++totals.tus;
        for (const Span& region : regions(data)) {
            const uint8_t* r = data.data() + region.off;
            ++totals.region_occ;
            const bool first_region = seen_regions.first(r, region.len, 1);
            if (first_region) {
                account_hit(totals.generic_region, generic_regions.required_bytes(r, region.len, 1), region.len);
                account_hit(totals.toolchain_region, toolchain_regions.required_bytes(r, region.len, 1), region.len);
            }

            const std::vector<Span> rlines = lines(r, region.len);
            Marker marker;
            const bool parsed = !rlines.empty() && parse_marker(r + rlines[0].off, rlines[0].len, marker);
            SourceFile* source = nullptr;
            if (parsed) {
                source = &sources.get(marker.path);
                if (source->available) { ++totals.source_regions; totals.source_region_bytes += region.len; }
            }
            for (uint32_t i = 0; i < rlines.size(); ++i) {
                const Span& line = rlines[i];
                const uint8_t* l = r + line.off;
                ++totals.line_occ;
                if (seen_lines.first(l, line.len, 2)) {
                    account_hit(totals.generic_line, generic_lines.required_bytes(l, line.len, 2), line.len);
                    account_hit(totals.toolchain_line, toolchain_lines.required_bytes(l, line.len, 2), line.len);
                }
                if (parsed && source && source->available && i > 0) {
                    ++totals.source_body_lines; totals.source_body_bytes += line.len;
                    const uint64_t source_index = uint64_t(marker.line) + (i - 1);
                    if (source_index > 0 && source_index <= source->lines.size()) {
                        const Span& sl = source->lines[source_index - 1];
                        if (sl.len == line.len &&
                            std::memcmp(source->data.data() + sl.off, l, line.len) == 0) {
                            ++totals.source_exact_lines; totals.source_exact_bytes += line.len;
                            if (source->system) {
                                ++totals.system_source_exact_lines;
                                totals.system_source_exact_bytes += line.len;
                            }
                        }
                    }
                }
            }
        }
        if ((tu + 1) % 100 == 0 || tu + 1 == paths.size())
            std::fprintf(stderr, "target %u/%zu raw=%.2f GiB regions=%llu lines=%llu\n", tu + 1,
                         paths.size(), totals.raw / double(1ULL << 30),
                         (unsigned long long)totals.region_occ,
                         (unsigned long long)totals.line_occ);
    }
}

[[maybe_unused]] static void print_coverage(const char* name, const Coverage& c) {
    for (size_t i = 0; i < kBudgets.size(); ++i) {
        std::printf("COVERAGE track=%s budget=%llu model_hit_objects=%llu/%llu "
                    "model_hit_bytes=%llu/%llu byte_fraction=%.8f\n", name,
                    (unsigned long long)kBudgets[i], (unsigned long long)c.static_hits[i],
                    (unsigned long long)c.first_objects, (unsigned long long)c.static_bytes[i],
                    (unsigned long long)c.first_bytes,
                    c.first_bytes ? double(c.static_bytes[i]) / c.first_bytes : 0.0);
    }
}

#ifndef ICE_REGION_COVERAGE_LIBRARY
int main(int argc, char** argv) {
    std::vector<std::string> training;
    std::string target, name = "target";
    size_t max_files = SIZE_MAX;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--train") && i + 1 < argc) training.emplace_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--target") && i + 1 < argc) target = argv[++i];
        else if (!std::strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!std::strcmp(argv[i], "--max-files") && i + 1 < argc)
            max_files = std::strtoull(argv[++i], nullptr, 10);
        else die(std::string("unknown/incomplete argument: ") + argv[i]);
    }
    if (training.empty() || target.empty() || training.size() > 63 || !max_files)
        die("usage: origin_coverage --train M [--train M...] --target M [--name N] [--max-files N]");

    const Clock::time_point begin = Clock::now();
    TrainingIndex index;
    scan_training(training, max_files, index);
    std::fprintf(stderr, "selecting exact assets from %zu unique candidates\n", index.stats.size());
    AssetLookup generic_regions(select_assets(index, false, kBudgets.back(), 1));
    AssetLookup generic_lines(select_assets(index, false, kBudgets.back(), 2));
    AssetLookup toolchain_regions(select_assets(index, true, kBudgets.back(), 1));
    AssetLookup toolchain_lines(select_assets(index, true, kBudgets.back(), 2));
    std::fprintf(stderr, "assets generic R/L=%zu/%zu toolchain R/L=%zu/%zu\n",
                 generic_regions.size(), generic_lines.size(), toolchain_regions.size(),
                 toolchain_lines.size());

    TargetTotals totals;
    SourceCache sources;
    scan_target(target, max_files, generic_regions, generic_lines, toolchain_regions,
                toolchain_lines, totals, sources);
    std::printf("==== ORIGIN/STATIC REGION COVERAGE %s ====\n", name.c_str());
    std::printf("target=%s training_manifests=%zu TUs=%llu raw=%llu region_occ=%llu line_occ=%llu\n",
                target.c_str(), training.size(), (unsigned long long)totals.tus,
                (unsigned long long)totals.raw, (unsigned long long)totals.region_occ,
                (unsigned long long)totals.line_occ);
    std::printf("training raw=%llu unique_assets=%zu region_occ=%llu line_occ=%llu\n",
                (unsigned long long)index.raw_bytes, index.stats.size(),
                (unsigned long long)index.region_occurrences,
                (unsigned long long)index.line_occurrences);
    print_coverage("GENERIC_II_REGION", totals.generic_region);
    print_coverage("GENERIC_II_LINE", totals.generic_line);
    print_coverage("TOOLCHAIN_REGION", totals.toolchain_region);
    print_coverage("TOOLCHAIN_LINE", totals.toolchain_line);
    std::printf("SOURCE exact_direct_lines=%llu/%llu exact_direct_bytes=%llu/%llu fraction=%.8f "
                "system_exact_lines=%llu system_exact_bytes=%llu\n",
                (unsigned long long)totals.source_exact_lines,
                (unsigned long long)totals.source_body_lines,
                (unsigned long long)totals.source_exact_bytes,
                (unsigned long long)totals.source_body_bytes,
                totals.source_body_bytes ? double(totals.source_exact_bytes) / totals.source_body_bytes : 0,
                (unsigned long long)totals.system_source_exact_lines,
                (unsigned long long)totals.system_source_exact_bytes);
    std::printf("SOURCE available_regions=%llu available_region_bytes=%llu "
                "system_files=%zu system_package_bytes=%llu project_files=%zu project_package_bytes=%llu\n",
                (unsigned long long)totals.source_regions,
                (unsigned long long)totals.source_region_bytes,
                sources.available_files(true), (unsigned long long)sources.available_bytes(true),
                sources.available_files(false), (unsigned long long)sources.available_bytes(false));
    struct rusage usage {};
    ::getrusage(RUSAGE_SELF, &usage);
    std::printf("elapsed=%.3f peak_RSS_MiB=%.1f raw_GBps=%.3f exact_compare=YES\n", seconds(begin),
                usage.ru_maxrss / 1024.0, totals.raw / seconds(begin) / 1e9);
    return 0;
}
#endif
