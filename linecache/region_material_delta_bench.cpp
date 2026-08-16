// Exact causal Region-material prefix/middle/suffix capability screen.
//
// The benchmark deliberately reuses codec50's real corpus parser and Region
// interner.  A Region first observed in TU t may use only a base Region learned
// after a completed TU < t.  The decoder receives only the two compressed
// streams below, reconstructs every first-seen Region, and compares it with the
// independently interned bytes.

#include <deque>
#include <unordered_set>

#define main codec50_capability_main
#include "codec50.cpp"
#undef main

struct RecentRegions {
  static constexpr size_t limit = 8;
  std::deque<uint32_t> ids;

  void add(uint32_t id) {
    if (ids.size() == limit)
      ids.pop_front();
    ids.push_back(id);
  }
};

static uint32_t common_prefix(const char *left, uint32_t left_size,
                              const char *right, uint32_t right_size) {
  const uint32_t shared = std::min(left_size, right_size);
  uint32_t offset = 0;
  while (offset + sizeof(uint64_t) <= shared &&
         read64(left + offset) == read64(right + offset))
    offset += sizeof(uint64_t);
  while (offset < shared && left[offset] == right[offset])
    ++offset;
  return offset;
}

static uint32_t common_suffix(const char *left, uint32_t left_size,
                              const char *right, uint32_t right_size,
                              uint32_t prefix) {
  const uint32_t shared = std::min(left_size, right_size) - prefix;
  uint32_t suffix = 0;
  while (suffix + sizeof(uint64_t) <= shared &&
         read64(left + left_size - suffix - sizeof(uint64_t)) ==
             read64(right + right_size - suffix - sizeof(uint64_t)))
    suffix += sizeof(uint64_t);
  while (suffix < shared && left[left_size - 1 - suffix] ==
                                right[right_size - 1 - suffix])
    ++suffix;
  return suffix;
}

static std::array<uint64_t, 4> region_anchors(const char *data,
                                               uint32_t size) {
  std::array<uint64_t, 4> result{};
  if (!size)
    return result;
  for (size_t partition = 0; partition < result.size(); ++partition) {
    const size_t begin = uint64_t(size) * partition / result.size();
    const size_t end = uint64_t(size) * (partition + 1) / result.size();
    uint64_t best = UINT64_MAX;
    if (end - begin >= 8) {
      for (size_t offset = begin; offset + 8 <= end; offset += 32)
        best = std::min(best, mix64(read64(data + offset)));
    } else {
      best = mix64(read_tail(data + begin, uint32_t(end - begin)));
    }
    result[partition] = best;
  }
  return result;
}

static void configure_stream(ZSTD_CCtx *context, int level) {
  if (ZSTD_isError(
          ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level)) ||
      ZSTD_isError(
          ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 0))) {
    std::fprintf(stderr, "cannot configure zstd stream\n");
    std::exit(2);
  }
}

