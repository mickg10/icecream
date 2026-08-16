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
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <tuple>
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

static std::vector<uint8_t>load_file_bytes(const std::string&path){
    FILE*file=std::fopen(path.c_str(),"rb");if(!file)die(path.c_str());struct stat st{};
    if(::fstat(::fileno(file),&st)!=0||st.st_size<0||uint64_t(st.st_size)>SIZE_MAX)die_msg("unsupported model file size");
    std::vector<uint8_t>bytes(size_t(st.st_size));if(st.st_size&&std::fread(bytes.data(),1,bytes.size(),file)!=bytes.size())die_msg("short model read");
    std::fclose(file);return bytes;
}

static void save_file_bytes(const std::string&path,const std::vector<uint8_t>&bytes){
    FILE*file=std::fopen(path.c_str(),"wb");if(!file)die(path.c_str());
    if(!bytes.empty()&&std::fwrite(bytes.data(),1,bytes.size(),file)!=bytes.size())die_msg("short model write");
    if(std::fclose(file)!=0)die_msg("model close failed");
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

static void build_parsed_keys(ParsedLine&out){
    if(out.literals.size()!=out.occurrence_slot.size()+1)die_msg("parsed template literal cardinality mismatch");
    out.key.clear();out.coarse_key.clear();
    key_varint(out.key,out.slot_type.size());
    for(uint8_t type:out.slot_type)out.key.push_back(char(type));
    key_varint(out.key,out.occurrence_slot.size());
    for(size_t k=0;k<out.occurrence_slot.size();++k){const uint32_t slot=out.occurrence_slot[k];
        if(slot>=out.slot_type.size())die_msg("parsed template slot out of range");
        key_varint(out.key,out.literals[k].size());
        out.key.append(reinterpret_cast<const char*>(out.literals[k].data()),out.literals[k].size());
        key_varint(out.key,slot);
    }
    key_varint(out.key,out.literals.back().size());
    out.key.append(reinterpret_cast<const char*>(out.literals.back().data()),out.literals.back().size());
    key_varint(out.coarse_key,out.occurrence_slot.size());
    for(size_t k=0;k<out.occurrence_slot.size();++k){
        key_varint(out.coarse_key,out.literals[k].size());
        out.coarse_key.append(reinterpret_cast<const char*>(out.literals[k].data()),out.literals[k].size());
        out.coarse_key.push_back(char(out.slot_type[out.occurrence_slot[k]]));
    }
    key_varint(out.coarse_key,out.literals.back().size());
    out.coarse_key.append(reinterpret_cast<const char*>(out.literals.back().data()),out.literals.back().size());
}

static ParsedLine parse_line(const uint8_t* p, uint32_t n,bool parameterize_keywords=false,
                             bool parameterize_whitespace=false) {
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
        } else if (parameterize_whitespace &&
                   (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r' ||
                    p[i] == '\f' || p[i] == '\v')) {
            const uint32_t begin = i++;
            while (i < n &&
                   (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r' ||
                    p[i] == '\f' || p[i] == '\v')) ++i;
            // Whitespace uses the opaque exact-byte slot channel.  Its type is intentionally not
            // semantic: parameterization only lets one portable statement shape retain exact
            // formatting as an instance value.
            emit_slot(kString, p + begin, i - begin);
        } else {
            literal.push_back(p[i++]);
        }
    }
    out.literals.push_back(std::move(literal));

    build_parsed_keys(out);
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
struct SemanticBuffer {std::vector<uint8_t>control;std::array<std::vector<uint8_t>,kSemanticChannels>payload;std::vector<uint32_t>raw_ids;};
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
        else for(size_t index:group.members){const uint32_t id=ids[index];out.control.push_back(0);put_zigzag(out.control,int64_t(id)-previous_line_id);previous_line_id=id;put_varint(out.control,store.len(id));const uint8_t*p=store.data(id);out.payload[kRawDefinition].insert(out.payload[kRawDefinition].end(),p,p+store.len(id));out.raw_ids.push_back(id);++stats.literal_fallbacks;}}
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
    uint32_t path_id=0;
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

static SourceLocation make_source_location(const std::string&path,uint32_t line,uint32_t path_id){
    SourceLocation result;result.logical_line=line;result.path_id=path_id;
    if(path.empty())return result;
    result.path_hash=hash_bytes(reinterpret_cast<const uint8_t*>(path.data()),uint32_t(path.size()));
    const size_t slash=path.find_last_of("/\\");const std::string_view base(path.data()+(slash==std::string::npos?0:slash+1),path.size()-(slash==std::string::npos?0:slash+1));
    result.basename_hash=hash_bytes(reinterpret_cast<const uint8_t*>(base.data()),uint32_t(base.size()));
    return result;
}

class SourceCatalog {
public:
    uint32_t intern(const std::string&path){
        auto found=by_path_.find(path);
        if(found!=by_path_.end())return found->second;
        if(files_.size()>=UINT32_MAX)die_msg("too many source paths");
        const uint32_t id=uint32_t(files_.size());
        File file;file.path=path;files_.push_back(std::move(file));
        by_path_.emplace(path,id);
        return id;
    }

    bool line(uint32_t path_id,uint32_t logical_line,const uint8_t*&data,uint32_t&len){
        if(path_id==0||path_id>=files_.size()||logical_line==0)return false;
        File&file=files_[path_id];
        load(file);
        if(!file.available||logical_line>file.lines.size())return false;
        const Span span=file.lines[logical_line-1];
        data=file.bytes.data()+span.off;len=span.len;
        return true;
    }

private:
    struct Span {uint32_t off=0,len=0;};
    struct File {
        std::string path;
        bool loaded=false,available=false;
        std::vector<uint8_t>bytes;
        std::vector<Span>lines;
    };

    static void load(File&file){
        if(file.loaded)return;
        file.loaded=true;
        if(file.path.empty()||file.path.front()=='<')return;
        FILE*input=std::fopen(file.path.c_str(),"rb");
        if(!input)return;
        struct stat st{};
        if(::fstat(::fileno(input),&st)!=0||st.st_size<0||uint64_t(st.st_size)>UINT32_MAX){std::fclose(input);return;}
        file.bytes.resize(size_t(st.st_size));
        if(st.st_size&&std::fread(file.bytes.data(),1,file.bytes.size(),input)!=file.bytes.size()){
            std::fclose(input);file.bytes.clear();return;
        }
        std::fclose(input);
        uint32_t begin=0;
        for(uint32_t i=0;i<file.bytes.size();++i){
            if(file.bytes[i]=='\n'){file.lines.push_back({begin,i+1-begin});begin=i+1;}
        }
        if(begin<file.bytes.size())file.lines.push_back({begin,uint32_t(file.bytes.size()-begin)});
        file.available=true;
    }

    std::vector<File>files_{File{}};
    std::unordered_map<std::string,uint32_t>by_path_;
};

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

// P7p: transmit only source lines that materially contribute to current-TU definitions.  Source
// bases are local to the frame, sorted in source-file order, and reconstructed before any definition
// refers to them.  This keeps the first capability row independent of persistent cache assumptions.
struct SourcePackPlanStats {
    uint64_t available_records=0,available_bytes=0,source_records=0,literal_records=0;
    uint64_t basis_records=0,basis_bytes=0,residual_bytes=0,target_bytes=0;
};

struct SourcePackRaw {
    std::vector<uint8_t>basis_control,basis_data,definition_control,definition_data;
    SourcePackPlanStats stats;
    bool implicit_order=false,basis_templates=false,basis_pretrained=false;
    bool valid=false;
};

struct SourcePackFrames {
    std::vector<uint8_t>basis_control,basis_data,definition_control,definition_data;
    std::vector<uint8_t>combined_frame;
    std::array<bool,4>dictionary{};
    bool combined=false,combined_dictionary=false;
    SourcePackPlanStats stats;
    uint64_t wire=UINT64_MAX;
    unsigned residual_percent=0;
    bool implicit_order=false,basis_templates=false,basis_pretrained=false;
    bool valid=false;
};

struct SourcePackStats {
    SourcePackPlanStats selected;
    std::array<uint64_t,6>threshold_wins{};
    std::array<uint64_t,6>threshold_tus{},threshold_wire{},threshold_basis_bytes{},threshold_residual_bytes{};
    uint64_t candidate_tus=0,frame_wins=0,literal_wire=0,source_wire=0,selected_wire=0;
    std::array<uint64_t,4>frame_wire{};
    uint64_t combined_wins=0,combined_wire=0;
};

static constexpr std::array<unsigned,6>kSourcePackThresholds={0,6,12,25,50,100};

struct SourcePackBase {
    std::string bytes;
    uint32_t path_id=0,logical_line=0,old_id=0;
};

struct SourcePackRecord {
    uint32_t id=0,base_old_id=0,prefix=0,suffix=0;
    uint32_t path_id=0,logical_line=0;
    bool source=false;
};

static SourcePackRaw encode_source_pack(const std::vector<uint32_t>&ids,const LineStore&store,
                                        const std::vector<LineSourceMeta>&meta,
                                        const std::unordered_map<uint32_t,std::vector<SourceLocation>>&locations,
                                        SourceCatalog&catalog,unsigned residual_percent,bool implicit_order=false){
    SourcePackRaw raw;raw.implicit_order=implicit_order;
    std::vector<SourcePackRecord>records;records.reserve(ids.size());
    std::vector<SourcePackBase>bases;
    std::unordered_map<std::string,uint32_t>base_ids;
    base_ids.reserve(ids.size()/2+1);

    for(uint32_t id:ids){
        SourcePackRecord record;record.id=id;
        const uint32_t target_len=store.len(id);raw.stats.target_bytes+=target_len;
        bool available=false;uint32_t best_prefix=0,best_suffix=0,best_path=0,best_line=0;
        const uint8_t*best_base=nullptr;uint32_t best_base_len=0;uint32_t best_residual=UINT32_MAX;
        auto consider=[&](const SourceLocation&location){
            if(!location.valid()||location.path_id==0)return;
            const uint8_t*base=nullptr;uint32_t base_len=0;
            if(!catalog.line(location.path_id,location.logical_line,base,base_len))return;
            available=true;uint32_t prefix=0,suffix=0;
            common_prefix_suffix(base,base_len,store.data(id),target_len,prefix,suffix);
            const uint32_t residual=target_len-prefix-suffix;
            if(residual<best_residual||
               (residual==best_residual&&std::tie(location.path_id,location.logical_line)<std::tie(best_path,best_line))){
                best_prefix=prefix;best_suffix=suffix;best_path=location.path_id;best_line=location.logical_line;
                best_base=base;best_base_len=base_len;best_residual=residual;
            }
        };
        if(!meta[id].marker&&meta[id].category==kProjectCategory){
            auto found=locations.find(id);
            if(found!=locations.end())for(const SourceLocation&location:found->second)consider(location);
            if(found==locations.end()||found->second.empty())consider(meta[id].location);
        }
        if(available){++raw.stats.available_records;raw.stats.available_bytes+=target_len;}
        const size_t literal_cost=literal_source_cost(id,target_len);
        const size_t source_cost=best_base?prefix_source_cost(id,target_len,1,best_prefix,best_suffix):SIZE_MAX;
        const bool ratio_ok=best_base&&uint64_t(best_residual)*100<=uint64_t(target_len)*residual_percent;
        if(ratio_ok&&source_cost<literal_cost){
            std::string key(reinterpret_cast<const char*>(best_base),best_base_len);
            auto inserted=base_ids.emplace(key,uint32_t(bases.size()));
            uint32_t old_id=inserted.first->second;
            if(inserted.second){bases.push_back({std::move(key),best_path,best_line,old_id});}
            record.source=true;record.base_old_id=old_id;record.prefix=best_prefix;record.suffix=best_suffix;
            record.path_id=best_path;record.logical_line=best_line;
            ++raw.stats.source_records;raw.stats.residual_bytes+=best_residual;
        }else ++raw.stats.literal_records;
        records.push_back(record);
    }
    if(bases.empty())return raw;
    raw.valid=true;raw.stats.basis_records=bases.size();
    std::vector<uint32_t>basis_order(bases.size()),basis_remap(bases.size());
    std::iota(basis_order.begin(),basis_order.end(),0);
    std::sort(basis_order.begin(),basis_order.end(),[&](uint32_t a,uint32_t b){
        const SourcePackBase&x=bases[a],&y=bases[b];
        if(x.path_id!=y.path_id)return x.path_id<y.path_id;
        if(x.logical_line!=y.logical_line)return x.logical_line<y.logical_line;
        return x.bytes<y.bytes;
    });
    put_varint(raw.basis_control,bases.size());
    for(uint32_t new_id=0;new_id<basis_order.size();++new_id){
        const SourcePackBase&base=bases[basis_order[new_id]];basis_remap[base.old_id]=new_id;
        put_varint(raw.basis_control,base.bytes.size());
        raw.basis_data.insert(raw.basis_data.end(),base.bytes.begin(),base.bytes.end());
        raw.stats.basis_bytes+=base.bytes.size();
    }

    if(implicit_order){
        put_varint(raw.definition_control,records.size());
        const size_t bitmap_bytes=(records.size()+7)/8,bitmap_offset=raw.definition_control.size();
        raw.definition_control.resize(bitmap_offset+bitmap_bytes);
        for(size_t index=0;index<records.size();++index)if(records[index].source)raw.definition_control[bitmap_offset+index/8]|=uint8_t(1u<<(index%8));
        for(const SourcePackRecord&record:records)if(record.source){
            put_varint(raw.definition_control,basis_remap[record.base_old_id]);put_varint(raw.definition_control,record.prefix);put_varint(raw.definition_control,record.suffix);
        }
        for(const SourcePackRecord&record:records){const uint32_t len=store.len(record.id);
            if(record.source)raw.definition_data.insert(raw.definition_data.end(),store.data(record.id)+record.prefix,store.data(record.id)+len-record.suffix);
            else raw.definition_data.insert(raw.definition_data.end(),store.data(record.id),store.data(record.id)+len);
        }
        return raw;
    }

    std::vector<uint32_t>record_order(records.size());std::iota(record_order.begin(),record_order.end(),0);
    std::sort(record_order.begin(),record_order.end(),[&](uint32_t a,uint32_t b){
        const SourcePackRecord&x=records[a],&y=records[b];
        if(x.source!=y.source)return x.source>y.source;
        if(x.source){
            if(x.path_id!=y.path_id)return x.path_id<y.path_id;
            if(x.logical_line!=y.logical_line)return x.logical_line<y.logical_line;
        }
        return line_less(x.id,y.id,store);
    });
    put_varint(raw.definition_control,records.size());
    for(uint32_t index:record_order){
        const SourcePackRecord&record=records[index];const uint32_t len=store.len(record.id);
        raw.definition_control.push_back(record.source?1:0);put_varint(raw.definition_control,record.id);put_varint(raw.definition_control,len);
        if(record.source){
            put_varint(raw.definition_control,basis_remap[record.base_old_id]);put_varint(raw.definition_control,record.prefix);put_varint(raw.definition_control,record.suffix);
            raw.definition_data.insert(raw.definition_data.end(),store.data(record.id)+record.prefix,store.data(record.id)+len-record.suffix);
        }else raw.definition_data.insert(raw.definition_data.end(),store.data(record.id),store.data(record.id)+len);
    }
    return raw;
}

static SourcePackRaw template_source_pack_basis(const SourcePackRaw&input,bool parameterize_keywords){
    if(!input.valid||input.basis_templates||input.basis_pretrained)return {};
    const uint8_t*p=input.basis_control.data(),*end=p+input.basis_control.size(),*data=input.basis_data.data(),*data_end=data+input.basis_data.size();const uint64_t count=get_varint(p,end);if(count>UINT32_MAX)die_msg("source template basis count too large");LineStore store;std::vector<uint32_t>ids;ids.reserve(size_t(count));
    for(uint64_t i=0;i<count;++i){const uint64_t len=get_varint(p,end);if(len>UINT32_MAX||len>uint64_t(data_end-data))die_msg("source template basis exceeds frame");const LineStore::Result result=store.intern(data,uint32_t(len));if(!result.first||result.id!=i+1)die_msg("source template basis is not unique/dense");ids.push_back(result.id);data+=len;}
    if(p!=end||data!=data_end)die_msg("trailing source template basis bytes");
    TemplateStats stats;const SplitBuffer encoded=encode_templates(ids,store,stats,parameterize_keywords);SourcePackRaw output=input;output.basis_control.clear();put_varint(output.basis_control,count);output.basis_control.insert(output.basis_control.end(),encoded.control.begin(),encoded.control.end());output.basis_data=encoded.data;output.basis_templates=true;return output;
}

// The frozen model is itself a bank of parameterized superblocks.  Apply those rules to the
// contributing source basis, allow TU-local rule additions inside this one frame, and discard the
// additions afterwards.  The same serialized package can independently serve as zstd history.
static SourcePackRaw pretrained_source_pack_basis(const SourcePackRaw&input,const PretrainedPackage&model,
                                                   bool parameterize_keywords){
    if(!input.valid||input.basis_templates||input.basis_pretrained)return {};
    const uint8_t*p=input.basis_control.data(),*end=p+input.basis_control.size(),*data=input.basis_data.data(),*data_end=data+input.basis_data.size();const uint64_t count=get_varint(p,end);if(count>UINT32_MAX)die_msg("pretrained source basis count too large");LineStore store;std::vector<uint32_t>ids;ids.reserve(size_t(count));
    for(uint64_t i=0;i<count;++i){const uint64_t len=get_varint(p,end);if(len>UINT32_MAX||len>uint64_t(data_end-data))die_msg("pretrained source basis exceeds frame");const LineStore::Result result=store.intern(data,uint32_t(len));if(!result.first||result.id!=i+1)die_msg("pretrained source basis is not unique/dense");ids.push_back(result.id);data+=len;}
    if(p!=end||data!=data_end)die_msg("trailing pretrained source basis bytes");
    PersistentEncoder encoder=model.encoder;PersistentStats stats;const SplitBuffer encoded=encode_persistent_templates(ids,store,encoder,stats,model.rule_count(),model.token_count(),parameterize_keywords);SourcePackRaw output=input;output.basis_control.clear();put_varint(output.basis_control,count);output.basis_control.insert(output.basis_control.end(),encoded.control.begin(),encoded.control.end());output.basis_data=encoded.data;output.basis_pretrained=true;return output;
}

static std::vector<std::vector<uint8_t>>decode_source_pack_bases(const SourcePackFrames&frames,
                                                                 const std::vector<uint8_t>&basis_control,
                                                                 const std::vector<uint8_t>&basis_data,
                                                                 const PersistentDecoder*pretrained){
    if(!frames.basis_templates&&!frames.basis_pretrained){const uint8_t*p=basis_control.data(),*end=p+basis_control.size(),*data=basis_data.data(),*data_end=data+basis_data.size();const uint64_t count=get_varint(p,end);std::vector<std::vector<uint8_t>>bases;bases.reserve(size_t(count));for(uint64_t i=0;i<count;++i){const uint64_t len=get_varint(p,end);if(len>uint64_t(data_end-data))die_msg("source-pack basis exceeds frame");bases.emplace_back(data,data+len);data+=len;}if(p!=end||data!=data_end)die_msg("trailing source-pack basis bytes");return bases;}
    const uint8_t*p=basis_control.data(),*end=p+basis_control.size();const uint64_t count=get_varint(p,end);std::vector<uint8_t>control(p,end);std::vector<std::vector<uint8_t>>bases;bases.reserve(size_t(count));
    if(frames.basis_pretrained){if(!pretrained)die_msg("pretrained source basis model unavailable");PersistentDecoder decoded=*pretrained;decode_persistent_templates(control,basis_data,decoded);if(decoded.lines.lines.size()!=count+1)die_msg("pretrained source basis cardinality mismatch");for(uint64_t i=1;i<=count;++i)bases.push_back(std::move(decoded.lines.lines[size_t(i)]));return bases;}
    DecoderStore decoded;decode_templates(control,basis_data,decoded);if(decoded.lines.size()!=count+1)die_msg("source template basis cardinality mismatch");for(uint64_t i=1;i<=count;++i)bases.push_back(std::move(decoded.lines[size_t(i)]));return bases;
}

static SourcePackFrames compress_source_pack(const SourcePackRaw&raw,FrameCodec&codec,int level,unsigned residual_percent,
                                             ModelFrameCodec*model=nullptr){
    SourcePackFrames frames;if(!raw.valid)return frames;
    frames.valid=true;frames.residual_percent=residual_percent;frames.stats=raw.stats;frames.implicit_order=raw.implicit_order;frames.basis_templates=raw.basis_templates;frames.basis_pretrained=raw.basis_pretrained;
    auto compress=[&](const std::vector<uint8_t>&input,std::vector<uint8_t>&output,size_t index){
        if(model){PackedFrame packed=model->compress_best(input,level);output=std::move(packed.bytes);frames.dictionary[index]=packed.dictionary;}
        else output=codec.compress(input,level);
    };
    compress(raw.basis_control,frames.basis_control,0);compress(raw.basis_data,frames.basis_data,1);compress(raw.definition_control,frames.definition_control,2);compress(raw.definition_data,frames.definition_data,3);
    const uint64_t split_wire=frames.basis_control.size()+frames.basis_data.size()+frames.definition_control.size()+frames.definition_data.size()+16+(model?4:0);
    std::vector<uint8_t>combined_raw;put_varint(combined_raw,raw.basis_control.size());put_varint(combined_raw,raw.basis_data.size());put_varint(combined_raw,raw.definition_control.size());put_varint(combined_raw,raw.definition_data.size());combined_raw.insert(combined_raw.end(),raw.basis_control.begin(),raw.basis_control.end());combined_raw.insert(combined_raw.end(),raw.basis_data.begin(),raw.basis_data.end());combined_raw.insert(combined_raw.end(),raw.definition_control.begin(),raw.definition_control.end());combined_raw.insert(combined_raw.end(),raw.definition_data.begin(),raw.definition_data.end());
    uint64_t combined_wire=0;if(model){PackedFrame packed=model->compress_best(combined_raw,level);frames.combined_frame=std::move(packed.bytes);frames.combined_dictionary=packed.dictionary;combined_wire=frames.combined_frame.size()+5;}else {frames.combined_frame=codec.compress(combined_raw,level);combined_wire=frames.combined_frame.size()+4;}
    frames.combined=combined_wire<split_wire;frames.wire=std::min(split_wire,combined_wire)+1;
    return frames;
}

static std::vector<uint8_t>decompress_source_pack_frame(const std::vector<uint8_t>&frame,bool dictionary,
                                                        FrameCodec&codec,ModelFrameCodec*model){
    if(dictionary){if(!model)die_msg("source-pack dictionary unavailable");PackedFrame packed;packed.bytes=frame;packed.dictionary=true;return model->decompress(packed);}
    return codec.decompress(frame);
}

static std::array<std::vector<uint8_t>,4>decompress_source_pack_frames(const SourcePackFrames&frames,FrameCodec&codec,ModelFrameCodec*model){
    std::array<std::vector<uint8_t>,4>raw;if(!frames.combined){raw[0]=decompress_source_pack_frame(frames.basis_control,frames.dictionary[0],codec,model);raw[1]=decompress_source_pack_frame(frames.basis_data,frames.dictionary[1],codec,model);raw[2]=decompress_source_pack_frame(frames.definition_control,frames.dictionary[2],codec,model);raw[3]=decompress_source_pack_frame(frames.definition_data,frames.dictionary[3],codec,model);return raw;}
    std::vector<uint8_t>packed;if(frames.combined_dictionary){if(!model)die_msg("source-pack combined dictionary unavailable");PackedFrame frame;frame.bytes=frames.combined_frame;frame.dictionary=true;packed=model->decompress(frame);}else packed=codec.decompress(frames.combined_frame);const uint8_t*p=packed.data(),*end=p+packed.size();std::array<uint64_t,4>lengths{};uint64_t total=0;for(uint64_t&len:lengths){len=get_varint(p,end);if(len>SIZE_MAX-total)die_msg("source-pack combined lengths overflow");total+=len;}if(total!=uint64_t(end-p))die_msg("source-pack combined length mismatch");for(size_t i=0;i<raw.size();++i){raw[i].assign(p,p+lengths[i]);p+=lengths[i];}return raw;
}

static void account_source_pack_layout(SourcePackStats&stats,const SourcePackFrames&frames){
    if(frames.combined){++stats.combined_wins;stats.combined_wire+=frames.combined_frame.size()+4+(frames.combined_dictionary?1:0);return;}
    stats.frame_wire[0]+=frames.basis_control.size()+4+(frames.dictionary[0]?1:0);
    stats.frame_wire[1]+=frames.basis_data.size()+4+(frames.dictionary[1]?1:0);
    stats.frame_wire[2]+=frames.definition_control.size()+4+(frames.dictionary[2]?1:0);
    stats.frame_wire[3]+=frames.definition_data.size()+4+(frames.dictionary[3]?1:0);
}

static void decode_source_pack(const SourcePackFrames&frames,FrameCodec&codec,DecoderStore&store,ModelFrameCodec*model=nullptr,const PersistentDecoder*pretrained=nullptr){
    if(!frames.valid)die_msg("missing source-pack frame");
    if(frames.implicit_order)die_msg("implicit source-pack requires semantic lengths");
    const std::array<std::vector<uint8_t>,4>raw=decompress_source_pack_frames(frames,codec,model);const std::vector<uint8_t>&basis_control=raw[0],&basis_data=raw[1],&definition_control=raw[2],&definition_data=raw[3];
    std::vector<std::vector<uint8_t>>bases=decode_source_pack_bases(frames,basis_control,basis_data,pretrained);const uint8_t*p=definition_control.data(),*end=p+definition_control.size(),*data=definition_data.data(),*data_end=data+definition_data.size();
    const uint64_t record_count=get_varint(p,end);
    for(uint64_t i=0;i<record_count;++i){
        if(p==end)die_msg("missing source-pack mode");
        const uint8_t mode=*p++;const uint32_t id=uint32_t(get_varint(p,end));const uint64_t output_len=get_varint(p,end);
        std::vector<uint8_t>result;result.reserve(size_t(output_len));
        if(mode==0){if(output_len>uint64_t(data_end-data))die_msg("source-pack literal exceeds frame");result.insert(result.end(),data,data+output_len);data+=output_len;}
        else if(mode==1){
            const uint64_t base_id=get_varint(p,end),prefix=get_varint(p,end),suffix=get_varint(p,end);
            if(base_id>=bases.size())die_msg("source-pack base missing");
            const std::vector<uint8_t>&base=bases[size_t(base_id)];
            if(prefix>base.size()||suffix>base.size()-prefix||prefix+suffix>output_len)die_msg("source-pack prefix/suffix range");
            const uint64_t middle=output_len-prefix-suffix;if(middle>uint64_t(data_end-data))die_msg("source-pack residual exceeds frame");
            result.insert(result.end(),base.begin(),base.begin()+prefix);result.insert(result.end(),data,data+middle);data+=middle;result.insert(result.end(),base.end()-suffix,base.end());
        }else die_msg("unknown source-pack mode");
        if(result.size()!=output_len)die_msg("source-pack output length mismatch");
        store.install(id,std::move(result));
    }
    if(p!=end||data!=data_end)die_msg("trailing source-pack definition bytes");
}

static std::vector<uint64_t>semantic_raw_lengths(const std::vector<uint8_t>&control){
    const uint8_t*p=control.data(),*end=p+control.size();const uint64_t rule_count=get_varint(p,end);
    std::vector<uint64_t>rule_slots;rule_slots.reserve(size_t(rule_count));
    for(uint64_t r=0;r<rule_count;++r){const uint64_t slots=get_varint(p,end);if(slots>uint64_t(end-p))die_msg("semantic source rule slots exceed control");p+=slots;rule_slots.push_back(slots);
        const uint64_t occurrences=get_varint(p,end);for(uint64_t i=0;i<occurrences;++i){(void)get_varint(p,end);const uint64_t slot=get_varint(p,end);if(slot>=slots)die_msg("semantic source rule slot out of range");}(void)get_varint(p,end);}
    const uint64_t lexicon_count=get_varint(p,end);for(uint64_t i=0;i<lexicon_count;++i){if(p==end)die_msg("semantic source lexicon type missing");++p;(void)get_varint(p,end);}
    const uint64_t blocks=get_varint(p,end);std::vector<uint64_t>lengths;
    for(uint64_t b=0;b<blocks;++b){if(p==end)die_msg("semantic source record mode missing");const uint8_t mode=*p++;
        if(mode==0){(void)get_zigzag(p,end);lengths.push_back(get_varint(p,end));continue;}
        if(mode!=2)die_msg("unknown semantic source record mode");
        const uint64_t rule_id=get_varint(p,end);if(rule_id>=rule_slots.size())die_msg("semantic source rule missing");const uint64_t rows=get_varint(p,end);
        for(uint64_t row=0;row<rows;++row){(void)get_zigzag(p,end);(void)get_varint(p,end);}
        for(uint64_t slot=0;slot<rule_slots[size_t(rule_id)];++slot)for(uint64_t row=0;row<rows;++row)(void)get_varint(p,end);
    }
    if(p!=end)die_msg("trailing semantic source control bytes");
    return lengths;
}

static std::vector<uint8_t>decode_implicit_source_pack(const SourcePackFrames&frames,FrameCodec&codec,
                                                       const std::vector<uint64_t>&lengths,ModelFrameCodec*model=nullptr,const PersistentDecoder*pretrained=nullptr){
    if(!frames.valid||!frames.implicit_order)die_msg("missing implicit source-pack frame");
    const std::array<std::vector<uint8_t>,4>raw=decompress_source_pack_frames(frames,codec,model);const std::vector<uint8_t>&basis_control=raw[0],&basis_data=raw[1],&definition_control=raw[2],&definition_data=raw[3];
    std::vector<std::vector<uint8_t>>bases=decode_source_pack_bases(frames,basis_control,basis_data,pretrained);const uint8_t*p=definition_control.data(),*end=p+definition_control.size();const uint64_t count=get_varint(p,end);if(count!=lengths.size())die_msg("implicit source record count mismatch");
    const size_t bitmap_bytes=(lengths.size()+7)/8;if(bitmap_bytes>size_t(end-p))die_msg("implicit source bitmap exceeds control");const uint8_t*bitmap=p;p+=bitmap_bytes;
    const uint8_t*data=definition_data.data();const uint8_t*data_end=data+definition_data.size();std::vector<uint8_t>payload;
    uint64_t total=0;for(uint64_t len:lengths){if(len>SIZE_MAX-total)die_msg("implicit source payload too large");total+=len;}payload.reserve(size_t(total));
    for(size_t i=0;i<lengths.size();++i){const uint64_t output_len=lengths[i];const bool source=(bitmap[i/8]>>(i%8))&1u;
        if(!source){if(output_len>uint64_t(data_end-data))die_msg("implicit source literal exceeds frame");payload.insert(payload.end(),data,data+output_len);data+=output_len;continue;}
        const uint64_t base_id=get_varint(p,end),prefix=get_varint(p,end),suffix=get_varint(p,end);if(base_id>=bases.size())die_msg("implicit source base missing");const std::vector<uint8_t>&base=bases[size_t(base_id)];
        if(prefix>base.size()||suffix>base.size()-prefix||prefix+suffix>output_len)die_msg("implicit source prefix/suffix range");
        const uint64_t middle=output_len-prefix-suffix;
        if(middle>uint64_t(data_end-data))die_msg("implicit source residual exceeds frame");
        payload.insert(payload.end(),base.begin(),base.begin()+prefix);payload.insert(payload.end(),data,data+middle);data+=middle;payload.insert(payload.end(),base.end()-suffix,base.end());
    }
    if(p!=end||data!=data_end)die_msg("trailing implicit source definition bytes");
    return payload;
}

