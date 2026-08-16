#include "mo_factor_codec.h"

#include <zstd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

[[noreturn]] static void fail(const char *message) {
  std::fprintf(stderr, "FATAL: %s\n", message);
  std::exit(2);
}

struct Frame {
  std::vector<uint8_t> bytes;
  size_t raw = 0;
};

static Frame compress(ZSTD_CCtx *context, const std::vector<uint8_t> &input,
                      int level) {
  if (ZSTD_isError(
          ZSTD_CCtx_reset(context, ZSTD_reset_session_and_parameters)) ||
      ZSTD_isError(
          ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(
          context, ZSTD_c_enableLongDistanceMatching, 1)) ||
      ZSTD_isError(ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, 27)))
    fail("zstd parameter setup");
  Frame frame;
  frame.raw = input.size();
  frame.bytes.resize(ZSTD_compressBound(input.size()));
  const size_t size =
      ZSTD_compress2(context, frame.bytes.data(), frame.bytes.size(),
                     input.data(), input.size());
  if (ZSTD_isError(size))
    fail(ZSTD_getErrorName(size));
  frame.bytes.resize(size);
  return frame;
}

static std::vector<uint8_t> decompress(ZSTD_DCtx *context, const Frame &frame) {
  std::vector<uint8_t> output(frame.raw);
  const size_t size =
      ZSTD_decompressDCtx(context, output.data(), output.size(),
                          frame.bytes.data(), frame.bytes.size());
  if (ZSTD_isError(size) || size != output.size())
    fail("zstd decode mismatch");
  return output;
}

static double elapsed(Clock::time_point begin) {
  return std::chrono::duration<double>(Clock::now() - begin).count();
}

