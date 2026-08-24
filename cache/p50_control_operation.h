#pragma once

// Exact, versioned operation envelope carried in a local Data frame after
// HELLO/HELLO_ACK.  The envelope is deliberately separate from the generic
// descriptor-handoff wire: SCM_RIGHTS without an operation is ambiguous and
// must never be interpreted as either a cache session or compiler input.

#include "p50_input_lifecycle.h"
#include "p50_local_transport.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace icecc::p50::local {

inline constexpr uint16_t kControlOperationVersionV1 = 1;
inline constexpr uint16_t kControlOperationVersionV2 = 2;
inline constexpr size_t kCacheSessionOperationBytes = 32;
inline constexpr size_t kLegacyInputFdAttachmentOperationBytes = 56;
inline constexpr size_t kInputFdAttachmentOperationBytes = 88;
inline constexpr size_t kInputLifecycleOperationBytes = 88;

enum class ControlOperationKind : uint16_t {
    CacheSession = 1,
    InputFdAttachment = 2,
    InputLifecycle = 3,
};

struct ControlOperation {
    ControlOperationKind kind = ControlOperationKind::CacheSession;
    Identity identity{};
    uint64_t request_id = 0;
    std::optional<icecc::p50::InputRecordKey> input;
    std::optional<icecc::p50::InputLeaseOwner> owner;
    icecc::p50::InputLifecycleAction lifecycle_action =
        icecc::p50::InputLifecycleAction::None;
    std::optional<icecc::p50::InputLifecycleApplyStatus> lifecycle_result;
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
           kind == ControlOperationKind::InputFdAttachment ||
           kind == ControlOperationKind::InputLifecycle;
}

inline bool control_kind_has_input(ControlOperationKind kind) noexcept {
    return kind == ControlOperationKind::InputFdAttachment ||
           kind == ControlOperationKind::InputLifecycle;
}

inline bool lifecycle_result_valid(
    InputLifecycleApplyStatus status) noexcept {
    return status >= InputLifecycleApplyStatus::Applied &&
           status <= InputLifecycleApplyStatus::CapacityExceeded;
}

}  // namespace detail

inline std::vector<uint8_t> encode_control_operation(
    const ControlOperation& operation) {
    if (!detail::control_identity_valid(operation.identity) ||
        operation.request_id == 0 || !detail::control_kind_valid(operation.kind) ||
        (operation.kind == ControlOperationKind::CacheSession &&
         (operation.input.has_value() || operation.owner.has_value() ||
          operation.lifecycle_action != InputLifecycleAction::None ||
          operation.lifecycle_result.has_value())) ||
        (detail::control_kind_has_input(operation.kind) &&
         (!operation.input.has_value() || operation.input->c_store_guid == CStoreGuid{} ||
          !operation.owner.has_value() ||
          !input_lease_owner_valid(*operation.owner))) ||
        (operation.kind == ControlOperationKind::InputFdAttachment &&
         (operation.lifecycle_action != InputLifecycleAction::None ||
          operation.lifecycle_result.has_value())) ||
        (operation.kind == ControlOperationKind::InputLifecycle &&
         (!input_lifecycle_action_valid(operation.lifecycle_action) ||
          (operation.lifecycle_result.has_value() &&
           !detail::lifecycle_result_valid(*operation.lifecycle_result)))))
        return {};

    const bool input = detail::control_kind_has_input(operation.kind);
    const size_t size = input ? kInputFdAttachmentOperationBytes
                              : kCacheSessionOperationBytes;
    std::vector<uint8_t> wire(size, 0);
    detail::control_put_u16(
        wire.data(), operation.kind == ControlOperationKind::CacheSession
                         ? kControlOperationVersionV1
                         : kControlOperationVersionV2);
    detail::control_put_u16(wire.data() + 2, static_cast<uint16_t>(operation.kind));
    detail::control_put_u32(wire.data() + 4, static_cast<uint32_t>(size));
    detail::control_put_u64(wire.data() + 8, operation.identity.generation);
    detail::control_put_u64(wire.data() + 16, operation.identity.attempt);
    detail::control_put_u64(wire.data() + 24, operation.request_id);
    if (input) {
        std::copy(operation.input->c_store_guid.bytes.begin(),
                  operation.input->c_store_guid.bytes.end(), wire.begin() + 32);
        detail::control_put_u64(wire.data() + 48, operation.input->tu_seq.value);
        detail::control_put_u64(wire.data() + 56, operation.owner->logical_job);
        detail::control_put_u64(wire.data() + 64, operation.owner->assignment_epoch);
        detail::control_put_u64(wire.data() + 72, operation.owner->assignment_nonce);
        detail::control_put_u16(wire.data() + 80,
                                static_cast<uint16_t>(operation.lifecycle_action));
        if (operation.lifecycle_result.has_value())
            detail::control_put_u16(
                wire.data() + 82,
                static_cast<uint16_t>(*operation.lifecycle_result) + 1);
    }
    return wire;
}

