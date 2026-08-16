// Exact offline ceiling for word-token superblocks over CODEC-50's residual
// byte plane.
//
// This is deliberately a ceiling, not a product codec: it sees the complete
// input when deciding which words are dictionary members.  A useful row must
// later survive causal first-profitable-use publication, per-TU framing,
// reorder/edit gates, and the complete >=1 GB/s pipeline.
//
// Build: g++ -O3 -DNDEBUG -march=native -std=c++17 residual_token_ceiling.cpp
// -lzstd

#include <zstd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;

[[noreturn]] static void fail(const char *message) {
  std::fprintf(stderr, "FATAL: %s\n", message);
  std::exit(2);
}

static bool word_byte(uint8_t value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '_';
}

static void put_varint(std::vector<uint8_t> &output, uint64_t value) {
  while (value >= 0x80) {
    output.push_back(uint8_t(value) | 0x80);
    value >>= 7;
  }
  output.push_back(uint8_t(value));
}

static bool get_varint(const std::vector<uint8_t> &input, size_t &cursor,
                       uint64_t &value) {
  value = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    if (cursor == input.size())
      return false;
    const uint8_t byte = input[cursor++];
    value |= uint64_t(byte & 0x7f) << shift;
    if (!(byte & 0x80))
      return true;
  }
  return false;
}

struct ViewHash {
  size_t operator()(std::string_view value) const noexcept {
    uint64_t hash = 1469598103934665603ULL;
    for (uint8_t byte : value) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
    hash ^= hash >> 32;
    return size_t(hash);
  }
};

struct WordStat {
  uint32_t count = 0;
  uint32_t first = 0;
  uint32_t id = 0;
};

struct Compressed {
  std::vector<uint8_t> bytes;
  uint64_t raw = 0;
};

struct Span {
  uint32_t offset = 0;
  uint32_t size = 0;
};

static Compressed compress(const std::vector<uint8_t> &input, int level) {
  Compressed result;
  result.raw = input.size();
  result.bytes.resize(ZSTD_compressBound(input.size()));
  const size_t size = ZSTD_compress(result.bytes.data(), result.bytes.size(),
                                    input.data(), input.size(), level);
  if (ZSTD_isError(size))
    fail(ZSTD_getErrorName(size));
  result.bytes.resize(size);
  return result;
}

static std::vector<uint8_t> decompress(const Compressed &input) {
  std::vector<uint8_t> output(input.raw);
  const size_t size = ZSTD_decompress(output.data(), output.size(),
                                      input.bytes.data(), input.bytes.size());
  if (ZSTD_isError(size) || size != output.size())
    fail("zstd decode mismatch");
  return output;
}

static double seconds(Clock::time_point begin) {
  return std::chrono::duration<double>(Clock::now() - begin).count();
}

