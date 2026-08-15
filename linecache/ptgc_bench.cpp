// ptgc_bench.cpp -- exact Parameterized Token Grammar Codec capability harness (issue #16).
//
// The complete current TU is available to C.  Definitions first seen in that TU may therefore be
// reordered and may share message-local rules, while persistent state remains strictly pre-TU.
// Every row emits actual zstd-3 frames.  A separate decoder consumes only decompressed frame bytes,
// installs definitions by explicit stable ID, and byte-compares every reconstructed definition.
//
// Initial ladder implemented here:
//   P0  first-seen literal definitions in appearance order
//   P1  the same definitions in lexicographic order (stable IDs explicitly carried)
//   P4  alpha-normalized one-Line templates with equality-constrained slots
//   P10 actual-frame best of P1/P4
//
// Build with g++ -O3 -DNDEBUG -march=native -std=c++17 -Wall -Wextra -Wpedantic -Werror,
// then link ptgc_bench.cpp with -lzstd.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <zstd.h>

using Clock = std::chrono::steady_clock;

static double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

[[noreturn]] static void die(const char* what) {
    std::perror(what);
    std::exit(2);
}

[[noreturn]] static void die_msg(const char* what) {
    std::fprintf(stderr, "%s\n", what);
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

static uint64_t hash_bytes(const uint8_t* p, uint32_t n) {
    constexpr uint64_t kMul = 0x9ddfea08eb382d69ULL;
    uint64_t h = mix64(uint64_t(n) * 0xa0761d6478bd642fULL);
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

static void put_varint(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(uint8_t(v) | 0x80);
        v >>= 7;
    }
    out.push_back(uint8_t(v));
}

static uint64_t get_varint(const uint8_t*& p, const uint8_t* end) {
    uint64_t v = 0;
    unsigned shift = 0;
    while (p < end && shift <= 63) {
        const uint8_t b = *p++;
        v |= uint64_t(b & 0x7f) << shift;
        if (!(b & 0x80)) return v;
        shift += 7;
    }
    die_msg("truncated/overlong varint");
}

static void put_zigzag(std::vector<uint8_t>&out,int64_t v){put_varint(out,(uint64_t(v)<<1)^uint64_t(v>>63));}
static int64_t get_zigzag(const uint8_t*&p,const uint8_t*end){uint64_t v=get_varint(p,end);return int64_t(v>>1)^-int64_t(v&1);}

static size_t varint_size(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

struct FileSpan {
    uint64_t off = 0;
    uint32_t len = 0;
};

struct Corpus {
    std::vector<uint8_t> bytes;
    std::vector<FileSpan> files;
    uint64_t raw = 0;
};

static Corpus load_corpus(const char* manifest, size_t max_files) {
    FILE* mf = std::fopen(manifest, "r");
    if (!mf) die(manifest);
    std::vector<std::string> paths;
    char path[16384];
    uint64_t total = 0;
    while (std::fgets(path, sizeof(path), mf)) {
        size_t n = std::strlen(path);
        while (n && (path[n - 1] == '\n' || path[n - 1] == '\r')) path[--n] = 0;
        if (!n) continue;
        struct stat st {};
        if (::stat(path, &st) != 0) die(path);
        if (st.st_size < 0 || uint64_t(st.st_size) > UINT32_MAX) die_msg("unsupported TU size");
        paths.emplace_back(path);
        total += uint64_t(st.st_size);
        if (paths.size() == max_files) break;
    }
    std::fclose(mf);

    Corpus c;
    c.bytes.resize(size_t(total) + 8);
    c.files.reserve(paths.size());
    uint64_t off = 0;
    for (const std::string& path_string : paths) {
        FILE* f = std::fopen(path_string.c_str(), "rb");
        if (!f) die(path_string.c_str());
        struct stat st {};
        if (::fstat(::fileno(f), &st) != 0) die(path_string.c_str());
        const size_t n = size_t(st.st_size);
        if (n && std::fread(c.bytes.data() + off, 1, n, f) != n) die_msg("short read");
        std::fclose(f);
        c.files.push_back({off, uint32_t(n)});
        off += n;
    }
    c.raw = off;
    return c;
}

class LineStore {
public:
    struct Ref {
        uint32_t off = 0;
        uint32_t len = 0;
    };
    struct Result {
        uint32_t id = 0;
        bool first = false;
    };

    LineStore() {
        refs_.push_back({});
        rehash(size_t(1) << 20);
    }

    Result intern(const uint8_t* p, uint32_t n) {
        if (count_ * 10 > table_.size() * 7) rehash(table_.size() * 2);
        const uint64_t h = hash_bytes(p, n);
        uint32_t slot = uint32_t(h) & mask_;
        for (;;) {
            Cell& c = table_[slot];
            if (!c.hash) {
                if (arena_.size() + n > UINT32_MAX) die_msg("Line arena exceeds 4 GiB");
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
    size_t size() const { return refs_.size() - 1; }
    uint64_t bytes() const { return arena_.size(); }

private:
    struct Cell {
        uint64_t hash = 0;
        uint32_t id = 0;
    };
    void rehash(size_t n) {
        std::vector<Cell> next(n);
        const uint32_t next_mask = uint32_t(n - 1);
        for (const Cell& c : table_) {
            if (!c.hash) continue;
            uint32_t slot = uint32_t(c.hash) & next_mask;
            while (next[slot].hash) slot = (slot + 1) & next_mask;
            next[slot] = c;
        }
        table_.swap(next);
        mask_ = next_mask;
    }
    std::vector<Cell> table_;
    std::vector<Ref> refs_;
    std::vector<uint8_t> arena_;
    uint32_t mask_ = 0;
    size_t count_ = 0;
};

static bool is_ident_start(uint8_t c) {
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool is_ident_continue(uint8_t c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static bool is_keyword(const uint8_t* p, uint32_t n) {
    static constexpr std::string_view words[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
        "break", "case", "catch", "char", "char16_t", "char32_t", "class", "compl", "concept",
        "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
        "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
        "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
        "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
        "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
        "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
        "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
    };
    const std::string_view word(reinterpret_cast<const char*>(p), n);
    return std::binary_search(std::begin(words), std::end(words), word);
}

enum SlotType : uint8_t {
    kIdentifier = 1,
    kNumber = 2,
    kString = 3
};

struct ParsedLine {
    std::string key;
    std::string coarse_key;
    std::vector<std::vector<uint8_t>> literals;
    std::vector<uint32_t> occurrence_slot;
    std::vector<uint8_t> slot_type;
    std::vector<std::vector<uint8_t>> values;
};

static void key_varint(std::string& key, uint64_t v) {
    while (v >= 0x80) {
        key.push_back(char(uint8_t(v) | 0x80));
        v >>= 7;
    }
    key.push_back(char(uint8_t(v)));
}

static ParsedLine parse_line(const uint8_t* p, uint32_t n,bool parameterize_keywords=false) {
    ParsedLine out;
    std::vector<uint8_t> literal;
    std::unordered_map<std::string, uint32_t> local;
    uint32_t i = 0;
    auto emit_slot = [&](uint8_t type, const uint8_t* token, uint32_t len) {
        std::string lookup;
        lookup.reserve(size_t(len) + 1);
        lookup.push_back(char(type));
        lookup.append(reinterpret_cast<const char*>(token), len);
        auto it = local.find(lookup);
        uint32_t slot = 0;
        if (it == local.end()) {
            slot = uint32_t(out.values.size());
            local.emplace(std::move(lookup), slot);
            out.slot_type.push_back(type);
            out.values.emplace_back(token, token + len);
        } else {
            slot = it->second;
        }
        out.literals.push_back(std::move(literal));
        literal.clear();
        out.occurrence_slot.push_back(slot);
    };

    while (i < n) {
        if (is_ident_start(p[i])) {
            const uint32_t begin = i++;
            while (i < n && is_ident_continue(p[i])) ++i;
            if (!parameterize_keywords&&is_keyword(p + begin, i - begin)) literal.insert(literal.end(), p + begin, p + i);
            else emit_slot(kIdentifier, p + begin, i - begin);
        } else if (p[i] >= '0' && p[i] <= '9') {
            const uint32_t begin = i++;
            while (i < n) {
                const uint8_t c = p[i];
                if (!(is_ident_continue(c) || c == '.' || c == '\'' || c == '+' || c == '-')) break;
                ++i;
            }
            emit_slot(kNumber, p + begin, i - begin);
        } else if (p[i] == '"' || p[i] == '\'') {
            const uint8_t quote = p[i];
            const uint32_t begin = i++;
            bool escaped = false;
            while (i < n) {
                const uint8_t c = p[i++];
                if (!escaped && c == quote) break;
                if (!escaped && c == '\\') escaped = true;
                else escaped = false;
            }
            emit_slot(kString, p + begin, i - begin);
        } else {
            literal.push_back(p[i++]);
        }
    }
    out.literals.push_back(std::move(literal));

    key_varint(out.key, out.values.size());
    for (uint8_t type : out.slot_type) out.key.push_back(char(type));
    key_varint(out.key, out.occurrence_slot.size());
    for (size_t k = 0; k < out.occurrence_slot.size(); ++k) {
        key_varint(out.key, out.literals[k].size());
        out.key.append(reinterpret_cast<const char*>(out.literals[k].data()), out.literals[k].size());
        key_varint(out.key, out.occurrence_slot[k]);
    }
    key_varint(out.key, out.literals.back().size());
    out.key.append(reinterpret_cast<const char*>(out.literals.back().data()), out.literals.back().size());
    key_varint(out.coarse_key,out.occurrence_slot.size());
    for(size_t k=0;k<out.occurrence_slot.size();++k){
        key_varint(out.coarse_key,out.literals[k].size());out.coarse_key.append(reinterpret_cast<const char*>(out.literals[k].data()),out.literals[k].size());
        out.coarse_key.push_back(char(out.slot_type[out.occurrence_slot[k]]));
    }
    key_varint(out.coarse_key,out.literals.back().size());out.coarse_key.append(reinterpret_cast<const char*>(out.literals.back().data()),out.literals.back().size());
    return out;
}

struct FrameCodec {
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    std::vector<uint8_t> scratch;

    ~FrameCodec() {
        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
    }

    std::vector<uint8_t> compress(const std::vector<uint8_t>& raw, int level) {
        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
        std::vector<uint8_t> out(ZSTD_compressBound(raw.size()));
        const size_t n = ZSTD_compress2(cctx, out.data(), out.size(), raw.empty() ? nullptr : raw.data(), raw.size());
        if (ZSTD_isError(n)) die_msg(ZSTD_getErrorName(n));
        out.resize(n);
        return out;
    }

    const std::vector<uint8_t>& decompress(const std::vector<uint8_t>& frame) {
        const unsigned long long size = ZSTD_getFrameContentSize(frame.data(), frame.size());
        if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > SIZE_MAX)
            die_msg("bad zstd frame size");
        scratch.resize(size_t(size));
        const size_t n = ZSTD_decompressDCtx(dctx, scratch.data(), scratch.size(), frame.data(), frame.size());
        if (ZSTD_isError(n) || n != scratch.size()) die_msg("zstd decode failed");
        return scratch;
    }
};

struct PackedFrame {
    std::vector<uint8_t> bytes;
    bool dictionary = false;
};

// The serialized pretrained model is also useful zstd history: it contains the exact literal
// fragments and common slot values used by its parameterized superblocks.  Compare ordinary and
// dictionary frames on the actual bytes, carry one explicit selector byte, and charge both the
// selector and the model package.  CDict/DDict construction is outside per-TU timing, as it is a
// once-per-model operation at C/F startup.
struct ModelFrameCodec {
    FrameCodec plain;
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    ZSTD_CDict* cdict = nullptr;
    ZSTD_DDict* ddict = nullptr;
    std::vector<uint8_t> scratch;
    uint64_t dictionary_wins = 0;
    uint64_t dictionary_saved = 0;

    ModelFrameCodec(const std::vector<uint8_t>& dictionary, int level) {
        if (!dictionary.empty()) {
            cdict = ZSTD_createCDict(dictionary.data(), dictionary.size(), level);
            ddict = ZSTD_createDDict(dictionary.data(), dictionary.size());
            if (!cdict || !ddict) die_msg("cannot create pretrained zstd dictionary");
        }
    }
    ~ModelFrameCodec() {
        ZSTD_freeCDict(cdict);
        ZSTD_freeDDict(ddict);
        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
    }

    PackedFrame compress_best(const std::vector<uint8_t>& raw, int level) {
        PackedFrame result;
        result.bytes = plain.compress(raw, level);
        if (!cdict) return result;
        std::vector<uint8_t> candidate(ZSTD_compressBound(raw.size()));
        const size_t n = ZSTD_compress_usingCDict(cctx, candidate.data(), candidate.size(),
                                                  raw.empty() ? nullptr : raw.data(), raw.size(), cdict);
        if (ZSTD_isError(n)) die_msg(ZSTD_getErrorName(n));
        candidate.resize(n);
        if (candidate.size() < result.bytes.size()) {
            dictionary_saved += result.bytes.size() - candidate.size();
            ++dictionary_wins;
            result.bytes = std::move(candidate);
            result.dictionary = true;
        }
        return result;
    }

    const std::vector<uint8_t>& decompress(const PackedFrame& frame) {
        if (!frame.dictionary) return plain.decompress(frame.bytes);
        const unsigned long long size = ZSTD_getFrameContentSize(frame.bytes.data(), frame.bytes.size());
        if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > SIZE_MAX)
            die_msg("bad pretrained zstd frame size");
        scratch.resize(size_t(size));
        const size_t n = ZSTD_decompress_usingDDict(dctx, scratch.data(), scratch.size(),
                                                     frame.bytes.data(), frame.bytes.size(), ddict);
        if (ZSTD_isError(n) || n != scratch.size()) die_msg("pretrained zstd decode failed");
        return scratch;
    }
};

struct DecoderStore {
    std::vector<std::vector<uint8_t>> lines{1};

    void ensure(uint32_t id) {
        if (lines.size() <= id) lines.resize(size_t(id) + 1);
    }
    void install(uint32_t id, std::vector<uint8_t> bytes) {
        ensure(id);
        if (!lines[id].empty()) die_msg("definition installed twice");
        lines[id] = std::move(bytes);
    }
};

static std::vector<uint8_t> encode_literals(const std::vector<uint32_t>& ids, const LineStore& store) {
    std::vector<uint8_t> out;
    put_varint(out, ids.size());
    for (uint32_t id : ids) {
        put_varint(out, id);
        const uint32_t len = store.len(id);
        put_varint(out, len);
        const uint8_t* p = store.data(id);
        out.insert(out.end(), p, p + len);
    }
    return out;
}

static void decode_literals(const std::vector<uint8_t>& raw, DecoderStore& store) {
    const uint8_t* p = raw.data();
    const uint8_t* end = p + raw.size();
    const uint64_t count = get_varint(p, end);
    for (uint64_t i = 0; i < count; ++i) {
        const uint32_t id = uint32_t(get_varint(p, end));
        const uint64_t len = get_varint(p, end);
        if (len > uint64_t(end - p)) die_msg("literal record exceeds frame");
        store.install(id, std::vector<uint8_t>(p, p + len));
        p += len;
    }
    if (p != end) die_msg("trailing literal-frame bytes");
}

struct TemplateStats {
    uint64_t rules = 0;
    uint64_t instances = 0;
    uint64_t unique_slots = 0;
    uint64_t slot_occurrences = 0;
    uint64_t literal_fallbacks = 0;
    uint64_t template_input_bytes = 0;
    uint64_t lexicon_entries = 0;
    uint64_t lexicon_references = 0;
};

struct TemplateGroup {
    ParsedLine shape;
    std::vector<size_t> members;
    uint32_t wire_id = UINT32_MAX;
};

static size_t encoded_template_size(const ParsedLine& shape) {
    size_t n = varint_size(shape.values.size()) + shape.slot_type.size();
    n += varint_size(shape.occurrence_slot.size());
    for (size_t i = 0; i < shape.occurrence_slot.size(); ++i) {
        n += varint_size(shape.literals[i].size()) + shape.literals[i].size();
        n += varint_size(shape.occurrence_slot[i]);
    }
    n += varint_size(shape.literals.back().size()) + shape.literals.back().size();
    return n;
}

static size_t encoded_instance_size(uint32_t line_id, uint32_t rule_id, const ParsedLine& parsed,
                                    uint32_t output_len) {
    size_t n = 1 + varint_size(line_id) + varint_size(output_len) + varint_size(rule_id);
    for (const auto& value : parsed.values) n += varint_size(value.size()) + value.size();
    return n;
}

static size_t encoded_literal_size(uint32_t line_id, uint32_t len) {
    return 1 + varint_size(line_id) + varint_size(len) + len;
}

struct SplitBuffer {std::vector<uint8_t> control,data;};

static SplitBuffer encode_templates(const std::vector<uint32_t>& ids, const LineStore& store,
                                    TemplateStats& stats,bool parameterize_keywords=false) {
    std::vector<ParsedLine> parsed;
    parsed.reserve(ids.size());
    std::vector<TemplateGroup> groups;
    std::unordered_map<std::string, uint32_t> by_key;
    by_key.reserve(ids.size() * 2 + 1);
    for (size_t i = 0; i < ids.size(); ++i) {
        const uint32_t id = ids[i];
        parsed.push_back(parse_line(store.data(id), store.len(id),parameterize_keywords));
        auto [it, inserted] = by_key.emplace(parsed.back().key, uint32_t(groups.size()));
        if (inserted) {
            TemplateGroup group;
            group.shape = parsed.back();
            groups.push_back(std::move(group));
        }
        groups[it->second].members.push_back(i);
    }

    uint32_t rule_count = 0;
    for (TemplateGroup& group : groups) {
        if (group.members.size() < 2 || group.shape.values.empty()) continue;
        size_t literal_cost = 0;
        size_t rule_cost = encoded_template_size(group.shape);
        for (size_t index : group.members) {
            const uint32_t id = ids[index];
            literal_cost += encoded_literal_size(id, store.len(id));
            rule_cost += encoded_instance_size(id, rule_count, parsed[index], store.len(id));
        }
        if (rule_cost < literal_cost) group.wire_id = rule_count++;
    }

    std::unordered_map<std::string, uint32_t> value_frequency;
    for (const TemplateGroup& group : groups) if (group.wire_id != UINT32_MAX) {
        for (size_t index : group.members) for (const auto& value : parsed[index].values) {
            ++value_frequency[std::string(reinterpret_cast<const char*>(value.data()), value.size())];
        }
    }
    std::vector<std::pair<std::string,uint32_t>> lexicon_candidates;
    lexicon_candidates.reserve(value_frequency.size());
    for (const auto& item : value_frequency) {
        const size_t len=item.first.size(); const uint64_t repeated=uint64_t(item.second)*(varint_size(len)+len);
        const uint64_t dictionary=varint_size(len)+len+uint64_t(item.second)*2;
        if(item.second>=2&&dictionary<repeated)lexicon_candidates.push_back(item);
    }
    std::sort(lexicon_candidates.begin(),lexicon_candidates.end(),[](const auto&a,const auto&b){
        if(a.second!=b.second)return a.second>b.second;
        return a.first<b.first;});
    std::unordered_map<std::string,uint32_t> lexicon_id; lexicon_id.reserve(lexicon_candidates.size()*2+1);
    for(uint32_t i=0;i<lexicon_candidates.size();++i)lexicon_id.emplace(lexicon_candidates[i].first,i);

    SplitBuffer out;
    put_varint(out.control, rule_count);
    for (const TemplateGroup& group : groups) {
        if (group.wire_id == UINT32_MAX) continue;
        const ParsedLine& shape = group.shape;
        put_varint(out.control, shape.values.size());
        out.control.insert(out.control.end(), shape.slot_type.begin(), shape.slot_type.end());
        put_varint(out.control, shape.occurrence_slot.size());
        for (size_t i = 0; i < shape.occurrence_slot.size(); ++i) {
            put_varint(out.control, shape.literals[i].size());
            out.data.insert(out.data.end(), shape.literals[i].begin(), shape.literals[i].end());
            put_varint(out.control, shape.occurrence_slot[i]);
        }
        put_varint(out.control, shape.literals.back().size());
        out.data.insert(out.data.end(), shape.literals.back().begin(), shape.literals.back().end());
        ++stats.rules;
        stats.unique_slots += shape.values.size();
        stats.slot_occurrences += shape.occurrence_slot.size();
    }

    put_varint(out.control,lexicon_candidates.size());
    for(const auto&item:lexicon_candidates){put_varint(out.control,item.first.size());out.data.insert(out.data.end(),item.first.begin(),item.first.end());}
    stats.lexicon_entries+=lexicon_candidates.size();

    uint64_t record_blocks=0;for(const TemplateGroup&group:groups)record_blocks+=group.wire_id==UINT32_MAX?group.members.size():1;
    put_varint(out.control, record_blocks);int64_t previous_line_id=0;
    for (const TemplateGroup& group : groups) {
        if (group.wire_id != UINT32_MAX) {
            std::vector<size_t>order=group.members;std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return ids[a]<ids[b];});
            out.control.push_back(2);put_varint(out.control,group.wire_id);put_varint(out.control,order.size());
            for(size_t index:order){uint32_t id=ids[index];put_zigzag(out.control,int64_t(id)-previous_line_id);previous_line_id=id;put_varint(out.control,store.len(id));}
            for(size_t slot=0;slot<group.shape.values.size();++slot)for(size_t index:order){const auto&value=parsed[index].values[slot];
                    const std::string key(reinterpret_cast<const char*>(value.data()),value.size());auto it=lexicon_id.find(key);
                    if(it!=lexicon_id.end()){put_varint(out.control,uint64_t(it->second)<<1);++stats.lexicon_references;}
                    else {put_varint(out.control,(uint64_t(value.size())<<1)|1);out.data.insert(out.data.end(),value.begin(),value.end());}
                }
            for(size_t index:order){const uint32_t id=ids[index];
                ++stats.instances;
                stats.template_input_bytes += store.len(id);
            }
        } else {
            for (size_t index : group.members) {
                const uint32_t id = ids[index];
                out.control.push_back(0);
                put_zigzag(out.control,int64_t(id)-previous_line_id);previous_line_id=id;
                put_varint(out.control, store.len(id));
                const uint8_t* p = store.data(id);
                out.data.insert(out.data.end(), p, p + store.len(id));
                ++stats.literal_fallbacks;
            }
        }
    }
    return out;
}

struct DecodedTemplate {
    std::vector<std::vector<uint8_t>> literals;
    std::vector<uint32_t> occurrence_slot;
    std::vector<uint8_t> slot_type;
};

static void decode_templates(const std::vector<uint8_t>& control,const std::vector<uint8_t>& data, DecoderStore& store) {
    const uint8_t* p = control.data();
    const uint8_t* end = p + control.size();
    const uint8_t*q=data.data(),*data_end=q+data.size();
    const uint64_t rule_count = get_varint(p, end);
    std::vector<DecodedTemplate> rules;
    rules.reserve(size_t(rule_count));
    for (uint64_t r = 0; r < rule_count; ++r) {
        DecodedTemplate rule;
        const uint64_t slots = get_varint(p, end);
        if (slots > uint64_t(end - p)) die_msg("bad template slot count");
        rule.slot_type.assign(p, p + slots);
        p += slots;
        const uint64_t occurrences = get_varint(p, end);
        rule.literals.reserve(size_t(occurrences) + 1);
        rule.occurrence_slot.reserve(size_t(occurrences));
        for (uint64_t i = 0; i < occurrences; ++i) {
            const uint64_t len = get_varint(p, end);
            if (len > uint64_t(data_end - q)) die_msg("bad template literal");
            rule.literals.emplace_back(q, q + len);
            q += len;
            const uint64_t slot = get_varint(p, end);
            if (slot >= slots) die_msg("template slot out of range");
            rule.occurrence_slot.push_back(uint32_t(slot));
        }
        const uint64_t final_len = get_varint(p, end);
        if (final_len > uint64_t(data_end - q)) die_msg("bad final template literal");
        rule.literals.emplace_back(q, q + final_len);
        q += final_len;
        rules.push_back(std::move(rule));
    }

    const uint64_t lexicon_count=get_varint(p,end);std::vector<std::vector<uint8_t>> lexicon;lexicon.reserve(size_t(lexicon_count));
    for(uint64_t i=0;i<lexicon_count;++i){const uint64_t len=get_varint(p,end);if(len>uint64_t(data_end-q))die_msg("lexicon value exceeds frame");lexicon.emplace_back(q,q+len);q+=len;}

    const uint64_t records = get_varint(p, end);int64_t previous_line_id=0;
    for (uint64_t i = 0; i < records; ++i) {
        if (p == end) die_msg("missing definition mode");
        const uint8_t mode = *p++;
        if (mode == 0) {
            previous_line_id+=get_zigzag(p,end);const uint32_t id = uint32_t(previous_line_id);
            const uint64_t output_len = get_varint(p, end);
            if (output_len > uint64_t(data_end - q)) die_msg("raw definition exceeds frame");
            store.install(id, std::vector<uint8_t>(q, q + output_len));
            q += output_len;
            continue;
        }
        if(mode==2){const uint64_t rule_id=get_varint(p,end);if(rule_id>=rules.size())die_msg("group rule id out of range");const DecodedTemplate&rule=rules[size_t(rule_id)];const uint64_t count=get_varint(p,end);
            const size_t rows=static_cast<size_t>(count);std::vector<uint32_t>ids(rows);std::vector<uint64_t>lengths(rows);for(size_t row=0;row<rows;++row){previous_line_id+=get_zigzag(p,end);ids[row]=uint32_t(previous_line_id);lengths[row]=get_varint(p,end);}
            std::vector<std::vector<std::vector<uint8_t>>>values(rows,std::vector<std::vector<uint8_t>>(rule.slot_type.size()));
            for(size_t slot=0;slot<rule.slot_type.size();++slot)for(size_t row=0;row<rows;++row){uint64_t code=get_varint(p,end);if(!(code&1)){uint64_t id=code>>1;if(id>=lexicon.size())die_msg("group lexicon id out of range");values[row][slot]=lexicon[size_t(id)];}
                else{uint64_t len=code>>1;if(len>uint64_t(data_end-q))die_msg("group slot exceeds stream");values[row][slot].assign(q,q+len);q+=len;}}
            for(size_t row=0;row<rows;++row){std::vector<uint8_t>result;result.reserve(size_t(lengths[row]));for(size_t k=0;k<rule.occurrence_slot.size();++k){result.insert(result.end(),rule.literals[k].begin(),rule.literals[k].end());const auto&value=values[row][rule.occurrence_slot[k]];result.insert(result.end(),value.begin(),value.end());}result.insert(result.end(),rule.literals.back().begin(),rule.literals.back().end());if(result.size()!=lengths[row])die_msg("group output length mismatch");store.install(ids[row],std::move(result));}
            continue;}
        if (mode != 1) die_msg("unknown definition mode");
        previous_line_id+=get_zigzag(p,end);const uint32_t id = uint32_t(previous_line_id);
        const uint64_t output_len = get_varint(p, end);
        const uint64_t rule_id = get_varint(p, end);
        if (rule_id >= rules.size()) die_msg("rule id out of range");
        const DecodedTemplate& rule = rules[size_t(rule_id)];
        std::vector<std::vector<uint8_t>> values(rule.slot_type.size());
        for (auto& value : values) {
            const uint64_t code=get_varint(p,end);
            if(!(code&1)){const uint64_t id=code>>1;if(id>=lexicon.size())die_msg("lexicon id out of range");value=lexicon[size_t(id)];}
            else {const uint64_t len=code>>1;if(len>uint64_t(data_end-q))die_msg("slot value exceeds frame");value.assign(q,q+len);q+=len;}
        }
        std::vector<uint8_t> result;
        result.reserve(size_t(output_len));
        for (size_t k = 0; k < rule.occurrence_slot.size(); ++k) {
            result.insert(result.end(), rule.literals[k].begin(), rule.literals[k].end());
            const auto& value = values[rule.occurrence_slot[k]];
            result.insert(result.end(), value.begin(), value.end());
        }
        result.insert(result.end(), rule.literals.back().begin(), rule.literals.back().end());
        if (result.size() != output_len) die_msg("template output length mismatch");
        store.install(id, std::move(result));
    }
    if (p != end||q!=data_end) die_msg("trailing template-frame bytes");
}

enum SemanticChannel : size_t {kRuleLiteral=0,kRawDefinition=1,kIdentifierValue=2,kNumberValue=3,kStringValue=4,kSemanticChannels=5};
struct SemanticBuffer {std::vector<uint8_t>control;std::array<std::vector<uint8_t>,kSemanticChannels>payload;};
struct TypedLexiconEntry {uint8_t type=0;std::string value;uint32_t count=0;};

static size_t semantic_slot_channel(uint8_t type){
    if(type<kIdentifier||type>kString)die_msg("unknown semantic slot type");
    return size_t(type)+1;
}

static SemanticBuffer encode_semantic_templates(const std::vector<uint32_t>&ids,const LineStore&store,
                                                TemplateStats&stats,bool parameterize_keywords){
    std::vector<ParsedLine>parsed;parsed.reserve(ids.size());std::vector<TemplateGroup>groups;std::unordered_map<std::string,uint32_t>by_key;by_key.reserve(ids.size()*2+1);
    for(size_t i=0;i<ids.size();++i){parsed.push_back(parse_line(store.data(ids[i]),store.len(ids[i]),parameterize_keywords));auto[it,inserted]=by_key.emplace(parsed.back().key,uint32_t(groups.size()));if(inserted){TemplateGroup group;group.shape=parsed.back();groups.push_back(std::move(group));}groups[it->second].members.push_back(i);}
    uint32_t rule_count=0;for(TemplateGroup&group:groups){if(group.members.size()<2||group.shape.values.empty())continue;size_t literal_cost=0,rule_cost=encoded_template_size(group.shape);for(size_t index:group.members){const uint32_t id=ids[index];literal_cost+=encoded_literal_size(id,store.len(id));rule_cost+=encoded_instance_size(id,rule_count,parsed[index],store.len(id));}if(rule_cost<literal_cost)group.wire_id=rule_count++;}

    std::unordered_map<std::string,uint32_t>frequency;
    for(const TemplateGroup&group:groups)if(group.wire_id!=UINT32_MAX)for(size_t index:group.members)for(size_t slot=0;slot<parsed[index].values.size();++slot){const auto&value=parsed[index].values[slot];std::string key;key.reserve(value.size()+1);key.push_back(char(parsed[index].slot_type[slot]));key.append(reinterpret_cast<const char*>(value.data()),value.size());++frequency[key];}
    std::vector<TypedLexiconEntry>lexicon;
    for(const auto&item:frequency){const size_t len=item.first.size()-1;const uint64_t repeated=uint64_t(item.second)*(varint_size(len)+len),dictionary=1+varint_size(len)+len+uint64_t(item.second)*2;if(item.second>=2&&dictionary<repeated)lexicon.push_back({uint8_t(item.first[0]),item.first.substr(1),item.second});}
    std::sort(lexicon.begin(),lexicon.end(),[](const TypedLexiconEntry&a,const TypedLexiconEntry&b){if(a.count!=b.count)return a.count>b.count;if(a.type!=b.type)return a.type<b.type;return a.value<b.value;});
    std::unordered_map<std::string,uint32_t>lexicon_id;lexicon_id.reserve(lexicon.size()*2+1);for(uint32_t i=0;i<lexicon.size();++i){std::string key;key.reserve(lexicon[i].value.size()+1);key.push_back(char(lexicon[i].type));key+=lexicon[i].value;lexicon_id.emplace(std::move(key),i);}

    SemanticBuffer out;put_varint(out.control,rule_count);
    for(const TemplateGroup&group:groups)if(group.wire_id!=UINT32_MAX){const ParsedLine&shape=group.shape;put_varint(out.control,shape.values.size());out.control.insert(out.control.end(),shape.slot_type.begin(),shape.slot_type.end());put_varint(out.control,shape.occurrence_slot.size());for(size_t i=0;i<shape.occurrence_slot.size();++i){put_varint(out.control,shape.literals[i].size());out.payload[kRuleLiteral].insert(out.payload[kRuleLiteral].end(),shape.literals[i].begin(),shape.literals[i].end());put_varint(out.control,shape.occurrence_slot[i]);}put_varint(out.control,shape.literals.back().size());out.payload[kRuleLiteral].insert(out.payload[kRuleLiteral].end(),shape.literals.back().begin(),shape.literals.back().end());++stats.rules;stats.unique_slots+=shape.values.size();stats.slot_occurrences+=shape.occurrence_slot.size();}
    put_varint(out.control,lexicon.size());for(const TypedLexiconEntry&entry:lexicon){out.control.push_back(entry.type);put_varint(out.control,entry.value.size());auto&channel=out.payload[semantic_slot_channel(entry.type)];channel.insert(channel.end(),entry.value.begin(),entry.value.end());}stats.lexicon_entries+=lexicon.size();
    uint64_t blocks=0;for(const TemplateGroup&group:groups)blocks+=group.wire_id==UINT32_MAX?group.members.size():1;put_varint(out.control,blocks);int64_t previous_line_id=0;
    auto emit_value=[&](uint8_t type,const std::vector<uint8_t>&value){std::string key;key.reserve(value.size()+1);key.push_back(char(type));key.append(reinterpret_cast<const char*>(value.data()),value.size());auto it=lexicon_id.find(key);if(it!=lexicon_id.end()){put_varint(out.control,uint64_t(it->second)<<1);++stats.lexicon_references;}else{put_varint(out.control,(uint64_t(value.size())<<1)|1);auto&channel=out.payload[semantic_slot_channel(type)];channel.insert(channel.end(),value.begin(),value.end());}};
    for(const TemplateGroup&group:groups){if(group.wire_id!=UINT32_MAX){std::vector<size_t>order=group.members;std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return ids[a]<ids[b];});out.control.push_back(2);put_varint(out.control,group.wire_id);put_varint(out.control,order.size());for(size_t index:order){const uint32_t id=ids[index];put_zigzag(out.control,int64_t(id)-previous_line_id);previous_line_id=id;put_varint(out.control,store.len(id));}for(size_t slot=0;slot<group.shape.values.size();++slot)for(size_t index:order)emit_value(group.shape.slot_type[slot],parsed[index].values[slot]);stats.instances+=order.size();for(size_t index:order)stats.template_input_bytes+=store.len(ids[index]);}
        else for(size_t index:group.members){const uint32_t id=ids[index];out.control.push_back(0);put_zigzag(out.control,int64_t(id)-previous_line_id);previous_line_id=id;put_varint(out.control,store.len(id));const uint8_t*p=store.data(id);out.payload[kRawDefinition].insert(out.payload[kRawDefinition].end(),p,p+store.len(id));++stats.literal_fallbacks;}}
    return out;
}

static void decode_semantic_templates(const SemanticBuffer&raw,DecoderStore&store){
    const uint8_t*p=raw.control.data(),*end=p+raw.control.size();std::array<const uint8_t*,kSemanticChannels>cursor{},channel_end{};for(size_t c=0;c<kSemanticChannels;++c){cursor[c]=raw.payload[c].data();channel_end[c]=cursor[c]+raw.payload[c].size();}
    auto take=[&](size_t channel,uint64_t len){if(len>uint64_t(channel_end[channel]-cursor[channel]))die_msg("semantic payload exceeds channel");std::vector<uint8_t>value(cursor[channel],cursor[channel]+len);cursor[channel]+=len;return value;};
    const uint64_t rule_count=get_varint(p,end);std::vector<DecodedTemplate>rules;rules.reserve(size_t(rule_count));
    for(uint64_t r=0;r<rule_count;++r){DecodedTemplate rule;const uint64_t slots=get_varint(p,end);if(slots>uint64_t(end-p))die_msg("semantic slot count exceeds control");rule.slot_type.assign(p,p+slots);p+=slots;const uint64_t occurrences=get_varint(p,end);for(uint64_t i=0;i<occurrences;++i){const uint64_t len=get_varint(p,end);rule.literals.push_back(take(kRuleLiteral,len));const uint64_t slot=get_varint(p,end);if(slot>=slots)die_msg("semantic rule slot out of range");rule.occurrence_slot.push_back(uint32_t(slot));}rule.literals.push_back(take(kRuleLiteral,get_varint(p,end)));rules.push_back(std::move(rule));}
    const uint64_t lexicon_count=get_varint(p,end);std::vector<std::vector<uint8_t>>lexicon;lexicon.reserve(size_t(lexicon_count));for(uint64_t i=0;i<lexicon_count;++i){if(p==end)die_msg("semantic lexicon type missing");const uint8_t type=*p++;lexicon.push_back(take(semantic_slot_channel(type),get_varint(p,end)));}
    auto read_value=[&](uint8_t type){const uint64_t code=get_varint(p,end);if(!(code&1)){const uint64_t id=code>>1;if(id>=lexicon.size())die_msg("semantic lexicon id out of range");return lexicon[size_t(id)];}return take(semantic_slot_channel(type),code>>1);};
    const uint64_t blocks=get_varint(p,end);int64_t previous_line_id=0;
    for(uint64_t b=0;b<blocks;++b){if(p==end)die_msg("semantic record mode missing");const uint8_t mode=*p++;if(mode==0){previous_line_id+=get_zigzag(p,end);const uint32_t id=uint32_t(previous_line_id);store.install(id,take(kRawDefinition,get_varint(p,end)));continue;}if(mode!=2)die_msg("unknown semantic record mode");const uint64_t rule_id=get_varint(p,end);if(rule_id>=rules.size())die_msg("semantic rule id out of range");const DecodedTemplate&rule=rules[size_t(rule_id)];const size_t rows=size_t(get_varint(p,end));std::vector<uint32_t>ids(rows);std::vector<uint64_t>lengths(rows);for(size_t row=0;row<rows;++row){previous_line_id+=get_zigzag(p,end);ids[row]=uint32_t(previous_line_id);lengths[row]=get_varint(p,end);}std::vector<std::vector<std::vector<uint8_t>>>values(rows,std::vector<std::vector<uint8_t>>(rule.slot_type.size()));for(size_t slot=0;slot<rule.slot_type.size();++slot)for(size_t row=0;row<rows;++row)values[row][slot]=read_value(rule.slot_type[slot]);
        for(size_t row=0;row<rows;++row){std::vector<uint8_t>result;result.reserve(size_t(lengths[row]));for(size_t i=0;i<rule.occurrence_slot.size();++i){result.insert(result.end(),rule.literals[i].begin(),rule.literals[i].end());const auto&value=values[row][rule.occurrence_slot[i]];result.insert(result.end(),value.begin(),value.end());}result.insert(result.end(),rule.literals.back().begin(),rule.literals.back().end());if(result.size()!=lengths[row])die_msg("semantic output length mismatch");store.install(ids[row],std::move(result));}}
    if(p!=end)die_msg("trailing semantic control bytes");
    for(size_t c=0;c<kSemanticChannels;++c)if(cursor[c]!=channel_end[c])die_msg("trailing semantic payload bytes");
}

struct SemanticFrames {std::vector<uint8_t>control;std::array<std::vector<uint8_t>,kSemanticChannels>payload;uint64_t wire=0;};
static SemanticFrames compress_semantic(const SemanticBuffer&raw,FrameCodec&codec,int level){SemanticFrames out;out.control=codec.compress(raw.control,level);out.wire=out.control.size()+4;for(size_t c=0;c<kSemanticChannels;++c)if(!raw.payload[c].empty()){out.payload[c]=codec.compress(raw.payload[c],level);out.wire+=out.payload[c].size()+4;}return out;}
static SemanticBuffer decompress_semantic(const SemanticFrames&frames,FrameCodec&codec){SemanticBuffer raw;raw.control=codec.decompress(frames.control);for(size_t c=0;c<kSemanticChannels;++c)if(!frames.payload[c].empty())raw.payload[c]=codec.decompress(frames.payload[c]);return raw;}

class SemanticFrameWriter {
public:
    explicit SemanticFrameWriter(const std::string&prefix){static constexpr const char*names[]={"control","rule","raw","identifier","number","string"};for(size_t i=0;i<files_.size();++i){const std::string path=prefix+"."+names[i]+".frames";files_[i]=std::fopen(path.c_str(),"wb+");if(!files_[i])die(path.c_str());const char magic[8]={'I','C','F','R','M','1',0,0};const uint32_t version=1,count=0;if(std::fwrite(magic,1,sizeof(magic),files_[i])!=sizeof(magic)||std::fwrite(&version,sizeof(version),1,files_[i])!=1||std::fwrite(&count,sizeof(count),1,files_[i])!=1)die_msg("semantic frame header write failed");}}
    ~SemanticFrameWriter(){for(FILE*f:files_)if(f){if(std::fseek(f,12,SEEK_SET)!=0||std::fwrite(&count_,sizeof(count_),1,f)!=1)die_msg("semantic frame count write failed");std::fclose(f);}}
    void add(uint64_t raw_bytes,const SemanticBuffer&buffer){std::array<const std::vector<uint8_t>*,6>streams{{&buffer.control,&buffer.payload[kRuleLiteral],&buffer.payload[kRawDefinition],&buffer.payload[kIdentifierValue],&buffer.payload[kNumberValue],&buffer.payload[kStringValue]}};for(size_t i=0;i<files_.size();++i){if(streams[i]->size()>UINT32_MAX)die_msg("semantic frame stream too large");const uint32_t size=uint32_t(streams[i]->size());if(std::fwrite(&raw_bytes,sizeof(raw_bytes),1,files_[i])!=1||std::fwrite(&size,sizeof(size),1,files_[i])!=1||(size&&std::fwrite(streams[i]->data(),1,size,files_[i])!=size))die_msg("semantic frame write failed");}++count_;}
private:
    std::array<FILE*,6>files_{};uint32_t count_=0;
};

struct MultiUnit{std::vector<uint32_t>ids,lengths;std::vector<uint8_t>bytes;ParsedLine parsed;};
struct MultiGroup{ParsedLine shape;std::vector<size_t>members;uint32_t wire_id=UINT32_MAX;};
struct MultiStats{uint64_t rules=0,instances=0,raw_units=0,lines=0;};

static SplitBuffer encode_multiline(const std::vector<uint32_t>&ids,const LineStore&store,size_t width,MultiStats&stats){
    std::vector<MultiUnit>units;for(size_t i=0;i<ids.size();i+=width){MultiUnit u;size_t e=std::min(ids.size(),i+width);for(size_t j=i;j<e;++j){uint32_t id=ids[j];u.ids.push_back(id);u.lengths.push_back(store.len(id));const uint8_t*p=store.data(id);u.bytes.insert(u.bytes.end(),p,p+store.len(id));}u.parsed=parse_line(u.bytes.data(),uint32_t(u.bytes.size()));units.push_back(std::move(u));}
    std::vector<MultiGroup>groups;std::unordered_map<std::string,uint32_t>by_key;by_key.reserve(units.size()*2+1);for(size_t i=0;i<units.size();++i){auto[it,inserted]=by_key.emplace(units[i].parsed.key,uint32_t(groups.size()));if(inserted){MultiGroup g;g.shape=units[i].parsed;groups.push_back(std::move(g));}groups[it->second].members.push_back(i);}
    uint32_t rule_count=0;for(MultiGroup&g:groups){if(g.members.size()<2||g.shape.values.empty())continue;size_t literal=0,rule=encoded_template_size(g.shape);for(size_t m:g.members){const MultiUnit&u=units[m];literal+=1+varint_size(u.ids.size())+u.bytes.size();for(size_t j=0;j<u.ids.size();++j)literal+=varint_size(u.ids[j])+varint_size(u.lengths[j]);rule+=1+varint_size(rule_count)+varint_size(u.ids.size());for(size_t j=0;j<u.ids.size();++j)rule+=varint_size(u.ids[j])+varint_size(u.lengths[j]);for(const auto&v:u.parsed.values)rule+=varint_size(v.size())+v.size();}if(rule<literal)g.wire_id=rule_count++;}
    std::unordered_map<std::string,uint32_t>freq;for(const MultiGroup&g:groups)if(g.wire_id!=UINT32_MAX)for(size_t m:g.members)for(const auto&v:units[m].parsed.values)++freq[std::string(reinterpret_cast<const char*>(v.data()),v.size())];
    std::vector<std::pair<std::string,uint32_t>>lex;for(const auto&item:freq){size_t n=item.first.size();if(item.second>=2&&varint_size(n)+n+uint64_t(item.second)*2<uint64_t(item.second)*(varint_size(n)+n))lex.push_back(item);}std::sort(lex.begin(),lex.end(),[](const auto&a,const auto&b){if(a.second!=b.second)return a.second>b.second;return a.first<b.first;});std::unordered_map<std::string,uint32_t>lexid;for(uint32_t i=0;i<lex.size();++i)lexid.emplace(lex[i].first,i);
    SplitBuffer out;put_varint(out.control,rule_count);for(const MultiGroup&g:groups)if(g.wire_id!=UINT32_MAX){const ParsedLine&s=g.shape;put_varint(out.control,s.values.size());out.control.insert(out.control.end(),s.slot_type.begin(),s.slot_type.end());put_varint(out.control,s.occurrence_slot.size());for(size_t i=0;i<s.occurrence_slot.size();++i){put_varint(out.control,s.literals[i].size());out.data.insert(out.data.end(),s.literals[i].begin(),s.literals[i].end());put_varint(out.control,s.occurrence_slot[i]);}put_varint(out.control,s.literals.back().size());out.data.insert(out.data.end(),s.literals.back().begin(),s.literals.back().end());++stats.rules;}
    put_varint(out.control,lex.size());for(const auto&item:lex){put_varint(out.control,item.first.size());out.data.insert(out.data.end(),item.first.begin(),item.first.end());}
    uint64_t blocks=0;for(const MultiGroup&g:groups)blocks+=g.wire_id==UINT32_MAX?g.members.size():1;put_varint(out.control,blocks);
    auto value=[&](const std::vector<uint8_t>&v){std::string k(reinterpret_cast<const char*>(v.data()),v.size());auto it=lexid.find(k);if(it!=lexid.end())put_varint(out.control,uint64_t(it->second)<<1);else{put_varint(out.control,(uint64_t(v.size())<<1)|1);out.data.insert(out.data.end(),v.begin(),v.end());}};
    for(const MultiGroup&g:groups){if(g.wire_id!=UINT32_MAX){std::vector<size_t>order=g.members;std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return units[a].parsed.values<units[b].parsed.values;});out.control.push_back(1);put_varint(out.control,g.wire_id);put_varint(out.control,order.size());for(size_t m:order){const MultiUnit&u=units[m];put_varint(out.control,u.ids.size());for(size_t j=0;j<u.ids.size();++j){put_varint(out.control,u.ids[j]);put_varint(out.control,u.lengths[j]);}}
            for(size_t slot=0;slot<g.shape.values.size();++slot)for(size_t m:order)value(units[m].parsed.values[slot]);
            stats.instances+=order.size();for(size_t m:order)stats.lines+=units[m].ids.size();}
        else for(size_t m:g.members){const MultiUnit&u=units[m];out.control.push_back(0);put_varint(out.control,u.ids.size());for(size_t j=0;j<u.ids.size();++j){put_varint(out.control,u.ids[j]);put_varint(out.control,u.lengths[j]);}out.data.insert(out.data.end(),u.bytes.begin(),u.bytes.end());++stats.raw_units;stats.lines+=u.ids.size();}}
    return out;
}

