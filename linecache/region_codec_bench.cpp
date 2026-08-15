// region_codec_bench.cpp -- issue #16 heterogeneous exact Region materializer (R1/R3/R4/R6).
//
// The immutable training assets and source coverage machinery come from origin_coverage.cpp.  This
// file adds actual self-describing Region records, an independent decoder, per-TU zstd frames, raw
// fallback, static exact Region references, source-Line COPY/PATCH/INSERT programs, and prior-Region
// deltas.  Each model-budget row chooses the smallest actual serialized record and then performs a
// whole-frame best-of against REGION_RAW.
//
// Build:
//   g++ -O3 -march=native -std=c++17 region_codec_bench.cpp -o region_codec_bench -lzstd

#define ICE_REGION_COVERAGE_LIBRARY 1
#include "origin_coverage.cpp"

#include <zstd.h>

static size_t varint_bytes(uint64_t value) {
    size_t n = 1;
    while (value >= 0x80) { value >>= 7; ++n; }
    return n;
}

static void put_varint(std::vector<uint8_t>& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(uint8_t(value) | 0x80);
        value >>= 7;
    }
    out.push_back(uint8_t(value));
}

static bool get_varint(const uint8_t*& p, const uint8_t* end, uint64_t& value) {
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

struct SourceAsset {
    std::string path;
    std::vector<uint8_t> data;
    std::vector<Span> lines;
    uint64_t count = 0, cumulative = 0;
};

class SourceModel {
public:
    SourceModel(const TrainingIndex& training, uint64_t maximum_bytes) {
        struct Candidate { std::string path; uint64_t count = 0, size = 0; };
        std::vector<Candidate> candidates;
        for (const auto& [path, count] : training.source_path_counts) {
            if (!system_path(path)) continue;
            struct stat st {};
            if (::stat(path.c_str(), &st) != 0 || st.st_size <= 0 || uint64_t(st.st_size) > UINT32_MAX)
                continue;
            candidates.push_back({path, count, uint64_t(st.st_size)});
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            const __uint128_t left = __uint128_t(a.count) * b.size;
            const __uint128_t right = __uint128_t(b.count) * a.size;
            if (left != right) return left > right;
            if (a.count != b.count) return a.count > b.count;
            return a.path < b.path;
        });
        uint64_t total = 0;
        for (const Candidate& candidate : candidates) {
            const uint64_t charge = candidate.path.size() + candidate.size + 32;
            if (total + charge > maximum_bytes) continue;
            SourceAsset asset;
            asset.path = candidate.path;
            asset.data = read_file(candidate.path);
            asset.lines = ::lines(asset.data.data(), uint32_t(asset.data.size()));
            asset.count = candidate.count;
            total += charge;
            asset.cumulative = total;
            const uint32_t id = uint32_t(assets_.size());
            by_path_[asset.path] = id;
            assets_.push_back(std::move(asset));
        }
    }

    uint32_t find(const std::string& path, uint64_t budget) const {
        auto it = by_path_.find(path);
        if (it == by_path_.end()) return UINT32_MAX;
        return assets_[it->second].cumulative <= budget ? it->second : UINT32_MAX;
    }
    const SourceAsset& asset(uint32_t id) const { return assets_.at(id); }
    size_t size() const { return assets_.size(); }
    uint64_t bytes() const { return assets_.empty() ? 0 : assets_.back().cumulative; }

private:
    std::vector<SourceAsset> assets_;
    std::unordered_map<std::string, uint32_t> by_path_;
};

class RegionTruth {
public:
    struct Ref { uint32_t off = 0, len = 0; };
    struct Result { uint32_t id = 0; bool first = false; };

    RegionTruth() { refs_.push_back({}); }

    Result intern(const uint8_t* p, uint32_t n) {
        const Key key = make_key(p, n, 1);
        auto& bucket = route_[key];
        for (uint32_t id : bucket) {
            const Ref& ref = refs_[id];
            if (std::memcmp(arena_.data() + ref.off, p, n) == 0) return {id, false};
        }
        if (arena_.size() + n > UINT32_MAX) die("Region truth arena exceeds 4 GiB");
        const uint32_t id = uint32_t(refs_.size());
        const uint32_t off = uint32_t(arena_.size());
        arena_.insert(arena_.end(), p, p + n);
        refs_.push_back({off, n});
        bucket.push_back(id);
        return {id, true};
    }

    const uint8_t* data(uint32_t id) const { return arena_.data() + refs_.at(id).off; }
    uint32_t len(uint32_t id) const { return refs_.at(id).len; }
    size_t size() const { return refs_.size() - 1; }
    size_t bytes() const { return arena_.size(); }

private:
    std::vector<uint8_t> arena_;
    std::vector<Ref> refs_;
    std::unordered_map<Key, std::vector<uint32_t>, KeyHash> route_;
};

enum class RegionMode : uint8_t {
    Raw = 0, StaticExact = 1, SourceProgram = 2, PriorRegion = 3, MixedLines = 4
};
enum class SourceOp : uint8_t { Copy = 0, Patch = 1, Insert = 2 };

struct SourceInstruction {
    SourceOp op = SourceOp::Insert;
    uint32_t source_line = 0;
    uint32_t target_off = 0;
    uint32_t target_len = 0;
    uint32_t prefix = 0;
    uint32_t suffix = 0;
};

