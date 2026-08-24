#include "../cache/p50_fd_handoff.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace icecc::p50::local;

namespace {

void check(bool condition, const char* expression) {
    if (!condition)
        throw std::runtime_error(expression);
}

#define CHECK(expression) check((expression), #expression)

constexpr size_t kWireSize = 40;

HandoffRequest request(uint64_t generation = 7, uint64_t attempt = 11,
                       uint64_t request_id = 13) {
    return HandoffRequest{Identity{generation, attempt}, request_id};
}

std::array<uint8_t, kWireSize> wire(uint16_t type, HandoffRequest value,
                                    uint32_t code = 0) {
    std::array<uint8_t, kWireSize> bytes{};
    bytes[0] = 'P'; bytes[1] = '5'; bytes[2] = '0'; bytes[3] = 'F';
    bytes[4] = 0; bytes[5] = 1;
    bytes[6] = static_cast<uint8_t>(type >> 8); bytes[7] = static_cast<uint8_t>(type);
    bytes[8] = 0; bytes[9] = 0; bytes[10] = 0; bytes[11] = kWireSize;
    auto put = [&bytes](size_t offset, uint64_t value) {
        for (size_t i = 0; i != 8; ++i)
            bytes[offset + i] = static_cast<uint8_t>(value >> (56 - 8 * i));
    };
    put(12, value.identity.generation);
    put(20, value.identity.attempt);
    put(28, value.request_id);
    bytes[36] = static_cast<uint8_t>(code >> 24);
    bytes[37] = static_cast<uint8_t>(code >> 16);
    bytes[38] = static_cast<uint8_t>(code >> 8);
    bytes[39] = static_cast<uint8_t>(code);
    return bytes;
}

bool send_all(int fd, const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset != size) {
        const ssize_t count = ::send(fd, data + offset, size - offset,
#if defined(MSG_NOSIGNAL)
                                     MSG_NOSIGNAL
#else
                                     0
#endif
        );
        if (count > 0)
            offset += static_cast<size_t>(count);
        else if (count < 0 && errno == EINTR)
            continue;
        else
            return false;
    }
    return true;
}

void authenticate(Connection& connection) {
    const CredentialExpectation expected{static_cast<uint64_t>(::getuid()),
                                         static_cast<uint64_t>(::getgid()),
                                         static_cast<uint64_t>(::getpid())};
    CHECK(connection.verify_peer_credentials(expected) == Status::Ok);
    CHECK(connection.peer_credentials_verified());
}

struct Pair {
    int raw = -1;
    Connection connection;
};

