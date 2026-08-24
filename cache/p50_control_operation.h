#pragma once

// Exact, versioned operation envelope carried in a local Data frame after
// HELLO/HELLO_ACK.  The envelope is deliberately separate from the generic
// descriptor-handoff wire: SCM_RIGHTS without an operation is ambiguous and
// must never be interpreted as either a cache session or compiler input.

#include "p50_input_record.h"
#include "p50_local_transport.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace icecc::p50::local {

inline constexpr uint16_t kControlOperationVersion = 1;
inline constexpr size_t kCacheSessionOperationBytes = 32;
inline constexpr size_t kInputFdAttachmentOperationBytes = 56;

enum class ControlOperationKind : uint16_t {
    CacheSession = 1,
    InputFdAttachment = 2,
};

struct ControlOperation {
    ControlOperationKind kind = ControlOperationKind::CacheSession;
    Identity identity{};
    uint64_t request_id = 0;
    std::optional<icecc::p50::InputRecordKey> input;
};

namespace detail {

inline void control_put_u16(uint8_t* out, uint16_t value) noexcept {
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
}

inline void control_put_u32(uint8_t* out, uint32_t value) noexcept {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

inline void control_put_u64(uint8_t* out, uint64_t value) noexcept {
    for (size_t i = 0; i != 8; ++i)
        out[i] = static_cast<uint8_t>(value >> (56 - i * 8));
}

inline uint16_t control_get_u16(const uint8_t* in) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) << 8 | in[1]);
}

inline uint32_t control_get_u32(const uint8_t* in) noexcept {
    return static_cast<uint32_t>(in[0]) << 24 | static_cast<uint32_t>(in[1]) << 16 |
           static_cast<uint32_t>(in[2]) << 8 | static_cast<uint32_t>(in[3]);
}

inline uint64_t control_get_u64(const uint8_t* in) noexcept {
    uint64_t value = 0;
    for (size_t i = 0; i != 8; ++i)
        value = (value << 8) | in[i];
    return value;
}

inline bool control_identity_valid(Identity identity) noexcept {
    return identity.generation != 0 && identity.attempt != 0;
}

inline bool control_kind_valid(ControlOperationKind kind) noexcept {
    return kind == ControlOperationKind::CacheSession ||
           kind == ControlOperationKind::InputFdAttachment;
}

}  // namespace detail

inline std::vector<uint8_t> encode_control_operation(
    const ControlOperation& operation) {
    if (!detail::control_identity_valid(operation.identity) ||
        operation.request_id == 0 || !detail::control_kind_valid(operation.kind) ||
        (operation.kind == ControlOperationKind::InputFdAttachment &&
         (!operation.input.has_value() || operation.input->c_store_guid == CStoreGuid{})))
        return {};

    const bool input = operation.kind == ControlOperationKind::InputFdAttachment;
    const size_t size = input ? kInputFdAttachmentOperationBytes
                              : kCacheSessionOperationBytes;
    std::vector<uint8_t> wire(size, 0);
    detail::control_put_u16(wire.data(), kControlOperationVersion);
    detail::control_put_u16(wire.data() + 2, static_cast<uint16_t>(operation.kind));
    detail::control_put_u32(wire.data() + 4, static_cast<uint32_t>(size));
    detail::control_put_u64(wire.data() + 8, operation.identity.generation);
    detail::control_put_u64(wire.data() + 16, operation.identity.attempt);
    detail::control_put_u64(wire.data() + 24, operation.request_id);
    if (input) {
        std::copy(operation.input->c_store_guid.bytes.begin(),
                  operation.input->c_store_guid.bytes.end(), wire.begin() + 32);
        detail::control_put_u64(wire.data() + 48, operation.input->tu_seq.value);
    }
    return wire;
}

inline bool decode_control_operation(std::span<const uint8_t> wire,
                                     ControlOperation& operation) noexcept {
    if (wire.size() < kCacheSessionOperationBytes ||
        detail::control_get_u16(wire.data()) != kControlOperationVersion)
        return false;
    const auto kind = static_cast<ControlOperationKind>(
        detail::control_get_u16(wire.data() + 2));
    const size_t expected_size =
        kind == ControlOperationKind::CacheSession
            ? kCacheSessionOperationBytes
            : kind == ControlOperationKind::InputFdAttachment
                  ? kInputFdAttachmentOperationBytes
                  : 0;
    if (expected_size == 0 || wire.size() != expected_size ||
        detail::control_get_u32(wire.data() + 4) != expected_size)
        return false;

    operation = ControlOperation{};
    operation.kind = kind;
    operation.identity.generation = detail::control_get_u64(wire.data() + 8);
    operation.identity.attempt = detail::control_get_u64(wire.data() + 16);
    operation.request_id = detail::control_get_u64(wire.data() + 24);
    if (!detail::control_identity_valid(operation.identity) || operation.request_id == 0)
        return false;
    if (kind == ControlOperationKind::InputFdAttachment) {
        InputRecordKey key;
        std::copy(wire.begin() + 32, wire.begin() + 48, key.c_store_guid.bytes.begin());
        key.tu_seq.value = detail::control_get_u64(wire.data() + 48);
        if (key.c_store_guid == CStoreGuid{})
            return false;
        operation.input = key;
    }
    return true;
}

inline ControlOperation make_cache_session_operation(Identity identity,
                                                       uint64_t request_id) noexcept {
    return ControlOperation{ControlOperationKind::CacheSession, identity, request_id,
                             std::nullopt};
}

inline ControlOperation make_input_fd_attachment_operation(
    Identity identity, InputRecordKey key, uint64_t request_id) noexcept {
    return ControlOperation{ControlOperationKind::InputFdAttachment, identity, request_id,
                             key};
}

}  // namespace icecc::p50::local
