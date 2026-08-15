// balanced_line_package_bench.cpp
//
// Held-out exact-Line package measurement for issue #16.  The input traces are the
// authoritative ml_bakeoff ICMLDS2 event streams.  For each target corpus, a package is
// selected using only Lines present in the other corpora.  The package and every target
// residual frame are serialized, optionally zstd-compressed at level <= 3, decoded by an
// independent path, and byte-checked.  Package bytes, selectors, and four-byte outer frame
// lengths are all charged.
//
// This is a definition-plane capability measurement.  It deliberately does not call its
// result a complete transport ratio: Region composition, superblock/root traffic, and the
// remaining protocol messages are separate charged legs.
//
// Build:
//   g++ -O3 -march=native -std=c++17 balanced_line_package_bench.cpp
//       -o balanced_line_package_bench -lzstd
//
// Run:
//   ./balanced_line_package_bench --level 3 --out results.tsv
//       --trace llvm=traces/ml-llvm.bin ...

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <zstd.h>

namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "balanced-line-package: %s\n", message.c_str());
    std::exit(2);
}

double seconds_since(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

uint32_t read_le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
}

uint64_t read_le64(const uint8_t* p) {
    return uint64_t(read_le32(p)) | (uint64_t(read_le32(p + 4)) << 32);
}

void append_le32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value));
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value >> 16));
    out.push_back(uint8_t(value >> 24));
}

void put_varint(std::vector<uint8_t>& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(uint8_t(value) | 0x80);
        value >>= 7;
    }
    out.push_back(uint8_t(value));
}

bool get_varint(const uint8_t*& p, const uint8_t* end, uint64_t& value) {
    value = 0;
    unsigned shift = 0;
    while (p < end && shift <= 63) {
        const uint8_t byte = *p++;
        value |= uint64_t(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return true;
        shift += 7;
    }
    return false;
}

size_t varint_size(uint64_t value) {
    size_t size = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++size;
    }
    return size;
}

uint64_t hash_bytes(const uint8_t* p, uint32_t n) {
    uint64_t h = 0xa0761d6478bd642fULL ^ (uint64_t(n) * 0xe7037ed1a0b428dbULL);
    while (n >= 8) {
        uint64_t word;
        std::memcpy(&word, p, sizeof(word));
        h ^= word + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h *= 0x8ebc6af09c88c6e3ULL;
        p += 8;
        n -= 8;
    }
    uint64_t tail = 0;
    if (n) std::memcpy(&tail, p, n);
    h ^= tail + 0x589965cc75374cc3ULL;
    h ^= h >> 32;
    h *= 0xd6e8feb86659fd93ULL;
    h ^= h >> 32;
    return h | 1ULL;
}

void read_exact(FILE* file, void* data, size_t size, const std::string& path) {
    if (size && std::fread(data, 1, size, file) != size)
        fail(path + ": truncated record");
}

void skip_exact(FILE* file, uint64_t size, const std::string& path) {
    while (size) {
        const uint64_t step = std::min<uint64_t>(
            size, uint64_t(std::numeric_limits<off_t>::max() / 2));
        if (fseeko(file, off_t(step), SEEK_CUR) != 0)
            fail(path + ": seek failed: " + std::strerror(errno));
        size -= step;
    }
}

struct GlobalLine {
    uint64_t off = 0;
    uint32_t len = 0;
    uint32_t corpus_mask = 0;
};

struct HashSlot {
    uint64_t hash = 0;
    uint32_t id_plus_one = 0;
};

class GlobalLineStore {
public:
    GlobalLineStore() { table_.resize(size_t(1) << 20); }

    uint32_t intern(const uint8_t* data, uint32_t len, uint32_t corpus_bit) {
        if ((lines_.size() + 1) * 10 > table_.size() * 7) rehash(table_.size() * 2);
        const uint64_t hash = hash_bytes(data, len);
        size_t slot = size_t(hash) & (table_.size() - 1);
        for (;;) {
            HashSlot& cell = table_[slot];
            if (!cell.hash) {
                const uint32_t id = uint32_t(lines_.size());
                const uint64_t off = arena_.size();
                arena_.insert(arena_.end(), data, data + len);
                lines_.push_back({off, len, corpus_bit});
                cell = {hash, id + 1};
                return id;
            }
            const GlobalLine& line = lines_[cell.id_plus_one - 1];
            if (cell.hash == hash && line.len == len &&
                std::memcmp(arena_.data() + line.off, data, len) == 0) {
                lines_[cell.id_plus_one - 1].corpus_mask |= corpus_bit;
                return cell.id_plus_one - 1;
            }
            slot = (slot + 1) & (table_.size() - 1);
        }
    }

