#include "../cache/p50_fd_handoff.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace icecc::p50::local;

static int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "FAILED: %s\n", #condition); ++failures; } \
} while (0)

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static void put64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i != 8; ++i) p[i] = v >> (56 - 8 * i);
}

static std::array<uint8_t, 40> request_wire(const HandoffRequest &request) {
    std::array<uint8_t, 40> wire{};
    std::memcpy(wire.data(), "P50F", 4);
    put16(wire.data() + 4, 1); put16(wire.data() + 6, 1); put32(wire.data() + 8, 40);
    put64(wire.data() + 12, request.identity.generation);
    put64(wire.data() + 20, request.identity.attempt);
    put64(wire.data() + 28, request.request_id);
    return wire;
}

static void send_request(int fd, const HandoffRequest &request, int attached) {
    auto wire = request_wire(request);
    alignas(struct cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int))> control{};
    struct iovec iov{wire.data(), wire.size()};
    struct msghdr message{};
    message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    auto *cmsg = reinterpret_cast<struct cmsghdr *>(control.data());
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &attached, sizeof(attached));
    CHECK(::sendmsg(fd, &message, 0) == static_cast<ssize_t>(wire.size()));
}

static void send_wire_without_fd(int fd, const HandoffRequest &request) {
    const auto wire = request_wire(request);
    CHECK(::send(fd, wire.data(), wire.size(), 0) == static_cast<ssize_t>(wire.size()));
}

static void send_request_with_two_fds(int fd, const HandoffRequest &request,
                                      int first, int second) {
    const auto wire = request_wire(request);
    alignas(struct cmsghdr) std::array<uint8_t, CMSG_SPACE(2 * sizeof(int))> control{};
    struct iovec iov{const_cast<uint8_t *>(wire.data()), wire.size()};
    struct msghdr message{};
    message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    auto *cmsg = reinterpret_cast<struct cmsghdr *>(control.data());
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(2 * sizeof(int));
    int fds[2] = {first, second};
    std::memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
    CHECK(::sendmsg(fd, &message, 0) == static_cast<ssize_t>(wire.size()));
}

static void authenticate(Connection &connection) {
    const auto peer = query_peer_credentials(connection.native_handle());
    CHECK(peer.has_value());
    if (peer.has_value())
        CHECK(connection.verify_peer_credentials(
                   CredentialExpectation{peer->uid, peer->gid, peer->pid}) == Status::Ok);
}

static void send_request_prefix(int fd, const HandoffRequest &request, int attached,
                                size_t bytes) {
    const auto wire = request_wire(request);
    alignas(struct cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int))> control{};
    struct iovec iov{const_cast<uint8_t *>(wire.data()), bytes};
    struct msghdr message{};
    message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    auto *cmsg = reinterpret_cast<struct cmsghdr *>(control.data());
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &attached, sizeof(attached));
    CHECK(::sendmsg(fd, &message, 0) == static_cast<ssize_t>(bytes));
}