// P11: keep only token-aligned source spans that are actually copied into P9's raw-definition
// channel.  Unlike P7p, F never receives a whole source line.  Each TU carries a compact immutable
// span basis, and each selected definition is a sequence of dense span references and exact ADD
// bytes.  The source file is used only by C to discover the program; the decoder consumes these
// four frames and the output lengths already present in P9 semantic control.
struct TokenSpanPlan {
    uint32_t id=0;
    SourceLocation location;
    std::string base;
    DeltaProgram delta;
    uint32_t copy_bytes=0,residual_bytes=0;
    bool available=false;
};

struct TokenSpanOp {
    bool copy=false;
    uint32_t target_off=0,len=0,basis_old_id=0;
};

struct TokenSpanRecord {
    uint32_t id=0;
    uint32_t path_id=0,logical_line=0;
    std::vector<TokenSpanOp>ops;
    bool source=false;
};

struct TokenSpanBasis {
    std::string bytes;
    uint32_t path_id=0,logical_line=0,source_off=0,old_id=0;
};

struct TokenSpanPlanStats {
    uint64_t available_records=0,available_bytes=0,source_records=0,literal_records=0;
    uint64_t basis_records=0,basis_bytes=0,copy_bytes=0,residual_bytes=0,target_bytes=0,program_ops=0;
};

struct TokenSpanRaw {
    std::vector<uint8_t>basis_control,basis_data,definition_control,definition_data;
    TokenSpanPlanStats stats;
    bool explicit_ids=false,param_columnar=false,valid=false;
};

struct TokenSpanFrames {
    std::vector<uint8_t>basis_control,basis_data,definition_control,definition_data;
    std::vector<uint8_t>combined_frame;
    std::array<bool,4>dictionary{};
    bool combined=false,combined_dictionary=false;
    TokenSpanPlanStats stats;
    uint64_t wire=UINT64_MAX;
    unsigned residual_percent=0,max_atoms=0,min_span_bytes=0;
    bool lexical_basis=false,explicit_ids=false,param_columnar=false,valid=false;
};

struct TokenSpanStats {
    TokenSpanPlanStats selected;
    uint64_t candidate_tus=0,frame_wins=0,literal_wire=0,span_wire=0,selected_wire=0;
    std::array<uint64_t,4>frame_wire{};
    uint64_t combined_wins=0,combined_wire=0;
    std::array<uint64_t,6>threshold_wins{};
    std::array<uint64_t,5>atom_wins{};
    std::array<uint64_t,2>order_wins{};
};

static constexpr std::array<unsigned,5>kTokenSpanAtoms={0,2,4,8,16};
static constexpr std::array<unsigned,5>kTokenSpanMinBytes={4,4,6,8,12};
static constexpr size_t kTokenSpanSweepSize=kTokenSpanAtoms.size()*kSourcePackThresholds.size()*2;

struct TokenSpanSweepStats {
    std::array<uint64_t,kTokenSpanSweepSize>wire{},wins{},candidate_tus{},basis_bytes{},copy_bytes{},residual_bytes{},source_records{};
};

static size_t token_span_sweep_index(size_t atom_index,size_t threshold_index,unsigned order){
    return (atom_index*kSourcePackThresholds.size()+threshold_index)*2+order;
}

static std::vector<TokenSpanPlan>build_token_span_plans(const std::vector<uint32_t>&ids,const LineStore&store,
                                                        const std::vector<LineSourceMeta>&meta,
                                                        const std::unordered_map<uint32_t,std::vector<SourceLocation>>&locations,
                                                        SourceCatalog&catalog){
    std::vector<TokenSpanPlan>plans;plans.reserve(ids.size());
    for(uint32_t id:ids){
        TokenSpanPlan best;best.id=id;const uint32_t target_len=store.len(id);
        if(meta[id].marker||meta[id].category!=kProjectCategory){plans.push_back(std::move(best));continue;}
        auto found=locations.find(id);std::vector<SourceLocation>candidates;
        if(found!=locations.end())candidates=found->second;
        if(candidates.empty()&&meta[id].location.valid())candidates.push_back(meta[id].location);
        for(const SourceLocation&location:candidates){
            const uint8_t*base=nullptr;uint32_t base_len=0;if(!catalog.line(location.path_id,location.logical_line,base,base_len))continue;
            DeltaProgram delta=token_delta(0,base,base_len,store.data(id),target_len);if(delta.encoded_size==SIZE_MAX)continue;
            uint32_t copy=0;for(const DeltaOp&op:delta.ops)if(op.copy)copy+=op.len;
            if(!copy)continue;
            const uint32_t residual=target_len-copy;
            if(!best.available||copy>best.copy_bytes||(copy==best.copy_bytes&&delta.ops.size()<best.delta.ops.size())||
               (copy==best.copy_bytes&&delta.ops.size()==best.delta.ops.size()&&std::tie(location.path_id,location.logical_line)<std::tie(best.location.path_id,best.location.logical_line))){
                best.location=location;best.base.assign(reinterpret_cast<const char*>(base),base_len);best.delta=std::move(delta);
                best.copy_bytes=copy;best.residual_bytes=residual;best.available=true;
            }
        }
        plans.push_back(std::move(best));
    }
    return plans;
}

static TokenSpanRaw encode_token_span_pack(const std::vector<TokenSpanPlan>&plans,const LineStore&store,
                                           unsigned residual_percent,unsigned max_atoms,unsigned min_span_bytes,
                                           bool lexical_basis,bool explicit_ids=false){
    TokenSpanRaw raw;raw.explicit_ids=explicit_ids;std::vector<TokenSpanRecord>records;records.reserve(plans.size());
    std::vector<TokenSpanBasis>bases;std::unordered_map<std::string,uint32_t>basis_ids;basis_ids.reserve(plans.size()*2+1);
    auto intern_basis=[&](const uint8_t*p,uint32_t len,const TokenSpanPlan&plan,uint32_t source_off){
        std::string key(reinterpret_cast<const char*>(p),len);auto inserted=basis_ids.emplace(key,uint32_t(bases.size()));
        if(inserted.second){const uint32_t old_id=inserted.first->second;bases.push_back({std::move(key),plan.location.path_id,plan.location.logical_line,source_off,old_id});}
        return inserted.first->second;
    };
    for(const TokenSpanPlan&plan:plans){
        TokenSpanRecord record;record.id=plan.id;record.path_id=plan.location.path_id;record.logical_line=plan.location.logical_line;const uint32_t target_len=store.len(plan.id);raw.stats.target_bytes+=target_len;
        if(plan.available){++raw.stats.available_records;raw.stats.available_bytes+=target_len;}
        uint32_t cursor=0,copy_bytes=0;
        auto append_add=[&](uint32_t off,uint32_t len){if(!len)return;if(!record.ops.empty()&&!record.ops.back().copy&&record.ops.back().target_off+record.ops.back().len==off)record.ops.back().len+=len;else record.ops.push_back({false,off,len,0});};
        auto append_copy=[&](uint32_t target_off,uint32_t source_off,uint32_t len){
            if(len<min_span_bytes){append_add(target_off,len);return;}
            const uint32_t old_id=intern_basis(reinterpret_cast<const uint8_t*>(plan.base.data())+source_off,len,plan,source_off);
            record.ops.push_back({true,target_off,len,old_id});copy_bytes+=len;
        };
        if(plan.available){
            for(const DeltaOp&op:plan.delta.ops){
                if(!op.copy){append_add(cursor,op.len);cursor+=op.len;continue;}
                if(max_atoms==0){append_copy(cursor,op.off,op.len);cursor+=op.len;continue;}
                const uint8_t*span=reinterpret_cast<const uint8_t*>(plan.base.data())+op.off;const std::vector<Atom>atoms=atomize(span,op.len);
                for(size_t begin=0;begin<atoms.size();begin+=max_atoms){const size_t finish=std::min(atoms.size(),begin+max_atoms);
                    const uint32_t local_begin=atoms[begin].off,local_end=atoms[finish-1].off+atoms[finish-1].len,len=local_end-local_begin;
                    append_copy(cursor,op.off+local_begin,len);cursor+=len;
                }
            }
        }
        if(!plan.available)append_add(0,target_len);
        if(cursor!=target_len&&plan.available)die_msg("token-span plan length mismatch");
        const uint32_t residual=target_len-copy_bytes;
        const bool ratio_ok=copy_bytes&&uint64_t(residual)*100<=uint64_t(target_len)*residual_percent;
        if(ratio_ok){record.source=true;++raw.stats.source_records;raw.stats.copy_bytes+=copy_bytes;raw.stats.residual_bytes+=residual;raw.stats.program_ops+=record.ops.size();}
        else {record.ops.clear();record.ops.push_back({false,0,target_len,0});++raw.stats.literal_records;}
        records.push_back(std::move(record));
    }
    // Discard basis entries that were introduced by a record which ultimately selected literal.
    std::unordered_set<uint32_t>used;for(const TokenSpanRecord&record:records)if(record.source)for(const TokenSpanOp&op:record.ops)if(op.copy)used.insert(op.basis_old_id);
    if(used.empty())return raw;
    std::vector<uint32_t>basis_order;basis_order.reserve(used.size());for(uint32_t id:used)basis_order.push_back(id);
    std::sort(basis_order.begin(),basis_order.end(),[&](uint32_t a,uint32_t b){const TokenSpanBasis&x=bases[a],&y=bases[b];
        if(lexical_basis){if(x.bytes!=y.bytes)return x.bytes<y.bytes;}
        else {if(x.path_id!=y.path_id)return x.path_id<y.path_id;if(x.logical_line!=y.logical_line)return x.logical_line<y.logical_line;if(x.source_off!=y.source_off)return x.source_off<y.source_off;}
        return x.bytes<y.bytes;
    });
    std::vector<uint32_t>remap(bases.size(),UINT32_MAX);put_varint(raw.basis_control,basis_order.size());
    for(uint32_t new_id=0;new_id<basis_order.size();++new_id){const TokenSpanBasis&basis=bases[basis_order[new_id]];remap[basis.old_id]=new_id;put_varint(raw.basis_control,basis.bytes.size());raw.basis_data.insert(raw.basis_data.end(),basis.bytes.begin(),basis.bytes.end());raw.stats.basis_bytes+=basis.bytes.size();}
    raw.stats.basis_records=basis_order.size();
    if(explicit_ids)std::sort(records.begin(),records.end(),[&](const TokenSpanRecord&a,const TokenSpanRecord&b){if(a.source!=b.source)return a.source>b.source;if(a.source){if(a.path_id!=b.path_id)return a.path_id<b.path_id;if(a.logical_line!=b.logical_line)return a.logical_line<b.logical_line;}return line_less(a.id,b.id,store);});
    put_varint(raw.definition_control,records.size());if(explicit_ids){int64_t previous_id=0;for(const TokenSpanRecord&record:records){put_zigzag(raw.definition_control,int64_t(record.id)-previous_id);previous_id=record.id;put_varint(raw.definition_control,store.len(record.id));}}
    const size_t bitmap_bytes=(records.size()+7)/8,bitmap_offset=raw.definition_control.size();raw.definition_control.resize(bitmap_offset+bitmap_bytes);
    for(size_t i=0;i<records.size();++i)if(records[i].source)raw.definition_control[bitmap_offset+i/8]|=uint8_t(1u<<(i%8));
    for(const TokenSpanRecord&record:records){
        if(!record.source){const uint32_t len=store.len(record.id);raw.definition_data.insert(raw.definition_data.end(),store.data(record.id),store.data(record.id)+len);continue;}
        for(const TokenSpanOp&op:record.ops){
            if(op.copy){if(op.basis_old_id>=remap.size()||remap[op.basis_old_id]==UINT32_MAX)die_msg("token-span basis remap missing");put_varint(raw.definition_control,(uint64_t(remap[op.basis_old_id])<<1)|1);}
            else {put_varint(raw.definition_control,uint64_t(op.len)<<1);raw.definition_data.insert(raw.definition_data.end(),store.data(record.id)+op.target_off,store.data(record.id)+op.target_off+op.len);}
        }
    }
    raw.valid=true;return raw;
}

static TokenSpanFrames compress_token_span_pack(const TokenSpanRaw&raw,FrameCodec&codec,int level,
                                                 unsigned residual_percent,unsigned max_atoms,unsigned min_span_bytes,
                                                 bool lexical_basis,ModelFrameCodec*model=nullptr){
    TokenSpanFrames frames;if(!raw.valid)return frames;frames.valid=true;frames.stats=raw.stats;frames.residual_percent=residual_percent;
    frames.max_atoms=max_atoms;frames.min_span_bytes=min_span_bytes;frames.lexical_basis=lexical_basis;frames.explicit_ids=raw.explicit_ids;frames.param_columnar=raw.param_columnar;
    auto compress=[&](const std::vector<uint8_t>&input,std::vector<uint8_t>&output,size_t index){
        if(model){PackedFrame packed=model->compress_best(input,level);output=std::move(packed.bytes);frames.dictionary[index]=packed.dictionary;}
        else output=codec.compress(input,level);
    };
    compress(raw.basis_control,frames.basis_control,0);compress(raw.basis_data,frames.basis_data,1);compress(raw.definition_control,frames.definition_control,2);compress(raw.definition_data,frames.definition_data,3);
    const uint64_t split_wire=frames.basis_control.size()+frames.basis_data.size()+frames.definition_control.size()+frames.definition_data.size()+16+(model?4:0);
    std::vector<uint8_t>combined_raw;put_varint(combined_raw,raw.basis_control.size());put_varint(combined_raw,raw.basis_data.size());put_varint(combined_raw,raw.definition_control.size());put_varint(combined_raw,raw.definition_data.size());
    combined_raw.insert(combined_raw.end(),raw.basis_control.begin(),raw.basis_control.end());combined_raw.insert(combined_raw.end(),raw.basis_data.begin(),raw.basis_data.end());combined_raw.insert(combined_raw.end(),raw.definition_control.begin(),raw.definition_control.end());combined_raw.insert(combined_raw.end(),raw.definition_data.begin(),raw.definition_data.end());
    uint64_t combined_wire=0;if(model){PackedFrame packed=model->compress_best(combined_raw,level);frames.combined_frame=std::move(packed.bytes);frames.combined_dictionary=packed.dictionary;combined_wire=frames.combined_frame.size()+5;}
    else {frames.combined_frame=codec.compress(combined_raw,level);combined_wire=frames.combined_frame.size()+4;}
    frames.wire=std::min(split_wire,combined_wire)+1;frames.combined=combined_wire<split_wire;
    return frames;
}

static std::array<std::vector<uint8_t>,4>decompress_token_span_frames(const TokenSpanFrames&frames,FrameCodec&codec,ModelFrameCodec*model){
    std::array<std::vector<uint8_t>,4>raw;
    if(!frames.combined){raw[0]=decompress_source_pack_frame(frames.basis_control,frames.dictionary[0],codec,model);raw[1]=decompress_source_pack_frame(frames.basis_data,frames.dictionary[1],codec,model);raw[2]=decompress_source_pack_frame(frames.definition_control,frames.dictionary[2],codec,model);raw[3]=decompress_source_pack_frame(frames.definition_data,frames.dictionary[3],codec,model);return raw;}
    std::vector<uint8_t>packed;if(frames.combined_dictionary){if(!model)die_msg("token-span combined dictionary unavailable");PackedFrame frame;frame.bytes=frames.combined_frame;frame.dictionary=true;packed=model->decompress(frame);}else packed=codec.decompress(frames.combined_frame);
    const uint8_t*p=packed.data(),*end=p+packed.size();std::array<uint64_t,4>lengths{};uint64_t total=0;for(uint64_t&len:lengths){len=get_varint(p,end);if(len>SIZE_MAX-total)die_msg("token-span combined lengths overflow");total+=len;}if(total!=uint64_t(end-p))die_msg("token-span combined length mismatch");
    for(size_t i=0;i<raw.size();++i){raw[i].assign(p,p+lengths[i]);p+=lengths[i];}return raw;
}

static std::vector<uint8_t>decode_implicit_token_span_pack(const TokenSpanFrames&frames,FrameCodec&codec,
                                                           const std::vector<uint64_t>&lengths,ModelFrameCodec*model=nullptr){
    if(!frames.valid||frames.explicit_ids)die_msg("missing implicit token-span frame");
    const std::array<std::vector<uint8_t>,4>raw=decompress_token_span_frames(frames,codec,model);const std::vector<uint8_t>&basis_control=raw[0],&basis_data=raw[1],&definition_control=raw[2],&definition_data=raw[3];
    const uint8_t*p=basis_control.data(),*end=p+basis_control.size(),*data=basis_data.data(),*data_end=data+basis_data.size();const uint64_t basis_count=get_varint(p,end);std::vector<std::vector<uint8_t>>bases;bases.reserve(size_t(basis_count));
    for(uint64_t i=0;i<basis_count;++i){const uint64_t len=get_varint(p,end);if(len>uint64_t(data_end-data))die_msg("token-span basis exceeds frame");bases.emplace_back(data,data+len);data+=len;}
    if(p!=end||data!=data_end)die_msg("trailing token-span basis bytes");
    p=definition_control.data();end=p+definition_control.size();const uint64_t count=get_varint(p,end);if(count!=lengths.size())die_msg("token-span record count mismatch");const size_t bitmap_bytes=(lengths.size()+7)/8;if(bitmap_bytes>size_t(end-p))die_msg("token-span bitmap exceeds control");const uint8_t*bitmap=p;p+=bitmap_bytes;
    data=definition_data.data();data_end=data+definition_data.size();std::vector<uint8_t>payload;uint64_t total=0;for(uint64_t len:lengths){if(len>SIZE_MAX-total)die_msg("token-span payload too large");total+=len;}payload.reserve(size_t(total));
    for(size_t i=0;i<lengths.size();++i){const uint64_t output_len=lengths[i];const bool source=(bitmap[i/8]>>(i%8))&1u;
        if(!source){if(output_len>uint64_t(data_end-data))die_msg("token-span literal exceeds frame");payload.insert(payload.end(),data,data+output_len);data+=output_len;continue;}
        const size_t output_begin=payload.size();while(payload.size()-output_begin<output_len){const uint64_t code=get_varint(p,end);
            if(code&1){const uint64_t id=code>>1;if(id>=bases.size())die_msg("token-span basis missing");const std::vector<uint8_t>&basis=bases[size_t(id)];if(basis.size()>output_len-(payload.size()-output_begin))die_msg("token-span COPY exceeds output");payload.insert(payload.end(),basis.begin(),basis.end());}
            else {const uint64_t len=code>>1;if(!len||len>output_len-(payload.size()-output_begin)||len>uint64_t(data_end-data))die_msg("token-span ADD exceeds output");payload.insert(payload.end(),data,data+len);data+=len;}
        }
    }
    if(p!=end||data!=data_end)die_msg("trailing token-span definition bytes");
    return payload;
}

static void decode_explicit_token_span_pack(const TokenSpanFrames&frames,FrameCodec&codec,DecoderStore&store,ModelFrameCodec*model=nullptr){
    if(!frames.valid||!frames.explicit_ids)die_msg("missing explicit token-span frame");
    const std::array<std::vector<uint8_t>,4>raw=decompress_token_span_frames(frames,codec,model);const std::vector<uint8_t>&basis_control=raw[0],&basis_data=raw[1],&definition_control=raw[2],&definition_data=raw[3];
    const uint8_t*p=basis_control.data(),*end=p+basis_control.size(),*data=basis_data.data(),*data_end=data+basis_data.size();const uint64_t basis_count=get_varint(p,end);std::vector<std::vector<uint8_t>>bases;bases.reserve(size_t(basis_count));
    for(uint64_t i=0;i<basis_count;++i){const uint64_t len=get_varint(p,end);if(len>uint64_t(data_end-data))die_msg("explicit token-span basis exceeds frame");bases.emplace_back(data,data+len);data+=len;}
    if(p!=end||data!=data_end)die_msg("trailing explicit token-span basis bytes");
    p=definition_control.data();end=p+definition_control.size();const uint64_t count=get_varint(p,end);std::vector<uint32_t>ids(static_cast<size_t>(count),0);std::vector<uint64_t>lengths(static_cast<size_t>(count),0);int64_t previous_id=0;
    for(size_t i=0;i<size_t(count);++i){previous_id+=get_zigzag(p,end);if(previous_id<=0||previous_id>UINT32_MAX)die_msg("explicit token-span id out of range");ids[i]=uint32_t(previous_id);lengths[i]=get_varint(p,end);}
    const size_t bitmap_bytes=(size_t(count)+7)/8;if(bitmap_bytes>size_t(end-p))die_msg("explicit token-span bitmap exceeds control");const uint8_t*bitmap=p;p+=bitmap_bytes;data=definition_data.data();data_end=data+definition_data.size();
    for(size_t i=0;i<size_t(count);++i){const uint64_t output_len=lengths[i];const bool source=(bitmap[i/8]>>(i%8))&1u;std::vector<uint8_t>result;result.reserve(size_t(output_len));
        if(!source){if(output_len>uint64_t(data_end-data))die_msg("explicit token-span literal exceeds frame");result.insert(result.end(),data,data+output_len);data+=output_len;store.install(ids[i],std::move(result));continue;}
        while(result.size()<output_len){const uint64_t code=get_varint(p,end);if(code&1){const uint64_t id=code>>1;if(id>=bases.size())die_msg("explicit token-span basis missing");const std::vector<uint8_t>&basis=bases[size_t(id)];if(basis.size()>output_len-result.size())die_msg("explicit token-span COPY exceeds output");result.insert(result.end(),basis.begin(),basis.end());}
            else {const uint64_t len=code>>1;if(!len||len>output_len-result.size()||len>uint64_t(data_end-data))die_msg("explicit token-span ADD exceeds output");result.insert(result.end(),data,data+len);data+=len;}}
        store.install(ids[i],std::move(result));
    }
    if(p!=end||data!=data_end)die_msg("trailing explicit token-span definition bytes");
}

// P12: TU-local parameterized token superblocks.  Rules are alpha-normalized windows rather than
// complete Lines, so a unique definition can still reuse several exact structural fragments.  A
// rule carries literal segments and equality-constrained typed slots; an instance carries each
// distinct slot spelling once.  The complete TU is known before rule selection, but no rule becomes
// persistent until a later explicit online-learning row.
struct ParamSpanCandidate {
    std::string key;
    ParsedLine shape;
    uint64_t count=0,literal_bytes=0,instance_bytes=0;
    int64_t benefit=0;
};

struct ParamSpanOp {
    bool rule=false;
    uint32_t target_off=0,len=0,rule_id=0,instance_id=0;
    std::vector<std::vector<uint8_t>>values;
};

struct ParamSpanRecord {
    uint32_t id=0;
    std::vector<ParamSpanOp>ops;
    bool program=false;
};

struct ParamSpanStats {
    uint64_t candidate_shapes=0,selected_rules=0,used_rules=0,rule_instances=0,unique_instances=0;
    uint64_t program_records=0,literal_records=0,covered_bytes=0,residual_bytes=0,target_bytes=0,program_ops=0;
    uint64_t lexicon_entries=0,lexicon_references=0;
};

static constexpr std::array<unsigned,4>kParamSpanWidths={4,8,16,32};
static constexpr std::array<unsigned,6>kPretrainedParamSpanWidths={4,8,16,32,64,128};

static void observe_param_span_windows(const uint8_t*p,uint32_t n,
                                       std::unordered_map<std::string,ParamSpanCandidate>&candidates){
    const std::vector<Atom>atoms=atomize(p,n);if(atoms.size()>256)return;
    for(size_t begin=0;begin<atoms.size();++begin){for(unsigned width:kParamSpanWidths){if(begin+width>atoms.size())continue;const uint32_t off=atoms[begin].off,end=atoms[begin+width-1].off+atoms[begin+width-1].len,len=end-off;
            ParsedLine parsed=parse_line(p+off,len);if(parsed.values.empty()||parsed.values.size()>32||parsed.occurrence_slot.size()>64||parsed.key.size()>2048)continue;
            size_t instance=1;for(const auto&value:parsed.values)instance+=varint_size(value.size())+value.size();auto inserted=candidates.emplace(parsed.key,ParamSpanCandidate{});ParamSpanCandidate&candidate=inserted.first->second;
            if(inserted.second){candidate.key=parsed.key;candidate.shape=std::move(parsed);}++candidate.count;candidate.literal_bytes+=len;candidate.instance_bytes+=instance;
        }}
}