    const GlobalLine& line(uint32_t id) const { return lines_.at(id); }
    const uint8_t* data(uint32_t id) const { return arena_.data() + line(id).off; }
    size_t size() const { return lines_.size(); }
    uint64_t bytes() const { return arena_.size(); }

    bool lexical_less(uint32_t a, uint32_t b) const {
        const GlobalLine& x = line(a);
        const GlobalLine& y = line(b);
        const uint32_t common = std::min(x.len, y.len);
        const int comparison = std::memcmp(data(a), data(b), common);
        if (comparison != 0) return comparison < 0;
        if (x.len != y.len) return x.len < y.len;
        return a < b;
    }

private:
    void rehash(size_t capacity) {
        std::vector<HashSlot> next(capacity);
        for (const HashSlot& old : table_) {
            if (!old.hash) continue;
            size_t slot = size_t(old.hash) & (capacity - 1);
            while (next[slot].hash) slot = (slot + 1) & (capacity - 1);
            next[slot] = old;
        }
        table_.swap(next);
    }

    std::vector<uint8_t> arena_;
    std::vector<GlobalLine> lines_;
    std::vector<HashSlot> table_;
};

struct TraceSpec {
    std::string name;
    std::string path;
};

struct CorpusLines {
    std::string name;
    std::string path;
    std::vector<std::vector<uint32_t>> new_lines_by_tu;
    std::vector<uint64_t> raw_by_tu;
    uint64_t line_bytes = 0;
    uint64_t candidate_rows = 0;
};

CorpusLines load_trace(const TraceSpec& spec, uint32_t corpus_index, GlobalLineStore& store) {
    constexpr size_t kTuHeadSize = 16;
    constexpr size_t kRegionSize = 28;
    constexpr size_t kEventFixedSize = 64;
    constexpr size_t kCandidateSize = 40;
    constexpr size_t kFooterSize = 40;

    FILE* file = std::fopen(spec.path.c_str(), "rb");
    if (!file) fail(spec.path + ": " + std::strerror(errno));
    std::array<uint8_t, 64> buffer{};
    read_exact(file, buffer.data(), 8, spec.path);
    if (std::memcmp(buffer.data(), "ICMLDS2\0", 8) != 0)
        fail(spec.path + ": wrong trace magic");
    read_exact(file, buffer.data(), 4, spec.path);
    if (read_le32(buffer.data()) != 2) fail(spec.path + ": unsupported trace version");

    CorpusLines result;
    result.name = spec.name;
    result.path = spec.path;
    uint32_t expected_line_id = 1;
    bool have_footer = false;
    for (;;) {
        const int tag = std::fgetc(file);
        if (tag == EOF) fail(spec.path + ": missing footer");
        if (tag == 0) {
            read_exact(file, buffer.data(), kFooterSize, spec.path);
            const uint64_t footer_raw = read_le64(buffer.data());
            const uint64_t footer_line_bytes = read_le64(buffer.data() + 8);
            const uint64_t footer_tus = read_le64(buffer.data() + 16);
            const uint64_t footer_events = read_le64(buffer.data() + 24);
            const uint64_t footer_candidates = read_le64(buffer.data() + 32);
            const uint64_t observed_raw = std::accumulate(
                result.raw_by_tu.begin(), result.raw_by_tu.end(), uint64_t(0));
            uint64_t observed_events = 0;
            for (const auto& lines : result.new_lines_by_tu) observed_events += lines.size();
            if (footer_raw != observed_raw || footer_line_bytes != result.line_bytes ||
                footer_tus != result.raw_by_tu.size() || footer_events != observed_events ||
                footer_candidates != result.candidate_rows)
                fail(spec.path + ": footer counters disagree with records");
            if (std::fgetc(file) != EOF) fail(spec.path + ": trailing bytes");
            have_footer = true;
            break;
        }
        if (tag == 1) {
            read_exact(file, buffer.data(), kTuHeadSize, spec.path);
            const uint32_t tu = read_le32(buffer.data());
            const uint64_t raw = read_le64(buffer.data() + 4);
            const uint32_t region_count = read_le32(buffer.data() + 12);
            if (tu != result.raw_by_tu.size()) fail(spec.path + ": non-sequential TU id");
            result.raw_by_tu.push_back(raw);
            result.new_lines_by_tu.emplace_back();
            skip_exact(file, uint64_t(region_count) * kRegionSize, spec.path);
            continue;
        }
        if (tag != 2) fail(spec.path + ": unknown record tag");
        read_exact(file, buffer.data(), kEventFixedSize, spec.path);
        const uint32_t tu = read_le32(buffer.data());
        const uint32_t line_id = read_le32(buffer.data() + 4);
        if (tu >= result.new_lines_by_tu.size()) fail(spec.path + ": event before TU record");
        if (line_id != expected_line_id++) fail(spec.path + ": non-sequential Line id");
        read_exact(file, buffer.data(), 4, spec.path);
        const uint32_t line_len = read_le32(buffer.data());
        std::vector<uint8_t> line(line_len);
        read_exact(file, line.data(), line.size(), spec.path);
        const uint32_t global_id = store.intern(
            line.data(), line_len, uint32_t(1) << corpus_index);
        result.new_lines_by_tu[tu].push_back(global_id);
        result.line_bytes += line_len;
        read_exact(file, buffer.data(), 4, spec.path);
        const uint32_t candidates = read_le32(buffer.data());
        result.candidate_rows += candidates;
        skip_exact(file, uint64_t(candidates) * kCandidateSize, spec.path);
    }
    std::fclose(file);
    if (!have_footer) fail(spec.path + ": footer not read");
    return result;
}

