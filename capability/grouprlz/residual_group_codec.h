#pragma once

// Exact bounded-group residual frames for the Issue #16 P29 experiment.
//
// A frame is one little-endian u32 followed by its payload.  The upper three
// header bits select the codec and the lower 29 bits carry the payload length:
//
//   0 = zstd-3, 1 = libbsc BWT+QLFC, 2 = zstd-10
//
// Both zstd frames and libbsc blocks describe their decoded size, so no raw
// length is duplicated on the wire.  A libbsc payload may contain consecutive
// self-describing blocks, each capped at 64 MiB raw.

#include <zstd.h>

#include "libbsc.h"
#if defined(_OPENMP)
#include <omp.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace residual_group {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kHeaderBytes = 4;
constexpr std::size_t kMaxPayloadBytes = (std::size_t{1} << 29) - 1;
constexpr std::size_t kBscBlockBytes = 64u * 1024u * 1024u;
constexpr int kBscFeatures =
    LIBBSC_FEATURE_FASTMODE | LIBBSC_FEATURE_MULTITHREADING;

enum class Kind : std::uint8_t {
    Zstd3 = 0,
    Bsc = 1,
    Zstd10 = 2,
};

[[noreturn]] inline void fail(const std::string &message) {
    throw std::runtime_error("residual group: " + message);
}

inline void checked_zstd(std::size_t result, const char *operation) {
    if (ZSTD_isError(result)) {
        fail(std::string(operation) + ": " + ZSTD_getErrorName(result));
    }
}

inline std::uint32_t read_u32le(const std::uint8_t *input) {
    return std::uint32_t(input[0]) | (std::uint32_t(input[1]) << 8) |
           (std::uint32_t(input[2]) << 16) | (std::uint32_t(input[3]) << 24);
}

inline void append_u32le(Bytes &output, std::uint32_t value) {
    output.push_back(std::uint8_t(value));
    output.push_back(std::uint8_t(value >> 8));
    output.push_back(std::uint8_t(value >> 16));
    output.push_back(std::uint8_t(value >> 24));
}

struct DecodedFrame {
    Kind kind = Kind::Zstd3;
    std::size_t wire_bytes = 0;
    Bytes raw;
};

// Product residual coding runs on one owner thread per transaction.  libbsc's
// LZP, BWT, and block-coder stages otherwise fan out over OpenMP worker threads
// inside every call.  That is the only nondeterministic execution on the codec
// path; pin each libbsc call to one thread.  The multithreading feature flag is
// kept so the block format (aux BWT indexes) stays byte-identical to the
// accepted evidence; only the execution width changes.
class SingleThreadLibbscScope {
public:
    SingleThreadLibbscScope() {
#if defined(_OPENMP)
        saved_ = omp_get_max_threads();
        omp_set_num_threads(1);
#endif
    }
    ~SingleThreadLibbscScope() {
#if defined(_OPENMP)
        omp_set_num_threads(saved_ > 0 ? saved_ : 1);
#endif
    }
    SingleThreadLibbscScope(const SingleThreadLibbscScope &) = delete;
    SingleThreadLibbscScope &operator=(const SingleThreadLibbscScope &) = delete;

private:
    int saved_ = 1;
};

class Codec {
public:
    Codec() : compressor_(ZSTD_createCCtx()), decompressor_(ZSTD_createDCtx()) {
        if (compressor_ == nullptr || decompressor_ == nullptr) {
            if (compressor_ != nullptr) ZSTD_freeCCtx(compressor_);
            if (decompressor_ != nullptr) ZSTD_freeDCtx(decompressor_);
            fail("zstd context allocation failed");
        }
        static std::once_flag initialized;
        static int initialize_result = LIBBSC_NO_ERROR;
        std::call_once(initialized, [] { initialize_result = bsc_init(kBscFeatures); });
        if (initialize_result != LIBBSC_NO_ERROR)
            fail("bsc_init failed: " + std::to_string(initialize_result));
    }

    Codec(const Codec &) = delete;
    Codec &operator=(const Codec &) = delete;

    ~Codec() {
        ZSTD_freeCCtx(compressor_);
        ZSTD_freeDCtx(decompressor_);
    }

    Bytes encode(const std::uint8_t *input, std::size_t size, Kind *selected = nullptr,
                 bool evaluate_zstd10 = true) {
        if (size == 0) {
            if (selected != nullptr) *selected = Kind::Zstd3;
            return {};
        }
        Bytes zstd3 = encode_zstd(input, size, 3);
        Bytes bsc = encode_bsc(input, size);
        Bytes zstd10;

        Kind kind = Kind::Zstd3;
        Bytes *payload = &zstd3;
        if (bsc.size() < payload->size()) {
            kind = Kind::Bsc;
            payload = &bsc;
        }
        if (evaluate_zstd10) {
            zstd10 = encode_zstd(input, size, 10);
            if (zstd10.size() < payload->size()) {
                kind = Kind::Zstd10;
                payload = &zstd10;
            }
        }
        if (payload->size() > kMaxPayloadBytes) fail("payload exceeds packed u32 frame");

        const std::uint32_t header =
            (std::uint32_t(kind) << 29) | std::uint32_t(payload->size());
        Bytes wire;
        wire.reserve(kHeaderBytes + payload->size());
        append_u32le(wire, header);
        wire.insert(wire.end(), payload->begin(), payload->end());
        if (selected != nullptr) *selected = kind;
        return wire;
    }