enum class MixedOp : uint8_t { Inline = 0, RefView = 1, DefineViewAndUse = 2 };
struct MixedInstruction {
    MixedOp op = MixedOp::Inline;
    uint32_t target_off = 0, target_len = 0;
    uint32_t view_id = 0, owner_region = 0, owner_off = 0;
    uint32_t entry_index = UINT32_MAX;
};

struct RegionEncoding {
    RegionMode mode = RegionMode::Raw;
    uint32_t region_id = 0;
    uint32_t static_id = UINT32_MAX;
    uint32_t source_id = UINT32_MAX;
    uint32_t base_region_id = 0;
    uint32_t marker_len = 0;
    uint32_t prefix = 0, suffix = 0;
    std::vector<SourceInstruction> source_ops;
    std::vector<MixedInstruction> mixed_ops;
    uint32_t bytes = UINT32_MAX;
};

static uint32_t prefix_length(const uint8_t* a, uint32_t an, const uint8_t* b, uint32_t bn) {
    uint32_t i = 0, n = std::min(an, bn);
    while (i + 8 <= n && load64(a + i) == load64(b + i)) i += 8;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

static uint32_t suffix_length(const uint8_t* a, uint32_t an, const uint8_t* b, uint32_t bn,
                              uint32_t prefix) {
    uint32_t i = 0;
    const uint32_t n = std::min(an - std::min(an, prefix), bn - std::min(bn, prefix));
    while (i < n && a[an - 1 - i] == b[bn - 1 - i]) ++i;
    return i;
}

static RegionEncoding raw_encoding(uint32_t id, uint32_t n) {
    RegionEncoding out;
    out.mode = RegionMode::Raw; out.region_id = id;
    out.bytes = uint32_t(1 + varint_bytes(id) + varint_bytes(n) + n);
    return out;
}

static RegionEncoding static_encoding(uint32_t region_id, uint32_t n, uint32_t static_id) {
    RegionEncoding out;
    out.mode = RegionMode::StaticExact; out.region_id = region_id; out.static_id = static_id;
    out.bytes = uint32_t(1 + varint_bytes(region_id) + varint_bytes(n) + varint_bytes(static_id));
    return out;
}

static RegionEncoding prior_encoding(const RegionTruth& truth, uint32_t id, uint32_t base_id) {
    const uint8_t* target = truth.data(id); const uint32_t tn = truth.len(id);
    const uint8_t* base = truth.data(base_id); const uint32_t bn = truth.len(base_id);
    RegionEncoding out;
    out.mode = RegionMode::PriorRegion; out.region_id = id; out.base_region_id = base_id;
    out.prefix = prefix_length(target, tn, base, bn);
    out.suffix = suffix_length(target, tn, base, bn, out.prefix);
    const uint32_t middle = tn - out.prefix - out.suffix;
    out.bytes = uint32_t(1 + varint_bytes(id) + varint_bytes(tn) + varint_bytes(base_id) +
                         varint_bytes(out.prefix) + varint_bytes(out.suffix) +
                         varint_bytes(middle) + middle);
    return out;
}

struct ParsedRegion {
    Marker marker;
    bool marker_ok = false;
    std::vector<Span> lines;
    std::vector<Key> line_keys;
};

static ParsedRegion parse_region(const uint8_t* p, uint32_t n) {
    ParsedRegion out;
    out.lines = ::lines(p, n);
    out.line_keys.reserve(out.lines.size());
    for (const Span& line : out.lines)
        out.line_keys.push_back(make_key(p + line.off, line.len, 2));
    out.marker_ok = !out.lines.empty() && parse_marker(p + out.lines[0].off,
                                                       out.lines[0].len, out.marker);
    return out;
}

class LineViewState {
public:
    explicit LineViewState(const RegionTruth* truth) : truth_(truth) {}

    RegionEncoding plan(uint32_t region_id, const ParsedRegion& parsed,
                        std::unordered_map<uint32_t, uint32_t>& planned_views,
                        uint32_t& provisional_next) const {
        RegionEncoding out;
        out.mode = RegionMode::MixedLines; out.region_id = region_id;
        const uint8_t* region = truth_->data(region_id);
        for (size_t line_index = 0; line_index < parsed.lines.size(); ++line_index) {
            const Span& line = parsed.lines[line_index];
            MixedInstruction op;
            op.target_off = line.off; op.target_len = line.len;
            const uint32_t entry = find_entry(parsed.line_keys[line_index],
                                              region + line.off, line.len);
            op.entry_index = entry;
            if (entry != UINT32_MAX && entries_[entry].view_id) {
                op.op = MixedOp::RefView; op.view_id = entries_[entry].view_id;
            } else if (entry != UINT32_MAX && planned_views.count(entry)) {
                op.op = MixedOp::RefView; op.view_id = planned_views.at(entry);
            } else if (entry != UINT32_MAX && entries_[entry].owner_region != region_id) {
                op.op = MixedOp::DefineViewAndUse; op.view_id = provisional_next++;
                op.owner_region = entries_[entry].owner_region;
                op.owner_off = entries_[entry].owner_off;
                planned_views.emplace(entry, op.view_id);
            } else {
                op.op = MixedOp::Inline;
            }
            out.mixed_ops.push_back(op);
        }
        uint32_t bytes = uint32_t(1 + varint_bytes(region_id) + varint_bytes(truth_->len(region_id)) +
                                  varint_bytes(out.mixed_ops.size()));
        for (const MixedInstruction& op : out.mixed_ops) {
            bytes += 1;
            if (op.op == MixedOp::Inline) bytes += uint32_t(varint_bytes(op.target_len) + op.target_len);
            else if (op.op == MixedOp::RefView) bytes += varint_bytes(op.view_id);
            else bytes += uint32_t(varint_bytes(op.view_id) + varint_bytes(op.owner_region) +
                                   varint_bytes(op.owner_off) + varint_bytes(op.target_len));
        }
        out.bytes = bytes;
        return out;
    }

    void commit(uint32_t region_id, const ParsedRegion& parsed, const RegionEncoding* selected) {
        const uint8_t* region = truth_->data(region_id);
        if (selected && selected->mode == RegionMode::MixedLines) {
            for (const MixedInstruction& op : selected->mixed_ops) {
                if (op.op != MixedOp::DefineViewAndUse || op.entry_index == UINT32_MAX) continue;
                Entry& entry = entries_.at(op.entry_index);
                if (!entry.view_id) {
                    if (op.view_id != next_view_id_) die("non-sequential Line view id");
                    entry.view_id = next_view_id_++;
                }
            }
        }
        for (size_t line_index = 0; line_index < parsed.lines.size(); ++line_index) {
            const Span& line = parsed.lines[line_index];
            if (find_entry(parsed.line_keys[line_index], region + line.off, line.len) != UINT32_MAX)
                continue;
            const uint32_t entry = uint32_t(entries_.size());
            entries_.push_back({region_id, line.off, line.len, 0});
            route_[parsed.line_keys[line_index]].push_back(entry);
        }
    }

    uint32_t views() const { return next_view_id_ - 1; }
    uint32_t next_view_id() const { return next_view_id_; }

private:
    struct Entry { uint32_t owner_region, owner_off, len, view_id; };
    uint32_t find_entry(const Key& key, const uint8_t* p, uint32_t n) const {
        auto it = route_.find(key);
        if (it == route_.end()) return UINT32_MAX;
        for (uint32_t id : it->second) {
            const Entry& entry = entries_[id];
            if (entry.len == n && !std::memcmp(truth_->data(entry.owner_region) + entry.owner_off, p, n))
                return id;
        }
        return UINT32_MAX;
    }
    const RegionTruth* truth_;
    std::vector<Entry> entries_;
    std::unordered_map<Key, std::vector<uint32_t>, KeyHash> route_;
    uint32_t next_view_id_ = 1;
};

static RegionEncoding source_encoding(const uint8_t* target, uint32_t n, uint32_t region_id,
                                      const ParsedRegion& parsed, uint32_t source_id,
                                      const SourceAsset& source) {
    RegionEncoding out;
    out.mode = RegionMode::SourceProgram; out.region_id = region_id; out.source_id = source_id;
    if (!parsed.marker_ok || parsed.lines.empty()) return out;
    out.marker_len = parsed.lines[0].len;
    for (uint32_t i = 1; i < parsed.lines.size(); ++i) {
        const Span& target_line = parsed.lines[i];
        SourceInstruction op;
        op.target_off = target_line.off; op.target_len = target_line.len;
        op.op = SourceOp::Insert;
        uint32_t best_cost = uint32_t(1 + varint_bytes(target_line.len) + target_line.len);
        constexpr int32_t kBand = 32;
        const int64_t expected = int64_t(parsed.marker.line) - 1 + int64_t(i - 1);
        const int64_t lo = std::max<int64_t>(0, expected - kBand);
        const int64_t hi = std::min<int64_t>(int64_t(source.lines.size()) - 1, expected + kBand);
        const uint8_t* t = target + target_line.off;
        for (int64_t candidate = lo; candidate <= hi; ++candidate) {
            const Span& base_line = source.lines[size_t(candidate)];
            const uint8_t* b = source.data.data() + base_line.off;
            if (target_line.len == base_line.len && !std::memcmp(t, b, target_line.len)) {
                const uint32_t cost = uint32_t(1 + varint_bytes(uint64_t(candidate)));
                if (cost < best_cost) {
                    best_cost = cost; op.op = SourceOp::Copy;
                    op.source_line = uint32_t(candidate); op.prefix = op.suffix = 0;
                }
                continue;
            }
            const uint32_t prefix = prefix_length(t, target_line.len, b, base_line.len);
            const uint32_t suffix = suffix_length(t, target_line.len, b, base_line.len, prefix);
            const uint32_t middle = target_line.len - prefix - suffix;
            const uint32_t cost = uint32_t(1 + varint_bytes(uint64_t(candidate)) +
                varint_bytes(target_line.len) + varint_bytes(prefix) + varint_bytes(suffix) +
                varint_bytes(middle) + middle);
            if (cost < best_cost) {
                best_cost = cost; op.op = SourceOp::Patch;
                op.source_line = uint32_t(candidate); op.prefix = prefix; op.suffix = suffix;
            }
        }
        out.source_ops.push_back(op);
    }
    uint32_t bytes = uint32_t(1 + varint_bytes(region_id) + varint_bytes(n) +
                              varint_bytes(source_id) + varint_bytes(out.marker_len) +
                              out.marker_len + varint_bytes(out.source_ops.size()));
    for (const SourceInstruction& op : out.source_ops) {
        bytes += 1;
        if (op.op == SourceOp::Copy) {
            bytes += varint_bytes(op.source_line);
        } else if (op.op == SourceOp::Patch) {
            const uint32_t middle = op.target_len - op.prefix - op.suffix;
            bytes += uint32_t(varint_bytes(op.source_line) + varint_bytes(op.target_len) +
                              varint_bytes(op.prefix) + varint_bytes(op.suffix) +
                              varint_bytes(middle) + middle);
        } else {
            bytes += uint32_t(varint_bytes(op.target_len) + op.target_len);
        }
    }
    out.bytes = bytes;
    return out;
}

static void serialize_region(const RegionTruth& truth, const RegionEncoding& encoding,
                             std::vector<uint8_t>& out) {
    const uint8_t* target = truth.data(encoding.region_id);
    const uint32_t n = truth.len(encoding.region_id);
    out.push_back(uint8_t(encoding.mode));
    put_varint(out, encoding.region_id); put_varint(out, n);
    if (encoding.mode == RegionMode::Raw) {
        out.insert(out.end(), target, target + n);
    } else if (encoding.mode == RegionMode::StaticExact) {
        put_varint(out, encoding.static_id);
    } else if (encoding.mode == RegionMode::PriorRegion) {
        put_varint(out, encoding.base_region_id); put_varint(out, encoding.prefix);
        put_varint(out, encoding.suffix);
        const uint32_t middle = n - encoding.prefix - encoding.suffix;
        put_varint(out, middle);
        out.insert(out.end(), target + encoding.prefix, target + encoding.prefix + middle);
    } else if (encoding.mode == RegionMode::MixedLines) {
        put_varint(out, encoding.mixed_ops.size());
        for (const MixedInstruction& op : encoding.mixed_ops) {
            out.push_back(uint8_t(op.op));
            if (op.op == MixedOp::Inline) {
                put_varint(out, op.target_len);
                out.insert(out.end(), target + op.target_off, target + op.target_off + op.target_len);
            } else if (op.op == MixedOp::RefView) {
                put_varint(out, op.view_id);
            } else {
                put_varint(out, op.view_id); put_varint(out, op.owner_region);
                put_varint(out, op.owner_off); put_varint(out, op.target_len);
            }
        }
    } else {
        put_varint(out, encoding.source_id); put_varint(out, encoding.marker_len);
        out.insert(out.end(), target, target + encoding.marker_len);
        put_varint(out, encoding.source_ops.size());
        for (const SourceInstruction& op : encoding.source_ops) {
            out.push_back(uint8_t(op.op));
            if (op.op == SourceOp::Copy) {
                put_varint(out, op.source_line);
            } else if (op.op == SourceOp::Patch) {
                put_varint(out, op.source_line); put_varint(out, op.target_len);
                put_varint(out, op.prefix); put_varint(out, op.suffix);
                const uint32_t middle = op.target_len - op.prefix - op.suffix;
                put_varint(out, middle);
                out.insert(out.end(), target + op.target_off + op.prefix,
                           target + op.target_off + op.prefix + middle);
            } else {
                put_varint(out, op.target_len);
                out.insert(out.end(), target + op.target_off, target + op.target_off + op.target_len);
            }
        }
    }
}

class RegionDecoder {
public:
    RegionDecoder(const AssetLookup* static_regions, const SourceModel* sources)
        : static_regions_(static_regions), sources_(sources) {
        refs_.push_back({}); views_.push_back({});
    }

    bool decode_frame(const uint8_t* p, size_t n) {
        const uint8_t* end = p + n;
        uint64_t count = 0;
        if (!get_varint(p, end, count) || count > (1u << 24)) return false;
        for (uint64_t i = 0; i < count; ++i) if (!decode_record(p, end)) return false;
        return p == end;
    }

    bool verify_tu(const std::vector<uint32_t>& ids, const uint8_t* truth, size_t n) const {
        size_t off = 0;
        for (uint32_t id : ids) {
            if (!id || id >= refs_.size()) return false;
            const Ref& ref = refs_[id];
            if (off + ref.len > n || std::memcmp(arena_.data() + ref.off, truth + off, ref.len))
                return false;
            off += ref.len;
        }
        return off == n;
    }

    size_t bytes() const { return arena_.size(); }

private:
    struct Ref { uint32_t off = 0, len = 0; };

    bool install(uint32_t id, const std::vector<uint8_t>& bytes) {
        if (id != refs_.size() || arena_.size() + bytes.size() > UINT32_MAX) return false;
        const uint32_t off = uint32_t(arena_.size());
        arena_.insert(arena_.end(), bytes.begin(), bytes.end());
        refs_.push_back({off, uint32_t(bytes.size())});
        return true;
    }

    bool decode_record(const uint8_t*& p, const uint8_t* end) {
        if (p == end) return false;
        const uint8_t raw_mode = *p++;
        if (raw_mode > uint8_t(RegionMode::MixedLines)) return false;
        const RegionMode mode = RegionMode(raw_mode);
        uint64_t id64 = 0, n64 = 0;
        if (!get_varint(p, end, id64) || !id64 || id64 > UINT32_MAX ||
            !get_varint(p, end, n64) || n64 > UINT32_MAX) return false;
        const uint32_t id = uint32_t(id64), n = uint32_t(n64);
        std::vector<uint8_t> bytes;
        bytes.reserve(n);
        if (mode == RegionMode::Raw) {
            if (uint64_t(end - p) < n) return false;
            bytes.assign(p, p + n); p += n;
        } else if (mode == RegionMode::StaticExact) {
            uint64_t model_id = 0;
            if (!get_varint(p, end, model_id) || !static_regions_ ||
                model_id >= static_regions_->size()) return false;
            const Asset& asset = static_regions_->asset(uint32_t(model_id));
            if (asset.bytes.size() != n) return false;
            bytes = asset.bytes;
        } else if (mode == RegionMode::PriorRegion) {
            uint64_t base64 = 0, prefix = 0, suffix = 0, middle = 0;
            if (!get_varint(p, end, base64) || !base64 || base64 >= refs_.size() ||
                !get_varint(p, end, prefix) || !get_varint(p, end, suffix) ||
                !get_varint(p, end, middle)) return false;
            const Ref& base = refs_[base64];
            if (prefix > base.len || suffix > base.len - prefix || prefix + suffix > n ||
                middle != n - prefix - suffix || middle > uint64_t(end - p)) return false;
            bytes.insert(bytes.end(), arena_.begin() + base.off, arena_.begin() + base.off + prefix);
            bytes.insert(bytes.end(), p, p + middle); p += middle;
            bytes.insert(bytes.end(), arena_.begin() + base.off + base.len - suffix,
                         arena_.begin() + base.off + base.len);
        } else if (mode == RegionMode::MixedLines) {
            uint64_t count = 0;
            if (!get_varint(p, end, count) || count > (1u << 22)) return false;
            for (uint64_t i = 0; i < count; ++i) {
                if (p == end) return false;
                const uint8_t raw_op = *p++;
                if (raw_op > uint8_t(MixedOp::DefineViewAndUse)) return false;
                const MixedOp op = MixedOp(raw_op);
                if (op == MixedOp::Inline) {
                    uint64_t len = 0;
                    if (!get_varint(p, end, len) || len > uint64_t(end - p) || bytes.size() + len > n)
                        return false;
                    bytes.insert(bytes.end(), p, p + len); p += len;
                } else if (op == MixedOp::RefView) {
                    uint64_t view = 0;
                    if (!get_varint(p, end, view) || !view || view >= views_.size()) return false;
                    const View& v = views_[view];
                    const Ref& owner = refs_.at(v.region);
                    if (v.off > owner.len || v.len > owner.len - v.off || bytes.size() + v.len > n)
                        return false;
                    bytes.insert(bytes.end(), arena_.begin() + owner.off + v.off,
                                 arena_.begin() + owner.off + v.off + v.len);
                } else {
                    uint64_t view = 0, owner_id = 0, off = 0, len = 0;
                    if (!get_varint(p, end, view) || view != views_.size() ||
                        !get_varint(p, end, owner_id) || !owner_id || owner_id >= refs_.size() ||
                        !get_varint(p, end, off) || !get_varint(p, end, len)) return false;
                    const Ref& owner = refs_[owner_id];
                    if (off > owner.len || len > owner.len - off || bytes.size() + len > n)
                        return false;
                    views_.push_back({uint32_t(owner_id), uint32_t(off), uint32_t(len)});
                    bytes.insert(bytes.end(), arena_.begin() + owner.off + off,
                                 arena_.begin() + owner.off + off + len);
                }
            }
        } else {
            uint64_t source64 = 0, marker_len = 0, count = 0;
            if (!get_varint(p, end, source64) || !sources_ || source64 >= sources_->size() ||
                !get_varint(p, end, marker_len) || marker_len > uint64_t(end - p)) return false;
            const SourceAsset& source = sources_->asset(uint32_t(source64));
            bytes.insert(bytes.end(), p, p + marker_len); p += marker_len;
            if (!get_varint(p, end, count) || count > (1u << 20)) return false;
            for (uint64_t i = 0; i < count; ++i) {
                if (p == end) return false;
                const uint8_t raw_op = *p++;
                if (raw_op > uint8_t(SourceOp::Insert)) return false;
                const SourceOp op = SourceOp(raw_op);
                if (op == SourceOp::Copy) {
                    uint64_t line = 0;
                    if (!get_varint(p, end, line) || line >= source.lines.size()) return false;
                    const Span& span = source.lines[line];
                    bytes.insert(bytes.end(), source.data.begin() + span.off,
                                 source.data.begin() + span.off + span.len);
                } else if (op == SourceOp::Patch) {
                    uint64_t line = 0, out_len = 0, prefix = 0, suffix = 0, middle = 0;
                    if (!get_varint(p, end, line) || line >= source.lines.size() ||
                        !get_varint(p, end, out_len) || !get_varint(p, end, prefix) ||
                        !get_varint(p, end, suffix) || !get_varint(p, end, middle)) return false;
                    const Span& span = source.lines[line];
                    if (prefix > span.len || suffix > span.len - prefix || prefix + suffix > out_len ||
                        middle != out_len - prefix - suffix || middle > uint64_t(end - p)) return false;
                    bytes.insert(bytes.end(), source.data.begin() + span.off,
                                 source.data.begin() + span.off + prefix);
                    bytes.insert(bytes.end(), p, p + middle); p += middle;
                    bytes.insert(bytes.end(), source.data.begin() + span.off + span.len - suffix,
                                 source.data.begin() + span.off + span.len);
                } else {
                    uint64_t len = 0;
                    if (!get_varint(p, end, len) || len > uint64_t(end - p)) return false;
                    bytes.insert(bytes.end(), p, p + len); p += len;
                }
                if (bytes.size() > n) return false;
            }
        }
        return bytes.size() == n && install(id, bytes);
    }

    const AssetLookup* static_regions_ = nullptr;
    const SourceModel* sources_ = nullptr;
    std::vector<uint8_t> arena_;
    std::vector<Ref> refs_;
    struct View { uint32_t region = 0, off = 0, len = 0; };
    std::vector<View> views_;
};

class FrameCodec {
public:
    explicit FrameCodec(int level) : level_(level), cctx_(ZSTD_createCCtx()), dctx_(ZSTD_createDCtx()) {
        if (!cctx_ || !dctx_) die("zstd context allocation failed");
    }
    ~FrameCodec() { ZSTD_freeCCtx(cctx_); ZSTD_freeDCtx(dctx_); }
    FrameCodec(const FrameCodec&) = delete;
    FrameCodec& operator=(const FrameCodec&) = delete;

    bool compress(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
        ZSTD_CCtx_reset(cctx_, ZSTD_reset_session_and_parameters);
        ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, level_);
        output.resize(ZSTD_compressBound(input.size()));
        const size_t n = ZSTD_compress2(cctx_, output.data(), output.size(), input.data(), input.size());
        if (ZSTD_isError(n)) return false;
        output.resize(n); return true;
    }
    bool decompress(const std::vector<uint8_t>& input, size_t size, std::vector<uint8_t>& output) {
        output.resize(size);
        ZSTD_DCtx_reset(dctx_, ZSTD_reset_session_only);
        const size_t n = ZSTD_decompressDCtx(dctx_, output.data(), output.size(), input.data(), input.size());
        return !ZSTD_isError(n) && n == size;
    }

private:
    int level_;
    ZSTD_CCtx* cctx_;
    ZSTD_DCtx* dctx_;
};