enum class Representation : uint8_t { Literal = 0, Front = 1 };

std::vector<uint8_t> serialize_literal(const std::vector<uint32_t>& ids,
                                       const GlobalLineStore& store) {
    std::vector<uint8_t> raw;
    put_varint(raw, ids.size());
    for (uint32_t id : ids) {
        const GlobalLine& line = store.line(id);
        put_varint(raw, line.len);
        raw.insert(raw.end(), store.data(id), store.data(id) + line.len);
    }
    return raw;
}

std::vector<uint8_t> serialize_front(std::vector<uint32_t> ids,
                                     const GlobalLineStore& store) {
    std::sort(ids.begin(), ids.end(),
              [&](uint32_t a, uint32_t b) { return store.lexical_less(a, b); });
    std::vector<uint8_t> raw;
    put_varint(raw, ids.size());
    const uint8_t* previous = nullptr;
    uint32_t previous_len = 0;
    for (uint32_t id : ids) {
        const GlobalLine& line = store.line(id);
        const uint8_t* data = store.data(id);
        uint32_t lcp = 0;
        const uint32_t limit = std::min(previous_len, line.len);
        while (lcp < limit && previous[lcp] == data[lcp]) ++lcp;
        put_varint(raw, lcp);
        put_varint(raw, line.len - lcp);
        raw.insert(raw.end(), data + lcp, data + line.len);
        previous = data;
        previous_len = line.len;
    }
    return raw;
}

struct PackedFrame {
    std::vector<uint8_t> bytes;
    Representation representation = Representation::Literal;
};

PackedFrame pack_raw(const std::vector<uint8_t>& raw, Representation representation,
                     int level, ZSTD_CCtx* compressor) {
    const size_t bound = ZSTD_compressBound(raw.size());
    std::vector<uint8_t> compressed(bound);
    ZSTD_CCtx_reset(compressor, ZSTD_reset_session_and_parameters);
    ZSTD_CCtx_setParameter(compressor, ZSTD_c_compressionLevel, level);
    const size_t compressed_size = ZSTD_compress2(
        compressor, compressed.data(), compressed.size(),
        raw.empty() ? static_cast<const void*>("") : raw.data(), raw.size());
    if (ZSTD_isError(compressed_size)) fail(ZSTD_getErrorName(compressed_size));
    const bool use_compressed = compressed_size < raw.size();
    const size_t payload_size = use_compressed ? compressed_size : raw.size();
    if (payload_size + 2 > UINT32_MAX) fail("frame exceeds u32 length");
    PackedFrame frame;
    frame.representation = representation;
    frame.bytes.reserve(payload_size + 6);
    append_le32(frame.bytes, uint32_t(payload_size + 2));
    frame.bytes.push_back(uint8_t(representation));
    frame.bytes.push_back(use_compressed ? 1 : 0);
    if (use_compressed)
        frame.bytes.insert(frame.bytes.end(), compressed.data(), compressed.data() + compressed_size);
    else
        frame.bytes.insert(frame.bytes.end(), raw.begin(), raw.end());
    return frame;
}

