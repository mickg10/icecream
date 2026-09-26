#pragma once

// Exact, versioned operation envelope carried in a local Data frame after
// HELLO/HELLO_ACK.  The envelope is deliberately separate from the generic
// descriptor-handoff wire: SCM_RIGHTS without an operation is ambiguous and
// must never be interpreted as either a cache session or compiler input.

#include "p50_input_lifecycle.h"
#include "p50_local_transport.h"
#include "services/comm.h"

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
inline constexpr uint16_t kControlOperationVersionV6 = 6;
inline constexpr size_t kCacheSessionOperationBytes = 32;
inline constexpr size_t kLegacyInputFdAttachmentOperationBytes = 56;
inline constexpr size_t kInputFdAttachmentOperationBytes = 88;
inline constexpr size_t kInputLifecycleOperationBytes = 88;
inline constexpr size_t kInputAttemptRetirementOperationBytes = 192;
inline constexpr size_t kControlBindingPlaceholderBytes = 32;
inline constexpr size_t kOperationCancelOperationBytes = 72;
// Source transfer carries the bounded daemon assignment request and unchanged
// absolute deadline.  The sidecar fills the authenticated C identity before
// sending P50SourceArm to F.  The fixed envelope keeps operation demux bounded
// while leaving the host field at its protocol maximum.
inline constexpr size_t kSourceTransferOperationBytes = 512;

enum class ControlOperationKind : uint16_t {
    CacheSession = 1,
    InputFdAttachment = 2,
    InputLifecycle = 3,
    OperationCancel = 4,
    SourceTransfer = 5,
};

enum class SourceTransferResultCode : uint16_t {
    None = 0,
    Committed = 1,
    Error = 2,
};

/* Typed sub-errors carried in P50SourceTransferResult::error_code.  Values
   remain inside the existing uint16 wire field; adding this name does not
   change the fixed control-operation layout. */
enum class SourceTransferErrorCode : uint16_t {
    PermanentLocalProfileUnavailable = 0x5001,
    RouteReplacementRequired = 0x5002,
    // F answered BUSY instead of READY: nothing was sent, try another F.
    FSessionCapacity = 0x5003,
};

// The daemon asks the supervised sidecar to transfer one source using the
// selected assignment.  C-side store/control identity is deliberately absent:
// the sidecar supplies its current launch identity when it arms F.
struct P50SourceTransferRequest {
    uint32_t wire_job_id = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;
    std::string selected_f_host;
    uint32_t selected_f_ordinary_port = 0;
    uint32_t selected_f_cache_port = 0;
    uint32_t cache_protocol = 0;
    uint32_t cache_profile = 0;
    uint64_t logical_job = 0;
    uint64_t compiler_attempt = 0;
    uint64_t source_request_id = 0;
    uint32_t source_mode = 0;

    [[nodiscard]] bool valid() const noexcept {
        return wire_job_id != 0 && assignment_epoch != 0 &&
               assignment_nonce != 0 && !selected_f_host.empty() &&
               selected_f_host.size() <= 255 &&
               selected_f_host.find('\0') == std::string::npos &&
               selected_f_ordinary_port != 0 && selected_f_ordinary_port <= UINT16_MAX &&
               selected_f_cache_port != 0 && selected_f_cache_port <= UINT16_MAX &&
               cache_protocol == CACHE_WIRE_REVISION &&
               p50_source_profile_mode_valid(cache_profile, source_mode) &&
               logical_job != 0 && compiler_attempt != 0 && source_request_id != 0;
    }
    auto operator<=>(const P50SourceTransferRequest&) const = default;
};

struct P50SourceTransferResult {
    SourceTransferResultCode code = SourceTransferResultCode::None;
    uint16_t error_code = 0;
    uint8_t attempts = 0;
    uint64_t tu_seq = 0;
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    CStoreGuid c_store_guid{};

