#include "cache/p50_control_operation.h"
#include "cache/p50_input_lifecycle.h"

#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace icecc::p50;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "p50_input_lifecycle_test: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition)
        fail(message);
}

InputRecordKey key(uint64_t value) {
    return InputRecordKey{Id128::from_u64(0x5000000000000000ULL + value),
                          TuSeq{value}};
}

InputLifecycleRequest request(local::Identity identity, InputRecordKey input,
                              InputLeaseOwner owner, uint64_t operation_id,
                              InputLifecycleAction action) {
    InputLifecycleRequest value;
    value.identity = identity;
    value.key = input;
    value.owner = owner;
    value.operation_id = operation_id;
    value.action = action;
    return value;
}

void put_u16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes.at(offset) = static_cast<uint8_t>(value >> 8);
    bytes.at(offset + 1) = static_cast<uint8_t>(value);
}

void put_u32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes.at(offset) = static_cast<uint8_t>(value >> 24);
    bytes.at(offset + 1) = static_cast<uint8_t>(value >> 16);
    bytes.at(offset + 2) = static_cast<uint8_t>(value >> 8);
    bytes.at(offset + 3) = static_cast<uint8_t>(value);
}

void put_u64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    for (size_t index = 0; index != 8; ++index)
        bytes.at(offset + index) =
            static_cast<uint8_t>(value >> (56 - index * 8));
}

void wire_fixtures() {
    using namespace icecc::p50::local;
    const Identity identity{0x1011121314151617ULL, 0x2122232425262728ULL};
    const InputRecordKey input = key(9);
    const InputLeaseOwner owner{0x3132333435363738ULL,
                                0x4142434445464748ULL,
                                0x5152535455565758ULL};

    const std::vector<uint8_t> cache = encode_control_operation(
        make_cache_session_operation(identity, 0x6162636465666768ULL));
    require(cache.size() == kCacheSessionOperationBytes && cache[0] == 0 &&
                cache[1] == kControlOperationVersionV1,
            "cache-session v1 fixture changed");

    const std::vector<uint8_t> attachment = encode_control_operation(
        make_input_fd_attachment_operation(identity, input, owner, 7));
    require(attachment.size() == kInputFdAttachmentOperationBytes &&
                attachment[0] == 0 &&
                attachment[1] == kControlOperationVersionV2 &&
                attachment[80] == 0 && attachment[81] == 0 &&
                attachment[82] == 0 && attachment[83] == 0,
            "owner-bearing attachment is not the exact v2 request fixture");
    ControlOperation decoded;
    require(decode_control_operation(attachment, decoded) &&
                decoded.kind == ControlOperationKind::InputFdAttachment &&
                decoded.identity == identity && decoded.input == input &&
                decoded.owner == owner && !decoded.lifecycle_result,
            "owner-bearing attachment did not round-trip exactly");

    const InputLifecycleRequest close = request(
        identity, input, owner, 11, InputLifecycleAction::CloseAcceptedJob);
    const std::vector<uint8_t> lifecycle = encode_control_operation(
        make_input_lifecycle_operation(close));
    require(lifecycle.size() == kInputLifecycleOperationBytes &&
                lifecycle[0] == 0 && lifecycle[1] == kControlOperationVersionV2 &&
                lifecycle[80] == 0 && lifecycle[81] == 2 &&
                lifecycle[82] == 0 && lifecycle[83] == 0,
            "lifecycle request fixture changed");
    const std::vector<uint8_t> reply = encode_control_operation(
        make_input_lifecycle_reply_operation(
            close, InputLifecycleApplyStatus::StaleOwner));
    require(reply.size() == kInputLifecycleOperationBytes &&
                reply[82] == 0 &&
                reply[83] ==
                    static_cast<uint8_t>(InputLifecycleApplyStatus::StaleOwner) + 1,
            "lifecycle result fixture changed");
    require(decode_control_operation(reply, decoded) &&
                decoded.lifecycle_result == InputLifecycleApplyStatus::StaleOwner &&
                decoded.lifecycle_action == InputLifecycleAction::CloseAcceptedJob,
            "lifecycle result did not round-trip exactly");

    std::vector<uint8_t> reserved_mutant = reply;
    reserved_mutant[84] = 1;
    require(!decode_control_operation(reserved_mutant, decoded),
            "nonzero v2 reserved byte was accepted");
    std::vector<uint8_t> version_mutant = lifecycle;
    version_mutant[1] = kControlOperationVersionV1;
    require(!decode_control_operation(version_mutant, decoded),
            "lifecycle operation was accepted under v1");
    std::vector<uint8_t> result_mutant = reply;
    result_mutant[82] = 0xff;
    result_mutant[83] = 0xff;
    require(!decode_control_operation(result_mutant, decoded),
            "unknown lifecycle result was accepted");

    // Exact 904d-era attachment bytes remain parseable for an explicit mixed
    // version refusal.  They cannot authorize because owner is absent.
    std::vector<uint8_t> legacy(kLegacyInputFdAttachmentOperationBytes, 0);
    put_u16(legacy, 0, kControlOperationVersionV1);
    put_u16(legacy, 2,
            static_cast<uint16_t>(ControlOperationKind::InputFdAttachment));
    put_u32(legacy, 4, static_cast<uint32_t>(legacy.size()));
    put_u64(legacy, 8, identity.generation);
    put_u64(legacy, 16, identity.attempt);
    put_u64(legacy, 24, 19);
    std::copy(input.c_store_guid.bytes.begin(), input.c_store_guid.bytes.end(),
              legacy.begin() + 32);
    put_u64(legacy, 48, input.tu_seq.value);
    require(decode_control_operation(legacy, decoded) &&
                decoded.kind == ControlOperationKind::InputFdAttachment &&
                decoded.input == input && !decoded.owner,
            "legacy v1 attachment fixture did not decode ownerless");
}