int main(int argc, char **argv) {
  const char *manifest = nullptr;
  int level = 3;
  for (int index = 1; index < argc; ++index) {
    if (!std::strcmp(argv[index], "--manifest") && index + 1 < argc)
      manifest = argv[++index];
    else if (!std::strcmp(argv[index], "--z") && index + 1 < argc) {
      char *end = nullptr;
      const long value = std::strtol(argv[++index], &end, 10);
      if (!end || *end || value < 1 || value > 9) {
        std::fprintf(stderr, "bad zstd level\n");
        return 2;
      }
      level = int(value);
    } else {
      std::fprintf(stderr,
                   "usage: region_material_delta_bench --manifest FILE "
                   "[--z 1..9]\n");
      return 2;
    }
  }
  if (!manifest) {
    std::fprintf(stderr,
                 "usage: region_material_delta_bench --manifest FILE "
                 "[--z 1..9]\n");
    return 2;
  }

  const auto begin = Clock::now();
  Corpus corpus = load_corpus(manifest, SIZE_MAX);
  Interner dictionary;
  std::vector<uint32_t> regions;
  std::vector<size_t> offsets{0};
  uint64_t hits = 0;
  for (const FileSpan &file : corpus.files) {
    std::vector<uint32_t> line_ids(size_t(file.len) + 1), region_ids;
    size_t count = 0;
    const char *data = corpus.bytes.data() + file.off;
    dictionary.process(data, data + file.len, line_ids.data(), count, hits,
                       true, &region_ids);
    regions.insert(regions.end(), region_ids.begin(), region_ids.end());
    offsets.push_back(regions.size());
  }
  std::fprintf(stderr,
               "loaded+interned %.3fs TUs=%zu raw=%llu Regions=%llu\n",
               secs(begin), corpus.files.size(),
               static_cast<unsigned long long>(corpus.raw),
               static_cast<unsigned long long>(dictionary.region_count()));

  std::vector<uint8_t> sent(dictionary.region_count());
  std::unordered_map<uint32_t, RecentRegions> first_line_index;
  std::unordered_map<std::string, RecentRegions> path_index;
  std::array<std::unordered_map<uint64_t, RecentRegions>, 4> anchor_index;
  std::vector<std::vector<uint8_t>> receiver_regions(dictionary.region_count());

  ZSTD_CCtx *control_encoder = ZSTD_createCCtx();
  ZSTD_CCtx *literal_encoder = ZSTD_createCCtx();
  ZSTD_DCtx *control_decoder = ZSTD_createDCtx();
  ZSTD_DCtx *literal_decoder = ZSTD_createDCtx();
  if (!control_encoder || !literal_encoder || !control_decoder ||
      !literal_decoder) {
    std::fprintf(stderr, "cannot allocate zstd contexts\n");
    return 2;
  }
  configure_stream(control_encoder, level);
  configure_stream(literal_encoder, level);

  uint64_t control_wire = 0, literal_wire = 0, raw_region_bytes = 0;
  uint64_t raw_record_bytes = 0, selected_record_bytes = 0;
  uint64_t patch_regions = 0, copied_bytes = 0, middle_bytes = 0;
  uint64_t xor_regions = 0, xor_payload_bytes = 0, xor_runs = 0;
  uint64_t candidate_regions = 0, candidate_evaluations = 0;
  std::vector<uint8_t> control_tail, literal_tail;

  for (size_t tu = 0; tu + 1 < offsets.size(); ++tu) {
    std::vector<uint32_t> new_regions;
    std::unordered_set<uint32_t> seen_in_tu;
    for (size_t position = offsets[tu]; position < offsets[tu + 1]; ++position) {
      const uint32_t region = regions[position];
      if (!sent[region] && seen_in_tu.insert(region).second)
        new_regions.push_back(region);
    }

    std::vector<uint8_t> control, literal;
    put_varint(control, new_regions.size());
    for (uint32_t region : new_regions) {
      const char *target = dictionary.region_data(region);
      const uint32_t target_size = dictionary.region_raw_len(region);
      raw_region_bytes += target_size;
      const size_t literal_cost =
          1 + varint_size(target_size) + size_t(target_size);
      raw_record_bytes += literal_cost;

      std::vector<uint32_t> candidates;
      auto append_recent = [&](const RecentRegions *recent) {
        if (!recent)
          return;
        for (auto it = recent->ids.rbegin(); it != recent->ids.rend(); ++it)
          if (std::find(candidates.begin(), candidates.end(), *it) ==
              candidates.end())
            candidates.push_back(*it);
      };

      const uint32_t *line_ids = dictionary.region_ids_ptr(region);
      const uint32_t line_count = dictionary.region_ids_count(region);
      if (line_count) {
        const auto found = first_line_index.find(line_ids[0]);
        append_recent(found == first_line_index.end() ? nullptr : &found->second);
        const LineRef &first = dictionary.ref(line_ids[0]);
        Marker marker;
        if (parse_marker(dictionary.line_data(first.off), first.len, marker)) {
          const auto path = path_index.find(marker.path);
          append_recent(path == path_index.end() ? nullptr : &path->second);
        }
      }
      const auto anchors = region_anchors(target, target_size);
      for (size_t part = 0; part < anchors.size(); ++part) {
        const auto found = anchor_index[part].find(anchors[part]);
        append_recent(found == anchor_index[part].end() ? nullptr
                                                       : &found->second);
      }
      if (!candidates.empty())
        ++candidate_regions;

      uint32_t best_base = UINT32_MAX, best_prefix = 0, best_suffix = 0;
      uint8_t best_mode = 0;
      std::vector<std::pair<uint32_t, uint32_t>> best_xor_runs;
      size_t best_cost = literal_cost;
      for (uint32_t base : candidates) {
        if (base >= region || !sent[base])
          continue;
        ++candidate_evaluations;
        const char *source = dictionary.region_data(base);
        const uint32_t source_size = dictionary.region_raw_len(base);
        const uint32_t prefix =
            common_prefix(source, source_size, target, target_size);
        const uint32_t suffix =
            common_suffix(source, source_size, target, target_size, prefix);
        const uint32_t middle = target_size - prefix - suffix;
        const size_t cost = 1 + varint_size(uint64_t(region) - base) +
                            varint_size(prefix) + varint_size(suffix) +
                            varint_size(middle) + middle;
        if (cost < best_cost) {
          best_cost = cost;
          best_base = base;
          best_prefix = prefix;
          best_suffix = suffix;
          best_mode = 1;
          best_xor_runs.clear();
        }

        // A sparse same-offset XOR is the bounded multi-edit control.  Merge
        // gaps of at most four equal bytes when carrying them literally costs
        // no more than another run descriptor.
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        uint32_t position = 0;
        while (position < target_size) {
          auto differs = [&](uint32_t offset) {
            return uint8_t(target[offset]) !=
                   (offset < source_size ? uint8_t(source[offset]) : 0);
          };
          while (position < target_size && !differs(position))
            ++position;
          if (position == target_size)
            break;
          const uint32_t run_begin = position++;
          uint32_t last_difference = run_begin;
          while (position < target_size) {
            if (differs(position)) {
              last_difference = position++;
              continue;
            }
            uint32_t gap_end = position;
            while (gap_end < target_size && !differs(gap_end) &&
                   gap_end - position <= 4)
              ++gap_end;
            if (gap_end < target_size && gap_end - position <= 4) {
              position = gap_end;
              continue;
            }
            break;
          }
          runs.emplace_back(run_begin, last_difference - run_begin + 1);
          position = last_difference + 1;
        }
        size_t xor_cost =
            1 + varint_size(uint64_t(region) - base) +
            varint_size(target_size) + varint_size(runs.size());
        uint32_t cursor = 0;
        for (const auto &run : runs) {
          xor_cost += varint_size(run.first - cursor) +
                      varint_size(run.second) + run.second;
          cursor = run.first + run.second;
        }
        if (xor_cost < best_cost) {
          best_cost = xor_cost;
          best_base = base;
          best_prefix = best_suffix = 0;
          best_mode = 2;
          best_xor_runs = std::move(runs);
        }
      }

      selected_record_bytes += best_cost;
      if (best_mode == 0) {
        control.push_back(0);
        put_varint(control, target_size);
        literal.insert(literal.end(), target, target + target_size);
      } else if (best_mode == 1) {
        const uint32_t middle = target_size - best_prefix - best_suffix;
        control.push_back(1);
        put_varint(control, uint64_t(region) - best_base);
        put_varint(control, best_prefix);
        put_varint(control, best_suffix);
        put_varint(control, middle);
        literal.insert(literal.end(), target + best_prefix,
                       target + best_prefix + middle);
        ++patch_regions;
        copied_bytes += uint64_t(best_prefix) + best_suffix;
        middle_bytes += middle;
      } else {
        control.push_back(2);
        put_varint(control, uint64_t(region) - best_base);
        put_varint(control, target_size);
        put_varint(control, best_xor_runs.size());
        const char *source = dictionary.region_data(best_base);
        const uint32_t source_size = dictionary.region_raw_len(best_base);
        uint32_t cursor = 0;
        for (const auto &run : best_xor_runs) {
          put_varint(control, run.first - cursor);
          put_varint(control, run.second);
          for (uint32_t offset = run.first;
               offset < run.first + run.second; ++offset)
            literal.push_back(uint8_t(target[offset]) ^
                              (offset < source_size ? uint8_t(source[offset])
                                                    : 0));
          cursor = run.first + run.second;
          xor_payload_bytes += run.second;
        }
        ++xor_regions;
        xor_runs += best_xor_runs.size();
      }
    }

    std::vector<uint8_t> control_encoded, literal_encoded;
    if (!control.empty()) {
      control_encoded =
          zstd_stream_encode(control_encoder, control, ZSTD_e_flush);
      control_wire += control_encoded.size() + 4;
    }
    if (!literal.empty()) {
      literal_encoded =
          zstd_stream_encode(literal_encoder, literal, ZSTD_e_flush);
      literal_wire += literal_encoded.size() + 4;
    }

    std::vector<uint8_t> decoded_control, decoded_literal;
    if (!control_encoded.empty()) {
      size_t remaining = 1;
      decoded_control = zstd_stream_decode(control_decoder, control_encoded,
                                           remaining);
      if (decoded_control != control) {
        std::fprintf(stderr, "control differs at TU %zu\n", tu);
        return 1;
      }
    }
    if (!literal_encoded.empty()) {
      size_t remaining = 1;
      decoded_literal = zstd_stream_decode(literal_decoder, literal_encoded,
                                           remaining);
      if (decoded_literal != literal) {
        std::fprintf(stderr, "literal differs at TU %zu\n", tu);
        return 1;
      }
    }

    const uint8_t *cp = decoded_control.data();
    const uint8_t *ce = cp + decoded_control.size();
    const uint8_t *lp = decoded_literal.data();
    const uint8_t *le = lp + decoded_literal.size();
    if (get_varint(cp) != new_regions.size()) {
      std::fprintf(stderr, "Region count differs at TU %zu\n", tu);
      return 1;
    }
    for (uint32_t region : new_regions) {
      if (cp >= ce) {
        std::fprintf(stderr, "truncated record at TU %zu\n", tu);
        return 1;
      }
      const uint8_t mode = *cp++;
      std::vector<uint8_t> value;
      if (mode == 0) {
        const uint64_t size = get_varint(cp);
        if (size > uint64_t(le - lp)) {
          std::fprintf(stderr, "truncated literal at TU %zu\n", tu);
          return 1;
        }
        value.assign(lp, lp + size);
        lp += size;
      } else if (mode == 1) {
        const uint64_t back = get_varint(cp);
        const uint64_t prefix = get_varint(cp);
        const uint64_t suffix = get_varint(cp);
        const uint64_t middle = get_varint(cp);
        if (!back || back > region || middle > uint64_t(le - lp)) {
          std::fprintf(stderr, "bad patch at TU %zu\n", tu);
          return 1;
        }
        const uint32_t base = region - uint32_t(back);
        const auto &source = receiver_regions[base];
        if (!sent[base] || prefix > source.size() ||
            suffix > source.size() - prefix) {
          std::fprintf(stderr, "bad patch base at TU %zu\n", tu);
          return 1;
        }
        value.insert(value.end(), source.begin(), source.begin() + prefix);
        value.insert(value.end(), lp, lp + middle);
        lp += middle;
        value.insert(value.end(), source.end() - suffix, source.end());
      } else if (mode == 2) {
        const uint64_t back = get_varint(cp);
        const uint64_t output_size = get_varint(cp);
        const uint64_t run_count = get_varint(cp);
        if (!back || back > region || output_size > UINT32_MAX) {
          std::fprintf(stderr, "bad xor header at TU %zu\n", tu);
          return 1;
        }
        const uint32_t base = region - uint32_t(back);
        if (!sent[base]) {
          std::fprintf(stderr, "bad xor base at TU %zu\n", tu);
          return 1;
        }
        const auto &source = receiver_regions[base];
        value.assign(size_t(output_size), 0);
        std::copy_n(source.begin(), std::min(source.size(), value.size()),
                    value.begin());
        uint64_t cursor = 0;
        for (uint64_t run = 0; run < run_count; ++run) {
          const uint64_t gap = get_varint(cp);
          const uint64_t length = get_varint(cp);
          if (gap > output_size - cursor ||
              length > output_size - cursor - gap ||
              length > uint64_t(le - lp)) {
            std::fprintf(stderr, "bad xor run at TU %zu\n", tu);
            return 1;
          }
          cursor += gap;
          for (uint64_t index = 0; index < length; ++index)
            value[size_t(cursor + index)] ^= *lp++;
          cursor += length;
        }
      } else {
        std::fprintf(stderr, "bad mode at TU %zu\n", tu);
        return 1;
      }
      const char *expected = dictionary.region_data(region);
      const uint32_t expected_size = dictionary.region_raw_len(region);
      if (value.size() != expected_size ||
          std::memcmp(value.data(), expected, expected_size)) {
        std::fprintf(stderr, "Region differs at TU %zu id %u\n", tu, region);
        return 1;
      }
      receiver_regions[region] = std::move(value);
    }
    if (cp != ce || lp != le) {
      std::fprintf(stderr, "trailing bytes at TU %zu\n", tu);
      return 1;
    }

    // Learn only after the complete TU has encoded and decoded exactly.
    for (uint32_t region : new_regions) {
      sent[region] = 1;
      const uint32_t *line_ids = dictionary.region_ids_ptr(region);
      const uint32_t line_count = dictionary.region_ids_count(region);
      if (line_count) {
        first_line_index[line_ids[0]].add(region);
        const LineRef &first = dictionary.ref(line_ids[0]);
        Marker marker;
        if (parse_marker(dictionary.line_data(first.off), first.len, marker))
          path_index[marker.path].add(region);
      }
      const char *data = dictionary.region_data(region);
      const auto anchors =
          region_anchors(data, dictionary.region_raw_len(region));
      for (size_t part = 0; part < anchors.size(); ++part)
        anchor_index[part][anchors[part]].add(region);
    }
  }

  const std::vector<uint8_t> empty;
  control_tail =
      zstd_stream_encode(control_encoder, empty, ZSTD_e_end);
  literal_tail =
      zstd_stream_encode(literal_encoder, empty, ZSTD_e_end);
  control_wire += control_tail.size();
  literal_wire += literal_tail.size();
  size_t remaining = 1;
  if (!control_tail.empty())
    (void)zstd_stream_decode(control_decoder, control_tail, remaining);
  if (remaining != 0) {
    std::fprintf(stderr, "control stream did not end\n");
    return 1;
  }
  remaining = 1;
  if (!literal_tail.empty())
    (void)zstd_stream_decode(literal_decoder, literal_tail, remaining);
  if (remaining != 0) {
    std::fprintf(stderr, "literal stream did not end\n");
    return 1;
  }

  ZSTD_freeCCtx(control_encoder);
  ZSTD_freeCCtx(literal_encoder);
  ZSTD_freeDCtx(control_decoder);
  ZSTD_freeDCtx(literal_decoder);

  std::printf(
      "z=%d TUs=%zu Regions=%llu raw_region_bytes=%llu "
      "candidate_regions=%llu evaluations=%llu patch_regions=%llu "
      "copied_bytes=%llu middle_bytes=%llu xor_regions=%llu xor_runs=%llu "
      "xor_payload_bytes=%llu raw_record_bytes=%llu "
      "selected_record_bytes=%llu control_wire=%llu literal_wire=%llu "
      "total_wire=%llu exact=YES elapsed_s=%.6f\n",
      level, corpus.files.size(),
      static_cast<unsigned long long>(dictionary.region_count()),
      static_cast<unsigned long long>(raw_region_bytes),
      static_cast<unsigned long long>(candidate_regions),
      static_cast<unsigned long long>(candidate_evaluations),
      static_cast<unsigned long long>(patch_regions),
      static_cast<unsigned long long>(copied_bytes),
      static_cast<unsigned long long>(middle_bytes),
      static_cast<unsigned long long>(xor_regions),
      static_cast<unsigned long long>(xor_runs),
      static_cast<unsigned long long>(xor_payload_bytes),
      static_cast<unsigned long long>(raw_record_bytes),
      static_cast<unsigned long long>(selected_record_bytes),
      static_cast<unsigned long long>(control_wire),
      static_cast<unsigned long long>(literal_wire),
      static_cast<unsigned long long>(control_wire + literal_wire), secs(begin));
}
