#include "../cache/p50_adopted_socket_lease.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace icecc::p50;
using namespace icecc::p50::daemon;
using namespace icecc::p50::local;
using namespace icecc::p50::sidecar;

template <typename T>
concept PublicNativeEndpointRelease = requires(
    T& value, const P50CacheSessionOutcome& outcome,
    const AbsoluteMonotonicDeadline& deadline) {
    value.release_native_fd_for_endpoint(outcome, deadline);
};

static_assert(!PublicNativeEndpointRelease<P5coAdoptedSocketLease>);
static_assert(!PublicNativeEndpointRelease<P5coRetainedSocketLease>);

namespace {

void check(bool value, const char* expression) {
    if (!value)
        throw std::runtime_error(expression);
}
#define CHECK(expression) check((expression), #expression)

std::array<uint8_t, 16> c_guid() {
    std::array<uint8_t, 16> result{};
    for (size_t i = 0; i != result.size(); ++i)
        result[i] = static_cast<uint8_t>(i + 1);
    result[kStoreIdentityRoleByte] &=
        static_cast<uint8_t>(~kStoreIdentityRoleMask);
    return result;
}

std::array<uint8_t, 16> f_guid() {
    std::array<uint8_t, 16> result{};
    for (size_t i = 0; i != result.size(); ++i)
        result[i] = static_cast<uint8_t>(i + 41);
    result[kStoreIdentityRoleByte] |= kStoreIdentityRoleMask;
    return result;
}

ClaimAttemptCapability128 capability() {
    ClaimAttemptCapability128 result;
    for (size_t i = 0; i != result.bytes.size(); ++i)
        result.bytes[i] = static_cast<uint8_t>(51 + i);
    return result;
}

P50CacheSessionOutcome adopted() {
    P50CacheSessionWireClaim claim;
    auto& arm = claim.binding.arm;
    arm.wire_job_id = 17;
    arm.assignment_epoch = 18;
    arm.assignment_nonce = 19;
    arm.selected_f_host = "f.example.test";
    arm.selected_f_ordinary_port = 10250;
    arm.selected_f_cache_port = 10251;
    arm.cache_protocol = CACHE_WIRE_PROTOCOL_V1;
    arm.cache_profile = CACHE_PROFILE_ZSTD_TU;
    arm.logical_job = 20;
    arm.compiler_attempt = 21;
    arm.c_store_generation = 22;
    arm.c_store_derivation_version = kStoreIdentityDerivationVersion;
    arm.c_store_guid = c_guid();
    arm.source_request_id = 23;
    arm.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    arm.c_control_generation = 24;
    arm.c_control_attempt = 25;
    claim.binding.f_control_generation = 31;
    claim.binding.f_control_attempt = 32;
    claim.binding.f_store_generation = 33;
    claim.binding.f_store_guid = f_guid();
    claim.binding.f_store_derivation_version =
        kStoreIdentityDerivationVersion;
    claim.binding.arm_observation_id = 34;
    claim.binding.source_budget_msec = 5000;
    claim.attempt = {1, capability()};
    CHECK(claim.valid());

    P50CacheSessionOutcome outcome;
    outcome.kind = P50CacheSessionOutcomeKind::Adopted;
    outcome.canonical_claim = encode_cache_session_wire_claim(claim);
    outcome.f_sidecar_launch = claim.binding.f_control_identity();
    outcome.f_store_guid = claim.binding.f_store_guid;
    outcome.operation = {outcome.f_sidecar_launch,
                         P50SessionOperationRole::FSession, 91};
    CHECK(outcome.valid());
    return outcome;
}

constexpr AbsoluteMonotonicDeadline kDeadline{1000, 3, 9};

struct SocketPair {
    int retained = -1;
    int peer = -1;

    SocketPair() {
        int descriptors[2]{-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
                           descriptors) == 0);
        retained = descriptors[0];
        peer = descriptors[1];
    }

    ~SocketPair() {
        if (retained >= 0)
            (void)::close(retained);
        if (peer >= 0)
            (void)::close(peer);
    }
};