void operation_cancel_wire_fixtures() {
    using namespace icecc::p50::local;
    const Identity identity{0x9192939495969798ULL, 0xa1a2a3a4a5a6a7a8ULL};
    ControlBindingPlaceholder binding{};
    for (size_t index = 0; index != binding.size(); ++index)
        binding[index] = static_cast<uint8_t>(index + 1);
    const ControlOperation request = make_operation_cancel_operation(
        identity, ControlCancelTargetRole::FSession, 0x4142434445464748ULL,
        ControlCancellationReason::Requested, binding);
    const std::vector<uint8_t> wire = encode_control_operation(request);
    require(wire.size() == kOperationCancelOperationBytes &&
                wire[0] == 0 && wire[1] == kControlOperationVersionV3 &&
                wire[2] == 0 && wire[3] == static_cast<uint8_t>(ControlOperationKind::OperationCancel) &&
                wire[32] == 0 && wire[33] == static_cast<uint8_t>(ControlCancelTargetRole::FSession) &&
                wire[34] == 0 && wire[35] == static_cast<uint8_t>(ControlCancellationReason::Requested) &&
                wire[36] == 0 && wire[37] == static_cast<uint8_t>(ControlOperationRole::Daemon) &&
                std::equal(binding.begin(), binding.end(), wire.begin() + 40),
            "operation-cancel v3 fixture changed");
    ControlOperation decoded;
    require(decode_control_operation(wire, decoded) &&
                decoded.kind == ControlOperationKind::OperationCancel &&
                decoded.identity == identity &&
                decoded.request_id == request.request_id &&
                decoded.cancel_target_role == ControlCancelTargetRole::FSession &&
                decoded.cancellation_reason == ControlCancellationReason::Requested &&
                decoded.sender_role == ControlOperationRole::Daemon &&
                decoded.binding_placeholder == binding,
            "operation-cancel did not round-trip exact launch binding");

    std::vector<uint8_t> reserved_mutant = wire;
    reserved_mutant[38] = 1;
    require(!decode_control_operation(reserved_mutant, decoded),
            "operation-cancel reserved bytes were accepted");
    std::vector<uint8_t> role_mutant = wire;
    role_mutant[33] = 0;
    require(!decode_control_operation(role_mutant, decoded),
            "operation-cancel unknown target role was accepted");
    std::vector<uint8_t> reason_mutant = wire;
    reason_mutant[35] = 0;
    require(!decode_control_operation(reason_mutant, decoded),
            "operation-cancel unknown reason was accepted");
    std::vector<uint8_t> sender_mutant = wire;
    sender_mutant[37] = static_cast<uint8_t>(ControlOperationRole::Sidecar);
    require(decode_control_operation(sender_mutant, decoded) &&
                decoded.sender_role == ControlOperationRole::Sidecar,
            "operation-cancel sender direction was not typed");
    std::vector<uint8_t> size_mutant = wire;
    put_u32(size_mutant, 4, static_cast<uint32_t>(kCacheSessionOperationBytes));
    require(!decode_control_operation(size_mutant, decoded),
            "operation-cancel accepted a cache-session size");
    std::vector<uint8_t> identity_mutant = wire;
    identity_mutant[15] = 0;
    identity_mutant[16] = 0;
    require(decode_control_operation(identity_mutant, decoded) &&
                decoded.identity != identity,
            "operation-cancel identity was not carried as an exact field");
}

