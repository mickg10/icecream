#include "cache/p50_control_operation.h"
#include "cache/p50_input_lifecycle.h"

#include <cerrno>
#include <cstdint>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <atomic>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
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
    InputLifecycleRegistry registry(4, 8);
    const local::Identity identity{9, 10};
    const InputRecordKey input = key(7);
    const InputLeaseOwner first{71, 72, 73};
    const InputLeaseOwner replacement{71, 72, 74};

    require(registry.prepare_route_commit(input) ==
                InputLifecycleCommitDecision::Open &&
                registry.observe_route_commit(input, true) &&
                registry.begin_attachment(input, first, 1),
            "attachment/cancellation race setup failed");

    const InputLifecycleRequest cancel = request(
        identity, input, first, 1, InputLifecycleAction::CancelAttempt);
    require(registry.begin_apply(cancel).status ==
                InputLifecycleApplyStatus::Applied,
            "pending attachment attempt cancellation failed");
    (void)registry.finish_apply(cancel, true);

    // A lost/negative descriptor ACK may finalize after cancellation.  It
    // must not clear the revocation and permit the same owner to attach again.
    registry.finish_attachment(input, first, 1, false);
    require(!registry.begin_attachment(input, first, 2),
            "cancelled exact owner reattached after lost descriptor ACK");
    require(registry.begin_attachment(input, replacement, 2),
            "fresh replacement owner was rejected after raced cancellation");
    registry.finish_attachment(input, replacement, 2, true);
}

InputLifecycleOperationLease refinement_lease(local::Identity identity,
                                              InputRecordKey input,
                                              InputLeaseOwner owner,
                                              uint64_t operation_id) {
    InputLifecycleRequest lifecycle_request = request(
        identity, input, owner, operation_id,
        InputLifecycleAction::PrepareAttemptRetirement);
    lifecycle_request.f_store_generation = 17;
    lifecycle_request.f_store_guid = Id128::from_u64(0xf500000000000017ULL);
    lifecycle_request.immutable_size = 23;
    lifecycle_request.immutable_digest =
        icecc::digest128("p50-refinement-input-digest");
    lifecycle_request.retirement_id = 29;
    const auto clock = sidecar::process_monotonic_clock_identity();
    lifecycle_request.absolute_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(5),
        clock.clock_domain_id, clock.time_namespace_id);
    lifecycle_request.deadline = lifecycle_request.absolute_deadline.as_steady_time_point();
    return make_input_lifecycle_operation_lease(lifecycle_request);
}

