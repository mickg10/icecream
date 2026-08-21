#include "digest128.h"

#include <xxhash.h>

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>

namespace icecc {
namespace {

Digest128 canonical_digest(XXH128_hash_t hash) {
    XXH128_canonical_t canonical;
    XXH128_canonicalFromHash(&canonical, hash);
    Digest128 result;
    static_assert(sizeof(canonical.digest) == result.bytes.size());
    std::copy_n(canonical.digest, result.bytes.size(), result.bytes.begin());
    return result;
}

XXH3_state_t* as_xxh3_state(void* state) {
    return static_cast<XXH3_state_t*>(state);
}

}  // namespace

Digest128Builder::Digest128Builder() : state_(XXH3_createState()) {
    if (!state_) throw std::bad_alloc();
    if (XXH3_128bits_reset(as_xxh3_state(state_)) != XXH_OK) {
        XXH3_freeState(as_xxh3_state(state_));
        state_ = nullptr;
        throw std::runtime_error("XXH3-128 state reset failed");
    }
}

Digest128Builder::~Digest128Builder() {
    if (state_) XXH3_freeState(as_xxh3_state(state_));
}

void Digest128Builder::append(std::span<const uint8_t> bytes) {
    if (finished_) throw std::logic_error("cannot append to a finished digest");
    if (!bytes.empty() &&
        XXH3_128bits_update(as_xxh3_state(state_), bytes.data(), bytes.size()) != XXH_OK)
        throw std::runtime_error("XXH3-128 update failed");
}

void Digest128Builder::append(std::string_view text) {
    append(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()),
                                    text.size()));
}

void Digest128Builder::append_u8(uint8_t value) { append({&value, 1}); }

void Digest128Builder::append_u16(uint16_t value) {
    const std::array<uint8_t, 2> bytes{
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    append(bytes);
}

void Digest128Builder::append_u32(uint32_t value) {
    const std::array<uint8_t, 4> bytes{
        static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    append(bytes);
}

void Digest128Builder::append_u64(uint64_t value) {
    const std::array<uint8_t, 8> bytes{
        static_cast<uint8_t>(value >> 56), static_cast<uint8_t>(value >> 48),
        static_cast<uint8_t>(value >> 40), static_cast<uint8_t>(value >> 32),
        static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
    append(bytes);
}

void Digest128Builder::append_digest(const Digest128& value) { append(value.bytes); }

Digest128 Digest128Builder::finish() {
    if (finished_) throw std::logic_error("digest was already finished");
    finished_ = true;
    const Digest128 result = canonical_digest(
        XXH3_128bits_digest(as_xxh3_state(state_)));
    XXH3_freeState(as_xxh3_state(state_));
    state_ = nullptr;
    return result;
}

Digest128 digest128(std::span<const uint8_t> bytes) {
    return canonical_digest(XXH3_128bits(bytes.data(), bytes.size()));
}

Digest128 digest128(std::string_view text) {
    return digest128(std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

std::string digest128_hex(const Digest128& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(value.bytes.size() * 2, '0');
    for (size_t index = 0; index < value.bytes.size(); ++index) {
        result[index * 2] = digits[value.bytes[index] >> 4];
        result[index * 2 + 1] = digits[value.bytes[index] & 0x0f];
    }
    return result;
}

}  // namespace icecc