void commit_attachment_cancel_replace_close() {
    InputLifecycleRegistry registry(8, 32);
    const local::Identity identity{7, 3};
    const InputRecordKey input = key(1);
    const InputLeaseOwner first{1001, 2001, 3001};
    const InputLeaseOwner second{1001, 2001, 3002};

    require(registry.prepare_route_commit(input) ==
                InputLifecycleCommitDecision::Open &&
                registry.observe_route_commit(input, true),
            "open route commit did not enter lifecycle ownership");
    require(registry.begin_attachment(input, first, 1),
            "first owner could not reserve attachment");
    registry.finish_attachment(input, first, 1, true);
    require(!registry.begin_attachment(input, first, 2),
            "one attempt admitted a second attachment");

    const InputLifecycleRequest cancel = request(
        identity, input, first, 10, InputLifecycleAction::CancelAttempt);
    InputLifecycleApplyResult decision = registry.begin_apply(cancel);
    require(decision.status == InputLifecycleApplyStatus::Applied &&
                !decision.close_record && !decision.collect_record,
            "attempt cancellation tried to close the logical input lease");
    (void)registry.finish_apply(cancel, true);
    require(registry.begin_apply(cancel).status ==
                InputLifecycleApplyStatus::AlreadyApplied,
            "lost cancellation ACK was not exactly replay-safe");
    InputLifecycleRequest conflicting = cancel;
    conflicting.action = InputLifecycleAction::CancelJob;
    require(registry.begin_apply(conflicting).status ==
                InputLifecycleApplyStatus::ConflictingReplay,
            "operation ID reuse with changed action was not rejected");

    require(registry.begin_attachment(input, second, 2),
            "cancelled attempt could not transfer ownership to replacement");
    registry.finish_attachment(input, second, 2, true);
    const InputLifecycleRequest stale = request(
        identity, input, first, 11, InputLifecycleAction::CloseAcceptedJob);
    require(registry.begin_apply(stale).status ==
                InputLifecycleApplyStatus::StaleOwner,
            "stale owner could close a replacement attempt");

    const InputLifecycleRequest close = request(
        identity, input, second, 12,
        InputLifecycleAction::CloseAcceptedJob);
    decision = registry.begin_apply(close);
    require(decision.status == InputLifecycleApplyStatus::Applied &&
                decision.close_record && decision.collect_record,
            "accepted result did not request close and collection");
    (void)registry.finish_apply(close, true);
    require(registry.owner_count() == 0 && registry.replay_count() >= 3,
            "settled committed owner was not boundedly reclaimed");
    require(registry.begin_apply(close).status ==
                InputLifecycleApplyStatus::AlreadyApplied,
            "settled close lost its exact replay result");
}

void close_before_commit_and_attach_close_race() {
    const local::Identity identity{9, 4};
    const InputLeaseOwner owner{101, 201, 301};
    {
        InputLifecycleRegistry registry(4, 8);
        const InputRecordKey input = key(2);
        const InputLifecycleRequest close = request(
            identity, input, owner, 1, InputLifecycleAction::CancelJob);
        InputLifecycleApplyResult decision = registry.begin_apply(close);
        require(decision.status == InputLifecycleApplyStatus::Applied &&
                    !decision.close_record,
                "close-before-commit fabricated an endpoint record");
        (void)registry.finish_apply(close, true);
        require(registry.job_closed(input) && registry.owner_count() == 1,
                "close-before-commit tombstone was not retained");
        require(registry.prepare_route_commit(input) ==
                    InputLifecycleCommitDecision::Closed,
                "late commit did not select the non-attachable closed path");
        require(registry.observe_route_commit(input, false) &&
                    registry.owner_count() == 0,
                "validated closed commit did not retire its tombstone");
        require(registry.begin_apply(close).status ==
                    InputLifecycleApplyStatus::AlreadyApplied,
                "closed-commit settlement lost exact ACK replay");
    }
    {
        InputLifecycleRegistry registry(4, 8);
        const InputRecordKey input = key(3);
        require(registry.prepare_route_commit(input) ==
                    InputLifecycleCommitDecision::Open &&
                    registry.observe_route_commit(input, true) &&
                    registry.begin_attachment(input, owner, 7),
                "attach-first race setup failed");
        const InputLifecycleRequest close = request(
            identity, input, owner, 2,
            InputLifecycleAction::CloseAcceptedJob);
        InputLifecycleApplyResult decision = registry.begin_apply(close);
        require(decision.status == InputLifecycleApplyStatus::Applied &&
                    decision.close_record,
                "attach-first close did not serialize behind authorization");
        (void)registry.finish_apply(close, true);
        require(registry.owner_count() == 1,
                "pending attachment owner was reclaimed before handoff finished");
        registry.finish_attachment(input, owner, 7, true);
        require(registry.owner_count() == 0,
                "attachment completion did not release the closed owner");
        require(!registry.begin_attachment(input, owner, 8),
                "closed input accepted a new attachment");
    }
}