static TokenSpanRaw encode_param_span_pack(const std::vector<uint32_t>&ids,const LineStore&store,size_t rule_cap,bool columnar,ParamSpanStats&stats){
    std::unordered_map<std::string,ParamSpanCandidate>candidates;candidates.reserve(ids.size()*8+1);
    for(uint32_t id:ids)observe_param_span_windows(store.data(id),store.len(id),candidates);
    stats.candidate_shapes=candidates.size();std::vector<ParamSpanCandidate*>ranked;ranked.reserve(candidates.size());
    for(auto&item:candidates){ParamSpanCandidate&candidate=item.second;if(candidate.count<2)continue;const uint64_t definition=encoded_template_size(candidate.shape)+varint_size(candidate.count);
        if(candidate.literal_bytes<=candidate.instance_bytes+definition)continue;
        candidate.benefit=int64_t(candidate.literal_bytes-candidate.instance_bytes-definition);ranked.push_back(&candidate);}
    std::sort(ranked.begin(),ranked.end(),[](const ParamSpanCandidate*a,const ParamSpanCandidate*b){if(a->benefit!=b->benefit)return a->benefit>b->benefit;if(a->count!=b->count)return a->count>b->count;return a->key<b->key;});if(ranked.size()>rule_cap)ranked.resize(rule_cap);stats.selected_rules=ranked.size();
    std::unordered_map<std::string,uint32_t>rule_ids;rule_ids.reserve(ranked.size()*2+1);for(uint32_t i=0;i<ranked.size();++i)rule_ids.emplace(ranked[i]->key,i);

    std::vector<ParamSpanRecord>records;records.reserve(ids.size());std::vector<uint64_t>rule_uses(ranked.size());
    for(uint32_t id:ids){const uint8_t*p=store.data(id);const uint32_t n=store.len(id);stats.target_bytes+=n;ParamSpanRecord record;record.id=id;const std::vector<Atom>atoms=atomize(p,n);
        if(atoms.empty()||atoms.size()>256||ranked.empty()){++stats.literal_records;records.push_back(std::move(record));continue;}
        const size_t count=atoms.size();std::vector<size_t>cost(count+1,SIZE_MAX);std::vector<uint32_t>previous(count+1),edge_rule(count+1,UINT32_MAX);cost[0]=0;
        for(size_t begin=0;begin<count;++begin){if(cost[begin]==SIZE_MAX)continue;const size_t raw_cost=cost[begin]+varint_size(uint64_t(atoms[begin].len)<<1)+atoms[begin].len;if(raw_cost<cost[begin+1]){cost[begin+1]=raw_cost;previous[begin+1]=uint32_t(begin);edge_rule[begin+1]=UINT32_MAX;}
            for(unsigned width:kParamSpanWidths){if(begin+width>count)continue;const uint32_t off=atoms[begin].off,end=atoms[begin+width-1].off+atoms[begin+width-1].len,len=end-off;ParsedLine parsed=parse_line(p+off,len);auto found=rule_ids.find(parsed.key);if(found==rule_ids.end())continue;
                const ParamSpanCandidate&candidate=*ranked[found->second];size_t edge=varint_size((uint64_t(found->second)<<1)|1)+encoded_template_size(candidate.shape)/candidate.count;for(const auto&value:parsed.values)edge+=varint_size(value.size())+value.size();const size_t next=begin+width;
                if(cost[begin]+edge<cost[next]){cost[next]=cost[begin]+edge;previous[next]=uint32_t(begin);edge_rule[next]=found->second;}}
        }
        struct Choice{uint32_t begin=0,end=0,rule=UINT32_MAX;};std::vector<Choice>choices;for(uint32_t end=uint32_t(count);end;){const uint32_t begin=previous[end];choices.push_back({begin,end,edge_rule[end]});end=begin;}std::reverse(choices.begin(),choices.end());
        auto append_raw=[&](uint32_t off,uint32_t len){if(!len)return;if(!record.ops.empty()&&!record.ops.back().rule&&record.ops.back().target_off+record.ops.back().len==off)record.ops.back().len+=len;else {ParamSpanOp op;op.target_off=off;op.len=len;record.ops.push_back(std::move(op));}};
        bool has_rule=false;for(const Choice&choice:choices){const uint32_t off=atoms[choice.begin].off,end=atoms[choice.end-1].off+atoms[choice.end-1].len,len=end-off;if(choice.rule==UINT32_MAX){append_raw(off,len);continue;}ParsedLine parsed=parse_line(p+off,len);ParamSpanOp op;op.rule=true;op.target_off=off;op.len=len;op.rule_id=choice.rule;op.values=std::move(parsed.values);record.ops.push_back(std::move(op));has_rule=true;}
        size_t encoded=0;for(const ParamSpanOp&op:record.ops){if(op.rule){encoded+=varint_size((uint64_t(op.rule_id)<<1)|1);for(const auto&value:op.values)encoded+=varint_size(value.size())+value.size();}else encoded+=varint_size(uint64_t(op.len)<<1)+op.len;}
        if(!has_rule||encoded>=n){record.ops.clear();++stats.literal_records;}
        else record.program=true;
        records.push_back(std::move(record));
    }

    // Candidate windows overlap.  Charge definitions against the uses that survive the line DP,
    // convert losing rules back to raw spans, and repeat so one-use remnants cannot remain.
    for(unsigned iteration=0;iteration<3;++iteration){std::vector<uint64_t>uses(ranked.size()),literal(ranked.size()),instance(ranked.size());
        for(const ParamSpanRecord&record:records)if(record.program)for(const ParamSpanOp&op:record.ops)if(op.rule){++uses[op.rule_id];literal[op.rule_id]+=op.len;instance[op.rule_id]+=varint_size((uint64_t(op.rule_id)<<1)|1);for(const auto&value:op.values)instance[op.rule_id]+=varint_size(value.size())+value.size();}
        std::vector<uint8_t>keep(ranked.size());for(size_t i=0;i<ranked.size();++i){const uint64_t definition=encoded_template_size(ranked[i]->shape);keep[i]=uses[i]>=2&&literal[i]>instance[i]+definition;}
        bool changed=false;for(ParamSpanRecord&record:records)if(record.program){std::vector<ParamSpanOp>next;auto append_raw=[&](uint32_t off,uint32_t len){if(!len)return;if(!next.empty()&&!next.back().rule&&next.back().target_off+next.back().len==off)next.back().len+=len;else {ParamSpanOp op;op.target_off=off;op.len=len;next.push_back(std::move(op));}};bool has_rule=false;
                for(ParamSpanOp&op:record.ops){if(op.rule&&!keep[op.rule_id]){append_raw(op.target_off,op.len);changed=true;}else if(!op.rule)append_raw(op.target_off,op.len);else {has_rule=true;next.push_back(std::move(op));}}
                size_t encoded=0;for(const ParamSpanOp&op:next){if(op.rule){encoded+=varint_size((uint64_t(op.rule_id)<<1)|1);for(const auto&value:op.values)encoded+=varint_size(value.size())+value.size();}else encoded+=varint_size(uint64_t(op.len)<<1)+op.len;}
                if(!has_rule||encoded>=store.len(record.id)){record.program=false;record.ops.clear();changed=true;}else record.ops=std::move(next);
            }
        if(!changed)break;
    }
    std::fill(rule_uses.begin(),rule_uses.end(),0);stats.program_records=0;stats.literal_records=0;stats.rule_instances=0;stats.covered_bytes=0;stats.residual_bytes=0;stats.program_ops=0;
    for(const ParamSpanRecord&record:records){if(!record.program){++stats.literal_records;continue;}++stats.program_records;stats.program_ops+=record.ops.size();for(const ParamSpanOp&op:record.ops){if(op.rule){++rule_uses[op.rule_id];++stats.rule_instances;stats.covered_bytes+=op.len;}else stats.residual_bytes+=op.len;}}
    std::vector<uint32_t>used;for(uint32_t i=0;i<rule_uses.size();++i)if(rule_uses[i])used.push_back(i);if(used.empty())return {};
    struct ParamInstanceTable{std::unordered_map<std::string,uint32_t>id;std::vector<std::vector<std::vector<uint8_t>>>rows;};std::vector<ParamInstanceTable>instances(ranked.size());
    if(columnar)for(ParamSpanRecord&record:records)if(record.program)for(ParamSpanOp&op:record.ops)if(op.rule){std::string key;for(const auto&value:op.values){key_varint(key,value.size());key.append(reinterpret_cast<const char*>(value.data()),value.size());}ParamInstanceTable&table=instances[op.rule_id];auto inserted=table.id.emplace(std::move(key),uint32_t(table.rows.size()));if(inserted.second)table.rows.push_back(op.values);op.instance_id=inserted.first->second;}
    std::vector<uint32_t>remap(ranked.size(),UINT32_MAX);TokenSpanRaw raw;raw.valid=true;raw.param_columnar=columnar;raw.stats.target_bytes=stats.target_bytes;raw.stats.source_records=stats.program_records;raw.stats.literal_records=stats.literal_records;raw.stats.copy_bytes=stats.covered_bytes;raw.stats.residual_bytes=stats.residual_bytes;raw.stats.program_ops=stats.program_ops;raw.stats.available_records=stats.program_records;stats.used_rules=used.size();raw.stats.basis_records=used.size();put_varint(raw.basis_control,used.size());
    for(uint32_t new_id=0;new_id<used.size();++new_id){const uint32_t old_id=used[new_id];remap[old_id]=new_id;const ParsedLine&shape=ranked[old_id]->shape;put_varint(raw.basis_control,shape.values.size());raw.basis_control.insert(raw.basis_control.end(),shape.slot_type.begin(),shape.slot_type.end());put_varint(raw.basis_control,shape.occurrence_slot.size());for(size_t i=0;i<shape.occurrence_slot.size();++i){put_varint(raw.basis_control,shape.literals[i].size());raw.basis_data.insert(raw.basis_data.end(),shape.literals[i].begin(),shape.literals[i].end());raw.stats.basis_bytes+=shape.literals[i].size();put_varint(raw.basis_control,shape.occurrence_slot[i]);}put_varint(raw.basis_control,shape.literals.back().size());raw.basis_data.insert(raw.basis_data.end(),shape.literals.back().begin(),shape.literals.back().end());raw.stats.basis_bytes+=shape.literals.back().size();const ParamInstanceTable&table=instances[old_id];put_varint(raw.basis_control,columnar?table.rows.size():0);if(columnar){stats.unique_instances+=table.rows.size();for(size_t slot=0;slot<shape.values.size();++slot)for(const auto&row:table.rows){const auto&value=row[slot];put_varint(raw.basis_control,value.size());raw.basis_data.insert(raw.basis_data.end(),value.begin(),value.end());raw.stats.basis_bytes+=value.size();}}}
    put_varint(raw.definition_control,records.size());const size_t bitmap_bytes=(records.size()+7)/8,bitmap_offset=raw.definition_control.size();raw.definition_control.resize(bitmap_offset+bitmap_bytes);for(size_t i=0;i<records.size();++i)if(records[i].program)raw.definition_control[bitmap_offset+i/8]|=uint8_t(1u<<(i%8));
    for(const ParamSpanRecord&record:records){if(!record.program){const uint32_t len=store.len(record.id);raw.definition_data.insert(raw.definition_data.end(),store.data(record.id),store.data(record.id)+len);continue;}for(const ParamSpanOp&op:record.ops){if(!op.rule){put_varint(raw.definition_control,uint64_t(op.len)<<1);raw.definition_data.insert(raw.definition_data.end(),store.data(record.id)+op.target_off,store.data(record.id)+op.target_off+op.len);continue;}if(op.rule_id>=remap.size()||remap[op.rule_id]==UINT32_MAX)die_msg("parameterized span rule remap missing");put_varint(raw.definition_control,(uint64_t(remap[op.rule_id])<<1)|1);if(columnar)put_varint(raw.definition_control,op.instance_id);else for(const auto&value:op.values){put_varint(raw.definition_control,value.size());raw.definition_data.insert(raw.definition_data.end(),value.begin(),value.end());}}}
    return raw;
}

static std::vector<uint8_t>decode_implicit_param_span_pack(const TokenSpanFrames&frames,FrameCodec&codec,const std::vector<uint64_t>&lengths){
    if(!frames.valid||frames.explicit_ids)die_msg("missing parameterized-span frame");
    const std::array<std::vector<uint8_t>,4>raw=decompress_token_span_frames(frames,codec,nullptr);const std::vector<uint8_t>&rule_control=raw[0],&rule_data=raw[1],&definition_control=raw[2],&definition_data=raw[3];
    struct DecodedParamRule{DecodedTemplate shape;std::vector<std::vector<std::vector<uint8_t>>>instances;};
    const uint8_t*p=rule_control.data(),*end=p+rule_control.size(),*data=rule_data.data(),*data_end=data+rule_data.size();const uint64_t rule_count=get_varint(p,end);std::vector<DecodedParamRule>rules;rules.reserve(size_t(rule_count));
    for(uint64_t r=0;r<rule_count;++r){DecodedParamRule decoded;DecodedTemplate&rule=decoded.shape;const uint64_t slots=get_varint(p,end);if(slots>uint64_t(end-p))die_msg("parameterized span slots exceed control");rule.slot_type.assign(p,p+slots);p+=slots;const uint64_t occurrences=get_varint(p,end);for(uint64_t i=0;i<occurrences;++i){const uint64_t len=get_varint(p,end);if(len>uint64_t(data_end-data))die_msg("parameterized span literal exceeds data");rule.literals.emplace_back(data,data+len);data+=len;const uint64_t slot=get_varint(p,end);if(slot>=slots)die_msg("parameterized span slot out of range");rule.occurrence_slot.push_back(uint32_t(slot));}const uint64_t len=get_varint(p,end);if(len>uint64_t(data_end-data))die_msg("parameterized span tail exceeds data");rule.literals.emplace_back(data,data+len);data+=len;const uint64_t instance_count=get_varint(p,end);if(instance_count>SIZE_MAX)die_msg("parameterized span instance count too large");decoded.instances.resize(size_t(instance_count),std::vector<std::vector<uint8_t>>(size_t(slots)));for(size_t slot=0;slot<size_t(slots);++slot)for(size_t row=0;row<size_t(instance_count);++row){const uint64_t value_len=get_varint(p,end);if(value_len>uint64_t(data_end-data))die_msg("parameterized span instance exceeds data");decoded.instances[row][slot].assign(data,data+value_len);data+=value_len;}rules.push_back(std::move(decoded));}
    if(p!=end||data!=data_end)die_msg("trailing parameterized span rule bytes");
    p=definition_control.data();end=p+definition_control.size();const uint64_t count=get_varint(p,end);if(count!=lengths.size())die_msg("parameterized span record count mismatch");const size_t bitmap_bytes=(lengths.size()+7)/8;if(bitmap_bytes>size_t(end-p))die_msg("parameterized span bitmap exceeds control");const uint8_t*bitmap=p;p+=bitmap_bytes;data=definition_data.data();data_end=data+definition_data.size();std::vector<uint8_t>payload;uint64_t total=0;for(uint64_t len:lengths){if(len>SIZE_MAX-total)die_msg("parameterized span payload too large");total+=len;}payload.reserve(size_t(total));
    for(size_t row=0;row<lengths.size();++row){const uint64_t output_len=lengths[row];const bool program=(bitmap[row/8]>>(row%8))&1u;if(!program){if(output_len>uint64_t(data_end-data))die_msg("parameterized span raw exceeds data");payload.insert(payload.end(),data,data+output_len);data+=output_len;continue;}const size_t output_begin=payload.size();
        while(payload.size()-output_begin<output_len){const uint64_t code=get_varint(p,end);if(!(code&1)){const uint64_t len=code>>1;if(!len||len>output_len-(payload.size()-output_begin)||len>uint64_t(data_end-data))die_msg("parameterized span ADD exceeds output");payload.insert(payload.end(),data,data+len);data+=len;continue;}const uint64_t rule_id=code>>1;if(rule_id>=rules.size())die_msg("parameterized span rule missing");const DecodedParamRule&decoded=rules[size_t(rule_id)];const DecodedTemplate&rule=decoded.shape;std::vector<std::vector<uint8_t>>inline_values;const std::vector<std::vector<uint8_t>>*values=nullptr;
            if(frames.param_columnar){const uint64_t instance_id=get_varint(p,end);if(instance_id>=decoded.instances.size())die_msg("parameterized span instance missing");values=&decoded.instances[size_t(instance_id)];}
            else {inline_values.resize(rule.slot_type.size());for(auto&value:inline_values){const uint64_t len=get_varint(p,end);if(len>uint64_t(data_end-data))die_msg("parameterized span inline value exceeds data");value.assign(data,data+len);data+=len;}values=&inline_values;}
            for(size_t i=0;i<rule.occurrence_slot.size();++i){payload.insert(payload.end(),rule.literals[i].begin(),rule.literals[i].end());const std::vector<uint8_t>&value=(*values)[rule.occurrence_slot[i]];payload.insert(payload.end(),value.begin(),value.end());}payload.insert(payload.end(),rule.literals.back().begin(),rule.literals.back().end());if(payload.size()-output_begin>output_len)die_msg("parameterized span rule exceeds output");}
    }
    if(p!=end||data!=data_end)die_msg("trailing parameterized span definition bytes");
    return payload;
}

struct ByteSpan {
    uint32_t off=0,len=0;
};

// Conservative lexical statement boundaries.  These are coding units, not a C++ parse tree:
// every byte belongs to exactly one unit and any uncertain construct can only reduce matching.
// Semicolons inside parentheses (notably for-loops) do not split.  Braces and top-level
// semicolons split immediately, directives split at their physical line end, and unusually large
// units are cut at a newline so planning remains bounded.
static std::vector<ByteSpan>split_cpp_statement_units(const uint8_t*p,uint32_t n){
    enum class State:uint8_t{Normal,LineComment,BlockComment,SingleQuote,DoubleQuote};
    std::vector<ByteSpan>units;uint32_t begin=0,i=0;unsigned paren=0,bracket=0;
    State state=State::Normal;bool escaped=false,at_line_start=true,directive=false;
    auto finish=[&](uint32_t end){if(end>begin)units.push_back({begin,end-begin});begin=end;directive=false;};
    while(i<n){const uint8_t c=p[i];
        if(state==State::LineComment){if(c=='\n'){++i;state=State::Normal;
                if(directive||i-begin>=8192)finish(i);
                at_line_start=true;
            }else ++i;continue;}
        if(state==State::BlockComment){if(c=='*'&&i+1<n&&p[i+1]=='/'){i+=2;state=State::Normal;}
            else{if(c=='\n')at_line_start=true;++i;}continue;}
        if(state==State::SingleQuote||state==State::DoubleQuote){const uint8_t quote=state==State::SingleQuote?'\'':'"';++i;
            if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c==quote)state=State::Normal;
            if(c=='\n')at_line_start=true;
            continue;}
        if(c=='/'&&i+1<n&&p[i+1]=='/'){i+=2;state=State::LineComment;continue;}
        if(c=='/'&&i+1<n&&p[i+1]=='*'){i+=2;state=State::BlockComment;continue;}
        if(c=='\''||c=='"'){state=c=='\''?State::SingleQuote:State::DoubleQuote;escaped=false;++i;at_line_start=false;continue;}
        if(c=='\n'){const bool continued=i>0&&p[i-1]=='\\';++i;
            if((directive&&!continued)||i-begin>=8192)finish(i);
            at_line_start=true;continue;}
        if(at_line_start){if(c==' '||c=='\t'||c=='\r'||c=='\f'||c=='\v'){++i;continue;}
            directive=c=='#';at_line_start=false;}
        if(directive){++i;continue;}
        if(c=='(')++paren;else if(c==')'&&paren)--paren;else if(c=='[')++bracket;else if(c==']'&&bracket)--bracket;
        ++i;
        if(((c==';'&&paren==0&&bracket==0)||((c=='{'||c=='}')&&paren==0&&bracket==0)))finish(i);
    }
    finish(n);return units;
}

static constexpr uint8_t kParamModelTokenWindows=0;
static constexpr uint8_t kParamModelStatements=1;

// P14: a frozen cross-project package of parameterized token sub-superblocks.  P8 learns complete
// Line shapes; this model learns reusable 4/8/16/32-atom shapes and can therefore cover fragments
// inside otherwise unique P9 literal definitions.  Training is deterministically sampled by exact
// Line content so model construction is insensitive to manifest order.  The package is rebuilt by
// an independent receiver and is also offered as optional zstd history by the ordinary frame codec.
struct PretrainedParamCandidate {
    ParsedLine shape;
    uint64_t count=0,literal_bytes=0,instance_bytes=0,corpus_mask=0;
};

struct PretrainedParamPackage {
    std::unordered_map<std::string,uint32_t>rule_id;
    std::vector<ParsedLine>encoder_rules;
    std::vector<DecodedTemplate>decoder_rules;
    std::vector<uint8_t>raw,frame;
    uint64_t training_raw=0,training_lines=0,training_distinct_lines=0,sampled_lines=0;
    uint64_t candidate_windows=0,candidate_rules=0;
    double training_seconds=0;
    uint8_t unit_mode=kParamModelTokenWindows;
};

static void decode_pretrained_param_package(const std::vector<uint8_t>&raw,
                                             std::vector<DecodedTemplate>&rules,
                                             uint8_t*unit_mode=nullptr){
    const uint8_t*p=raw.data(),*end=p+raw.size();
    const uint64_t version=get_varint(p,end);uint8_t mode=kParamModelTokenWindows;
    if(version==2){const uint64_t encoded_mode=get_varint(p,end);if(encoded_mode>kParamModelStatements)die_msg("unknown pretrained parameterized unit mode");mode=uint8_t(encoded_mode);}
    else if(version!=1)die_msg("unknown pretrained parameterized package version");
    if(unit_mode)*unit_mode=mode;
    const uint64_t count=get_varint(p,end);rules.clear();rules.reserve(size_t(count));
    for(uint64_t r=0;r<count;++r){DecodedTemplate rule;const uint64_t slots=get_varint(p,end);
        if(slots>uint64_t(end-p))die_msg("pretrained parameterized slots exceed package");
        rule.slot_type.assign(p,p+slots);p+=slots;const uint64_t occurrences=get_varint(p,end);
        rule.literals.reserve(size_t(occurrences)+1);rule.occurrence_slot.reserve(size_t(occurrences));
        for(uint64_t i=0;i<occurrences;++i){const uint64_t len=get_varint(p,end);
            if(len>uint64_t(end-p))die_msg("pretrained parameterized literal exceeds package");
            rule.literals.emplace_back(p,p+len);p+=len;const uint64_t slot=get_varint(p,end);
            if(slot>=slots)die_msg("pretrained parameterized slot out of range");
            rule.occurrence_slot.push_back(uint32_t(slot));}
        const uint64_t len=get_varint(p,end);
        if(len>uint64_t(end-p))die_msg("pretrained parameterized tail exceeds package");
        rule.literals.emplace_back(p,p+len);p+=len;rules.push_back(std::move(rule));}
    if(p!=end)die_msg("trailing pretrained parameterized package bytes");
}

static PretrainedParamPackage load_pretrained_param_package(const std::string&path){
    PretrainedParamPackage package;package.frame=load_file_bytes(path);
    if(package.frame.empty())die_msg("empty pretrained parameterized package frame");
    FrameCodec codec;package.raw=codec.decompress(package.frame);
    if(package.raw.size()>UINT32_MAX)die_msg("pretrained parameterized package exceeds 4 GiB");
    decode_pretrained_param_package(package.raw,package.decoder_rules,&package.unit_mode);
    package.encoder_rules.reserve(package.decoder_rules.size());
    package.rule_id.reserve(package.decoder_rules.size()*2+1);
    for(const DecodedTemplate&decoded:package.decoder_rules){ParsedLine rule;
        rule.literals=decoded.literals;rule.occurrence_slot=decoded.occurrence_slot;rule.slot_type=decoded.slot_type;
        for(uint8_t type:rule.slot_type)if(type<kIdentifier||type>kString)die_msg("unknown pretrained parameterized slot type");
        rule.values.resize(rule.slot_type.size());build_parsed_keys(rule);
        const uint32_t id=uint32_t(package.encoder_rules.size());
        if(!package.rule_id.emplace(rule.key,id).second)die_msg("duplicate pretrained parameterized rule");
        package.encoder_rules.push_back(std::move(rule));
    }
    std::vector<uint8_t>rebuilt;
    if(package.unit_mode==kParamModelStatements){put_varint(rebuilt,2);put_varint(rebuilt,package.unit_mode);}
    else put_varint(rebuilt,1);
    put_varint(rebuilt,package.encoder_rules.size());
    for(const ParsedLine&rule:package.encoder_rules)serialize_pretrained_rule(rebuilt,rule);
    if(rebuilt!=package.raw)die_msg("pretrained parameterized package is not canonical");
    return package;
}

class PretrainedParamBuilder {
public:
    explicit PretrainedParamBuilder(bool coarse_lines=false,bool statements=false):
        coarse_lines_(coarse_lines),statements_(statements){}

    void scan_manifest(const std::string&manifest,size_t max_files,size_t corpus_index){
        if(corpus_index>=64)die_msg("at most 64 parameterized training corpora are supported");
        LineStore distinct;FILE*mf=std::fopen(manifest.c_str(),"r");if(!mf)die(manifest.c_str());
        char path[16384];size_t files=0;
        while(files<max_files&&std::fgets(path,sizeof(path),mf)){size_t pn=std::strlen(path);
            while(pn&&(path[pn-1]=='\n'||path[pn-1]=='\r'))path[--pn]=0;
            if(!pn)continue;
            scan_file(path,distinct,corpus_index);++files;
        }
        std::fclose(mf);
    }

    void scan_source_root(const std::string&root,size_t max_files,size_t corpus_index){
        if(corpus_index>=64)die_msg("at most 64 parameterized training corpora are supported");
        namespace fs=std::filesystem;std::vector<std::string>paths;std::error_code error;
        fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied,error),end;
        if(error)die_msg("cannot traverse parameterized source-training root");
        for(;it!=end;it.increment(error)){if(error){error.clear();continue;}const fs::directory_entry&entry=*it;
            if(entry.is_directory(error)){const std::string name=entry.path().filename().string();
                if(name==".git"||name=="build"||name=="CMakeFiles"||name=="cmake-build-debug"||name=="cmake-build-release")it.disable_recursion_pending();
                error.clear();continue;}
            if(!entry.is_regular_file(error)){error.clear();continue;}error.clear();
            const std::string extension=entry.path().extension().string();
            if(extension==".c"||extension==".cc"||extension==".cpp"||extension==".cxx"||extension==".C"||
               extension==".h"||extension==".hh"||extension==".hpp"||extension==".hxx"||extension==".inc"||
               extension==".inl"||extension==".ipp"||extension==".tcc"||extension==".def"||extension==".cu"||extension==".cuh")
                paths.push_back(entry.path().string());}
        std::sort(paths.begin(),paths.end());if(paths.size()>max_files)paths.resize(max_files);
        LineStore distinct;for(const std::string&path:paths)scan_file(path,distinct,corpus_index);
    }

    PretrainedParamPackage compile(size_t budget,int zlevel,double seconds,unsigned min_corpora){
        if(budget<4096)die_msg("pretrained parameterized package budget must be at least 4 KiB");
        struct Ranked{const std::string*key=nullptr;PretrainedParamCandidate*candidate=nullptr;uint64_t cost=0,benefit=0;};
        std::vector<Ranked>ranked;ranked.reserve(candidates_.size());
        for(auto&item:candidates_){PretrainedParamCandidate&candidate=item.second;
            const unsigned corpora=unsigned(__builtin_popcountll(candidate.corpus_mask));
            const uint64_t cost=encoded_template_size(candidate.shape)+2;
            if(corpora<min_corpora||candidate.count<2||candidate.literal_bytes<=candidate.instance_bytes+cost)continue;
            ranked.push_back({&item.first,&candidate,cost,candidate.literal_bytes-candidate.instance_bytes-cost});}
        std::sort(ranked.begin(),ranked.end(),[](const Ranked&a,const Ranked&b){
            const __uint128_t lhs=__uint128_t(a.benefit)*b.cost,rhs=__uint128_t(b.benefit)*a.cost;
            if(lhs!=rhs)return lhs>rhs;
            if(a.benefit!=b.benefit)return a.benefit>b.benefit;
            return *a.key<*b.key;});
        PretrainedParamPackage package;package.training_raw=training_raw_;package.training_lines=training_lines_;
        package.training_distinct_lines=training_distinct_lines_;package.sampled_lines=sampled_lines_;
        package.candidate_windows=candidate_windows_;package.candidate_rules=candidates_.size();package.training_seconds=seconds;
        package.unit_mode=statements_?kParamModelStatements:kParamModelTokenWindows;
        const size_t usable=budget-64;size_t used=0;
        for(const Ranked&entry:ranked){if(entry.cost>usable-used)continue;
            const uint32_t id=uint32_t(package.encoder_rules.size());package.rule_id.emplace(*entry.key,id);
            package.encoder_rules.push_back(entry.candidate->shape);used+=entry.cost;}
        if(statements_){put_varint(package.raw,2);put_varint(package.raw,package.unit_mode);}
        else put_varint(package.raw,1);
        put_varint(package.raw,package.encoder_rules.size());
        for(const ParsedLine&rule:package.encoder_rules)serialize_pretrained_rule(package.raw,rule);
        if(package.raw.size()>budget)die_msg("pretrained parameterized package exceeded explicit budget");
        FrameCodec codec;package.frame=codec.compress(package.raw,zlevel);
        const std::vector<uint8_t>decoded=codec.decompress(package.frame);
        if(decoded!=package.raw)die_msg("pretrained parameterized package frame mismatch");
        decode_pretrained_param_package(decoded,package.decoder_rules);
        if(package.decoder_rules.size()!=package.encoder_rules.size())die_msg("pretrained parameterized package cardinality mismatch");
        return package;
    }
private:
    void observe_statement_candidate(const uint8_t*p,uint32_t len,LineStore&distinct,size_t corpus_index){
        if(len<8||len>16384)return;
        // Sample before interning so the exact set retained during a large source scan remains
        // bounded.  The decision depends only on candidate bytes, never traversal order.
        if(((hash_bytes(p,len)>>1)&31u)!=0)return;
        const LineStore::Result seen=distinct.intern(p,len);if(!seen.first)return;
        ++training_distinct_lines_;++sampled_lines_;
        ParsedLine parsed=parse_line(p,len,false,true);
        if(parsed.values.empty()||parsed.values.size()>128||parsed.occurrence_slot.size()>256||parsed.key.size()>16384)return;
        ++candidate_windows_;size_t instance=1;
        for(const auto&value:parsed.values)instance+=varint_size(value.size())+value.size();
        auto inserted=candidates_.try_emplace(parsed.key);PretrainedParamCandidate&candidate=inserted.first->second;
        if(inserted.second)candidate.shape=std::move(parsed);
        ++candidate.count;candidate.literal_bytes+=len;candidate.instance_bytes+=instance;
        candidate.corpus_mask|=uint64_t(1)<<corpus_index;
    }

    void scan_statement_file(const uint8_t*p,uint32_t len,LineStore&distinct,size_t corpus_index){
        training_lines_+=std::count(p,p+len,uint8_t('\n'))+(len&&p[len-1]!='\n'?1:0);
        const std::vector<ByteSpan>units=split_cpp_statement_units(p,len);
        static constexpr std::array<size_t,3>widths={1,2,4};
        for(size_t begin=0;begin<units.size();++begin)for(size_t width:widths){if(begin+width>units.size())continue;
            const uint32_t off=units[begin].off;const ByteSpan&last=units[begin+width-1];const uint32_t finish=last.off+last.len;
            observe_statement_candidate(p+off,finish-off,distinct,corpus_index);}
    }

    void scan_file(const std::string&path,LineStore&distinct,size_t corpus_index){
        FILE*f=std::fopen(path.c_str(),"rb");if(!f)die(path.c_str());struct stat st{};
        if(::fstat(::fileno(f),&st)!=0)die(path.c_str());
        if(st.st_size<0||uint64_t(st.st_size)>UINT32_MAX)die_msg("unsupported parameterized training file size");
        std::vector<uint8_t>bytes(size_t(st.st_size)+8);
        if(st.st_size&&std::fread(bytes.data(),1,size_t(st.st_size),f)!=size_t(st.st_size))die_msg("short parameterized training read");
        std::fclose(f);training_raw_+=uint64_t(st.st_size);
        if(statements_){scan_statement_file(bytes.data(),uint32_t(st.st_size),distinct,corpus_index);return;}
        const uint8_t*p=bytes.data(),*end=p+st.st_size;
        while(p<end){const void*hit=std::memchr(p,'\n',size_t(end-p));
            const uint8_t*line_end=hit?static_cast<const uint8_t*>(hit)+1:end;
            const uint32_t len=uint32_t(line_end-p);++training_lines_;
            const LineStore::Result seen=distinct.intern(p,len);
            if(seen.first){++training_distinct_lines_;
                // A fixed 1/32 content sample bounds training work without making it dependent
                // on file or repository order.  High-frequency generic shapes remain observable.
                if(((hash_bytes(p,len)>>1)&31u)==0&&len<=4096){++sampled_lines_;
                    const std::vector<Atom>atoms=atomize(p,len);
                    if(atoms.size()<=256){auto observe=[&](size_t begin,size_t width){
                            const uint32_t off=atoms[begin].off;
                            const uint32_t finish=atoms[begin+width-1].off+atoms[begin+width-1].len;
                            const uint32_t span_len=finish-off;ParsedLine parsed=parse_line(p+off,span_len);
                            if(parsed.values.empty()||parsed.values.size()>32||parsed.occurrence_slot.size()>64||parsed.key.size()>2048)return;
                            ++candidate_windows_;size_t instance=1;
                            for(const auto&value:parsed.values)instance+=varint_size(value.size())+value.size();
                            auto inserted=candidates_.try_emplace(parsed.key);PretrainedParamCandidate&candidate=inserted.first->second;
                            if(inserted.second)candidate.shape=std::move(parsed);
                            ++candidate.count;candidate.literal_bytes+=span_len;candidate.instance_bytes+=instance;
                            candidate.corpus_mask|=uint64_t(1)<<corpus_index;
                        };
                        if(coarse_lines_){
                            for(size_t begin=0;begin<atoms.size();++begin)for(unsigned width:kPretrainedParamSpanWidths)
                                if(begin+width<=atoms.size())observe(begin,width);
                            if(atoms.size()>=4&&std::find(kPretrainedParamSpanWidths.begin(),kPretrainedParamSpanWidths.end(),unsigned(atoms.size()))==kPretrainedParamSpanWidths.end())
                                observe(0,atoms.size());
                        }else for(size_t begin=0;begin<atoms.size();++begin)for(unsigned width:kParamSpanWidths)
                            if(begin+width<=atoms.size())observe(begin,width);}
                }
            }
            p=line_end;
        }
    }
    std::unordered_map<std::string,PretrainedParamCandidate>candidates_;
    uint64_t training_raw_=0,training_lines_=0,training_distinct_lines_=0,sampled_lines_=0,candidate_windows_=0;
    bool coarse_lines_=false;
    bool statements_=false;
};