static void decode_multiline(const std::vector<uint8_t>&control,const std::vector<uint8_t>&data,DecoderStore&store){
    const uint8_t*p=control.data(),*end=p+control.size(),*q=data.data(),*data_end=q+data.size();uint64_t rule_count=get_varint(p,end);std::vector<DecodedTemplate>rules;rules.reserve(size_t(rule_count));for(uint64_t r=0;r<rule_count;++r){DecodedTemplate rule;uint64_t slots=get_varint(p,end);if(slots>uint64_t(end-p))die_msg("multi slots exceed control");rule.slot_type.assign(p,p+slots);p+=slots;uint64_t occ=get_varint(p,end);for(uint64_t i=0;i<occ;++i){uint64_t n=get_varint(p,end);if(n>uint64_t(data_end-q))die_msg("multi literal exceeds data");rule.literals.emplace_back(q,q+n);q+=n;uint64_t slot=get_varint(p,end);if(slot>=slots)die_msg("multi slot out of range");rule.occurrence_slot.push_back(uint32_t(slot));}uint64_t n=get_varint(p,end);if(n>uint64_t(data_end-q))die_msg("multi tail exceeds data");rule.literals.emplace_back(q,q+n);q+=n;rules.push_back(std::move(rule));}
    uint64_t lex_count=get_varint(p,end);std::vector<std::vector<uint8_t>>lex;for(uint64_t i=0;i<lex_count;++i){uint64_t n=get_varint(p,end);if(n>uint64_t(data_end-q))die_msg("multi lex exceeds data");lex.emplace_back(q,q+n);q+=n;}
    auto read_value=[&](){uint64_t code=get_varint(p,end);if(!(code&1)){uint64_t id=code>>1;if(id>=lex.size())die_msg("multi lex id");return lex[size_t(id)];}uint64_t n=code>>1;if(n>uint64_t(data_end-q))die_msg("multi value exceeds data");std::vector<uint8_t>v(q,q+n);q+=n;return v;};
    uint64_t blocks=get_varint(p,end);for(uint64_t b=0;b<blocks;++b){if(p==end)die_msg("missing multi mode");uint8_t mode=*p++;if(mode==0){uint64_t lines=get_varint(p,end);for(uint64_t j=0;j<lines;++j){uint32_t id=uint32_t(get_varint(p,end));uint64_t n=get_varint(p,end);if(n>uint64_t(data_end-q))die_msg("multi raw exceeds data");store.install(id,std::vector<uint8_t>(q,q+n));q+=n;}continue;}if(mode!=1)die_msg("unknown multi mode");uint64_t rid=get_varint(p,end);if(rid>=rules.size())die_msg("multi rule id");const DecodedTemplate&rule=rules[size_t(rid)];uint64_t instances=get_varint(p,end);size_t rows=static_cast<size_t>(instances);std::vector<std::vector<uint32_t>>ids(rows);std::vector<std::vector<uint64_t>>lengths(rows);for(size_t row=0;row<rows;++row){uint64_t lines=get_varint(p,end);ids[row].resize(size_t(lines));lengths[row].resize(size_t(lines));for(size_t j=0;j<lines;++j){ids[row][j]=uint32_t(get_varint(p,end));lengths[row][j]=get_varint(p,end);}}
        std::vector<std::vector<std::vector<uint8_t>>>values(rows,std::vector<std::vector<uint8_t>>(rule.slot_type.size()));for(size_t slot=0;slot<rule.slot_type.size();++slot)for(size_t row=0;row<rows;++row)values[row][slot]=read_value();for(size_t row=0;row<rows;++row){uint64_t total=0;for(uint64_t n:lengths[row])total+=n;std::vector<uint8_t>result;result.reserve(size_t(total));for(size_t k=0;k<rule.occurrence_slot.size();++k){result.insert(result.end(),rule.literals[k].begin(),rule.literals[k].end());const auto&v=values[row][rule.occurrence_slot[k]];result.insert(result.end(),v.begin(),v.end());}result.insert(result.end(),rule.literals.back().begin(),rule.literals.back().end());if(result.size()!=total)die_msg("multi output mismatch");size_t off=0;for(size_t j=0;j<ids[row].size();++j){store.install(ids[row][j],std::vector<uint8_t>(result.begin()+off,result.begin()+off+lengths[row][j]));off+=lengths[row][j];}}}
    if(p!=end||q!=data_end)die_msg("trailing multi bytes");
}

