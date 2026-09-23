#include "unittests/support/p50_reverse_fd_retry.h"
#include "../services/comm.h"

#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace icecc::p50;
using namespace icecc::p50::local;

namespace {

void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

P50SourceArm arm() {
    P50SourceArm value;
    value.wire_job_id = 11;
    value.assignment_epoch = 13;
    value.assignment_nonce = 17;
    value.selected_f_host = "f.example";
    value.selected_f_ordinary_port = 8765;
    value.selected_f_cache_port = 9876;
    value.cache_protocol = CACHE_WIRE_REVISION;
    value.cache_profile = CACHE_PROFILE_ZSTD_TU;
    value.logical_job = 19;
    value.attempt_id = 23;
    value.c_store_generation = 29;
    value.c_store_guid.bytes[15] = 31;
    value.source_request_id = 37;
    value.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    return value;
}

ReverseFdAttempt attempt(uint64_t id = 41, uint64_t token = 43,
                         uint32_t remaining = 100) {
    return ReverseFdAttempt{Identity{47, 53}, id, token, remaining, arm()};
}

void codec_rejects_timepoint_and_bounds() {
    ReverseFdAttempt value = attempt();
    const auto wire = encode_reverse_fd_attempt(value);
    require(!wire.empty(), "attempt did not encode");
    const auto decoded = decode_reverse_fd_attempt(wire);
    require(decoded.has_value() && *decoded == value, "attempt did not round-trip");
    auto trailing = wire;
    trailing.push_back(0);
    require(!decode_reverse_fd_attempt(trailing), "trailing bytes crossed frame boundary");
    value.remaining_ms = kMaxReverseFdRemainingMs + 1;
    require(encode_reverse_fd_attempt(value).empty(), "unbounded duration was encoded");
    value = attempt();
    value.delivery_id = 0;
    require(encode_reverse_fd_attempt(value).empty(), "zero DeliveryId was encoded");
    value = attempt();
    value.token = 0;
    require(encode_reverse_fd_attempt(value).empty(), "zero token was encoded");
}

void reducer_replay_and_fencing() {
    auto owner = ReverseFdOwner::stage(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("sealed-input"), 12),
        attempt(101, 103, 0),
        ReverseFdOwner::Clock::now() + std::chrono::seconds(10));
    require(owner.has_value() && owner->valid() && owner->cloexec(),
            "sealed master was not staged");
    const auto original_deadline = owner->original_deadline();
    ReverseFdReceiverLedger ledger(107);
    const auto now = ReverseFdOwner::Clock::now();
    require(ledger.arm_input(arm(), now, now + std::chrono::seconds(5)) ==
                ReverseFdDecision::Accepted,
            "WAITP50INPUT arm was not installed");

    const int first = ::fcntl(owner->master_fd(), F_DUPFD_CLOEXEC, 0);
    require(first >= 0, "first duplicate failed");
    ReverseFdAttempt first_attempt = attempt(101, 103, 100);
    require(ledger.accept(first_attempt, HandoffFd(first), now) == ReverseFdDecision::Accepted,
            "first delivery was not accepted");
    require(ledger.state() == ReverseFdReceiverState::ToCompile &&
                ledger.transition_count() == 1 && ledger.fork_count() == 1,
            "accept did not transition exactly once before ACK");

    const int duplicate = ::fcntl(owner->master_fd(), F_DUPFD_CLOEXEC, 0);
    require(duplicate >= 0, "duplicate attempt failed");
    require(ledger.accept(first_attempt, HandoffFd(duplicate), now) ==
                ReverseFdDecision::ExactReplay,
            "exact replay was not ACKable");
    require(ledger.transition_count() == 1 && ledger.fork_count() == 1,
            "exact replay caused a second transition/fork");

    const int conflict_fd = ::fcntl(owner->master_fd(), F_DUPFD_CLOEXEC, 0);
    require(conflict_fd >= 0, "conflict duplicate failed");
    ReverseFdAttempt conflict = first_attempt;
    conflict.token++;
    require(ledger.accept(conflict, HandoffFd(conflict_fd), now) == ReverseFdDecision::Conflict,
            "conflicting duplicate was accepted");
    const int stale_fd = ::fcntl(owner->master_fd(), F_DUPFD_CLOEXEC, 0);
    require(stale_fd >= 0, "stale duplicate failed");
    ReverseFdAttempt stale = first_attempt;
    stale.delivery_id--;
    require(ledger.accept(stale, HandoffFd(stale_fd), now) == ReverseFdDecision::Stale,
            "lower duplicate was accepted");
    require(ledger.take_for_compile().valid(), "accepted compiler FD was not retained");
    ledger.cancel();
    require(ledger.state() == ReverseFdReceiverState::Cancelled &&
                !ledger.has_compiler_fd(), "cancel did not close compiler FD");
    require(owner->original_deadline() == original_deadline,
            "retry operation changed original deadline");
    owner->cancel();
    require(!owner->valid(), "cancel did not close sealed master");

