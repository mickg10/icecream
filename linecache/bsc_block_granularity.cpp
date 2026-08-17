// Measure exact residual-lane coding at fixed TU group sizes.
//
// The input is one concatenated raw P29 material lane plus codec50's binary
// little-endian u32 length table.  The selected lane lengths must sum exactly
// to the raw input size.  Each candidate group is independently decoded and
// compared before its bytes enter the result.
//
// Example build (libbsc is intentionally an external research dependency):
//   g++ -O3 -std=c++17 -Wall -Wextra -Wpedantic -Werror
//   -I/path/to/libbsc linecache/bsc_block_granularity.cpp /path/to/libbsc.a
//   -lzstd -fopenmp -o bsc_block_granularity

#include <zstd.h>

#include "libbsc.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kFrameBytes = 4;
constexpr std::size_t kSelectorBytes = 1;
constexpr std::size_t kBscBlockBytes = 64u * 1024u * 1024u;
constexpr int kBscFeatures =
    LIBBSC_FEATURE_FASTMODE | LIBBSC_FEATURE_MULTITHREADING;

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error(message);
}

Bytes read_file(const std::string &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) fail("cannot open " + path);
    const std::streamoff end = input.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) >
                       std::numeric_limits<std::size_t>::max()) {
        fail("invalid file size for " + path);
    }
    Bytes bytes(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!input) fail("short read from " + path);
    }
    return bytes;
}

std::vector<std::uint32_t> read_lengths(const std::string &path) {
    const Bytes raw = read_file(path);
    if (raw.size() % 4 != 0) fail("length table is not a u32 array");
    std::vector<std::uint32_t> lengths(raw.size() / 4);
    for (std::size_t index = 0; index < lengths.size(); ++index) {
        const unsigned char *p = raw.data() + 4 * index;
        lengths[index] = std::uint32_t(p[0]) |
                         (std::uint32_t(p[1]) << 8) |
                         (std::uint32_t(p[2]) << 16) |
                         (std::uint32_t(p[3]) << 24);
    }
    return lengths;
}

std::size_t parse_size(std::string_view text, const char *name) {
    if (text.empty()) fail(std::string("empty ") + name);
    std::size_t value = 0;
    for (char character : text) {
        if (character < '0' || character > '9') {
            fail(std::string("invalid ") + name + ": " + std::string(text));
        }
        const std::size_t digit = static_cast<std::size_t>(character - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            fail(std::string(name) + " overflows");
        }
        value = value * 10 + digit;
    }
    return value;
}

std::vector<std::size_t> parse_group_sizes(std::string_view text) {
    std::vector<std::size_t> result;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        if (item == "full") {
            result.push_back(0);
        } else {
            const std::size_t value = parse_size(item, "group size");
            if (value == 0) fail("numeric group size must be positive");
            result.push_back(value);
        }
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    if (result.empty()) fail("no group sizes supplied");
    return result;
}

struct CodecResult {
    std::size_t payload_bytes = 0;
    std::size_t blocks = 0;
    double encode_seconds = 0.0;
    double decode_seconds = 0.0;
};

struct GroupMeasurement {
    std::size_t raw_bytes = 0;
    std::size_t bsc_blocks = 0;
    std::size_t bsc_wire = 0;
    std::size_t zstd3_wire = 0;
    std::size_t zstd10_wire = 0;
    std::size_t selected_wire = 0;
    const char *selected_codec = "empty";
};

