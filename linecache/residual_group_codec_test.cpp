#include "residual_group_codec.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Function>
void require_failure(Function function, const char *message) {
    try {
        function();
    } catch (const std::exception &) {
        return;
    }
    throw std::runtime_error(message);
}

}  // namespace

int main() try {
    residual_group::Bytes raw;
    const std::string repeated =
        "template <class T> inline T generated_identifier_123(T value) { return value + 7; }\n";
    for (std::size_t index = 0; index < 20000; ++index) {
        raw.insert(raw.end(), repeated.begin(), repeated.end());
        raw.push_back(std::uint8_t(index));
        raw.push_back(std::uint8_t(index >> 8));
    }

    residual_group::Codec codec;
    residual_group::Kind selected = residual_group::Kind::Zstd3;
    residual_group::Bytes wire = codec.encode(raw.data(), raw.size(), &selected);
    require(wire.size() >= residual_group::kHeaderBytes, "missing frame header");
    const residual_group::DecodedFrame decoded = codec.decode(wire.data(), wire.size());
    require(decoded.wire_bytes == wire.size(), "frame extent differs");
    require(decoded.kind == selected, "selector differs");
    require(decoded.raw == raw, "roundtrip differs");

    residual_group::Kind fast_selected = residual_group::Kind::Zstd10;
    const residual_group::Bytes fast_wire =
        codec.encode(raw.data(), raw.size(), &fast_selected, false);
    const residual_group::DecodedFrame fast_decoded =
        codec.decode(fast_wire.data(), fast_wire.size());
    require(fast_selected != residual_group::Kind::Zstd10,
            "disabled zstd-10 candidate was selected");
    require(fast_decoded.kind == fast_selected && fast_decoded.raw == raw,
            "two-candidate roundtrip differs");

    require_failure(
        [&] { codec.decode(wire.data(), residual_group::kHeaderBytes - 1); },
        "truncated header was accepted");
    require_failure(
        [&] { codec.decode(wire.data(), wire.size() - 1); },
        "truncated payload was accepted");

    residual_group::Bytes trailing = wire;
    trailing.push_back(0);
    const residual_group::DecodedFrame first = codec.decode(trailing.data(), trailing.size());
    require(first.wire_bytes == wire.size(), "frame boundary consumed trailing byte");

    residual_group::Bytes bad_kind = wire;
    bad_kind[3] = std::uint8_t((bad_kind[3] & 0x1f) | 0xe0);
    require_failure(
        [&] { codec.decode(bad_kind.data(), bad_kind.size()); },
        "unknown codec was accepted");

    require(codec.encode(nullptr, 0).empty(), "empty group emitted a frame");
    std::printf("residual group frame exactness/parser PASS codec=%s raw=%zu wire=%zu\n",
                residual_group::kind_name(selected), raw.size(), wire.size());
    return 0;
} catch (const std::exception &error) {
    std::fprintf(stderr, "residual_group_codec_test: %s\n", error.what());
    return 2;
}