struct PersistentEncoder {
    std::unordered_map<std::string,uint32_t> rule_id;
    std::vector<ParsedLine> rules;
    std::unordered_map<std::string,uint32_t> token_id;
    std::vector<std::vector<uint8_t>> tokens;
};

struct PersistentDecoder {
    DecoderStore lines;
    std::vector<DecodedTemplate> rules;
    std::vector<std::vector<uint8_t>> tokens;
};

struct PersistentStats {
    uint64_t new_rules=0,rule_refs=0,new_tokens=0,token_refs=0,inline_values=0,raw_records=0;
    uint64_t pretrained_superblocks=0,pretrained_rule_refs=0,pretrained_token_refs=0;
};

static SplitBuffer encode_persistent_templates(const std::vector<uint32_t>&ids,const LineStore&store,
                                               PersistentEncoder&state,PersistentStats&stats,
                                               uint32_t pretrained_rules=0,uint32_t pretrained_tokens=0,
                                               bool parameterize_keywords=false){
    std::vector<ParsedLine>parsed;parsed.reserve(ids.size());std::vector<TemplateGroup>groups;std::unordered_map<std::string,uint32_t>by_key;by_key.reserve(ids.size()*2+1);
    for(size_t i=0;i<ids.size();++i){parsed.push_back(parse_line(store.data(ids[i]),store.len(ids[i]),parameterize_keywords));auto[it,inserted]=by_key.emplace(parsed.back().key,uint32_t(groups.size()));
        if(inserted){TemplateGroup g;g.shape=parsed.back();groups.push_back(std::move(g));}groups[it->second].members.push_back(i);}
    const uint32_t old_rule_count=uint32_t(state.rules.size());std::vector<uint32_t>new_group;
    for(uint32_t gi=0;gi<groups.size();++gi){TemplateGroup&g=groups[gi];auto known=state.rule_id.find(g.shape.key);
        size_t literals=0,refs=0;for(size_t index:g.members){uint32_t id=ids[index];literals+=encoded_literal_size(id,store.len(id));refs+=encoded_instance_size(id,known==state.rule_id.end()?old_rule_count+new_group.size():known->second,parsed[index],store.len(id));}
        if(known!=state.rule_id.end()){if(refs<literals)g.wire_id=known->second;}
        else if(g.members.size()>=2&&!g.shape.values.empty()&&encoded_template_size(g.shape)+refs<literals){g.wire_id=old_rule_count+uint32_t(new_group.size());new_group.push_back(gi);}}

    std::unordered_map<std::string,uint32_t>new_frequency;
    for(const TemplateGroup&g:groups)if(g.wire_id!=UINT32_MAX)for(size_t index:g.members)for(const auto&value:parsed[index].values){std::string key(reinterpret_cast<const char*>(value.data()),value.size());if(state.token_id.find(key)==state.token_id.end())++new_frequency[key];}
    std::vector<std::pair<std::string,uint32_t>>new_tokens;for(const auto&item:new_frequency){size_t len=item.first.size();uint64_t inline_cost=uint64_t(item.second)*(varint_size((uint64_t(len)<<1)|1)+len);uint64_t def_cost=varint_size(len)+len+uint64_t(item.second)*2;if(item.second>=2&&def_cost<inline_cost)new_tokens.push_back(item);}
    std::sort(new_tokens.begin(),new_tokens.end(),[](const auto&a,const auto&b){if(a.second!=b.second)return a.second>b.second;return a.first<b.first;});
    std::unordered_map<std::string,uint32_t>new_token_id;new_token_id.reserve(new_tokens.size()*2+1);for(uint32_t i=0;i<new_tokens.size();++i)new_token_id.emplace(new_tokens[i].first,uint32_t(state.tokens.size())+i);

    SplitBuffer out;put_varint(out.control,new_group.size());
    for(uint32_t gi:new_group){const TemplateGroup&g=groups[gi];put_varint(out.control,g.wire_id);const ParsedLine&s=g.shape;put_varint(out.control,s.values.size());out.control.insert(out.control.end(),s.slot_type.begin(),s.slot_type.end());put_varint(out.control,s.occurrence_slot.size());
        for(size_t i=0;i<s.occurrence_slot.size();++i){put_varint(out.control,s.literals[i].size());out.data.insert(out.data.end(),s.literals[i].begin(),s.literals[i].end());put_varint(out.control,s.occurrence_slot[i]);}
        put_varint(out.control,s.literals.back().size());out.data.insert(out.data.end(),s.literals.back().begin(),s.literals.back().end());++stats.new_rules;}
    put_varint(out.control,new_tokens.size());for(uint32_t i=0;i<new_tokens.size();++i){put_varint(out.control,uint32_t(state.tokens.size())+i);put_varint(out.control,new_tokens[i].first.size());out.data.insert(out.data.end(),new_tokens[i].first.begin(),new_tokens[i].first.end());++stats.new_tokens;}
    uint64_t record_blocks=0;for(const TemplateGroup&g:groups)record_blocks+=g.wire_id==UINT32_MAX?g.members.size():1;put_varint(out.control,record_blocks);int64_t previous_line_id=0;
    for(const TemplateGroup&g:groups){if(g.wire_id!=UINT32_MAX){std::vector<size_t>order=g.members;std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return ids[a]<ids[b];});out.control.push_back(2);put_varint(out.control,g.wire_id);put_varint(out.control,order.size());
            if(g.wire_id<pretrained_rules){++stats.pretrained_superblocks;stats.pretrained_rule_refs+=order.size();}
            for(size_t index:order){uint32_t id=ids[index];put_zigzag(out.control,int64_t(id)-previous_line_id);previous_line_id=id;put_varint(out.control,store.len(id));}
            for(size_t slot=0;slot<g.shape.values.size();++slot)for(size_t index:order){const auto&value=parsed[index].values[slot];std::string key(reinterpret_cast<const char*>(value.data()),value.size());auto old=state.token_id.find(key);auto fresh=new_token_id.find(key);
                    if(old!=state.token_id.end()){put_varint(out.control,uint64_t(old->second)<<1);++stats.token_refs;if(old->second<pretrained_tokens)++stats.pretrained_token_refs;}
                    else if(fresh!=new_token_id.end()){put_varint(out.control,uint64_t(fresh->second)<<1);++stats.token_refs;}
                    else{put_varint(out.control,(uint64_t(value.size())<<1)|1);out.data.insert(out.data.end(),value.begin(),value.end());++stats.inline_values;}}
            stats.rule_refs+=order.size();}
        else for(size_t index:g.members){uint32_t id=ids[index];out.control.push_back(0);put_zigzag(out.control,int64_t(id)-previous_line_id);previous_line_id=id;put_varint(out.control,store.len(id));const uint8_t*p=store.data(id);out.data.insert(out.data.end(),p,p+store.len(id));++stats.raw_records;}}

    for(uint32_t gi:new_group){TemplateGroup&g=groups[gi];state.rule_id.emplace(g.shape.key,g.wire_id);state.rules.push_back(std::move(g.shape));}
    for(const auto&item:new_tokens){uint32_t id=uint32_t(state.tokens.size());state.token_id.emplace(item.first,id);state.tokens.emplace_back(item.first.begin(),item.first.end());}
    return out;
}

