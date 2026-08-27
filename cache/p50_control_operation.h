#pragma once

// Exact, versioned operation envelope carried in a local Data frame after
// HELLO/HELLO_ACK.  The envelope is deliberately separate from the generic
// descriptor-handoff wire: SCM_RIGHTS without an operation is ambiguous and
// must never be interpreted as either a cache session or compiler input.

#include "p50_input_lifecycle.h"
#include "p50_local_transport.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace icecc::p50::local {

inline constexpr uint16_t kControlOperationVersionV1 = 1;
inline constexpr uint16_t kControlOperationVersionV2 = 2;
// v3 is the first frozen OP_CANCEL shape: typed target role, bounded reason,
// and the immutable binding placeholder.  No earlier draft is compatible.
inline constexpr uint16_t kControlOperationVersionV3 = 3;
// v4 carries the allocator-bound F-store and immutable-record identity for
// incremental attempt retirement.  A new shape prevents a v2 peer from
// silently dropping the proof binding.
inline constexpr uint16_t kControlOperationVersionV4 = 4;
// v5 extends the retirement envelope with the unchanged absolute
// CLOCK_MONOTONIC deadline and daemon/sidecar clock-namespace identity.
inline constexpr uint16_t kControlOperationVersionV5 = 5;
inline constexpr size_t kCacheSessionOperationBytes = 32;
inline constexpr size_t kLegacyInputFdAttachmentOperationBytes = 56;
inline constexpr size_t kInputFdAttachmentOperationBytes = 88;
inline constexpr size_t kInputLifecycleOperationBytes = 88;
inline constexpr size_t kInputAttemptRetirementOperationBytes = 192;
inline constexpr size_t kControlBindingPlaceholderBytes = 32;
inline constexpr size_t kOperationCancelOperationBytes = 72;

enum class ControlOperationKind : uint16_t {
    CacheSession = 1,
    InputFdAttachment = 2,
    InputLifecycle = 3,
    OperationCancel = 4,
};

enum class ControlOperationRole : uint16_t {
    Daemon = 1,
    Sidecar = 2,
};

// This is the role being cancelled, not the sender direction above.
enum class ControlCancelTargetRole : uint16_t {
    CSource = 1,
    FSession = 2,
};

enum class ControlCancellationReason : uint16_t {
    Requested = 1,
    Deadline = 2,
    ControlEof = 3,
};

using ControlBindingPlaceholder =
    std::array<uint8_t, kControlBindingPlaceholderBytes>;

struct ControlOperation {
    ControlOperationKind kind = ControlOperationKind::CacheSession;
    Identity identity{};
    uint64_t request_id = 0;
    std::optional<icecc::p50::InputRecordKey> input;
    std::optional<icecc::p50::InputLeaseOwner> owner;
    icecc::p50::InputLifecycleAction lifecycle_action =
        icecc::p50::InputLifecycleAction::None;
    std::optional<icecc::p50::InputLifecycleApplyStatus> lifecycle_result;
    ControlOperationRole sender_role = ControlOperationRole::Daemon;
    ControlCancelTargetRole cancel_target_role = ControlCancelTargetRole::FSession;
    ControlCancellationReason cancellation_reason =
        ControlCancellationReason::Requested;
    ControlBindingPlaceholder binding_placeholder{};
    // These extension fields are nonzero only for v4 retirement operations.
    uint64_t f_store_generation = 0;
    FStoreGuid f_store_guid{};
    uint64_t immutable_size = 0;
    Digest128 immutable_digest{};
    uint64_t retirement_id = 0;
    std::optional<InputLeaseOwner> replacement_owner;
    sidecar::AbsoluteMonotonicDeadline absolute_deadline{};
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
           kind == ControlOperationKind::InputLifecycle ||
           kind == ControlOperationKind::OperationCancel;
}

inline bool control_role_valid(ControlOperationRole role) noexcept {
    return role == ControlOperationRole::Daemon ||
           role == ControlOperationRole::Sidecar;
}