int main(int argc, char **argv) {
  const char *input_path = nullptr;
  int level = 3;
  std::vector<uint32_t> thresholds{2, 3, 4, 8, 16, 32};
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--input") && i + 1 < argc)
      input_path = argv[++i];
    else if (!std::strcmp(argv[i], "--z") && i + 1 < argc)
      level = std::atoi(argv[++i]);
    else
      fail("usage: residual_token_ceiling --input FILE [--z LEVEL]");
  }
  if (!input_path || level < 1 || level > 19)
    fail("usage: residual_token_ceiling --input FILE [--z LEVEL]");

  std::ifstream file(input_path, std::ios::binary);
  if (!file)
    fail("cannot open input");
  std::vector<uint8_t> input((std::istreambuf_iterator<char>(file)), {});

  const auto count_begin = Clock::now();
  std::unordered_map<std::string_view, WordStat, ViewHash> words;
  words.reserve(input.size() / 32);
  uint32_t ordinal = 0;
  for (size_t cursor = 0; cursor < input.size();) {
    if (!word_byte(input[cursor])) {
      ++cursor;
      continue;
    }
    const size_t begin = cursor++;
    while (cursor < input.size() && word_byte(input[cursor]))
      ++cursor;
    std::string_view word(reinterpret_cast<const char *>(input.data() + begin),
                          cursor - begin);
    auto [position, inserted] = words.try_emplace(word);
    if (inserted)
      position->second.first = ordinal++;
    ++position->second.count;
  }
  const double count_time = seconds(count_begin);

  const auto ordinary_begin = Clock::now();
  const Compressed ordinary = compress(input, level);
  const double ordinary_time = seconds(ordinary_begin);
  std::printf(
      "input=%zu distinct_words=%zu count_s=%.6f zstd%d=%zu zstd_s=%.6f\n",
      input.size(), words.size(), count_time, level, ordinary.bytes.size(),
      ordinary_time);

  const auto line_begin = Clock::now();
  std::vector<Span> lines;
  for (size_t begin = 0; begin < input.size();) {
    size_t end = begin;
    while (end < input.size() && input[end++] != '\n') {
    }
    if (begin > UINT32_MAX || end - begin > UINT32_MAX)
      fail("line extent overflow");
    lines.push_back({uint32_t(begin), uint32_t(end - begin)});
    begin = end;
  }
  std::vector<uint32_t> order(lines.size());
  for (size_t index = 0; index < order.size(); ++index)
    order[index] = uint32_t(index);
  auto less_line = [&](uint32_t left, uint32_t right) {
    const Span a = lines[left], b = lines[right];
    const size_t common = std::min(a.size, b.size);
    const int comparison =
        std::memcmp(input.data() + a.offset, input.data() + b.offset, common);
    return comparison ? comparison < 0 : a.size < b.size;
  };
  std::sort(order.begin(), order.end(), less_line);
  std::vector<uint32_t> occurrence_rank(lines.size());
  std::vector<uint8_t> line_meta, line_suffix, line_ranks;
  std::vector<Span> unique_lines;
  std::vector<uint8_t> previous;
  for (uint32_t occurrence : order) {
    const Span line = lines[occurrence];
    const uint8_t *data = input.data() + line.offset;
    if (!unique_lines.empty()) {
      const Span prior = unique_lines.back();
      if (prior.size == line.size &&
          !std::memcmp(input.data() + prior.offset, data, line.size)) {
        occurrence_rank[occurrence] = uint32_t(unique_lines.size() - 1);
        continue;
      }
    }
    size_t lcp = 0;
    while (lcp < previous.size() && lcp < line.size &&
           previous[lcp] == data[lcp])
      ++lcp;
    put_varint(line_meta, lcp);
    put_varint(line_meta, line.size - lcp);
    line_suffix.insert(line_suffix.end(), data + lcp, data + line.size);
    previous.assign(data, data + line.size);
    unique_lines.push_back(line);
    occurrence_rank[occurrence] = uint32_t(unique_lines.size() - 1);
  }
  put_varint(line_ranks, lines.size());
  for (uint32_t rank : occurrence_rank)
    put_varint(line_ranks, rank);
  std::vector<uint8_t> line_header;
  put_varint(line_header, unique_lines.size());
  const Compressed line_header_c = compress(line_header, level);
  const Compressed line_meta_c = compress(line_meta, level);
  const Compressed line_suffix_c = compress(line_suffix, level);
  const Compressed line_ranks_c = compress(line_ranks, level);
  const uint64_t line_wire =
      line_header_c.bytes.size() + line_meta_c.bytes.size() +
      line_suffix_c.bytes.size() + line_ranks_c.bytes.size() + 32;
  const double line_encode = seconds(line_begin);

  const auto line_decode_begin = Clock::now();
  const auto header_decoded = decompress(line_header_c);
  const auto meta_decoded = decompress(line_meta_c);
  const auto suffix_decoded = decompress(line_suffix_c);
  const auto ranks_decoded = decompress(line_ranks_c);
  size_t header_cursor = 0, meta_cursor = 0, suffix_cursor = 0,
         ranks_cursor = 0;
  uint64_t unique_count = 0, occurrence_count = 0;
  if (!get_varint(header_decoded, header_cursor, unique_count) ||
      !get_varint(ranks_decoded, ranks_cursor, occurrence_count) ||
      unique_count != unique_lines.size() || occurrence_count != lines.size())
    fail("sorted-Line header mismatch");
  std::vector<std::vector<uint8_t>> decoded_lines;
  decoded_lines.reserve(unique_count);
  previous.clear();
  for (uint64_t index = 0; index < unique_count; ++index) {
    uint64_t lcp = 0, suffix = 0;
    if (!get_varint(meta_decoded, meta_cursor, lcp) ||
        !get_varint(meta_decoded, meta_cursor, suffix) ||
        lcp > previous.size() || suffix > suffix_decoded.size() - suffix_cursor)
      fail("bad sorted-Line definition");
    std::vector<uint8_t> line(previous.begin(), previous.begin() + lcp);
    line.insert(line.end(), suffix_decoded.begin() + suffix_cursor,
                suffix_decoded.begin() + suffix_cursor + suffix);
    suffix_cursor += suffix;
    previous = line;
    decoded_lines.push_back(std::move(line));
  }
  std::vector<uint8_t> line_recovered;
  line_recovered.reserve(input.size());
  for (uint64_t occurrence = 0; occurrence < occurrence_count; ++occurrence) {
    uint64_t rank = 0;
    if (!get_varint(ranks_decoded, ranks_cursor, rank) ||
        rank >= decoded_lines.size())
      fail("bad sorted-Line rank");
    line_recovered.insert(line_recovered.end(), decoded_lines[rank].begin(),
                          decoded_lines[rank].end());
  }
  const bool line_exact = line_recovered == input &&
                          header_cursor == header_decoded.size() &&
                          meta_cursor == meta_decoded.size() &&
                          suffix_cursor == suffix_decoded.size() &&
                          ranks_cursor == ranks_decoded.size();
  const double line_decode = seconds(line_decode_begin);
  std::printf(
      "sorted_lines\toccurrences=%zu\tunique=%zu\theader=%zu\tmeta=%zu\t"
      "suffix=%zu\tranks=%zu\twire=%llu\tsaving=%lld\tencode_s=%.6f\t"
      "decode_s=%.6f\t%s\n",
      lines.size(), unique_lines.size(), line_header_c.bytes.size(),
      line_meta_c.bytes.size(), line_suffix_c.bytes.size(),
      line_ranks_c.bytes.size(), static_cast<unsigned long long>(line_wire),
      static_cast<long long>(ordinary.bytes.size()) -
          static_cast<long long>(line_wire),
      line_encode, line_decode, line_exact ? "EXACT" : "FAIL");
  if (!line_exact)
    return 1;

  std::printf("min_count\tdict_words\tdef_wire\tcontrol_wire\traw_wire\ttotal_"
              "wire\tsaving"
              "\tencode_s\tdecode_s\texact\n");

  for (uint32_t threshold : thresholds) {
    const auto encode_begin = Clock::now();
    std::vector<std::pair<std::string_view, WordStat *>> selected;
    selected.reserve(words.size());
    for (auto &[word, stat] : words)
      if (stat.count >= threshold)
        selected.emplace_back(word, &stat);
    std::sort(selected.begin(), selected.end(),
              [](const auto &left, const auto &right) {
                if (left.second->count != right.second->count)
                  return left.second->count > right.second->count;
                if (left.first.size() != right.first.size())
                  return left.first.size() < right.first.size();
                return left.first < right.first;
              });
    for (size_t index = 0; index < selected.size(); ++index)
      selected[index].second->id = uint32_t(index + 1);

    std::vector<uint8_t> definitions, control, raw;
    put_varint(definitions, selected.size());
    for (const auto &[word, stat] : selected) {
      (void)stat;
      put_varint(definitions, word.size());
      definitions.insert(definitions.end(), word.begin(), word.end());
    }
    uint64_t atoms = 0;
    size_t raw_begin = 0;
    auto flush_raw = [&](size_t end) {
      if (end == raw_begin)
        return;
      put_varint(control, 0);
      put_varint(control, end - raw_begin);
      raw.insert(raw.end(), input.begin() + raw_begin, input.begin() + end);
      ++atoms;
    };
    for (size_t cursor = 0; cursor < input.size();) {
      if (!word_byte(input[cursor])) {
        ++cursor;
        continue;
      }
      const size_t begin = cursor++;
      while (cursor < input.size() && word_byte(input[cursor]))
        ++cursor;
      const std::string_view word(
          reinterpret_cast<const char *>(input.data() + begin), cursor - begin);
      auto position = words.find(word);
      if (position == words.end() || position->second.count < threshold)
        continue;
      flush_raw(begin);
      put_varint(control, position->second.id);
      ++atoms;
      raw_begin = cursor;
    }
    flush_raw(input.size());

    const Compressed def_compressed = compress(definitions, level);
    const Compressed control_compressed = compress(control, level);
    const Compressed raw_compressed = compress(raw, level);
    const uint64_t wire = def_compressed.bytes.size() +
                          control_compressed.bytes.size() +
                          raw_compressed.bytes.size() + 24;
    const double encode_time = seconds(encode_begin);

    const auto decode_begin = Clock::now();
    const std::vector<uint8_t> def_decoded = decompress(def_compressed);
    const std::vector<uint8_t> control_decoded = decompress(control_compressed);
    const std::vector<uint8_t> raw_decoded = decompress(raw_compressed);
    size_t def_cursor = 0, control_cursor = 0, raw_cursor = 0;
    uint64_t dictionary_size = 0;
    if (!get_varint(def_decoded, def_cursor, dictionary_size) ||
        dictionary_size != selected.size())
      fail("definition count mismatch");
    std::vector<std::string_view> dictionary(1);
    dictionary.reserve(selected.size() + 1);
    for (size_t index = 0; index < selected.size(); ++index) {
      uint64_t size = 0;
      if (!get_varint(def_decoded, def_cursor, size) ||
          size > def_decoded.size() - def_cursor)
        fail("bad definition");
      dictionary.emplace_back(
          reinterpret_cast<const char *>(def_decoded.data() + def_cursor),
          size_t(size));
      def_cursor += size;
    }
    std::vector<uint8_t> recovered;
    recovered.reserve(input.size());
    uint64_t decoded_atoms = 0;
    while (control_cursor < control_decoded.size()) {
      uint64_t code = 0;
      if (!get_varint(control_decoded, control_cursor, code))
        fail("bad control code");
      if (!code) {
        uint64_t size = 0;
        if (!get_varint(control_decoded, control_cursor, size) ||
            size > raw_decoded.size() - raw_cursor)
          fail("bad raw run");
        recovered.insert(recovered.end(), raw_decoded.begin() + raw_cursor,
                         raw_decoded.begin() + raw_cursor + size);
        raw_cursor += size;
      } else {
        if (code >= dictionary.size())
          fail("bad word id");
        const auto word = dictionary[code];
        recovered.insert(recovered.end(), word.begin(), word.end());
      }
      ++decoded_atoms;
    }
    const bool exact = recovered == input && raw_cursor == raw_decoded.size() &&
                       def_cursor == def_decoded.size() &&
                       decoded_atoms == atoms;
    const double decode_time = seconds(decode_begin);
    std::printf("%u\t%zu\t%zu\t%zu\t%zu\t%llu\t%lld\t%.6f\t%.6f\t%s\n",
                threshold, selected.size(), def_compressed.bytes.size(),
                control_compressed.bytes.size(), raw_compressed.bytes.size(),
                static_cast<unsigned long long>(wire),
                static_cast<long long>(ordinary.bytes.size()) -
                    static_cast<long long>(wire),
                encode_time, decode_time, exact ? "EXACT" : "FAIL");
    if (!exact)
      return 1;
  }
  return 0;
}