std::vector<uint8_t> unpack_raw(const PackedFrame& frame, ZSTD_DCtx* decompressor) {
    if (frame.bytes.size() < 6) fail("short packed frame");
    const uint32_t body_size = read_le32(frame.bytes.data());
    if (uint64_t(body_size) + 4 != frame.bytes.size()) fail("packed frame length mismatch");
    if (frame.bytes[4] != uint8_t(frame.representation)) fail("representation byte mismatch");
    const uint8_t compression = frame.bytes[5];
    const uint8_t* payload = frame.bytes.data() + 6;
    const size_t payload_size = frame.bytes.size() - 6;
    if (compression == 0) return std::vector<uint8_t>(payload, payload + payload_size);
    if (compression != 1) fail("unknown frame compression mode");
    const unsigned long long raw_size = ZSTD_getFrameContentSize(payload, payload_size);
    if (raw_size == ZSTD_CONTENTSIZE_ERROR || raw_size == ZSTD_CONTENTSIZE_UNKNOWN ||
        raw_size > SIZE_MAX)
        fail("compressed frame lacks a usable content size");
    std::vector<uint8_t> raw(static_cast<size_t>(raw_size), uint8_t{});
    const size_t result = ZSTD_decompressDCtx(
        decompressor, raw.data(), raw.size(), payload, payload_size);
    if (ZSTD_isError(result) || result != raw.size()) fail("frame decompression failed");
    return raw;
}

std::vector<std::vector<uint8_t>> decode_lines(const PackedFrame& frame, ZSTD_DCtx* decompressor) {
    const std::vector<uint8_t> raw = unpack_raw(frame, decompressor);
    const uint8_t* p = raw.data();
    const uint8_t* end = p + raw.size();
    uint64_t count = 0;
    if (!get_varint(p, end, count) || count > UINT32_MAX) fail("bad Line count");
    std::vector<std::vector<uint8_t>> lines;
    lines.reserve(size_t(count));
    if (frame.representation == Representation::Literal) {
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t len = 0;
            if (!get_varint(p, end, len) || len > uint64_t(end - p))
                fail("bad literal Line length");
            lines.emplace_back(p, p + len);
            p += len;
        }
    } else {
        std::vector<uint8_t> previous;
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t lcp = 0, suffix = 0;
            if (!get_varint(p, end, lcp) || !get_varint(p, end, suffix) ||
                lcp > previous.size() || suffix > uint64_t(end - p))
                fail("bad front-coded Line record");
            std::vector<uint8_t> line(previous.begin(), previous.begin() + lcp);
            line.insert(line.end(), p, p + suffix);
            p += suffix;
            lines.push_back(line);
            previous = std::move(line);
        }
    }
    if (p != end) fail("trailing bytes in Line frame");
    return lines;
}

void verify_frame(const PackedFrame& frame, const std::vector<uint32_t>& expected,
                  const GlobalLineStore& store, ZSTD_DCtx* decompressor) {
    std::vector<uint32_t> ordered = expected;
    if (frame.representation == Representation::Front)
        std::sort(ordered.begin(), ordered.end(),
                  [&](uint32_t a, uint32_t b) { return store.lexical_less(a, b); });
    const std::vector<std::vector<uint8_t>> decoded = decode_lines(frame, decompressor);
    if (decoded.size() != ordered.size()) fail("decoded Line count mismatch");
    for (size_t i = 0; i < ordered.size(); ++i) {
        const GlobalLine& line = store.line(ordered[i]);
        if (decoded[i].size() != line.len ||
            std::memcmp(decoded[i].data(), store.data(ordered[i]), line.len) != 0)
            fail("decoded Line bytes differ");
    }
}

PackedFrame best_frame(const std::vector<uint32_t>& ids, const GlobalLineStore& store,
                       int level, ZSTD_CCtx* compressor, ZSTD_DCtx* decompressor) {
    const std::vector<uint8_t> literal = serialize_literal(ids, store);
    const std::vector<uint8_t> front = serialize_front(ids, store);
    PackedFrame literal_frame = pack_raw(literal, Representation::Literal, level, compressor);
    PackedFrame front_frame = pack_raw(front, Representation::Front, level, compressor);
    PackedFrame winner = front_frame.bytes.size() < literal_frame.bytes.size()
                             ? std::move(front_frame)
                             : std::move(literal_frame);
    verify_frame(winner, ids, store, decompressor);
    return winner;
}