enum RowModes : uint8_t { AllowStatic = 1, AllowSource = 2, AllowPrior = 4, AllowMixed = 8 };

struct Row {
    std::string name;
    uint64_t model_budget = 0, region_budget = 0, source_budget = 0;
    uint8_t modes = 0;
    RegionDecoder decoder;
    std::unique_ptr<LineViewState> views;
    FrameCodec frames;
    uint64_t wire = 0, raw_records = 0;
    std::array<uint64_t, 5> mode_count{};
    uint64_t fallback_frames = 0;
    double encode_seconds = 0, decode_seconds = 0;
    bool exact = true;

    Row(std::string n, uint64_t budget, uint64_t rb, uint64_t sb, uint8_t m,
        const AssetLookup* assets, const SourceModel* sources, const RegionTruth* truth, int level)
        : name(std::move(n)), model_budget(budget), region_budget(rb), source_budget(sb), modes(m),
          decoder(assets, sources), frames(level) {
        if (modes & AllowMixed) views = std::make_unique<LineViewState>(truth);
    }
};

struct RegionEvent {
    uint32_t id = 0;
    ParsedRegion parsed;
    uint32_t static_generic = UINT32_MAX;
    uint32_t static_toolchain = UINT32_MAX;
    uint32_t source_id = UINT32_MAX;
    std::vector<uint32_t> prior_candidates;
};

