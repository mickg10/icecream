#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mo_factor {

struct Slice {
  const uint8_t *data = nullptr;
  uint32_t size = 0;
};

inline uint64_t state_mix(uint64_t value) {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

inline uint64_t append_state_digest(uint64_t prior, uint32_t id,
                                    const std::string &value) {
  uint64_t bytes = 1469598103934665603ULL;
  for (const unsigned char byte : value)
    bytes = (bytes ^ byte) * 1099511628211ULL;
  return state_mix(prior ^ state_mix(bytes ^ uint64_t(value.size())) ^
                   state_mix(uint64_t(id) + 0x9e3779b97f4a7c15ULL));
}

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

inline size_t varint_size(uint64_t value) {
  size_t size = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++size;
  }
  return size;
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
  struct StateMark {
    uint32_t values = 0;
    uint64_t string_bytes = 0;
    uint64_t content_digest = 0;
    bool operator==(const StateMark &other) const {
      return values == other.values && string_bytes == other.string_bytes &&
             content_digest == other.content_digest;
    }
    bool operator!=(const StateMark &other) const { return !(*this == other); }
  };

  uint32_t size() const { return next_id_; }
  uint64_t string_bytes() const { return string_bytes_; }
  uint64_t content_digest() const { return content_digest_; }
  StateMark state_mark() const { return {next_id_, string_bytes_, content_digest_}; }

  bool encode(const std::vector<Slice> &members, Encoded &output,
              bool transpose_translations = false,
              bool patch_translations = false,
              bool patch_previous_translations = false,
              bool best_translation_patches = false) const {
    if (unsigned(transpose_translations) + unsigned(patch_translations) +
                unsigned(patch_previous_translations) +
                unsigned(best_translation_patches) >
            1 ||
        members.size() > UINT32_MAX)
      return false;
    output = Encoded{};
    put_varint(output.control, members.size());
    std::unordered_map<std::string, uint32_t> pending_ids;
    pending_ids.reserve(4096);
    struct PendingTranslation {
      uint32_t original_id;
      uint32_t member_index;
      uint32_t entry_index;
      Slice value;
    };
    std::vector<PendingTranslation> pending_translations;
    std::unordered_map<uint32_t, std::string> previous_translations;
    std::unordered_map<uint32_t, std::vector<std::string>>
        translation_histories;
    Parsed parsed;
    for (size_t member_index = 0; member_index < members.size();
         ++member_index) {
      const auto &member = members[member_index];
      if (!parse_canonical(member.data, member.size, parsed)) {
        output.control.push_back(0);
        put_varint(output.control, member.size);
        output.ordinary.insert(output.ordinary.end(), member.data,
                               member.data + member.size);
        continue;
      }
      output.control.push_back(1);
      put_varint(output.control, parsed.originals.size());
      std::vector<uint32_t> original_ids;
      if (transpose_translations || patch_previous_translations ||
          best_translation_patches)
        original_ids.reserve(parsed.originals.size());
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
        if (transpose_translations || patch_previous_translations ||
            best_translation_patches)
          original_ids.push_back(id);
      }
      for (size_t entry_index = 0; entry_index < parsed.translations.size();
           ++entry_index) {
        const auto &translation = parsed.translations[entry_index];
        if (transpose_translations) {
          pending_translations.push_back(
              {original_ids[entry_index], uint32_t(member_index),
               uint32_t(entry_index), translation});
        } else if (best_translation_patches) {
          struct Choice {
            size_t cost;
            uint8_t mode;
            size_t age;
            uint32_t prefix;
            uint32_t suffix;
            uint32_t middle;
          } best{varint_size(uint64_t(translation.size) << 2) +
                         translation.size,
                     0, 0, 0, 0, translation.size};
          auto consider = [&](const Slice &base, uint8_t mode, size_t age) {
            uint32_t prefix = 0;
            const uint32_t shared = std::min(base.size, translation.size);
            while (prefix < shared &&
                   base.data[prefix] == translation.data[prefix])
              ++prefix;
            uint32_t suffix = 0;
            while (suffix < shared - prefix &&
                   base.data[base.size - 1 - suffix] ==
                       translation.data[translation.size - 1 - suffix])
              ++suffix;
            const uint32_t middle = translation.size - prefix - suffix;
            const size_t cost = varint_size((uint64_t(middle) << 2) | mode) +
                                (mode == 2 ? varint_size(age) : 0) +
                                varint_size(prefix) + varint_size(suffix) +
                                middle;
            if (cost < best.cost)
              best = {cost, mode, age, prefix, suffix, middle};
          };
          consider(parsed.originals[entry_index], 1, 0);
          auto &history =
              translation_histories[original_ids[entry_index]];
          for (size_t age = 0; age < history.size(); ++age) {
            const auto &value = history[history.size() - 1 - age];
            consider({reinterpret_cast<const uint8_t *>(value.data()),
                      uint32_t(value.size())},
                     2, age);
          }
          put_varint(output.translations,
                     (uint64_t(best.middle) << 2) | best.mode);
          if (best.mode == 2)
            put_varint(output.translations, best.age);
          if (best.mode) {
            put_varint(output.translations, best.prefix);
            put_varint(output.translations, best.suffix);
          }
          output.translations.insert(
              output.translations.end(), translation.data + best.prefix,
              translation.data + best.prefix + best.middle);
          history.emplace_back(
              reinterpret_cast<const char *>(translation.data),
              translation.size);
        } else if (patch_translations || patch_previous_translations) {
          Slice previous_base{};
          const Slice *base = &parsed.originals[entry_index];
          if (patch_previous_translations) {
            const auto known =
                previous_translations.find(original_ids[entry_index]);
            if (known != previous_translations.end()) {
              previous_base = {
                  reinterpret_cast<const uint8_t *>(known->second.data()),
                  uint32_t(known->second.size())};
              base = &previous_base;
            } else {
              base = nullptr;
            }
          }
          uint32_t prefix = 0;
          const uint32_t shared =
              base ? std::min(base->size, translation.size) : 0;
          while (prefix < shared && base->data[prefix] == translation.data[prefix])
            ++prefix;
          uint32_t suffix = 0;
          while (base && suffix < shared - prefix &&
                 base->data[base->size - 1 - suffix] ==
                     translation.data[translation.size - 1 - suffix])
            ++suffix;
          const uint32_t middle = translation.size - prefix - suffix;
          const size_t raw_cost =
              varint_size(uint64_t(translation.size) << 1) + translation.size;
          const size_t patch_cost =
              varint_size((uint64_t(middle) << 1) | 1) +
              varint_size(prefix) + varint_size(suffix) + middle;
          if (patch_cost < raw_cost) {
            put_varint(output.translations, (uint64_t(middle) << 1) | 1);
            put_varint(output.translations, prefix);
            put_varint(output.translations, suffix);
            output.translations.insert(
                output.translations.end(), translation.data + prefix,
                translation.data + prefix + middle);
          } else {
            put_varint(output.translations, uint64_t(translation.size) << 1);
            output.translations.insert(output.translations.end(),
                                       translation.data,
                                       translation.data + translation.size);
          }
          if (patch_previous_translations)
            previous_translations[original_ids[entry_index]] = std::string(
                reinterpret_cast<const char *>(translation.data),
                translation.size);
        } else {
          put_varint(output.translations, translation.size);
          output.translations.insert(output.translations.end(), translation.data,
                                     translation.data + translation.size);
        }
      }
      ++output.mo_members;
      output.mo_bytes += member.size;
    }
    if (transpose_translations) {
      std::sort(pending_translations.begin(), pending_translations.end(),
                [](const PendingTranslation &left,
                   const PendingTranslation &right) {
                  if (left.original_id != right.original_id)
                    return left.original_id < right.original_id;
                  if (left.member_index != right.member_index)
                    return left.member_index < right.member_index;
                  return left.entry_index < right.entry_index;
                });
      for (const auto &entry : pending_translations) {
        put_varint(output.translations, entry.value.size);
        output.translations.insert(output.translations.end(), entry.value.data,
                                   entry.value.data + entry.value.size);
      }
    }
    put_varint(output.definitions, output.pending_definitions.size());
    for (const auto &value : output.pending_definitions) {
      put_varint(output.definitions, value.size());
      output.definitions.insert(output.definitions.end(), value.begin(),
                                value.end());
    }
    return true;
  }

  // Apply prepared definitions with a strong all-or-nothing result.  Encoded remains intact
  // so a transfer attempt can be retried byte-for-byte; C should call this only after Ack.
  bool commit(const Encoded &encoded) {
    const size_t count = encoded.pending_definitions.size();
    if (count > uint64_t(UINT32_MAX) - next_id_)
      return false;
    uint64_t added_bytes = 0;
    for (const auto &value : encoded.pending_definitions) {
      if (value.size() > UINT64_MAX - added_bytes)
        return false;
      added_bytes += value.size();
    }
    if (added_bytes > UINT64_MAX - string_bytes_)
      return false;

    size_t inserted_count = 0;
    auto rollback = [&]() {
      for (size_t index = 0; index < inserted_count; ++index)
        ids_.erase(encoded.pending_definitions[index]);
    };
    try {
      for (size_t index = 0; index < count; ++index) {
        const uint32_t id = uint32_t(next_id_ + index);
        const auto [position, inserted] =
            ids_.emplace(encoded.pending_definitions[index], id);
        if (!inserted || position->second != id) {
          rollback();
          return false;
        }
        ++inserted_count;
      }
    } catch (...) {
      rollback();
      throw;
    }
    for (size_t index = 0; index < count; ++index)
      content_digest_ = append_state_digest(
          content_digest_, uint32_t(next_id_ + index),
          encoded.pending_definitions[index]);
    next_id_ += uint32_t(count);
    string_bytes_ += added_bytes;
    return true;
  }