struct Row {
    std::string corpus;
    uint64_t raw_bytes = 0;
    uint64_t target_line_bytes = 0;
    uint64_t budget = 0;
    uint64_t package_lines = 0;
    uint64_t package_raw_bytes = 0;
    uint64_t package_wire_bytes = 0;
    uint64_t hit_lines = 0;
    uint64_t hit_bytes = 0;
    uint64_t residual_lines = 0;
    uint64_t residual_bytes = 0;
    uint64_t residual_wire_bytes = 0;
    uint64_t literal_tus = 0;
    uint64_t front_tus = 0;
    uint64_t literal_only_wire_bytes = 0;
    uint64_t baseline_wire_bytes = 0;
    bool exact = true;

    uint64_t total_wire() const { return package_wire_bytes + residual_wire_bytes; }
    double gain() const { return double(baseline_wire_bytes) / std::max<uint64_t>(1, total_wire()); }
};

struct Config {
    int level = 3;
    std::string output;
    std::vector<uint64_t> budgets{0, 64u << 10, 128u << 10, 256u << 10,
                                  512u << 10, 1u << 20, 2u << 20, 4u << 20};
    std::vector<TraceSpec> traces;
};

std::vector<uint64_t> parse_budgets(const std::string& value) {
    std::vector<uint64_t> budgets;
    size_t begin = 0;
    while (begin <= value.size()) {
        const size_t comma = value.find(',', begin);
        const std::string token = value.substr(begin, comma - begin);
        if (token.empty()) fail("empty budget token");
        char* end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::strtoull(token.c_str(), &end, 10);
        if (errno || !end || *end) fail("bad budget: " + token);
        budgets.push_back(parsed);
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    std::sort(budgets.begin(), budgets.end());
    budgets.erase(std::unique(budgets.begin(), budgets.end()), budgets.end());
    if (budgets.empty() || budgets.front() != 0) budgets.insert(budgets.begin(), 0);
    return budgets;
}

Config parse_args(int argc, char** argv) {
    Config config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* option) -> std::string {
            if (i + 1 >= argc) fail(std::string("missing value for ") + option);
            return argv[++i];
        };
        if (arg == "--level") {
            config.level = std::stoi(value("--level"));
        } else if (arg == "--out") {
            config.output = value("--out");
        } else if (arg == "--budgets") {
            config.budgets = parse_budgets(value("--budgets"));
        } else if (arg == "--trace") {
            const std::string item = value("--trace");
            const size_t equal = item.find('=');
            if (equal == std::string::npos || equal == 0 || equal + 1 == item.size())
                fail("--trace must be NAME=PATH");
            config.traces.push_back({item.substr(0, equal), item.substr(equal + 1)});
        } else {
            fail("unknown option: " + arg);
        }
    }
    if (config.level < 0 || config.level > 3) fail("level must be between 0 and 3");
    if (config.output.empty()) fail("--out is required");
    if (config.traces.size() < 2 || config.traces.size() > 31)
        fail("between 2 and 31 traces are required");
    for (size_t i = 0; i < config.traces.size(); ++i)
        for (size_t j = i + 1; j < config.traces.size(); ++j)
            if (config.traces[i].name == config.traces[j].name)
                fail("duplicate corpus name: " + config.traces[i].name);
    return config;
}

void write_header(FILE* output) {
    std::fprintf(output,
        "corpus\traw_bytes\ttarget_line_bytes\tpackage_budget_raw\tpackage_lines\t"
        "package_raw_bytes\tpackage_wire_bytes\thit_lines\thit_bytes\tresidual_lines\t"
        "residual_bytes\tresidual_wire_bytes\tliteral_tus\tfront_tus\tline_wire_bytes\t"
        "literal_only_line_wire_bytes\tbaseline_line_wire_bytes\ttu_front_gain\tline_gain\t"
        "raw_over_line_wire\texact\n");
}