static uint64_t location_key(const Marker& marker) {
    const uint64_t path = hash_bytes(reinterpret_cast<const uint8_t*>(marker.path.data()),
                                     uint32_t(marker.path.size()), 0x726567696f6eULL);
    return mix64(path ^ (uint64_t(marker.line) << 16) ^ marker.flags) | 1ULL;
}

class PriorIndex {
public:
    std::vector<uint32_t> query(const ParsedRegion& region) const {
        if (!region.marker_ok) return {};
        auto it = recent_.find(location_key(region.marker));
        return it == recent_.end() ? std::vector<uint32_t>{} : it->second;
    }
    void publish(const ParsedRegion& region, uint32_t id) {
        if (!region.marker_ok) return;
        auto& ids = recent_[location_key(region.marker)];
        ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
        ids.insert(ids.begin(), id);
        if (ids.size() > 4) ids.resize(4);
    }
private:
    std::unordered_map<uint64_t, std::vector<uint32_t>> recent_;
};

static RegionEncoding choose_for_row(const RegionTruth& truth, const RegionEvent& event,
                                     Row& row, const AssetLookup& generic,
                                     const AssetLookup& toolchain, const SourceModel& sources,
                                     std::unordered_map<uint32_t, uint32_t>& planned_views,
                                     uint32_t& provisional_next) {
    (void)generic;
    RegionEncoding best = raw_encoding(event.id, truth.len(event.id));
    if (row.modes & AllowStatic) {
        const uint32_t id = toolchain.find_id(truth.data(event.id), truth.len(event.id), 1,
                                              row.region_budget);
        if (id != UINT32_MAX) {
            RegionEncoding candidate = static_encoding(event.id, truth.len(event.id), id);
            if (candidate.bytes < best.bytes) best = std::move(candidate);
        }
    }
    if ((row.modes & AllowSource) && event.parsed.marker_ok) {
        const uint32_t source_id = sources.find(event.parsed.marker.path, row.source_budget);
        if (source_id != UINT32_MAX) {
            RegionEncoding candidate = source_encoding(truth.data(event.id), truth.len(event.id),
                event.id, event.parsed, source_id, sources.asset(source_id));
            if (candidate.bytes < best.bytes) best = std::move(candidate);
        }
    }
    if (row.modes & AllowPrior) {
        for (uint32_t base : event.prior_candidates) {
            RegionEncoding candidate = prior_encoding(truth, event.id, base);
            if (candidate.bytes < best.bytes) best = std::move(candidate);
        }
    }
    if ((row.modes & AllowMixed) && row.views) {
        // Candidate construction must be transactional. A losing MixedLines
        // candidate may reserve view ids, but those ids must not become visible
        // to later Regions in the TU unless the defining candidate wins.
        auto trial_views = planned_views;
        uint32_t trial_next = provisional_next;
        RegionEncoding candidate = row.views->plan(event.id, event.parsed, trial_views,
                                                   trial_next);
        if (candidate.bytes < best.bytes) {
            best = std::move(candidate);
            planned_views = std::move(trial_views);
            provisional_next = trial_next;
        }
    }
    return best;
}