static TokenSpanRaw encode_frozen_param_span_pack(const std::vector<uint32_t>&ids,const LineStore&store,
                                                   const PretrainedParamPackage&model,bool columnar,
                                                   ParamSpanStats&stats,std::vector<ParamSpanRecord>*planned_records=nullptr){
    std::vector<ParamSpanRecord>records;records.reserve(ids.size());std::vector<uint64_t>rule_uses(model.encoder_rules.size());
    for(uint32_t id:ids){const uint8_t*p=store.data(id);const uint32_t n=store.len(id);stats.target_bytes+=n;
        ParamSpanRecord record;record.id=id;const std::vector<Atom>atoms=atomize(p,n);
        if(atoms.empty()||atoms.size()>256||model.encoder_rules.empty()){++stats.literal_records;records.push_back(std::move(record));continue;}
        const size_t count=atoms.size();std::vector<size_t>cost(count+1,SIZE_MAX);std::vector<uint32_t>previous(count+1),edge_rule(count+1,UINT32_MAX);cost[0]=0;
        for(size_t begin=0;begin<count;++begin){if(cost[begin]==SIZE_MAX)continue;
            const size_t raw_cost=cost[begin]+varint_size(uint64_t(atoms[begin].len)<<1)+atoms[begin].len;
            if(raw_cost<cost[begin+1]){cost[begin+1]=raw_cost;previous[begin+1]=uint32_t(begin);edge_rule[begin+1]=UINT32_MAX;}
            auto consider=[&](size_t next){const uint32_t off=atoms[begin].off;
                const uint32_t finish=atoms[next-1].off+atoms[next-1].len;ParsedLine parsed=parse_line(p+off,finish-off);
                auto found=model.rule_id.find(parsed.key);if(found==model.rule_id.end())return;size_t edge=varint_size((uint64_t(found->second)<<1)|1);
                for(const auto&value:parsed.values)edge+=varint_size(value.size())+value.size();
                if(cost[begin]+edge<cost[next]){cost[next]=cost[begin]+edge;previous[next]=uint32_t(begin);edge_rule[next]=found->second;}};
            for(unsigned width:kPretrainedParamSpanWidths)if(begin+width<=count)consider(begin+width);
            if(begin==0&&count>=4&&std::find(kPretrainedParamSpanWidths.begin(),kPretrainedParamSpanWidths.end(),unsigned(count))==kPretrainedParamSpanWidths.end())consider(count);
        }
        struct Choice{uint32_t begin=0,end=0,rule=UINT32_MAX;};std::vector<Choice>choices;
        for(uint32_t finish=uint32_t(count);finish;){const uint32_t begin=previous[finish];choices.push_back({begin,finish,edge_rule[finish]});finish=begin;}
        std::reverse(choices.begin(),choices.end());
        auto append_raw=[&](uint32_t off,uint32_t len){if(!len)return;
            if(!record.ops.empty()&&!record.ops.back().rule&&record.ops.back().target_off+record.ops.back().len==off)record.ops.back().len+=len;
            else{ParamSpanOp op;op.target_off=off;op.len=len;record.ops.push_back(std::move(op));}};
        bool has_rule=false;
        for(const Choice&choice:choices){const uint32_t off=atoms[choice.begin].off;
            const uint32_t finish=atoms[choice.end-1].off+atoms[choice.end-1].len;const uint32_t len=finish-off;
            if(choice.rule==UINT32_MAX){append_raw(off,len);continue;}ParsedLine parsed=parse_line(p+off,len);
            ParamSpanOp op;op.rule=true;op.target_off=off;op.len=len;op.rule_id=choice.rule;op.values=std::move(parsed.values);
            record.ops.push_back(std::move(op));has_rule=true;}
        size_t encoded=0;for(const ParamSpanOp&op:record.ops){if(op.rule){encoded+=varint_size((uint64_t(op.rule_id)<<1)|1);
                for(const auto&value:op.values)encoded+=varint_size(value.size())+value.size();}
            else encoded+=varint_size(uint64_t(op.len)<<1)+op.len;}
        if(!has_rule||encoded>=n){record.ops.clear();++stats.literal_records;}else record.program=true;
        records.push_back(std::move(record));
    }
    stats.program_records=0;stats.literal_records=0;stats.rule_instances=0;stats.covered_bytes=0;stats.residual_bytes=0;stats.program_ops=0;
    for(const ParamSpanRecord&record:records){if(!record.program){++stats.literal_records;continue;}++stats.program_records;
        stats.program_ops+=record.ops.size();for(const ParamSpanOp&op:record.ops){if(op.rule){++rule_uses[op.rule_id];++stats.rule_instances;stats.covered_bytes+=op.len;}
            else stats.residual_bytes+=op.len;}}
    if(planned_records)*planned_records=records;
    std::vector<uint32_t>used;for(uint32_t i=0;i<rule_uses.size();++i)if(rule_uses[i])used.push_back(i);
    if(used.empty())return {};
    stats.used_rules=used.size();
    std::unordered_map<std::string,uint32_t>value_frequency;
    for(const ParamSpanRecord&record:records)if(record.program)for(const ParamSpanOp&op:record.ops)if(op.rule){
        const ParsedLine&shape=model.encoder_rules[op.rule_id];
        for(size_t slot=0;slot<op.values.size();++slot){std::string key;key.reserve(op.values[slot].size()+1);
            key.push_back(char(shape.slot_type[slot]));
            key.append(reinterpret_cast<const char*>(op.values[slot].data()),op.values[slot].size());++value_frequency[key];}}
    struct LexiconValue{uint8_t type=0;std::string value;uint32_t count=0;};std::vector<LexiconValue>lexicon;
    for(const auto&item:value_frequency){const size_t len=item.first.size()-1;
        const uint64_t inline_cost=uint64_t(item.second)*(varint_size((uint64_t(len)<<1)|1)+len);
        const uint64_t dictionary_cost=1+varint_size(len)+len+uint64_t(item.second)*2;
        if(item.second>=2&&dictionary_cost<inline_cost)lexicon.push_back({uint8_t(item.first[0]),item.first.substr(1),item.second});}
    std::sort(lexicon.begin(),lexicon.end(),[](const LexiconValue&a,const LexiconValue&b){
        if(a.count!=b.count)return a.count>b.count;
        if(a.type!=b.type)return a.type<b.type;
        return a.value<b.value;});
    std::unordered_map<std::string,uint32_t>lexicon_id;lexicon_id.reserve(lexicon.size()*2+1);
    for(uint32_t i=0;i<lexicon.size();++i){std::string key;key.reserve(lexicon[i].value.size()+1);key.push_back(char(lexicon[i].type));key+=lexicon[i].value;lexicon_id.emplace(std::move(key),i);}
    stats.lexicon_entries=lexicon.size();
    struct InstanceTable{std::unordered_map<std::string,uint32_t>id;std::vector<std::vector<std::vector<uint8_t>>>rows;};
    std::vector<InstanceTable>instances(model.encoder_rules.size());
    if(columnar)for(ParamSpanRecord&record:records)if(record.program)for(ParamSpanOp&op:record.ops)if(op.rule){std::string key;
        for(const auto&value:op.values){key_varint(key,value.size());key.append(reinterpret_cast<const char*>(value.data()),value.size());}
        InstanceTable&table=instances[op.rule_id];auto inserted=table.id.emplace(std::move(key),uint32_t(table.rows.size()));
        if(inserted.second)table.rows.push_back(op.values);
        op.instance_id=inserted.first->second;}
    std::vector<uint32_t>remap(model.encoder_rules.size(),UINT32_MAX);TokenSpanRaw raw;raw.valid=true;raw.param_columnar=columnar;
    raw.stats.target_bytes=stats.target_bytes;raw.stats.source_records=stats.program_records;raw.stats.literal_records=stats.literal_records;
    raw.stats.copy_bytes=stats.covered_bytes;raw.stats.residual_bytes=stats.residual_bytes;raw.stats.program_ops=stats.program_ops;
    raw.stats.available_records=stats.program_records;raw.stats.basis_records=used.size();
    put_varint(raw.basis_control,lexicon.size());
    for(const LexiconValue&entry:lexicon){raw.basis_control.push_back(entry.type);put_varint(raw.basis_control,entry.value.size());
        raw.basis_data.insert(raw.basis_data.end(),entry.value.begin(),entry.value.end());raw.stats.basis_bytes+=entry.value.size();}
    auto emit_value=[&](std::vector<uint8_t>&control,uint8_t type,const std::vector<uint8_t>&value){std::string key;
        key.reserve(value.size()+1);key.push_back(char(type));key.append(reinterpret_cast<const char*>(value.data()),value.size());
        auto found=lexicon_id.find(key);if(found!=lexicon_id.end()){put_varint(control,uint64_t(found->second)<<1);++stats.lexicon_references;}
        else{put_varint(control,(uint64_t(value.size())<<1)|1);raw.basis_data.insert(raw.basis_data.end(),value.begin(),value.end());raw.stats.basis_bytes+=value.size();}};
    put_varint(raw.basis_control,used.size());
    for(uint32_t local_id=0;local_id<used.size();++local_id){const uint32_t rule_id=used[local_id];remap[rule_id]=local_id;
        const ParsedLine&shape=model.encoder_rules[rule_id];const InstanceTable&table=instances[rule_id];put_varint(raw.basis_control,rule_id);
        put_varint(raw.basis_control,columnar?table.rows.size():0);if(columnar)stats.unique_instances+=table.rows.size();
        if(columnar)for(size_t slot=0;slot<shape.values.size();++slot)for(const auto&row:table.rows)emit_value(raw.basis_control,shape.slot_type[slot],row[slot]);}
    put_varint(raw.definition_control,records.size());const size_t bitmap_bytes=(records.size()+7)/8,bitmap_offset=raw.definition_control.size();
    raw.definition_control.resize(bitmap_offset+bitmap_bytes);
    for(size_t i=0;i<records.size();++i)if(records[i].program)raw.definition_control[bitmap_offset+i/8]|=uint8_t(1u<<(i%8));
    for(const ParamSpanRecord&record:records){if(!record.program){const uint32_t len=store.len(record.id);
            raw.definition_data.insert(raw.definition_data.end(),store.data(record.id),store.data(record.id)+len);continue;}
        for(const ParamSpanOp&op:record.ops){if(!op.rule){put_varint(raw.definition_control,uint64_t(op.len)<<1);
                raw.definition_data.insert(raw.definition_data.end(),store.data(record.id)+op.target_off,store.data(record.id)+op.target_off+op.len);continue;}
            const uint32_t wire_rule=remap[op.rule_id];
            if(wire_rule==UINT32_MAX)die_msg("frozen parameterized rule remap missing");
            put_varint(raw.definition_control,(uint64_t(wire_rule)<<1)|1);
            if(columnar)put_varint(raw.definition_control,op.instance_id);
            else {const ParsedLine&shape=model.encoder_rules[op.rule_id];
                for(size_t slot=0;slot<op.values.size();++slot)emit_value(raw.definition_control,shape.slot_type[slot],op.values[slot]);}}}
    return raw;
}

static std::vector<uint8_t>decode_frozen_param_span_pack(const TokenSpanFrames&frames,FrameCodec&codec,
                                                         ModelFrameCodec&frame_codec,
                                                         const std::vector<DecodedTemplate>&model_rules,
                                                         const std::vector<uint64_t>&lengths){
    if(!frames.valid||frames.explicit_ids)die_msg("missing frozen parameterized-span frame");
    const std::array<std::vector<uint8_t>,4>raw=decompress_token_span_frames(frames,codec,&frame_codec);
    const std::vector<uint8_t>&table_control=raw[0],&table_data=raw[1],&definition_control=raw[2],&definition_data=raw[3];
    struct LexiconValue{uint8_t type=0;std::vector<uint8_t>value;};
    struct Table{uint32_t rule_id=0;std::vector<std::vector<std::vector<uint8_t>>>rows;};
    const uint8_t*p=table_control.data(),*end=p+table_control.size();
    const uint8_t*slot_data=table_data.data(),*slot_end=slot_data+table_data.size();
    const uint64_t lexicon_count=get_varint(p,end);std::vector<LexiconValue>lexicon;lexicon.reserve(size_t(lexicon_count));
    for(uint64_t i=0;i<lexicon_count;++i){if(p==end)die_msg("frozen parameterized lexicon type missing");const uint8_t type=*p++;
        const uint64_t len=get_varint(p,end);if(len>uint64_t(slot_end-slot_data))die_msg("frozen parameterized lexicon value exceeds data");
        lexicon.push_back({type,std::vector<uint8_t>(slot_data,slot_data+len)});slot_data+=len;}
    auto read_value=[&](const uint8_t*&control,const uint8_t*control_end,uint8_t expected_type){const uint64_t code=get_varint(control,control_end);
        if(!(code&1)){const uint64_t id=code>>1;if(id>=lexicon.size())die_msg("frozen parameterized lexicon id missing");
            if(lexicon[size_t(id)].type!=expected_type)die_msg("frozen parameterized lexicon type mismatch");
            return lexicon[size_t(id)].value;}
        const uint64_t len=code>>1;if(len>uint64_t(slot_end-slot_data))die_msg("frozen parameterized slot value exceeds data");
        std::vector<uint8_t>value(slot_data,slot_data+len);slot_data+=len;return value;};
    const uint64_t table_count=get_varint(p,end);std::vector<Table>tables;tables.reserve(size_t(table_count));
    for(uint64_t t=0;t<table_count;++t){Table table;table.rule_id=uint32_t(get_varint(p,end));
        if(table.rule_id>=model_rules.size())die_msg("frozen parameterized table rule missing");
        const DecodedTemplate&rule=model_rules[table.rule_id];
        const uint64_t row_count=get_varint(p,end);if(row_count>SIZE_MAX)die_msg("frozen parameterized row count too large");
        table.rows.resize(size_t(row_count),std::vector<std::vector<uint8_t>>(rule.slot_type.size()));
        for(size_t slot=0;slot<rule.slot_type.size();++slot)for(size_t row=0;row<size_t(row_count);++row)
            table.rows[row][slot]=read_value(p,end,rule.slot_type[slot]);
        tables.push_back(std::move(table));}
    if(p!=end)die_msg("trailing frozen parameterized table control bytes");
    p=definition_control.data();end=p+definition_control.size();const uint64_t count=get_varint(p,end);
    if(count!=lengths.size())die_msg("frozen parameterized record count mismatch");
    const size_t bitmap_bytes=(lengths.size()+7)/8;
    if(bitmap_bytes>size_t(end-p))die_msg("frozen parameterized bitmap exceeds control");
    const uint8_t*bitmap=p;p+=bitmap_bytes;
    const uint8_t*literal_data=definition_data.data(),*literal_end=literal_data+definition_data.size();std::vector<uint8_t>payload;uint64_t total=0;
    for(uint64_t len:lengths){if(len>SIZE_MAX-total)die_msg("frozen parameterized payload too large");total+=len;}payload.reserve(size_t(total));
    for(size_t row=0;row<lengths.size();++row){const uint64_t output_len=lengths[row];const bool program=(bitmap[row/8]>>(row%8))&1u;
        if(!program){if(output_len>uint64_t(literal_end-literal_data))die_msg("frozen parameterized raw exceeds data");payload.insert(payload.end(),literal_data,literal_data+output_len);literal_data+=output_len;continue;}
        const size_t output_begin=payload.size();while(payload.size()-output_begin<output_len){const uint64_t code=get_varint(p,end);
            if(!(code&1)){const uint64_t len=code>>1;if(!len||len>output_len-(payload.size()-output_begin)||len>uint64_t(literal_end-literal_data))die_msg("frozen parameterized ADD exceeds output");
                payload.insert(payload.end(),literal_data,literal_data+len);literal_data+=len;continue;}
            const uint64_t wire_rule=code>>1;uint32_t rule_id=0;const std::vector<std::vector<uint8_t>>*values=nullptr;
            std::vector<std::vector<uint8_t>>inline_values;
            if(frames.param_columnar){if(wire_rule>=tables.size())die_msg("frozen parameterized table missing");const Table&table=tables[size_t(wire_rule)];
                rule_id=table.rule_id;const uint64_t instance_id=get_varint(p,end);if(instance_id>=table.rows.size())die_msg("frozen parameterized instance missing");values=&table.rows[size_t(instance_id)];}
            else {if(wire_rule>=tables.size())die_msg("frozen parameterized inline rule table missing");rule_id=tables[size_t(wire_rule)].rule_id;
                const DecodedTemplate&rule=model_rules[rule_id];inline_values.resize(rule.slot_type.size());
                for(size_t slot=0;slot<inline_values.size();++slot)inline_values[slot]=read_value(p,end,rule.slot_type[slot]);
                values=&inline_values;}
            const DecodedTemplate&rule=model_rules[rule_id];
            for(size_t i=0;i<rule.occurrence_slot.size();++i){payload.insert(payload.end(),rule.literals[i].begin(),rule.literals[i].end());
                const std::vector<uint8_t>&value=(*values)[rule.occurrence_slot[i]];payload.insert(payload.end(),value.begin(),value.end());}
            payload.insert(payload.end(),rule.literals.back().begin(),rule.literals.back().end());
            if(payload.size()-output_begin>output_len)die_msg("frozen parameterized rule exceeds output");}
    }
    if(p!=end||slot_data!=slot_end||literal_data!=literal_end)die_msg("trailing frozen parameterized definition bytes");
    return payload;
}

struct FrozenSemanticParamRaw {
    std::vector<uint8_t>control,residual;
    std::array<std::vector<uint8_t>,3>slot_data;
    uint64_t lexicon_entries=0,lexicon_references=0;
    bool valid=false;
};

static size_t frozen_slot_index(uint8_t type){
    if(type<kIdentifier||type>kString)die_msg("frozen semantic slot type out of range");
    return size_t(type-kIdentifier);
}

struct StatementProgramStats {
    uint64_t units=0,candidate_windows=0,matched_windows=0,rule_ops=0,raw_ops=0;
    uint64_t covered_bytes=0,residual_bytes=0,used_rules=0,lexicon_entries=0,lexicon_references=0;
};

struct StatementChoice {
    bool rule=false;
    uint32_t off=0,len=0,rule_id=0,instance_id=0;
    std::vector<std::vector<uint8_t>>values;
};

// P18: apply a portable statement/multi-statement package to the complete P9 raw-definition
// payload.  Unlike P14, one operation may span several physical Line definitions.  P9 already
// carries their exact lengths and IDs, so the independent decoder reconstructs this byte stream
// first and then lets the ordinary semantic decoder split/install it.
static FrozenSemanticParamRaw encode_frozen_statement_pack(const std::vector<uint8_t>&input,
                                                             const PretrainedParamPackage&model,
                                                             bool columnar,
                                                             StatementProgramStats&stats){
    FrozenSemanticParamRaw raw;if(input.empty()||model.encoder_rules.empty())return raw;
    const std::vector<ByteSpan>units=split_cpp_statement_units(input.data(),uint32_t(input.size()));
    stats.units+=units.size();if(units.empty())return raw;
    const size_t count=units.size();std::vector<size_t>cost(count+1,SIZE_MAX);
    std::vector<uint32_t>previous(count+1),edge_rule(count+1,UINT32_MAX);cost[0]=0;
    static constexpr std::array<size_t,3>widths={1,2,4};
    for(size_t begin=0;begin<count;++begin){if(cost[begin]==SIZE_MAX)continue;
        const size_t raw_cost=cost[begin]+varint_size(uint64_t(units[begin].len)<<1)+units[begin].len;
        if(raw_cost<cost[begin+1]){cost[begin+1]=raw_cost;previous[begin+1]=uint32_t(begin);edge_rule[begin+1]=UINT32_MAX;}
        for(size_t width:widths){if(begin+width>count)continue;++stats.candidate_windows;
            const uint32_t off=units[begin].off;const ByteSpan&last=units[begin+width-1];const uint32_t finish=last.off+last.len;
            ParsedLine parsed=parse_line(input.data()+off,finish-off,false,true);auto found=model.rule_id.find(parsed.key);
            if(found==model.rule_id.end())continue;
            ++stats.matched_windows;size_t edge=varint_size((uint64_t(found->second)<<1)|1);
            for(const auto&value:parsed.values)edge+=varint_size(value.size())+value.size();
            const size_t next=begin+width;
            if(cost[begin]+edge<cost[next]){cost[next]=cost[begin]+edge;previous[next]=uint32_t(begin);edge_rule[next]=found->second;}}
    }
    std::vector<StatementChoice>choices;
    for(uint32_t finish=uint32_t(count);finish;){const uint32_t begin=previous[finish];const uint32_t off=units[begin].off;
        const ByteSpan&last=units[finish-1];const uint32_t span_finish=last.off+last.len;const uint32_t len=span_finish-off;
        StatementChoice choice;choice.off=off;choice.len=len;choice.rule_id=edge_rule[finish];choice.rule=choice.rule_id!=UINT32_MAX;
        if(choice.rule)choice.values=parse_line(input.data()+off,len,false,true).values;
        choices.push_back(std::move(choice));finish=begin;}
    std::reverse(choices.begin(),choices.end());
    std::vector<StatementChoice>merged;for(StatementChoice&choice:choices){if(!choice.rule&&!merged.empty()&&!merged.back().rule&&merged.back().off+merged.back().len==choice.off)merged.back().len+=choice.len;
        else merged.push_back(std::move(choice));}
    choices=std::move(merged);std::vector<uint64_t>uses(model.encoder_rules.size());bool any_rule=false;
    for(const StatementChoice&choice:choices){if(choice.rule){++uses[choice.rule_id];++stats.rule_ops;stats.covered_bytes+=choice.len;any_rule=true;}
        else{++stats.raw_ops;stats.residual_bytes+=choice.len;}}
    if(!any_rule)return raw;
    std::vector<uint32_t>used;for(uint32_t i=0;i<uses.size();++i)if(uses[i])used.push_back(i);stats.used_rules+=used.size();
    struct InstanceTable{std::unordered_map<std::string,uint32_t>id;std::vector<std::vector<std::vector<uint8_t>>>rows;};
    std::vector<InstanceTable>instances(model.encoder_rules.size());
    for(StatementChoice&choice:choices)if(choice.rule){std::string key;for(const auto&value:choice.values){key_varint(key,value.size());key.append(reinterpret_cast<const char*>(value.data()),value.size());}
        InstanceTable&table=instances[choice.rule_id];auto inserted=table.id.emplace(std::move(key),uint32_t(table.rows.size()));if(inserted.second)table.rows.push_back(choice.values);choice.instance_id=inserted.first->second;}
    std::vector<uint32_t>remap(model.encoder_rules.size(),UINT32_MAX);std::unordered_map<std::string,uint32_t>frequency;
    auto count_value=[&](uint8_t type,const std::vector<uint8_t>&value){std::string key;key.reserve(value.size()+1);key.push_back(char(type));
        key.append(reinterpret_cast<const char*>(value.data()),value.size());++frequency[key];};
    if(columnar)for(uint32_t rule_id:used){const ParsedLine&shape=model.encoder_rules[rule_id];for(const auto&row:instances[rule_id].rows)
            for(size_t slot=0;slot<row.size();++slot)count_value(shape.slot_type[slot],row[slot]);}
    else for(const StatementChoice&choice:choices)if(choice.rule){const ParsedLine&shape=model.encoder_rules[choice.rule_id];
            for(size_t slot=0;slot<choice.values.size();++slot)count_value(shape.slot_type[slot],choice.values[slot]);}
    struct LexiconValue{uint8_t type=0;std::string value;uint32_t count=0;};std::vector<LexiconValue>lexicon;
    for(const auto&item:frequency){const size_t len=item.first.size()-1;const uint64_t inline_cost=uint64_t(item.second)*(varint_size((uint64_t(len)<<1)|1)+len);
        const uint64_t dictionary_cost=1+varint_size(len)+len+uint64_t(item.second)*2;
        if(item.second>=2&&dictionary_cost<inline_cost)lexicon.push_back({uint8_t(item.first[0]),item.first.substr(1),item.second});}
    std::sort(lexicon.begin(),lexicon.end(),[](const LexiconValue&a,const LexiconValue&b){if(a.count!=b.count)return a.count>b.count;if(a.type!=b.type)return a.type<b.type;return a.value<b.value;});
    std::unordered_map<std::string,uint32_t>lexicon_id;lexicon_id.reserve(lexicon.size()*2+1);
    for(uint32_t i=0;i<lexicon.size();++i){std::string key;key.reserve(lexicon[i].value.size()+1);key.push_back(char(lexicon[i].type));key+=lexicon[i].value;lexicon_id.emplace(std::move(key),i);}
    put_varint(raw.control,columnar?2:1);put_varint(raw.control,lexicon.size());
    for(const LexiconValue&entry:lexicon){raw.control.push_back(entry.type);put_varint(raw.control,entry.value.size());auto&channel=raw.slot_data[frozen_slot_index(entry.type)];channel.insert(channel.end(),entry.value.begin(),entry.value.end());}
    stats.lexicon_entries+=lexicon.size();
    auto emit_value=[&](uint8_t type,const std::vector<uint8_t>&value){std::string key;key.reserve(value.size()+1);key.push_back(char(type));key.append(reinterpret_cast<const char*>(value.data()),value.size());
        auto found=lexicon_id.find(key);if(found!=lexicon_id.end()){put_varint(raw.control,uint64_t(found->second)<<1);++stats.lexicon_references;}
        else{put_varint(raw.control,(uint64_t(value.size())<<1)|1);auto&channel=raw.slot_data[frozen_slot_index(type)];channel.insert(channel.end(),value.begin(),value.end());}};
    put_varint(raw.control,used.size());for(uint32_t local=0;local<used.size();++local){const uint32_t rule_id=used[local];remap[rule_id]=local;put_varint(raw.control,rule_id);
        if(columnar){const ParsedLine&shape=model.encoder_rules[rule_id];const InstanceTable&table=instances[rule_id];put_varint(raw.control,table.rows.size());
            for(size_t slot=0;slot<shape.values.size();++slot)for(const auto&row:table.rows)emit_value(shape.slot_type[slot],row[slot]);}}
    put_varint(raw.control,input.size());
    for(const StatementChoice&choice:choices){if(!choice.rule){put_varint(raw.control,uint64_t(choice.len)<<1);raw.residual.insert(raw.residual.end(),input.begin()+choice.off,input.begin()+choice.off+choice.len);continue;}
        const uint32_t local=remap[choice.rule_id];if(local==UINT32_MAX)die_msg("statement rule remap missing");put_varint(raw.control,(uint64_t(local)<<1)|1);
        if(columnar)put_varint(raw.control,choice.instance_id);else{const ParsedLine&shape=model.encoder_rules[choice.rule_id];
            for(size_t slot=0;slot<choice.values.size();++slot)emit_value(shape.slot_type[slot],choice.values[slot]);}}
    raw.lexicon_entries=stats.lexicon_entries;raw.lexicon_references=stats.lexicon_references;raw.valid=true;return raw;
}

static std::vector<uint8_t>decode_frozen_statement_pack(const FrozenSemanticParamRaw&raw,
                                                         const std::vector<DecodedTemplate>&model_rules){
    if(!raw.valid)die_msg("missing frozen statement program");
    const uint8_t*p=raw.control.data(),*end=p+raw.control.size();
    const uint64_t version=get_varint(p,end);if(version!=1&&version!=2)die_msg("unknown frozen statement program version");
    std::array<const uint8_t*,3>cursor{},slot_end{};for(size_t i=0;i<3;++i){cursor[i]=raw.slot_data[i].data();slot_end[i]=cursor[i]+raw.slot_data[i].size();}
    struct LexiconValue{uint8_t type=0;std::vector<uint8_t>value;};const uint64_t lexicon_count=get_varint(p,end);std::vector<LexiconValue>lexicon;lexicon.reserve(size_t(lexicon_count));
    for(uint64_t i=0;i<lexicon_count;++i){if(p==end)die_msg("statement lexicon type missing");const uint8_t type=*p++;const size_t channel=frozen_slot_index(type);const uint64_t len=get_varint(p,end);
        if(len>uint64_t(slot_end[channel]-cursor[channel]))die_msg("statement lexicon exceeds slot stream");
        lexicon.push_back({type,std::vector<uint8_t>(cursor[channel],cursor[channel]+len)});cursor[channel]+=len;}
    auto read_value=[&](uint8_t type){const uint64_t code=get_varint(p,end);if(!(code&1)){const uint64_t id=code>>1;if(id>=lexicon.size()||lexicon[size_t(id)].type!=type)die_msg("statement lexicon reference mismatch");return lexicon[size_t(id)].value;}
        const size_t channel=frozen_slot_index(type);const uint64_t len=code>>1;if(len>uint64_t(slot_end[channel]-cursor[channel]))die_msg("statement inline slot exceeds stream");std::vector<uint8_t>value(cursor[channel],cursor[channel]+len);cursor[channel]+=len;return value;};
    struct Table{uint32_t rule_id=0;std::vector<std::vector<std::vector<uint8_t>>>rows;};
    const uint64_t table_count=get_varint(p,end);std::vector<Table>tables;tables.reserve(size_t(table_count));
    for(uint64_t t=0;t<table_count;++t){Table table;const uint64_t rule_id=get_varint(p,end);if(rule_id>=model_rules.size())die_msg("statement model rule missing");table.rule_id=uint32_t(rule_id);
        const DecodedTemplate&rule=model_rules[table.rule_id];if(version==2){const uint64_t row_count=get_varint(p,end);if(row_count>SIZE_MAX)die_msg("statement row count too large");
            table.rows.resize(size_t(row_count),std::vector<std::vector<uint8_t>>(rule.slot_type.size()));
            for(size_t slot=0;slot<rule.slot_type.size();++slot)for(size_t row=0;row<size_t(row_count);++row)table.rows[row][slot]=read_value(rule.slot_type[slot]);}
        tables.push_back(std::move(table));}
    const uint64_t output_len=get_varint(p,end);if(output_len>SIZE_MAX)die_msg("statement output too large");std::vector<uint8_t>output;output.reserve(size_t(output_len));
    const uint8_t*residual=raw.residual.data(),*residual_end=residual+raw.residual.size();
    while(output.size()<output_len){const uint64_t code=get_varint(p,end);if(!(code&1)){const uint64_t len=code>>1;if(!len||len>output_len-output.size()||len>uint64_t(residual_end-residual))die_msg("statement ADD exceeds output");output.insert(output.end(),residual,residual+len);residual+=len;continue;}
        const uint64_t local=code>>1;if(local>=tables.size())die_msg("statement local rule missing");const Table&table=tables[size_t(local)];const DecodedTemplate&rule=model_rules[table.rule_id];
        std::vector<std::vector<uint8_t>>inline_values;const std::vector<std::vector<uint8_t>>*values=nullptr;
        if(version==2){const uint64_t instance_id=get_varint(p,end);if(instance_id>=table.rows.size())die_msg("statement instance missing");values=&table.rows[size_t(instance_id)];}
        else{inline_values.resize(rule.slot_type.size());for(size_t slot=0;slot<inline_values.size();++slot)inline_values[slot]=read_value(rule.slot_type[slot]);values=&inline_values;}
        for(size_t i=0;i<rule.occurrence_slot.size();++i){output.insert(output.end(),rule.literals[i].begin(),rule.literals[i].end());const auto&value=(*values)[rule.occurrence_slot[i]];output.insert(output.end(),value.begin(),value.end());}
        output.insert(output.end(),rule.literals.back().begin(),rule.literals.back().end());if(output.size()>output_len)die_msg("statement rule exceeds output");}
    if(p!=end||residual!=residual_end)die_msg("trailing frozen statement control/residual");
    for(size_t i=0;i<3;++i)if(cursor[i]!=slot_end[i])die_msg("trailing frozen statement slot bytes");
    return output;
}