Pair raw_pair(bool pass_credentials = false) {
    int fds[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    if (pass_credentials) {
#if defined(SO_PASSCRED)
        int enabled = 1;
        CHECK(::setsockopt(fds[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
#endif
    }
    return Pair{fds[0], Connection(fds[1])};
}

void send_with_one_fd(int socket, const std::array<uint8_t, kWireSize>& bytes,
                      const int* fds, size_t fd_count) {
    std::array<uint8_t, CMSG_SPACE(sizeof(int) * 3)> control{};
    struct iovec iov{const_cast<uint8_t*>(bytes.data()), bytes.size()};
    struct msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (fd_count != 0) {
        message.msg_control = control.data();
        message.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
        auto* cmsg = reinterpret_cast<struct cmsghdr*>(control.data());
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
        std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
    }
    CHECK(::sendmsg(socket, &message,
#if defined(MSG_NOSIGNAL)
                    MSG_NOSIGNAL
#else
                    0
#endif
                    ) == static_cast<ssize_t>(bytes.size()));
}

void send_unexpected_control(int socket, const std::array<uint8_t, kWireSize>& bytes) {
#if defined(SCM_CREDENTIALS)
    std::array<uint8_t, CMSG_SPACE(sizeof(struct ucred))> control{};
    struct iovec iov{const_cast<uint8_t*>(bytes.data()), bytes.size()};
    struct msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    auto* cmsg = reinterpret_cast<struct cmsghdr*>(control.data());
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
    const struct ucred credentials{::getpid(), ::getuid(), ::getgid()};
    std::memcpy(CMSG_DATA(cmsg), &credentials, sizeof(credentials));
    CHECK(::sendmsg(socket, &message,
#if defined(MSG_NOSIGNAL)
                    MSG_NOSIGNAL
#else
                    0
#endif
                    ) == static_cast<ssize_t>(bytes.size()));
#else
    (void)socket;
    (void)bytes;
#endif
}

void test_happy_move_once() {
    int fds[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    Connection sender_connection(fds[0]);
    Connection receiver_connection(fds[1]);
    authenticate(sender_connection);
    authenticate(receiver_connection);
    const HandoffRequest expected = request();
    const int payload = ::open("/dev/null", O_RDONLY);
    CHECK(payload >= 0);
    FdHandoffSender sender{HandoffFd(payload)};
    FdHandoffReceiver receiver;
    FdHandoffResult sender_result;
    std::thread sender_thread([&] {
        sender_result = sender.send(sender_connection, expected,
                                     std::chrono::steady_clock::now() + std::chrono::seconds(2));
    });
    const FdHandoffResult receiver_result = receiver.receive_and_ack(
        receiver_connection, expected, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    sender_thread.join();
    CHECK(receiver_result.status == FdHandoffStatus::Accepted);
    CHECK(sender_result.status == FdHandoffStatus::Accepted);
    CHECK(sender_result.sender_state == FdHandoffSenderState::Acked);
    CHECK(!sender.owns_fd());
    CHECK(receiver.consumed() && receiver.adopted());
    HandoffFd adopted = receiver.take_adopted_fd();
    CHECK(adopted.valid() && adopted.cloexec());
    CHECK(!receiver.adopted());
    CHECK(sender.send(sender_connection, expected, std::chrono::steady_clock::now())
          .status == FdHandoffStatus::AlreadySent);
}

void test_nack_identity_and_duplicate() {
    int fds[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    Connection sender_connection(fds[0]);
    Connection receiver_connection(fds[1]);
    authenticate(sender_connection);
    authenticate(receiver_connection);
    const HandoffRequest expected = request();
    const HandoffRequest stale = request(8);
    FdHandoffSender sender{HandoffFd(::open("/dev/null", O_RDONLY))};
    FdHandoffReceiver receiver;
    FdHandoffResult sr;
    std::thread thread([&] {
        sr = sender.send(sender_connection, stale,
                         std::chrono::steady_clock::now() + std::chrono::seconds(2));
    });
    const FdHandoffResult rr = receiver.receive_and_ack(
        receiver_connection, expected, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    thread.join();
    CHECK(rr.status == FdHandoffStatus::StaleGeneration);
    CHECK(sr.status == FdHandoffStatus::StaleGeneration);
    CHECK(sr.sender_state == FdHandoffSenderState::Nacked && !sender.owns_fd());
}

void test_duplicate_replay() {
    int fds[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    Connection sender_connection(fds[0]);
    Connection receiver_connection(fds[1]);
    authenticate(sender_connection);
    authenticate(receiver_connection);
    const HandoffRequest expected = request();
    FdHandoffReceiver receiver;
    FdHandoffSender first{HandoffFd(::open("/dev/null", O_RDONLY))};
    FdHandoffResult first_result;
    std::thread first_thread([&] {
        first_result = first.send(sender_connection, expected,
                                  std::chrono::steady_clock::now() + std::chrono::seconds(2));
    });
    CHECK(receiver.receive_and_ack(receiver_connection, expected,
                                   std::chrono::steady_clock::now() + std::chrono::seconds(2)).status ==
          FdHandoffStatus::Accepted);
    first_thread.join();
    CHECK(first_result.status == FdHandoffStatus::Accepted);
    FdHandoffSender replay{HandoffFd(::open("/dev/null", O_RDONLY))};
    FdHandoffResult replay_result;
    std::thread replay_thread([&] {
        replay_result = replay.send(sender_connection, expected,
                                     std::chrono::steady_clock::now() + std::chrono::seconds(2));
    });
    const FdHandoffResult duplicate = receiver.receive_and_ack(
        receiver_connection, expected, std::chrono::steady_clock::now() + std::chrono::seconds(2));
    replay_thread.join();
    CHECK(duplicate.status == FdHandoffStatus::AlreadyConsumed ||
          duplicate.status == FdHandoffStatus::DuplicateRequest);
    CHECK(replay_result.status == FdHandoffStatus::Nack);
    CHECK(!replay.owns_fd() && receiver.adopted());
}

void test_raw_rejections() {
    const HandoffRequest expected = request();
    for (int kind = 0; kind != 6; ++kind) {
        Pair pair = raw_pair(kind == 5);
        authenticate(pair.connection);
        int fd = ::open("/dev/null", O_RDONLY);
        CHECK(fd >= 0);
        auto bytes = wire(1, expected);
        if (kind == 3)
            bytes[11] = 41;
        if (kind == 2)
            bytes[7] = 9;
        if (kind == 0)
            send_all(pair.raw, bytes.data(), bytes.size());
        else if (kind == 1) {
            int extras[2] = {fd, ::dup(fd)};
            CHECK(extras[1] >= 0);
            send_with_one_fd(pair.raw, bytes, extras, 2);
            ::close(extras[1]);
        } else if (kind == 4) {
            int extras[3] = {fd, ::dup(fd), ::dup(fd)};
            CHECK(extras[1] >= 0 && extras[2] >= 0);
            send_with_one_fd(pair.raw, bytes, extras, 3);
            ::close(extras[1]);
            ::close(extras[2]);
        } else if (kind == 5) {
            send_unexpected_control(pair.raw, bytes);
        } else
            send_with_one_fd(pair.raw, bytes, &fd, 1);
        const FdHandoffStatus status = pair.connection
            .valid() ? FdHandoffReceiver{}.receive_and_ack(
                pair.connection, expected,
                std::chrono::steady_clock::now() + std::chrono::seconds(1)).status
                     : FdHandoffStatus::IoError;
        if (kind == 0)
            CHECK(status == FdHandoffStatus::MissingFd);
        else if (kind == 1)
            CHECK(status == FdHandoffStatus::ExtraFd);
        else if (kind == 2)
            CHECK(status == FdHandoffStatus::Malformed);
        else if (kind == 4)
            CHECK(status == FdHandoffStatus::ControlTruncated || status == FdHandoffStatus::ExtraFd);
        else if (kind == 5) {
            if (status != FdHandoffStatus::UnexpectedControl) {
                std::fprintf(stderr, "unexpected control status: %s\\n",
                             fd_handoff_status_name(status));
                CHECK(false);
            }
        }
        else
            CHECK(status == FdHandoffStatus::MessageTruncated || status == FdHandoffStatus::Malformed);
        ::close(fd);
        ::close(pair.raw);
    }
}

void test_timeout_disconnect_auth() {
    int fds[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    Connection sender_connection(fds[0]);
    Connection peer(fds[1]);
    authenticate(sender_connection);
    FdHandoffSender sender(HandoffFd(::open("/dev/null", O_RDONLY)));
    const FdHandoffResult timeout = sender.send(
        sender_connection, request(), std::chrono::steady_clock::now() + std::chrono::milliseconds(30));
    CHECK(timeout.status == FdHandoffStatus::Timeout);
    CHECK(timeout.sender_state == FdHandoffSenderState::TimedOut && !sender.owns_fd());
    peer = Connection(-1);
    const int fd = ::open("/dev/null", O_RDONLY);
    FdHandoffSender disconnected{HandoffFd(fd)};
    const FdHandoffResult result = disconnected.send(
        sender_connection, request(9), std::chrono::steady_clock::now() + std::chrono::seconds(1));
    CHECK(result.status == FdHandoffStatus::Disconnected || result.status == FdHandoffStatus::IoError);
    CHECK(!disconnected.owns_fd());
    int unauth_fds[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, unauth_fds) == 0);
    Connection unauth(unauth_fds[0]);
    FdHandoffSender unauth_sender{HandoffFd(::open("/dev/null", O_RDONLY))};
    CHECK(unauth_sender.send(unauth, request(), std::chrono::steady_clock::now()).status ==
          FdHandoffStatus::NotAuthenticated);
    ::close(unauth_fds[1]);
}

} // namespace

int main() {
    test_happy_move_once();
    test_nack_identity_and_duplicate();
    test_duplicate_replay();
    test_raw_rejections();
    test_timeout_disconnect_auth();
    return 0;
}
