#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mo_factor {

struct Slice {
  const uint8_t *data = nullptr;
  uint32_t size = 0;
};

inline uint32_t read_u32(const uint8_t *data) {
  return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
         (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

inline void put_u32(std::vector<uint8_t> &output, uint32_t value) {
  output.push_back(uint8_t(value));
  output.push_back(uint8_t(value >> 8));
  output.push_back(uint8_t(value >> 16));
  output.push_back(uint8_t(value >> 24));
}

inline void put_varint(std::vector<uint8_t> &output, uint64_t value) {
  while (value >= 0x80) {
    output.push_back(uint8_t(value) | 0x80);
    value >>= 7;
  }
  output.push_back(uint8_t(value));
}

inline bool get_varint(const uint8_t *&cursor, const uint8_t *end,
                       uint64_t &value) {
  value = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    if (cursor == end)
      return false;
    const uint8_t byte = *cursor++;
    value |= uint64_t(byte & 0x7f) << shift;
    if (!(byte & 0x80))
      return true;
  }
  return false;
}

struct Parsed {
  std::vector<Slice> originals;
  std::vector<Slice> translations;
};

inline bool build(const std::vector<Slice> &originals,
                  const std::vector<Slice> &translations,
                  std::vector<uint8_t> &output) {
  if (originals.size() != translations.size() || originals.size() > UINT32_MAX)
    return false;
  const uint64_t count = originals.size();
  uint64_t total = 28 + count * 16;
  for (const auto &value : originals)
    total += uint64_t(value.size) + 1;
  for (const auto &value : translations)
    total += uint64_t(value.size) + 1;
  if (total > UINT32_MAX)
    return false;

  const uint32_t n = uint32_t(count), original_table = 28;
  const uint32_t translation_table = original_table + n * 8;
  uint32_t offset = translation_table + n * 8;
  output.clear();
  output.reserve(size_t(total));
  put_u32(output, 0x950412deu);
  put_u32(output, 0);
  put_u32(output, n);
  put_u32(output, original_table);
  put_u32(output, translation_table);
  put_u32(output, 0);
  put_u32(output, offset);
  for (const auto &value : originals) {
    put_u32(output, value.size);
    put_u32(output, offset);
    offset += value.size + 1;
  }
  for (const auto &value : translations) {
    put_u32(output, value.size);
    put_u32(output, offset);
    offset += value.size + 1;
  }
  for (const auto &value : originals) {
    output.insert(output.end(), value.data, value.data + value.size);
    output.push_back(0);
  }
  for (const auto &value : translations) {
    output.insert(output.end(), value.data, value.data + value.size);
    output.push_back(0);
  }
  return output.size() == total;
}

inline bool parse_canonical(const uint8_t *data, size_t size, Parsed &parsed) {
  parsed.originals.clear();
  parsed.translations.clear();
  if (size < 28 || read_u32(data) != 0x950412deu || read_u32(data + 4) != 0)
    return false;
  const uint32_t count = read_u32(data + 8),
                 original_table = read_u32(data + 12);
  const uint32_t translation_table = read_u32(data + 16),
                 hash_size = read_u32(data + 20);
  const uint32_t hash_offset = read_u32(data + 24);
  if (original_table != 28 || translation_table != 28u + uint64_t(count) * 8 ||
      hash_size || hash_offset != 28u + uint64_t(count) * 16 ||
      hash_offset > size)
    return false;
  parsed.originals.reserve(count);
  parsed.translations.reserve(count);
  for (const auto table : {original_table, translation_table}) {
    auto &values =
        table == original_table ? parsed.originals : parsed.translations;
    for (uint32_t index = 0; index < count; ++index) {
      const uint64_t entry = uint64_t(table) + uint64_t(index) * 8;
      if (entry + 8 > size)
        return false;
      const uint32_t length = read_u32(data + entry),
                     offset = read_u32(data + entry + 4);
      if (uint64_t(offset) + length >= size || data[offset + length] != 0)
        return false;
      values.push_back({data + offset, length});
    }
  }
  std::vector<uint8_t> rebuilt;
  return build(parsed.originals, parsed.translations, rebuilt) &&
         rebuilt.size() == size && !std::memcmp(rebuilt.data(), data, size);
}

struct Encoded {
  std::vector<uint8_t> control;
  std::vector<uint8_t> definitions;
  std::vector<uint8_t> translations;
  std::vector<uint8_t> ordinary;
  std::vector<std::string> pending_definitions;
  uint32_t mo_members = 0;
  uint64_t mo_bytes = 0;
};

class EncoderState {
public:
  uint32_t size() const { return next_id_; }
  uint64_t string_bytes() const { return string_bytes_; }

  bool encode(const std::vector<Slice> &members, Encoded &output) const {
    output = Encoded{};
    put_varint(output.control, members.size());
    std::unordered_map<std::string, uint32_t> pending_ids;
    pending_ids.reserve(4096);
    Parsed parsed;
    for (const auto &member : members) {
      if (!parse_canonical(member.data, member.size, parsed)) {
        output.control.push_back(0);
        put_varint(output.control, member.size);
        output.ordinary.insert(output.ordinary.end(), member.data,
                               member.data + member.size);
        continue;
      }
      output.control.push_back(1);
      put_varint(output.control, parsed.originals.size());
      for (const auto &original : parsed.originals) {
        std::string key(reinterpret_cast<const char *>(original.data),
                        original.size);
        uint32_t id;
        auto known = ids_.find(key);
        if (known != ids_.end())
          id = known->second;
        else {
          auto pending = pending_ids.find(key);
          if (pending != pending_ids.end())
            id = pending->second;
          else {
            if (uint64_t(next_id_) + output.pending_definitions.size() >=
                UINT32_MAX)
              return false;
            id = uint32_t(next_id_ + output.pending_definitions.size());
            output.pending_definitions.push_back(key);
            pending_ids.emplace(std::move(key), id);
          }
        }
        put_varint(output.control, id);
      }
      for (const auto &translation : parsed.translations) {
        put_varint(output.translations, translation.size);
        output.translations.insert(output.translations.end(), translation.data,
                                   translation.data + translation.size);
      }
      ++output.mo_members;
      output.mo_bytes += member.size;
    }
    put_varint(output.definitions, output.pending_definitions.size());
    for (const auto &value : output.pending_definitions) {
      put_varint(output.definitions, value.size());
      output.definitions.insert(output.definitions.end(), value.begin(),
                                value.end());
    }
    return true;
  }

  bool commit(Encoded &encoded) {
    for (auto &value : encoded.pending_definitions) {
      if (next_id_ >= UINT32_MAX)
        return false;
      const uint32_t id = next_id_;
      auto [position, inserted] = ids_.emplace(std::move(value), id);
      if (!inserted || position->second != id)
        return false;
      ++next_id_;
      string_bytes_ += position->first.size();
    }
    return true;
  }

private:
  std::unordered_map<std::string, uint32_t> ids_;
  uint32_t next_id_ = 0;
  uint64_t string_bytes_ = 0;
};

class DecoderState {
public:
  uint32_t size() const { return uint32_t(values_.size()); }
  uint64_t string_bytes() const { return string_bytes_; }

  bool decode(const std::vector<uint8_t> &control,
              const std::vector<uint8_t> &definitions,
              const std::vector<uint8_t> &translations,
              const std::vector<uint8_t> &ordinary,
              std::vector<uint8_t> &output, std::vector<uint32_t> &lengths) {
    const uint8_t *definition = definitions.data();
    const uint8_t *definition_end = definition + definitions.size();
    uint64_t new_count = 0;
    if (!get_varint(definition, definition_end, new_count) ||
        uint64_t(values_.size()) + new_count > UINT32_MAX)
      return false;
    std::vector<std::string> pending_values;
    pending_values.reserve(size_t(new_count));
    uint64_t pending_string_bytes = 0;
    for (uint64_t index = 0; index < new_count; ++index) {
      uint64_t size = 0;
      if (!get_varint(definition, definition_end, size) ||
          size > uint64_t(definition_end - definition))
        return false;
      pending_values.emplace_back(reinterpret_cast<const char *>(definition),
                                  size_t(size));
      pending_string_bytes += size;
      definition += size;
    }
    if (definition != definition_end)
      return false;

    const uint8_t *command = control.data();
    const uint8_t *command_end = command + control.size();
    const uint8_t *translated = translations.data();
    const uint8_t *translated_end = translated + translations.size();
    const uint8_t *raw = ordinary.data();
    const uint8_t *raw_end = raw + ordinary.size();
    uint64_t member_count = 0;
    if (!get_varint(command, command_end, member_count) ||
        member_count > UINT32_MAX)
      return false;
    output.clear();
    lengths.clear();
    lengths.reserve(size_t(member_count));
    std::vector<Slice> originals, translated_values;
    std::vector<uint8_t> member;
    for (uint64_t member_index = 0; member_index < member_count;
         ++member_index) {
      if (command == command_end)
        return false;
      const uint8_t mode = *command++;
      if (!mode) {
        uint64_t size = 0;
        if (!get_varint(command, command_end, size) ||
            size > uint64_t(raw_end - raw) || size > UINT32_MAX)
          return false;
        output.insert(output.end(), raw, raw + size);
        raw += size;
        lengths.push_back(uint32_t(size));
        continue;
      }
      if (mode != 1)
        return false;
      uint64_t count = 0;
      if (!get_varint(command, command_end, count) || count > UINT32_MAX)
        return false;
      originals.clear();
      translated_values.clear();
      originals.reserve(size_t(count));
      translated_values.reserve(size_t(count));
      for (uint64_t index = 0; index < count; ++index) {
        uint64_t id = 0;
        if (!get_varint(command, command_end, id) ||
            id >= uint64_t(values_.size()) + pending_values.size())
          return false;
        const auto &value = id < values_.size()
                                ? values_[size_t(id)]
                                : pending_values[size_t(id - values_.size())];
        originals.push_back({reinterpret_cast<const uint8_t *>(value.data()),
                             uint32_t(value.size())});
      }
      for (uint64_t index = 0; index < count; ++index) {
        uint64_t size = 0;
        if (!get_varint(translated, translated_end, size) ||
            size > uint64_t(translated_end - translated) || size > UINT32_MAX)
          return false;
        translated_values.push_back({translated, uint32_t(size)});
        translated += size;
      }
      if (!build(originals, translated_values, member) ||
          member.size() > UINT32_MAX)
        return false;
      output.insert(output.end(), member.begin(), member.end());
      lengths.push_back(uint32_t(member.size()));
    }
    if (command != command_end || translated != translated_end ||
        raw != raw_end)
      return false;
    values_.reserve(values_.size() + pending_values.size());
    for (auto &value : pending_values)
      values_.push_back(std::move(value));
    string_bytes_ += pending_string_bytes;
    return true;
  }

private:
  std::vector<std::string> values_;
  uint64_t string_bytes_ = 0;
};

} // namespace mo_factor