int main(int argc, char **argv) {
  const char *input_path = nullptr;
  const char *lengths_path = nullptr;
  bool transpose_translations = false;
  bool patch_translations = false;
  for (int index = 1; index < argc; ++index) {
    if (!std::strcmp(argv[index], "--input") && index + 1 < argc)
      input_path = argv[++index];
    else if (!std::strcmp(argv[index], "--lengths") && index + 1 < argc)
      lengths_path = argv[++index];
    else if (!std::strcmp(argv[index], "--transpose-translations"))
      transpose_translations = true;
    else if (!std::strcmp(argv[index], "--patch-translations"))
      patch_translations = true;
    else
      fail("usage: mo_factor_bench --input FILE --lengths FILE "
           "[--transpose-translations|--patch-translations]");
  }
  if (!input_path || !lengths_path)
    fail("usage: mo_factor_bench --input FILE --lengths FILE "
         "[--transpose-translations|--patch-translations]");
  if (transpose_translations && patch_translations)
    fail("translation layouts are mutually exclusive");

  std::ifstream input_file(input_path, std::ios::binary),
      lengths_file(lengths_path, std::ios::binary);
  if (!input_file || !lengths_file)
    fail("cannot open input");
  std::vector<uint8_t> input((std::istreambuf_iterator<char>(input_file)), {});
  std::vector<uint8_t> length_bytes(
      (std::istreambuf_iterator<char>(lengths_file)), {});
  if (length_bytes.size() % 4)
    fail("bad lengths file");
  std::vector<uint32_t> expected_lengths;
  std::vector<mo_factor::Slice> members;
  size_t offset = 0;
  for (size_t cursor = 0; cursor < length_bytes.size(); cursor += 4) {
    const uint32_t size = mo_factor::read_u32(length_bytes.data() + cursor);
    if (size > input.size() - offset)
      fail("member extent exceeds input");
    expected_lengths.push_back(size);
    members.push_back({input.data() + offset, size});
    offset += size;
  }
  if (offset != input.size())
    fail("member lengths do not cover input");

  mo_factor::EncoderState encoder;
  mo_factor::Encoded encoded;
  const auto transform_begin = Clock::now();
  if (!encoder.encode(members, encoded, transpose_translations,
                      patch_translations))
    fail("MO factor encode");
  const double transform_seconds = elapsed(transform_begin);
  std::printf(
      "members=%zu input=%zu mo_members=%u mo_bytes=%llu new_originals=%zu "
      "control_raw=%zu definitions_raw=%zu translations_raw=%zu "
      "ordinary_raw=%zu "
      "transform_s=%.6f layout=%s\n",
      members.size(), input.size(), encoded.mo_members,
      static_cast<unsigned long long>(encoded.mo_bytes),
      encoded.pending_definitions.size(), encoded.control.size(),
      encoded.definitions.size(), encoded.translations.size(),
      encoded.ordinary.size(), transform_seconds,
      transpose_translations ? "transpose"
                             : (patch_translations ? "original-patch"
                                                   : "plain"));
  std::vector<uint8_t> material = encoded.definitions;
  material.insert(material.end(), encoded.translations.begin(),
                  encoded.translations.end());
  std::vector<uint8_t> semantic = encoded.control;
  semantic.insert(semantic.end(), material.begin(), material.end());
  std::vector<uint8_t> factor_all = semantic;
  factor_all.insert(factor_all.end(), encoded.ordinary.begin(),
                    encoded.ordinary.end());
  std::printf(
      "level\tbaseline\tfactor4\tfactor3\tfactor2\tfactor1\tbest\tsaving\t"
      "encode_s\tdecode_s\texact\n");

  ZSTD_CCtx *compression = ZSTD_createCCtx();
  ZSTD_DCtx *decompression = ZSTD_createDCtx();
  if (!compression || !decompression)
    fail("zstd context allocation");
  for (const int level : {1, 3, 6, 9}) {
    const auto encode_begin = Clock::now();
    const Frame baseline = compress(compression, input, level);
    const Frame control = compress(compression, encoded.control, level);
    const Frame definitions = compress(compression, encoded.definitions, level);
    const Frame translations =
        compress(compression, encoded.translations, level);
    const Frame ordinary = compress(compression, encoded.ordinary, level);
    const Frame material_frame = compress(compression, material, level);
    const Frame semantic_frame = compress(compression, semantic, level);
    const Frame all_frame = compress(compression, factor_all, level);
    const double encode_seconds = elapsed(encode_begin) + transform_seconds;
    const uint64_t wire4 = control.bytes.size() + definitions.bytes.size() +
                           translations.bytes.size() + ordinary.bytes.size() +
                           16;
    const uint64_t wire3 = control.bytes.size() + material_frame.bytes.size() +
                           ordinary.bytes.size() + 12;
    const uint64_t wire2 =
        semantic_frame.bytes.size() + ordinary.bytes.size() + 8;
    const uint64_t wire1 = all_frame.bytes.size() + 4;
    const uint64_t wire = std::min({wire4, wire3, wire2, wire1});

    const auto decode_begin = Clock::now();
    const auto control_raw = decompress(decompression, control);
    const auto definitions_raw = decompress(decompression, definitions);
    const auto translations_raw = decompress(decompression, translations);
    const auto ordinary_raw = decompress(decompression, ordinary);
    mo_factor::DecoderState decoder;
    std::vector<uint8_t> recovered;
    std::vector<uint32_t> recovered_lengths;
    auto trailing_control = control_raw;
    trailing_control.push_back(0);
    if (decoder.decode(trailing_control, definitions_raw, translations_raw,
                       ordinary_raw, recovered, recovered_lengths,
                       transpose_translations, patch_translations) ||
        decoder.size() || decoder.string_bytes())
      fail("failed frame changed decoder state");
    const bool decoded =
        decoder.decode(control_raw, definitions_raw, translations_raw,
                       ordinary_raw, recovered, recovered_lengths,
                       transpose_translations, patch_translations);
    const double decode_seconds = elapsed(decode_begin);
    const bool exact =
        decoded && recovered == input && recovered_lengths == expected_lengths;
    std::printf("%d\t%zu\t%llu\t%llu\t%llu\t%llu\t%llu\t%lld\t%.6f\t%.6f\t%s\n",
                level, baseline.bytes.size() + 4,
                static_cast<unsigned long long>(wire4),
                static_cast<unsigned long long>(wire3),
                static_cast<unsigned long long>(wire2),
                static_cast<unsigned long long>(wire1),
                static_cast<unsigned long long>(wire),
                static_cast<long long>(baseline.bytes.size() + 4) -
                    static_cast<long long>(wire),
                encode_seconds, decode_seconds, exact ? "EXACT" : "FAIL");
    if (!exact)
      return 1;
  }
  ZSTD_freeCCtx(compression);
  ZSTD_freeDCtx(decompression);
  return 0;
}