static void decode_persistent_templates(const std::vector<uint8_t>&control,const std::vector<uint8_t>&data,PersistentDecoder&state){
    const uint8_t*p=control.data(),*end=p+control.size(),*q=data.data(),*data_end=q+data.size();uint64_t new_rules=get_varint(p,end);
    for(uint64_t r=0;r<new_rules;++r){uint32_t id=uint32_t(get_varint(p,end));if(id!=state.rules.size())die_msg("nonsequential persistent rule id");DecodedTemplate rule;uint64_t slots=get_varint(p,end);if(slots>uint64_t(end-p))die_msg("bad persistent slot count");rule.slot_type.assign(p,p+slots);p+=slots;uint64_t occ=get_varint(p,end);
        for(uint64_t i=0;i<occ;++i){uint64_t len=get_varint(p,end);if(len>uint64_t(data_end-q))die_msg("persistent literal exceeds stream");rule.literals.emplace_back(q,q+len);q+=len;uint64_t slot=get_varint(p,end);if(slot>=slots)die_msg("persistent slot out of range");rule.occurrence_slot.push_back(uint32_t(slot));}
        uint64_t len=get_varint(p,end);if(len>uint64_t(data_end-q))die_msg("persistent tail exceeds stream");rule.literals.emplace_back(q,q+len);q+=len;state.rules.push_back(std::move(rule));}
    uint64_t new_tokens=get_varint(p,end);for(uint64_t i=0;i<new_tokens;++i){uint32_t id=uint32_t(get_varint(p,end));if(id!=state.tokens.size())die_msg("nonsequential persistent token id");uint64_t len=get_varint(p,end);if(len>uint64_t(data_end-q))die_msg("persistent token exceeds stream");state.tokens.emplace_back(q,q+len);q+=len;}
    uint64_t records=get_varint(p,end);int64_t previous_line_id=0;for(uint64_t i=0;i<records;++i){if(p==end)die_msg("missing persistent mode");uint8_t mode=*p++;
        if(mode==0){previous_line_id+=get_zigzag(p,end);uint32_t id=uint32_t(previous_line_id);uint64_t output_len=get_varint(p,end);if(output_len>uint64_t(data_end-q))die_msg("persistent raw exceeds stream");state.lines.install(id,std::vector<uint8_t>(q,q+output_len));q+=output_len;continue;}
        if(mode==2){uint64_t rule_id=get_varint(p,end);if(rule_id>=state.rules.size())die_msg("persistent group rule missing");const DecodedTemplate&rule=state.rules[size_t(rule_id)];uint64_t count=get_varint(p,end);size_t rows=static_cast<size_t>(count);std::vector<uint32_t>ids(rows);std::vector<uint64_t>lengths(rows);for(size_t row=0;row<rows;++row){previous_line_id+=get_zigzag(p,end);ids[row]=uint32_t(previous_line_id);lengths[row]=get_varint(p,end);}std::vector<std::vector<std::vector<uint8_t>>>values(rows,std::vector<std::vector<uint8_t>>(rule.slot_type.size()));
            for(size_t slot=0;slot<rule.slot_type.size();++slot)for(size_t row=0;row<rows;++row){uint64_t code=get_varint(p,end);if(!(code&1)){uint64_t tid=code>>1;if(tid>=state.tokens.size())die_msg("persistent group token missing");values[row][slot]=state.tokens[size_t(tid)];}else{uint64_t len=code>>1;if(len>uint64_t(data_end-q))die_msg("persistent group inline exceeds stream");values[row][slot].assign(q,q+len);q+=len;}}
            for(size_t row=0;row<rows;++row){std::vector<uint8_t>result;result.reserve(size_t(lengths[row]));for(size_t k=0;k<rule.occurrence_slot.size();++k){result.insert(result.end(),rule.literals[k].begin(),rule.literals[k].end());const auto&v=values[row][rule.occurrence_slot[k]];result.insert(result.end(),v.begin(),v.end());}result.insert(result.end(),rule.literals.back().begin(),rule.literals.back().end());if(result.size()!=lengths[row])die_msg("persistent group output mismatch");state.lines.install(ids[row],std::move(result));}continue;}
        if(mode!=1)die_msg("unknown persistent mode");
        previous_line_id+=get_zigzag(p,end);uint32_t id=uint32_t(previous_line_id);uint64_t output_len=get_varint(p,end);uint64_t rule_id=get_varint(p,end);if(rule_id>=state.rules.size())die_msg("persistent rule missing");const DecodedTemplate&rule=state.rules[size_t(rule_id)];std::vector<std::vector<uint8_t>>values(rule.slot_type.size());
        for(auto&value:values){uint64_t code=get_varint(p,end);if(!(code&1)){uint64_t tid=code>>1;if(tid>=state.tokens.size())die_msg("persistent token missing");value=state.tokens[size_t(tid)];}else{uint64_t len=code>>1;if(len>uint64_t(data_end-q))die_msg("persistent inline exceeds stream");value.assign(q,q+len);q+=len;}}
        std::vector<uint8_t>result;result.reserve(size_t(output_len));for(size_t k=0;k<rule.occurrence_slot.size();++k){result.insert(result.end(),rule.literals[k].begin(),rule.literals[k].end());const auto&v=values[rule.occurrence_slot[k]];result.insert(result.end(),v.begin(),v.end());}result.insert(result.end(),rule.literals.back().begin(),rule.literals.back().end());if(result.size()!=output_len)die_msg("persistent output mismatch");state.lines.install(id,std::move(result));}
    if(p!=end||q!=data_end)die_msg("trailing persistent bytes");
}

struct PretrainedRuleCandidate {
    ParsedLine shape;
    uint64_t count = 0;
    uint64_t skeleton_bytes = 0;
    uint64_t corpus_mask = 0;
};

struct PretrainedTokenCandidate {uint64_t count=0,corpus_mask=0;};

struct PretrainedPackage {
    PersistentEncoder encoder;
    PersistentDecoder decoder;
    std::vector<uint8_t> raw;
    std::vector<uint8_t> frame;
    uint64_t training_raw = 0;
    uint64_t training_lines = 0;
    uint64_t training_distinct_lines = 0;
    uint64_t candidate_rules = 0;
    uint64_t candidate_tokens = 0;
    uint32_t fixed_rules = 0;
    uint32_t fixed_tokens = 0;
    double training_seconds = 0;

    uint32_t rule_count() const { return fixed_rules; }
    uint32_t token_count() const { return fixed_tokens; }
};

static void serialize_pretrained_rule(std::vector<uint8_t>&out,const ParsedLine&shape){
    put_varint(out,shape.slot_type.size());out.insert(out.end(),shape.slot_type.begin(),shape.slot_type.end());
    put_varint(out,shape.occurrence_slot.size());
    for(size_t i=0;i<shape.occurrence_slot.size();++i){put_varint(out,shape.literals[i].size());out.insert(out.end(),shape.literals[i].begin(),shape.literals[i].end());put_varint(out,shape.occurrence_slot[i]);}
    put_varint(out,shape.literals.back().size());out.insert(out.end(),shape.literals.back().begin(),shape.literals.back().end());
}

static void decode_pretrained_package(const std::vector<uint8_t>&raw,PersistentDecoder&decoder){
    const uint8_t*p=raw.data(),*end=p+raw.size();
    if(get_varint(p,end)!=1)die_msg("unknown pretrained package version");
    const uint64_t rules=get_varint(p,end);decoder.rules.reserve(size_t(rules));
    for(uint64_t r=0;r<rules;++r){DecodedTemplate rule;const uint64_t slots=get_varint(p,end);if(slots>uint64_t(end-p))die_msg("pretrained slot count exceeds package");rule.slot_type.assign(p,p+slots);p+=slots;
        const uint64_t occurrences=get_varint(p,end);rule.literals.reserve(size_t(occurrences)+1);rule.occurrence_slot.reserve(size_t(occurrences));
        for(uint64_t i=0;i<occurrences;++i){const uint64_t len=get_varint(p,end);if(len>uint64_t(end-p))die_msg("pretrained literal exceeds package");rule.literals.emplace_back(p,p+len);p+=len;const uint64_t slot=get_varint(p,end);if(slot>=slots)die_msg("pretrained slot out of range");rule.occurrence_slot.push_back(uint32_t(slot));}
        const uint64_t len=get_varint(p,end);if(len>uint64_t(end-p))die_msg("pretrained tail exceeds package");rule.literals.emplace_back(p,p+len);p+=len;decoder.rules.push_back(std::move(rule));}
    const uint64_t tokens=get_varint(p,end);decoder.tokens.reserve(size_t(tokens));
    for(uint64_t i=0;i<tokens;++i){const uint64_t len=get_varint(p,end);if(len>uint64_t(end-p))die_msg("pretrained token exceeds package");decoder.tokens.emplace_back(p,p+len);p+=len;}
    if(p!=end)die_msg("trailing pretrained package bytes");
}

// Offline model builder.  It observes only exact lines from explicitly separate manifests.  Counts
// are over distinct training definitions, matching the target definition plane rather than include
// multiplicity.  Selected alpha templates become parameterized columnar superblocks: every target
// TU can group all matching first-use definitions, carry stable IDs once, then transmit slot columns.
class PretrainedBuilder {
public:
    explicit PretrainedBuilder(bool parameterize_keywords):parameterize_keywords_(parameterize_keywords){}

    void scan_manifest(const std::string&manifest,size_t max_files,size_t corpus_index){
        if(corpus_index>=64)die_msg("at most 64 pretraining corpora are supported");
        LineStore corpus_distinct;
        FILE*mf=std::fopen(manifest.c_str(),"r");if(!mf)die(manifest.c_str());char path[16384];size_t files=0;
        while(files<max_files&&std::fgets(path,sizeof(path),mf)){size_t pn=std::strlen(path);while(pn&&(path[pn-1]=='\n'||path[pn-1]=='\r'))path[--pn]=0;if(!pn)continue;
            FILE*f=std::fopen(path,"rb");if(!f)die(path);struct stat st{};if(::fstat(::fileno(f),&st)!=0)die(path);if(st.st_size<0||uint64_t(st.st_size)>UINT32_MAX)die_msg("unsupported training TU size");
            std::vector<uint8_t>bytes(size_t(st.st_size)+8);if(st.st_size&&std::fread(bytes.data(),1,size_t(st.st_size),f)!=size_t(st.st_size))die_msg("short pretrained read");std::fclose(f);training_raw_+=uint64_t(st.st_size);++files;
            const uint8_t*p=bytes.data(),*end=p+st.st_size;while(p<end){const void*hit=std::memchr(p,'\n',size_t(end-p));const uint8_t*line_end=hit?static_cast<const uint8_t*>(hit)+1:end;const uint32_t len=uint32_t(line_end-p);++training_lines_;
                const LineStore::Result seen=corpus_distinct.intern(p,len);if(seen.first){++training_distinct_lines_;ParsedLine parsed=parse_line(p,len,parameterize_keywords_);if(!parsed.values.empty()&&parsed.key.size()<=65536&&parsed.occurrence_slot.size()<=512){for(const auto&value:parsed.values)if(value.size()>=2&&value.size()<=1024){PretrainedTokenCandidate&token=tokens_[std::string(reinterpret_cast<const char*>(value.data()),value.size())];++token.count;token.corpus_mask|=uint64_t(1)<<corpus_index;}
                            auto[it,inserted]=rules_.try_emplace(parsed.key);PretrainedRuleCandidate&candidate=it->second;if(inserted){candidate.shape=std::move(parsed);for(const auto&literal:candidate.shape.literals)candidate.skeleton_bytes+=literal.size();}++candidate.count;candidate.corpus_mask|=uint64_t(1)<<corpus_index;}}
                p=line_end;}}
        std::fclose(mf);
    }