inline bool control_cancel_target_role_valid(ControlCancelTargetRole role) noexcept {
    return role == ControlCancelTargetRole::CSource ||
           role == ControlCancelTargetRole::FSession;
}

inline bool control_cancellation_reason_valid(ControlCancellationReason reason) noexcept {
    return reason == ControlCancellationReason::Requested ||
           reason == ControlCancellationReason::Deadline ||
           reason == ControlCancellationReason::ControlEof;
}

inline bool control_kind_has_input(ControlOperationKind kind) noexcept {
    return kind == ControlOperationKind::InputFdAttachment ||
           kind == ControlOperationKind::InputLifecycle;
}

inline bool lifecycle_result_valid(
    InputLifecycleApplyStatus status) noexcept {
    return status >= InputLifecycleApplyStatus::Applied &&
           status <= InputLifecycleApplyStatus::GenerationMismatch;
}

inline bool retirement_action(InputLifecycleAction action) noexcept {
    return action == InputLifecycleAction::PrepareAttemptRetirement ||
           action == InputLifecycleAction::CommitAttemptReplacement ||
           action == InputLifecycleAction::CloseLogicalInputLease;
}

}  // namespace detail

inline std::vector<uint8_t> encode_control_operation(
    const ControlOperation& operation) {
    const bool input = detail::control_kind_has_input(operation.kind);
    const bool cancel = operation.kind == ControlOperationKind::OperationCancel;
    const bool retirement = operation.kind == ControlOperationKind::InputLifecycle &&
                            detail::retirement_action(operation.lifecycle_action);
    bool invalid = !detail::control_identity_valid(operation.identity) ||
                   operation.request_id == 0 ||
                   !detail::control_kind_valid(operation.kind);
    if (!invalid && operation.kind == ControlOperationKind::CacheSession) {
        invalid = operation.input.has_value() || operation.owner.has_value() ||
                  operation.lifecycle_action != InputLifecycleAction::None ||
                  operation.lifecycle_result.has_value() ||
                  operation.f_store_generation != 0 ||
                  operation.f_store_guid != FStoreGuid{} ||
                  operation.immutable_size != 0 ||
                  operation.immutable_digest != Digest128{} ||
                  operation.retirement_id != 0 ||
                  operation.replacement_owner.has_value() ||
                  operation.absolute_deadline != sidecar::AbsoluteMonotonicDeadline{};
    }
    if (!invalid && input) {
        invalid = !operation.input.has_value() ||
                  operation.input->c_store_guid == CStoreGuid{} ||
                  !operation.owner.has_value() ||
                  !input_lease_owner_valid(*operation.owner);
    }
    if (!invalid && operation.kind == ControlOperationKind::InputFdAttachment) {
        invalid = operation.lifecycle_action != InputLifecycleAction::None ||
                  operation.lifecycle_result.has_value() ||
                  operation.f_store_generation != 0 ||
                  operation.f_store_guid != FStoreGuid{} ||
                  operation.immutable_size != 0 ||
                  operation.immutable_digest != Digest128{} ||
                  operation.retirement_id != 0 ||
                  operation.replacement_owner.has_value() ||
                  operation.absolute_deadline != sidecar::AbsoluteMonotonicDeadline{};
    }
    if (!invalid && operation.kind == ControlOperationKind::InputLifecycle) {
        invalid = !input_lifecycle_action_valid(operation.lifecycle_action) ||
                  (operation.lifecycle_result.has_value() &&
                   !detail::lifecycle_result_valid(*operation.lifecycle_result));
        if (!invalid && !retirement) {
            invalid = operation.f_store_generation != 0 ||
                      operation.f_store_guid != FStoreGuid{} ||
                      operation.immutable_size != 0 ||
                      operation.immutable_digest != Digest128{} ||
                      operation.retirement_id != 0 ||
                      operation.replacement_owner.has_value() ||
                      operation.absolute_deadline != sidecar::AbsoluteMonotonicDeadline{};
        }
        if (!invalid && retirement) {
            invalid = operation.f_store_generation == 0 ||
                      operation.f_store_guid == FStoreGuid{} ||
                      operation.immutable_digest == Digest128{} ||
                      operation.retirement_id == 0 ||
                      !operation.absolute_deadline.valid() ||
                      (operation.lifecycle_action ==
                           InputLifecycleAction::CommitAttemptReplacement &&
                       (!operation.replacement_owner.has_value() ||
                        !input_lease_owner_valid(*operation.replacement_owner))) ||
                      (operation.lifecycle_action !=
                           InputLifecycleAction::CommitAttemptReplacement &&
                       operation.replacement_owner.has_value());
        }
    }
    if (!invalid && cancel) {
        invalid = !detail::control_cancel_target_role_valid(operation.cancel_target_role) ||
                  !detail::control_cancellation_reason_valid(operation.cancellation_reason) ||
                  !detail::control_role_valid(operation.sender_role) ||
                  operation.input.has_value() || operation.owner.has_value() ||
                  operation.lifecycle_action != InputLifecycleAction::None ||
                  operation.lifecycle_result.has_value() ||
                  operation.absolute_deadline != sidecar::AbsoluteMonotonicDeadline{};
    }
    if (invalid)
        return {};

    const size_t size = retirement ? kInputAttemptRetirementOperationBytes
                                   : input ? kInputFdAttachmentOperationBytes
                              : cancel ? kOperationCancelOperationBytes
                                       : kCacheSessionOperationBytes;
    std::vector<uint8_t> wire(size, 0);
    detail::control_put_u16(
        wire.data(), operation.kind == ControlOperationKind::CacheSession
                         ? kControlOperationVersionV1
                         : cancel ? kControlOperationVersionV3
                                  : retirement ? kControlOperationVersionV5
                                                : kControlOperationVersionV2);
    detail::control_put_u16(wire.data() + 2, static_cast<uint16_t>(operation.kind));
    detail::control_put_u32(wire.data() + 4, static_cast<uint32_t>(size));
    detail::control_put_u64(wire.data() + 8, operation.identity.generation);
    detail::control_put_u64(wire.data() + 16, operation.identity.attempt);
    detail::control_put_u64(wire.data() + 24, operation.request_id);
    if (cancel) {
        detail::control_put_u16(wire.data() + 32,
                                static_cast<uint16_t>(operation.cancel_target_role));
        detail::control_put_u16(
            wire.data() + 34,
            static_cast<uint16_t>(operation.cancellation_reason));
        detail::control_put_u16(wire.data() + 36,
                                static_cast<uint16_t>(operation.sender_role));
        std::copy(operation.binding_placeholder.begin(),
                  operation.binding_placeholder.end(), wire.begin() + 40);
    }
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
        if (retirement) {
            detail::control_put_u64(wire.data() + 88,
                                    operation.f_store_generation);
            std::copy(operation.f_store_guid.bytes.begin(),
                      operation.f_store_guid.bytes.end(), wire.begin() + 96);
            detail::control_put_u64(wire.data() + 112, operation.immutable_size);
            std::copy(operation.immutable_digest.bytes.begin(),
                      operation.immutable_digest.bytes.end(), wire.begin() + 120);
            detail::control_put_u64(wire.data() + 136, operation.retirement_id);
            if (operation.replacement_owner.has_value()) {
                detail::control_put_u64(wire.data() + 144,
                                        operation.replacement_owner->logical_job);
                detail::control_put_u64(wire.data() + 152,
                                        operation.replacement_owner->assignment_epoch);
                detail::control_put_u64(wire.data() + 160,
                                        operation.replacement_owner->assignment_nonce);
            }
            detail::control_put_u64(
                wire.data() + 168,
                static_cast<uint64_t>(operation.absolute_deadline.expires_at_ns));
            detail::control_put_u64(wire.data() + 176,
                                    operation.absolute_deadline.clock_domain_id);
            detail::control_put_u64(wire.data() + 184,
                                    operation.absolute_deadline.time_namespace_id);
        }
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
                      version == kControlOperationVersionV5
                  ? kInputAttemptRetirementOperationBytes
            : kind == ControlOperationKind::InputLifecycle &&
                      version == kControlOperationVersionV2
                  ? kInputLifecycleOperationBytes
            : kind == ControlOperationKind::OperationCancel &&
                      version == kControlOperationVersionV3
                  ? kOperationCancelOperationBytes
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
    if (kind == ControlOperationKind::OperationCancel) {
        operation.cancel_target_role = static_cast<ControlCancelTargetRole>(
            detail::control_get_u16(wire.data() + 32));
        operation.cancellation_reason = static_cast<ControlCancellationReason>(
            detail::control_get_u16(wire.data() + 34));
        operation.sender_role = static_cast<ControlOperationRole>(
            detail::control_get_u16(wire.data() + 36));
        if (!detail::control_cancel_target_role_valid(operation.cancel_target_role) ||
            !detail::control_cancellation_reason_valid(operation.cancellation_reason) ||
            !detail::control_role_valid(operation.sender_role) ||
            std::any_of(wire.begin() + 38, wire.begin() + 40,
                        [](uint8_t byte) { return byte != 0; }))
            return false;
        std::copy(wire.begin() + 40, wire.end(),
                  operation.binding_placeholder.begin());
        return true;
    }
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
            std::any_of(wire.begin() + 84, wire.begin() + 88,
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
        const bool invalid_kind_payload =
            (kind == ControlOperationKind::InputFdAttachment &&
             (operation.lifecycle_action != InputLifecycleAction::None ||
              operation.lifecycle_result.has_value())) ||
            (kind == ControlOperationKind::InputLifecycle &&
             (!input_lifecycle_action_valid(operation.lifecycle_action) ||
              (version == kControlOperationVersionV2 &&
               detail::retirement_action(operation.lifecycle_action))));
        if (invalid_kind_payload)
            return false;
        if (kind == ControlOperationKind::InputLifecycle &&
            version == kControlOperationVersionV5) {
            if (!detail::retirement_action(operation.lifecycle_action))
                return false;
            operation.f_store_generation = detail::control_get_u64(wire.data() + 88);
            std::copy(wire.begin() + 96, wire.begin() + 112,
                      operation.f_store_guid.bytes.begin());
            operation.immutable_size = detail::control_get_u64(wire.data() + 112);
            std::copy(wire.begin() + 120, wire.begin() + 136,
                      operation.immutable_digest.bytes.begin());
            operation.retirement_id = detail::control_get_u64(wire.data() + 136);
            if (operation.f_store_generation == 0 ||
                operation.f_store_guid == FStoreGuid{} ||
                operation.immutable_digest == Digest128{} ||
                operation.retirement_id == 0)
                return false;
            InputLeaseOwner replacement;
            replacement.logical_job = detail::control_get_u64(wire.data() + 144);
            replacement.assignment_epoch = detail::control_get_u64(wire.data() + 152);
            replacement.assignment_nonce = detail::control_get_u64(wire.data() + 160);
            const bool has_replacement = replacement.logical_job != 0 ||
                                         replacement.assignment_epoch != 0 ||
                                         replacement.assignment_nonce != 0;
            if (operation.lifecycle_action == InputLifecycleAction::CommitAttemptReplacement) {
                if (!has_replacement || !input_lease_owner_valid(replacement))
                    return false;
                operation.replacement_owner = replacement;
            } else if (has_replacement) {
                return false;
            }
            operation.absolute_deadline.expires_at_ns =
                static_cast<int64_t>(detail::control_get_u64(wire.data() + 168));
            operation.absolute_deadline.clock_domain_id =
                detail::control_get_u64(wire.data() + 176);
            operation.absolute_deadline.time_namespace_id =
                detail::control_get_u64(wire.data() + 184);
            if (!operation.absolute_deadline.valid())
                return false;
        }
    }
    return true;
}