    [[nodiscard]] bool valid() const noexcept {
        if (code == SourceTransferResultCode::Committed)
            // TU sequence numbers are zero based: the first committed
            // transfer is deliberately TU0.
            return error_code == 0 && attempts != 0 &&
                   raw_digest != Digest128{} && c_store_guid != CStoreGuid{};
        if (code == SourceTransferResultCode::Error)
            return error_code != 0 && attempts <= 2;
        return false;
    }
    auto operator<=>(const P50SourceTransferResult&) const = default;
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
    std::optional<P50SourceTransferRequest> source_arm;
    std::optional<P50SourceTransferResult> source_result;
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
           kind == ControlOperationKind::OperationCancel ||
           kind == ControlOperationKind::SourceTransfer;
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

inline bool source_result_code_valid(SourceTransferResultCode code) noexcept {
    return code == SourceTransferResultCode::Committed ||
           code == SourceTransferResultCode::Error;
}

}  // namespace detail

std::vector<uint8_t> encode_control_operation(
    const ControlOperation& operation);
bool decode_control_operation(std::span<const uint8_t> wire,
                              ControlOperation& operation) noexcept;

inline ControlOperation make_cache_session_operation(Identity identity,
                                                       uint64_t request_id) noexcept {
    return ControlOperation{ControlOperationKind::CacheSession, identity, request_id,
                             std::nullopt, std::nullopt,
                             InputLifecycleAction::None, std::nullopt,
                             ControlOperationRole::Daemon,
                             ControlCancelTargetRole::FSession,
                             ControlCancellationReason::Requested, {}, 0, {}, 0,
                             {}, 0, std::nullopt, {}, std::nullopt, std::nullopt};
}

inline ControlOperation make_input_fd_attachment_operation(
    Identity identity, InputRecordKey key, InputLeaseOwner owner,
    uint64_t request_id) noexcept {
    return ControlOperation{ControlOperationKind::InputFdAttachment, identity, request_id,
                             key, owner, InputLifecycleAction::None,
                             std::nullopt, ControlOperationRole::Daemon,
                             ControlCancelTargetRole::FSession,
                             ControlCancellationReason::Requested, {}, 0, {}, 0,
                             {}, 0, std::nullopt, {}, std::nullopt, std::nullopt};
}

inline ControlOperation make_input_lifecycle_operation(
    const InputLifecycleRequest& request) noexcept {
    ControlOperation operation{ControlOperationKind::InputLifecycle, request.identity,
                               request.operation_id, request.key, request.owner,
                               request.action, std::nullopt,
                               ControlOperationRole::Daemon,
                               ControlCancelTargetRole::FSession,
                               ControlCancellationReason::Requested, {}, 0, {}, 0,
                               {}, 0, std::nullopt, {}, std::nullopt, std::nullopt};
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
                               {}, 0, std::nullopt, {}, std::nullopt, std::nullopt};
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
                            std::nullopt, {}, std::nullopt, std::nullopt};
}

inline ControlOperation make_source_transfer_operation(
    Identity identity, const P50SourceTransferRequest& arm,
    sidecar::AbsoluteMonotonicDeadline deadline) noexcept {
    ControlOperation operation{ControlOperationKind::SourceTransfer, identity,
                               arm.source_request_id, std::nullopt, std::nullopt,
                               InputLifecycleAction::None, std::nullopt,
                               ControlOperationRole::Daemon,
                               ControlCancelTargetRole::FSession,
                               ControlCancellationReason::Requested, {}, 0, {}, 0,
                               {}, 0, std::nullopt, {}, std::nullopt, std::nullopt};
    operation.absolute_deadline = deadline;
    operation.source_arm = arm;
    return operation;
}

inline ControlOperation make_source_transfer_reply_operation(
    const ControlOperation& request, P50SourceTransferResult result) noexcept {
    ControlOperation operation = request;
    operation.source_result = result;
    operation.sender_role = ControlOperationRole::Sidecar;
    return operation;
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