    PretrainedPackage compile(size_t budget,int zlevel,double seconds,unsigned min_corpora) {
        if(budget<4096)die_msg("pretrained package budget must be at least 4 KiB");
        struct RankedRule{const std::string*key;PretrainedRuleCandidate*candidate;uint64_t cost,benefit;};std::vector<RankedRule>ranked_rules;ranked_rules.reserve(rules_.size());
        for(auto&item:rules_){PretrainedRuleCandidate&candidate=item.second;const unsigned corpora=unsigned(__builtin_popcountll(candidate.corpus_mask));const uint64_t cost=encoded_template_size(candidate.shape)+2;const uint64_t per_hit=candidate.skeleton_bytes>4?candidate.skeleton_bytes-4:0;const uint64_t stable_count=candidate.count*uint64_t(corpora)*uint64_t(corpora);const uint64_t gross=per_hit*stable_count;if(corpora>=min_corpora&&candidate.count>=2&&gross>cost)ranked_rules.push_back({&item.first,&candidate,cost,gross-cost});}
        std::sort(ranked_rules.begin(),ranked_rules.end(),[](const RankedRule&a,const RankedRule&b){const __uint128_t lhs=__uint128_t(a.benefit)*b.cost,rhs=__uint128_t(b.benefit)*a.cost;if(lhs!=rhs)return lhs>rhs;if(a.benefit!=b.benefit)return a.benefit>b.benefit;return *a.key<*b.key;});

        struct RankedToken{const std::string*value;uint64_t cost,benefit,count;};std::vector<RankedToken>ranked_tokens;ranked_tokens.reserve(tokens_.size());
        for(const auto&item:tokens_){const unsigned corpora=unsigned(__builtin_popcountll(item.second.corpus_mask));const uint64_t len=item.first.size(),count=item.second.count*uint64_t(corpora)*uint64_t(corpora),cost=varint_size(len)+len;const uint64_t inline_cost=count*(varint_size((len<<1)|1)+len);const uint64_t ref_cost=count*2;if(corpora>=min_corpora&&count>=3&&inline_cost>cost+ref_cost)ranked_tokens.push_back({&item.first,cost,inline_cost-cost-ref_cost,count});}
        std::sort(ranked_tokens.begin(),ranked_tokens.end(),[](const RankedToken&a,const RankedToken&b){const __uint128_t lhs=__uint128_t(a.benefit)*b.cost,rhs=__uint128_t(b.benefit)*a.cost;if(lhs!=rhs)return lhs>rhs;if(a.count!=b.count)return a.count>b.count;return *a.value<*b.value;});

        PretrainedPackage package;package.training_raw=training_raw_;package.training_lines=training_lines_;package.training_distinct_lines=training_distinct_lines_;package.candidate_rules=rules_.size();package.candidate_tokens=tokens_.size();package.training_seconds=seconds;
        const size_t usable=budget-128,rule_budget=usable*3/4;size_t rule_used=0;
        for(const RankedRule&ranked:ranked_rules){if(ranked.cost>rule_budget-rule_used)continue;const uint32_t id=uint32_t(package.encoder.rules.size());package.encoder.rule_id.emplace(*ranked.key,id);package.encoder.rules.push_back(ranked.candidate->shape);rule_used+=ranked.cost;}
        size_t used=rule_used;
        for(const RankedToken&ranked:ranked_tokens){if(ranked.cost>usable-used)continue;const uint32_t id=uint32_t(package.encoder.tokens.size());package.encoder.token_id.emplace(*ranked.value,id);package.encoder.tokens.emplace_back(ranked.value->begin(),ranked.value->end());used+=ranked.cost;}

        package.fixed_rules=uint32_t(package.encoder.rules.size());package.fixed_tokens=uint32_t(package.encoder.tokens.size());

        put_varint(package.raw,1);put_varint(package.raw,package.encoder.rules.size());for(const ParsedLine&rule:package.encoder.rules)serialize_pretrained_rule(package.raw,rule);
        put_varint(package.raw,package.encoder.tokens.size());for(const auto&token:package.encoder.tokens){put_varint(package.raw,token.size());package.raw.insert(package.raw.end(),token.begin(),token.end());}
        if(package.raw.size()>budget)die_msg("pretrained package exceeded explicit budget");
        FrameCodec codec;package.frame=codec.compress(package.raw,zlevel);const std::vector<uint8_t>decoded=codec.decompress(package.frame);if(decoded!=package.raw)die_msg("pretrained package frame mismatch");decode_pretrained_package(decoded,package.decoder);
        if(package.decoder.rules.size()!=package.encoder.rules.size()||package.decoder.tokens.size()!=package.encoder.tokens.size())die_msg("pretrained package cardinality mismatch");
        return package;
    }

private:
    std::unordered_map<std::string,PretrainedRuleCandidate>rules_;
    std::unordered_map<std::string,PretrainedTokenCandidate>tokens_;
    uint64_t training_raw_=0,training_lines_=0,training_distinct_lines_=0;
    bool parameterize_keywords_=false;
};

static void verify_definitions(const std::vector<uint32_t>& ids, const LineStore& truth,
                               const DecoderStore& decoded) {
    for (uint32_t id : ids) {
        if (id >= decoded.lines.size()) die_msg("decoded line id missing");
        const auto& got = decoded.lines[id];
        if (got.size() != truth.len(id) || std::memcmp(got.data(), truth.data(id), got.size()) != 0)
            die_msg("definition byte mismatch");
    }
}

struct Atom {
    uint32_t off=0,len=0;
};

static std::vector<Atom> atomize(const uint8_t*p,uint32_t n){
    std::vector<Atom> out;uint32_t i=0;
    while(i<n){uint32_t begin=i;
        if(is_ident_start(p[i])){++i;while(i<n&&is_ident_continue(p[i]))++i;}
        else if(p[i]>='0'&&p[i]<='9'){++i;while(i<n&&(is_ident_continue(p[i])||p[i]=='.'||p[i]=='\''||p[i]=='+'||p[i]=='-'))++i;}
        else if(p[i]=='"'||p[i]=='\''){uint8_t q=p[i++];bool esc=false;while(i<n){uint8_t c=p[i++];if(!esc&&c==q)break;if(!esc&&c=='\\')esc=true;else esc=false;}}
        else if(p[i]==' '||p[i]=='\t'||p[i]=='\r'||p[i]=='\n'){++i;while(i<n&&(p[i]==' '||p[i]=='\t'||p[i]=='\r'||p[i]=='\n'))++i;}
        else ++i;
        out.push_back({begin,i-begin});
    }
    return out;
}

struct DeltaOp {uint8_t copy=0;uint32_t off=0,len=0;};
struct DeltaProgram {uint32_t base_id=0;std::vector<DeltaOp> ops;size_t encoded_size=SIZE_MAX;};

static bool atom_equal(const uint8_t*a,const Atom&aa,const uint8_t*b,const Atom&bb){
    return aa.len==bb.len&&std::memcmp(a+aa.off,b+bb.off,aa.len)==0;
}

static DeltaProgram token_delta(uint32_t base_id,const uint8_t*base,uint32_t base_len,const uint8_t*target,uint32_t target_len){
    DeltaProgram result;result.base_id=base_id;
    if(base_len>4096||target_len>4096)return result;
    const std::vector<Atom> ba=atomize(base,base_len),ta=atomize(target,target_len);
    if(ba.size()>192||ta.size()>192)return result;
    const size_t cols=ta.size()+1;std::vector<uint16_t> dp((ba.size()+1)*cols);
    for(size_t i=1;i<=ba.size();++i)for(size_t j=1;j<=ta.size();++j){
        uint16_t&cell=dp[i*cols+j];cell=std::max(dp[(i-1)*cols+j],dp[i*cols+j-1]);
        if(atom_equal(base,ba[i-1],target,ta[j-1])){uint32_t v=uint32_t(dp[(i-1)*cols+j-1])+ta[j-1].len;cell=uint16_t(std::min<uint32_t>(v,UINT16_MAX));}
    }
    std::vector<std::pair<uint32_t,uint32_t>> matches;size_t i=ba.size(),j=ta.size();
    while(i&&j){if(atom_equal(base,ba[i-1],target,ta[j-1])&&dp[i*cols+j]==uint16_t(std::min<uint32_t>(uint32_t(dp[(i-1)*cols+j-1])+ta[j-1].len,UINT16_MAX))){matches.push_back({uint32_t(i-1),uint32_t(j-1)});--i;--j;}
        else if(dp[(i-1)*cols+j]>=dp[i*cols+j-1])--i;else --j;}
    std::reverse(matches.begin(),matches.end());uint32_t target_cursor=0;size_t m=0;
    auto add=[&](uint32_t off,uint32_t len){if(!len)return;if(!result.ops.empty()&&!result.ops.back().copy&&result.ops.back().off+result.ops.back().len==off)result.ops.back().len+=len;else result.ops.push_back({0,off,len});};
    while(m<matches.size()){
        size_t e=m+1;while(e<matches.size()&&matches[e].first==matches[e-1].first+1&&matches[e].second==matches[e-1].second+1)++e;
        const Atom&bs=ba[matches[m].first];const Atom&ts=ta[matches[m].second];const Atom&be=ba[matches[e-1].first];const Atom&te=ta[matches[e-1].second];
        uint32_t copy_len=(te.off+te.len)-ts.off;
        if(copy_len>=4){add(target_cursor,ts.off-target_cursor);result.ops.push_back({1,bs.off,(be.off+be.len)-bs.off});target_cursor=ts.off+copy_len;}
        m=e;
    }
    add(target_cursor,target_len-target_cursor);
    size_t size=1+varint_size(base_id)+varint_size(target_len)+varint_size(result.ops.size());
    for(const DeltaOp&op:result.ops){size+=1+varint_size(op.len);if(op.copy)size+=varint_size(op.off);else size+=op.len;}
    result.encoded_size=size;return result;
}

struct ForestRecord {uint32_t id=0;bool delta=false;DeltaProgram program;};
struct ForestStats {uint64_t delta_records=0,copy_bytes=0,add_bytes=0,comparisons=0;};

static std::vector<uint8_t> encode_forest(const std::vector<uint32_t>&ids,const LineStore&store,ForestStats&stats){
    std::vector<ParsedLine> parsed;parsed.reserve(ids.size());std::unordered_map<std::string,std::vector<size_t>> groups;groups.reserve(ids.size()*2+1);
    for(size_t i=0;i<ids.size();++i){parsed.push_back(parse_line(store.data(ids[i]),store.len(ids[i])));groups[parsed.back().coarse_key].push_back(i);}
    std::vector<ForestRecord> records;records.reserve(ids.size());
    for(auto&entry:groups){auto&members=entry.second;std::sort(members.begin(),members.end(),[&](size_t a,size_t b){uint32_t ai=ids[a],bi=ids[b],al=store.len(ai),bl=store.len(bi);int c=std::memcmp(store.data(ai),store.data(bi),std::min(al,bl));return c!=0?c<0:al<bl;});
        for(size_t pos=0;pos<members.size();++pos){size_t ti=members[pos];uint32_t id=ids[ti];size_t best=encoded_literal_size(id,store.len(id));DeltaProgram bp;
            size_t begin=pos>4?pos-4:0;for(size_t q=begin;q<pos;++q){uint32_t base_id=ids[members[q]];++stats.comparisons;DeltaProgram p=token_delta(base_id,store.data(base_id),store.len(base_id),store.data(id),store.len(id));
                size_t full=1+varint_size(id)+p.encoded_size;if(full<best){best=full;bp=std::move(p);}}
            ForestRecord rec;rec.id=id;if(bp.encoded_size!=SIZE_MAX){rec.delta=true;rec.program=std::move(bp);}records.push_back(std::move(rec));}}
    std::vector<uint8_t> out;put_varint(out,records.size());
    for(const ForestRecord&rec:records){out.push_back(rec.delta?1:0);put_varint(out,rec.id);uint32_t len=store.len(rec.id);put_varint(out,len);
        if(!rec.delta){const uint8_t*p=store.data(rec.id);out.insert(out.end(),p,p+len);continue;}
        ++stats.delta_records;put_varint(out,rec.program.base_id);put_varint(out,rec.program.ops.size());
        for(const DeltaOp&op:rec.program.ops){out.push_back(op.copy?1:0);put_varint(out,op.len);if(op.copy){put_varint(out,op.off);stats.copy_bytes+=op.len;}else{const uint8_t*p=store.data(rec.id)+op.off;out.insert(out.end(),p,p+op.len);stats.add_bytes+=op.len;}}}
    return out;
}

static void decode_forest(const std::vector<uint8_t>&raw,DecoderStore&store){
    const uint8_t*p=raw.data(),*end=p+raw.size();uint64_t count=get_varint(p,end);
    for(uint64_t i=0;i<count;++i){if(p==end)die_msg("missing forest mode");uint8_t mode=*p++;uint32_t id=uint32_t(get_varint(p,end));uint64_t output_len=get_varint(p,end);
        if(mode==0){if(output_len>uint64_t(end-p))die_msg("forest literal exceeds frame");store.install(id,std::vector<uint8_t>(p,p+output_len));p+=output_len;continue;}
        if(mode!=1)die_msg("unknown forest mode");
        uint32_t base_id=uint32_t(get_varint(p,end));if(base_id>=store.lines.size()||store.lines[base_id].empty())die_msg("forest base unavailable");
        const auto&base=store.lines[base_id];uint64_t ops=get_varint(p,end);std::vector<uint8_t> result;result.reserve(size_t(output_len));
        for(uint64_t k=0;k<ops;++k){if(p==end)die_msg("missing forest op");uint8_t op=*p++;uint64_t len=get_varint(p,end);
            if(op==0){if(len>uint64_t(end-p))die_msg("forest add exceeds frame");result.insert(result.end(),p,p+len);p+=len;}
            else if(op==1){uint64_t off=get_varint(p,end);if(off>base.size()||len>base.size()-off)die_msg("forest copy range");result.insert(result.end(),base.begin()+off,base.begin()+off+len);}
            else die_msg("unknown forest op");}
        if(result.size()!=output_len)die_msg("forest output length mismatch");
        store.install(id,std::move(result));}
    if(p!=end)die_msg("trailing forest bytes");
}

// P7s: source-location variants plus same-TU siblings.  Candidate selection is encoder-only;
// F receives an explicit already-installed base ID and an exact correction program.  The learner
// is observed only after the current TU has been encoded and checked.
enum SourceCategory : uint8_t { kMarkerCategory=0,kToolchainCategory=1,kProjectCategory=2,kSourceCategoryCount=3 };

struct SourceLocation {
    uint64_t path_hash=0;
    uint64_t basename_hash=0;
    uint32_t logical_line=0;
    bool valid() const { return path_hash!=0&&logical_line!=0; }
};

struct LineSourceMeta {
    SourceLocation location;
    uint8_t category=kProjectCategory;
    bool marker=false;
};

struct ParsedMarker {
    std::string path;
    uint32_t logical_line=0;
    uint32_t flags=0;
};

static bool parse_source_marker(const uint8_t*p,uint32_t n,ParsedMarker&out){
    if(n<5||p[0]!='#'||p[1]!=' ')return false;
    const uint8_t*cur=p+2,*end=p+n;
    if(cur==end||*cur<'0'||*cur>'9')return false;
    uint64_t line=0;
    while(cur<end&&*cur>='0'&&*cur<='9'){line=line*10+uint64_t(*cur-'0');if(line>UINT32_MAX)return false;++cur;}
    if(cur+2>end||cur[0]!=' '||cur[1]!='"')return false;
    cur+=2;const uint8_t*begin=cur;while(cur<end&&*cur!='"')++cur;if(cur==end)return false;
    out.path.assign(reinterpret_cast<const char*>(begin),size_t(cur-begin));++cur;out.logical_line=uint32_t(line);out.flags=0;
    while(cur<end&&*cur==' '){++cur;if(cur==end||*cur<'0'||*cur>'9')return false;uint32_t flag=0;
        while(cur<end&&*cur>='0'&&*cur<='9'){flag=flag*10+uint32_t(*cur-'0');++cur;}
        if(flag>=1&&flag<=4)out.flags|=uint32_t(1)<<(flag-1);}
    if(cur<end&&*cur=='\n')++cur;
    return cur==end;
}

static SourceLocation make_source_location(const std::string&path,uint32_t line){
    SourceLocation result;result.logical_line=line;
    if(path.empty())return result;
    result.path_hash=hash_bytes(reinterpret_cast<const uint8_t*>(path.data()),uint32_t(path.size()));
    const size_t slash=path.find_last_of("/\\");const std::string_view base(path.data()+(slash==std::string::npos?0:slash+1),path.size()-(slash==std::string::npos?0:slash+1));
    result.basename_hash=hash_bytes(reinterpret_cast<const uint8_t*>(base.data()),uint32_t(base.size()));
    return result;
}

static uint8_t classify_source_path(const std::string&path){
    return path.rfind("/usr",0)==0||path.rfind("/lib",0)==0?kToolchainCategory:kProjectCategory;
}

struct LocationKey {
    uint64_t path=0;
    uint32_t line=0;
    bool operator==(const LocationKey&o)const{return path==o.path&&line==o.line;}
};

struct LocationKeyHash {
    size_t operator()(const LocationKey&key)const{return size_t(mix64(key.path^(uint64_t(key.line)*0x9e3779b97f4a7c15ULL)));}
};

static bool line_less(uint32_t a,uint32_t b,const LineStore&store){
    const uint32_t al=store.len(a),bl=store.len(b);const int cmp=std::memcmp(store.data(a),store.data(b),std::min(al,bl));
    return cmp!=0?cmp<0:al<bl;
}

struct VariantHistory {
    struct Item {uint32_t id=0,count=0;};
    std::vector<Item> items;
    void observe(uint32_t id){for(Item&item:items)if(item.id==id){++item.count;return;}items.push_back({id,1});}
    std::vector<uint32_t> top(const LineStore&store,size_t limit)const{
        std::vector<Item> ranked=items;std::sort(ranked.begin(),ranked.end(),[&](const Item&a,const Item&b){
            if(a.count!=b.count)return a.count>b.count;
            return line_less(a.id,b.id,store);});
        if(ranked.size()>limit)ranked.resize(limit);
        std::vector<uint32_t>ids;ids.reserve(ranked.size());for(const Item&item:ranked)ids.push_back(item.id);return ids;
    }
};

static void common_prefix_suffix(const uint8_t*a,uint32_t an,const uint8_t*b,uint32_t bn,uint32_t&prefix,uint32_t&suffix){
    const uint32_t common=std::min(an,bn);prefix=0;while(prefix<common&&a[prefix]==b[prefix])++prefix;
    suffix=0;while(suffix<common-prefix&&a[an-1-suffix]==b[bn-1-suffix])++suffix;
}

static uint64_t medoid_distance(uint32_t a,uint32_t b,const LineStore&store){
    uint32_t prefix=0,suffix=0;common_prefix_suffix(store.data(a),store.len(a),store.data(b),store.len(b),prefix,suffix);
    return uint64_t(store.len(a)-prefix-suffix)+uint64_t(store.len(b)-prefix-suffix);
}

static uint32_t history_medoid(const VariantHistory&history,const LineStore&store){
    const std::vector<uint32_t>ids=history.top(store,8);if(ids.empty())return 0;
    uint32_t best=ids.front();uint64_t best_cost=UINT64_MAX;
    for(uint32_t candidate:ids){uint64_t cost=0;for(uint32_t other:ids)cost+=medoid_distance(candidate,other,store);
        if(cost<best_cost||(cost==best_cost&&line_less(candidate,best,store))){best=candidate;best_cost=cost;}}
    return best;
}

enum CandidateSource : uint32_t {
    kExactLocationCandidate=1u<<0,
    kMedoidCandidate=1u<<1,
    kBasenameCandidate=1u<<2,
    kSkeletonCandidate=1u<<3,
    kSameTuCandidate=1u<<4
};

struct SourceCandidate {uint32_t id=0,sources=0;};

struct SourceObservation {
    SourceLocation location;
    uint32_t id=0;
    bool operator==(const SourceObservation&o)const{return location.path_hash==o.location.path_hash&&location.logical_line==o.location.logical_line&&id==o.id;}
};

struct SourceObservationHash {
    size_t operator()(const SourceObservation&o)const{return size_t(mix64(o.location.path_hash^(uint64_t(o.location.logical_line)<<32)^o.id));}
};

