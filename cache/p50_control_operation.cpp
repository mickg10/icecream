#include "p50_control_operation.h"

#include <algorithm>

namespace icecc::p50::local {

std::vector<uint8_t> encode_control_operation(
    const ControlOperation& operation) {
    const bool input = detail::control_kind_has_input(operation.kind);
    const bool cancel = operation.kind == ControlOperationKind::OperationCancel;
    const bool source = operation.kind == ControlOperationKind::SourceTransfer;
    bool invalid = !detail::control_identity_valid(operation.identity) ||
                   operation.request_id == 0 ||
                   !detail::control_kind_valid(operation.kind);
    if (!invalid && operation.kind == ControlOperationKind::CacheSession) {
        invalid = operation.input.has_value() || operation.owner.has_value() ||
                  operation.lifecycle_action != InputLifecycleAction::None ||
                  operation.lifecycle_result.has_value() ||
                  operation.absolute_deadline != sidecar::AbsoluteMonotonicDeadline{};
    }
    if (!invalid && !source)
        invalid = operation.source_patience_ms != 0;
    if (!invalid && input) {
        invalid = !operation.input.has_value() ||
                  operation.input->c_store_guid == CStoreGuid{} ||
                  !operation.owner.has_value() ||
                  !input_lease_owner_valid(*operation.owner);
    }
    if (!invalid && operation.kind == ControlOperationKind::InputFdAttachment) {
        invalid = operation.lifecycle_action != InputLifecycleAction::None ||
                  operation.lifecycle_result.has_value() ||
                  operation.absolute_deadline != sidecar::AbsoluteMonotonicDeadline{};
    }
    if (!invalid && operation.kind == ControlOperationKind::InputLifecycle) {
        // input_lifecycle_action_valid() refuses the reserved retirement
        // actions 4-6, so no extension payload can accompany a live action.
        invalid = !input_lifecycle_action_valid(operation.lifecycle_action) ||
                  (operation.lifecycle_result.has_value() &&
                   !detail::lifecycle_result_valid(*operation.lifecycle_result)) ||
                  operation.absolute_deadline != sidecar::AbsoluteMonotonicDeadline{};
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
    if (!invalid && source) {
        invalid = !operation.source_arm.has_value() ||
                  !operation.source_arm->valid() ||
                  operation.request_id != operation.source_arm->source_request_id ||
                  operation.source_patience_ms == 0 ||
                  operation.source_patience_ms > kMaxSourceTransferPatienceMs ||
                  operation.absolute_deadline != sidecar::AbsoluteMonotonicDeadline{} ||
                  operation.input.has_value() || operation.owner.has_value() ||
                  operation.lifecycle_action != InputLifecycleAction::None ||
                  operation.lifecycle_result.has_value() ||
                  (operation.source_result.has_value() &&
                   (!operation.source_result->valid() ||
                    !detail::source_result_code_valid(operation.source_result->code)));
        if (!invalid && operation.source_result.has_value() &&
            operation.source_result->code == SourceTransferResultCode::Committed &&
            operation.source_result->raw_bytes == 0)
            invalid = true;
    }
    if (invalid)
        return {};

    const size_t size = source ? kSourceTransferOperationBytes
                               : input ? kInputFdAttachmentOperationBytes
                              : cancel ? kOperationCancelOperationBytes
                                       : kCacheSessionOperationBytes;
    std::vector<uint8_t> wire(size, 0);
    detail::control_put_u16(
        wire.data(), operation.kind == ControlOperationKind::CacheSession
                         ? kControlOperationVersionV1
                         : cancel ? kControlOperationVersionV3
                                  : source ? kControlOperationVersionV6
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
    }
    if (source) {
        const P50SourceTransferRequest& arm = *operation.source_arm;
        detail::control_put_u32(wire.data() + 32, arm.wire_job_id);
        detail::control_put_u64(wire.data() + 36, arm.assignment_epoch);
        detail::control_put_u64(wire.data() + 44, arm.assignment_nonce);
        detail::control_put_u16(wire.data() + 52,
                                static_cast<uint16_t>(arm.selected_f_host.size()));
        std::copy(arm.selected_f_host.begin(), arm.selected_f_host.end(),
                  wire.begin() + 56);
        detail::control_put_u32(wire.data() + 312, arm.selected_f_ordinary_port);
        detail::control_put_u32(wire.data() + 316, arm.selected_f_cache_port);
        detail::control_put_u32(wire.data() + 320, arm.cache_protocol);
        detail::control_put_u32(wire.data() + 324, arm.cache_profile);
        detail::control_put_u64(wire.data() + 328, arm.logical_job);
        detail::control_put_u64(wire.data() + 336, arm.compiler_attempt);
        detail::control_put_u64(wire.data() + 376, arm.source_request_id);
        detail::control_put_u32(wire.data() + 384, arm.source_mode);
        // 412..431 stay zero: a peer still sending the old absolute deadline
        // there is refused instead of misread.
        detail::control_put_u32(wire.data() + 408, operation.source_patience_ms);
        if (operation.source_result.has_value()) {
            const P50SourceTransferResult& result = *operation.source_result;
            std::copy(result.c_store_guid.bytes.begin(),
                      result.c_store_guid.bytes.end(), wire.begin() + 432);
            detail::control_put_u16(wire.data() + 448,
                                    static_cast<uint16_t>(result.code));
            detail::control_put_u16(wire.data() + 450, result.error_code);
            wire[452] = result.attempts;
            detail::control_put_u64(wire.data() + 456, result.tu_seq);
            detail::control_put_u64(wire.data() + 464, result.raw_bytes);
            std::copy(result.raw_digest.bytes.begin(), result.raw_digest.bytes.end(),
                      wire.begin() + 472);
        }
    }
    return wire;
}

bool decode_control_operation(std::span<const uint8_t> wire,
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
            : kind == ControlOperationKind::OperationCancel &&
                      version == kControlOperationVersionV3
                  ? kOperationCancelOperationBytes
            : kind == ControlOperationKind::SourceTransfer &&
                      version == kControlOperationVersionV6
                  ? kSourceTransferOperationBytes
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
        // input_lifecycle_action_valid() also refuses the reserved
        // retirement actions 4-6 here, whatever envelope version carries
        // them.
        const bool invalid_kind_payload =
            (kind == ControlOperationKind::InputFdAttachment &&
             (operation.lifecycle_action != InputLifecycleAction::None ||
              operation.lifecycle_result.has_value())) ||
            (kind == ControlOperationKind::InputLifecycle &&
             !input_lifecycle_action_valid(operation.lifecycle_action));
        if (invalid_kind_payload)
            return false;
    }
    if (kind == ControlOperationKind::SourceTransfer) {
        P50SourceTransferRequest arm;
        arm.wire_job_id = detail::control_get_u32(wire.data() + 32);
        arm.assignment_epoch = detail::control_get_u64(wire.data() + 36);
        arm.assignment_nonce = detail::control_get_u64(wire.data() + 44);
        const uint16_t host_size = detail::control_get_u16(wire.data() + 52);
        if (host_size > 255)
            return false;
        if (std::any_of(wire.begin() + 56 + host_size, wire.begin() + 312,
                        [](uint8_t byte) { return byte != 0; }))
            return false;
        arm.selected_f_host.assign(
            reinterpret_cast<const char*>(wire.data() + 56), host_size);
        arm.selected_f_ordinary_port = detail::control_get_u32(wire.data() + 312);
        arm.selected_f_cache_port = detail::control_get_u32(wire.data() + 316);
        arm.cache_protocol = detail::control_get_u32(wire.data() + 320);
        arm.cache_profile = detail::control_get_u32(wire.data() + 324);
        arm.logical_job = detail::control_get_u64(wire.data() + 328);
        arm.compiler_attempt = detail::control_get_u64(wire.data() + 336);
        arm.source_request_id = detail::control_get_u64(wire.data() + 376);
        arm.source_mode = detail::control_get_u32(wire.data() + 384);
        if (std::any_of(wire.begin() + 344, wire.begin() + 376,
                        [](uint8_t byte) { return byte != 0; }) ||
            std::any_of(wire.begin() + 388, wire.begin() + 408,
                        [](uint8_t byte) { return byte != 0; }))
            return false;
        operation.source_arm = std::move(arm);
        operation.source_patience_ms = detail::control_get_u32(wire.data() + 408);
        if (operation.source_patience_ms == 0 ||
            operation.source_patience_ms > kMaxSourceTransferPatienceMs ||
            std::any_of(wire.begin() + 412, wire.begin() + 432,
                        [](uint8_t byte) { return byte != 0; }) ||
            std::any_of(wire.begin() + 453, wire.begin() + 456,
                        [](uint8_t byte) { return byte != 0; }) ||
            std::any_of(wire.begin() + 488, wire.end(),
                        [](uint8_t byte) { return byte != 0; }) ||
            !operation.source_arm->valid() ||
            operation.request_id != operation.source_arm->source_request_id)
            return false;
        const auto result_code = static_cast<SourceTransferResultCode>(
            detail::control_get_u16(wire.data() + 448));
        if (result_code == SourceTransferResultCode::None &&
            std::any_of(wire.begin() + 432, wire.begin() + 488,
                        [](uint8_t byte) { return byte != 0; }))
            return false;
        if (result_code != SourceTransferResultCode::None) {
            P50SourceTransferResult result;
            std::copy(wire.begin() + 432, wire.begin() + 448,
                      result.c_store_guid.bytes.begin());
            result.code = result_code;
            result.error_code = detail::control_get_u16(wire.data() + 450);
            result.attempts = wire[452];
            result.tu_seq = detail::control_get_u64(wire.data() + 456);
            result.raw_bytes = detail::control_get_u64(wire.data() + 464);
            std::copy(wire.begin() + 472, wire.begin() + 488,
                      result.raw_digest.bytes.begin());
            if (!result.valid())
                return false;
            operation.source_result = result;
        }
    }
    return true;
}

}  // namespace icecc::p50::local