private:
  std::unordered_map<std::string, uint32_t> ids_;
  uint32_t next_id_ = 0;
  uint64_t string_bytes_ = 0;
  uint64_t content_digest_ = 0;
};

class DecoderState {
public:
  struct StateMark {
    size_t values = 0;
    uint64_t string_bytes = 0;
    uint64_t content_digest = 0;
    bool operator==(const StateMark &other) const {
      return values == other.values && string_bytes == other.string_bytes &&
             content_digest == other.content_digest;
    }
    bool operator!=(const StateMark &other) const { return !(*this == other); }
  };

  uint32_t size() const { return uint32_t(values_.size()); }
  uint64_t string_bytes() const { return string_bytes_; }
  uint64_t content_digest() const { return content_digest_; }
  StateMark state_mark() const {
    return {values_.size(), string_bytes_, content_digest_};
  }

  // Decoder definitions append only.  A whole-TU abort therefore destroys just the values
  // appended by that TU and restores three scalars; it never copies the established dictionary.
  void begin_transaction() {
    if (transaction_active_)
      throw std::logic_error("MO decoder transaction already active");
    checkpoint_ = state_mark();
    transaction_active_ = true;
  }
  void commit_transaction() {
    require_transaction();
    transaction_active_ = false;
  }
  void abort_transaction() {
    require_transaction();
    values_.resize(checkpoint_.values);
    string_bytes_ = checkpoint_.string_bytes;
    content_digest_ = checkpoint_.content_digest;
    transaction_active_ = false;
  }
  bool has_pending_transaction() const { return transaction_active_; }