class SourceLearner {
public:
    void candidates(const std::vector<SourceLocation>&locations,const ParsedLine&parsed,const LineStore&store,std::vector<SourceCandidate>&out)const{
        auto add=[&](uint32_t id,uint32_t source){if(!id)return;for(SourceCandidate&candidate:out)if(candidate.id==id){candidate.sources|=source;return;}out.push_back({id,source});};
        for(const SourceLocation&location:locations){if(!location.valid())continue;
            auto exact=by_location_.find({location.path_hash,location.logical_line});if(exact!=by_location_.end()){
                for(uint32_t id:exact->second.top(store,4))add(id,kExactLocationCandidate);
                add(history_medoid(exact->second,store),kMedoidCandidate);}
            auto backed=by_basename_.find({location.basename_hash,location.logical_line});if(backed!=by_basename_.end())for(uint32_t id:backed->second.top(store,2))add(id,kBasenameCandidate);
        }
        auto skeleton=by_skeleton_.find(parsed.coarse_key);if(skeleton!=by_skeleton_.end())for(uint32_t id:skeleton->second.top(store,4))add(id,kSkeletonCandidate);
    }
    bool has_prior(const std::vector<SourceLocation>&locations)const{
        for(const SourceLocation&location:locations)if(location.valid()&&by_location_.find({location.path_hash,location.logical_line})!=by_location_.end())return true;
        return false;
    }
    void observe_tu(const std::vector<SourceObservation>&observations,const std::vector<uint32_t>&new_ids,const LineStore&store){
        for(const SourceObservation&observation:observations){if(!observation.location.valid())continue;
            by_location_[{observation.location.path_hash,observation.location.logical_line}].observe(observation.id);
            by_basename_[{observation.location.basename_hash,observation.location.logical_line}].observe(observation.id);}
        for(uint32_t id:new_ids){ParsedLine parsed=parse_line(store.data(id),store.len(id));by_skeleton_[parsed.coarse_key].observe(id);}
    }
    size_t locations()const{return by_location_.size();}
    std::array<uint64_t,6> variant_histogram()const{
        std::array<uint64_t,6>hist{};for(const auto&entry:by_location_){const size_t n=entry.second.items.size();++hist[std::min<size_t>(n,5)];}return hist;
    }
private:
    std::unordered_map<LocationKey,VariantHistory,LocationKeyHash>by_location_;
    std::unordered_map<LocationKey,VariantHistory,LocationKeyHash>by_basename_;
    std::unordered_map<std::string,VariantHistory>by_skeleton_;
};

struct SourceStats {
    uint64_t records=0,literal_records=0,prefix_records=0,program_records=0;
    uint64_t candidates=0,program_comparisons=0,copy_bytes=0,add_bytes=0,raw_fallback_bytes=0;
    uint64_t first_variant_bytes=0,later_variant_bytes=0,union_raw_saving=0;
    std::array<uint64_t,5>exclusive_raw_saving{};
    std::array<uint64_t,kSourceCategoryCount>raw_bytes{},selected_wire{},literal_wire{},mixed_wire{};
};

struct SourceRecord {
    uint8_t mode=0;
    uint32_t id=0,base_id=0,prefix=0,suffix=0;
    DeltaProgram program;
    size_t cost=SIZE_MAX;
    uint32_t sources=0;
};

static size_t literal_source_cost(uint32_t id,uint32_t len){return 1+varint_size(id)+varint_size(len)+len;}

static size_t prefix_source_cost(uint32_t id,uint32_t len,uint32_t base_id,uint32_t prefix,uint32_t suffix){
    return 1+varint_size(id)+varint_size(len)+varint_size(base_id)+varint_size(prefix)+varint_size(suffix)+(len-prefix-suffix);
}

static size_t program_source_cost(uint32_t id,uint32_t len,const DeltaProgram&program){
    size_t cost=1+varint_size(id)+varint_size(len)+varint_size(program.base_id)+varint_size(program.ops.size());
    for(const DeltaOp&op:program.ops){cost+=1+varint_size(op.len);if(op.copy)cost+=varint_size(op.off);else cost+=op.len;}return cost;
}

static SourceRecord choose_source_record(uint32_t id,const ParsedLine&parsed,const std::vector<SourceLocation>&locations,
                                         const std::vector<uint32_t>&same_tu,const LineStore&store,const SourceLearner&learner,
                                         unsigned program_k,bool force_literal,SourceStats*stats){
    const uint8_t*target=store.data(id);const uint32_t target_len=store.len(id);SourceRecord best;best.id=id;best.cost=literal_source_cost(id,target_len);
    if(force_literal)return best;
    std::vector<SourceCandidate>candidates;learner.candidates(locations,parsed,store,candidates);
    auto add=[&](uint32_t base){if(base==id)return;for(SourceCandidate&candidate:candidates)if(candidate.id==base){candidate.sources|=kSameTuCandidate;return;}candidates.push_back({base,kSameTuCandidate});};
    for(uint32_t base:same_tu)add(base);
    if(stats)stats->candidates+=candidates.size();
    struct Ranked {SourceCandidate candidate;uint32_t prefix=0,suffix=0;size_t cost=SIZE_MAX;};
    std::vector<Ranked>ranked;ranked.reserve(candidates.size());std::array<size_t,5>source_best;source_best.fill(best.cost);
    static constexpr uint32_t source_bits[5]={kExactLocationCandidate,kMedoidCandidate,kBasenameCandidate,kSkeletonCandidate,kSameTuCandidate};
    for(const SourceCandidate&candidate:candidates){uint32_t prefix=0,suffix=0;common_prefix_suffix(store.data(candidate.id),store.len(candidate.id),target,target_len,prefix,suffix);
        const size_t cost=prefix_source_cost(id,target_len,candidate.id,prefix,suffix);ranked.push_back({candidate,prefix,suffix,cost});
        for(size_t s=0;s<5;++s)if(candidate.sources&source_bits[s])source_best[s]=std::min(source_best[s],cost);
        if(cost<best.cost){best.mode=1;best.base_id=candidate.id;best.prefix=prefix;best.suffix=suffix;best.cost=cost;best.sources=candidate.sources;}}
    std::sort(ranked.begin(),ranked.end(),[&](const Ranked&a,const Ranked&b){if(a.cost!=b.cost)return a.cost<b.cost;return line_less(a.candidate.id,b.candidate.id,store);});
    const size_t limit=std::min<size_t>(program_k?program_k:ranked.size(),ranked.size());
    for(size_t i=0;i<limit;++i){if(stats)++stats->program_comparisons;DeltaProgram program=token_delta(ranked[i].candidate.id,store.data(ranked[i].candidate.id),store.len(ranked[i].candidate.id),target,target_len);
        if(program.encoded_size==SIZE_MAX)continue;
        const size_t cost=program_source_cost(id,target_len,program);
        for(size_t s=0;s<5;++s)if(ranked[i].candidate.sources&source_bits[s])source_best[s]=std::min(source_best[s],cost);
        if(cost<best.cost){best.mode=2;best.base_id=ranked[i].candidate.id;best.program=std::move(program);best.cost=cost;best.sources=ranked[i].candidate.sources;}}
    if(stats){const size_t literal=literal_source_cost(id,target_len);stats->union_raw_saving+=literal-best.cost;
        for(size_t s=0;s<5;++s)stats->exclusive_raw_saving[s]+=literal-source_best[s];}
    return best;
}

struct SourceCategoryRaw {std::vector<uint8_t>control,data;uint64_t records=0;};
struct SourceRaw {std::array<SourceCategoryRaw,kSourceCategoryCount>category;};

static SourceRaw encode_source_variants(const std::vector<uint32_t>&ids,const LineStore&store,const std::vector<LineSourceMeta>&meta,
                                        const std::unordered_map<uint32_t,std::vector<SourceLocation>>&locations,const SourceLearner&learner,
                                        unsigned program_k,bool force_literal,SourceStats*stats){
    SourceRaw raw;std::array<std::vector<uint32_t>,kSourceCategoryCount>category_ids;
    std::unordered_map<uint32_t,ParsedLine>parsed;parsed.reserve(ids.size()*2+1);
    for(uint32_t id:ids){ParsedLine value=parse_line(store.data(id),store.len(id));category_ids[meta[id].category].push_back(id);parsed.emplace(id,std::move(value));
        ++raw.category[meta[id].category].records;if(stats)stats->raw_bytes[meta[id].category]+=store.len(id);}
    auto primary_location=[&](uint32_t id){
        SourceLocation result;
        auto found=locations.find(id);
        if(found==locations.end())return result;
        for(const SourceLocation&location:found->second){
            if(location.valid()&&(!result.valid()||location.path_hash<result.path_hash||
                                 (location.path_hash==result.path_hash&&location.logical_line<result.logical_line))){
                result=location;
            }
        }
        return result;
    };
    for(size_t category=0;category<kSourceCategoryCount;++category){SourceCategoryRaw&out=raw.category[category];put_varint(out.control,out.records);
        std::vector<uint32_t>&ordered=category_ids[category];
        std::sort(ordered.begin(),ordered.end(),[&](uint32_t a,uint32_t b){
            const SourceLocation al=primary_location(a),bl=primary_location(b);
            if(al.valid()!=bl.valid())return al.valid()>bl.valid();
            if(al.path_hash!=bl.path_hash)return al.path_hash<bl.path_hash;
            if(al.logical_line!=bl.logical_line)return al.logical_line<bl.logical_line;
            const std::string&ak=parsed.at(a).coarse_key,&bk=parsed.at(b).coarse_key;
            if(ak!=bk)return ak<bk;
            return line_less(a,b,store);
        });
        std::unordered_map<LocationKey,std::vector<uint32_t>,LocationKeyHash>location_done;std::unordered_map<std::string,std::vector<uint32_t>>skeleton_done;
        for(uint32_t id:ordered){auto found=locations.find(id);static const std::vector<SourceLocation>empty;const std::vector<SourceLocation>&line_locations=found==locations.end()?empty:found->second;std::vector<uint32_t>same_tu;
                auto append_recent=[&](const std::vector<uint32_t>&done){const size_t begin=done.size()>4?done.size()-4:0;for(size_t i=begin;i<done.size();++i)if(std::find(same_tu.begin(),same_tu.end(),done[i])==same_tu.end())same_tu.push_back(done[i]);};
                for(const SourceLocation&location:line_locations)if(location.valid()){auto old=location_done.find({location.path_hash,location.logical_line});if(old!=location_done.end())append_recent(old->second);}
                auto same_shape=skeleton_done.find(parsed.at(id).coarse_key);if(same_shape!=skeleton_done.end())append_recent(same_shape->second);
                SourceRecord record=choose_source_record(id,parsed.at(id),line_locations,same_tu,store,learner,program_k,force_literal,stats);const uint32_t len=store.len(id);out.control.push_back(record.mode);put_varint(out.control,id);put_varint(out.control,len);
                if(stats){++stats->records;if(record.mode==0){++stats->literal_records;stats->raw_fallback_bytes+=len;}else if(record.mode==1)++stats->prefix_records;else ++stats->program_records;}
                if(record.mode==0){out.data.insert(out.data.end(),store.data(id),store.data(id)+len);}
                else if(record.mode==1){put_varint(out.control,record.base_id);put_varint(out.control,record.prefix);put_varint(out.control,record.suffix);out.data.insert(out.data.end(),store.data(id)+record.prefix,store.data(id)+len-record.suffix);}
                else {put_varint(out.control,record.base_id);put_varint(out.control,record.program.ops.size());for(const DeltaOp&op:record.program.ops){out.control.push_back(op.copy?1:0);put_varint(out.control,op.len);
                        if(op.copy){put_varint(out.control,op.off);if(stats)stats->copy_bytes+=op.len;}else{out.data.insert(out.data.end(),store.data(id)+op.off,store.data(id)+op.off+op.len);if(stats)stats->add_bytes+=op.len;}}}
                for(const SourceLocation&location:line_locations){
                    if(location.valid())location_done[{location.path_hash,location.logical_line}].push_back(id);
                }
                skeleton_done[parsed.at(id).coarse_key].push_back(id);
            }
    }
    return raw;
}

struct SourceCategoryFrames {bool present=false,use_mixed=false;std::vector<uint8_t>control,data;uint64_t selected_wire=0,literal_wire=0,mixed_wire=0;};
struct SourceFrames {std::array<SourceCategoryFrames,kSourceCategoryCount>category;uint64_t wire=0;};

static SourceFrames compress_source_variants(const SourceRaw&mixed,const SourceRaw&literal,FrameCodec&codec,int level){
    SourceFrames frames;
    for(size_t category=0;category<kSourceCategoryCount;++category){if(!mixed.category[category].records)continue;SourceCategoryFrames&out=frames.category[category];out.present=true;
        std::vector<uint8_t>mc=codec.compress(mixed.category[category].control,level),md=codec.compress(mixed.category[category].data,level),lc=codec.compress(literal.category[category].control,level),ld=codec.compress(literal.category[category].data,level);
        const uint64_t mw=mc.size()+md.size()+8,lw=lc.size()+ld.size()+8;out.mixed_wire=mw;out.literal_wire=lw;out.use_mixed=mw<lw;out.selected_wire=std::min(mw,lw)+1;
        if(out.use_mixed){out.control=std::move(mc);out.data=std::move(md);}else{out.control=std::move(lc);out.data=std::move(ld);}frames.wire+=out.selected_wire;}
    return frames;
}

static SourceRaw decompress_source_variants(const SourceFrames&frames,FrameCodec&codec){
    SourceRaw raw;for(size_t category=0;category<kSourceCategoryCount;++category){const SourceCategoryFrames&in=frames.category[category];if(!in.present)continue;
        raw.category[category].control=codec.decompress(in.control);raw.category[category].data=codec.decompress(in.data);}return raw;
}

static void decode_source_variants(const SourceRaw&raw,DecoderStore&store){
    for(size_t category=0;category<kSourceCategoryCount;++category){const SourceCategoryRaw&in=raw.category[category];if(in.control.empty())continue;const uint8_t*p=in.control.data(),*end=p+in.control.size(),*data=in.data.data(),*data_end=data+in.data.size();const uint64_t count=get_varint(p,end);
        for(uint64_t record=0;record<count;++record){if(p==end)die_msg("missing source mode");const uint8_t mode=*p++;const uint32_t id=uint32_t(get_varint(p,end));const uint64_t output_len=get_varint(p,end);std::vector<uint8_t>result;result.reserve(size_t(output_len));
            if(mode==0){if(output_len>uint64_t(data_end-data))die_msg("source literal exceeds data");result.insert(result.end(),data,data+output_len);data+=output_len;}
            else {const uint32_t base_id=uint32_t(get_varint(p,end));if(base_id>=store.lines.size()||store.lines[base_id].empty())die_msg("source base unavailable");const std::vector<uint8_t>&base=store.lines[base_id];
                if(mode==1){const uint64_t prefix=get_varint(p,end),suffix=get_varint(p,end);if(prefix>base.size()||suffix>base.size()-prefix||prefix+suffix>output_len)die_msg("source prefix/suffix range");const uint64_t middle=output_len-prefix-suffix;if(middle>uint64_t(data_end-data))die_msg("source middle exceeds data");result.insert(result.end(),base.begin(),base.begin()+prefix);result.insert(result.end(),data,data+middle);data+=middle;result.insert(result.end(),base.end()-suffix,base.end());}
                else if(mode==2){const uint64_t ops=get_varint(p,end);for(uint64_t op_index=0;op_index<ops;++op_index){if(p==end)die_msg("missing source op");const uint8_t op=*p++;const uint64_t len=get_varint(p,end);
                        if(op==0){if(len>uint64_t(data_end-data))die_msg("source add exceeds data");result.insert(result.end(),data,data+len);data+=len;}
                        else if(op==1){const uint64_t off=get_varint(p,end);if(off>base.size()||len>base.size()-off)die_msg("source copy range");result.insert(result.end(),base.begin()+off,base.begin()+off+len);}
                        else die_msg("unknown source op");}}
                else die_msg("unknown source mode");}
            if(result.size()!=output_len)die_msg("source output length mismatch");
            store.install(id,std::move(result));}
        if(p!=end||data!=data_end)die_msg("trailing source frame bytes");}
}

struct Row {
    const char* name = nullptr;
    uint64_t wire = 0;
    double encode_seconds = 0;
    double decode_seconds = 0;
    DecoderStore decoder;
    std::vector<uint64_t> tu_wire;
    std::vector<double> checkpoints;
    explicit Row(const char* row_name) : name(row_name) {}
};

static double trailing_ratio(const std::vector<FileSpan>& files, const std::vector<uint64_t>& wire,
                             uint64_t corpus_raw) {
    const uint64_t target = std::max<uint64_t>(uint64_t(double(corpus_raw) * 0.05), 1);
    uint64_t raw_sum = 0;
    uint64_t wire_sum = 0;
    size_t count = 0;
    for (size_t i = files.size(); i-- > 0;) {
        raw_sum += files[i].len;
        wire_sum += wire[i];
        ++count;
        if (raw_sum >= target && count >= std::min<size_t>(64, files.size())) break;
    }
    return wire_sum ? double(raw_sum) / double(wire_sum) : 0.0;
}