static bool process_row(Row& row, const RegionTruth& truth, const std::vector<RegionEvent>& events,
                        const std::vector<uint32_t>& root, const std::vector<uint8_t>& original,
                        const AssetLookup& generic, const AssetLookup& toolchain,
                        const SourceModel& sources) {
    if (events.empty()) return row.decoder.verify_tu(root, original.data(), original.size());
    const Clock::time_point encode_begin = Clock::now();
    std::vector<uint8_t> selected, raw;
    put_varint(selected, events.size()); put_varint(raw, events.size());
    std::array<uint64_t, 5> selected_modes{};
    std::vector<RegionEncoding> chosen;
    chosen.reserve(events.size());
    std::unordered_map<uint32_t, uint32_t> planned_views;
    uint32_t provisional_next = row.views ? row.views->next_view_id() : 1;
    for (const RegionEvent& event : events) {
        RegionEncoding encoding = choose_for_row(truth, event, row, generic, toolchain, sources,
                                                 planned_views, provisional_next);
        serialize_region(truth, encoding, selected);
        ++selected_modes[uint8_t(encoding.mode)];
        chosen.push_back(std::move(encoding));
        serialize_region(truth, raw_encoding(event.id, truth.len(event.id)), raw);
    }
    std::vector<uint8_t> selected_z, raw_z;
    if (!row.frames.compress(selected, selected_z) || !row.frames.compress(raw, raw_z)) return false;
    if (raw_z.size() < selected_z.size()) {
        selected.swap(raw); selected_z.swap(raw_z);
        selected_modes = {}; selected_modes[0] = events.size(); ++row.fallback_frames;
        for (size_t i = 0; i < chosen.size(); ++i)
            chosen[i] = raw_encoding(events[i].id, truth.len(events[i].id));
    }
    row.wire += selected_z.size() + 4;
    row.raw_records += selected.size();
    for (size_t i = 0; i < selected_modes.size(); ++i) row.mode_count[i] += selected_modes[i];
    row.encode_seconds += seconds(encode_begin);

    const Clock::time_point decode_begin = Clock::now();
    std::vector<uint8_t> decoded;
    const bool ok = row.frames.decompress(selected_z, selected.size(), decoded) && decoded == selected &&
                    row.decoder.decode_frame(decoded.data(), decoded.size()) &&
                    row.decoder.verify_tu(root, original.data(), original.size());
    row.decode_seconds += seconds(decode_begin);
    row.exact &= ok;
    if (ok && row.views)
        for (size_t i = 0; i < events.size(); ++i)
            row.views->commit(events[i].id, events[i].parsed, &chosen[i]);
    return ok;
}