void replacement_cycle_and_bounds() {
    const InputRecordKey input = key(4);
    const InputLeaseOwner first{42, 1, 11};
    const InputLeaseOwner second{42, 1, 12};
    const local::Identity identity{5, 6};
    InputLifecycleRegistry registry(1, 4);
    require(registry.prepare_route_commit(input) ==
                InputLifecycleCommitDecision::Open &&
                registry.observe_route_commit(input, true) &&
                registry.begin_attachment(input, first, 1),
            "replacement-cycle setup failed");
    registry.finish_attachment(input, first, 1, true);
    const InputLifecycleRequest cancel_first = request(
        identity, input, first, 1, InputLifecycleAction::CancelAttempt);
    require(registry.begin_apply(cancel_first).status ==
                InputLifecycleApplyStatus::Applied,
            "first attempt cancellation failed");
    (void)registry.finish_apply(cancel_first, true);
    require(registry.begin_attachment(input, second, 2),
            "replacement owner was not admitted");
    registry.finish_attachment(input, second, 2, true);
    const InputLifecycleRequest cancel_second = request(
        identity, input, second, 2, InputLifecycleAction::CancelAttempt);
    require(registry.begin_apply(cancel_second).status ==
                InputLifecycleApplyStatus::Applied,
            "second attempt cancellation failed");
    (void)registry.finish_apply(cancel_second, true);
    require(!registry.begin_attachment(input, first, 3),
            "retired assignment owner was resurrected");
    require(registry.prepare_route_commit(key(5)) ==
                InputLifecycleCommitDecision::CapacityExceeded,
            "owner capacity was not enforced before publication");
    registry.clear();
    require(registry.owner_count() == 0 && registry.replay_count() == 0,
            "incarnation clear retained lifecycle state");

    InputLifecycleRegistry reservation(1, 2);
    require(reservation.prepare_route_commit(key(6)) ==
                InputLifecycleCommitDecision::Open,
            "commit reservation failed");
    reservation.abort_route_commit(key(6));
    require(reservation.owner_count() == 0,
            "failed commit reservation leaked capacity");
}

void cancellation_racing_attachment_finalization_revokes_exact_owner() {
    for (const bool begin_before_cancel : {false, true}) {
    for (const bool acknowledged : {false, true}) {
    InputLifecycleRegistry registry(4, 8);
    const local::Identity identity{9, 10};
    const InputRecordKey input = key(7);
    const InputLeaseOwner first{71, 72, 73};
    const InputLeaseOwner replacement{71, 72, 74};

    require(registry.prepare_route_commit(input) ==
                InputLifecycleCommitDecision::Open &&
                registry.observe_route_commit(input, true),
            "attachment/cancellation race setup failed");
    if (begin_before_cancel)
        require(registry.begin_attachment(input, first, 1),
                "initial attachment reservation failed");

    const InputLifecycleRequest cancel = request(
        identity, input, first, 1, InputLifecycleAction::CancelAttempt);
    require(registry.begin_apply(cancel).status ==
                InputLifecycleApplyStatus::Applied,
            "pending attachment attempt cancellation failed");
    (void)registry.finish_apply(cancel, true);

    // Neither positive nor negative late ACK may resurrect a cancelled owner.
    // Cancellation before the first reservation must fence that owner too.
    registry.finish_attachment(input, first, 1, acknowledged);
    require(!registry.begin_attachment(input, first, 2),
            "cancelled exact owner reattached after lost descriptor ACK");
    require(registry.begin_attachment(input, replacement, 2),
            "fresh replacement owner was rejected after raced cancellation");
    registry.finish_attachment(input, replacement, 2, true);
    }
    }
}


