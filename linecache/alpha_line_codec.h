#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// A bounded, TU-local codec for the exact whole-Line residuals left by P24.
//
// The surrounding Region program consumes one concatenated residual byte stream.  Some bytes in
// that stream are source-patch middles and are deliberately not template candidates.  `gaps`
// records those bytes before each eligible Line (plus one tail gap), allowing this codec to reorder
// template instances internally and still reproduce the original residual stream byte-for-byte.
namespace alpha_line {

struct Slice {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

struct Stats {
    uint64_t rules = 0;
    uint64_t instances = 0;
    uint64_t unique_slots = 0;
    uint64_t slot_occurrences = 0;
    uint64_t literal_fallbacks = 0;
    uint64_t template_input_bytes = 0;
    uint64_t lexicon_entries = 0;
    uint64_t lexicon_references = 0;
};

struct Encoded {
    std::vector<uint8_t> control;
    std::vector<uint8_t> data;
    Stats stats;
    bool valid = true;
    std::string error;
};

inline void put_varint(std::vector<uint8_t>& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(uint8_t(value) | 0x80);
        value >>= 7;
    }
    out.push_back(uint8_t(value));
}

inline void put_zigzag(std::vector<uint8_t>& out, int64_t value) {
    put_varint(out, (uint64_t(value) << 1) ^ uint64_t(value >> 63));
}

inline bool get_varint(const uint8_t*& p, const uint8_t* end, uint64_t& value) {
    value = 0;
    unsigned shift = 0;
    while (p != end && shift <= 63) {
        const uint8_t byte = *p++;
        if (shift == 63 && (byte & 0x7e)) return false;
        value |= uint64_t(byte & 0x7f) << shift;
        if (!(byte & 0x80)) return true;
        shift += 7;
    }
    return false;
}

inline bool get_zigzag(const uint8_t*& p, const uint8_t* end, int64_t& value) {
    uint64_t encoded = 0;
    if (!get_varint(p, end, encoded)) return false;
    value = int64_t(encoded >> 1) ^ -int64_t(encoded & 1);
    return true;
}

inline size_t varint_size(uint64_t value) {
    size_t size = 1;
    while (value >= 0x80) {
        ++size;
        value >>= 7;
    }
    return size;
}

inline bool is_ident_start(uint8_t c) {
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline bool is_ident_continue(uint8_t c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

inline bool is_keyword(const uint8_t* p, uint32_t size) {
    static constexpr std::string_view words[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
        "break", "case", "catch", "char", "char16_t", "char32_t", "class", "co_await",
        "co_return", "co_yield", "compl", "concept", "const", "const_cast", "consteval",
        "constexpr", "constinit", "continue", "decltype", "default", "delete", "do", "double",
        "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
        "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private",
        "protected", "public", "register", "reinterpret_cast", "requires", "return", "short",
        "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch",
        "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid",
        "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
        "while", "xor", "xor_eq"
    };
    return std::binary_search(std::begin(words), std::end(words),
                              std::string_view(reinterpret_cast<const char*>(p), size));
}

enum SlotType : uint8_t {
    kIdentifier = 1,
    kNumber = 2,
    kString = 3
};

struct ParsedLine {
    std::string key;
    std::vector<std::vector<uint8_t>> literals;
    std::vector<uint32_t> occurrence_slot;
    std::vector<uint8_t> slot_type;
    std::vector<std::vector<uint8_t>> values;
};

inline void key_varint(std::string& key, uint64_t value) {
    while (value >= 0x80) {
        key.push_back(char(uint8_t(value) | 0x80));
        value >>= 7;
    }
    key.push_back(char(uint8_t(value)));
}

inline ParsedLine parse_line(const Slice& line, bool parameterize_keywords) {
    ParsedLine out;
    std::vector<uint8_t> literal;
    std::unordered_map<std::string, uint32_t> local;
    uint32_t offset = 0;

    auto emit_slot = [&](uint8_t type, const uint8_t* token, uint32_t size) {
        std::string lookup;
        lookup.reserve(size_t(size) + 1);
        lookup.push_back(char(type));
        lookup.append(reinterpret_cast<const char*>(token), size);
        auto found = local.find(lookup);
        uint32_t slot;
        if (found == local.end()) {
            slot = uint32_t(out.values.size());
            local.emplace(std::move(lookup), slot);
            out.slot_type.push_back(type);
            out.values.emplace_back(token, token + size);
        } else {
            slot = found->second;
        }
        out.literals.push_back(std::move(literal));
        literal.clear();
        out.occurrence_slot.push_back(slot);
    };

    while (offset < line.size) {
        if (is_ident_start(line.data[offset])) {
            const uint32_t begin = offset++;
            while (offset < line.size && is_ident_continue(line.data[offset])) ++offset;
            if (!parameterize_keywords && is_keyword(line.data + begin, offset - begin))
                literal.insert(literal.end(), line.data + begin, line.data + offset);
            else
                emit_slot(kIdentifier, line.data + begin, offset - begin);
        } else if (line.data[offset] >= '0' && line.data[offset] <= '9') {
            const uint32_t begin = offset++;
            while (offset < line.size) {
                const uint8_t c = line.data[offset];
                if (!(is_ident_continue(c) || c == '.' || c == '\'' || c == '+' || c == '-')) break;
                ++offset;
            }
            emit_slot(kNumber, line.data + begin, offset - begin);
        } else if (line.data[offset] == '"' || line.data[offset] == '\'') {
            const uint8_t quote = line.data[offset];
            const uint32_t begin = offset++;
            bool escaped = false;
            while (offset < line.size) {
                const uint8_t c = line.data[offset++];
                if (!escaped && c == quote) break;
                if (!escaped && c == '\\') escaped = true;
                else escaped = false;
            }
            emit_slot(kString, line.data + begin, offset - begin);
        } else {
            literal.push_back(line.data[offset++]);
        }
    }
    out.literals.push_back(std::move(literal));

    key_varint(out.key, out.values.size());
    for (uint8_t type : out.slot_type) out.key.push_back(char(type));
    key_varint(out.key, out.occurrence_slot.size());
    for (size_t i = 0; i < out.occurrence_slot.size(); ++i) {
        key_varint(out.key, out.literals[i].size());
        out.key.append(reinterpret_cast<const char*>(out.literals[i].data()), out.literals[i].size());
        key_varint(out.key, out.occurrence_slot[i]);
    }
    key_varint(out.key, out.literals.back().size());
    out.key.append(reinterpret_cast<const char*>(out.literals.back().data()),
                   out.literals.back().size());
    return out;
}

struct Group {
    ParsedLine shape;
    std::vector<size_t> members;
    uint32_t wire_id = UINT32_MAX;
};

inline size_t template_size(const ParsedLine& shape) {
    size_t size = varint_size(shape.values.size()) + shape.slot_type.size();
    size += varint_size(shape.occurrence_slot.size());
    for (size_t i = 0; i < shape.occurrence_slot.size(); ++i)
        size += varint_size(shape.literals[i].size()) + shape.literals[i].size() +
                varint_size(shape.occurrence_slot[i]);
    return size + varint_size(shape.literals.back().size()) + shape.literals.back().size();
}

inline size_t instance_size(size_t ordinal, uint32_t rule, const ParsedLine& parsed,
                            uint32_t output_size) {
    size_t size = varint_size(ordinal) + varint_size(output_size) + varint_size(rule);
    for (const auto& value : parsed.values) size += varint_size(value.size()) + value.size();
    return size;
}

inline Encoded encode(const std::vector<Slice>& lines, const std::vector<uint32_t>& gaps,
                      const std::vector<uint8_t>& gap_data, bool parameterize_keywords) {
    Encoded out;
    if (gaps.size() != lines.size() + 1) {
        out.valid = false;
        out.error = "alpha gap count differs";
        return out;
    }
    uint64_t gap_sum = 0;
    for (uint32_t gap : gaps) gap_sum += gap;
    if (gap_sum != gap_data.size()) {
        out.valid = false;
        out.error = "alpha gap bytes differ";
        return out;
    }

    put_varint(out.control, 1);  // format version
    put_varint(out.control, lines.size());
    for (uint32_t gap : gaps) put_varint(out.control, gap);
    out.data = gap_data;

    std::vector<ParsedLine> parsed;
    parsed.reserve(lines.size());
    std::vector<Group> groups;
    std::unordered_map<std::string, uint32_t> by_key;
    by_key.reserve(lines.size() * 2 + 1);
    for (size_t i = 0; i < lines.size(); ++i) {
        parsed.push_back(parse_line(lines[i], parameterize_keywords));
        auto [position, inserted] = by_key.emplace(parsed.back().key, uint32_t(groups.size()));
        if (inserted) {
            Group group;
            group.shape = parsed.back();
            groups.push_back(std::move(group));
        }
        groups[position->second].members.push_back(i);
    }

    uint32_t rule_count = 0;
    for (Group& group : groups) {
        if (group.members.size() < 2 || group.shape.values.empty()) continue;
        size_t literal_cost = 0;
        size_t rule_cost = template_size(group.shape);
        for (size_t ordinal : group.members) {
            literal_cost += varint_size(ordinal) + varint_size(lines[ordinal].size) +
                            lines[ordinal].size;
            rule_cost += instance_size(ordinal, rule_count, parsed[ordinal], lines[ordinal].size);
        }
        if (rule_cost < literal_cost) group.wire_id = rule_count++;
    }

    std::unordered_map<std::string, uint32_t> value_frequency;
    for (const Group& group : groups) {
        if (group.wire_id == UINT32_MAX) continue;
        for (size_t ordinal : group.members) {
            for (const auto& value : parsed[ordinal].values)
                ++value_frequency[std::string(reinterpret_cast<const char*>(value.data()),
                                              value.size())];
        }
    }
    std::vector<std::pair<std::string, uint32_t>> lexicon;
    lexicon.reserve(value_frequency.size());
    for (const auto& item : value_frequency) {
        const size_t size = item.first.size();
        const uint64_t repeated = uint64_t(item.second) * (varint_size(size) + size);
        const uint64_t shared = varint_size(size) + size + uint64_t(item.second) * 2;
        if (item.second >= 2 && shared < repeated) lexicon.push_back(item);
    }
    std::sort(lexicon.begin(), lexicon.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    std::unordered_map<std::string, uint32_t> lexicon_id;
    lexicon_id.reserve(lexicon.size() * 2 + 1);
    for (uint32_t i = 0; i < lexicon.size(); ++i) lexicon_id.emplace(lexicon[i].first, i);

    put_varint(out.control, rule_count);
    for (const Group& group : groups) {
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
        ++out.stats.rules;
        out.stats.unique_slots += shape.values.size();
        out.stats.slot_occurrences += shape.occurrence_slot.size();
    }

    put_varint(out.control, lexicon.size());
    for (const auto& item : lexicon) {
        put_varint(out.control, item.first.size());
        out.data.insert(out.data.end(), item.first.begin(), item.first.end());
    }
    out.stats.lexicon_entries += lexicon.size();

    uint64_t record_blocks = 0;
    for (const Group& group : groups)
        record_blocks += group.wire_id == UINT32_MAX ? group.members.size() : 1;
    put_varint(out.control, record_blocks);
    int64_t previous_ordinal = 0;
    for (const Group& group : groups) {
        if (group.wire_id != UINT32_MAX) {
            std::vector<size_t> order = group.members;
            std::sort(order.begin(), order.end());
            out.control.push_back(2);
            put_varint(out.control, group.wire_id);
            put_varint(out.control, order.size());
            for (size_t ordinal : order) {
                put_zigzag(out.control, int64_t(ordinal) - previous_ordinal);
                previous_ordinal = int64_t(ordinal);
                put_varint(out.control, lines[ordinal].size);
            }
            for (size_t slot = 0; slot < group.shape.values.size(); ++slot) {
                for (size_t ordinal : order) {
                    const auto& value = parsed[ordinal].values[slot];
                    const std::string key(reinterpret_cast<const char*>(value.data()), value.size());
                    const auto found = lexicon_id.find(key);
                    if (found != lexicon_id.end()) {
                        put_varint(out.control, uint64_t(found->second) << 1);
                        ++out.stats.lexicon_references;
                    } else {
                        put_varint(out.control, (uint64_t(value.size()) << 1) | 1);
                        out.data.insert(out.data.end(), value.begin(), value.end());
                    }
                }
            }
            out.stats.instances += order.size();
            for (size_t ordinal : order) out.stats.template_input_bytes += lines[ordinal].size;
        } else {
            for (size_t ordinal : group.members) {
                out.control.push_back(0);
                put_zigzag(out.control, int64_t(ordinal) - previous_ordinal);
                previous_ordinal = int64_t(ordinal);
                put_varint(out.control, lines[ordinal].size);
                out.data.insert(out.data.end(), lines[ordinal].data,
                                lines[ordinal].data + lines[ordinal].size);
                ++out.stats.literal_fallbacks;
            }
        }
    }
    return out;
}

struct Rule {
    std::vector<std::vector<uint8_t>> literals;
    std::vector<uint32_t> occurrence_slot;
    std::vector<uint8_t> slot_type;
};

inline bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

inline bool decode(const std::vector<uint8_t>& control, const std::vector<uint8_t>& data,
                   std::vector<uint8_t>& output, std::string& error) {
    static const uint8_t empty = 0;
    const uint8_t* p = control.empty() ? &empty : control.data();
    const uint8_t* end = p + control.size();
    const uint8_t* data_begin = data.empty() ? &empty : data.data();
    const uint8_t* data_end = data_begin + data.size();
    uint64_t version = 0, line_count64 = 0;
    if (!get_varint(p, end, version) || version != 1 ||
        !get_varint(p, end, line_count64) || line_count64 > std::numeric_limits<size_t>::max())
        return fail(error, "bad alpha header");
    const size_t line_count = size_t(line_count64);
    std::vector<size_t> gaps(line_count + 1);
    size_t gap_total = 0;
    for (size_t i = 0; i <= line_count; ++i) {
        uint64_t size = 0;
        if (!get_varint(p, end, size) || size > size_t(data_end - data_begin) - gap_total)
            return fail(error, "bad alpha gap extent");
        gaps[i] = size_t(size);
        gap_total += size_t(size);
    }
    const uint8_t* gap = data_begin;
    const uint8_t* q = data_begin + gap_total;

    uint64_t rule_count = 0;
    if (!get_varint(p, end, rule_count) || rule_count > std::numeric_limits<size_t>::max())
        return fail(error, "bad alpha rule count");
    std::vector<Rule> rules;
    rules.reserve(size_t(rule_count));
    for (uint64_t r = 0; r < rule_count; ++r) {
        Rule rule;
        uint64_t slots = 0;
        if (!get_varint(p, end, slots) || slots > uint64_t(end - p))
            return fail(error, "bad alpha slot count");
        rule.slot_type.assign(p, p + slots);
        p += slots;
        uint64_t occurrences = 0;
        if (!get_varint(p, end, occurrences) || occurrences > std::numeric_limits<size_t>::max())
            return fail(error, "bad alpha occurrence count");
        rule.literals.reserve(size_t(occurrences) + 1);
        rule.occurrence_slot.reserve(size_t(occurrences));
        for (uint64_t i = 0; i < occurrences; ++i) {
            uint64_t size = 0, slot = 0;
            if (!get_varint(p, end, size) || size > uint64_t(data_end - q))
                return fail(error, "bad alpha rule literal");
            rule.literals.emplace_back(q, q + size);
            q += size;
            if (!get_varint(p, end, slot) || slot >= slots)
                return fail(error, "bad alpha rule slot");
            rule.occurrence_slot.push_back(uint32_t(slot));
        }
        uint64_t final_size = 0;
        if (!get_varint(p, end, final_size) || final_size > uint64_t(data_end - q))
            return fail(error, "bad alpha final literal");
        rule.literals.emplace_back(q, q + final_size);
        q += final_size;
        rules.push_back(std::move(rule));
    }

    uint64_t lexicon_count = 0;
    if (!get_varint(p, end, lexicon_count) || lexicon_count > std::numeric_limits<size_t>::max())
        return fail(error, "bad alpha lexicon count");
    std::vector<std::vector<uint8_t>> lexicon;
    lexicon.reserve(size_t(lexicon_count));
    for (uint64_t i = 0; i < lexicon_count; ++i) {
        uint64_t size = 0;
        if (!get_varint(p, end, size) || size > uint64_t(data_end - q))
            return fail(error, "bad alpha lexicon entry");
        lexicon.emplace_back(q, q + size);
        q += size;
    }

    uint64_t records = 0;
    if (!get_varint(p, end, records)) return fail(error, "bad alpha record count");
    std::vector<std::vector<uint8_t>> lines(line_count);
    std::vector<uint8_t> seen(line_count);
    int64_t previous_ordinal = 0;
    auto read_ordinal = [&](size_t& ordinal) {
        int64_t delta = 0;
        if (!get_zigzag(p, end, delta)) return false;
        if ((delta > 0 && previous_ordinal > std::numeric_limits<int64_t>::max() - delta) ||
            (delta < 0 && previous_ordinal < std::numeric_limits<int64_t>::min() - delta)) return false;
        previous_ordinal += delta;
        if (previous_ordinal < 0 || uint64_t(previous_ordinal) >= line_count) return false;
        ordinal = size_t(previous_ordinal);
        return !seen[ordinal];
    };
    for (uint64_t record = 0; record < records; ++record) {
        if (p == end) return fail(error, "missing alpha record mode");
        const uint8_t mode = *p++;
        if (mode == 0) {
            size_t ordinal = 0;
            uint64_t size = 0;
            if (!read_ordinal(ordinal) || !get_varint(p, end, size) || size > uint64_t(data_end - q))
                return fail(error, "bad alpha literal record");
            lines[ordinal].assign(q, q + size);
            q += size;
            seen[ordinal] = 1;
            continue;
        }
        if (mode != 2) return fail(error, "unknown alpha record mode");
        uint64_t rule_id = 0, count64 = 0;
        if (!get_varint(p, end, rule_id) || rule_id >= rules.size() ||
            !get_varint(p, end, count64) || count64 > std::numeric_limits<size_t>::max())
            return fail(error, "bad alpha group header");
        const Rule& rule = rules[size_t(rule_id)];
        const size_t count = size_t(count64);
        std::vector<size_t> ordinals(count);
        std::vector<size_t> lengths(count);
        for (size_t row = 0; row < count; ++row) {
            uint64_t size = 0;
            if (!read_ordinal(ordinals[row]) || !get_varint(p, end, size) ||
                size > std::numeric_limits<size_t>::max())
                return fail(error, "bad alpha group row");
            lengths[row] = size_t(size);
        }
        std::vector<std::vector<std::vector<uint8_t>>> values(
            count, std::vector<std::vector<uint8_t>>(rule.slot_type.size()));
        for (size_t slot = 0; slot < rule.slot_type.size(); ++slot) {
            for (size_t row = 0; row < count; ++row) {
                uint64_t code = 0;
                if (!get_varint(p, end, code)) return fail(error, "bad alpha slot code");
                if (!(code & 1)) {
                    const uint64_t id = code >> 1;
                    if (id >= lexicon.size()) return fail(error, "bad alpha lexicon reference");
                    values[row][slot] = lexicon[size_t(id)];
                } else {
                    const uint64_t size = code >> 1;
                    if (size > uint64_t(data_end - q)) return fail(error, "bad alpha slot extent");
                    values[row][slot].assign(q, q + size);
                    q += size;
                }
            }
        }
        for (size_t row = 0; row < count; ++row) {
            std::vector<uint8_t> line;
            line.reserve(lengths[row]);
            for (size_t i = 0; i < rule.occurrence_slot.size(); ++i) {
                line.insert(line.end(), rule.literals[i].begin(), rule.literals[i].end());
                line.insert(line.end(), values[row][rule.occurrence_slot[i]].begin(),
                            values[row][rule.occurrence_slot[i]].end());
            }
            line.insert(line.end(), rule.literals.back().begin(), rule.literals.back().end());
            if (line.size() != lengths[row]) return fail(error, "alpha output length differs");
            lines[ordinals[row]] = std::move(line);
            seen[ordinals[row]] = 1;
        }
    }
    if (p != end || q != data_end) return fail(error, "trailing alpha bytes");
    if (std::find(seen.begin(), seen.end(), uint8_t(0)) != seen.end())
        return fail(error, "alpha output line missing");

    size_t output_size = gap_total;
    for (const auto& line : lines) {
        if (line.size() > std::numeric_limits<size_t>::max() - output_size)
            return fail(error, "alpha output too large");
        output_size += line.size();
    }
    output.clear();
    output.reserve(output_size);
    for (size_t i = 0; i < line_count; ++i) {
        output.insert(output.end(), gap, gap + gaps[i]);
        gap += gaps[i];
        output.insert(output.end(), lines[i].begin(), lines[i].end());
    }
    output.insert(output.end(), gap, gap + gaps.back());
    gap += gaps.back();
    if (gap != data_begin + gap_total || output.size() != output_size)
        return fail(error, "alpha interleave extent differs");
    return true;
}

}  // namespace alpha_line