int main(int argc, char** argv) {
    std::vector<std::string> training;
    std::string target, name = "target";
    size_t max_files = SIZE_MAX;
    int zlevel = 1;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--train") && i + 1 < argc) training.emplace_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--target") && i + 1 < argc) target = argv[++i];
        else if (!std::strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!std::strcmp(argv[i], "--max-files") && i + 1 < argc)
            max_files = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--z") && i + 1 < argc) zlevel = std::atoi(argv[++i]);
        else die(std::string("unknown/incomplete argument: ") + argv[i]);
    }
    if (training.empty() || target.empty() || training.size() > 63 || !max_files ||
        zlevel < 0 || zlevel > 3)
        die("usage: region_codec_bench --train M... --target M [--name N] [--max-files N] [--z 0..3]");

    const Clock::time_point all_begin = Clock::now();
    TrainingIndex train;
    scan_training(training, max_files, train);
    AssetLookup generic(select_assets(train, false, kBudgets.back(), 1));
    AssetLookup toolchain(select_assets(train, true, kBudgets.back(), 1));
    SourceModel sources(train, kBudgets.back());
    std::fprintf(stderr, "models generic-regions=%zu toolchain-regions=%zu sources=%zu\n",
                 generic.size(), toolchain.size(), sources.size());

    RegionTruth truth;
    PriorIndex priors;
    std::vector<std::unique_ptr<Row>> rows;
    rows.emplace_back(std::make_unique<Row>("raw", 0, 0, 0, 0,
                                            &toolchain, &sources, &truth, zlevel));
    rows.emplace_back(std::make_unique<Row>("prior", 0, 0, 0, AllowPrior,
                                            &toolchain, &sources, &truth, zlevel));
    rows.emplace_back(std::make_unique<Row>("line-view", 0, 0, 0, AllowMixed,
                                            &toolchain, &sources, &truth, zlevel));
    rows.emplace_back(std::make_unique<Row>("static-max", kBudgets.back(), kBudgets.back(), 0,
                                            AllowStatic, &toolchain, &sources, &truth, zlevel));
    rows.emplace_back(std::make_unique<Row>("source-max", kBudgets.back(), 0, kBudgets.back(),
                                            AllowSource, &toolchain, &sources, &truth, zlevel));
    rows.emplace_back(std::make_unique<Row>("source-prior-max", kBudgets.back(), 0, kBudgets.back(),
                                            AllowSource | AllowPrior,
                                            &toolchain, &sources, &truth, zlevel));
    rows.emplace_back(std::make_unique<Row>("all-max", kBudgets.back(), kBudgets.back(),
                                            kBudgets.back(),
                                            AllowStatic | AllowSource | AllowPrior | AllowMixed,
                                            &toolchain, &sources, &truth, zlevel));
    for (uint64_t budget : kBudgets) {
        const uint64_t region_budget = budget / 2;
        const uint64_t source_budget = budget - region_budget;
        rows.emplace_back(std::make_unique<Row>("union-" + std::to_string(budget), budget,
            region_budget, source_budget, AllowStatic | AllowSource | AllowPrior | AllowMixed,
            &toolchain, &sources, &truth, zlevel));
    }

    uint64_t raw_bytes = 0, tu_count = 0, region_occurrences = 0, first_regions = 0;
    const std::vector<std::string> paths = manifest_paths(target, max_files);
    for (uint32_t tu = 0; tu < paths.size(); ++tu) {
        std::vector<uint8_t> data = read_file(paths[tu]);
        raw_bytes += data.size(); ++tu_count;
        std::vector<RegionEvent> events;
        std::vector<uint32_t> root;
        for (const Span& span : regions(data)) {
            const uint8_t* bytes = data.data() + span.off;
            const RegionTruth::Result result = truth.intern(bytes, span.len);
            ParsedRegion parsed = parse_region(bytes, span.len);
            root.push_back(result.id); ++region_occurrences;
            if (result.first) {
                RegionEvent event;
                event.id = result.id; event.parsed = parsed;
                event.prior_candidates = priors.query(parsed);
                events.push_back(std::move(event)); ++first_regions;
            }
            priors.publish(parsed, result.id);
        }
        for (auto& row : rows)
            if (!process_row(*row, truth, events, root, data, generic, toolchain, sources))
                die("exact Region replay failed TU=" + std::to_string(tu) + " row=" + row->name);
        if ((tu + 1) % 50 == 0 || tu + 1 == paths.size())
            std::fprintf(stderr, "target %u/%zu raw=%.2f GiB distinct-regions=%zu elapsed=%.1fs\n",
                         tu + 1, paths.size(), raw_bytes / double(1ULL << 30), truth.size(),
                         seconds(all_begin));
    }

    std::printf("==== HETEROGENEOUS REGION CODEC %s ====\n", name.c_str());
    std::printf("target=%s training=%zu TUs=%llu raw=%llu region_occ=%llu distinct_regions=%llu "
                "distinct_region_bytes=%zu zstd=%d\n", target.c_str(), training.size(),
                (unsigned long long)tu_count, (unsigned long long)raw_bytes,
                (unsigned long long)region_occurrences, (unsigned long long)first_regions,
                truth.bytes(), zlevel);
    std::printf("model assets: generic_region_bytes=%llu toolchain_region_bytes=%llu "
                "source_bytes=%llu\n", (unsigned long long)generic.bytes(),
                (unsigned long long)toolchain.bytes(), (unsigned long long)sources.bytes());
    for (const auto& row : rows) {
        const double enc = row->encode_seconds ? raw_bytes / row->encode_seconds / 1e9 : 0;
        const double dec = row->decode_seconds ? raw_bytes / row->decode_seconds / 1e9 : 0;
        std::printf("ROW name=%s model_budget=%llu wire=%llu fill_ratio=%.3f "
                    "raw_records=%llu modes_raw/static/source/prior/view=%llu/%llu/%llu/%llu/%llu "
                    "line_views=%u fallback_frames=%llu enc_GBps=%.3f dec_GBps=%.3f "
                    "decoder_MiB=%.2f exact=%s\n",
                    row->name.c_str(), (unsigned long long)row->model_budget,
                    (unsigned long long)row->wire, row->wire ? double(raw_bytes) / row->wire : 0,
                    (unsigned long long)row->raw_records,
                    (unsigned long long)row->mode_count[0],
                    (unsigned long long)row->mode_count[1],
                    (unsigned long long)row->mode_count[2],
                    (unsigned long long)row->mode_count[3],
                    (unsigned long long)row->mode_count[4],
                    row->views ? row->views->views() : 0,
                    (unsigned long long)row->fallback_frames, enc, dec,
                    row->decoder.bytes() / 1048576.0, row->exact ? "PASS" : "FAIL");
    }
    struct rusage usage {};
    ::getrusage(RUSAGE_SELF, &usage);
    std::printf("elapsed=%.3f peak_RSS_MiB=%.1f complete_harness_raw_GBps=%.3f\n",
                seconds(all_begin), usage.ru_maxrss / 1024.0,
                raw_bytes / seconds(all_begin) / 1e9);
    return 0;
}