void write_row(FILE* output, const Row& row) {
    std::fprintf(output,
        "%s\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t"
        "%llu\t%llu\t%llu\t%llu\t%llu\t%.9f\t%.9f\t%.9f\t%s\n",
        row.corpus.c_str(),
        static_cast<unsigned long long>(row.raw_bytes),
        static_cast<unsigned long long>(row.target_line_bytes),
        static_cast<unsigned long long>(row.budget),
        static_cast<unsigned long long>(row.package_lines),
        static_cast<unsigned long long>(row.package_raw_bytes),
        static_cast<unsigned long long>(row.package_wire_bytes),
        static_cast<unsigned long long>(row.hit_lines),
        static_cast<unsigned long long>(row.hit_bytes),
        static_cast<unsigned long long>(row.residual_lines),
        static_cast<unsigned long long>(row.residual_bytes),
        static_cast<unsigned long long>(row.residual_wire_bytes),
        static_cast<unsigned long long>(row.literal_tus),
        static_cast<unsigned long long>(row.front_tus),
        static_cast<unsigned long long>(row.total_wire()),
        static_cast<unsigned long long>(row.literal_only_wire_bytes),
        static_cast<unsigned long long>(row.baseline_wire_bytes),
        double(row.literal_only_wire_bytes) /
            std::max<uint64_t>(1, row.baseline_wire_bytes),
        row.gain(), double(row.raw_bytes) / std::max<uint64_t>(1, row.total_wire()),
        row.exact ? "OK" : "FAIL");
}

}  // namespace