inline ControlOperation make_cache_session_operation(Identity identity,
                                                       uint64_t request_id) noexcept {
    return ControlOperation{ControlOperationKind::CacheSession, identity, request_id,
                             std::nullopt, std::nullopt,
                             InputLifecycleAction::None, std::nullopt,
                             ControlOperationRole::Daemon,
                             ControlCancelTargetRole::FSession,
                             ControlCancellationReason::Requested, {}, 0, {}, 0,
                             {}, 0, std::nullopt};
}

inline ControlOperation make_input_fd_attachment_operation(
    Identity identity, InputRecordKey key, InputLeaseOwner owner,
    uint64_t request_id) noexcept {
    return ControlOperation{ControlOperationKind::InputFdAttachment, identity, request_id,
                             key, owner, InputLifecycleAction::None,
                             std::nullopt, ControlOperationRole::Daemon,
                             ControlCancelTargetRole::FSession,
                             ControlCancellationReason::Requested, {}, 0, {}, 0,
                             {}, 0, std::nullopt};
}

inline ControlOperation make_input_lifecycle_operation(
    const InputLifecycleRequest& request) noexcept {
    ControlOperation operation{ControlOperationKind::InputLifecycle, request.identity,
                               request.operation_id, request.key, request.owner,
                               request.action, std::nullopt,
                               ControlOperationRole::Daemon,
                               ControlCancelTargetRole::FSession,
                               ControlCancellationReason::Requested, {}, 0, {}, 0,
                               {}, 0, std::nullopt};
    operation.f_store_generation = request.f_store_generation;
    operation.f_store_guid = request.f_store_guid;
    operation.immutable_size = request.immutable_size;
    operation.immutable_digest = request.immutable_digest;
    operation.retirement_id = request.retirement_id;
    operation.replacement_owner = request.replacement_owner;
    operation.absolute_deadline = request.absolute_deadline;
    return operation;
}