struct FrozenStatementFrames {
    std::vector<uint8_t>control,residual;
    std::array<std::vector<uint8_t>,3>slot_data;
    uint64_t wire=0;
    bool valid=false;
};

static FrozenStatementFrames compress_frozen_statement_pack(const FrozenSemanticParamRaw&raw,FrameCodec&codec,int level){
    FrozenStatementFrames out;if(!raw.valid)return out;out.control=codec.compress(raw.control,level);out.wire=out.control.size()+4;
    if(!raw.residual.empty()){out.residual=codec.compress(raw.residual,level);out.wire+=out.residual.size()+4;}
    for(size_t i=0;i<3;++i)if(!raw.slot_data[i].empty()){out.slot_data[i]=codec.compress(raw.slot_data[i],level);out.wire+=out.slot_data[i].size()+4;}
    out.valid=true;return out;
}

static FrozenSemanticParamRaw decompress_frozen_statement_pack(const FrozenStatementFrames&frames,FrameCodec&codec){
    if(!frames.valid)die_msg("missing frozen statement frames");
    FrozenSemanticParamRaw raw;raw.valid=true;raw.control=codec.decompress(frames.control);
    if(!frames.residual.empty())raw.residual=codec.decompress(frames.residual);
    for(size_t i=0;i<3;++i)if(!frames.slot_data[i].empty())raw.slot_data[i]=codec.decompress(frames.slot_data[i]);
    return raw;
}

static FrozenSemanticParamRaw encode_frozen_semantic_param_pack(const std::vector<ParamSpanRecord>&records,
                                                                 const LineStore&store,
                                                                 const PretrainedParamPackage&model){
    FrozenSemanticParamRaw raw;std::vector<uint64_t>uses(model.encoder_rules.size());
    for(const ParamSpanRecord&record:records)if(record.program)for(const ParamSpanOp&op:record.ops)if(op.rule)++uses[op.rule_id];
    std::vector<uint32_t>used;for(uint32_t i=0;i<uses.size();++i)if(uses[i])used.push_back(i);if(used.empty())return raw;
    std::vector<uint32_t>remap(model.encoder_rules.size(),UINT32_MAX);
    std::unordered_map<std::string,uint32_t>frequency;
    for(const ParamSpanRecord&record:records)if(record.program)for(const ParamSpanOp&op:record.ops)if(op.rule){const ParsedLine&shape=model.encoder_rules[op.rule_id];
        for(size_t slot=0;slot<op.values.size();++slot){std::string key;key.reserve(op.values[slot].size()+1);key.push_back(char(shape.slot_type[slot]));
            key.append(reinterpret_cast<const char*>(op.values[slot].data()),op.values[slot].size());++frequency[key];}}
    struct LexiconValue{uint8_t type=0;std::string value;uint32_t count=0;};std::vector<LexiconValue>lexicon;
    for(const auto&item:frequency){const size_t len=item.first.size()-1;const uint64_t inline_cost=uint64_t(item.second)*(varint_size((uint64_t(len)<<1)|1)+len);
        const uint64_t dictionary_cost=1+varint_size(len)+len+uint64_t(item.second)*2;
        if(item.second>=2&&dictionary_cost<inline_cost)lexicon.push_back({uint8_t(item.first[0]),item.first.substr(1),item.second});}
    std::sort(lexicon.begin(),lexicon.end(),[](const LexiconValue&a,const LexiconValue&b){
        if(a.count!=b.count)return a.count>b.count;
        if(a.type!=b.type)return a.type<b.type;
        return a.value<b.value;
    });
    std::unordered_map<std::string,uint32_t>lexicon_id;lexicon_id.reserve(lexicon.size()*2+1);
    for(uint32_t i=0;i<lexicon.size();++i){std::string key;key.reserve(lexicon[i].value.size()+1);key.push_back(char(lexicon[i].type));key+=lexicon[i].value;lexicon_id.emplace(std::move(key),i);}
    put_varint(raw.control,lexicon.size());for(const LexiconValue&entry:lexicon){raw.control.push_back(entry.type);put_varint(raw.control,entry.value.size());
        auto&channel=raw.slot_data[frozen_slot_index(entry.type)];channel.insert(channel.end(),entry.value.begin(),entry.value.end());}
    raw.lexicon_entries=lexicon.size();put_varint(raw.control,used.size());
    for(uint32_t local=0;local<used.size();++local){remap[used[local]]=local;put_varint(raw.control,used[local]);}
    auto emit_value=[&](uint8_t type,const std::vector<uint8_t>&value){std::string key;key.reserve(value.size()+1);key.push_back(char(type));
        key.append(reinterpret_cast<const char*>(value.data()),value.size());auto found=lexicon_id.find(key);
        if(found!=lexicon_id.end()){put_varint(raw.control,uint64_t(found->second)<<1);++raw.lexicon_references;}
        else{put_varint(raw.control,(uint64_t(value.size())<<1)|1);auto&channel=raw.slot_data[frozen_slot_index(type)];channel.insert(channel.end(),value.begin(),value.end());}};
    put_varint(raw.control,records.size());const size_t bitmap_bytes=(records.size()+7)/8,bitmap_offset=raw.control.size();raw.control.resize(bitmap_offset+bitmap_bytes);
    for(size_t i=0;i<records.size();++i)if(records[i].program)raw.control[bitmap_offset+i/8]|=uint8_t(1u<<(i%8));
    for(const ParamSpanRecord&record:records){if(!record.program){const uint32_t len=store.len(record.id);raw.residual.insert(raw.residual.end(),store.data(record.id),store.data(record.id)+len);continue;}
        for(const ParamSpanOp&op:record.ops){if(!op.rule){put_varint(raw.control,uint64_t(op.len)<<1);raw.residual.insert(raw.residual.end(),store.data(record.id)+op.target_off,store.data(record.id)+op.target_off+op.len);continue;}
            const uint32_t local=remap[op.rule_id];if(local==UINT32_MAX)die_msg("frozen semantic rule remap missing");put_varint(raw.control,(uint64_t(local)<<1)|1);
            const ParsedLine&shape=model.encoder_rules[op.rule_id];for(size_t slot=0;slot<op.values.size();++slot)emit_value(shape.slot_type[slot],op.values[slot]);}}
    raw.valid=true;return raw;
}

static std::vector<uint8_t>decode_frozen_semantic_param_pack(const FrozenSemanticParamRaw&raw,
                                                              const std::vector<DecodedTemplate>&model_rules,
                                                              const std::vector<uint64_t>&lengths){
    if(!raw.valid)die_msg("missing frozen semantic parameterized program");
    const uint8_t*p=raw.control.data(),*end=p+raw.control.size();
    std::array<const uint8_t*,3>cursor{},slot_end{};for(size_t i=0;i<3;++i){cursor[i]=raw.slot_data[i].data();slot_end[i]=cursor[i]+raw.slot_data[i].size();}
    struct LexiconValue{uint8_t type=0;std::vector<uint8_t>value;};const uint64_t lexicon_count=get_varint(p,end);std::vector<LexiconValue>lexicon;lexicon.reserve(size_t(lexicon_count));
    for(uint64_t i=0;i<lexicon_count;++i){if(p==end)die_msg("frozen semantic lexicon type missing");const uint8_t type=*p++;const size_t channel=frozen_slot_index(type);
        const uint64_t len=get_varint(p,end);if(len>uint64_t(slot_end[channel]-cursor[channel]))die_msg("frozen semantic lexicon exceeds slot stream");
        lexicon.push_back({type,std::vector<uint8_t>(cursor[channel],cursor[channel]+len)});cursor[channel]+=len;}
    const uint64_t used_count=get_varint(p,end);std::vector<uint32_t>used;used.reserve(size_t(used_count));
    for(uint64_t i=0;i<used_count;++i){const uint64_t id=get_varint(p,end);if(id>=model_rules.size())die_msg("frozen semantic model rule missing");used.push_back(uint32_t(id));}
    auto read_value=[&](uint8_t type){const uint64_t code=get_varint(p,end);if(!(code&1)){const uint64_t id=code>>1;
            if(id>=lexicon.size()||lexicon[size_t(id)].type!=type)die_msg("frozen semantic lexicon reference mismatch");
            return lexicon[size_t(id)].value;
        }
        const size_t channel=frozen_slot_index(type);const uint64_t len=code>>1;if(len>uint64_t(slot_end[channel]-cursor[channel]))die_msg("frozen semantic inline slot exceeds stream");
        std::vector<uint8_t>value(cursor[channel],cursor[channel]+len);cursor[channel]+=len;return value;};
    const uint64_t count=get_varint(p,end);if(count!=lengths.size())die_msg("frozen semantic record count mismatch");const size_t bitmap_bytes=(lengths.size()+7)/8;
    if(bitmap_bytes>size_t(end-p))die_msg("frozen semantic bitmap exceeds control");
    const uint8_t*bitmap=p;p+=bitmap_bytes;
    const uint8_t*residual=raw.residual.data(),*residual_end=residual+raw.residual.size();std::vector<uint8_t>payload;uint64_t total=0;
    for(uint64_t len:lengths){if(len>SIZE_MAX-total)die_msg("frozen semantic output too large");total+=len;}payload.reserve(size_t(total));
    for(size_t row=0;row<lengths.size();++row){const uint64_t output_len=lengths[row];const bool program=(bitmap[row/8]>>(row%8))&1u;
        if(!program){if(output_len>uint64_t(residual_end-residual))die_msg("frozen semantic raw exceeds stream");payload.insert(payload.end(),residual,residual+output_len);residual+=output_len;continue;}
        const size_t output_begin=payload.size();while(payload.size()-output_begin<output_len){const uint64_t code=get_varint(p,end);
            if(!(code&1)){const uint64_t len=code>>1;if(!len||len>output_len-(payload.size()-output_begin)||len>uint64_t(residual_end-residual))die_msg("frozen semantic ADD exceeds output");
                payload.insert(payload.end(),residual,residual+len);residual+=len;continue;}
            const uint64_t local=code>>1;if(local>=used.size())die_msg("frozen semantic local rule missing");const DecodedTemplate&rule=model_rules[used[size_t(local)]];
            std::vector<std::vector<uint8_t>>values(rule.slot_type.size());for(size_t slot=0;slot<values.size();++slot)values[slot]=read_value(rule.slot_type[slot]);
            for(size_t i=0;i<rule.occurrence_slot.size();++i){payload.insert(payload.end(),rule.literals[i].begin(),rule.literals[i].end());
                const auto&value=values[rule.occurrence_slot[i]];payload.insert(payload.end(),value.begin(),value.end());}
            payload.insert(payload.end(),rule.literals.back().begin(),rule.literals.back().end());if(payload.size()-output_begin>output_len)die_msg("frozen semantic rule exceeds output");}}
    if(p!=end||residual!=residual_end)die_msg("trailing frozen semantic control/residual");
    for(size_t i=0;i<3;++i)if(cursor[i]!=slot_end[i])die_msg("trailing frozen semantic slot bytes");
    return payload;
}

static SemanticBuffer fuse_frozen_semantic_param(const SemanticBuffer&base,const FrozenSemanticParamRaw&program){
    SemanticBuffer out;put_varint(out.control,1);put_varint(out.control,base.control.size());
    for(size_t c=0;c<kSemanticChannels;++c)put_varint(out.control,base.payload[c].size());
    put_varint(out.control,program.control.size());out.control.insert(out.control.end(),base.control.begin(),base.control.end());
    out.control.insert(out.control.end(),program.control.begin(),program.control.end());out.payload=base.payload;
    out.payload[kRawDefinition]=program.residual;
    out.payload[kIdentifierValue].insert(out.payload[kIdentifierValue].end(),program.slot_data[0].begin(),program.slot_data[0].end());
    out.payload[kNumberValue].insert(out.payload[kNumberValue].end(),program.slot_data[1].begin(),program.slot_data[1].end());
    out.payload[kStringValue].insert(out.payload[kStringValue].end(),program.slot_data[2].begin(),program.slot_data[2].end());
    out.raw_ids=base.raw_ids;return out;
}