int main(int argc, char** argv) {
    const Config config = parse_args(argc, argv);
    const Clock::time_point all_begin = Clock::now();
    GlobalLineStore store;
    std::vector<CorpusLines> corpora;
    corpora.reserve(config.traces.size());
    for (uint32_t i = 0; i < config.traces.size(); ++i) {
        const Clock::time_point begin = Clock::now();
        corpora.push_back(load_trace(config.traces[i], i, store));
        const CorpusLines& corpus = corpora.back();
        uint64_t events = 0;
        for (const auto& lines : corpus.new_lines_by_tu) events += lines.size();
        std::fprintf(stderr,
            "loaded %-18s TUs=%zu Lines=%llu LineBytes=%.2f MiB candidates=%llu %.2fs\n",
            corpus.name.c_str(), corpus.raw_by_tu.size(),
            static_cast<unsigned long long>(events), corpus.line_bytes / 1048576.0,
            static_cast<unsigned long long>(corpus.candidate_rows), seconds_since(begin));
    }
    std::fprintf(stderr, "global exact Lines=%zu union-bytes=%.2f MiB scan=%.2fs\n",
                 store.size(), store.bytes() / 1048576.0, seconds_since(all_begin));

    FILE* output = std::fopen(config.output.c_str(), "wb");
    if (!output) fail(config.output + ": " + std::strerror(errno));
    write_header(output);

    ZSTD_CCtx* compressor = ZSTD_createCCtx();
    ZSTD_DCtx* decompressor = ZSTD_createDCtx();
    if (!compressor || !decompressor) fail("cannot allocate zstd contexts");
    std::vector<uint32_t> selected_stamp(store.size(), 0);
    uint32_t stamp = 0;
    std::vector<std::vector<double>> gain_by_budget(config.budgets.size());

    for (uint32_t target = 0; target < corpora.size(); ++target) {
        const Clock::time_point target_begin = Clock::now();
        const CorpusLines& corpus = corpora[target];
        const uint32_t target_bit = uint32_t(1) << target;
        std::vector<uint32_t> candidates;
        candidates.reserve(store.size());
        for (uint32_t id = 0; id < store.size(); ++id)
            if (store.line(id).corpus_mask & ~target_bit) candidates.push_back(id);
        std::sort(candidates.begin(), candidates.end(), [&](uint32_t a, uint32_t b) {
            const GlobalLine& x = store.line(a);
            const GlobalLine& y = store.line(b);
            const unsigned xdf = unsigned(__builtin_popcount(x.corpus_mask & ~target_bit));
            const unsigned ydf = unsigned(__builtin_popcount(y.corpus_mask & ~target_bit));
            if (xdf != ydf) return xdf > ydf;
            if (x.len != y.len) return x.len > y.len;
            return store.lexical_less(a, b);
        });

        uint64_t literal_only_wire = 0;
        uint64_t baseline_wire = 0;
        for (const auto& ids : corpus.new_lines_by_tu) {
            if (ids.empty()) continue;
            PackedFrame literal_frame = pack_raw(
                serialize_literal(ids, store), Representation::Literal,
                config.level, compressor);
            verify_frame(literal_frame, ids, store, decompressor);
            literal_only_wire += literal_frame.bytes.size();
            baseline_wire += best_frame(ids, store, config.level, compressor, decompressor).bytes.size();
        }

        for (size_t budget_index = 0; budget_index < config.budgets.size(); ++budget_index) {
            const uint64_t budget = config.budgets[budget_index];
            if (++stamp == 0) {
                std::fill(selected_stamp.begin(), selected_stamp.end(), 0);
                stamp = 1;
            }
            std::vector<uint32_t> package;
            uint64_t package_raw = 0;
            for (uint32_t id : candidates) {
                const GlobalLine& line = store.line(id);
                const uint64_t record = line.len + varint_size(line.len);
                if (package_raw + record > budget) continue;
                package.push_back(id);
                package_raw += record;
                selected_stamp[id] = stamp;
            }

            Row row;
            row.corpus = corpus.name;
            row.raw_bytes = std::accumulate(
                corpus.raw_by_tu.begin(), corpus.raw_by_tu.end(), uint64_t(0));
            row.target_line_bytes = corpus.line_bytes;
            row.budget = budget;
            row.package_lines = package.size();
            row.package_raw_bytes = package_raw;
            row.literal_only_wire_bytes = literal_only_wire;
            row.baseline_wire_bytes = baseline_wire;
            if (!package.empty()) {
                PackedFrame package_frame = pack_raw(
                    serialize_front(package, store), Representation::Front,
                    config.level, compressor);
                verify_frame(package_frame, package, store, decompressor);
                row.package_wire_bytes = package_frame.bytes.size();
            }

            std::vector<uint32_t> residual;
            for (const auto& ids : corpus.new_lines_by_tu) {
                residual.clear();
                residual.reserve(ids.size());
                for (uint32_t id : ids) {
                    const GlobalLine& line = store.line(id);
                    if (selected_stamp[id] == stamp) {
                        ++row.hit_lines;
                        row.hit_bytes += line.len;
                    } else {
                        residual.push_back(id);
                        ++row.residual_lines;
                        row.residual_bytes += line.len;
                    }
                }
                if (residual.empty()) continue;
                PackedFrame frame = best_frame(
                    residual, store, config.level, compressor, decompressor);
                row.residual_wire_bytes += frame.bytes.size();
                if (frame.representation == Representation::Front) ++row.front_tus;
                else ++row.literal_tus;
            }
            write_row(output, row);
            gain_by_budget[budget_index].push_back(row.gain());
            std::fprintf(stderr,
                "%-18s budget=%7llu KiB package=%7.2f KiB hits=%7llu/%7llu "
                "wire=%8.2f KiB gain=%6.3fx exact=OK\n",
                corpus.name.c_str(), static_cast<unsigned long long>(budget >> 10),
                row.package_wire_bytes / 1024.0,
                static_cast<unsigned long long>(row.hit_lines),
                static_cast<unsigned long long>(row.hit_lines + row.residual_lines),
                row.total_wire() / 1024.0, row.gain());
        }
        std::fprintf(stderr, "target %-18s complete %.2fs\n",
                     corpus.name.c_str(), seconds_since(target_begin));
        std::fflush(output);
    }

    std::fprintf(stderr, "\nEqual-corpus held-out Line-package summary:\n");
    for (size_t i = 0; i < config.budgets.size(); ++i) {
        std::vector<double> gains = gain_by_budget[i];
        std::sort(gains.begin(), gains.end());
        const double harmonic = gains.size() /
            std::accumulate(gains.begin(), gains.end(), 0.0,
                            [](double sum, double value) { return sum + 1.0 / value; });
        const size_t wins = size_t(std::count_if(
            gains.begin(), gains.end(), [](double value) { return value > 1.0; }));
        std::fprintf(stderr,
            "  budget=%7llu KiB  harmonic-gain=%.4fx min=%.4fx median=%.4fx "
            "max=%.4fx wins=%zu/%zu\n",
            static_cast<unsigned long long>(config.budgets[i] >> 10), harmonic,
            gains.front(), gains[gains.size() / 2], gains.back(), wins, gains.size());
    }

    ZSTD_freeCCtx(compressor);
    ZSTD_freeDCtx(decompressor);
    if (std::fclose(output) != 0) fail(config.output + ": close failed");
    std::fprintf(stderr, "wrote %s; total %.2fs\n",
                 config.output.c_str(), seconds_since(all_begin));
    return 0;
}