  bool decode(const std::vector<uint8_t> &control,
              const std::vector<uint8_t> &definitions,
              const std::vector<uint8_t> &translations,
              const std::vector<uint8_t> &ordinary,
              std::vector<uint8_t> &output, std::vector<uint32_t> &lengths,
              bool transpose_translations = false,
              bool patch_translations = false,
              bool patch_previous_translations = false,
              bool best_translation_patches = false) {
    if (unsigned(transpose_translations) + unsigned(patch_translations) +
            unsigned(patch_previous_translations) +
            unsigned(best_translation_patches) >
        1)
      return false;
    static const uint8_t empty = 0;
    const uint8_t *definition =
        definitions.empty() ? &empty : definitions.data();
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
    if (transpose_translations)
      return decode_transposed(control, translations, ordinary, pending_values,
                               pending_string_bytes, output, lengths);

    const uint8_t *command = control.empty() ? &empty : control.data();
    const uint8_t *command_end = command + control.size();
    const uint8_t *translated =
        translations.empty() ? &empty : translations.data();
    const uint8_t *translated_end = translated + translations.size();
    const uint8_t *raw = ordinary.empty() ? &empty : ordinary.data();
    const uint8_t *raw_end = raw + ordinary.size();
    uint64_t member_count = 0;
    if (!get_varint(command, command_end, member_count) ||
        member_count > UINT32_MAX)
      return false;
    output.clear();
    lengths.clear();
    lengths.reserve(size_t(member_count));
    std::vector<Slice> originals, translated_values;
    std::vector<uint32_t> original_ids;
    std::unordered_map<uint32_t, std::string> previous_translations;
    std::unordered_map<uint32_t, std::vector<std::string>>
        translation_histories;
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
      original_ids.clear();
      originals.reserve(size_t(count));
      translated_values.reserve(size_t(count));
      if (patch_previous_translations || best_translation_patches)
        original_ids.reserve(size_t(count));
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
        if (patch_previous_translations || best_translation_patches)
          original_ids.push_back(uint32_t(id));
      }
      std::vector<std::vector<uint8_t>> patched_values;
      if (patch_translations || patch_previous_translations ||
          best_translation_patches)
        patched_values.resize(size_t(count));
      for (uint64_t index = 0; index < count; ++index) {
        uint64_t descriptor = 0;
        if (!get_varint(translated, translated_end, descriptor))
          return false;
        if (best_translation_patches) {
          const uint8_t mode = uint8_t(descriptor & 3);
          const uint64_t middle = descriptor >> 2;
          if (mode == 3 || middle > uint64_t(translated_end - translated) ||
              middle > UINT32_MAX)
            return false;
          auto &history =
              translation_histories[original_ids[size_t(index)]];
          if (!mode) {
            translated_values.push_back({translated, uint32_t(middle)});
            history.emplace_back(reinterpret_cast<const char *>(translated),
                                 size_t(middle));
            translated += middle;
            continue;
          }
          Slice base = originals[size_t(index)];
          if (mode == 2) {
            uint64_t age = 0;
            if (!get_varint(translated, translated_end, age) ||
                age >= history.size())
              return false;
            const auto &value = history[history.size() - 1 - size_t(age)];
            base = {reinterpret_cast<const uint8_t *>(value.data()),
                    uint32_t(value.size())};
          }
          uint64_t prefix = 0, suffix = 0;
          if (!get_varint(translated, translated_end, prefix) ||
              !get_varint(translated, translated_end, suffix) ||
              prefix > base.size || suffix > base.size - prefix ||
              middle > uint64_t(translated_end - translated) ||
              prefix > UINT32_MAX - middle ||
              suffix > UINT32_MAX - prefix - middle)
            return false;
          auto &value = patched_values[size_t(index)];
          value.reserve(size_t(prefix + middle + suffix));
          value.insert(value.end(), base.data, base.data + prefix);
          value.insert(value.end(), translated, translated + middle);
          value.insert(value.end(), base.data + base.size - suffix,
                       base.data + base.size);
          translated += middle;
          translated_values.push_back(
              {value.empty() ? &empty : value.data(), uint32_t(value.size())});
          history.emplace_back(value.empty()
                                   ? std::string()
                                   : std::string(reinterpret_cast<const char *>(
                                                     value.data()),
                                                 value.size()));
          continue;
        }
        if (!patch_translations && !patch_previous_translations) {
          if (descriptor > uint64_t(translated_end - translated) ||
              descriptor > UINT32_MAX)
            return false;
          translated_values.push_back({translated, uint32_t(descriptor)});
          translated += descriptor;
          continue;
        }
        const uint64_t middle = descriptor >> 1;
        if (middle > uint64_t(translated_end - translated) ||
            middle > UINT32_MAX)
          return false;
        if (!(descriptor & 1)) {
          translated_values.push_back({translated, uint32_t(middle)});
          if (patch_previous_translations)
            previous_translations[original_ids[size_t(index)]] = std::string(
                reinterpret_cast<const char *>(translated), size_t(middle));
          translated += middle;
          continue;
        }
        uint64_t prefix = 0, suffix = 0;
        Slice base = originals[size_t(index)];
        if (patch_previous_translations) {
          const auto known =
              previous_translations.find(original_ids[size_t(index)]);
          if (known == previous_translations.end())
            return false;
          base = {reinterpret_cast<const uint8_t *>(known->second.data()),
                  uint32_t(known->second.size())};
        }
        const uint64_t original_size = base.size;
        if (!get_varint(translated, translated_end, prefix) ||
            !get_varint(translated, translated_end, suffix) ||
            prefix > original_size || suffix > original_size - prefix ||
            middle > uint64_t(translated_end - translated) ||
            prefix > UINT32_MAX - middle ||
            suffix > UINT32_MAX - prefix - middle)
          return false;
        auto &value = patched_values[size_t(index)];
        value.reserve(size_t(prefix + middle + suffix));
        value.insert(value.end(), base.data, base.data + prefix);
        value.insert(value.end(), translated, translated + middle);
        value.insert(value.end(), base.data + base.size - suffix,
                     base.data + base.size);
        translated += middle;
        translated_values.push_back(
            {value.empty() ? &empty : value.data(), uint32_t(value.size())});
        if (patch_previous_translations)
          previous_translations[original_ids[size_t(index)]] = value.empty()
              ? std::string()
              : std::string(reinterpret_cast<const char *>(value.data()),
                            value.size());
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
    if (pending_string_bytes > UINT64_MAX - string_bytes_)
      return false;
    values_.reserve(values_.size() + pending_values.size());
    for (auto &value : pending_values) {
      content_digest_ = append_state_digest(content_digest_, uint32_t(values_.size()), value);
      values_.push_back(std::move(value));
    }
    string_bytes_ += pending_string_bytes;
    return true;
  }

private:
  void require_transaction() const {
    if (!transaction_active_)
      throw std::logic_error("MO decoder has no active transaction");
  }