static void decode_fused_frozen_semantic(const SemanticBuffer&fused,const std::vector<DecodedTemplate>&model_rules,
                                         DecoderStore&store){
    const uint8_t*p=fused.control.data(),*end=p+fused.control.size();if(get_varint(p,end)!=1)die_msg("unknown fused semantic version");
    const uint64_t base_control_len=get_varint(p,end);std::array<uint64_t,kSemanticChannels>base_lengths{};
    for(uint64_t&len:base_lengths)len=get_varint(p,end);
    const uint64_t program_control_len=get_varint(p,end);
    if(base_control_len+program_control_len!=uint64_t(end-p))die_msg("fused semantic control lengths mismatch");
    SemanticBuffer base;base.control.assign(p,p+base_control_len);p+=base_control_len;FrozenSemanticParamRaw program;program.valid=true;
    program.control.assign(p,p+program_control_len);p+=program_control_len;if(p!=end)die_msg("trailing fused semantic control");
    for(size_t c=0;c<kSemanticChannels;++c){if(c==kRawDefinition)continue;if(base_lengths[c]>fused.payload[c].size())die_msg("fused semantic base payload exceeds stream");
        base.payload[c].assign(fused.payload[c].begin(),fused.payload[c].begin()+base_lengths[c]);}
    program.residual=fused.payload[kRawDefinition];
    const size_t slot_channels[3]={kIdentifierValue,kNumberValue,kStringValue};
    for(size_t i=0;i<3;++i){const size_t c=slot_channels[i];program.slot_data[i].assign(fused.payload[c].begin()+base_lengths[c],fused.payload[c].end());}
    const std::vector<uint64_t>raw_lengths=semantic_raw_lengths(base.control);
    base.payload[kRawDefinition]=decode_frozen_semantic_param_pack(program,model_rules,raw_lengths);
    if(base.payload[kRawDefinition].size()!=base_lengths[kRawDefinition])die_msg("fused semantic reconstructed raw length mismatch");
    decode_semantic_templates(base,store);
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
    size_t param_model_kib = 0;
    unsigned model_min_corpora = 1;
    unsigned source_program_k = 4;
    bool pretrain_all_identifiers = false;
    bool param_coarse_lines = false;
    bool param_statements = false;
    std::vector<std::string> pretrain_manifests;
    std::vector<std::string> param_pretrain_source_roots;
    std::string semantic_export_prefix;
    std::string source_dictionary_path;
    std::string param_model_in_path;
    std::string param_model_out_path;
    bool semantic_export_only = false;
    int zlevel = 3;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--manifest") && i + 1 < argc) manifest = argv[++i];
        else if (!std::strcmp(argv[i], "--max-files") && i + 1 < argc) max_files = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--pretrain-manifest") && i + 1 < argc) pretrain_manifests.emplace_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--param-pretrain-source-root") && i + 1 < argc) param_pretrain_source_roots.emplace_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--pretrain-max-files") && i + 1 < argc) pretrain_max_files = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--model-kib") && i + 1 < argc) model_kib = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--param-model-kib") && i + 1 < argc) param_model_kib = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--param-model-in") && i + 1 < argc) param_model_in_path = argv[++i];
        else if (!std::strcmp(argv[i], "--param-model-out") && i + 1 < argc) param_model_out_path = argv[++i];
        else if (!std::strcmp(argv[i], "--model-min-corpora") && i + 1 < argc) model_min_corpora = unsigned(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--source-k") && i + 1 < argc) source_program_k = unsigned(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--pretrain-all-identifiers")) pretrain_all_identifiers = true;
        else if (!std::strcmp(argv[i], "--param-coarse-lines")) param_coarse_lines = true;
        else if (!std::strcmp(argv[i], "--param-statements")) param_statements = true;
        else if (!std::strcmp(argv[i], "--semantic-export-prefix") && i + 1 < argc) semantic_export_prefix = argv[++i];
        else if (!std::strcmp(argv[i], "--semantic-export-only")) semantic_export_only = true;
        else if (!std::strcmp(argv[i], "--source-dict") && i + 1 < argc) source_dictionary_path = argv[++i];
        else if (!std::strcmp(argv[i], "--z") && i + 1 < argc) zlevel = std::atoi(argv[++i]);
        else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    const bool build_param_model=param_model_kib!=0;
    const bool load_param_model=!param_model_in_path.empty();
    const bool use_param_pretrained=build_param_model||load_param_model;
    const bool param_training_available=!pretrain_manifests.empty()||!param_pretrain_source_roots.empty();
    if (!manifest || zlevel < 0 || zlevel > 3 || source_program_k > 32 || (model_kib&&pretrain_manifests.empty()) ||
        (build_param_model&&!param_training_available) || (build_param_model&&load_param_model) ||
        (!param_model_out_path.empty()&&!build_param_model) || (param_coarse_lines&&!build_param_model) ||
        (param_statements&&!build_param_model) || (param_coarse_lines&&param_statements) || (load_param_model&&!param_pretrain_source_roots.empty()) ||
        (!model_kib&&!build_param_model&&!pretrain_manifests.empty()) || (!use_param_pretrained&&!param_pretrain_source_roots.empty()) ||
        (semantic_export_only&&semantic_export_prefix.empty())) {
        std::fprintf(stderr, "usage: %s --manifest FILE [--max-files N] [--z 0..3] [--source-k 0..32] [--source-dict FILE] [--semantic-export-prefix PATH [--semantic-export-only]] [--pretrain-manifest FILE ... --model-kib N] [--param-model-kib N --param-pretrain-source-root DIR ... --param-model-out FILE [--param-coarse-lines|--param-statements]] [--param-model-in FILE] [--pretrain-max-files N] [--model-min-corpora N] [--pretrain-all-identifiers]\n", argv[0]);
        return 2;
    }
    for(const std::string&training:pretrain_manifests)if(training==manifest)die_msg("pretraining and target manifests must be disjoint");

    const bool use_pretrained=model_kib!=0;
    PretrainedPackage pretrained;
    if(use_pretrained){const auto training_begin=Clock::now();PretrainedBuilder builder(pretrain_all_identifiers);for(size_t i=0;i<pretrain_manifests.size();++i)builder.scan_manifest(pretrain_manifests[i],pretrain_max_files,i);const double training_seconds=elapsed(training_begin);pretrained=builder.compile(model_kib*1024,zlevel,training_seconds,model_min_corpora);}
    const PretrainedPackage frozen_pretrained=pretrained;
    FrameCodec pretrained_package_codec;const std::vector<uint8_t>pretrained_receiver_raw=use_pretrained?pretrained_package_codec.decompress(pretrained.frame):std::vector<uint8_t>{};if(pretrained_receiver_raw!=pretrained.raw)die_msg("pretrained receiver package mismatch");
    PretrainedParamPackage pretrained_param;
    if(load_param_model)pretrained_param=load_pretrained_param_package(param_model_in_path);
    else if(build_param_model){const auto training_begin=Clock::now();PretrainedParamBuilder builder(param_coarse_lines,param_statements);
        for(size_t i=0;i<pretrain_manifests.size();++i)builder.scan_manifest(pretrain_manifests[i],pretrain_max_files,i);
        for(size_t i=0;i<param_pretrain_source_roots.size();++i)builder.scan_source_root(param_pretrain_source_roots[i],pretrain_max_files,pretrain_manifests.size()+i);
        const double training_seconds=elapsed(training_begin);
        pretrained_param=builder.compile(param_model_kib*1024,zlevel,training_seconds,model_min_corpora);
        if(!param_model_out_path.empty()){save_file_bytes(param_model_out_path,pretrained_param.frame);
            const PretrainedParamPackage exported=load_pretrained_param_package(param_model_out_path);
            if(exported.frame!=pretrained_param.frame||exported.raw!=pretrained_param.raw)die_msg("exported pretrained parameterized package mismatch");}}
    FrameCodec pretrained_param_package_codec;
    const std::vector<uint8_t>pretrained_param_receiver_raw=use_param_pretrained?pretrained_param_package_codec.decompress(pretrained_param.frame):std::vector<uint8_t>{};
    if(pretrained_param_receiver_raw!=pretrained_param.raw)die_msg("pretrained parameterized receiver package mismatch");
    std::vector<DecodedTemplate>pretrained_param_receiver_rules;
    if(use_param_pretrained)decode_pretrained_param_package(pretrained_param_receiver_raw,pretrained_param_receiver_rules);
    const bool use_statement_model=use_param_pretrained&&pretrained_param.unit_mode==kParamModelStatements;
    const bool use_source_dictionary=!source_dictionary_path.empty();
    const std::vector<uint8_t>source_dictionary=use_source_dictionary?load_file_bytes(source_dictionary_path):std::vector<uint8_t>{};
    if(use_source_dictionary&&(source_dictionary.empty()||source_dictionary.size()>UINT32_MAX))die_msg("source dictionary size out of range");
    FrameCodec source_dictionary_package_codec;
    const std::vector<uint8_t>source_dictionary_package=use_source_dictionary?source_dictionary_package_codec.compress(source_dictionary,zlevel):std::vector<uint8_t>{};
    const std::vector<uint8_t>source_dictionary_decoded=use_source_dictionary?source_dictionary_package_codec.decompress(source_dictionary_package):std::vector<uint8_t>{};
    if(source_dictionary_decoded!=source_dictionary)die_msg("source dictionary package mismatch");

    const auto begin = Clock::now();
    Corpus corpus = load_corpus(manifest, max_files);
    if(semantic_export_only){LineStore export_truth;DecoderStore export_decoder;FrameCodec export_frames;SemanticFrameWriter writer(semantic_export_prefix);uint64_t wire=0,definitions=0;
        for(const FileSpan&file:corpus.files){const uint8_t*p=corpus.bytes.data()+file.off,*end=p+file.len;std::vector<uint32_t>new_ids;while(p<end){const void*hit=std::memchr(p,'\n',size_t(end-p));const uint8_t*line_end=hit?static_cast<const uint8_t*>(hit)+1:end;const LineStore::Result result=export_truth.intern(p,uint32_t(line_end-p));if(result.first)new_ids.push_back(result.id);p=line_end;}
            TemplateStats a_stats,b_stats;const SemanticBuffer a=encode_semantic_templates(new_ids,export_truth,a_stats,false),b=encode_semantic_templates(new_ids,export_truth,b_stats,true);const SemanticFrames af=compress_semantic(a,export_frames,zlevel),bf=compress_semantic(b,export_frames,zlevel);const SemanticBuffer&selected=bf.wire<af.wire?b:a;const SemanticFrames&selected_frame=bf.wire<af.wire?bf:af;writer.add(file.len,selected);const SemanticBuffer decoded=decompress_semantic(selected_frame,export_frames);decode_semantic_templates(decoded,export_decoder);verify_definitions(new_ids,export_truth,export_decoder);wire+=selected_frame.wire;definitions+=new_ids.size();}
        std::printf("semantic export exact=PASS manifest=%s TUs=%zu raw=%llu definitions=%llu zstd-wire=%llu prefix=%s seconds=%.2f\n",manifest,corpus.files.size(),static_cast<unsigned long long>(corpus.raw),static_cast<unsigned long long>(definitions),static_cast<unsigned long long>(wire),semantic_export_prefix.c_str(),elapsed(begin));return 0;}
    LineStore truth;
    FrameCodec frames;
    ModelFrameCodec model_encoder_frames(pretrained.raw,zlevel),model_decoder_frames(pretrained_receiver_raw,zlevel);
    ModelFrameCodec model_p4_encoder_frames(pretrained.raw,zlevel),model_p4_decoder_frames(pretrained_receiver_raw,zlevel);
    ModelFrameCodec model_local_superblock_encoder_frames(pretrained.raw,zlevel),model_local_superblock_decoder_frames(pretrained_receiver_raw,zlevel);
    ModelFrameCodec pretrained_source_encoder_frames(pretrained.raw,zlevel),pretrained_source_decoder_frames(pretrained_receiver_raw,zlevel);
    ModelFrameCodec pretrained_param_encoder_frames(pretrained_param.raw,zlevel),pretrained_param_decoder_frames(pretrained_param_receiver_raw,zlevel);
    ModelFrameCodec source_dictionary_encoder_frames(source_dictionary,zlevel),source_dictionary_decoder_frames(source_dictionary_decoded,zlevel);
    ModelFrameCodec token_dictionary_encoder_frames(source_dictionary,zlevel),token_dictionary_decoder_frames(source_dictionary_decoded,zlevel);
    Row p0("P0-appearance"), p1("P1-lexicographic"), p4("P4-alpha-template"),p5("P5-multiline"),p6("P6-token-forest"),p7s("P7-source-location"),p7p("P7p-source-superblock"),p7t("P7t-all-token-spans"),p7("P7-online-template"),p7b("P7b-local-rule-online-token"),p9("P9-semantic-split-zstd"),p9s("P9s-semantic+source"),p9sg("P9sg-source-basis-grammar"),p9sm("P9sm-pretrained-source-grammar"),p9sd("P9s+trained-source-dict"),p9t("P9t-semantic+token-spans"),p9td("P9t+trained-source-dict"),p9g("P9g-parameterized-spans"),p8("P8-pretrained-superblock"),p8c("P8c-frozen+TU-local-superblock"),p8d("P8-pretrained-dict-P4"), p10("P10-best-local-frame"),p10sd("P10+source-dict"),p10p("P10+P8-charged-union"),p11("P11-best-token-span-union"),p11d("P11+token-spans+dict"),p12("P12-best-param-superblock"),p13("P13-best-pretrained-superblock-union"),p14("P14-pretrained-param-spans"),p15("P15-all-pretrained-union"),p16("P16-fused-pretrained-param"),p17("P17-all-pretrained-fused-union"),p18("P18-portable-statements");
    std::vector<Row*> rows{&p0, &p1, &p4,&p5,&p6,&p7s,&p7p,&p7t,&p7,&p7b,&p9,&p9s,&p9sg};if(use_pretrained)rows.push_back(&p9sm);if(use_source_dictionary)rows.push_back(&p9sd);rows.push_back(&p9t);if(use_source_dictionary)rows.push_back(&p9td);rows.push_back(&p9g);if(use_pretrained){rows.push_back(&p8);rows.push_back(&p8c);rows.push_back(&p8d);}rows.push_back(&p10);if(use_source_dictionary)rows.push_back(&p10sd);if(use_pretrained)rows.push_back(&p10p);rows.push_back(&p11);if(use_source_dictionary)rows.push_back(&p11d);rows.push_back(&p12);if(use_pretrained)rows.push_back(&p13);if(use_param_pretrained){rows.push_back(&p14);rows.push_back(&p15);rows.push_back(&p16);rows.push_back(&p17);}if(use_statement_model)rows.push_back(&p18);
    if(use_pretrained){const uint64_t package_wire=pretrained.frame.size()+4;p8.wire=package_wire;p8c.wire=package_wire;p8d.wire=package_wire;p9sm.wire=package_wire;p10p.wire=package_wire;p13.wire=package_wire;}
    if(use_param_pretrained){const uint64_t package_wire=pretrained_param.frame.size()+4;p14.wire=package_wire;p15.wire=package_wire;p16.wire=package_wire;p17.wire=package_wire;if(use_statement_model)p18.wire=package_wire;if(use_pretrained){p15.wire+=pretrained.frame.size()+4;p17.wire+=pretrained.frame.size()+4;}}
    if(use_source_dictionary){const uint64_t package_wire=source_dictionary_package.size()+4;p9sd.wire=package_wire;p9td.wire=package_wire;p10sd.wire=package_wire;p11d.wire=package_wire;}
    std::unique_ptr<SemanticFrameWriter>semantic_writer;if(!semantic_export_prefix.empty())semantic_writer=std::make_unique<SemanticFrameWriter>(semantic_export_prefix);
    for (Row* row : rows) row->tu_wire.reserve(corpus.files.size());
    TemplateStats template_stats;
    TemplateStats semantic_stats;
    ForestStats forest_stats;
    PersistentStats persistent_stats;PersistentEncoder persistent_encoder;PersistentDecoder persistent_decoder;
    PersistentStats hybrid_stats;PersistentEncoder hybrid_encoder;PersistentDecoder hybrid_decoder;
    PersistentStats pretrained_stats,pretrained_local_stats;
    MultiStats multi_stats;
    SourceLearner source_learner;SourceStats source_stats;SourcePackStats source_pack_stats,semantic_source_pack_stats,grammar_source_pack_stats,pretrained_source_pack_stats,dictionary_source_pack_stats;TokenSpanStats all_token_span_stats,token_span_stats,dictionary_token_span_stats;TokenSpanSweepStats all_token_span_sweep,token_span_sweep,dictionary_token_span_sweep;SourceCatalog source_catalog;std::vector<LineSourceMeta>source_meta(1);
    ParamSpanStats param_span_stats;uint64_t param_span_literal_wire=0,param_span_candidate_wire=0,param_span_selected_wire=0,param_span_frame_wins=0,param_span_combined_wins=0,param_span_columnar_wins=0;
    ParamSpanStats pretrained_param_stats;uint64_t pretrained_param_literal_wire=0,pretrained_param_candidate_wire=0,pretrained_param_selected_wire=0,pretrained_param_frame_wins=0,pretrained_param_combined_wins=0,pretrained_param_columnar_wins=0;
    uint64_t fused_param_candidate_wire=0,fused_param_selected_wire=0,fused_param_frame_wins=0;
    StatementProgramStats statement_stats;uint64_t statement_literal_wire=0,statement_candidate_wire=0,statement_selected_wire=0,statement_frame_wins=0;
    std::array<uint64_t,4>p13_choice_tus{},p13_choice_wire{};
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
            const uint32_t marker_path_id=marker_line?source_catalog.intern(marker.path):0;
            if(marker_line){line_meta.marker=true;line_meta.category=kMarkerCategory;line_meta.location=make_source_location(marker.path,marker.logical_line,marker_path_id);}
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
            if(marker_line){current_path=marker.path;current_location=make_source_location(current_path,marker.logical_line,marker_path_id);current_category=classify_source_path(current_path);}
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
        if(use_pretrained){const auto e8d=Clock::now();p8d_control_frame=model_p4_encoder_frames.compress_best(p4_selected->control,zlevel);p8d_data_frame=model_p4_encoder_frames.compress_best(p4_selected->data,zlevel);w8d=p8d_control_frame.bytes.size()+p8d_data_frame.bytes.size()+10;p8d.encode_seconds+=elapsed(e8d);
            const auto d8d=Clock::now();p8d_control_decoded=model_p4_decoder_frames.decompress(p8d_control_frame);p8d_data_decoded=model_p4_decoder_frames.decompress(p8d_data_frame);decode_templates(p8d_control_decoded,p8d_data_decoded,p8d.decoder);p8d.decode_seconds+=elapsed(d8d);verify_definitions(new_ids,truth,p8d.decoder);}

        const auto e9=Clock::now();TemplateStats p9_stats_a,p9_stats_b;const SemanticBuffer p9_raw_a=encode_semantic_templates(new_ids,truth,p9_stats_a,false),p9_raw_b=encode_semantic_templates(new_ids,truth,p9_stats_b,true);const SemanticBuffer*p9_selected=&p9_raw_a;SemanticFrames p9_frames=compress_semantic(p9_raw_a,frames,zlevel),p9_frames_b=compress_semantic(p9_raw_b,frames,zlevel);TemplateStats selected_p9=p9_stats_a;
        if(p9_frames_b.wire<p9_frames.wire){p9_frames=std::move(p9_frames_b);selected_p9=p9_stats_b;p9_selected=&p9_raw_b;}if(semantic_writer)semantic_writer->add(file.len,*p9_selected);p9.encode_seconds+=elapsed(e9);semantic_stats.rules+=selected_p9.rules;semantic_stats.instances+=selected_p9.instances;semantic_stats.unique_slots+=selected_p9.unique_slots;semantic_stats.slot_occurrences+=selected_p9.slot_occurrences;semantic_stats.literal_fallbacks+=selected_p9.literal_fallbacks;semantic_stats.template_input_bytes+=selected_p9.template_input_bytes;semantic_stats.lexicon_entries+=selected_p9.lexicon_entries;semantic_stats.lexicon_references+=selected_p9.lexicon_references;p9_control_wire+=p9_frames.control.size()+4;for(size_t c=0;c<kSemanticChannels;++c)if(!p9_frames.payload[c].empty())p9_payload_wire[c]+=p9_frames.payload[c].size()+4;
        const auto d9=Clock::now();const SemanticBuffer p9_decoded=decompress_semantic(p9_frames,frames);decode_semantic_templates(p9_decoded,p9.decoder);p9.decode_seconds+=elapsed(d9);verify_definitions(new_ids,truth,p9.decoder);

        SourcePackFrames p9s_source_frames,p9sg_source_frames,p9sm_source_frames,p9sd_source_frames;size_t p9s_threshold_index=0,p9sg_threshold_index=0,p9sm_threshold_index=0,p9sd_threshold_index=0;double p9s_tu_encode=0,p9sg_tu_encode=0,p9sm_tu_encode=0,p9sd_tu_encode=0;
        for(size_t threshold_index=0;threshold_index<kSourcePackThresholds.size();++threshold_index){
            const unsigned threshold=kSourcePackThresholds[threshold_index];
            const auto plan_begin=Clock::now();
            SourcePackRaw candidate_raw=encode_source_pack(p9_selected->raw_ids,truth,source_meta,current_locations,source_catalog,threshold,true);
            const double plan_seconds=elapsed(plan_begin);const auto plain_begin=Clock::now();
            SourcePackFrames candidate=compress_source_pack(candidate_raw,frames,zlevel,threshold);
            p9s_tu_encode+=plan_seconds+elapsed(plain_begin);
            if(candidate.valid){++semantic_source_pack_stats.threshold_tus[threshold_index];semantic_source_pack_stats.threshold_wire[threshold_index]+=candidate.wire;
                semantic_source_pack_stats.threshold_basis_bytes[threshold_index]+=candidate.stats.basis_bytes;semantic_source_pack_stats.threshold_residual_bytes[threshold_index]+=candidate.stats.residual_bytes;}
            if(candidate.valid&&(!p9s_source_frames.valid||candidate.wire<p9s_source_frames.wire)){p9s_source_frames=std::move(candidate);p9s_threshold_index=threshold_index;}
            const auto grammar_begin=Clock::now();SourcePackRaw grammar_a=template_source_pack_basis(candidate_raw,false),grammar_b=template_source_pack_basis(candidate_raw,true);SourcePackFrames grammar_frame_a=compress_source_pack(grammar_a,frames,zlevel,threshold),grammar_frame_b=compress_source_pack(grammar_b,frames,zlevel,threshold);SourcePackFrames grammar_candidate=grammar_frame_b.valid&&(!grammar_frame_a.valid||grammar_frame_b.wire<grammar_frame_a.wire)?std::move(grammar_frame_b):std::move(grammar_frame_a);p9sg_tu_encode+=plan_seconds+elapsed(grammar_begin);
            if(grammar_candidate.valid){++grammar_source_pack_stats.threshold_tus[threshold_index];grammar_source_pack_stats.threshold_wire[threshold_index]+=grammar_candidate.wire;grammar_source_pack_stats.threshold_basis_bytes[threshold_index]+=grammar_candidate.stats.basis_bytes;grammar_source_pack_stats.threshold_residual_bytes[threshold_index]+=grammar_candidate.stats.residual_bytes;}
            if(grammar_candidate.valid&&(!p9sg_source_frames.valid||grammar_candidate.wire<p9sg_source_frames.wire)){p9sg_source_frames=std::move(grammar_candidate);p9sg_threshold_index=threshold_index;}
            if(use_pretrained){const auto pretrained_begin=Clock::now();SourcePackRaw pretrained_raw=pretrained_source_pack_basis(candidate_raw,frozen_pretrained,pretrain_all_identifiers);SourcePackFrames pretrained_candidate=compress_source_pack(pretrained_raw,frames,zlevel,threshold,&pretrained_source_encoder_frames);p9sm_tu_encode+=plan_seconds+elapsed(pretrained_begin);
                if(pretrained_candidate.valid){++pretrained_source_pack_stats.threshold_tus[threshold_index];pretrained_source_pack_stats.threshold_wire[threshold_index]+=pretrained_candidate.wire;pretrained_source_pack_stats.threshold_basis_bytes[threshold_index]+=pretrained_candidate.stats.basis_bytes;pretrained_source_pack_stats.threshold_residual_bytes[threshold_index]+=pretrained_candidate.stats.residual_bytes;}
                if(pretrained_candidate.valid&&(!p9sm_source_frames.valid||pretrained_candidate.wire<p9sm_source_frames.wire)){p9sm_source_frames=std::move(pretrained_candidate);p9sm_threshold_index=threshold_index;}}
            if(use_source_dictionary){const auto dictionary_begin=Clock::now();SourcePackFrames dictionary_candidate=compress_source_pack(candidate_raw,frames,zlevel,threshold,&source_dictionary_encoder_frames);p9sd_tu_encode+=plan_seconds+elapsed(dictionary_begin);
                if(dictionary_candidate.valid){++dictionary_source_pack_stats.threshold_tus[threshold_index];dictionary_source_pack_stats.threshold_wire[threshold_index]+=dictionary_candidate.wire;
                    dictionary_source_pack_stats.threshold_basis_bytes[threshold_index]+=dictionary_candidate.stats.basis_bytes;dictionary_source_pack_stats.threshold_residual_bytes[threshold_index]+=dictionary_candidate.stats.residual_bytes;}
                if(dictionary_candidate.valid&&(!p9sd_source_frames.valid||dictionary_candidate.wire<p9sd_source_frames.wire)){p9sd_source_frames=std::move(dictionary_candidate);p9sd_threshold_index=threshold_index;}}
        }
        const uint64_t p9_raw_wire=p9_frames.payload[kRawDefinition].empty()?0:p9_frames.payload[kRawDefinition].size()+4;
        const uint64_t p9_without_raw=p9_frames.wire-p9_raw_wire;
        const uint64_t p9_source_size=p9s_source_frames.valid?p9_without_raw+p9s_source_frames.wire:UINT64_MAX;
        const uint64_t p9_grammar_source_size=p9sg_source_frames.valid?p9_without_raw+p9sg_source_frames.wire:UINT64_MAX;
        const uint64_t p9_pretrained_source_size=p9sm_source_frames.valid?p9_without_raw+p9sm_source_frames.wire:UINT64_MAX;
        const uint64_t p9_dictionary_source_size=p9sd_source_frames.valid?p9_without_raw+p9sd_source_frames.wire:UINT64_MAX;
        const bool p9s_use_source=p9s_source_frames.valid&&p9_source_size<p9_frames.wire;
        const bool p9sg_use_source=p9sg_source_frames.valid&&p9_grammar_source_size<p9_frames.wire;
        const bool p9sm_use_source=p9sm_source_frames.valid&&p9_pretrained_source_size<p9_frames.wire;
        const bool p9sd_use_source=p9sd_source_frames.valid&&p9_dictionary_source_size<p9_frames.wire;
        const uint64_t w9s=std::min(p9_frames.wire,p9_source_size)+1;p9s.encode_seconds+=p9s_tu_encode;
        const uint64_t w9sg=std::min(p9_frames.wire,p9_grammar_source_size)+1;p9sg.encode_seconds+=p9sg_tu_encode;
        const uint64_t w9sm=std::min(p9_frames.wire,p9_pretrained_source_size)+1;if(use_pretrained)p9sm.encode_seconds+=p9sm_tu_encode;
        const uint64_t w9sd=std::min(p9_frames.wire,p9_dictionary_source_size)+1;
        if(use_source_dictionary)p9sd.encode_seconds+=p9sd_tu_encode;
        semantic_source_pack_stats.literal_wire+=p9_raw_wire;semantic_source_pack_stats.selected_wire+=(p9s_use_source?p9s_source_frames.wire:p9_raw_wire)+1;
        if(p9s_source_frames.valid){
            ++semantic_source_pack_stats.candidate_tus;++semantic_source_pack_stats.threshold_wins[p9s_threshold_index];semantic_source_pack_stats.source_wire+=p9s_source_frames.wire;
            if(p9s_use_source)++semantic_source_pack_stats.frame_wins;
            semantic_source_pack_stats.selected.available_records+=p9s_source_frames.stats.available_records;semantic_source_pack_stats.selected.available_bytes+=p9s_source_frames.stats.available_bytes;
            semantic_source_pack_stats.selected.source_records+=p9s_source_frames.stats.source_records;semantic_source_pack_stats.selected.literal_records+=p9s_source_frames.stats.literal_records;
            semantic_source_pack_stats.selected.basis_records+=p9s_source_frames.stats.basis_records;semantic_source_pack_stats.selected.basis_bytes+=p9s_source_frames.stats.basis_bytes;
            semantic_source_pack_stats.selected.residual_bytes+=p9s_source_frames.stats.residual_bytes;semantic_source_pack_stats.selected.target_bytes+=p9s_source_frames.stats.target_bytes;
            account_source_pack_layout(semantic_source_pack_stats,p9s_source_frames);
        }
        grammar_source_pack_stats.literal_wire+=p9_raw_wire;grammar_source_pack_stats.selected_wire+=(p9sg_use_source?p9sg_source_frames.wire:p9_raw_wire)+1;
        if(p9sg_source_frames.valid){
            ++grammar_source_pack_stats.candidate_tus;++grammar_source_pack_stats.threshold_wins[p9sg_threshold_index];grammar_source_pack_stats.source_wire+=p9sg_source_frames.wire;if(p9sg_use_source)++grammar_source_pack_stats.frame_wins;
            grammar_source_pack_stats.selected.available_records+=p9sg_source_frames.stats.available_records;grammar_source_pack_stats.selected.available_bytes+=p9sg_source_frames.stats.available_bytes;grammar_source_pack_stats.selected.source_records+=p9sg_source_frames.stats.source_records;grammar_source_pack_stats.selected.literal_records+=p9sg_source_frames.stats.literal_records;grammar_source_pack_stats.selected.basis_records+=p9sg_source_frames.stats.basis_records;grammar_source_pack_stats.selected.basis_bytes+=p9sg_source_frames.stats.basis_bytes;grammar_source_pack_stats.selected.residual_bytes+=p9sg_source_frames.stats.residual_bytes;grammar_source_pack_stats.selected.target_bytes+=p9sg_source_frames.stats.target_bytes;
            account_source_pack_layout(grammar_source_pack_stats,p9sg_source_frames);
        }
        if(use_pretrained){pretrained_source_pack_stats.literal_wire+=p9_raw_wire;pretrained_source_pack_stats.selected_wire+=(p9sm_use_source?p9sm_source_frames.wire:p9_raw_wire)+1;
            if(p9sm_source_frames.valid){++pretrained_source_pack_stats.candidate_tus;++pretrained_source_pack_stats.threshold_wins[p9sm_threshold_index];pretrained_source_pack_stats.source_wire+=p9sm_source_frames.wire;if(p9sm_use_source)++pretrained_source_pack_stats.frame_wins;
                pretrained_source_pack_stats.selected.available_records+=p9sm_source_frames.stats.available_records;pretrained_source_pack_stats.selected.available_bytes+=p9sm_source_frames.stats.available_bytes;pretrained_source_pack_stats.selected.source_records+=p9sm_source_frames.stats.source_records;pretrained_source_pack_stats.selected.literal_records+=p9sm_source_frames.stats.literal_records;pretrained_source_pack_stats.selected.basis_records+=p9sm_source_frames.stats.basis_records;pretrained_source_pack_stats.selected.basis_bytes+=p9sm_source_frames.stats.basis_bytes;pretrained_source_pack_stats.selected.residual_bytes+=p9sm_source_frames.stats.residual_bytes;pretrained_source_pack_stats.selected.target_bytes+=p9sm_source_frames.stats.target_bytes;account_source_pack_layout(pretrained_source_pack_stats,p9sm_source_frames);}}
        dictionary_source_pack_stats.literal_wire+=p9_raw_wire;dictionary_source_pack_stats.selected_wire+=(p9sd_use_source?p9sd_source_frames.wire:p9_raw_wire)+1;
        if(p9sd_source_frames.valid){
            ++dictionary_source_pack_stats.candidate_tus;++dictionary_source_pack_stats.threshold_wins[p9sd_threshold_index];dictionary_source_pack_stats.source_wire+=p9sd_source_frames.wire;
            if(p9sd_use_source)++dictionary_source_pack_stats.frame_wins;
            dictionary_source_pack_stats.selected.available_records+=p9sd_source_frames.stats.available_records;dictionary_source_pack_stats.selected.available_bytes+=p9sd_source_frames.stats.available_bytes;
            dictionary_source_pack_stats.selected.source_records+=p9sd_source_frames.stats.source_records;dictionary_source_pack_stats.selected.literal_records+=p9sd_source_frames.stats.literal_records;
            dictionary_source_pack_stats.selected.basis_records+=p9sd_source_frames.stats.basis_records;dictionary_source_pack_stats.selected.basis_bytes+=p9sd_source_frames.stats.basis_bytes;
            dictionary_source_pack_stats.selected.residual_bytes+=p9sd_source_frames.stats.residual_bytes;dictionary_source_pack_stats.selected.target_bytes+=p9sd_source_frames.stats.target_bytes;
            account_source_pack_layout(dictionary_source_pack_stats,p9sd_source_frames);
        }
        auto decode_p9_source=[&](const SourcePackFrames&source_frames,ModelFrameCodec*model,DecoderStore&decoder,const PersistentDecoder*basis_model=nullptr){
            SemanticBuffer decoded=decompress_semantic(p9_frames,frames);const std::vector<uint64_t>lengths=semantic_raw_lengths(decoded.control);
            decoded.payload[kRawDefinition]=decode_implicit_source_pack(source_frames,frames,lengths,model,basis_model);decode_semantic_templates(decoded,decoder);
        };
        const auto d9s=Clock::now();if(p9s_use_source)decode_p9_source(p9s_source_frames,nullptr,p9s.decoder);else decode_semantic_templates(p9_decoded,p9s.decoder);
        p9s.decode_seconds+=elapsed(d9s);verify_definitions(new_ids,truth,p9s.decoder);
        const auto d9sg=Clock::now();if(p9sg_use_source)decode_p9_source(p9sg_source_frames,nullptr,p9sg.decoder);else decode_semantic_templates(p9_decoded,p9sg.decoder);p9sg.decode_seconds+=elapsed(d9sg);verify_definitions(new_ids,truth,p9sg.decoder);
        if(use_pretrained){const auto d9sm=Clock::now();if(p9sm_use_source)decode_p9_source(p9sm_source_frames,&pretrained_source_decoder_frames,p9sm.decoder,&frozen_pretrained.decoder);else decode_semantic_templates(p9_decoded,p9sm.decoder);p9sm.decode_seconds+=elapsed(d9sm);verify_definitions(new_ids,truth,p9sm.decoder);}
        if(use_source_dictionary){const auto d9sd=Clock::now();if(p9sd_use_source)decode_p9_source(p9sd_source_frames,&source_dictionary_decoder_frames,p9sd.decoder);else decode_semantic_templates(p9_decoded,p9sd.decoder);p9sd.decode_seconds+=elapsed(d9sd);verify_definitions(new_ids,truth,p9sd.decoder);}

        const auto token_plan_begin=Clock::now();const std::vector<TokenSpanPlan>token_plans=build_token_span_plans(p9_selected->raw_ids,truth,source_meta,current_locations,source_catalog);const double token_plan_seconds=elapsed(token_plan_begin);
        TokenSpanFrames p9t_span_frames,p9td_span_frames;double p9t_tu_encode=token_plan_seconds,p9td_tu_encode=token_plan_seconds;
        for(size_t atom_index=0;atom_index<kTokenSpanAtoms.size();++atom_index){for(size_t threshold_index=0;threshold_index<kSourcePackThresholds.size();++threshold_index){for(unsigned order=0;order<2;++order){
            const auto raw_begin=Clock::now();TokenSpanRaw candidate_raw=encode_token_span_pack(token_plans,truth,kSourcePackThresholds[threshold_index],kTokenSpanAtoms[atom_index],kTokenSpanMinBytes[atom_index],order!=0);const double raw_seconds=elapsed(raw_begin);p9t_tu_encode+=raw_seconds;p9td_tu_encode+=raw_seconds;
            const auto plain_begin=Clock::now();TokenSpanFrames candidate=compress_token_span_pack(candidate_raw,frames,zlevel,kSourcePackThresholds[threshold_index],kTokenSpanAtoms[atom_index],kTokenSpanMinBytes[atom_index],order!=0);p9t_tu_encode+=elapsed(plain_begin);
            const size_t sweep_index=token_span_sweep_index(atom_index,threshold_index,order);token_span_sweep.wire[sweep_index]+=candidate.valid?candidate.wire:p9_raw_wire;
            if(candidate.valid){++token_span_sweep.candidate_tus[sweep_index];if(candidate.wire<p9_raw_wire)++token_span_sweep.wins[sweep_index];token_span_sweep.basis_bytes[sweep_index]+=candidate.stats.basis_bytes;token_span_sweep.copy_bytes[sweep_index]+=candidate.stats.copy_bytes;token_span_sweep.residual_bytes[sweep_index]+=candidate.stats.residual_bytes;token_span_sweep.source_records[sweep_index]+=candidate.stats.source_records;}
            if(candidate.valid&&(!p9t_span_frames.valid||candidate.wire<p9t_span_frames.wire))p9t_span_frames=std::move(candidate);
            if(use_source_dictionary){const auto dictionary_begin=Clock::now();TokenSpanFrames dictionary_candidate=compress_token_span_pack(candidate_raw,frames,zlevel,kSourcePackThresholds[threshold_index],kTokenSpanAtoms[atom_index],kTokenSpanMinBytes[atom_index],order!=0,&token_dictionary_encoder_frames);p9td_tu_encode+=elapsed(dictionary_begin);
                dictionary_token_span_sweep.wire[sweep_index]+=dictionary_candidate.valid?dictionary_candidate.wire:p9_raw_wire;
                if(dictionary_candidate.valid){++dictionary_token_span_sweep.candidate_tus[sweep_index];if(dictionary_candidate.wire<p9_raw_wire)++dictionary_token_span_sweep.wins[sweep_index];dictionary_token_span_sweep.basis_bytes[sweep_index]+=dictionary_candidate.stats.basis_bytes;dictionary_token_span_sweep.copy_bytes[sweep_index]+=dictionary_candidate.stats.copy_bytes;dictionary_token_span_sweep.residual_bytes[sweep_index]+=dictionary_candidate.stats.residual_bytes;dictionary_token_span_sweep.source_records[sweep_index]+=dictionary_candidate.stats.source_records;}
                if(dictionary_candidate.valid&&(!p9td_span_frames.valid||dictionary_candidate.wire<p9td_span_frames.wire))p9td_span_frames=std::move(dictionary_candidate);}
        }}}
        const uint64_t p9_token_span_size=p9t_span_frames.valid?p9_without_raw+p9t_span_frames.wire:UINT64_MAX;
        const uint64_t p9_dictionary_token_span_size=p9td_span_frames.valid?p9_without_raw+p9td_span_frames.wire:UINT64_MAX;
        const bool p9t_use_span=p9t_span_frames.valid&&p9_token_span_size<p9_frames.wire;
        const bool p9td_use_span=p9td_span_frames.valid&&p9_dictionary_token_span_size<p9_frames.wire;
        const uint64_t w9t=std::min(p9_frames.wire,p9_token_span_size)+1,w9td=std::min(p9_frames.wire,p9_dictionary_token_span_size)+1;
        p9t.encode_seconds+=p9t_tu_encode;if(use_source_dictionary)p9td.encode_seconds+=p9td_tu_encode;
        auto accumulate_token_span=[&](TokenSpanStats&stats,const TokenSpanFrames&selected,bool used,uint64_t literal_wire){
            stats.literal_wire+=literal_wire;stats.selected_wire+=(used?selected.wire:literal_wire)+1;if(!selected.valid)return;++stats.candidate_tus;stats.span_wire+=selected.wire;if(used)++stats.frame_wins;
            stats.selected.available_records+=selected.stats.available_records;stats.selected.available_bytes+=selected.stats.available_bytes;stats.selected.source_records+=selected.stats.source_records;stats.selected.literal_records+=selected.stats.literal_records;
            stats.selected.basis_records+=selected.stats.basis_records;stats.selected.basis_bytes+=selected.stats.basis_bytes;stats.selected.copy_bytes+=selected.stats.copy_bytes;stats.selected.residual_bytes+=selected.stats.residual_bytes;stats.selected.target_bytes+=selected.stats.target_bytes;stats.selected.program_ops+=selected.stats.program_ops;
            if(selected.combined){++stats.combined_wins;stats.combined_wire+=selected.combined_frame.size()+4+(selected.combined_dictionary?1:0);}
            else {stats.frame_wire[0]+=selected.basis_control.size()+4+(selected.dictionary[0]?1:0);stats.frame_wire[1]+=selected.basis_data.size()+4+(selected.dictionary[1]?1:0);stats.frame_wire[2]+=selected.definition_control.size()+4+(selected.dictionary[2]?1:0);stats.frame_wire[3]+=selected.definition_data.size()+4+(selected.dictionary[3]?1:0);}
            for(size_t i=0;i<kSourcePackThresholds.size();++i)if(selected.residual_percent==kSourcePackThresholds[i])++stats.threshold_wins[i];
            for(size_t i=0;i<kTokenSpanAtoms.size();++i)if(selected.max_atoms==kTokenSpanAtoms[i]&&selected.min_span_bytes==kTokenSpanMinBytes[i])++stats.atom_wins[i];
            ++stats.order_wins[selected.lexical_basis?1:0];
        };
        accumulate_token_span(token_span_stats,p9t_span_frames,p9t_use_span,p9_raw_wire);if(use_source_dictionary)accumulate_token_span(dictionary_token_span_stats,p9td_span_frames,p9td_use_span,p9_raw_wire);
        auto decode_p9_token_span=[&](const TokenSpanFrames&span_frames,ModelFrameCodec*model,DecoderStore&decoder){SemanticBuffer decoded=decompress_semantic(p9_frames,frames);const std::vector<uint64_t>lengths=semantic_raw_lengths(decoded.control);decoded.payload[kRawDefinition]=decode_implicit_token_span_pack(span_frames,frames,lengths,model);decode_semantic_templates(decoded,decoder);};
        const auto d9t=Clock::now();if(p9t_use_span)decode_p9_token_span(p9t_span_frames,nullptr,p9t.decoder);else decode_semantic_templates(p9_decoded,p9t.decoder);p9t.decode_seconds+=elapsed(d9t);verify_definitions(new_ids,truth,p9t.decoder);
        if(use_source_dictionary){const auto d9td=Clock::now();if(p9td_use_span)decode_p9_token_span(p9td_span_frames,&token_dictionary_decoder_frames,p9td.decoder);else decode_semantic_templates(p9_decoded,p9td.decoder);p9td.decode_seconds+=elapsed(d9td);verify_definitions(new_ids,truth,p9td.decoder);}

        const auto e9g=Clock::now();ParamSpanStats inline_param_stats,columnar_param_stats;TokenSpanRaw p9g_inline_raw=encode_param_span_pack(p9_selected->raw_ids,truth,8192,false,inline_param_stats),p9g_columnar_raw=encode_param_span_pack(p9_selected->raw_ids,truth,8192,true,columnar_param_stats);TokenSpanFrames p9g_inline_frames=compress_token_span_pack(p9g_inline_raw,frames,zlevel,100,32,0,false),p9g_columnar_frames=compress_token_span_pack(p9g_columnar_raw,frames,zlevel,100,32,0,false);const bool use_columnar=p9g_columnar_frames.valid&&(!p9g_inline_frames.valid||p9g_columnar_frames.wire<p9g_inline_frames.wire);TokenSpanFrames p9g_span_frames=use_columnar?std::move(p9g_columnar_frames):std::move(p9g_inline_frames);const ParamSpanStats&current_param_stats=use_columnar?columnar_param_stats:inline_param_stats;const uint64_t p9_param_span_size=p9g_span_frames.valid?p9_without_raw+p9g_span_frames.wire:UINT64_MAX;const bool p9g_use_span=p9g_span_frames.valid&&p9_param_span_size<p9_frames.wire;const uint64_t w9g=std::min(p9_frames.wire,p9_param_span_size)+1;p9g.encode_seconds+=elapsed(e9g);
        param_span_stats.candidate_shapes+=current_param_stats.candidate_shapes;param_span_stats.selected_rules+=current_param_stats.selected_rules;param_span_stats.used_rules+=current_param_stats.used_rules;param_span_stats.rule_instances+=current_param_stats.rule_instances;param_span_stats.unique_instances+=current_param_stats.unique_instances;param_span_stats.program_records+=current_param_stats.program_records;param_span_stats.literal_records+=current_param_stats.literal_records;param_span_stats.covered_bytes+=current_param_stats.covered_bytes;param_span_stats.residual_bytes+=current_param_stats.residual_bytes;param_span_stats.target_bytes+=current_param_stats.target_bytes;param_span_stats.program_ops+=current_param_stats.program_ops;
        param_span_literal_wire+=p9_raw_wire;param_span_candidate_wire+=p9g_span_frames.valid?p9g_span_frames.wire:p9_raw_wire;param_span_selected_wire+=(p9g_use_span?p9g_span_frames.wire:p9_raw_wire)+1;if(p9g_use_span)++param_span_frame_wins;if(p9g_span_frames.valid&&p9g_span_frames.combined)++param_span_combined_wins;if(use_columnar)++param_span_columnar_wins;
        const auto d9g=Clock::now();if(p9g_use_span){SemanticBuffer decoded=decompress_semantic(p9_frames,frames);const std::vector<uint64_t>lengths=semantic_raw_lengths(decoded.control);decoded.payload[kRawDefinition]=decode_implicit_param_span_pack(p9g_span_frames,frames,lengths);decode_semantic_templates(decoded,p9g.decoder);}else decode_semantic_templates(p9_decoded,p9g.decoder);p9g.decode_seconds+=elapsed(d9g);verify_definitions(new_ids,truth,p9g.decoder);

        TokenSpanFrames p9h_span_frames;SemanticFrames p16_frames;
        uint64_t p9_pretrained_param_size=UINT64_MAX,p16_candidate_size=UINT64_MAX,w14=0,w16=0;
        bool p9h_use_span=false,p16_use_fused=false;
        auto decode_p9_frozen_param=[&](DecoderStore&decoder){SemanticBuffer decoded=decompress_semantic(p9_frames,frames);
            const std::vector<uint64_t>lengths=semantic_raw_lengths(decoded.control);
            decoded.payload[kRawDefinition]=decode_frozen_param_span_pack(p9h_span_frames,frames,pretrained_param_decoder_frames,pretrained_param_receiver_rules,lengths);
            decode_semantic_templates(decoded,decoder);};
        auto decode_p16_fused=[&](DecoderStore&decoder){decode_fused_frozen_semantic(decompress_semantic(p16_frames,frames),pretrained_param_receiver_rules,decoder);};
        if(use_param_pretrained){const auto e9h=Clock::now();ParamSpanStats inline_stats,columnar_stats;
            std::vector<ParamSpanRecord>planned_records;const auto shared_plan_begin=Clock::now();
            TokenSpanRaw inline_raw=encode_frozen_param_span_pack(p9_selected->raw_ids,truth,pretrained_param,false,inline_stats,&planned_records);
            const double shared_plan_seconds=elapsed(shared_plan_begin);
            TokenSpanRaw columnar_raw=encode_frozen_param_span_pack(p9_selected->raw_ids,truth,pretrained_param,true,columnar_stats);
            TokenSpanFrames inline_frames=compress_token_span_pack(inline_raw,frames,zlevel,100,32,0,false,&pretrained_param_encoder_frames);
            TokenSpanFrames columnar_frames=compress_token_span_pack(columnar_raw,frames,zlevel,100,32,0,false,&pretrained_param_encoder_frames);
            const bool param_columnar=columnar_frames.valid&&(!inline_frames.valid||columnar_frames.wire<inline_frames.wire);
            p9h_span_frames=param_columnar?std::move(columnar_frames):std::move(inline_frames);
            const ParamSpanStats&current=param_columnar?columnar_stats:inline_stats;
            p9_pretrained_param_size=p9h_span_frames.valid?p9_without_raw+p9h_span_frames.wire:UINT64_MAX;
            p9h_use_span=p9h_span_frames.valid&&p9_pretrained_param_size<p9_frames.wire;
            w14=std::min<uint64_t>(p9_frames.wire,p9_pretrained_param_size)+1;p14.encode_seconds+=elapsed(e9h);
            pretrained_param_stats.used_rules+=current.used_rules;pretrained_param_stats.rule_instances+=current.rule_instances;
            pretrained_param_stats.unique_instances+=current.unique_instances;pretrained_param_stats.program_records+=current.program_records;
            pretrained_param_stats.literal_records+=current.literal_records;pretrained_param_stats.covered_bytes+=current.covered_bytes;
            pretrained_param_stats.residual_bytes+=current.residual_bytes;pretrained_param_stats.target_bytes+=current.target_bytes;
            pretrained_param_stats.program_ops+=current.program_ops;pretrained_param_stats.lexicon_entries+=current.lexicon_entries;
            pretrained_param_stats.lexicon_references+=current.lexicon_references;pretrained_param_literal_wire+=p9_raw_wire;
            pretrained_param_candidate_wire+=p9h_span_frames.valid?p9h_span_frames.wire:p9_raw_wire;
            pretrained_param_selected_wire+=(p9h_use_span?p9h_span_frames.wire:p9_raw_wire)+1;
            if(p9h_use_span)++pretrained_param_frame_wins;
            if(p9h_span_frames.valid&&p9h_span_frames.combined)++pretrained_param_combined_wins;
            if(param_columnar)++pretrained_param_columnar_wins;
            const auto d9h=Clock::now();if(p9h_use_span)decode_p9_frozen_param(p14.decoder);else decode_semantic_templates(p9_decoded,p14.decoder);
            p14.decode_seconds+=elapsed(d9h);verify_definitions(new_ids,truth,p14.decoder);

            const auto e16=Clock::now();const FrozenSemanticParamRaw semantic_program=encode_frozen_semantic_param_pack(planned_records,truth,pretrained_param);
            if(semantic_program.valid){const SemanticBuffer fused= fuse_frozen_semantic_param(*p9_selected,semantic_program);
                p16_frames=compress_semantic(fused,frames,zlevel);p16_candidate_size=p16_frames.wire;
                p16_use_fused=p16_candidate_size<p9_frames.wire;}
            p16.encode_seconds+=shared_plan_seconds+elapsed(e16);w16=std::min<uint64_t>(p9_frames.wire,p16_candidate_size)+1;
            if(p16_candidate_size!=UINT64_MAX)fused_param_candidate_wire+=p16_candidate_size;
            else fused_param_candidate_wire+=p9_frames.wire;
            fused_param_selected_wire+=(p16_use_fused?p16_candidate_size:p9_frames.wire)+1;
            if(p16_use_fused)++fused_param_frame_wins;
            const auto d16=Clock::now();if(p16_use_fused)decode_p16_fused(p16.decoder);
            else decode_semantic_templates(p9_decoded,p16.decoder);
            p16.decode_seconds+=elapsed(d16);verify_definitions(new_ids,truth,p16.decoder);
        }

        uint64_t w18=0;if(use_statement_model){const auto e18=Clock::now();StatementProgramStats inline_statement_stats,columnar_statement_stats;
            const FrozenSemanticParamRaw inline_statement_raw=encode_frozen_statement_pack(p9_selected->payload[kRawDefinition],pretrained_param,false,inline_statement_stats);
            const FrozenSemanticParamRaw columnar_statement_raw=encode_frozen_statement_pack(p9_selected->payload[kRawDefinition],pretrained_param,true,columnar_statement_stats);
            FrozenStatementFrames inline_statement_frames=compress_frozen_statement_pack(inline_statement_raw,frames,zlevel);
            FrozenStatementFrames columnar_statement_frames=compress_frozen_statement_pack(columnar_statement_raw,frames,zlevel);
            const bool statement_columnar=columnar_statement_frames.valid&&(!inline_statement_frames.valid||columnar_statement_frames.wire<inline_statement_frames.wire);
            const FrozenStatementFrames statement_frames=statement_columnar?std::move(columnar_statement_frames):std::move(inline_statement_frames);
            const StatementProgramStats&current_statement_stats=statement_columnar?columnar_statement_stats:inline_statement_stats;
            const uint64_t candidate=statement_frames.valid?p9_without_raw+statement_frames.wire:UINT64_MAX;
            const bool use_statement=statement_frames.valid&&candidate<p9_frames.wire;w18=std::min<uint64_t>(p9_frames.wire,candidate)+1;p18.encode_seconds+=elapsed(e18);
            statement_stats.units+=current_statement_stats.units;statement_stats.candidate_windows+=current_statement_stats.candidate_windows;
            statement_stats.matched_windows+=current_statement_stats.matched_windows;statement_stats.rule_ops+=current_statement_stats.rule_ops;
            statement_stats.raw_ops+=current_statement_stats.raw_ops;statement_stats.covered_bytes+=current_statement_stats.covered_bytes;
            statement_stats.residual_bytes+=current_statement_stats.residual_bytes;statement_stats.used_rules+=current_statement_stats.used_rules;
            statement_stats.lexicon_entries+=current_statement_stats.lexicon_entries;statement_stats.lexicon_references+=current_statement_stats.lexicon_references;
            statement_literal_wire+=p9_raw_wire;statement_candidate_wire+=statement_frames.valid?statement_frames.wire:p9_raw_wire;
            statement_selected_wire+=(use_statement?statement_frames.wire:p9_raw_wire)+1;if(use_statement)++statement_frame_wins;
            const auto d18=Clock::now();if(use_statement){const FrozenSemanticParamRaw receiver_raw=decompress_frozen_statement_pack(statement_frames,frames);
                const std::vector<uint8_t>reconstructed=decode_frozen_statement_pack(receiver_raw,pretrained_param_receiver_rules);
                if(reconstructed!=p9_selected->payload[kRawDefinition])die_msg("frozen statement raw-channel mismatch");
                SemanticBuffer decoded=p9_decoded;
                decoded.payload[kRawDefinition]=reconstructed;decode_semantic_templates(decoded,p18.decoder);
            }else decode_semantic_templates(p9_decoded,p18.decoder);
            p18.decode_seconds+=elapsed(d18);verify_definitions(new_ids,truth,p18.decoder);
        }

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

        const auto e7p=Clock::now();
        SourcePackFrames p7p_frames;size_t p7p_threshold_index=0;
        for(size_t threshold_index=0;threshold_index<kSourcePackThresholds.size();++threshold_index){
            const unsigned threshold=kSourcePackThresholds[threshold_index];
            SourcePackRaw candidate_raw=encode_source_pack(new_ids,truth,source_meta,current_locations,source_catalog,threshold);
            SourcePackFrames candidate=compress_source_pack(candidate_raw,frames,zlevel,threshold);
            if(candidate.valid){++source_pack_stats.threshold_tus[threshold_index];source_pack_stats.threshold_wire[threshold_index]+=candidate.wire;
                source_pack_stats.threshold_basis_bytes[threshold_index]+=candidate.stats.basis_bytes;source_pack_stats.threshold_residual_bytes[threshold_index]+=candidate.stats.residual_bytes;}
            if(candidate.valid&&(!p7p_frames.valid||candidate.wire<p7p_frames.wire)){p7p_frames=std::move(candidate);p7p_threshold_index=threshold_index;}
        }
        const uint64_t p1_wire=p1_frame.size()+4,p7p_size=p7p_frames.valid?p7p_frames.wire:UINT64_MAX;
        const bool p7p_use_source=p7p_frames.valid&&p7p_size<p1_wire;const uint64_t w7p=std::min(p1_wire,p7p_size)+1;p7p.encode_seconds+=elapsed(e7p);
        source_pack_stats.literal_wire+=p1_wire;source_pack_stats.selected_wire+=w7p;
        if(p7p_frames.valid){
            ++source_pack_stats.candidate_tus;++source_pack_stats.threshold_wins[p7p_threshold_index];source_pack_stats.source_wire+=p7p_size;
            if(p7p_use_source)++source_pack_stats.frame_wins;
            source_pack_stats.selected.available_records+=p7p_frames.stats.available_records;source_pack_stats.selected.available_bytes+=p7p_frames.stats.available_bytes;
            source_pack_stats.selected.source_records+=p7p_frames.stats.source_records;source_pack_stats.selected.literal_records+=p7p_frames.stats.literal_records;
            source_pack_stats.selected.basis_records+=p7p_frames.stats.basis_records;source_pack_stats.selected.basis_bytes+=p7p_frames.stats.basis_bytes;
            source_pack_stats.selected.residual_bytes+=p7p_frames.stats.residual_bytes;source_pack_stats.selected.target_bytes+=p7p_frames.stats.target_bytes;
            account_source_pack_layout(source_pack_stats,p7p_frames);
        }
        const auto d7p=Clock::now();
        if(p7p_use_source)decode_source_pack(p7p_frames,frames,p7p.decoder);else decode_literals(frames.decompress(p1_frame),p7p.decoder);
        p7p.decode_seconds+=elapsed(d7p);verify_definitions(new_ids,truth,p7p.decoder);

        const auto all_token_plan_begin=Clock::now();const std::vector<TokenSpanPlan>all_token_plans=build_token_span_plans(new_ids,truth,source_meta,current_locations,source_catalog);TokenSpanFrames p7t_span_frames;double p7t_tu_encode=elapsed(all_token_plan_begin);
        for(size_t atom_index=0;atom_index<kTokenSpanAtoms.size();++atom_index){for(size_t threshold_index=0;threshold_index<kSourcePackThresholds.size();++threshold_index){for(unsigned order=0;order<2;++order){
            const auto raw_begin=Clock::now();TokenSpanRaw candidate_raw=encode_token_span_pack(all_token_plans,truth,kSourcePackThresholds[threshold_index],kTokenSpanAtoms[atom_index],kTokenSpanMinBytes[atom_index],order!=0,true);p7t_tu_encode+=elapsed(raw_begin);
            const auto compress_begin=Clock::now();TokenSpanFrames candidate=compress_token_span_pack(candidate_raw,frames,zlevel,kSourcePackThresholds[threshold_index],kTokenSpanAtoms[atom_index],kTokenSpanMinBytes[atom_index],order!=0);p7t_tu_encode+=elapsed(compress_begin);
            const size_t sweep_index=token_span_sweep_index(atom_index,threshold_index,order);all_token_span_sweep.wire[sweep_index]+=candidate.valid?candidate.wire:p1_wire;
            if(candidate.valid){++all_token_span_sweep.candidate_tus[sweep_index];if(candidate.wire<p1_wire)++all_token_span_sweep.wins[sweep_index];all_token_span_sweep.basis_bytes[sweep_index]+=candidate.stats.basis_bytes;all_token_span_sweep.copy_bytes[sweep_index]+=candidate.stats.copy_bytes;all_token_span_sweep.residual_bytes[sweep_index]+=candidate.stats.residual_bytes;all_token_span_sweep.source_records[sweep_index]+=candidate.stats.source_records;}
            if(candidate.valid&&(!p7t_span_frames.valid||candidate.wire<p7t_span_frames.wire))p7t_span_frames=std::move(candidate);
        }}}
        const uint64_t p7t_size=p7t_span_frames.valid?p7t_span_frames.wire:UINT64_MAX;const bool p7t_use_span=p7t_span_frames.valid&&p7t_size<p1_wire;const uint64_t w7t=std::min(p1_wire,p7t_size)+1;p7t.encode_seconds+=p7t_tu_encode;accumulate_token_span(all_token_span_stats,p7t_span_frames,p7t_use_span,p1_wire);
        const auto d7t=Clock::now();if(p7t_use_span)decode_explicit_token_span_pack(p7t_span_frames,frames,p7t.decoder);else decode_literals(frames.decompress(p1_frame),p7t.decoder);p7t.decode_seconds+=elapsed(d7t);verify_definitions(new_ids,truth,p7t.decoder);

        const auto e7=Clock::now();const SplitBuffer p7_raw=encode_persistent_templates(new_ids,truth,persistent_encoder,persistent_stats);const std::vector<uint8_t>p7_control_frame=frames.compress(p7_raw.control,zlevel);const std::vector<uint8_t>p7_data_frame=frames.compress(p7_raw.data,zlevel);const uint64_t w7=p7_control_frame.size()+p7_data_frame.size()+8;p7.encode_seconds+=elapsed(e7);
        p7_control_wire+=p7_control_frame.size()+4;p7_data_wire+=p7_data_frame.size()+4;
        const auto d7=Clock::now();const std::vector<uint8_t>p7_control_decoded=frames.decompress(p7_control_frame);const std::vector<uint8_t>p7_data_decoded=frames.decompress(p7_data_frame);decode_persistent_templates(p7_control_decoded,p7_data_decoded,persistent_decoder);p7.decode_seconds+=elapsed(d7);verify_definitions(new_ids,truth,persistent_decoder.lines);

        hybrid_encoder.rule_id.clear();hybrid_encoder.rules.clear();hybrid_decoder.rules.clear();const auto e7b=Clock::now();const SplitBuffer p7b_raw=encode_persistent_templates(new_ids,truth,hybrid_encoder,hybrid_stats);const std::vector<uint8_t>p7b_control_frame=frames.compress(p7b_raw.control,zlevel),p7b_data_frame=frames.compress(p7b_raw.data,zlevel);const uint64_t w7b=p7b_control_frame.size()+p7b_data_frame.size()+8;p7b.encode_seconds+=elapsed(e7b);
        const auto d7b=Clock::now();const std::vector<uint8_t>p7b_control_decoded=frames.decompress(p7b_control_frame),p7b_data_decoded=frames.decompress(p7b_data_frame);decode_persistent_templates(p7b_control_decoded,p7b_data_decoded,hybrid_decoder);p7b.decode_seconds+=elapsed(d7b);verify_definitions(new_ids,truth,hybrid_decoder.lines);

        uint64_t w8=0;
        if(use_pretrained){const auto e8=Clock::now();const SplitBuffer p8_raw=encode_persistent_templates(new_ids,truth,pretrained.encoder,pretrained_stats,pretrained.rule_count(),pretrained.token_count(),pretrain_all_identifiers);const PackedFrame p8_control_frame=model_encoder_frames.compress_best(p8_raw.control,zlevel),p8_data_frame=model_encoder_frames.compress_best(p8_raw.data,zlevel);w8=p8_control_frame.bytes.size()+p8_data_frame.bytes.size()+10;p8.encode_seconds+=elapsed(e8);
            const auto d8=Clock::now();const std::vector<uint8_t>p8_control_decoded=model_decoder_frames.decompress(p8_control_frame),p8_data_decoded=model_decoder_frames.decompress(p8_data_frame);decode_persistent_templates(p8_control_decoded,p8_data_decoded,pretrained.decoder);p8.decode_seconds+=elapsed(d8);verify_definitions(new_ids,truth,pretrained.decoder.lines);}

        uint64_t w8c=0;PackedFrame p8c_control_frame,p8c_data_frame;
        if(use_pretrained){const auto e8c=Clock::now();PersistentEncoder local_encoder=frozen_pretrained.encoder;const SplitBuffer p8c_raw=encode_persistent_templates(new_ids,truth,local_encoder,pretrained_local_stats,frozen_pretrained.rule_count(),frozen_pretrained.token_count(),pretrain_all_identifiers);p8c_control_frame=model_local_superblock_encoder_frames.compress_best(p8c_raw.control,zlevel);p8c_data_frame=model_local_superblock_encoder_frames.compress_best(p8c_raw.data,zlevel);w8c=p8c_control_frame.bytes.size()+p8c_data_frame.bytes.size()+10;p8c.encode_seconds+=elapsed(e8c);}
        auto decode_p8c=[&](DecoderStore&target){PersistentDecoder local_decoder=frozen_pretrained.decoder;const std::vector<uint8_t>control=model_local_superblock_decoder_frames.decompress(p8c_control_frame),data=model_local_superblock_decoder_frames.decompress(p8c_data_frame);decode_persistent_templates(control,data,local_decoder);verify_definitions(new_ids,truth,local_decoder.lines);for(uint32_t id:new_ids)target.install(id,std::move(local_decoder.lines.lines[id]));};
        if(use_pretrained){const auto d8c=Clock::now();decode_p8c(p8c.decoder);p8c.decode_seconds+=elapsed(d8c);verify_definitions(new_ids,truth,p8c.decoder);}

        const uint64_t w1=p1_frame.size()+4,w5=p5_size,w6=p6_frame.size()+4;
        const size_t best_size=std::min({size_t(w1),p4_size,size_t(w5),size_t(w6),size_t(w7s),size_t(p7p_size),size_t(p9_frames.wire),size_t(p9_source_size)});
        const auto d10 = Clock::now();
        if(best_size==p4_size)decode_templates(p4_control_decoded,p4_data_decoded,p10.decoder);
        else if(best_size==p9_frames.wire)decode_semantic_templates(p9_decoded,p10.decoder);
        else if(p9s_source_frames.valid&&best_size==p9_source_size)decode_p9_source(p9s_source_frames,nullptr,p10.decoder);
        else if(best_size==p5_size)decode_multiline(p5_control_decoded,p5_data_decoded,p10.decoder);
        else if(best_size==p6_frame.size()+4)decode_forest(frames.decompress(p6_frame),p10.decoder);
        else if(best_size==w7s)decode_source_variants(p7s_decoded,p10.decoder);
        else if(p7p_frames.valid&&best_size==p7p_size)decode_source_pack(p7p_frames,frames,p10.decoder);
        else decode_literals(frames.decompress(p1_frame), p10.decoder);
        p10.decode_seconds += elapsed(d10);
        verify_definitions(new_ids, truth, p10.decoder);

        uint64_t w10p=0;
        if(use_pretrained){const size_t best_pretrained=std::min({size_t(w1),size_t(w8d),size_t(w5),size_t(w6),size_t(w7s),size_t(p7p_size),size_t(p9_frames.wire),size_t(p9_source_size)});const auto d10p=Clock::now();if(best_pretrained==w8d)decode_templates(p8d_control_decoded,p8d_data_decoded,p10p.decoder);else if(best_pretrained==p9_frames.wire)decode_semantic_templates(p9_decoded,p10p.decoder);else if(p9s_source_frames.valid&&best_pretrained==p9_source_size)decode_p9_source(p9s_source_frames,nullptr,p10p.decoder);else if(best_pretrained==w5)decode_multiline(p5_control_decoded,p5_data_decoded,p10p.decoder);else if(best_pretrained==w6)decode_forest(frames.decompress(p6_frame),p10p.decoder);else if(best_pretrained==w7s)decode_source_variants(p7s_decoded,p10p.decoder);else if(p7p_frames.valid&&best_pretrained==p7p_size)decode_source_pack(p7p_frames,frames,p10p.decoder);else decode_literals(frames.decompress(p1_frame),p10p.decoder);p10p.decode_seconds+=elapsed(d10p);verify_definitions(new_ids,truth,p10p.decoder);w10p=best_pretrained+1;}

        uint64_t w10sd=0;
        if(use_source_dictionary){const size_t best_source_dictionary=std::min({size_t(w1),p4_size,size_t(w5),size_t(w6),size_t(w7s),size_t(p7p_size),size_t(p9_frames.wire),size_t(p9_dictionary_source_size)});const auto d10sd=Clock::now();
            if(best_source_dictionary==p4_size)decode_templates(p4_control_decoded,p4_data_decoded,p10sd.decoder);
            else if(best_source_dictionary==p9_frames.wire)decode_semantic_templates(p9_decoded,p10sd.decoder);
            else if(p9sd_source_frames.valid&&best_source_dictionary==p9_dictionary_source_size)decode_p9_source(p9sd_source_frames,&source_dictionary_decoder_frames,p10sd.decoder);
            else if(best_source_dictionary==w5)decode_multiline(p5_control_decoded,p5_data_decoded,p10sd.decoder);
            else if(best_source_dictionary==w6)decode_forest(frames.decompress(p6_frame),p10sd.decoder);
            else if(best_source_dictionary==w7s)decode_source_variants(p7s_decoded,p10sd.decoder);
            else if(p7p_frames.valid&&best_source_dictionary==p7p_size)decode_source_pack(p7p_frames,frames,p10sd.decoder);
            else decode_literals(frames.decompress(p1_frame),p10sd.decoder);
            p10sd.decode_seconds+=elapsed(d10sd);verify_definitions(new_ids,truth,p10sd.decoder);w10sd=best_source_dictionary+1;}

        const size_t best_token_span=std::min({size_t(w1),p4_size,size_t(w5),size_t(w6),size_t(w7s),size_t(p7p_size),size_t(p7t_size),size_t(p9_frames.wire),size_t(p9_source_size),size_t(p9_token_span_size)});const auto d11=Clock::now();
        if(best_token_span==p9_token_span_size)decode_p9_token_span(p9t_span_frames,nullptr,p11.decoder);
        else if(best_token_span==p7t_size)decode_explicit_token_span_pack(p7t_span_frames,frames,p11.decoder);
        else if(best_token_span==p4_size)decode_templates(p4_control_decoded,p4_data_decoded,p11.decoder);
        else if(best_token_span==p9_frames.wire)decode_semantic_templates(p9_decoded,p11.decoder);
        else if(p9s_source_frames.valid&&best_token_span==p9_source_size)decode_p9_source(p9s_source_frames,nullptr,p11.decoder);
        else if(best_token_span==p5_size)decode_multiline(p5_control_decoded,p5_data_decoded,p11.decoder);
        else if(best_token_span==p6_frame.size()+4)decode_forest(frames.decompress(p6_frame),p11.decoder);
        else if(best_token_span==w7s)decode_source_variants(p7s_decoded,p11.decoder);
        else if(p7p_frames.valid&&best_token_span==p7p_size)decode_source_pack(p7p_frames,frames,p11.decoder);
        else decode_literals(frames.decompress(p1_frame),p11.decoder);
        p11.decode_seconds+=elapsed(d11);verify_definitions(new_ids,truth,p11.decoder);const uint64_t w11=best_token_span+1;

        uint64_t w11d=0;if(use_source_dictionary){const size_t best_token_dictionary=std::min({size_t(w1),p4_size,size_t(w5),size_t(w6),size_t(w7s),size_t(p7p_size),size_t(p7t_size),size_t(p9_frames.wire),size_t(p9_dictionary_source_size),size_t(p9_dictionary_token_span_size)});const auto d11d=Clock::now();
            if(best_token_dictionary==p9_dictionary_token_span_size)decode_p9_token_span(p9td_span_frames,&token_dictionary_decoder_frames,p11d.decoder);
            else if(best_token_dictionary==p7t_size)decode_explicit_token_span_pack(p7t_span_frames,frames,p11d.decoder);
            else if(best_token_dictionary==p4_size)decode_templates(p4_control_decoded,p4_data_decoded,p11d.decoder);
            else if(best_token_dictionary==p9_frames.wire)decode_semantic_templates(p9_decoded,p11d.decoder);
            else if(p9sd_source_frames.valid&&best_token_dictionary==p9_dictionary_source_size)decode_p9_source(p9sd_source_frames,&source_dictionary_decoder_frames,p11d.decoder);
            else if(best_token_dictionary==p5_size)decode_multiline(p5_control_decoded,p5_data_decoded,p11d.decoder);
            else if(best_token_dictionary==p6_frame.size()+4)decode_forest(frames.decompress(p6_frame),p11d.decoder);
            else if(best_token_dictionary==w7s)decode_source_variants(p7s_decoded,p11d.decoder);
            else if(p7p_frames.valid&&best_token_dictionary==p7p_size)decode_source_pack(p7p_frames,frames,p11d.decoder);
            else decode_literals(frames.decompress(p1_frame),p11d.decoder);
            p11d.decode_seconds+=elapsed(d11d);verify_definitions(new_ids,truth,p11d.decoder);w11d=best_token_dictionary+1;}

        const size_t best_param_span=std::min({best_token_span,size_t(p9_param_span_size),size_t(p9_grammar_source_size)});
        auto decode_param_span_choice=[&](DecoderStore&decoder){
            if(best_param_span==p9_grammar_source_size)decode_p9_source(p9sg_source_frames,nullptr,decoder);
            else if(best_param_span==p9_param_span_size){SemanticBuffer decoded=decompress_semantic(p9_frames,frames);const std::vector<uint64_t>lengths=semantic_raw_lengths(decoded.control);decoded.payload[kRawDefinition]=decode_implicit_param_span_pack(p9g_span_frames,frames,lengths);decode_semantic_templates(decoded,decoder);}
            else if(best_token_span==p9_token_span_size)decode_p9_token_span(p9t_span_frames,nullptr,decoder);
            else if(best_token_span==p7t_size)decode_explicit_token_span_pack(p7t_span_frames,frames,decoder);
            else if(best_token_span==p4_size)decode_templates(p4_control_decoded,p4_data_decoded,decoder);
            else if(best_token_span==p9_frames.wire)decode_semantic_templates(p9_decoded,decoder);
            else if(p9s_source_frames.valid&&best_token_span==p9_source_size)decode_p9_source(p9s_source_frames,nullptr,decoder);
            else if(best_token_span==p5_size)decode_multiline(p5_control_decoded,p5_data_decoded,decoder);
            else if(best_token_span==p6_frame.size()+4)decode_forest(frames.decompress(p6_frame),decoder);
            else if(best_token_span==w7s)decode_source_variants(p7s_decoded,decoder);
            else if(p7p_frames.valid&&best_token_span==p7p_size)decode_source_pack(p7p_frames,frames,decoder);
            else decode_literals(frames.decompress(p1_frame),decoder);
        };
        const auto d12=Clock::now();decode_param_span_choice(p12.decoder);
        p12.decode_seconds+=elapsed(d12);verify_definitions(new_ids,truth,p12.decoder);const uint64_t w12=best_param_span+1;

        size_t best_pretrained_superblock=best_param_span;uint64_t w13=0;
        if(use_pretrained){best_pretrained_superblock=std::min({best_param_span,size_t(w8c),size_t(w8d),size_t(p9_pretrained_source_size)});const auto d13=Clock::now();
            size_t choice=0;if(best_pretrained_superblock==p9_pretrained_source_size){choice=3;decode_p9_source(p9sm_source_frames,&pretrained_source_decoder_frames,p13.decoder,&frozen_pretrained.decoder);}
            else if(best_pretrained_superblock==w8c){choice=1;decode_p8c(p13.decoder);}
            else if(best_pretrained_superblock==w8d){choice=2;decode_templates(p8d_control_decoded,p8d_data_decoded,p13.decoder);}
            else decode_param_span_choice(p13.decoder);
            ++p13_choice_tus[choice];p13_choice_wire[choice]+=best_pretrained_superblock;
            p13.decode_seconds+=elapsed(d13);verify_definitions(new_ids,truth,p13.decoder);w13=best_pretrained_superblock+1;}

        uint64_t w15=0,w17=0;if(use_param_pretrained){const size_t best_all_pretrained=std::min(best_pretrained_superblock,size_t(p9_pretrained_param_size));
            const auto d15=Clock::now();
            if(best_all_pretrained==p9_pretrained_param_size)decode_p9_frozen_param(p15.decoder);
            else if(use_pretrained&&best_all_pretrained==p9_pretrained_source_size)decode_p9_source(p9sm_source_frames,&pretrained_source_decoder_frames,p15.decoder,&frozen_pretrained.decoder);
            else if(use_pretrained&&best_all_pretrained==w8c)decode_p8c(p15.decoder);
            else if(use_pretrained&&best_all_pretrained==w8d)decode_templates(p8d_control_decoded,p8d_data_decoded,p15.decoder);
            else decode_param_span_choice(p15.decoder);
            p15.decode_seconds+=elapsed(d15);verify_definitions(new_ids,truth,p15.decoder);w15=best_all_pretrained+1;

            const size_t best_fused_pretrained=std::min(best_all_pretrained,size_t(p16_candidate_size));const auto d17=Clock::now();
            if(best_fused_pretrained==p16_candidate_size)decode_p16_fused(p17.decoder);
            else if(best_fused_pretrained==p9_pretrained_param_size)decode_p9_frozen_param(p17.decoder);
            else if(use_pretrained&&best_fused_pretrained==p9_pretrained_source_size)decode_p9_source(p9sm_source_frames,&pretrained_source_decoder_frames,p17.decoder,&frozen_pretrained.decoder);
            else if(use_pretrained&&best_fused_pretrained==w8c)decode_p8c(p17.decoder);
            else if(use_pretrained&&best_fused_pretrained==w8d)decode_templates(p8d_control_decoded,p8d_data_decoded,p17.decoder);
            else decode_param_span_choice(p17.decoder);
            p17.decode_seconds+=elapsed(d17);verify_definitions(new_ids,truth,p17.decoder);w17=best_fused_pretrained+1;}

        const uint64_t w0 = p0_frame.size() + 4;
        const uint64_t w4 = p4_size;
        const uint64_t w9=p9_frames.wire;
        const uint64_t w10 = std::min({w1,w4,w5,w6,w7s,p7p_size,w9,p9_source_size}) + 1;
        std::vector<uint64_t>current{w0,w1,w4,w5,w6,w7s,w7p,w7t,w7,w7b,w9,w9s,w9sg};if(use_pretrained)current.push_back(w9sm);if(use_source_dictionary)current.push_back(w9sd);current.push_back(w9t);if(use_source_dictionary)current.push_back(w9td);current.push_back(w9g);if(use_pretrained){current.push_back(w8);current.push_back(w8c);current.push_back(w8d);}current.push_back(w10);if(use_source_dictionary)current.push_back(w10sd);if(use_pretrained)current.push_back(w10p);current.push_back(w11);if(use_source_dictionary)current.push_back(w11d);current.push_back(w12);if(use_pretrained)current.push_back(w13);if(use_param_pretrained){current.push_back(w14);current.push_back(w15);current.push_back(w16);current.push_back(w17);}if(use_statement_model)current.push_back(w18);
        for (size_t r = 0; r < rows.size(); ++r) {rows[r]->wire += current[r];rows[r]->tu_wire.push_back(current[r]);}
        p10.encode_seconds = p1.encode_seconds + p4.encode_seconds+p5.encode_seconds+p6.encode_seconds+p7s.encode_seconds+p7p.encode_seconds+p9.encode_seconds+p9s.encode_seconds;
        p11.encode_seconds=p10.encode_seconds+p7t.encode_seconds+p9t.encode_seconds;
        if(use_source_dictionary)p11d.encode_seconds=p10sd.encode_seconds+p7t.encode_seconds+p9td.encode_seconds;
        p12.encode_seconds=p11.encode_seconds+p9g.encode_seconds+p9sg.encode_seconds;
        if(use_pretrained){p10p.encode_seconds=p1.encode_seconds+p8d.encode_seconds+p5.encode_seconds+p6.encode_seconds+p7s.encode_seconds+p7p.encode_seconds+p9.encode_seconds+p9s.encode_seconds;p13.encode_seconds=p12.encode_seconds+p8c.encode_seconds+p8d.encode_seconds+p9sm.encode_seconds;}
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

    // P9s reuses all of P9 and adds an actual-cost source basis over only P9's raw fallback channel.
    p9s.encode_seconds+=p9.encode_seconds;
    p9sg.encode_seconds+=p9.encode_seconds;
    if(use_pretrained)p9sm.encode_seconds+=p9.encode_seconds;
    p9t.encode_seconds+=p9.encode_seconds;
    p10.encode_seconds=p1.encode_seconds+p4.encode_seconds+p5.encode_seconds+p6.encode_seconds+p7s.encode_seconds+p7p.encode_seconds+p9s.encode_seconds;
    p11.encode_seconds=p10.encode_seconds+p7t.encode_seconds+p9t.encode_seconds;
    p12.encode_seconds=p11.encode_seconds+p9g.encode_seconds+p9sg.encode_seconds;
    if(use_source_dictionary){p9sd.encode_seconds+=p9.encode_seconds;p9td.encode_seconds+=p9.encode_seconds;p10sd.encode_seconds=p1.encode_seconds+p4.encode_seconds+p5.encode_seconds+p6.encode_seconds+p7s.encode_seconds+p7p.encode_seconds+p9sd.encode_seconds;p11d.encode_seconds=p10sd.encode_seconds+p7t.encode_seconds+p9td.encode_seconds;}

    // P8d reuses the P4 transform output in this all-row harness.  Charge the transform work as
    // well as its additional dictionary/plain actual-frame comparison before reporting throughput.
    if(use_pretrained){p8d.encode_seconds+=p4.encode_seconds;p10p.encode_seconds=p1.encode_seconds+p8d.encode_seconds+p5.encode_seconds+p6.encode_seconds+p7s.encode_seconds+p7p.encode_seconds+p9s.encode_seconds;p13.encode_seconds=p12.encode_seconds+p8c.encode_seconds+p8d.encode_seconds+p9sm.encode_seconds;}
    if(use_param_pretrained){const double parameterized_transform_seconds=p14.encode_seconds;
        const double fused_transform_seconds=p16.encode_seconds;
        p14.encode_seconds+=p9.encode_seconds;
        p16.encode_seconds+=p9.encode_seconds;
        p15.encode_seconds=p12.encode_seconds+parameterized_transform_seconds;
        p17.encode_seconds=p12.encode_seconds+parameterized_transform_seconds+fused_transform_seconds;
        if(use_pretrained){p15.encode_seconds+=p8c.encode_seconds+p8d.encode_seconds+p9sm.encode_seconds;
            p17.encode_seconds+=p8c.encode_seconds+p8d.encode_seconds+p9sm.encode_seconds;}}

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
    std::printf("P7p source-superblock candidate-TUs=%llu frame-wins=%llu available=%llu/%.2f MiB selected source/literal=%llu/%llu basis=%llu/%.2f MiB residual=%.2f MiB target=%.2f MiB\n",
                static_cast<unsigned long long>(source_pack_stats.candidate_tus),static_cast<unsigned long long>(source_pack_stats.frame_wins),
                static_cast<unsigned long long>(source_pack_stats.selected.available_records),source_pack_stats.selected.available_bytes/1048576.0,
                static_cast<unsigned long long>(source_pack_stats.selected.source_records),static_cast<unsigned long long>(source_pack_stats.selected.literal_records),
                static_cast<unsigned long long>(source_pack_stats.selected.basis_records),source_pack_stats.selected.basis_bytes/1048576.0,
                source_pack_stats.selected.residual_bytes/1048576.0,source_pack_stats.selected.target_bytes/1048576.0);
    std::printf("P7p wire literal/source/selected=%.3f/%.3f/%.3f MiB split frames basis-control/basis-data/definition-control/residual=%.3f/%.3f/%.3f/%.3f MiB combined-wins/wire=%llu/%.3f MiB threshold-wins 0/6/12/25/50/100%%=%llu/%llu/%llu/%llu/%llu/%llu\n",
                source_pack_stats.literal_wire/1048576.0,source_pack_stats.source_wire/1048576.0,source_pack_stats.selected_wire/1048576.0,
                source_pack_stats.frame_wire[0]/1048576.0,source_pack_stats.frame_wire[1]/1048576.0,source_pack_stats.frame_wire[2]/1048576.0,source_pack_stats.frame_wire[3]/1048576.0,
                static_cast<unsigned long long>(source_pack_stats.combined_wins),source_pack_stats.combined_wire/1048576.0,
                static_cast<unsigned long long>(source_pack_stats.threshold_wins[0]),static_cast<unsigned long long>(source_pack_stats.threshold_wins[1]),
                static_cast<unsigned long long>(source_pack_stats.threshold_wins[2]),static_cast<unsigned long long>(source_pack_stats.threshold_wins[3]),
                static_cast<unsigned long long>(source_pack_stats.threshold_wins[4]),static_cast<unsigned long long>(source_pack_stats.threshold_wins[5]));
    for(size_t i=0;i<kSourcePackThresholds.size();++i){
        std::printf("  P7p threshold=%u%% candidate-TUs=%llu wire=%.3f MiB basis-raw=%.2f MiB residual-raw=%.2f MiB\n",kSourcePackThresholds[i],
                    static_cast<unsigned long long>(source_pack_stats.threshold_tus[i]),source_pack_stats.threshold_wire[i]/1048576.0,
                    source_pack_stats.threshold_basis_bytes[i]/1048576.0,source_pack_stats.threshold_residual_bytes[i]/1048576.0);
    }
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
    std::printf("P9s raw-channel source-superblock candidate-TUs=%llu frame-wins=%llu available=%llu/%.2f MiB selected source/literal=%llu/%llu basis=%llu/%.2f MiB residual=%.2f MiB target=%.2f MiB\n",
                static_cast<unsigned long long>(semantic_source_pack_stats.candidate_tus),static_cast<unsigned long long>(semantic_source_pack_stats.frame_wins),
                static_cast<unsigned long long>(semantic_source_pack_stats.selected.available_records),semantic_source_pack_stats.selected.available_bytes/1048576.0,
                static_cast<unsigned long long>(semantic_source_pack_stats.selected.source_records),static_cast<unsigned long long>(semantic_source_pack_stats.selected.literal_records),
                static_cast<unsigned long long>(semantic_source_pack_stats.selected.basis_records),semantic_source_pack_stats.selected.basis_bytes/1048576.0,
                semantic_source_pack_stats.selected.residual_bytes/1048576.0,semantic_source_pack_stats.selected.target_bytes/1048576.0);
    std::printf("P9s raw wire literal/source/selected=%.3f/%.3f/%.3f MiB split frames basis-control/basis-data/definition-control/residual=%.3f/%.3f/%.3f/%.3f MiB combined-wins/wire=%llu/%.3f MiB threshold-wins 0/6/12/25/50/100%%=%llu/%llu/%llu/%llu/%llu/%llu\n",
                semantic_source_pack_stats.literal_wire/1048576.0,semantic_source_pack_stats.source_wire/1048576.0,semantic_source_pack_stats.selected_wire/1048576.0,
                semantic_source_pack_stats.frame_wire[0]/1048576.0,semantic_source_pack_stats.frame_wire[1]/1048576.0,semantic_source_pack_stats.frame_wire[2]/1048576.0,semantic_source_pack_stats.frame_wire[3]/1048576.0,
                static_cast<unsigned long long>(semantic_source_pack_stats.combined_wins),semantic_source_pack_stats.combined_wire/1048576.0,
                static_cast<unsigned long long>(semantic_source_pack_stats.threshold_wins[0]),static_cast<unsigned long long>(semantic_source_pack_stats.threshold_wins[1]),
                static_cast<unsigned long long>(semantic_source_pack_stats.threshold_wins[2]),static_cast<unsigned long long>(semantic_source_pack_stats.threshold_wins[3]),
                static_cast<unsigned long long>(semantic_source_pack_stats.threshold_wins[4]),static_cast<unsigned long long>(semantic_source_pack_stats.threshold_wins[5]));
    for(size_t i=0;i<kSourcePackThresholds.size();++i){
        std::printf("  P9s threshold=%u%% candidate-TUs=%llu wire=%.3f MiB basis-raw=%.2f MiB residual-raw=%.2f MiB\n",kSourcePackThresholds[i],
                    static_cast<unsigned long long>(semantic_source_pack_stats.threshold_tus[i]),semantic_source_pack_stats.threshold_wire[i]/1048576.0,
                    semantic_source_pack_stats.threshold_basis_bytes[i]/1048576.0,semantic_source_pack_stats.threshold_residual_bytes[i]/1048576.0);
    }
    std::printf("P9sg source-basis grammar candidate-TUs=%llu frame-wins=%llu selected source/literal=%llu/%llu source-lines=%llu/%.2f MiB raw wire literal/grammar/selected=%.3f/%.3f/%.3f MiB split basis-control/basis-data/definition-control/residual wire=%.3f/%.3f/%.3f/%.3f MiB combined-wins/wire=%llu/%.3f MiB threshold-wins 0/6/12/25/50/100%%=%llu/%llu/%llu/%llu/%llu/%llu\n",
                static_cast<unsigned long long>(grammar_source_pack_stats.candidate_tus),static_cast<unsigned long long>(grammar_source_pack_stats.frame_wins),static_cast<unsigned long long>(grammar_source_pack_stats.selected.source_records),static_cast<unsigned long long>(grammar_source_pack_stats.selected.literal_records),static_cast<unsigned long long>(grammar_source_pack_stats.selected.basis_records),grammar_source_pack_stats.selected.basis_bytes/1048576.0,
                grammar_source_pack_stats.literal_wire/1048576.0,grammar_source_pack_stats.source_wire/1048576.0,grammar_source_pack_stats.selected_wire/1048576.0,grammar_source_pack_stats.frame_wire[0]/1048576.0,grammar_source_pack_stats.frame_wire[1]/1048576.0,grammar_source_pack_stats.frame_wire[2]/1048576.0,grammar_source_pack_stats.frame_wire[3]/1048576.0,static_cast<unsigned long long>(grammar_source_pack_stats.combined_wins),grammar_source_pack_stats.combined_wire/1048576.0,
                static_cast<unsigned long long>(grammar_source_pack_stats.threshold_wins[0]),static_cast<unsigned long long>(grammar_source_pack_stats.threshold_wins[1]),static_cast<unsigned long long>(grammar_source_pack_stats.threshold_wins[2]),static_cast<unsigned long long>(grammar_source_pack_stats.threshold_wins[3]),static_cast<unsigned long long>(grammar_source_pack_stats.threshold_wins[4]),static_cast<unsigned long long>(grammar_source_pack_stats.threshold_wins[5]));
    if(use_pretrained){std::printf("P9sm pretrained source-basis superblocks candidate-TUs=%llu frame-wins=%llu selected source/literal=%llu/%llu source-lines=%llu/%.2f MiB raw wire literal/model/selected=%.3f/%.3f/%.3f MiB split basis-control/basis-data/definition-control/residual=%.3f/%.3f/%.3f/%.3f MiB combined-wins/wire=%llu/%.3f MiB model-dictionary wins=%llu saved-before-charge=%.1f KiB\n",
                static_cast<unsigned long long>(pretrained_source_pack_stats.candidate_tus),static_cast<unsigned long long>(pretrained_source_pack_stats.frame_wins),static_cast<unsigned long long>(pretrained_source_pack_stats.selected.source_records),static_cast<unsigned long long>(pretrained_source_pack_stats.selected.literal_records),static_cast<unsigned long long>(pretrained_source_pack_stats.selected.basis_records),pretrained_source_pack_stats.selected.basis_bytes/1048576.0,
                pretrained_source_pack_stats.literal_wire/1048576.0,pretrained_source_pack_stats.source_wire/1048576.0,pretrained_source_pack_stats.selected_wire/1048576.0,pretrained_source_pack_stats.frame_wire[0]/1048576.0,pretrained_source_pack_stats.frame_wire[1]/1048576.0,pretrained_source_pack_stats.frame_wire[2]/1048576.0,pretrained_source_pack_stats.frame_wire[3]/1048576.0,static_cast<unsigned long long>(pretrained_source_pack_stats.combined_wins),pretrained_source_pack_stats.combined_wire/1048576.0,static_cast<unsigned long long>(pretrained_source_encoder_frames.dictionary_wins),pretrained_source_encoder_frames.dictionary_saved/1024.0);
        for(size_t i=0;i<kSourcePackThresholds.size();++i)std::printf("  P9sm threshold=%u%% candidate-TUs=%llu wire=%.3f MiB basis-raw=%.2f MiB residual-raw=%.2f MiB\n",kSourcePackThresholds[i],static_cast<unsigned long long>(pretrained_source_pack_stats.threshold_tus[i]),pretrained_source_pack_stats.threshold_wire[i]/1048576.0,pretrained_source_pack_stats.threshold_basis_bytes[i]/1048576.0,pretrained_source_pack_stats.threshold_residual_bytes[i]/1048576.0);}
    auto print_token_span_stats=[&](const char*name,const TokenSpanStats&stats){
        std::printf("%s contributing-token-span candidate-TUs=%llu frame-wins=%llu available=%llu/%.2f MiB selected span/literal=%llu/%llu basis=%llu/%.2f MiB copy=%.2f MiB residual=%.2f MiB ops=%llu target=%.2f MiB\n",name,
                    static_cast<unsigned long long>(stats.candidate_tus),static_cast<unsigned long long>(stats.frame_wins),static_cast<unsigned long long>(stats.selected.available_records),stats.selected.available_bytes/1048576.0,
                    static_cast<unsigned long long>(stats.selected.source_records),static_cast<unsigned long long>(stats.selected.literal_records),static_cast<unsigned long long>(stats.selected.basis_records),stats.selected.basis_bytes/1048576.0,
                    stats.selected.copy_bytes/1048576.0,stats.selected.residual_bytes/1048576.0,static_cast<unsigned long long>(stats.selected.program_ops),stats.selected.target_bytes/1048576.0);
        std::printf("%s raw wire literal/span/selected=%.3f/%.3f/%.3f MiB split frames basis-control/basis-data/program-control/residual=%.3f/%.3f/%.3f/%.3f MiB combined-wins/wire=%llu/%.3f MiB threshold-wins 0/6/12/25/50/100%%=%llu/%llu/%llu/%llu/%llu/%llu atom-cap-wins whole/2/4/8/16=%llu/%llu/%llu/%llu/%llu order source/lexical=%llu/%llu\n",name,
                    stats.literal_wire/1048576.0,stats.span_wire/1048576.0,stats.selected_wire/1048576.0,stats.frame_wire[0]/1048576.0,stats.frame_wire[1]/1048576.0,stats.frame_wire[2]/1048576.0,stats.frame_wire[3]/1048576.0,static_cast<unsigned long long>(stats.combined_wins),stats.combined_wire/1048576.0,
                    static_cast<unsigned long long>(stats.threshold_wins[0]),static_cast<unsigned long long>(stats.threshold_wins[1]),static_cast<unsigned long long>(stats.threshold_wins[2]),static_cast<unsigned long long>(stats.threshold_wins[3]),static_cast<unsigned long long>(stats.threshold_wins[4]),static_cast<unsigned long long>(stats.threshold_wins[5]),
                    static_cast<unsigned long long>(stats.atom_wins[0]),static_cast<unsigned long long>(stats.atom_wins[1]),static_cast<unsigned long long>(stats.atom_wins[2]),static_cast<unsigned long long>(stats.atom_wins[3]),static_cast<unsigned long long>(stats.atom_wins[4]),static_cast<unsigned long long>(stats.order_wins[0]),static_cast<unsigned long long>(stats.order_wins[1]));
    };
    auto print_token_span_sweep=[&](const char*name,const TokenSpanSweepStats&sweep){std::vector<size_t>order(kTokenSpanSweepSize);std::iota(order.begin(),order.end(),0);std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return sweep.wire[a]<sweep.wire[b];});
        std::printf("%s best fixed token-span configurations (raw-channel wire includes literal fallback for unavailable TUs):\n",name);
        for(size_t rank=0;rank<std::min<size_t>(12,order.size());++rank){const size_t index=order[rank],basis_order=index%2,packed=index/2,threshold_index=packed%kSourcePackThresholds.size(),atom_index=packed/kSourcePackThresholds.size();
            std::printf("  #%zu atoms=%u min=%u residual<=%u%% order=%s wire=%.3f MiB wins=%llu/%llu source=%llu basis/copy/residual=%.2f/%.2f/%.2f MiB\n",rank+1,kTokenSpanAtoms[atom_index],kTokenSpanMinBytes[atom_index],kSourcePackThresholds[threshold_index],basis_order?"lexical":"source",sweep.wire[index]/1048576.0,
                        static_cast<unsigned long long>(sweep.wins[index]),static_cast<unsigned long long>(sweep.candidate_tus[index]),static_cast<unsigned long long>(sweep.source_records[index]),sweep.basis_bytes[index]/1048576.0,sweep.copy_bytes[index]/1048576.0,sweep.residual_bytes[index]/1048576.0);}
    };
    print_token_span_stats("P7t",all_token_span_stats);
    print_token_span_sweep("P7t",all_token_span_sweep);
    print_token_span_stats("P9t",token_span_stats);
    print_token_span_sweep("P9t",token_span_sweep);
    std::printf("P9g parameterized token superblocks candidate-shapes=%llu selected/used-rules=%llu/%llu instances/unique=%llu/%llu program/literal-records=%llu/%llu covered/residual/target=%.2f/%.2f/%.2f MiB ops=%llu frame-wins=%llu combined/columnar-candidates=%llu/%llu raw-channel literal/candidate/selected=%.3f/%.3f/%.3f MiB\n",
                static_cast<unsigned long long>(param_span_stats.candidate_shapes),static_cast<unsigned long long>(param_span_stats.selected_rules),static_cast<unsigned long long>(param_span_stats.used_rules),static_cast<unsigned long long>(param_span_stats.rule_instances),static_cast<unsigned long long>(param_span_stats.unique_instances),
                static_cast<unsigned long long>(param_span_stats.program_records),static_cast<unsigned long long>(param_span_stats.literal_records),param_span_stats.covered_bytes/1048576.0,param_span_stats.residual_bytes/1048576.0,param_span_stats.target_bytes/1048576.0,static_cast<unsigned long long>(param_span_stats.program_ops),
                static_cast<unsigned long long>(param_span_frame_wins),static_cast<unsigned long long>(param_span_combined_wins),static_cast<unsigned long long>(param_span_columnar_wins),param_span_literal_wire/1048576.0,param_span_candidate_wire/1048576.0,param_span_selected_wire/1048576.0);
    if(use_param_pretrained)std::printf("P14 pretrained parameterized sub-superblocks input=%s training=%.2f GiB/%llu lines/%llu distinct sampled=%llu windows=%llu candidate-shapes=%llu in %.2fs selected-rules=%zu package raw/wire=%.1f/%.1f KiB model-id=%016llx; TU rule-sets=%llu instances/unique=%llu/%llu program/literal-records=%llu/%llu covered/residual/target=%.2f/%.2f/%.2f MiB ops=%llu lexicon entries/refs=%llu/%llu frame-wins=%llu combined/columnar-candidates=%llu/%llu raw-channel literal/candidate/selected=%.3f/%.3f/%.3f MiB model-dictionary wins=%llu saved-before-charge=%.1f KiB\n",
                load_param_model?(pretrained_param.unit_mode==kParamModelStatements?"loaded-statements":"loaded-package"):(param_statements?"source-statements":(param_coarse_lines?"coarse-lines":(param_pretrain_source_roots.empty()?"expanded-ii":(pretrain_manifests.empty()?"raw-source":"mixed")))),pretrained_param.training_raw/1073741824.0,static_cast<unsigned long long>(pretrained_param.training_lines),static_cast<unsigned long long>(pretrained_param.training_distinct_lines),static_cast<unsigned long long>(pretrained_param.sampled_lines),
                static_cast<unsigned long long>(pretrained_param.candidate_windows),static_cast<unsigned long long>(pretrained_param.candidate_rules),pretrained_param.training_seconds,pretrained_param.encoder_rules.size(),pretrained_param.raw.size()/1024.0,(pretrained_param.frame.size()+4)/1024.0,
                static_cast<unsigned long long>(hash_bytes(pretrained_param.raw.data(),uint32_t(pretrained_param.raw.size()))),
                static_cast<unsigned long long>(pretrained_param_stats.used_rules),static_cast<unsigned long long>(pretrained_param_stats.rule_instances),static_cast<unsigned long long>(pretrained_param_stats.unique_instances),static_cast<unsigned long long>(pretrained_param_stats.program_records),static_cast<unsigned long long>(pretrained_param_stats.literal_records),
                pretrained_param_stats.covered_bytes/1048576.0,pretrained_param_stats.residual_bytes/1048576.0,pretrained_param_stats.target_bytes/1048576.0,static_cast<unsigned long long>(pretrained_param_stats.program_ops),static_cast<unsigned long long>(pretrained_param_stats.lexicon_entries),static_cast<unsigned long long>(pretrained_param_stats.lexicon_references),static_cast<unsigned long long>(pretrained_param_frame_wins),static_cast<unsigned long long>(pretrained_param_combined_wins),static_cast<unsigned long long>(pretrained_param_columnar_wins),
                pretrained_param_literal_wire/1048576.0,pretrained_param_candidate_wire/1048576.0,pretrained_param_selected_wire/1048576.0,static_cast<unsigned long long>(pretrained_param_encoder_frames.dictionary_wins),pretrained_param_encoder_frames.dictionary_saved/1024.0);
    if(use_param_pretrained)std::printf("P16 fused pretrained parameter program candidate/selected=%.3f/%.3f MiB frame-wins=%llu/%zu; control and typed slot values share P9 semantic streams\n",
                fused_param_candidate_wire/1048576.0,fused_param_selected_wire/1048576.0,
                static_cast<unsigned long long>(fused_param_frame_wins),corpus.files.size());
    if(use_statement_model)std::printf("P18 portable statement/multi-statement program units=%llu candidates/matches=%llu/%llu rule/raw-ops=%llu/%llu used-rules=%llu covered/residual=%.3f/%.3f MiB lexicon entries/refs=%llu/%llu frame-wins=%llu/%zu raw-channel literal/candidate/selected=%llu/%llu/%llu bytes (%.3f/%.3f/%.3f MiB)\n",
                static_cast<unsigned long long>(statement_stats.units),static_cast<unsigned long long>(statement_stats.candidate_windows),static_cast<unsigned long long>(statement_stats.matched_windows),
                static_cast<unsigned long long>(statement_stats.rule_ops),static_cast<unsigned long long>(statement_stats.raw_ops),static_cast<unsigned long long>(statement_stats.used_rules),statement_stats.covered_bytes/1048576.0,statement_stats.residual_bytes/1048576.0,
                static_cast<unsigned long long>(statement_stats.lexicon_entries),static_cast<unsigned long long>(statement_stats.lexicon_references),static_cast<unsigned long long>(statement_frame_wins),corpus.files.size(),
                static_cast<unsigned long long>(statement_literal_wire),static_cast<unsigned long long>(statement_candidate_wire),static_cast<unsigned long long>(statement_selected_wire),
                statement_literal_wire/1048576.0,statement_candidate_wire/1048576.0,statement_selected_wire/1048576.0);
    if(use_source_dictionary){
        std::printf("P9sd trained source-superblock dictionary raw/package-wire=%llu/%llu bytes (%.1f/%.1f KiB) model-id=%016llx comparisons-won=%llu saved-before-charge=%.1f KiB frame-wins=%llu raw-channel literal/dictionary/selected=%.3f/%.3f/%.3f MiB\n",
                    static_cast<unsigned long long>(source_dictionary.size()),static_cast<unsigned long long>(source_dictionary_package.size()+4),source_dictionary.size()/1024.0,(source_dictionary_package.size()+4)/1024.0,
                    static_cast<unsigned long long>(hash_bytes(source_dictionary.data(),uint32_t(source_dictionary.size()))),static_cast<unsigned long long>(source_dictionary_encoder_frames.dictionary_wins),source_dictionary_encoder_frames.dictionary_saved/1024.0,
                    static_cast<unsigned long long>(dictionary_source_pack_stats.frame_wins),dictionary_source_pack_stats.literal_wire/1048576.0,dictionary_source_pack_stats.source_wire/1048576.0,dictionary_source_pack_stats.selected_wire/1048576.0);
        for(size_t i=0;i<kSourcePackThresholds.size();++i){
            std::printf("  P9sd threshold=%u%% candidate-TUs=%llu wire=%.3f MiB basis-raw=%.2f MiB residual-raw=%.2f MiB\n",kSourcePackThresholds[i],
                        static_cast<unsigned long long>(dictionary_source_pack_stats.threshold_tus[i]),dictionary_source_pack_stats.threshold_wire[i]/1048576.0,
                        dictionary_source_pack_stats.threshold_basis_bytes[i]/1048576.0,dictionary_source_pack_stats.threshold_residual_bytes[i]/1048576.0);
        }
        print_token_span_stats("P9td",dictionary_token_span_stats);
        print_token_span_sweep("P9td",dictionary_token_span_sweep);
        std::printf("P9td trained token-span dictionary comparisons-won=%llu saved-before-charge=%.1f KiB\n",static_cast<unsigned long long>(token_dictionary_encoder_frames.dictionary_wins),token_dictionary_encoder_frames.dictionary_saved/1024.0);
    }
    if(use_pretrained){std::printf("P8 pretrained model: training=%.2f GiB/%llu lines/%llu per-corpus-distinct in %.2fs; mode=%s min-corpora=%u; candidates=%llu rules/%llu tokens; selected=%u rules/%u tokens; package raw=%.1f KiB wire-z%d=%.1f KiB (charged)\n",
                pretrained.training_raw/1073741824.0,static_cast<unsigned long long>(pretrained.training_lines),static_cast<unsigned long long>(pretrained.training_distinct_lines),pretrained.training_seconds,
                pretrain_all_identifiers?"all-identifiers":"keywords-literal",model_min_corpora,
                static_cast<unsigned long long>(pretrained.candidate_rules),static_cast<unsigned long long>(pretrained.candidate_tokens),pretrained.rule_count(),pretrained.token_count(),pretrained.raw.size()/1024.0,zlevel,pretrained.frame.size()/1024.0);
        std::printf("P8 online additions: rules=%llu refs=%llu tokens=%llu token-refs=%llu inline=%llu raw=%llu; pretrained superblocks=%llu lines=%llu token-refs=%llu; model-dictionary wins=%llu saved=%.1f KiB\n",
                static_cast<unsigned long long>(pretrained_stats.new_rules),static_cast<unsigned long long>(pretrained_stats.rule_refs),static_cast<unsigned long long>(pretrained_stats.new_tokens),static_cast<unsigned long long>(pretrained_stats.token_refs),static_cast<unsigned long long>(pretrained_stats.inline_values),static_cast<unsigned long long>(pretrained_stats.raw_records),
                static_cast<unsigned long long>(pretrained_stats.pretrained_superblocks),static_cast<unsigned long long>(pretrained_stats.pretrained_rule_refs),static_cast<unsigned long long>(pretrained_stats.pretrained_token_refs),static_cast<unsigned long long>(model_encoder_frames.dictionary_wins),model_encoder_frames.dictionary_saved/1024.0);
        std::printf("P8c frozen+TU-local additions: rules=%llu refs=%llu tokens=%llu token-refs=%llu inline=%llu raw=%llu; pretrained superblocks=%llu lines=%llu token-refs=%llu; model-dictionary wins=%llu saved=%.1f KiB\n",
                static_cast<unsigned long long>(pretrained_local_stats.new_rules),static_cast<unsigned long long>(pretrained_local_stats.rule_refs),static_cast<unsigned long long>(pretrained_local_stats.new_tokens),static_cast<unsigned long long>(pretrained_local_stats.token_refs),static_cast<unsigned long long>(pretrained_local_stats.inline_values),static_cast<unsigned long long>(pretrained_local_stats.raw_records),
                static_cast<unsigned long long>(pretrained_local_stats.pretrained_superblocks),static_cast<unsigned long long>(pretrained_local_stats.pretrained_rule_refs),static_cast<unsigned long long>(pretrained_local_stats.pretrained_token_refs),static_cast<unsigned long long>(model_local_superblock_encoder_frames.dictionary_wins),model_local_superblock_encoder_frames.dictionary_saved/1024.0);
        std::printf("P8 model-as-dictionary over P4: wins=%llu saved-before-model-charge=%.1f KiB\n",static_cast<unsigned long long>(model_p4_encoder_frames.dictionary_wins),model_p4_encoder_frames.dictionary_saved/1024.0);
        std::printf("P13 actual selector TUs/wire: untrained=%llu/%.3f MiB frozen-local=%llu/%.3f MiB model-P4=%llu/%.3f MiB model-source=%llu/%.3f MiB\n",
                static_cast<unsigned long long>(p13_choice_tus[0]),p13_choice_wire[0]/1048576.0,static_cast<unsigned long long>(p13_choice_tus[1]),p13_choice_wire[1]/1048576.0,
                static_cast<unsigned long long>(p13_choice_tus[2]),p13_choice_wire[2]/1048576.0,static_cast<unsigned long long>(p13_choice_tus[3]),p13_choice_wire[3]/1048576.0);}
    std::printf("split wire: P4 control=%.3f data=%.3f MiB; P7 control=%.3f data=%.3f MiB\n",p4_control_wire/1048576.0,p4_data_wire/1048576.0,p7_control_wire/1048576.0,p7_data_wire/1048576.0);
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    std::fprintf(stderr, "PTGC total=%.2fs effective=%.2f GB/s peakRSS=%.1f MiB\n", total_seconds,
                 corpus.raw / 1e9 / total_seconds, usage.ru_maxrss / 1024.0);
    return 0;
}