int main() {
    int pair[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const HandoffRequest request{{7, 9}, 11};
    Connection connection(pair[0]);
    const auto peer = query_peer_credentials(connection.native_handle());
    CHECK(peer.has_value());
    CredentialExpectation credentials{peer->uid, peer->gid, peer->pid};
    CHECK(connection.verify_peer_credentials(credentials) == Status::Ok);
    AsyncFdHandoffReceiver receiver;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    CHECK(receiver.start(connection, request, deadline));
    CHECK(!receiver.start(connection, request, deadline));
    const int source = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(source >= 0);
    send_request(pair[1], request, source);
    receiver.advance(POLLIN);
    CHECK(!receiver.done() && (receiver.poll_events() & POLLOUT) != 0);
    CHECK(!receiver.take_fd().valid());
    receiver.advance(POLLOUT);
    CHECK(receiver.done());
    CHECK(receiver.result().status == FdHandoffStatus::Accepted);
    HandoffFd adopted = receiver.take_fd();
    CHECK(adopted.valid() && adopted.cloexec());
    ::close(source);
    ::close(pair[1]);

    int mismatch_pair[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, mismatch_pair) == 0);
    Connection mismatch_connection(mismatch_pair[0]);
    authenticate(mismatch_connection);
    AsyncFdHandoffReceiver mismatch_receiver;
    CHECK(mismatch_receiver.start(mismatch_connection, request,
                                  std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    const HandoffRequest other{{7, 9}, 12};
    const int mismatch_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(mismatch_fd >= 0);
    send_request(mismatch_pair[1], other, mismatch_fd);
    mismatch_receiver.advance(POLLIN);
    mismatch_receiver.advance(POLLOUT);
    CHECK(mismatch_receiver.done() &&
          mismatch_receiver.result().status == FdHandoffStatus::RequestMismatch);
    CHECK(!mismatch_receiver.take_fd().valid());
    ::close(mismatch_fd); ::close(mismatch_pair[1]);

    int missing_pair[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, missing_pair) == 0);
    Connection missing_connection(missing_pair[0]);
    authenticate(missing_connection);
    AsyncFdHandoffReceiver missing_receiver;
    CHECK(missing_receiver.start(missing_connection, request,
                                 std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    send_wire_without_fd(missing_pair[1], request);
    missing_receiver.advance(POLLIN); missing_receiver.advance(POLLOUT);
    CHECK(missing_receiver.done() &&
          missing_receiver.result().status == FdHandoffStatus::MissingFd);
    ::close(missing_pair[1]);

    int extra_pair[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, extra_pair) == 0);
    Connection extra_connection(extra_pair[0]);
    authenticate(extra_connection);
    AsyncFdHandoffReceiver extra_receiver;
    CHECK(extra_receiver.start(extra_connection, request,
                               std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    const int extra_fd1 = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    const int extra_fd2 = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(extra_fd1 >= 0 && extra_fd2 >= 0);
    send_request_with_two_fds(extra_pair[1], request, extra_fd1, extra_fd2);
    extra_receiver.advance(POLLIN); extra_receiver.advance(POLLOUT);
    CHECK(extra_receiver.done() &&
          extra_receiver.result().status == FdHandoffStatus::ExtraFd);
    ::close(extra_fd1); ::close(extra_fd2); ::close(extra_pair[1]);

    int fragmented_pair[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fragmented_pair) == 0);
    Connection fragmented_connection(fragmented_pair[0]);
    authenticate(fragmented_connection);
    AsyncFdHandoffReceiver fragmented_receiver;
    CHECK(fragmented_receiver.start(fragmented_connection, request,
                                    std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    const int fragmented_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(fragmented_fd >= 0);
    const auto fragmented_wire = request_wire(request);
    send_request_prefix(fragmented_pair[1], request, fragmented_fd, 8);
    fragmented_receiver.advance(POLLIN);
    CHECK(!fragmented_receiver.done());
    CHECK(::send(fragmented_pair[1], fragmented_wire.data() + 8,
                 fragmented_wire.size() - 8, 0) ==
          static_cast<ssize_t>(fragmented_wire.size() - 8));
    fragmented_receiver.cancel();
    CHECK(fragmented_receiver.done() &&
          fragmented_receiver.result().status == FdHandoffStatus::Disconnected);
    CHECK(!fragmented_receiver.take_fd().valid());
    ::close(fragmented_fd); ::close(fragmented_pair[1]);

    int unauth_pair[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, unauth_pair) == 0);
    Connection unauth(unauth_pair[0]);
    AsyncFdHandoffReceiver unauth_receiver;
    CHECK(!unauth_receiver.start(unauth, request,
                                  std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    CHECK(unauth_receiver.result().status == FdHandoffStatus::NotAuthenticated);
    ::close(unauth_pair[1]);

    int expired_pair[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, expired_pair) == 0);
    Connection expired_connection(expired_pair[0]);
    const auto expired_peer = query_peer_credentials(expired_connection.native_handle());
    CHECK(expired_peer.has_value());
    CHECK(expired_connection.verify_peer_credentials(
               CredentialExpectation{expired_peer->uid, expired_peer->gid, expired_peer->pid}) == Status::Ok);
    AsyncFdHandoffReceiver expired;
    CHECK(expired.start(expired_connection, request,
                         std::chrono::steady_clock::now() - std::chrono::milliseconds(1)));
    expired.advance(POLLIN);
    CHECK(expired.done() && expired.result().status == FdHandoffStatus::Timeout);
    ::close(expired_pair[1]);
    return failures == 0 ? 0 : 1;
}