  bool decode_transposed(const std::vector<uint8_t> &control,
                         const std::vector<uint8_t> &translations,
                         const std::vector<uint8_t> &ordinary,
                         std::vector<std::string> &pending_values,
                         uint64_t pending_string_bytes,
                         std::vector<uint8_t> &output,
                         std::vector<uint32_t> &lengths) {
    struct MemberPlan {
      bool canonical = false;
      Slice ordinary{};
      std::vector<Slice> originals;
      std::vector<Slice> translated_values;
    };
    struct TranslationSlot {
      uint32_t original_id;
      uint32_t member_index;
      uint32_t entry_index;
    };

    static const uint8_t empty = 0;
    const uint8_t *command = control.empty() ? &empty : control.data();
    const uint8_t *command_end = command + control.size();
    const uint8_t *raw = ordinary.empty() ? &empty : ordinary.data();
    const uint8_t *raw_end = raw + ordinary.size();
    uint64_t member_count = 0;
    if (!get_varint(command, command_end, member_count) ||
        member_count > UINT32_MAX)
      return false;
    std::vector<MemberPlan> plans(static_cast<size_t>(member_count));
    std::vector<TranslationSlot> slots;
    for (size_t member_index = 0; member_index < member_count;
         ++member_index) {
      if (command == command_end)
        return false;
      const uint8_t mode = *command++;
      auto &plan = plans[member_index];
      if (!mode) {
        uint64_t size = 0;
        if (!get_varint(command, command_end, size) ||
            size > uint64_t(raw_end - raw) || size > UINT32_MAX)
          return false;
        plan.ordinary = {raw, uint32_t(size)};
        raw += size;
        continue;
      }
      if (mode != 1)
        return false;
      uint64_t count = 0;
      if (!get_varint(command, command_end, count) || count > UINT32_MAX ||
          count > SIZE_MAX - slots.size())
        return false;
      plan.canonical = true;
      plan.originals.reserve(size_t(count));
      plan.translated_values.resize(size_t(count));
      for (size_t entry_index = 0; entry_index < count; ++entry_index) {
        uint64_t id = 0;
        if (!get_varint(command, command_end, id) ||
            id >= uint64_t(values_.size()) + pending_values.size())
          return false;
        const auto &value = id < values_.size()
                                ? values_[size_t(id)]
                                : pending_values[size_t(id - values_.size())];
        plan.originals.push_back(
            {reinterpret_cast<const uint8_t *>(value.data()),
             uint32_t(value.size())});
        slots.push_back(
            {uint32_t(id), uint32_t(member_index), uint32_t(entry_index)});
      }
    }
    if (command != command_end || raw != raw_end)
      return false;

    std::sort(slots.begin(), slots.end(),
              [](const TranslationSlot &left, const TranslationSlot &right) {
                if (left.original_id != right.original_id)
                  return left.original_id < right.original_id;
                if (left.member_index != right.member_index)
                  return left.member_index < right.member_index;
                return left.entry_index < right.entry_index;
              });
    const uint8_t *translated =
        translations.empty() ? &empty : translations.data();
    const uint8_t *translated_end = translated + translations.size();
    for (const auto &slot : slots) {
      uint64_t size = 0;
      if (!get_varint(translated, translated_end, size) ||
          size > uint64_t(translated_end - translated) || size > UINT32_MAX)
        return false;
      plans[slot.member_index].translated_values[slot.entry_index] =
          {translated, uint32_t(size)};
      translated += size;
    }
    if (translated != translated_end)
      return false;

    output.clear();
    lengths.clear();
    lengths.reserve(plans.size());
    std::vector<uint8_t> member;
    for (const auto &plan : plans) {
      if (!plan.canonical) {
        output.insert(output.end(), plan.ordinary.data,
                      plan.ordinary.data + plan.ordinary.size);
        lengths.push_back(plan.ordinary.size);
        continue;
      }
      if (!build(plan.originals, plan.translated_values, member) ||
          member.size() > UINT32_MAX)
        return false;
      output.insert(output.end(), member.begin(), member.end());
      lengths.push_back(uint32_t(member.size()));
    }

    if (pending_string_bytes > UINT64_MAX - string_bytes_)
      return false;
    values_.reserve(values_.size() + pending_values.size());
    for (auto &value : pending_values) {
      content_digest_ = append_state_digest(content_digest_, uint32_t(values_.size()), value);
      values_.push_back(std::move(value));
    }
    string_bytes_ += pending_string_bytes;
    return true;
  }

  std::vector<std::string> values_;
  uint64_t string_bytes_ = 0;
  uint64_t content_digest_ = 0;
  bool transaction_active_ = false;
  StateMark checkpoint_{};
};

} // namespace mo_factor