void exact_send_preserves_flags_and_binding() {
    SocketPair pair;
    const int original_flags = ::fcntl(pair.retained, F_GETFL);
    CHECK(original_flags >= 0);
    const int flag_probe = ::dup(pair.retained);
    CHECK(flag_probe >= 0);
    const auto outcome = adopted();
    P5coRetainedSocketLease lease(HandoffFd(pair.retained), outcome,
                                  kDeadline);
    pair.retained = -1;

    CHECK(lease.revalidate(outcome, kDeadline));
    auto wrong_outcome = outcome;
    ++wrong_outcome.operation.operation_sequence;
    CHECK(!lease.revalidate(wrong_outcome, kDeadline));
    CHECK(!lease.revalidate(
        outcome, AbsoluteMonotonicDeadline{1001, 3, 9}));

    const std::array<uint8_t, 4> bytes{0x50, 0x35, 0x43, 0x4f};
    const P5coWriteResult wrong_flags = lease.send_nonblocking(
        bytes, static_cast<uint8_t>(P5coSendFlag::DontWait));
    CHECK(wrong_flags.kind == P5coWriteKind::Error &&
          wrong_flags.bytes == 0);
    const P5coWriteResult result = lease.send_nonblocking(
        bytes, P5coSendFlag::DontWait | P5coSendFlag::NoSignal);
    CHECK(result.kind == P5coWriteKind::Sent &&
          result.bytes == bytes.size());

    std::array<uint8_t, 4> received{};
    CHECK(::recv(pair.peer, received.data(), received.size(), 0) ==
          static_cast<ssize_t>(received.size()));
    CHECK(received == bytes);

    CHECK(::fcntl(flag_probe, F_GETFL) == original_flags);
    (void)::close(flag_probe);

    lease.fence();
    CHECK(!lease.revalidate(outcome, kDeadline));
    uint8_t byte = 0;
    CHECK(::recv(pair.peer, &byte, 1, 0) == 0);
}

void closed_peer_is_operation_failure_not_sigpipe() {
    SocketPair pair;
    const auto outcome = adopted();
    P5coRetainedSocketLease lease(HandoffFd(pair.retained), outcome,
                                  kDeadline);
    pair.retained = -1;
    (void)::close(pair.peer);
    pair.peer = -1;

    const std::array<uint8_t, 1> byte{0x50};
    const P5coWriteResult result = lease.send_nonblocking(
        byte, P5coSendFlag::DontWait | P5coSendFlag::NoSignal);
    CHECK(result.kind == P5coWriteKind::Error && result.bytes == 0);
    CHECK(lease.revalidate(outcome, kDeadline));
    lease.fence();
}

void full_send_buffer_returns_without_blocking() {
    SocketPair pair;
    const std::array<uint8_t, 4096> fill{};
    for (;;) {
        const ssize_t sent = ::send(pair.retained, fill.data(), fill.size(),
                                    MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent > 0)
            continue;
        CHECK(sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        break;
    }
    const auto outcome = adopted();
    P5coRetainedSocketLease lease(HandoffFd(pair.retained), outcome,
                                  kDeadline);
    pair.retained = -1;
    const std::array<uint8_t, 1> byte{0x50};
    const P5coWriteResult result = lease.send_nonblocking(
        byte, P5coSendFlag::DontWait | P5coSendFlag::NoSignal);
    CHECK(result.kind == P5coWriteKind::WouldBlock && result.bytes == 0);
    lease.fence();
}

void post_release_fence_records_without_closing_transferred_fd() {
    SocketPair pair;
    const auto outcome = adopted();
    P5coRetainedSocketLease lease(HandoffFd(pair.retained), outcome,
                                  kDeadline);
    pair.retained = -1;

    const int transferred =
        lease.release_native_fd_for_test(outcome, kDeadline);
    CHECK(transferred >= 0);
    CHECK(::fcntl(transferred, F_GETFD) >= 0);
    lease.fence();
    CHECK(lease.fenced_for_test());
    CHECK(!lease.revalidate(outcome, kDeadline));
    // Fencing after transfer is an operation-level fact; descriptor ownership
    // has already moved to the endpoint and must not be closed behind it.
    CHECK(::fcntl(transferred, F_GETFD) >= 0);
    lease.fence();
    CHECK(::fcntl(transferred, F_GETFD) >= 0);
    (void)::close(transferred);
}

} // namespace

int main() {
    exact_send_preserves_flags_and_binding();
    closed_peer_is_operation_failure_not_sigpipe();
    full_send_buffer_returns_without_blocking();
    post_release_fence_records_without_closing_transferred_fd();
    return 0;
}