CodecResult encode_bsc_exact(const unsigned char *input, std::size_t size) {
    CodecResult result;
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t part_size = std::min(kBscBlockBytes, size - offset);
        if (part_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            fail("libbsc block exceeds int range");
        }
        Bytes encoded(part_size + LIBBSC_HEADER_SIZE);
        const auto encode_start = Clock::now();
        int encoded_size = bsc_compress(
            input + offset, encoded.data(), static_cast<int>(part_size),
            LIBBSC_DEFAULT_LZPHASHSIZE, LIBBSC_DEFAULT_LZPMINLEN,
            LIBBSC_BLOCKSORTER_BWT, LIBBSC_CODER_QLFC_ADAPTIVE,
            kBscFeatures);
        if (encoded_size == LIBBSC_NOT_COMPRESSIBLE) {
            encoded_size = bsc_store(input + offset, encoded.data(),
                                     static_cast<int>(part_size), kBscFeatures);
        }
        result.encode_seconds +=
            std::chrono::duration<double>(Clock::now() - encode_start).count();
        if (encoded_size < LIBBSC_NO_ERROR) {
            fail("libbsc encode failed: " + std::to_string(encoded_size));
        }
        encoded.resize(static_cast<std::size_t>(encoded_size));

        int block_size = 0;
        int decoded_size = 0;
        if (bsc_block_info(encoded.data(), static_cast<int>(encoded.size()),
                           &block_size, &decoded_size, kBscFeatures) !=
                LIBBSC_NO_ERROR ||
            block_size != encoded_size || decoded_size != static_cast<int>(part_size)) {
            fail("libbsc block header differs from encoded extent");
        }
        Bytes decoded(part_size);
        const auto decode_start = Clock::now();
        const int decode_result =
            bsc_decompress(encoded.data(), encoded_size, decoded.data(),
                           decoded_size, kBscFeatures);
        result.decode_seconds +=
            std::chrono::duration<double>(Clock::now() - decode_start).count();
        if (decode_result != LIBBSC_NO_ERROR ||
            !std::equal(decoded.begin(), decoded.end(), input + offset)) {
            fail("libbsc independent decode differs");
        }
        result.payload_bytes += encoded.size();
        ++result.blocks;
        offset += part_size;
    }
    return result;
}

CodecResult encode_zstd_exact(ZSTD_CCtx *compressor, ZSTD_DCtx *decompressor,
                              const unsigned char *input, std::size_t size,
                              int level) {
    CodecResult result;
    Bytes encoded(ZSTD_compressBound(size));
    const auto encode_start = Clock::now();
    const std::size_t encoded_size = ZSTD_compressCCtx(
        compressor, encoded.data(), encoded.size(), input, size, level);
    result.encode_seconds =
        std::chrono::duration<double>(Clock::now() - encode_start).count();
    if (ZSTD_isError(encoded_size)) fail(ZSTD_getErrorName(encoded_size));
    encoded.resize(encoded_size);

    Bytes decoded(size);
    const auto decode_start = Clock::now();
    const std::size_t decoded_size = ZSTD_decompressDCtx(
        decompressor, decoded.data(), decoded.size(), encoded.data(), encoded.size());
    result.decode_seconds =
        std::chrono::duration<double>(Clock::now() - decode_start).count();
    if (ZSTD_isError(decoded_size) || decoded_size != size ||
        !std::equal(decoded.begin(), decoded.end(), input)) {
        fail("zstd independent decode differs");
    }
    result.payload_bytes = encoded.size();
    result.blocks = 1;
    return result;
}

struct Totals {
    std::size_t groups = 0;
    std::size_t nonempty_groups = 0;
    std::size_t raw_bytes = 0;
    std::size_t bsc_blocks = 0;
    std::size_t bsc_wire = 0;
    std::size_t zstd3_wire = 0;
    std::size_t zstd10_wire = 0;
    std::size_t selected_wire = 0;
    std::size_t selected_bsc = 0;
    std::size_t selected_zstd3 = 0;
    std::size_t selected_zstd10 = 0;
    double bsc_encode_seconds = 0.0;
    double bsc_decode_seconds = 0.0;
    double zstd3_encode_seconds = 0.0;
    double zstd10_encode_seconds = 0.0;
};