inline bool decode_control_operation(std::span<const uint8_t> wire,
                                     ControlOperation& operation) noexcept {
    operation = ControlOperation{};
    if (wire.size() < kCacheSessionOperationBytes)
        return false;
    const uint16_t version = detail::control_get_u16(wire.data());
    const auto kind = static_cast<ControlOperationKind>(
        detail::control_get_u16(wire.data() + 2));
    const size_t expected_size =
        kind == ControlOperationKind::CacheSession &&
                version == kControlOperationVersionV1
            ? kCacheSessionOperationBytes
            : kind == ControlOperationKind::InputFdAttachment &&
                      version == kControlOperationVersionV1
                  ? kLegacyInputFdAttachmentOperationBytes
            : kind == ControlOperationKind::InputFdAttachment &&
                      version == kControlOperationVersionV2
                  ? kInputFdAttachmentOperationBytes
            : kind == ControlOperationKind::InputLifecycle &&
                      version == kControlOperationVersionV2
                  ? kInputLifecycleOperationBytes
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
    if (detail::control_kind_has_input(kind)) {
        InputRecordKey key;
        std::copy(wire.begin() + 32, wire.begin() + 48, key.c_store_guid.bytes.begin());
        key.tu_seq.value = detail::control_get_u64(wire.data() + 48);
        if (key.c_store_guid == CStoreGuid{})
            return false;
        operation.input = key;
        if (version == kControlOperationVersionV1) {
            operation.lifecycle_action = InputLifecycleAction::None;
            return kind == ControlOperationKind::InputFdAttachment;
        }
        InputLeaseOwner owner;
        owner.logical_job = detail::control_get_u64(wire.data() + 56);
        owner.assignment_epoch = detail::control_get_u64(wire.data() + 64);
        owner.assignment_nonce = detail::control_get_u64(wire.data() + 72);
        if (!input_lease_owner_valid(owner) ||
            std::any_of(wire.begin() + 84, wire.end(),
                        [](uint8_t byte) { return byte != 0; }))
            return false;
        operation.owner = owner;
        operation.lifecycle_action = static_cast<InputLifecycleAction>(
            detail::control_get_u16(wire.data() + 80));
        const uint16_t encoded_result =
            detail::control_get_u16(wire.data() + 82);
        if (encoded_result != 0) {
            const auto status = static_cast<InputLifecycleApplyStatus>(
                encoded_result - 1);
            if (!detail::lifecycle_result_valid(status))
                return false;
            operation.lifecycle_result = status;
        }
        if ((kind == ControlOperationKind::InputFdAttachment &&
             (operation.lifecycle_action != InputLifecycleAction::None ||
              operation.lifecycle_result.has_value())) ||
            (kind == ControlOperationKind::InputLifecycle &&
             !input_lifecycle_action_valid(operation.lifecycle_action)))
            return false;
    }
    return true;
}

inline ControlOperation make_cache_session_operation(Identity identity,
                                                       uint64_t request_id) noexcept {
    return ControlOperation{ControlOperationKind::CacheSession, identity, request_id,
                             std::nullopt, std::nullopt,
                             InputLifecycleAction::None, std::nullopt};
}

inline ControlOperation make_input_fd_attachment_operation(
    Identity identity, InputRecordKey key, InputLeaseOwner owner,
    uint64_t request_id) noexcept {
    return ControlOperation{ControlOperationKind::InputFdAttachment, identity, request_id,
                             key, owner, InputLifecycleAction::None,
                             std::nullopt};
}

inline ControlOperation make_input_lifecycle_operation(
    const InputLifecycleRequest& request) noexcept {
    return ControlOperation{ControlOperationKind::InputLifecycle, request.identity,
                            request.operation_id, request.key, request.owner,
                            request.action, std::nullopt};
}

inline ControlOperation make_input_lifecycle_reply_operation(
    const InputLifecycleRequest& request,
    InputLifecycleApplyStatus status) noexcept {
    return ControlOperation{ControlOperationKind::InputLifecycle, request.identity,
                            request.operation_id, request.key, request.owner,
                            request.action, status};
}

}  // namespace icecc::p50::local
