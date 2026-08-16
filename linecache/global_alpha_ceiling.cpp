// Exact generation-wide ceiling for alpha-normalized Line superblocks.
//
// This intentionally gives the rule builder the complete residual stream.  It
// answers only whether cross-TU rule lifetime has enough byte headroom to
// justify a causal online codec.

#include "alpha_line_codec.h"

#include <zstd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using Clock = std::chrono::steady_clock;

[[noreturn]] static void fail(const char *message) {
  std::fprintf(stderr, "FATAL: %s\n", message);
  std::exit(2);
}

struct Frame {
  std::vector<uint8_t> bytes;
};

static Frame compress(const std::vector<uint8_t> &input, int level) {
  Frame frame;
  frame.bytes.resize(ZSTD_compressBound(input.size()));
  const size_t size = ZSTD_compress(frame.bytes.data(), frame.bytes.size(),
                                    input.data(), input.size(), level);
  if (ZSTD_isError(size))
    fail(ZSTD_getErrorName(size));
  frame.bytes.resize(size);
  return frame;
}

static double elapsed(Clock::time_point begin) {
  return std::chrono::duration<double>(Clock::now() - begin).count();
}

int main(int argc, char **argv) {
  const char *input_path = nullptr;
  int level = 3;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--input") && i + 1 < argc)
      input_path = argv[++i];
    else if (!std::strcmp(argv[i], "--z") && i + 1 < argc)
      level = std::atoi(argv[++i]);
    else
      fail("usage: global_alpha_ceiling --input FILE [--z LEVEL]");
  }
  if (!input_path || level < 1 || level > 19)
    fail("usage: global_alpha_ceiling --input FILE [--z LEVEL]");

  std::ifstream file(input_path, std::ios::binary);
  if (!file)
    fail("cannot open input");
  std::vector<uint8_t> input((std::istreambuf_iterator<char>(file)), {});
  std::vector<alpha_line::Slice> lines;
  for (size_t begin = 0; begin < input.size();) {
    size_t end = begin;
    while (end < input.size() && input[end++] != '\n') {
    }
    if (end - begin > UINT32_MAX)
      fail("Line too large");
    lines.push_back({input.data() + begin, uint32_t(end - begin)});
    begin = end;
  }
  std::vector<uint32_t> gaps(lines.size() + 1);
  const std::vector<uint8_t> gap_bytes;
  const Frame ordinary = compress(input, level);
  std::printf("input=%zu lines=%zu zstd%d=%zu\n", input.size(), lines.size(),
              level, ordinary.bytes.size());
  std::printf(
      "mode\tcontrol_raw\tdata_raw\tcontrol_wire\tdata_wire\ttotal_wire\t"
      "saving\trules\tinstances\ttemplate_bytes\tfallbacks\tlexicon\t"
      "lexicon_refs\tencode_s\texact\n");
  for (int parameterized = 0; parameterized <= 1; ++parameterized) {
    const auto begin = Clock::now();
    alpha_line::Encoded encoded =
        alpha_line::encode(lines, gaps, gap_bytes, parameterized);
    if (!encoded.valid) {
      std::fprintf(stderr, "alpha encode: %s\n", encoded.error.c_str());
      return 2;
    }
    const Frame control = compress(encoded.control, level);
    const Frame data = compress(encoded.data, level);
    std::vector<uint8_t> recovered;
    std::string error;
    const bool exact =
        alpha_line::decode(encoded.control, encoded.data, recovered, error) &&
        recovered == input;
    const uint64_t wire = control.bytes.size() + data.bytes.size() + 8;
    const double encode_seconds = elapsed(begin);
    const auto &stats = encoded.stats;
    std::printf("%s\t%zu\t%zu\t%zu\t%zu\t%llu\t%lld\t%llu\t%llu\t%llu\t"
                "%llu\t%llu\t%llu\t%.6f\t%s\n",
                parameterized ? "parameterized-keywords" : "literal-keywords",
                encoded.control.size(), encoded.data.size(),
                control.bytes.size(), data.bytes.size(),
                static_cast<unsigned long long>(wire),
                static_cast<long long>(ordinary.bytes.size()) -
                    static_cast<long long>(wire),
                static_cast<unsigned long long>(stats.rules),
                static_cast<unsigned long long>(stats.instances),
                static_cast<unsigned long long>(stats.template_input_bytes),
                static_cast<unsigned long long>(stats.literal_fallbacks),
                static_cast<unsigned long long>(stats.lexicon_entries),
                static_cast<unsigned long long>(stats.lexicon_references),
                encode_seconds, exact ? "EXACT" : "FAIL");
    if (!exact) {
      std::fprintf(stderr, "alpha decode: %s\n", error.c_str());
      return 1;
    }
  }
  return 0;
}
