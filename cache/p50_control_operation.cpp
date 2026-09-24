#include "p50_control_operation.h"

#include <algorithm>

namespace icecc::p50::local {

namespace {

constexpr size_t kP51ArmBytes = 376;
constexpr size_t kP51ReplyOffset = 456;
constexpr size_t kP51ReplyBytes = 144;
constexpr size_t kP51TransferResultOffset = 600;
constexpr size_t kP51TransferResultBytes = 56;

void encode_p51_arm(uint8_t* out, const P51SourceArmFields& arm) {
    const P50SourceArmFields& source = arm.source;
    detail::control_put_u32(out, source.wire_job_id);
    detail::control_put_u64(out + 4, source.assignment_epoch);
    detail::control_put_u64(out + 12, source.assignment_nonce);
    detail::control_put_u16(out + 20,
                            static_cast<uint16_t>(source.selected_f_host.size()));
    std::copy(source.selected_f_host.begin(), source.selected_f_host.end(), out + 22);
    detail::control_put_u32(out + 280, source.selected_f_ordinary_port);
    detail::control_put_u32(out + 284, source.selected_f_cache_port);
    detail::control_put_u32(out + 288, source.cache_protocol);
    detail::control_put_u32(out + 292, source.cache_profile);
    detail::control_put_u64(out + 296, source.logical_job);
    detail::control_put_u64(out + 304, source.compiler_attempt);
    detail::control_put_u64(out + 312, source.c_store_generation);
    detail::control_put_u64(out + 320, source.c_store_derivation_version);
    std::copy(source.c_store_guid.begin(), source.c_store_guid.end(), out + 328);
    detail::control_put_u64(out + 344, source.source_request_id);
    detail::control_put_u32(out + 352, source.source_mode);
    detail::control_put_u64(out + 356, source.c_control_generation);
    detail::control_put_u64(out + 364, source.c_control_attempt);
    detail::control_put_u32(out + 372, arm.requested_window);
}

bool decode_p51_arm(const uint8_t* in, P51SourceArmFields& arm) {
    arm = {};
    P50SourceArmFields& source = arm.source;
    source.wire_job_id = detail::control_get_u32(in);
    source.assignment_epoch = detail::control_get_u64(in + 4);
    source.assignment_nonce = detail::control_get_u64(in + 12);
    const uint16_t host_size = detail::control_get_u16(in + 20);
    if (host_size == 0 || host_size > 255 ||
        std::any_of(in + 22 + host_size, in + 277,
                    [](uint8_t byte) { return byte != 0; }) ||
        std::any_of(in + 277, in + 280,
                    [](uint8_t byte) { return byte != 0; }))
        return false;
    source.selected_f_host.assign(reinterpret_cast<const char*>(in + 22), host_size);
    source.selected_f_ordinary_port = detail::control_get_u32(in + 280);
    source.selected_f_cache_port = detail::control_get_u32(in + 284);
    source.cache_protocol = detail::control_get_u32(in + 288);
    source.cache_profile = detail::control_get_u32(in + 292);
    source.logical_job = detail::control_get_u64(in + 296);
    source.compiler_attempt = detail::control_get_u64(in + 304);
    source.c_store_generation = detail::control_get_u64(in + 312);
    source.c_store_derivation_version = detail::control_get_u64(in + 320);
    std::copy(in + 328, in + 344, source.c_store_guid.begin());
    source.source_request_id = detail::control_get_u64(in + 344);
    source.source_mode = detail::control_get_u32(in + 352);
    source.c_control_generation = detail::control_get_u64(in + 356);
    source.c_control_attempt = detail::control_get_u64(in + 364);
    arm.requested_window = detail::control_get_u32(in + 372);
    return std::all_of(in + 376, in + kP51ArmBytes,
                       [](uint8_t byte) { return byte == 0; }) &&
           arm.valid();
}

void encode_p51_armed(uint8_t* out, const P51SourceArmedFields& armed) {
    detail::control_put_u64(out, armed.f_control_generation);
    detail::control_put_u64(out + 8, armed.f_control_attempt);
    detail::control_put_u64(out + 16, armed.f_store_generation);
    std::copy(armed.f_store_guid.begin(), armed.f_store_guid.end(), out + 24);
    detail::control_put_u64(out + 40, armed.f_store_derivation_version);
    detail::control_put_u64(out + 48, armed.arm_observation_id);
    detail::control_put_u32(out + 56, armed.source_budget_msec);
    std::copy(armed.attempt_capability_1.bytes.begin(),
              armed.attempt_capability_1.bytes.end(), out + 60);
    std::copy(armed.attempt_capability_2.bytes.begin(),
              armed.attempt_capability_2.bytes.end(), out + 76);
    std::copy(armed.reservation_id.begin(), armed.reservation_id.end(), out + 92);
    std::copy(armed.logical_relationship_id.begin(),
              armed.logical_relationship_id.end(), out + 108);
    detail::control_put_u64(out + 124, armed.relationship_epoch);
    detail::control_put_u32(out + 132, armed.selected_revision);
    detail::control_put_u32(out + 136, armed.selected_window);
}

bool decode_p51_armed(const uint8_t* in, P51SourceArmedFields& armed) {
    armed.f_control_generation = detail::control_get_u64(in);
    armed.f_control_attempt = detail::control_get_u64(in + 8);
    armed.f_store_generation = detail::control_get_u64(in + 16);
    std::copy(in + 24, in + 40, armed.f_store_guid.begin());
    armed.f_store_derivation_version = detail::control_get_u64(in + 40);
    armed.arm_observation_id = detail::control_get_u64(in + 48);
    armed.source_budget_msec = detail::control_get_u32(in + 56);
    std::copy(in + 60, in + 76, armed.attempt_capability_1.bytes.begin());
    std::copy(in + 76, in + 92, armed.attempt_capability_2.bytes.begin());
    std::copy(in + 92, in + 108, armed.reservation_id.begin());
    std::copy(in + 108, in + 124, armed.logical_relationship_id.begin());
    armed.relationship_epoch = detail::control_get_u64(in + 124);
    armed.selected_revision = detail::control_get_u32(in + 132);
    armed.selected_window = detail::control_get_u32(in + 136);
    return std::all_of(in + 140, in + kP51ReplyBytes,
                       [](uint8_t byte) { return byte == 0; });
}

} // namespace

std::vector<uint8_t> encode_control_operation(
    const ControlOperation& operation) {
    const bool input = detail::control_kind_has_input(operation.kind);
    const bool cancel = operation.kind == ControlOperationKind::OperationCancel;
    const bool source = operation.kind == ControlOperationKind::SourceTransfer;
    const bool p51_reservation =
        operation.kind == ControlOperationKind::SourceReservation;
    const bool p51_reservation_cancel =
        operation.kind == ControlOperationKind::SourceReservationCancel;
    const bool p51_transfer =
        operation.kind == ControlOperationKind::P51SourceTransfer;
    const bool retirement = operation.kind == ControlOperationKind::InputLifecycle &&
                            detail::retirement_action(operation.lifecycle_action);
    bool invalid = !detail::control_identity_valid(operation.identity) ||
                   operation.request_id == 0 ||
                   !detail::control_kind_valid(operation.kind);
    if (!invalid &&
        (operation.kind == ControlOperationKind::CacheSession ||
         operation.kind == ControlOperationKind::CacheLinkSession)) {
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
    if (!invalid && source) {
        invalid = !operation.source_arm.has_value() ||
                  !operation.source_arm->valid() ||
                  operation.request_id != operation.source_arm->source_request_id ||
                  !operation.absolute_deadline.valid() ||
                  operation.input.has_value() || operation.owner.has_value() ||
                  operation.lifecycle_action != InputLifecycleAction::None ||
                  operation.lifecycle_result.has_value() ||
                  operation.f_store_generation != 0 ||
                  operation.f_store_guid != FStoreGuid{} ||
                  operation.immutable_size != 0 ||
                  operation.immutable_digest != Digest128{} ||
                  operation.retirement_id != 0 ||
                  operation.replacement_owner.has_value() ||
                  (operation.source_result.has_value() &&
                   (!operation.source_result->valid() ||
                    !detail::source_result_code_valid(operation.source_result->code)));
        if (!invalid && operation.source_result.has_value() &&
            operation.source_result->code == SourceTransferResultCode::Committed &&
            operation.source_result->raw_bytes == 0)
            invalid = true;
    }
    if (!invalid && p51_reservation) {
        invalid = !operation.p51_reservation.has_value() ||
                  !operation.p51_reservation->arm.valid() ||
                  !operation.p51_reservation->absolute_deadline.valid() ||
                  operation.request_id !=
                      operation.p51_reservation->arm.source.source_request_id ||
                  operation.source_arm.has_value() ||
                  operation.source_result.has_value();
        if (!invalid && operation.p51_reservation_result.has_value())
            invalid = !operation.p51_reservation_result->valid() ||
                      (operation.p51_reservation_result->armed.has_value() &&
                       operation.p51_reservation_result->armed->arm !=
                           operation.p51_reservation->arm);
    }
    if (!invalid && p51_reservation_cancel) {
        invalid = !operation.p51_reservation_cancel.has_value() ||
                  !operation.p51_reservation_cancel->arm.valid() ||
                  !operation.p51_reservation_cancel->armed.valid() ||
                  operation.p51_reservation_cancel->armed.arm !=
                      operation.p51_reservation_cancel->arm ||
                  !operation.p51_reservation_cancel->absolute_deadline.valid() ||
                  operation.request_id != operation.p51_reservation_cancel->arm.source.source_request_id ||
                  operation.p51_reservation.has_value() ||
                  operation.p51_reservation_result.has_value() ||
                  (operation.p51_reservation_cancel_result !=
                       operation.p51_reservation_cancel->cancelled);
    }
    if (!invalid && p51_transfer) {
        invalid = !operation.p51_source_transfer.has_value() ||
                  !operation.p51_source_transfer->armed.valid() ||
                  !operation.p51_source_transfer->absolute_deadline.valid() ||
                  operation.absolute_deadline !=
                      operation.p51_source_transfer->absolute_deadline ||
                  operation.request_id != operation.p51_source_transfer->armed
                                              .arm.source.source_request_id ||
                  operation.source_arm.has_value() ||
                  operation.source_result.has_value() ||
                  operation.p51_reservation.has_value() ||
                  operation.p51_reservation_cancel.has_value();
        if (!invalid && operation.p51_source_transfer_result.has_value())
            invalid = !operation.p51_source_transfer_result->valid();
    }
    if (invalid)
        return {};

    const size_t size = (p51_reservation || p51_reservation_cancel || p51_transfer)
                               ? kP51SourceReservationOperationBytes
                               : source ? kSourceTransferOperationBytes
                               : retirement ? kInputAttemptRetirementOperationBytes
                                   : input ? kInputFdAttachmentOperationBytes
                              : cancel ? kOperationCancelOperationBytes
                                       : kCacheSessionOperationBytes;
    std::vector<uint8_t> wire(size, 0);
    detail::control_put_u16(
        wire.data(), operation.kind == ControlOperationKind::CacheSession
                         ? kControlOperationVersionV1
                         : operation.kind == ControlOperationKind::CacheLinkSession
                             ? kControlOperationVersionV2
                         : cancel ? kControlOperationVersionV3
                         : (p51_reservation || p51_reservation_cancel || p51_transfer)
                               ? kControlOperationVersionV7
                         : source ? kControlOperationVersionV6
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
        detail::control_put_u64(
            wire.data() + 408,
            static_cast<uint64_t>(operation.absolute_deadline.expires_at_ns));
        detail::control_put_u64(wire.data() + 416,
                                operation.absolute_deadline.clock_domain_id);
        detail::control_put_u64(wire.data() + 424,
                                operation.absolute_deadline.time_namespace_id);
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
    if (p51_reservation) {
        const P51SourceReservationRequest& request = *operation.p51_reservation;
        const uint16_t phase = !operation.p51_reservation_result.has_value()
                                   ? 0
                               : operation.p51_reservation_result->armed.has_value()
                                   ? 1
                                   : 2;
        detail::control_put_u16(wire.data() + 32, phase);
        if (phase == 2)
            detail::control_put_u16(
                wire.data() + 34,
                operation.p51_reservation_result->error_code);
        detail::control_put_u64(
            wire.data() + 40,
            static_cast<uint64_t>(request.absolute_deadline.expires_at_ns));
        detail::control_put_u64(wire.data() + 48,
                                request.absolute_deadline.clock_domain_id);
        detail::control_put_u64(wire.data() + 56,
                                request.absolute_deadline.time_namespace_id);
        encode_p51_arm(wire.data() + 64, request.arm);
        if (phase == 1)
            encode_p51_armed(wire.data() + kP51ReplyOffset,
                             *operation.p51_reservation_result->armed);
    }
    if (p51_reservation_cancel) {
        const P51SourceReservationCancel& cancel =
            *operation.p51_reservation_cancel;
        const uint16_t phase = !operation.p51_reservation_cancel_result.has_value()
                                   ? 0
                               : *operation.p51_reservation_cancel_result ? 1 : 2;
        detail::control_put_u16(wire.data() + 32, phase);
        detail::control_put_u64(
            wire.data() + 40,
            static_cast<uint64_t>(cancel.absolute_deadline.expires_at_ns));
        detail::control_put_u64(wire.data() + 48,
                                cancel.absolute_deadline.clock_domain_id);
        detail::control_put_u64(wire.data() + 56,
                                cancel.absolute_deadline.time_namespace_id);
        encode_p51_arm(wire.data() + 64,
                       P51SourceArmFields{cancel.arm.source,
                                          cancel.armed.arm.requested_window});
        encode_p51_armed(wire.data() + kP51ReplyOffset, cancel.armed);
    }
    if (p51_transfer) {
        const P51SourceTransferRequest& request =
            *operation.p51_source_transfer;
        detail::control_put_u16(
            wire.data() + 32,
            operation.p51_source_transfer_result.has_value() ? 1 : 0);
        detail::control_put_u64(
            wire.data() + 40,
            static_cast<uint64_t>(request.absolute_deadline.expires_at_ns));
        detail::control_put_u64(wire.data() + 48,
                                request.absolute_deadline.clock_domain_id);
        detail::control_put_u64(wire.data() + 56,
                                request.absolute_deadline.time_namespace_id);
        encode_p51_arm(wire.data() + 64, request.armed.arm);
        encode_p51_armed(wire.data() + kP51ReplyOffset, request.armed);
        if (operation.p51_source_transfer_result.has_value()) {
            const P50SourceTransferResult& result =
                *operation.p51_source_transfer_result;
            std::copy(result.c_store_guid.bytes.begin(),
                      result.c_store_guid.bytes.end(),
                      wire.begin() + kP51TransferResultOffset);
            detail::control_put_u16(
                wire.data() + kP51TransferResultOffset + 16,
                static_cast<uint16_t>(result.code));
            detail::control_put_u16(
                wire.data() + kP51TransferResultOffset + 18,
                result.error_code);
            wire[kP51TransferResultOffset + 20] = result.attempts;
            detail::control_put_u64(
                wire.data() + kP51TransferResultOffset + 24, result.tu_seq);
            detail::control_put_u64(
                wire.data() + kP51TransferResultOffset + 32, result.raw_bytes);
            std::copy(result.raw_digest.bytes.begin(),
                      result.raw_digest.bytes.end(),
                      wire.begin() + kP51TransferResultOffset + 40);
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
            : kind == ControlOperationKind::CacheLinkSession &&
                      version == kControlOperationVersionV2
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
            : kind == ControlOperationKind::SourceTransfer &&
                      version == kControlOperationVersionV6
                  ? kSourceTransferOperationBytes
            : kind == ControlOperationKind::SourceReservation &&
                      version == kControlOperationVersionV7
                  ? kP51SourceReservationOperationBytes
            : kind == ControlOperationKind::SourceReservationCancel &&
                      version == kControlOperationVersionV7
                  ? kP51SourceReservationOperationBytes
            : kind == ControlOperationKind::P51SourceTransfer &&
                      version == kControlOperationVersionV7
                  ? kP51SourceReservationOperationBytes
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
        operation.absolute_deadline.expires_at_ns =
            static_cast<int64_t>(detail::control_get_u64(wire.data() + 408));
        operation.absolute_deadline.clock_domain_id =
            detail::control_get_u64(wire.data() + 416);
        operation.absolute_deadline.time_namespace_id =
            detail::control_get_u64(wire.data() + 424);
        if (!operation.absolute_deadline.valid() ||
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
    if (kind == ControlOperationKind::SourceReservation) {
        const uint16_t phase = detail::control_get_u16(wire.data() + 32);
        const uint16_t error = detail::control_get_u16(wire.data() + 34);
        if (phase > 2 || (phase == 0 && error != 0) ||
            (phase == 1 && error != 0) ||
            (phase == 2 && error == 0) ||
            std::any_of(wire.begin() + 36, wire.begin() + 40,
                        [](uint8_t byte) { return byte != 0; }) ||
            std::any_of(wire.begin() + 440, wire.begin() + kP51ReplyOffset,
                        [](uint8_t byte) { return byte != 0; }) ||
            std::any_of(wire.begin() + kP51ReplyOffset + kP51ReplyBytes,
                        wire.end(), [](uint8_t byte) { return byte != 0; }))
            return false;
        P51SourceReservationRequest request;
        request.absolute_deadline.expires_at_ns =
            static_cast<int64_t>(detail::control_get_u64(wire.data() + 40));
        request.absolute_deadline.clock_domain_id =
            detail::control_get_u64(wire.data() + 48);
        request.absolute_deadline.time_namespace_id =
            detail::control_get_u64(wire.data() + 56);
        if (!request.absolute_deadline.valid() ||
            !decode_p51_arm(wire.data() + 64, request.arm) ||
            operation.request_id != request.arm.source.source_request_id)
            return false;
        operation.p51_reservation = request;
        if (phase != 0) {
            P51SourceReservationResult result;
            result.error_code = error;
            if (phase == 1) {
                P51SourceArmedFields armed;
                armed.arm = request.arm;
                if (!decode_p51_armed(wire.data() + kP51ReplyOffset, armed))
                    return false;
                result.armed = std::move(armed);
            }
            if (!result.valid())
                return false;
            operation.p51_reservation_result = std::move(result);
        }
        if (phase != 1 &&
            std::any_of(wire.begin() + kP51ReplyOffset, wire.end(),
                        [](uint8_t byte) { return byte != 0; }))
            return false;
    }
    if (kind == ControlOperationKind::SourceReservationCancel) {
        const uint16_t phase = detail::control_get_u16(wire.data() + 32);
        if (phase > 2 || detail::control_get_u16(wire.data() + 34) != 0 ||
            std::any_of(wire.begin() + 36, wire.begin() + 40,
                        [](uint8_t byte) { return byte != 0; }) ||
            std::any_of(wire.begin() + 440, wire.begin() + kP51ReplyOffset,
                        [](uint8_t byte) { return byte != 0; }) ||
            std::any_of(wire.begin() + kP51ReplyOffset + kP51ReplyBytes,
                        wire.end(), [](uint8_t byte) { return byte != 0; }))
            return false;
        P51SourceReservationCancel cancel;
        cancel.absolute_deadline.expires_at_ns =
            static_cast<int64_t>(detail::control_get_u64(wire.data() + 40));
        cancel.absolute_deadline.clock_domain_id =
            detail::control_get_u64(wire.data() + 48);
        cancel.absolute_deadline.time_namespace_id =
            detail::control_get_u64(wire.data() + 56);
        P51SourceArmFields arm;
        if (!cancel.absolute_deadline.valid() ||
            !decode_p51_arm(wire.data() + 64, arm) ||
            operation.request_id != arm.source.source_request_id)
            return false;
        P51SourceArmedFields armed;
        armed.arm = arm;
        if (!decode_p51_armed(wire.data() + kP51ReplyOffset, armed))
            return false;
        cancel.arm = arm;
        cancel.armed = std::move(armed);
        if (phase != 0)
            cancel.cancelled = phase == 1;
        operation.p51_reservation_cancel = std::move(cancel);
        operation.p51_reservation_cancel_result =
            operation.p51_reservation_cancel->cancelled;
    }
    if (kind == ControlOperationKind::P51SourceTransfer) {
        const uint16_t phase = detail::control_get_u16(wire.data() + 32);
        if (phase > 1 || detail::control_get_u16(wire.data() + 34) != 0 ||
            std::any_of(wire.begin() + 36, wire.begin() + 40,
                        [](uint8_t byte) { return byte != 0; }) ||
            std::any_of(wire.begin() + 440, wire.begin() + kP51ReplyOffset,
                        [](uint8_t byte) { return byte != 0; }) ||
            std::any_of(wire.begin() + kP51TransferResultOffset +
                            kP51TransferResultBytes,
                        wire.end(), [](uint8_t byte) { return byte != 0; }))
            return false;
        P51SourceTransferRequest request;
        request.absolute_deadline.expires_at_ns =
            static_cast<int64_t>(detail::control_get_u64(wire.data() + 40));
        request.absolute_deadline.clock_domain_id =
            detail::control_get_u64(wire.data() + 48);
        request.absolute_deadline.time_namespace_id =
            detail::control_get_u64(wire.data() + 56);
        P51SourceArmFields arm;
        P51SourceArmedFields armed;
        if (!request.absolute_deadline.valid() ||
            !decode_p51_arm(wire.data() + 64, arm) ||
            operation.request_id != arm.source.source_request_id)
            return false;
        armed.arm = arm;
        if (!decode_p51_armed(wire.data() + kP51ReplyOffset, armed) ||
            !armed.valid() ||
            armed.arm != arm)
            return false;
        request.armed = std::move(armed);
        operation.absolute_deadline = request.absolute_deadline;
        operation.p51_source_transfer = request;
        if (phase == 1) {
            P50SourceTransferResult result;
            std::copy(wire.begin() + kP51TransferResultOffset,
                      wire.begin() + kP51TransferResultOffset + 16,
                      result.c_store_guid.bytes.begin());
            result.code = static_cast<SourceTransferResultCode>(
                detail::control_get_u16(wire.data() +
                                        kP51TransferResultOffset + 16));
            result.error_code = detail::control_get_u16(
                wire.data() + kP51TransferResultOffset + 18);
            result.attempts = wire[kP51TransferResultOffset + 20];
            if (wire[kP51TransferResultOffset + 21] != 0 ||
                wire[kP51TransferResultOffset + 22] != 0 ||
                wire[kP51TransferResultOffset + 23] != 0)
                return false;
            result.tu_seq = detail::control_get_u64(
                wire.data() + kP51TransferResultOffset + 24);
            result.raw_bytes = detail::control_get_u64(
                wire.data() + kP51TransferResultOffset + 32);
            std::copy(wire.begin() + kP51TransferResultOffset + 40,
                      wire.begin() + kP51TransferResultOffset + 56,
                      result.raw_digest.bytes.begin());
            if (!result.valid())
                return false;
            operation.p51_source_transfer_result = std::move(result);
        } else if (std::any_of(
                       wire.begin() + kP51TransferResultOffset,
                       wire.begin() + kP51TransferResultOffset +
                           kP51TransferResultBytes,
                       [](uint8_t byte) { return byte != 0; })) {
            return false;
        }
    }
    return true;
}

}  // namespace icecc::p50::local