void refinement_cancel_commit_ordering() {
    const local::Identity identity{31, 41};
    const InputRecordKey input = key(31);
    const InputLeaseOwner owner{301, 401, 501};
    const InputLifecycleOperationLease cancel_lease =
        refinement_lease(identity, input, owner, 601);
    require(cancel_lease.valid(), "refinement operation lease was not valid");

    InputLifecycleRegistry cancel_first(4, 8);
    const auto prepared = cancel_first.ObservePrepared(cancel_lease, 701);
    require(prepared.has_value() && prepared->valid() &&
                prepared->decoder_touched && prepared->operation == cancel_lease,
            "ObservePrepared did not mint an exact touched observation");
    const auto replay = cancel_first.ObservePrepared(cancel_lease, 701);
    require(replay.has_value() && *replay == *prepared,
            "ObservePrepared exact replay changed its observation identity");
    auto stale = cancel_lease;
    stale.identity.attempt++;
    require(!cancel_first.ObservePrepared(stale, 701).has_value(),
            "stale operation was accepted by ObservePrepared");
    require(cancel_first.CancelOrExpire(cancel_lease, prepared->observation_id) ==
                InputLifecycleRefinementResult::CancelledNoDurability &&
                cancel_first.refinement_state(cancel_lease) ==
                    InputLifecycleRefinementState::CancelledNoDurability &&
                !cancel_first.durable_bundle(cancel_lease).has_value(),
            "cancel-first did not select profile-reset/no-durability state");
    require(cancel_first.CancelOrExpire(cancel_lease, prepared->observation_id) ==
                InputLifecycleRefinementResult::AlreadyCancelled &&
                !cancel_first.SelectCommit(cancel_lease, prepared->observation_id)
                     .has_value(),
            "cancel-first state admitted a second commit");

    InputLifecycleRegistry commit_first(4, 8);
    const auto committed_prepared =
        commit_first.ObservePrepared(cancel_lease, 702);
    require(committed_prepared.has_value(),
            "commit-first preparation was not observed");
    auto permit = commit_first.SelectCommit(
        cancel_lease, committed_prepared->observation_id);
    require(permit.has_value() && permit->valid() &&
                commit_first.refinement_state(cancel_lease) ==
                    InputLifecycleRefinementState::CommitSelected,
            "SelectCommit did not mint one exact permit");
    require(commit_first.CancelOrExpire(cancel_lease,
                                        committed_prepared->observation_id) ==
                InputLifecycleRefinementResult::CommitWon,
            "cancel after commit selection did not preserve commit-wins");

    InputLifecycleDurableBundle bundle;
    bundle.operation = cancel_lease;
    bundle.prepared_id = committed_prepared->prepared_id;
    bundle.observation_id = committed_prepared->observation_id;
    bundle.permit_id = permit->permit_id();
    bundle.durable_sequence = 801;
    bundle.durable_digest = icecc::digest128("p50-refinement-durable-digest");
    require(bundle.valid() &&
                commit_first.CommitDurable(std::move(*permit), bundle) ==
                    InputLifecycleRefinementResult::DurableCommitted &&
                !permit->valid() &&
                commit_first.durable_bundle(cancel_lease) == bundle,
            "CommitDurable did not atomically retain the exact bundle");
    require(commit_first.CancelOrExpire(cancel_lease,
                                        committed_prepared->observation_id) ==
                InputLifecycleRefinementResult::CommitWon &&
                commit_first.SuppressDeliveryAfterCommit(
                    cancel_lease, committed_prepared->observation_id) &&
                commit_first.refinement_state(cancel_lease) ==
                    InputLifecycleRefinementState::DeliverySuppressed &&
                commit_first.SuppressDeliveryAfterCommit(bundle),
            "commit-first durable state was not delivery-suppressed idempotently");
    auto stale_bundle = bundle;
    stale_bundle.operation.identity.attempt++;
    require(!commit_first.SuppressDeliveryAfterCommit(stale_bundle),
            "stale durable completion suppressed a replacement delivery");
}