GroupMeasurement add_group(Totals &totals, ZSTD_CCtx *compressor,
                           ZSTD_DCtx *decompressor,
                           const unsigned char *input, std::size_t size) {
    GroupMeasurement measurement;
    measurement.raw_bytes = size;
    ++totals.groups;
    totals.raw_bytes += size;
    if (size == 0) return measurement;
    ++totals.nonempty_groups;

    const CodecResult bsc = encode_bsc_exact(input, size);
    const CodecResult zstd3 =
        encode_zstd_exact(compressor, decompressor, input, size, 3);
    const CodecResult zstd10 =
        encode_zstd_exact(compressor, decompressor, input, size, 10);
    const std::size_t framing = kFrameBytes + kSelectorBytes;
    const std::size_t bsc_wire = bsc.payload_bytes + framing;
    const std::size_t zstd3_wire = zstd3.payload_bytes + framing;
    const std::size_t zstd10_wire = zstd10.payload_bytes + framing;

    measurement.bsc_blocks = bsc.blocks;
    measurement.bsc_wire = bsc_wire;
    measurement.zstd3_wire = zstd3_wire;
    measurement.zstd10_wire = zstd10_wire;

    totals.bsc_blocks += bsc.blocks;
    totals.bsc_wire += bsc_wire;
    totals.zstd3_wire += zstd3_wire;
    totals.zstd10_wire += zstd10_wire;
    totals.bsc_encode_seconds += bsc.encode_seconds;
    totals.bsc_decode_seconds += bsc.decode_seconds;
    totals.zstd3_encode_seconds += zstd3.encode_seconds;
    totals.zstd10_encode_seconds += zstd10.encode_seconds;

    const std::size_t selected = std::min({bsc_wire, zstd3_wire, zstd10_wire});
    totals.selected_wire += selected;
    measurement.selected_wire = selected;
    if (selected == bsc_wire) {
        ++totals.selected_bsc;
        measurement.selected_codec = "bsc";
    } else if (selected == zstd3_wire) {
        ++totals.selected_zstd3;
        measurement.selected_codec = "zstd3";
    } else {
        ++totals.selected_zstd10;
        measurement.selected_codec = "zstd10";
    }
    return measurement;
}

}  // namespace