// The retired attempt-replacement lane stays wire-reserved but is refused
// everywhere: a hand-minted 192-byte v5 frame no longer has a decode row,
// a v2 InputLifecycle frame carrying action 4 is rejected by the admission
// gate at decode, and encoding an InputLifecycle operation with action 4
// yields no bytes at all.
void retired_v5_lane_refused() {
    using namespace icecc::p50::local;
    const Identity identity{0x1011121314151617ULL, 0x2122232425262728ULL};
    const InputRecordKey input = key(41);
    const InputLeaseOwner owner{0x3132333435363738ULL,
                                0x4142434445464748ULL,
                                0x5152535455565758ULL};

    std::vector<uint8_t> v5(kInputAttemptRetirementOperationBytes, 0);
    put_u16(v5, 0, kControlOperationVersionV5);
    put_u16(v5, 2, static_cast<uint16_t>(ControlOperationKind::InputLifecycle));
    put_u32(v5, 4, static_cast<uint32_t>(v5.size()));
    put_u64(v5, 8, identity.generation);
    put_u64(v5, 16, identity.attempt);
    put_u64(v5, 24, 4242);
    std::copy(input.c_store_guid.bytes.begin(), input.c_store_guid.bytes.end(),
              v5.begin() + 32);
    put_u64(v5, 48, input.tu_seq.value);
    put_u64(v5, 56, owner.logical_job);
    put_u64(v5, 64, owner.assignment_epoch);
    put_u64(v5, 72, owner.assignment_nonce);
    put_u16(v5, 80,
            static_cast<uint16_t>(InputLifecycleAction::PrepareAttemptRetirement));
    ControlOperation decoded;
    require(!decode_control_operation(v5, decoded),
            "retired v5 retirement frame still decodes");

    std::vector<uint8_t> v2(kInputLifecycleOperationBytes, 0);
    put_u16(v2, 0, kControlOperationVersionV2);
    put_u16(v2, 2, static_cast<uint16_t>(ControlOperationKind::InputLifecycle));
    put_u32(v2, 4, static_cast<uint32_t>(v2.size()));
    put_u64(v2, 8, identity.generation);
    put_u64(v2, 16, identity.attempt);
    put_u64(v2, 24, 4243);
    std::copy(input.c_store_guid.bytes.begin(), input.c_store_guid.bytes.end(),
              v2.begin() + 32);
    put_u64(v2, 48, input.tu_seq.value);
    put_u64(v2, 56, owner.logical_job);
    put_u64(v2, 64, owner.assignment_epoch);
    put_u64(v2, 72, owner.assignment_nonce);
    put_u16(v2, 80,
            static_cast<uint16_t>(InputLifecycleAction::CommitAttemptReplacement));
    require(!decode_control_operation(v2, decoded),
            "v2 InputLifecycle frame with reserved action 5 was accepted");
    put_u16(v2, 80,
            static_cast<uint16_t>(InputLifecycleAction::CloseLogicalInputLease));
    require(!decode_control_operation(v2, decoded),
            "v2 InputLifecycle frame with reserved action 6 was accepted");

    InputLifecycleRequest forged = request(
        identity, input, owner, 4244,
        InputLifecycleAction::CloseLogicalInputLease);
    require(encode_control_operation(
                make_input_lifecycle_operation(forged)).empty(),
            "encode produced bytes for a reserved retirement action");
    require(!input_lifecycle_action_valid(forged.action),
            "admission gate still accepts a reserved retirement action");
}

}  // namespace

int main() {
    bool rejected_zero_limits = false;
    try {
        InputLifecycleRegistry invalid(0, 1);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected_zero_limits = true;
    }
    require(rejected_zero_limits, "zero lifecycle limit was accepted");
    require(!input_lease_owner_valid({}) &&
                input_lease_owner_valid({1, 2, 3}) &&
                !input_lifecycle_action_valid(InputLifecycleAction::None),
            "identity/action validation changed");
    wire_fixtures();
    operation_cancel_wire_fixtures();
    commit_attachment_cancel_replace_close();
    close_before_commit_and_attach_close_race();
    replacement_cycle_and_bounds();
    cancellation_racing_attachment_finalization_revokes_exact_owner();
    retired_v5_lane_refused();
    return 0;
}