int main(int argc, char** argv) {
    const char* manifest = nullptr;
    size_t max_files = SIZE_MAX;
    size_t pretrain_max_files = SIZE_MAX;
    size_t model_kib = 0;
    unsigned model_min_corpora = 1;
    unsigned source_program_k = 4;
    bool pretrain_all_identifiers = false;
    std::vector<std::string> pretrain_manifests;
    std::string semantic_export_prefix;
    bool semantic_export_only = false;
    int zlevel = 3;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
        else if (!std::strcmp(argv[i], "--max-files") && i + 1 < argc) max_files = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--pretrain-manifest") && i + 1 < argc) pretrain_manifests.emplace_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--pretrain-max-files") && i + 1 < argc) pretrain_max_files = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--model-kib") && i + 1 < argc) model_kib = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--model-min-corpora") && i + 1 < argc) model_min_corpora = unsigned(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--source-k") && i + 1 < argc) source_program_k = unsigned(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--pretrain-all-identifiers")) pretrain_all_identifiers = true;
        else if (!std::strcmp(argv[i], "--semantic-export-prefix") && i + 1 < argc) semantic_export_prefix = argv[++i];
        else if (!std::strcmp(argv[i], "--semantic-export-only")) semantic_export_only = true;
        else if (!std::strcmp(argv[i], "--z") && i + 1 < argc) zlevel = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!manifest || zlevel < 0 || zlevel > 3 || source_program_k > 32 || (model_kib && pretrain_manifests.empty()) || (!model_kib && !pretrain_manifests.empty()) || (semantic_export_only&&semantic_export_prefix.empty())) {
        std::fprintf(stderr, "usage: %s --manifest FILE [--max-files N] [--z 0..3] [--source-k 0..32] [--semantic-export-prefix PATH [--semantic-export-only]] [--pretrain-manifest FILE ... --model-kib N] [--pretrain-max-files N] [--model-min-corpora N] [--pretrain-all-identifiers]\n", argv[0]);
        return 2;
    }
    for(const std::string&training:pretrain_manifests)if(training==manifest)die_msg("pretraining and target manifests must be disjoint");

    const bool use_pretrained=model_kib!=0;
    PretrainedPackage pretrained;
    if(use_pretrained){const auto training_begin=Clock::now();PretrainedBuilder builder(pretrain_all_identifiers);for(size_t i=0;i<pretrain_manifests.size();++i)builder.scan_manifest(pretrain_manifests[i],pretrain_max_files,i);const double training_seconds=elapsed(training_begin);pretrained=builder.compile(model_kib*1024,zlevel,training_seconds,model_min_corpora);}

    const auto begin = Clock::now();
    Corpus corpus = load_corpus(manifest, max_files);
    if(semantic_export_only){LineStore export_truth;DecoderStore export_decoder;FrameCodec export_frames;SemanticFrameWriter writer(semantic_export_prefix);uint64_t wire=0,definitions=0;
        for(const FileSpan&file:corpus.files){const uint8_t*p=corpus.bytes.data()+file.off,*end=p+file.len;std::vector<uint32_t>new_ids;while(p<end){const void*hit=std::memchr(p,'\n',size_t(end-p));const uint8_t*line_end=hit?static_cast<const uint8_t*>(hit)+1:end;const LineStore::Result result=export_truth.intern(p,uint32_t(line_end-p));if(result.first)new_ids.push_back(result.id);p=line_end;}
            TemplateStats a_stats,b_stats;const SemanticBuffer a=encode_semantic_templates(new_ids,export_truth,a_stats,false),b=encode_semantic_templates(new_ids,export_truth,b_stats,true);const SemanticFrames af=compress_semantic(a,export_frames,zlevel),bf=compress_semantic(b,export_frames,zlevel);const SemanticBuffer&selected=bf.wire<af.wire?b:a;const SemanticFrames&selected_frame=bf.wire<af.wire?bf:af;writer.add(file.len,selected);const SemanticBuffer decoded=decompress_semantic(selected_frame,export_frames);decode_semantic_templates(decoded,export_decoder);verify_definitions(new_ids,export_truth,export_decoder);wire+=selected_frame.wire;definitions+=new_ids.size();}
        std::printf("semantic export exact=PASS manifest=%s TUs=%zu raw=%llu definitions=%llu zstd-wire=%llu prefix=%s seconds=%.2f\n",manifest,corpus.files.size(),static_cast<unsigned long long>(corpus.raw),static_cast<unsigned long long>(definitions),static_cast<unsigned long long>(wire),semantic_export_prefix.c_str(),elapsed(begin));return 0;}
    LineStore truth;
    FrameCodec frames;
    ModelFrameCodec model_frames(pretrained.raw,zlevel);
    ModelFrameCodec model_p4_frames(pretrained.raw,zlevel);
    Row p0("P0-appearance"), p1("P1-lexicographic"), p4("P4-alpha-template"),p5("P5-multiline"),p6("P6-token-forest"),p7s("P7-source-location"),p7("P7-online-template"),p7b("P7b-local-rule-online-token"),p9("P9-semantic-split-zstd"),p8("P8-pretrained-superblock"),p8d("P8-pretrained-dict-P4"), p10("P10-best-local-frame"),p10p("P10+P8-charged-union");
    std::vector<Row*> rows{&p0, &p1, &p4,&p5,&p6,&p7s,&p7,&p7b,&p9};if(use_pretrained){rows.push_back(&p8);rows.push_back(&p8d);}rows.push_back(&p10);if(use_pretrained)rows.push_back(&p10p);
    if(use_pretrained){p8.wire=pretrained.frame.size()+4;p8d.wire=pretrained.frame.size()+4;p10p.wire=pretrained.frame.size()+4;}
    std::unique_ptr<SemanticFrameWriter>semantic_writer;if(!semantic_export_prefix.empty())semantic_writer=std::make_unique<SemanticFrameWriter>(semantic_export_prefix);
    for (Row* row : rows) row->tu_wire.reserve(corpus.files.size());
    TemplateStats template_stats;
    TemplateStats semantic_stats;
    ForestStats forest_stats;
    PersistentStats persistent_stats;PersistentEncoder persistent_encoder;PersistentDecoder persistent_decoder;
    PersistentStats hybrid_stats;PersistentEncoder hybrid_encoder;PersistentDecoder hybrid_decoder;
    PersistentStats pretrained_stats;
    MultiStats multi_stats;
    SourceLearner source_learner;SourceStats source_stats;std::vector<LineSourceMeta>source_meta(1);
    std::array<std::vector<uint8_t>,kSourceCategoryCount>attributed_bytes;
    uint64_t cumulative_raw = 0;
    const double fractions[] = {0.10, 0.25, 0.50, 0.75, 1.00};
    size_t checkpoint = 0;
    uint64_t total_occurrences = 0;
    uint64_t marker_definition_bytes = 0;
    uint64_t p4_control_wire=0,p4_data_wire=0,p7_control_wire=0,p7_data_wire=0,p9_control_wire=0;std::array<uint64_t,kSemanticChannels>p9_payload_wire{};

    for (size_t tu = 0; tu < corpus.files.size(); ++tu) {
        const FileSpan& file = corpus.files[tu];
        const uint8_t* p = corpus.bytes.data() + file.off;
        const uint8_t* end = p + file.len;
        std::vector<uint32_t> new_ids;
        std::unordered_set<uint32_t>current_new_ids;
        std::unordered_map<uint32_t,std::vector<SourceLocation>>current_locations;
        std::unordered_set<SourceObservation,SourceObservationHash>observation_seen;
        std::vector<SourceObservation>observations;
        std::string current_path;SourceLocation current_location;uint8_t current_category=kProjectCategory;
        while (p < end) {
            const void* hit = std::memchr(p, '\n', size_t(end - p));
            const uint8_t* line_end = hit ? static_cast<const uint8_t*>(hit) + 1 : end;
            const uint32_t len = uint32_t(line_end - p);
            ParsedMarker marker;const bool marker_line=parse_source_marker(p,len,marker);LineSourceMeta line_meta;
            if(marker_line){line_meta.marker=true;line_meta.category=kMarkerCategory;line_meta.location=make_source_location(marker.path,marker.logical_line);}
            else {line_meta.location=current_location;line_meta.category=current_category;}
            const LineStore::Result result = truth.intern(p, len);
            if (result.first) {
                new_ids.push_back(result.id);
                current_new_ids.insert(result.id);if(source_meta.size()<=result.id)source_meta.resize(size_t(result.id)+1);source_meta[result.id]=line_meta;
                attributed_bytes[line_meta.category].insert(attributed_bytes[line_meta.category].end(),p,line_end);
                if (marker_line) marker_definition_bytes += len;
            }
            if(!marker_line&&line_meta.location.valid()){
                SourceObservation observation{line_meta.location,result.id};if(observation_seen.insert(observation).second)observations.push_back(observation);
                if(current_new_ids.find(result.id)!=current_new_ids.end()){std::vector<SourceLocation>&locations=current_locations[result.id];bool duplicate=false;for(const SourceLocation&old:locations)if(old.path_hash==line_meta.location.path_hash&&old.logical_line==line_meta.location.logical_line){duplicate=true;break;}if(!duplicate&&locations.size()<8)locations.push_back(line_meta.location);}}
            ++total_occurrences;
            if(marker_line){current_path=marker.path;current_location=make_source_location(current_path,marker.logical_line);current_category=classify_source_path(current_path);}
            else if(current_location.logical_line!=UINT32_MAX)++current_location.logical_line;
            p = line_end;
        }
        for(uint32_t id:new_ids)if(!source_meta[id].marker){auto found=current_locations.find(id);static const std::vector<SourceLocation>empty;const std::vector<SourceLocation>&locations=found==current_locations.end()?empty:found->second;
            if(source_learner.has_prior(locations))source_stats.later_variant_bytes+=truth.len(id);else source_stats.first_variant_bytes+=truth.len(id);}

        std::vector<uint32_t> sorted_ids = new_ids;
        std::sort(sorted_ids.begin(), sorted_ids.end(), [&](uint32_t a, uint32_t b) {
            const uint32_t al = truth.len(a), bl = truth.len(b);
            const int cmp = std::memcmp(truth.data(a), truth.data(b), std::min(al, bl));
            return cmp != 0 ? cmp < 0 : al < bl;
        });

        const auto e0 = Clock::now();
        const std::vector<uint8_t> p0_raw = encode_literals(new_ids, truth);
        const std::vector<uint8_t> p0_frame = frames.compress(p0_raw, zlevel);
        p0.encode_seconds += elapsed(e0);
        const auto d0 = Clock::now();
        decode_literals(frames.decompress(p0_frame), p0.decoder);
        p0.decode_seconds += elapsed(d0);
        verify_definitions(new_ids, truth, p0.decoder);

        const auto e1 = Clock::now();
        const std::vector<uint8_t> p1_raw = encode_literals(sorted_ids, truth);
        const std::vector<uint8_t> p1_frame = frames.compress(p1_raw, zlevel);
        p1.encode_seconds += elapsed(e1);
        const auto d1 = Clock::now();
        decode_literals(frames.decompress(p1_frame), p1.decoder);
        p1.decode_seconds += elapsed(d1);
        verify_definitions(new_ids, truth, p1.decoder);

        const auto e4 = Clock::now();
        TemplateStats p4_stats_a,p4_stats_b;const SplitBuffer p4_raw_a=encode_templates(new_ids,truth,p4_stats_a,false),p4_raw_b=encode_templates(new_ids,truth,p4_stats_b,true);const SplitBuffer*p4_selected=&p4_raw_a;
        std::vector<uint8_t>p4_control_frame=frames.compress(p4_raw_a.control,zlevel),p4_data_frame=frames.compress(p4_raw_a.data,zlevel);std::vector<uint8_t>p4_control_b=frames.compress(p4_raw_b.control,zlevel),p4_data_b=frames.compress(p4_raw_b.data,zlevel);TemplateStats selected_p4=p4_stats_a;
        if(p4_control_b.size()+p4_data_b.size()<p4_control_frame.size()+p4_data_frame.size()){p4_control_frame=std::move(p4_control_b);p4_data_frame=std::move(p4_data_b);selected_p4=p4_stats_b;p4_selected=&p4_raw_b;}
        template_stats.rules+=selected_p4.rules;template_stats.instances+=selected_p4.instances;template_stats.unique_slots+=selected_p4.unique_slots;template_stats.slot_occurrences+=selected_p4.slot_occurrences;template_stats.literal_fallbacks+=selected_p4.literal_fallbacks;template_stats.template_input_bytes+=selected_p4.template_input_bytes;template_stats.lexicon_entries+=selected_p4.lexicon_entries;template_stats.lexicon_references+=selected_p4.lexicon_references;
        p4_control_wire+=p4_control_frame.size()+4;p4_data_wire+=p4_data_frame.size()+4;
        const size_t p4_size=p4_control_frame.size()+p4_data_frame.size()+8;
        p4.encode_seconds += elapsed(e4);
        const auto d4 = Clock::now();
        const std::vector<uint8_t>p4_control_decoded=frames.decompress(p4_control_frame);
        const std::vector<uint8_t>p4_data_decoded=frames.decompress(p4_data_frame);
        decode_templates(p4_control_decoded,p4_data_decoded, p4.decoder);
        p4.decode_seconds += elapsed(d4);
        verify_definitions(new_ids, truth, p4.decoder);

        uint64_t w8d=0;PackedFrame p8d_control_frame,p8d_data_frame;std::vector<uint8_t>p8d_control_decoded,p8d_data_decoded;
        if(use_pretrained){const auto e8d=Clock::now();p8d_control_frame=model_p4_frames.compress_best(p4_selected->control,zlevel);p8d_data_frame=model_p4_frames.compress_best(p4_selected->data,zlevel);w8d=p8d_control_frame.bytes.size()+p8d_data_frame.bytes.size()+10;p8d.encode_seconds+=elapsed(e8d);
            const auto d8d=Clock::now();p8d_control_decoded=model_p4_frames.decompress(p8d_control_frame);p8d_data_decoded=model_p4_frames.decompress(p8d_data_frame);decode_templates(p8d_control_decoded,p8d_data_decoded,p8d.decoder);p8d.decode_seconds+=elapsed(d8d);verify_definitions(new_ids,truth,p8d.decoder);}

        const auto e9=Clock::now();TemplateStats p9_stats_a,p9_stats_b;const SemanticBuffer p9_raw_a=encode_semantic_templates(new_ids,truth,p9_stats_a,false),p9_raw_b=encode_semantic_templates(new_ids,truth,p9_stats_b,true);const SemanticBuffer*p9_selected=&p9_raw_a;SemanticFrames p9_frames=compress_semantic(p9_raw_a,frames,zlevel),p9_frames_b=compress_semantic(p9_raw_b,frames,zlevel);TemplateStats selected_p9=p9_stats_a;
        if(p9_frames_b.wire<p9_frames.wire){p9_frames=std::move(p9_frames_b);selected_p9=p9_stats_b;p9_selected=&p9_raw_b;}if(semantic_writer)semantic_writer->add(file.len,*p9_selected);p9.encode_seconds+=elapsed(e9);semantic_stats.rules+=selected_p9.rules;semantic_stats.instances+=selected_p9.instances;semantic_stats.unique_slots+=selected_p9.unique_slots;semantic_stats.slot_occurrences+=selected_p9.slot_occurrences;semantic_stats.literal_fallbacks+=selected_p9.literal_fallbacks;semantic_stats.template_input_bytes+=selected_p9.template_input_bytes;semantic_stats.lexicon_entries+=selected_p9.lexicon_entries;semantic_stats.lexicon_references+=selected_p9.lexicon_references;p9_control_wire+=p9_frames.control.size()+4;for(size_t c=0;c<kSemanticChannels;++c)if(!p9_frames.payload[c].empty())p9_payload_wire[c]+=p9_frames.payload[c].size()+4;
        const auto d9=Clock::now();const SemanticBuffer p9_decoded=decompress_semantic(p9_frames,frames);decode_semantic_templates(p9_decoded,p9.decoder);p9.decode_seconds+=elapsed(d9);verify_definitions(new_ids,truth,p9.decoder);

        const auto e5=Clock::now();size_t p5_size=SIZE_MAX;std::vector<uint8_t>p5_control_frame,p5_data_frame;MultiStats selected_multi;
        for(size_t width:{size_t(2),size_t(4),size_t(8),size_t(16)}){MultiStats candidate_stats;SplitBuffer candidate=encode_multiline(new_ids,truth,width,candidate_stats);std::vector<uint8_t>cf=frames.compress(candidate.control,zlevel),df=frames.compress(candidate.data,zlevel);size_t size=cf.size()+df.size()+8;if(size<p5_size){p5_size=size;p5_control_frame=std::move(cf);p5_data_frame=std::move(df);selected_multi=candidate_stats;}}
        p5.encode_seconds+=elapsed(e5);multi_stats.rules+=selected_multi.rules;multi_stats.instances+=selected_multi.instances;multi_stats.raw_units+=selected_multi.raw_units;multi_stats.lines+=selected_multi.lines;
        const auto d5=Clock::now();const std::vector<uint8_t>p5_control_decoded=frames.decompress(p5_control_frame);const std::vector<uint8_t>p5_data_decoded=frames.decompress(p5_data_frame);decode_multiline(p5_control_decoded,p5_data_decoded,p5.decoder);p5.decode_seconds+=elapsed(d5);verify_definitions(new_ids,truth,p5.decoder);

        const auto e6=Clock::now();const std::vector<uint8_t>p6_raw=encode_forest(new_ids,truth,forest_stats);const std::vector<uint8_t>p6_frame=frames.compress(p6_raw,zlevel);p6.encode_seconds+=elapsed(e6);
        const auto d6=Clock::now();decode_forest(frames.decompress(p6_frame),p6.decoder);p6.decode_seconds+=elapsed(d6);verify_definitions(new_ids,truth,p6.decoder);

        const auto e7s=Clock::now();const SourceRaw p7s_mixed=encode_source_variants(new_ids,truth,source_meta,current_locations,source_learner,source_program_k,false,&source_stats);
        const SourceRaw p7s_literal=encode_source_variants(new_ids,truth,source_meta,current_locations,source_learner,source_program_k,true,nullptr);
        const SourceFrames p7s_frames=compress_source_variants(p7s_mixed,p7s_literal,frames,zlevel);const uint64_t w7s=p7s_frames.wire;p7s.encode_seconds+=elapsed(e7s);
        for(size_t category=0;category<kSourceCategoryCount;++category){source_stats.selected_wire[category]+=p7s_frames.category[category].selected_wire;source_stats.literal_wire[category]+=p7s_frames.category[category].literal_wire;source_stats.mixed_wire[category]+=p7s_frames.category[category].mixed_wire;}
        const auto d7s=Clock::now();const SourceRaw p7s_decoded=decompress_source_variants(p7s_frames,frames);decode_source_variants(p7s_decoded,p7s.decoder);p7s.decode_seconds+=elapsed(d7s);verify_definitions(new_ids,truth,p7s.decoder);

        const auto e7=Clock::now();const SplitBuffer p7_raw=encode_persistent_templates(new_ids,truth,persistent_encoder,persistent_stats);const std::vector<uint8_t>p7_control_frame=frames.compress(p7_raw.control,zlevel);const std::vector<uint8_t>p7_data_frame=frames.compress(p7_raw.data,zlevel);const uint64_t w7=p7_control_frame.size()+p7_data_frame.size()+8;p7.encode_seconds+=elapsed(e7);
        p7_control_wire+=p7_control_frame.size()+4;p7_data_wire+=p7_data_frame.size()+4;
        const auto d7=Clock::now();const std::vector<uint8_t>p7_control_decoded=frames.decompress(p7_control_frame);const std::vector<uint8_t>p7_data_decoded=frames.decompress(p7_data_frame);decode_persistent_templates(p7_control_decoded,p7_data_decoded,persistent_decoder);p7.decode_seconds+=elapsed(d7);verify_definitions(new_ids,truth,persistent_decoder.lines);

        hybrid_encoder.rule_id.clear();hybrid_encoder.rules.clear();hybrid_decoder.rules.clear();const auto e7b=Clock::now();const SplitBuffer p7b_raw=encode_persistent_templates(new_ids,truth,hybrid_encoder,hybrid_stats);const std::vector<uint8_t>p7b_control_frame=frames.compress(p7b_raw.control,zlevel),p7b_data_frame=frames.compress(p7b_raw.data,zlevel);const uint64_t w7b=p7b_control_frame.size()+p7b_data_frame.size()+8;p7b.encode_seconds+=elapsed(e7b);
        const auto d7b=Clock::now();const std::vector<uint8_t>p7b_control_decoded=frames.decompress(p7b_control_frame),p7b_data_decoded=frames.decompress(p7b_data_frame);decode_persistent_templates(p7b_control_decoded,p7b_data_decoded,hybrid_decoder);p7b.decode_seconds+=elapsed(d7b);verify_definitions(new_ids,truth,hybrid_decoder.lines);

        uint64_t w8=0;
        if(use_pretrained){const auto e8=Clock::now();const SplitBuffer p8_raw=encode_persistent_templates(new_ids,truth,pretrained.encoder,pretrained_stats,pretrained.rule_count(),pretrained.token_count(),pretrain_all_identifiers);const PackedFrame p8_control_frame=model_frames.compress_best(p8_raw.control,zlevel),p8_data_frame=model_frames.compress_best(p8_raw.data,zlevel);w8=p8_control_frame.bytes.size()+p8_data_frame.bytes.size()+10;p8.encode_seconds+=elapsed(e8);
            const auto d8=Clock::now();const std::vector<uint8_t>p8_control_decoded=model_frames.decompress(p8_control_frame),p8_data_decoded=model_frames.decompress(p8_data_frame);decode_persistent_templates(p8_control_decoded,p8_data_decoded,pretrained.decoder);p8.decode_seconds+=elapsed(d8);verify_definitions(new_ids,truth,pretrained.decoder.lines);}

        const uint64_t w1=p1_frame.size()+4,w5=p5_size,w6=p6_frame.size()+4;
        const size_t best_size=std::min({size_t(w1),p4_size,size_t(w5),size_t(w6),size_t(w7s),size_t(p9_frames.wire)});
        const auto d10 = Clock::now();
        if(best_size==p4_size)decode_templates(p4_control_decoded,p4_data_decoded,p10.decoder);
        else if(best_size==p9_frames.wire)decode_semantic_templates(p9_decoded,p10.decoder);
        else if(best_size==p5_size)decode_multiline(p5_control_decoded,p5_data_decoded,p10.decoder);
        else if(best_size==p6_frame.size()+4)decode_forest(frames.decompress(p6_frame),p10.decoder);
        else if(best_size==w7s)decode_source_variants(p7s_decoded,p10.decoder);
        else decode_literals(frames.decompress(p1_frame), p10.decoder);
        p10.decode_seconds += elapsed(d10);
        verify_definitions(new_ids, truth, p10.decoder);

        uint64_t w10p=0;
        if(use_pretrained){const size_t best_pretrained=std::min({size_t(w1),size_t(w8d),size_t(w5),size_t(w6),size_t(w7s),size_t(p9_frames.wire)});const auto d10p=Clock::now();if(best_pretrained==w8d)decode_templates(p8d_control_decoded,p8d_data_decoded,p10p.decoder);else if(best_pretrained==p9_frames.wire)decode_semantic_templates(p9_decoded,p10p.decoder);else if(best_pretrained==w5)decode_multiline(p5_control_decoded,p5_data_decoded,p10p.decoder);else if(best_pretrained==w6)decode_forest(frames.decompress(p6_frame),p10p.decoder);else if(best_pretrained==w7s)decode_source_variants(p7s_decoded,p10p.decoder);else decode_literals(frames.decompress(p1_frame),p10p.decoder);p10p.decode_seconds+=elapsed(d10p);verify_definitions(new_ids,truth,p10p.decoder);w10p=best_pretrained+1;}

        const uint64_t w0 = p0_frame.size() + 4;
        const uint64_t w4 = p4_size;
        const uint64_t w9=p9_frames.wire;
        const uint64_t w10 = std::min({w1,w4,w5,w6,w7s,w9}) + 1;
        std::vector<uint64_t>current{w0,w1,w4,w5,w6,w7s,w7,w7b,w9};if(use_pretrained){current.push_back(w8);current.push_back(w8d);}current.push_back(w10);if(use_pretrained)current.push_back(w10p);
        for (size_t r = 0; r < rows.size(); ++r) {rows[r]->wire += current[r];rows[r]->tu_wire.push_back(current[r]);}
        p10.encode_seconds = p1.encode_seconds + p4.encode_seconds+p5.encode_seconds+p6.encode_seconds+p7s.encode_seconds+p9.encode_seconds;
        if(use_pretrained)p10p.encode_seconds=p1.encode_seconds+p8d.encode_seconds+p5.encode_seconds+p6.encode_seconds+p7s.encode_seconds+p9.encode_seconds;
        source_learner.observe_tu(observations,new_ids,truth);
        cumulative_raw += file.len;
        while (checkpoint < std::size(fractions) &&
               double(cumulative_raw) >= fractions[checkpoint] * double(corpus.raw)) {
            for (Row* row : rows) row->checkpoints.push_back(double(cumulative_raw) / double(row->wire));
            ++checkpoint;
        }
    }

    while (checkpoint < std::size(fractions)) {
        for (Row* row : rows) row->checkpoints.push_back(double(cumulative_raw) / double(row->wire));
        ++checkpoint;
    }

    // P8d reuses the P4 transform output in this all-row harness.  Charge the transform work as
    // well as its additional dictionary/plain actual-frame comparison before reporting throughput.
    if(use_pretrained){p8d.encode_seconds+=p4.encode_seconds;p10p.encode_seconds=p1.encode_seconds+p8d.encode_seconds+p5.encode_seconds+p6.encode_seconds+p7s.encode_seconds+p9.encode_seconds;}

    std::array<uint64_t,kSourceCategoryCount>attributed_zstd{};
    for(size_t category=0;category<kSourceCategoryCount;++category)attributed_zstd[category]=frames.compress(attributed_bytes[category],zlevel).size();
    const std::array<uint64_t,6>source_variant_hist=source_learner.variant_histogram();
    const double total_seconds = elapsed(begin);
    std::printf("PTGC exact definition capability: %s\n", manifest);
    std::printf("TUs=%zu raw=%.2f MiB line_occurrences=%llu distinct_lines=%zu distinct_bytes=%.2f MiB marker_def_bytes=%.2f MiB\n",
                corpus.files.size(), corpus.raw / 1048576.0, static_cast<unsigned long long>(total_occurrences),
                truth.size(), truth.bytes() / 1048576.0, marker_definition_bytes / 1048576.0);
    std::printf("all rows: independently decoded exact definitions = PASS\n\n");
    std::printf("row                         wire bytes  wire MiB   raw/wire  vs-P0   enc-effective GB/s  dec-effective GB/s  trailing-window\n");
    for (const Row* row : rows) {
        std::printf("%-27s %11llu %9.3f %10.1fx %7.3fx %19.2f %19.2f %15.1fx\n", row->name,
                    static_cast<unsigned long long>(row->wire),row->wire / 1048576.0, double(corpus.raw) / double(row->wire),
                    double(p0.wire) / double(row->wire), corpus.raw / 1e9 / row->encode_seconds,
                    corpus.raw / 1e9 / row->decode_seconds,
                    trailing_ratio(corpus.files, row->tu_wire, corpus.raw));
        std::printf("  checkpoints 10/25/50/75/100%%:");
        for (double ratio : row->checkpoints) std::printf(" %.1fx", ratio);
        std::printf("\n");
    }
    std::printf("\nP4 rules=%llu instances=%llu template-input=%.2f MiB literal-fallbacks=%llu unique-rule-slots=%llu slot-occurrences=%llu lexicon-entries=%llu lexicon-refs=%llu\n",
                static_cast<unsigned long long>(template_stats.rules),
                static_cast<unsigned long long>(template_stats.instances),
                template_stats.template_input_bytes / 1048576.0,
                static_cast<unsigned long long>(template_stats.literal_fallbacks),
                static_cast<unsigned long long>(template_stats.unique_slots),
                static_cast<unsigned long long>(template_stats.slot_occurrences),
                static_cast<unsigned long long>(template_stats.lexicon_entries),
                static_cast<unsigned long long>(template_stats.lexicon_references));
    std::printf("P6 deltas=%llu comparisons=%llu copied=%.2f MiB added=%.2f MiB\n",
                static_cast<unsigned long long>(forest_stats.delta_records),static_cast<unsigned long long>(forest_stats.comparisons),
                forest_stats.copy_bytes/1048576.0,forest_stats.add_bytes/1048576.0);
    std::printf("P7-source k=%u candidate-plan records=%llu literal=%llu prefix=%llu program=%llu candidates=%llu program-comparisons=%llu copied=%.2f MiB added=%.2f MiB raw-fallback=%.2f MiB raw-saving=%.2f MiB\n",
                source_program_k,static_cast<unsigned long long>(source_stats.records),static_cast<unsigned long long>(source_stats.literal_records),
                static_cast<unsigned long long>(source_stats.prefix_records),static_cast<unsigned long long>(source_stats.program_records),
                static_cast<unsigned long long>(source_stats.candidates),static_cast<unsigned long long>(source_stats.program_comparisons),
                source_stats.copy_bytes/1048576.0,source_stats.add_bytes/1048576.0,source_stats.raw_fallback_bytes/1048576.0,source_stats.union_raw_saving/1048576.0);
    std::printf("P7-source attributed raw/z%d: marker=%.3f/%.3f toolchain=%.3f/%.3f project=%.3f/%.3f MiB; actual literal wire marker/tool/project=%.3f/%.3f/%.3f; mixed=%.3f/%.3f/%.3f; selected=%.3f/%.3f/%.3f MiB\n",zlevel,
                attributed_bytes[kMarkerCategory].size()/1048576.0,attributed_zstd[kMarkerCategory]/1048576.0,
                attributed_bytes[kToolchainCategory].size()/1048576.0,attributed_zstd[kToolchainCategory]/1048576.0,
                attributed_bytes[kProjectCategory].size()/1048576.0,attributed_zstd[kProjectCategory]/1048576.0,
                source_stats.literal_wire[kMarkerCategory]/1048576.0,source_stats.literal_wire[kToolchainCategory]/1048576.0,source_stats.literal_wire[kProjectCategory]/1048576.0,
                source_stats.mixed_wire[kMarkerCategory]/1048576.0,source_stats.mixed_wire[kToolchainCategory]/1048576.0,source_stats.mixed_wire[kProjectCategory]/1048576.0,
                source_stats.selected_wire[kMarkerCategory]/1048576.0,source_stats.selected_wire[kToolchainCategory]/1048576.0,source_stats.selected_wire[kProjectCategory]/1048576.0);
    std::printf("P7-source online locations=%zu variant-hist 1/2/3/4/5+=%llu/%llu/%llu/%llu/%llu first-variant=%.2f MiB later-variant=%.2f MiB exclusive raw savings exact/medoid/basename/skeleton/sameTU=%.2f/%.2f/%.2f/%.2f/%.2f MiB\n",
                source_learner.locations(),static_cast<unsigned long long>(source_variant_hist[1]),static_cast<unsigned long long>(source_variant_hist[2]),
                static_cast<unsigned long long>(source_variant_hist[3]),static_cast<unsigned long long>(source_variant_hist[4]),static_cast<unsigned long long>(source_variant_hist[5]),
                source_stats.first_variant_bytes/1048576.0,source_stats.later_variant_bytes/1048576.0,
                source_stats.exclusive_raw_saving[0]/1048576.0,source_stats.exclusive_raw_saving[1]/1048576.0,source_stats.exclusive_raw_saving[2]/1048576.0,
                source_stats.exclusive_raw_saving[3]/1048576.0,source_stats.exclusive_raw_saving[4]/1048576.0);
    std::printf("P5 selected rules=%llu instances=%llu raw-units=%llu lines=%llu\n",static_cast<unsigned long long>(multi_stats.rules),static_cast<unsigned long long>(multi_stats.instances),static_cast<unsigned long long>(multi_stats.raw_units),static_cast<unsigned long long>(multi_stats.lines));
    std::printf("P7 new-rules=%llu refs=%llu new-tokens=%llu token-refs=%llu inline-values=%llu raw-records=%llu\n",
                static_cast<unsigned long long>(persistent_stats.new_rules),static_cast<unsigned long long>(persistent_stats.rule_refs),
                static_cast<unsigned long long>(persistent_stats.new_tokens),static_cast<unsigned long long>(persistent_stats.token_refs),
                static_cast<unsigned long long>(persistent_stats.inline_values),static_cast<unsigned long long>(persistent_stats.raw_records));
    std::printf("P7b new-rules=%llu refs=%llu new-tokens=%llu token-refs=%llu inline-values=%llu raw-records=%llu\n",
                static_cast<unsigned long long>(hybrid_stats.new_rules),static_cast<unsigned long long>(hybrid_stats.rule_refs),static_cast<unsigned long long>(hybrid_stats.new_tokens),
                static_cast<unsigned long long>(hybrid_stats.token_refs),static_cast<unsigned long long>(hybrid_stats.inline_values),static_cast<unsigned long long>(hybrid_stats.raw_records));
    std::printf("P9 semantic rules=%llu instances=%llu literal-fallbacks=%llu lexicon-entries=%llu refs=%llu; wire control=%.3f rule-literal=%.3f raw=%.3f identifier=%.3f number=%.3f string=%.3f MiB\n",
                static_cast<unsigned long long>(semantic_stats.rules),static_cast<unsigned long long>(semantic_stats.instances),static_cast<unsigned long long>(semantic_stats.literal_fallbacks),static_cast<unsigned long long>(semantic_stats.lexicon_entries),static_cast<unsigned long long>(semantic_stats.lexicon_references),p9_control_wire/1048576.0,p9_payload_wire[kRuleLiteral]/1048576.0,p9_payload_wire[kRawDefinition]/1048576.0,p9_payload_wire[kIdentifierValue]/1048576.0,p9_payload_wire[kNumberValue]/1048576.0,p9_payload_wire[kStringValue]/1048576.0);
    std::printf("P9 semantic exact wire bytes: control=%llu rule-literal=%llu raw=%llu identifier=%llu number=%llu string=%llu\n",static_cast<unsigned long long>(p9_control_wire),static_cast<unsigned long long>(p9_payload_wire[kRuleLiteral]),static_cast<unsigned long long>(p9_payload_wire[kRawDefinition]),static_cast<unsigned long long>(p9_payload_wire[kIdentifierValue]),static_cast<unsigned long long>(p9_payload_wire[kNumberValue]),static_cast<unsigned long long>(p9_payload_wire[kStringValue]));
    if(use_pretrained){std::printf("P8 pretrained model: training=%.2f GiB/%llu lines/%llu per-corpus-distinct in %.2fs; mode=%s min-corpora=%u; candidates=%llu rules/%llu tokens; selected=%u rules/%u tokens; package raw=%.1f KiB wire-z%d=%.1f KiB (charged)\n",
                pretrained.training_raw/1073741824.0,static_cast<unsigned long long>(pretrained.training_lines),static_cast<unsigned long long>(pretrained.training_distinct_lines),pretrained.training_seconds,
                pretrain_all_identifiers?"all-identifiers":"keywords-literal",model_min_corpora,
                static_cast<unsigned long long>(pretrained.candidate_rules),static_cast<unsigned long long>(pretrained.candidate_tokens),pretrained.rule_count(),pretrained.token_count(),pretrained.raw.size()/1024.0,zlevel,pretrained.frame.size()/1024.0);
        std::printf("P8 online additions: rules=%llu refs=%llu tokens=%llu token-refs=%llu inline=%llu raw=%llu; pretrained superblocks=%llu lines=%llu token-refs=%llu; model-dictionary wins=%llu saved=%.1f KiB\n",
                static_cast<unsigned long long>(pretrained_stats.new_rules),static_cast<unsigned long long>(pretrained_stats.rule_refs),static_cast<unsigned long long>(pretrained_stats.new_tokens),static_cast<unsigned long long>(pretrained_stats.token_refs),static_cast<unsigned long long>(pretrained_stats.inline_values),static_cast<unsigned long long>(pretrained_stats.raw_records),
                static_cast<unsigned long long>(pretrained_stats.pretrained_superblocks),static_cast<unsigned long long>(pretrained_stats.pretrained_rule_refs),static_cast<unsigned long long>(pretrained_stats.pretrained_token_refs),static_cast<unsigned long long>(model_frames.dictionary_wins),model_frames.dictionary_saved/1024.0);
        std::printf("P8 model-as-dictionary over P4: wins=%llu saved-before-model-charge=%.1f KiB\n",static_cast<unsigned long long>(model_p4_frames.dictionary_wins),model_p4_frames.dictionary_saved/1024.0);}
    std::printf("split wire: P4 control=%.3f data=%.3f MiB; P7 control=%.3f data=%.3f MiB\n",p4_control_wire/1048576.0,p4_data_wire/1048576.0,p7_control_wire/1048576.0,p7_data_wire/1048576.0);
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    std::fprintf(stderr, "PTGC total=%.2fs effective=%.2f GB/s peakRSS=%.1f MiB\n", total_seconds,
                 corpus.raw / 1e9 / total_seconds, usage.ru_maxrss / 1024.0);
    return 0;
}
