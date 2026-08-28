#pragma once

// CW_P29_BSC_Z3_M64 residual-group transport.  The reviewed implementation is
// capability/grouprlz/residual_group_codec.h (SHA-256
// 9f4d721e1495987bc1d5f207e23d31a76fab9b780d5610832927df8f398895f9).
// It is sourced from origin/implementer/issue16-capability at
// 0b70be88b57e5162442b04fd6e795d06bbc61034.
// Keep the include conditional so ordinary developer builds remain usable when
// libbsc is not installed; strict P29 builds add the reviewed libbsc include
// and library roots and therefore exercise the canonical BSC-vs-ZSTD choice.

#if __has_include("libbsc.h")
#include "capability/grouprlz/residual_group_codec.h"
#else

#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace residual_group {

using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t kHeaderBytes = 4;
constexpr std::size_t kMaxPayloadBytes = (std::size_t{1} << 29) - 1;
enum class Kind : std::uint8_t { Zstd3 = 0, Bsc = 1, Zstd10 = 2 };

[[noreturn]] inline void fail(const std::string& message) {
    throw std::runtime_error("residual group: " + message);
}
inline void append_u32le(Bytes& out, std::uint32_t v) {
    out.push_back(std::uint8_t(v)); out.push_back(std::uint8_t(v >> 8));
    out.push_back(std::uint8_t(v >> 16)); out.push_back(std::uint8_t(v >> 24));
}
inline std::uint32_t read_u32le(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
struct DecodedFrame { Kind kind = Kind::Zstd3; std::size_t wire_bytes = 0; Bytes raw; };

class Codec {
public:
    Bytes encode(const std::uint8_t* input, std::size_t size, Kind* selected = nullptr,
                 bool evaluate_zstd10 = true) {
        (void)evaluate_zstd10;
        if (size == 0) { if (selected) *selected = Kind::Zstd3; return {}; }
        Bytes payload(ZSTD_compressBound(size));
        const std::size_t n = ZSTD_compress(payload.data(), payload.size(), input, size, 3);
        if (ZSTD_isError(n)) fail(ZSTD_getErrorName(n));
        payload.resize(n);
        if (payload.size() > kMaxPayloadBytes) fail("payload exceeds packed u32 frame");
        Bytes wire; wire.reserve(kHeaderBytes + payload.size());
        append_u32le(wire, std::uint32_t(payload.size()));
        wire.insert(wire.end(), payload.begin(), payload.end());
        if (selected) *selected = Kind::Zstd3;
        return wire;
    }
    DecodedFrame decode(const std::uint8_t* wire, std::size_t available) {
        if (available < kHeaderBytes) fail("truncated frame header");
        const std::uint32_t header = read_u32le(wire);
        const std::uint8_t kind = std::uint8_t(header >> 29);
        const std::size_t payload_size = header & ((std::uint32_t{1} << 29) - 1);
        if (kind != 0 || payload_size > available - kHeaderBytes)
            fail("invalid residual frame");
        const auto* payload = wire + kHeaderBytes;
        const auto raw_size = ZSTD_getFrameContentSize(payload, payload_size);
        if (raw_size == ZSTD_CONTENTSIZE_ERROR || raw_size == ZSTD_CONTENTSIZE_UNKNOWN ||
            raw_size > SIZE_MAX) fail("zstd frame has no valid decoded size");
        Bytes raw(static_cast<std::size_t>(raw_size));
        const std::size_t n = ZSTD_decompress(raw.data(), raw.size(), payload, payload_size);
        if (ZSTD_isError(n) || n != raw.size()) fail("zstd residual decode failed");
        if (ZSTD_findFrameCompressedSize(payload, payload_size) != payload_size)
            fail("zstd residual has trailing bytes");
        return {Kind::Zstd3, kHeaderBytes + payload_size, std::move(raw)};
    }
};
inline const char* kind_name(Kind kind) {
    return kind == Kind::Zstd3 ? "zstd3" : kind == Kind::Bsc ? "bsc" : "zstd10";
}
}  // namespace residual_group

#endif