void lifecycle_transport_round_trip() {
    namespace fs = std::filesystem;
    const char* temporary = std::getenv("TMPDIR");
    const fs::path runtime =
        fs::path(temporary != nullptr ? temporary : "/tmp") /
        ("p50-input-lifecycle-" +
         std::to_string(static_cast<unsigned long long>(::getpid())));
    std::error_code error;
    fs::remove_all(runtime, error);
    require(fs::create_directory(runtime, error) && !error &&
                ::chmod(runtime.c_str(), S_IRWXU) == 0,
            "private lifecycle transport directory creation failed");
    const std::string socket_path = (runtime / "lifecycle.sock").string();
    local::Status listen_status = local::Status::InvalidArgument;
    const int listener = local::listen_unix(socket_path, 3, &listen_status);
    require(listener >= 0 && listen_status == local::Status::Ok,
            "lifecycle transport listener setup failed");

    const local::Identity identity{91, 17};
    const InputLifecycleRequest operation = request(
        identity, key(17), InputLeaseOwner{7001, 8001, 9001}, 41,
        InputLifecycleAction::CloseAcceptedJob);
    const local::CredentialExpectation peer{
        static_cast<uint64_t>(::geteuid()), std::nullopt, std::nullopt};
    std::atomic<bool> server_ok{true};
    std::thread server([&] {
        for (int turn = 0; turn != 3; ++turn) {
            local::Status accept_status = local::Status::InvalidArgument;
            local::Connection connection =
                local::accept_unix(listener, &accept_status);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            local::Frame hello;
            const local::Status credentials = connection.valid()
                ? connection.verify_peer_credentials(peer)
                : local::Status::InvalidArgument;
            const local::Status received = connection.valid()
                ? connection.receive_until(hello, deadline)
                : local::Status::InvalidArgument;
            const local::Status validated = received == local::Status::Ok
                ? local::validate_handshake(hello, local::MessageType::Hello,
                                            local::PeerRole::Daemon, identity)
                : local::Status::InvalidArgument;
            const local::Status acknowledged = validated == local::Status::Ok
                ? connection.send_until(
                      local::make_hello_ack(local::PeerRole::Sidecar, identity),
                      deadline)
                : local::Status::InvalidArgument;
            if (!connection.valid() || accept_status != local::Status::Ok ||
                credentials != local::Status::Ok ||
                received != local::Status::Ok || validated != local::Status::Ok ||
                acknowledged != local::Status::Ok) {
                std::cerr << "lifecycle fixture handshake failed at turn " << turn
                          << " statuses " << static_cast<int>(accept_status) << ' '
                          << static_cast<int>(credentials) << ' '
                          << static_cast<int>(received) << ' '
                          << static_cast<int>(validated) << ' '
                          << static_cast<int>(acknowledged) << '\n';
                server_ok.store(false, std::memory_order_release);
                continue;
            }
            local::Frame frame;
            local::ControlOperation decoded;
            const local::ControlOperation expected =
                local::make_input_lifecycle_operation(operation);
            if (connection.receive_until(frame, deadline) != local::Status::Ok ||
                frame.type != local::MessageType::Data ||
                !local::decode_control_operation(frame.payload, decoded) ||
                decoded.kind != expected.kind ||
                decoded.identity != expected.identity ||
                decoded.request_id != expected.request_id ||
                decoded.input != expected.input ||
                decoded.owner != expected.owner ||
                decoded.lifecycle_action != expected.lifecycle_action ||
                decoded.lifecycle_result.has_value()) {
                server_ok.store(false, std::memory_order_release);
                continue;
            }
            if (turn == 2)
                continue;  // Applied locally but response lost/disconnected.

            InputLifecycleRequest reply_operation = operation;
            if (turn == 1)
                ++reply_operation.owner.assignment_nonce;
            const std::vector<uint8_t> payload =
                local::encode_control_operation(
                    local::make_input_lifecycle_reply_operation(
                        reply_operation, InputLifecycleApplyStatus::Applied));
            const local::Frame reply{local::kProtocolVersion,
                                     local::MessageType::Data,
                                     identity, payload};
            const local::Status sent = payload.empty()
                ? local::Status::InvalidArgument
                : connection.send_until(reply, deadline);
            if (payload.empty() || sent != local::Status::Ok) {
                std::cerr << "lifecycle fixture reply failed at turn " << turn
                          << " payload " << payload.size() << " status "
                          << local::status_name(sent) << '\n';
                server_ok.store(false, std::memory_order_release);
            }
            local::Frame acknowledgement;
            // Production deliberately classifies POLLIN|POLLHUP as a lost
            // best-effort final ACK after the reducer result is durable.  The
            // fixture still needs to prove that the client emitted the exact
            // ACK, so consume already-queued bytes before interpreting HUP.
            pollfd final_ack{connection.native_handle(), POLLIN, 0};
            int final_ack_ready = -1;
            do {
                final_ack_ready = ::poll(&final_ack, 1, 1000);
            } while (final_ack_ready < 0 && errno == EINTR);
            if (sent == local::Status::Ok &&
                (final_ack_ready != 1 ||
                 (final_ack.revents & POLLIN) == 0 ||
                 connection.receive(acknowledgement) != local::Status::Ok ||
                 acknowledgement.type != local::MessageType::Goodbye ||
                 !acknowledgement.payload.empty() ||
                 local::validate_identity(acknowledgement, identity) !=
                     local::Status::Ok)) {
                std::cerr << "lifecycle fixture final ACK failed at turn "
                          << turn << '\n';
                server_ok.store(false, std::memory_order_release);
            }
        }
    });

    const InputLifecycleResult applied = InputLifecycleClient::apply(
        socket_path, operation, peer,
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    require(applied.status == InputLifecycleStatus::Applied,
            "exact lifecycle transport result was not applied");
    const InputLifecycleResult mismatched = InputLifecycleClient::apply(
        socket_path, operation, peer,
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    if (mismatched.status != InputLifecycleStatus::MalformedResponse) {
        std::cerr << "identity-mutated lifecycle response status: "
                  << input_lifecycle_status_name(mismatched.status) << '\n';
        fail("identity-mutated lifecycle response was accepted");
    }
    const InputLifecycleResult lost = InputLifecycleClient::apply(
        socket_path, operation, peer,
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    require(lost.status == InputLifecycleStatus::Disconnected,
            "lost lifecycle response was not retryable/disconnected");

    server.join();
    (void)::close(listener);
    fs::remove_all(runtime, error);
    require(server_ok.load(std::memory_order_acquire),
            "lifecycle transport fixture server failed");
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
    refinement_cancel_commit_ordering();
    lifecycle_transport_round_trip();
    return 0;
}