inline ControlOperation make_input_lifecycle_reply_operation(
    const InputLifecycleRequest& request,
    InputLifecycleApplyStatus status) noexcept {
    ControlOperation operation{ControlOperationKind::InputLifecycle, request.identity,
                               request.operation_id, request.key, request.owner,
                               request.action, status, ControlOperationRole::Sidecar,
                               ControlCancelTargetRole::FSession,
                               ControlCancellationReason::Requested, {}, 0, {}, 0,
                               {}, 0, std::nullopt};
    operation.f_store_generation = request.f_store_generation;
    operation.f_store_guid = request.f_store_guid;
    operation.immutable_size = request.immutable_size;
    operation.immutable_digest = request.immutable_digest;
    operation.retirement_id = request.retirement_id;
    operation.replacement_owner = request.replacement_owner;
    operation.absolute_deadline = request.absolute_deadline;
    return operation;
}

inline ControlOperation make_operation_cancel_operation(
    Identity identity, ControlCancelTargetRole target_role, uint64_t request_id,
    ControlCancellationReason reason = ControlCancellationReason::Requested,
    ControlBindingPlaceholder binding_placeholder = {},
    ControlOperationRole sender_role = ControlOperationRole::Daemon) noexcept {
    // This lane freezes the sidecar decoder/validator only.  Daemon OP_CANCEL
    // emission and replacement of this bounded placeholder with the exact
    // session claim remain an explicit integration HOLD.
    return ControlOperation{ControlOperationKind::OperationCancel, identity, request_id,
                            std::nullopt, std::nullopt, InputLifecycleAction::None,
                            std::nullopt, sender_role, target_role,
                            reason, binding_placeholder, 0, {}, 0, {}, 0,
                            std::nullopt};
}

inline ControlOperation make_cancel_operation(
    Identity identity, ControlCancelTargetRole target_role, uint64_t request_id,
    ControlCancellationReason reason = ControlCancellationReason::Requested,
    ControlBindingPlaceholder binding_placeholder = {},
    ControlOperationRole sender_role = ControlOperationRole::Daemon) noexcept {
    return make_operation_cancel_operation(identity, target_role, request_id, reason,
                                            binding_placeholder, sender_role);
}

}  // namespace icecc::p50::local