    DecodedFrame decode(const std::uint8_t *wire, std::size_t available) {
        if (available < kHeaderBytes) fail("truncated frame header");
        const std::uint32_t header = read_u32le(wire);
        const std::uint8_t raw_kind = std::uint8_t(header >> 29);
        const std::size_t payload_size = header & ((std::uint32_t{1} << 29) - 1);
        if (payload_size > available - kHeaderBytes) fail("truncated frame payload");
        if (raw_kind > std::uint8_t(Kind::Zstd10)) fail("unknown frame codec");
        const Kind kind = Kind(raw_kind);
        const std::uint8_t *payload = wire + kHeaderBytes;
        Bytes raw = kind == Kind::Bsc
                        ? decode_bsc(payload, payload_size)
                        : decode_zstd(payload, payload_size);
        return {kind, kHeaderBytes + payload_size, std::move(raw)};
    }

private:
    Bytes encode_zstd(const std::uint8_t *input, std::size_t size, int level) {
        checked_zstd(ZSTD_CCtx_reset(compressor_, ZSTD_reset_session_and_parameters),
                     "reset zstd encoder");
        checked_zstd(ZSTD_CCtx_setParameter(compressor_, ZSTD_c_compressionLevel, level),
                     "set zstd level");
        checked_zstd(ZSTD_CCtx_setParameter(compressor_, ZSTD_c_contentSizeFlag, 1),
                     "enable zstd content size");
        Bytes output(ZSTD_compressBound(size));
        const std::size_t result = ZSTD_compress2(
            compressor_, output.data(), output.size(), input, size);
        checked_zstd(result, "encode zstd frame");
        output.resize(result);
        return output;
    }

    Bytes decode_zstd(const std::uint8_t *payload, std::size_t payload_size) {
        const unsigned long long raw_size =
            ZSTD_getFrameContentSize(payload, payload_size);
        if (raw_size == ZSTD_CONTENTSIZE_ERROR ||
            raw_size == ZSTD_CONTENTSIZE_UNKNOWN || raw_size > SIZE_MAX) {
            fail("zstd frame has no valid decoded size");
        }
        Bytes output(static_cast<std::size_t>(raw_size));
        std::uint8_t empty = 0;
        void *destination = output.empty() ? static_cast<void *>(&empty)
                                           : static_cast<void *>(output.data());
        const std::size_t result = ZSTD_decompressDCtx(
            decompressor_, destination, output.size(), payload, payload_size);
        checked_zstd(result, "decode zstd frame");
        if (result != output.size()) fail("zstd decoded size differs");
        const std::size_t frame_size = ZSTD_findFrameCompressedSize(payload, payload_size);
        checked_zstd(frame_size, "measure zstd frame");
        if (frame_size != payload_size) fail("zstd payload has trailing bytes");
        return output;
    }

    Bytes encode_bsc(const std::uint8_t *input, std::size_t size) {
        const SingleThreadLibbscScope single_thread;
        Bytes output;
        std::size_t offset = 0;
        while (offset < size) {
            const std::size_t part_size = std::min(kBscBlockBytes, size - offset);
            if (part_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                fail("libbsc block exceeds int range");
            }
            Bytes block(part_size + LIBBSC_HEADER_SIZE);
            int encoded_size = bsc_compress(
                input + offset, block.data(), static_cast<int>(part_size),
                LIBBSC_DEFAULT_LZPHASHSIZE, LIBBSC_DEFAULT_LZPMINLEN,
                LIBBSC_BLOCKSORTER_BWT, LIBBSC_CODER_QLFC_ADAPTIVE,
                kBscFeatures);
            if (encoded_size == LIBBSC_NOT_COMPRESSIBLE) {
                encoded_size = bsc_store(input + offset, block.data(),
                                         static_cast<int>(part_size), kBscFeatures);
            }
            if (encoded_size < LIBBSC_NO_ERROR) {
                fail("libbsc encode failed: " + std::to_string(encoded_size));
            }
            block.resize(static_cast<std::size_t>(encoded_size));
            output.insert(output.end(), block.begin(), block.end());
            offset += part_size;
        }
        return output;
    }

    Bytes decode_bsc(const std::uint8_t *payload, std::size_t payload_size) {
        const SingleThreadLibbscScope single_thread;
        Bytes output;
        std::size_t offset = 0;
        while (offset < payload_size) {
            const std::size_t remaining = payload_size - offset;
            if (remaining > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                fail("libbsc payload exceeds int range");
            }
            int block_size = 0;
            int raw_size = 0;
            const int info = bsc_block_info(
                payload + offset, static_cast<int>(remaining), &block_size, &raw_size,
                kBscFeatures);
            if (info != LIBBSC_NO_ERROR || block_size <= 0 || raw_size < 0 ||
                static_cast<std::size_t>(block_size) > remaining) {
                fail("invalid libbsc block header");
            }
            const std::size_t destination = output.size();
            output.resize(destination + static_cast<std::size_t>(raw_size));
            std::uint8_t empty = 0;
            std::uint8_t *decoded = raw_size == 0 ? &empty : output.data() + destination;
            const int result = bsc_decompress(
                payload + offset, block_size, decoded, raw_size, kBscFeatures);
            if (result != LIBBSC_NO_ERROR) {
                fail("libbsc decode failed: " + std::to_string(result));
            }
            offset += static_cast<std::size_t>(block_size);
        }
        if (offset != payload_size) fail("libbsc payload has trailing bytes");
        return output;
    }

    ZSTD_CCtx *compressor_ = nullptr;
    ZSTD_DCtx *decompressor_ = nullptr;
};

inline const char *kind_name(Kind kind) {
    switch (kind) {
    case Kind::Zstd3:
        return "zstd3";
    case Kind::Bsc:
        return "bsc";
    case Kind::Zstd10:
        return "zstd10";
    }
    return "unknown";
}

}  // namespace residual_group