int main(int argc, char **argv) try {
    std::string raw_path;
    std::string lengths_path;
    std::string output_path;
    std::string detail_path;
    std::vector<std::size_t> group_sizes{1, 10, 25, 50, 100, 0};
    std::size_t stride = 0;
    std::size_t lane = 0;

    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        auto value = [&](const char *name) -> std::string_view {
            if (++index >= argc) fail(std::string("missing value for ") + name);
            return argv[index];
        };
        if (option == "--raw") {
            raw_path = value("--raw");
        } else if (option == "--lengths") {
            lengths_path = value("--lengths");
        } else if (option == "--stride") {
            stride = parse_size(value("--stride"), "stride");
        } else if (option == "--lane") {
            lane = parse_size(value("--lane"), "lane");
        } else if (option == "--group-tus") {
            group_sizes = parse_group_sizes(value("--group-tus"));
        } else if (option == "--output") {
            output_path = value("--output");
        } else if (option == "--detail-output") {
            detail_path = value("--detail-output");
        } else {
            fail("unknown option: " + std::string(option));
        }
    }
    if (raw_path.empty() || lengths_path.empty() || stride == 0 || lane >= stride) {
        fail("usage: bsc_block_granularity --raw FILE --lengths FILE "
             "--stride N --lane N [--group-tus 1,10,25,50,100,full] "
             "[--output TSV] [--detail-output TSV]");
    }

    const Bytes raw = read_file(raw_path);
    const std::vector<std::uint32_t> lengths = read_lengths(lengths_path);
    if (lengths.size() % stride != 0) fail("length count is not divisible by stride");
    const std::size_t tus = lengths.size() / stride;
    if (tus == 0) fail("length table contains no TUs");

    std::vector<std::size_t> offsets(tus + 1);
    for (std::size_t tu = 0; tu < tus; ++tu) {
        const std::size_t length = lengths[tu * stride + lane];
        if (offsets[tu] > std::numeric_limits<std::size_t>::max() - length) {
            fail("selected lane size overflows");
        }
        offsets[tu + 1] = offsets[tu] + length;
    }
    if (offsets.back() != raw.size()) {
        fail("selected lane lengths sum to " + std::to_string(offsets.back()) +
             " but raw input has " + std::to_string(raw.size()) + " bytes");
    }
    if (bsc_init(kBscFeatures) != LIBBSC_NO_ERROR) fail("bsc_init failed");
    ZSTD_CCtx *compressor = ZSTD_createCCtx();
    ZSTD_DCtx *decompressor = ZSTD_createDCtx();
    if (compressor == nullptr || decompressor == nullptr) fail("zstd allocation failed");

    std::ofstream output_file;
    std::ostream *output = &std::cout;
    if (!output_path.empty()) {
        output_file.open(output_path);
        if (!output_file) fail("cannot open output " + output_path);
        output = &output_file;
    }
    std::ofstream detail_file;
    std::ostream *detail = nullptr;
    if (!detail_path.empty()) {
        detail_file.open(detail_path);
        if (!detail_file) fail("cannot open detail output " + detail_path);
        detail = &detail_file;
        *detail << "group_tus\tfirst_tu\tlast_tu\traw_bytes\tbsc_blocks"
                   "\tbsc_wire_bytes\tzstd3_wire_bytes\tzstd10_wire_bytes"
                   "\tselected_codec\tselected_wire_bytes"
                   "\tcumulative_selected_wire_bytes\texact\n";
    }

    *output << "group_tus\ttus\tgroups\tnonempty_groups\traw_bytes\tbsc_blocks"
               "\tbsc_wire_bytes\tzstd3_wire_bytes\tzstd10_wire_bytes"
               "\tselected_wire_bytes\tselected_bsc_groups"
               "\tselected_zstd3_groups\tselected_zstd10_groups"
               "\tbsc_encode_seconds\tbsc_decode_seconds"
               "\tzstd3_encode_seconds\tzstd10_encode_seconds\texact\n";
    for (const std::size_t requested : group_sizes) {
        const std::size_t group_tus = requested == 0 ? tus : requested;
        const std::string label = requested == 0 ? "full" : std::to_string(requested);
        Totals totals;
        std::size_t cumulative_selected = 0;
        for (std::size_t first = 0; first < tus; first += group_tus) {
            const std::size_t last = std::min(tus, first + group_tus);
            const GroupMeasurement measurement = add_group(
                totals, compressor, decompressor, raw.data() + offsets[first],
                offsets[last] - offsets[first]);
            cumulative_selected += measurement.selected_wire;
            if (detail != nullptr) {
                *detail << label << '\t' << first + 1 << '\t' << last << '\t'
                        << measurement.raw_bytes << '\t' << measurement.bsc_blocks
                        << '\t' << measurement.bsc_wire << '\t'
                        << measurement.zstd3_wire << '\t'
                        << measurement.zstd10_wire << '\t'
                        << measurement.selected_codec << '\t'
                        << measurement.selected_wire << '\t' << cumulative_selected
                        << "\ttrue\n";
            }
        }
        *output << label << '\t' << tus << '\t' << totals.groups << '\t'
                << totals.nonempty_groups << '\t' << totals.raw_bytes << '\t'
                << totals.bsc_blocks << '\t' << totals.bsc_wire << '\t'
                << totals.zstd3_wire << '\t' << totals.zstd10_wire << '\t'
                << totals.selected_wire << '\t' << totals.selected_bsc << '\t'
                << totals.selected_zstd3 << '\t' << totals.selected_zstd10 << '\t'
                << totals.bsc_encode_seconds << '\t' << totals.bsc_decode_seconds
                << '\t' << totals.zstd3_encode_seconds << '\t'
                << totals.zstd10_encode_seconds << "\ttrue\n";
    }

    ZSTD_freeCCtx(compressor);
    ZSTD_freeDCtx(decompressor);
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "bsc_block_granularity: %s\n", error.what());
    return 2;
}
