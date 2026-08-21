/* Common 128-bit content digest wrapper used by Icecream product code. */
#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace icecc {

struct Digest128 {
    std::array<uint8_t, 16> bytes{};

    auto operator<=>(const Digest128&) const = default;
};

class Digest128Builder {
public:
    Digest128Builder();
    ~Digest128Builder();
    Digest128Builder(const Digest128Builder&) = delete;
    Digest128Builder& operator=(const Digest128Builder&) = delete;

    void append(std::span<const uint8_t> bytes);
    void append(std::string_view text);
    void append_u8(uint8_t value);
    void append_u16(uint16_t value);
    void append_u32(uint32_t value);
    void append_u64(uint64_t value);
    void append_digest(const Digest128& value);
    Digest128 finish();

private:
    void* state_ = nullptr;
    bool finished_ = false;
};

Digest128 digest128(std::span<const uint8_t> bytes);
Digest128 digest128(std::string_view text);
std::string digest128_hex(const Digest128& value);

}  // namespace icecc