    auto expiring_owner = ReverseFdOwner::stage(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("expiry"), 6),
        attempt(109, 113, 0), ReverseFdOwner::Clock::now() + std::chrono::milliseconds(1));
    require(expiring_owner.has_value(), "expiry owner stage failed");
    expiring_owner->expire(expiring_owner->original_deadline() + std::chrono::milliseconds(1));
    require(!expiring_owner->valid(), "expiry did not close master");

    ReverseFdReceiverLedger expired_ledger(127);
    const auto expired_now = ReverseFdOwner::Clock::now();
    require(expired_ledger.arm_input(arm(), expired_now,
                                     expired_now + std::chrono::milliseconds(1)) ==
                ReverseFdDecision::Accepted,
            "expiry arm was not installed");
    auto receiver_expiry_owner = ReverseFdOwner::stage(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("expiry-fd"), 9),
        attempt(131, 137, 0), ReverseFdOwner::Clock::now() + std::chrono::seconds(5));
    require(receiver_expiry_owner.has_value(), "receiver expiry stage failed");
    const int expired_fd = ::fcntl(receiver_expiry_owner->master_fd(), F_DUPFD_CLOEXEC, 0);
    require(expired_fd >= 0, "receiver expiry duplicate failed");
    require(expired_ledger.accept(attempt(131, 137, 100), HandoffFd(expired_fd),
                                  expired_now + std::chrono::seconds(1)) ==
                ReverseFdDecision::Expired && !expired_ledger.has_compiler_fd(),
            "receiver expiry retained a compiler descriptor");
    receiver_expiry_owner->cancel();
}

void transport_attempt_and_ack() {
    auto owner = ReverseFdOwner::stage(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("transport"), 9),
        attempt(151, 157, 0), ReverseFdOwner::Clock::now() + std::chrono::seconds(5));
    require(owner.has_value(), "transport owner stage failed");
    int fds[2] = {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");
    Connection sender(fds[0]);
    Connection receiver(fds[1]);
    const CredentialExpectation credentials{static_cast<uint64_t>(::getuid()),
                                            static_cast<uint64_t>(::getgid()),
                                            static_cast<uint64_t>(::getpid())};
    require(sender.verify_peer_credentials(credentials) == Status::Ok &&
                receiver.verify_peer_credentials(credentials) == Status::Ok,
            "peer authentication failed");
    ReverseFdReceiverLedger ledger(163);
    const auto now = ReverseFdOwner::Clock::now();
    require(ledger.arm_input(arm(), now, now + std::chrono::seconds(4)) ==
                ReverseFdDecision::Accepted,
            "transport arm failed");
    ReverseFdDecision receiver_result = ReverseFdDecision::Invalid;
    std::thread receiving([&] {
        receiver_result = receive_reverse_fd_attempt(
            receiver, ledger, ReverseFdOwner::Clock::now() + std::chrono::seconds(3));
    });
    const ReverseFdDecision sender_result = owner->send_attempt(sender);
    receiving.join();
    require(sender_result == ReverseFdDecision::Accepted &&
                receiver_result == ReverseFdDecision::Accepted,
            "reverse FD attempt/ACK did not complete");
    ReverseFdDecision replay_receiver_result = ReverseFdDecision::Invalid;
    std::thread replay_receiving([&] {
        replay_receiver_result = receive_reverse_fd_attempt(
            receiver, ledger, ReverseFdOwner::Clock::now() + std::chrono::seconds(3));
    });
    const ReverseFdDecision replay_sender_result = owner->send_attempt(sender);
    replay_receiving.join();
    require(replay_sender_result == ReverseFdDecision::ExactReplay &&
                replay_receiver_result == ReverseFdDecision::ExactReplay &&
                ledger.transition_count() == 1 && ledger.fork_count() == 1,
            "retry did not reuse identity/deadline as an exact replay");
    require(ledger.take_for_compile().valid(), "transport did not retain sealed duplicate");
    owner->cancel();
}

}  // namespace

int main() {
    try {
        codec_rejects_timepoint_and_bounds();
        reducer_replay_and_fencing();
        transport_attempt_and_ack();
        std::cout << "ok - bounded reverse sealed-FD retry/replay reducer\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
