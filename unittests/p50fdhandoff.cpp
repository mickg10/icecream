#include "../cache/p50_fd_handoff.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
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

constexpr int no_signal_flag() noexcept {
#if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

size_t open_fd_count() {
    DIR* directory = ::opendir("/proc/self/fd");
    CHECK(directory != nullptr);
    const int directory_fd = ::dirfd(directory);
    size_t count = 0;
    while (const struct dirent* entry = ::readdir(directory)) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        char* end = nullptr;
        const long fd = std::strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0' && fd >= 0 && fd != directory_fd)
            ++count;
    }
    CHECK(::closedir(directory) == 0);
    return count;
}

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
        const ssize_t count = ::send(fd, data + offset, size - offset, no_signal_flag());
        if (count > 0)
            offset += static_cast<size_t>(count);
        else if (count < 0 && errno == EINTR)
            continue;
        else
            return false;
    }
    return true;
}

bool saturate_socket(int fd) {
    int send_buffer = 1024;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) != 0)
        return false;
    const int original_flags = ::fcntl(fd, F_GETFL);
    if (original_flags < 0 || ::fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0)
        return false;
    std::array<uint8_t, 64 * 1024> bytes{};
    bool saturated = false;
    for (;;) {
        const ssize_t count = ::send(fd, bytes.data(), bytes.size(), no_signal_flag());
        if (count > 0)
            continue;
        if (count < 0 && errno == EINTR)
            continue;
        saturated = count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        break;
    }
    const bool restored = ::fcntl(fd, F_SETFL, original_flags) == 0;
    return saturated && restored;
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
    int connection_fd = -1;
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
    return Pair{fds[0], fds[1], Connection(fds[1])};
}

void send_bytes_with_fds(int socket, const uint8_t* bytes, size_t byte_count,
                         const int* fds, size_t fd_count) {
    alignas(struct cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int) * 3)> control{};
    struct iovec iov{const_cast<uint8_t*>(bytes), byte_count};
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
    CHECK(::sendmsg(socket, &message, no_signal_flag()) ==
          static_cast<ssize_t>(byte_count));
}

void send_with_one_fd(int socket, const std::array<uint8_t, kWireSize>& bytes,
                      const int* fds, size_t fd_count) {
    send_bytes_with_fds(socket, bytes.data(), bytes.size(), fds, fd_count);
}

int receive_request_fd(int socket, std::array<uint8_t, kWireSize>& bytes) {
    alignas(struct cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int))> control{};
    struct iovec iov{bytes.data(), bytes.size()};
    struct msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    CHECK(::recvmsg(socket, &message, 0) == static_cast<ssize_t>(bytes.size()));
    CHECK((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
    CHECK(cmsg != nullptr);
    CHECK(cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS);
    CHECK(cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    CHECK(fd >= 0);
    CHECK(CMSG_NXTHDR(&message, cmsg) == nullptr);
    return fd;
}

void send_unexpected_control(int socket, const std::array<uint8_t, kWireSize>& bytes) {
#if defined(SCM_CREDENTIALS)
    alignas(struct cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(struct ucred))> control{};
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
    CHECK(::sendmsg(socket, &message, no_signal_flag()) ==
          static_cast<ssize_t>(bytes.size()));
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

void test_fragmented_and_overlong_request() {
    const HandoffRequest expected = request();
    {
        Pair pair = raw_pair();
        authenticate(pair.connection);
        const auto bytes = wire(1, expected);
        const int fd = ::open("/dev/null", O_RDONLY);
        CHECK(fd >= 0);
        send_bytes_with_fds(pair.raw, bytes.data(), 20, &fd, 1);
        bool tail_ok = false;
        std::thread tail([&] {
            ::usleep(20000);
            tail_ok = send_all(pair.raw, bytes.data() + 20, bytes.size() - 20);
        });
        FdHandoffReceiver receiver;
        const FdHandoffResult result = receiver.receive_and_ack(
            pair.connection, expected,
            std::chrono::steady_clock::now() + std::chrono::seconds(2));
        tail.join();
        CHECK(tail_ok);
        CHECK(result.status == FdHandoffStatus::Accepted);
        CHECK(receiver.adopted());
        HandoffFd adopted = receiver.take_adopted_fd();
        CHECK(adopted.valid() && adopted.cloexec());
        CHECK(::close(fd) == 0);
        CHECK(::close(pair.raw) == 0);
    }
    {
        Pair pair = raw_pair();
        authenticate(pair.connection);
        const auto record = wire(1, expected);
        std::array<uint8_t, kWireSize + 1> overlong{};
        std::copy(record.begin(), record.end(), overlong.begin());
        overlong.back() = 0xa5;
        const int fd = ::open("/dev/null", O_RDONLY);
        CHECK(fd >= 0);
        send_bytes_with_fds(pair.raw, overlong.data(), overlong.size(), &fd, 1);
        FdHandoffReceiver receiver;
        const FdHandoffResult result = receiver.receive_and_ack(
            pair.connection, expected,
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        CHECK(result.status == FdHandoffStatus::TrailingData);
        CHECK(!receiver.adopted());
        CHECK(::close(fd) == 0);
        CHECK(::close(pair.raw) == 0);
    }
}

void test_fragmented_rights_and_error_cleanup() {
    const HandoffRequest expected = request();
    const size_t baseline = open_fd_count();
    {
        Pair pair = raw_pair();
        authenticate(pair.connection);
        const auto bytes = wire(1, expected);
        const int fd = ::open("/dev/null", O_RDONLY);
        CHECK(fd >= 0);
        CHECK(send_all(pair.raw, bytes.data(), 13));
        send_bytes_with_fds(pair.raw, bytes.data() + 13, bytes.size() - 13, &fd, 1);
        FdHandoffReceiver receiver;
        const FdHandoffResult result = receiver.receive_and_ack(
            pair.connection, expected,
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        CHECK(result.status == FdHandoffStatus::Accepted);
        HandoffFd adopted = receiver.take_adopted_fd();
        CHECK(adopted.valid() && adopted.cloexec());
        CHECK(::close(fd) == 0);
        CHECK(::close(pair.raw) == 0);
    }
    CHECK(open_fd_count() == baseline);

    {
        Pair pair = raw_pair();
        authenticate(pair.connection);
        const auto bytes = wire(1, expected);
        const int fd = ::open("/dev/null", O_RDONLY);
        CHECK(fd >= 0);
        send_bytes_with_fds(pair.raw, bytes.data(), 20, &fd, 1);
        CHECK(::shutdown(pair.raw, SHUT_WR) == 0);
        FdHandoffReceiver receiver;
        const FdHandoffResult result = receiver.receive_and_ack(
            pair.connection, expected,
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        CHECK(result.status == FdHandoffStatus::Truncated);
        CHECK(!receiver.adopted());
        CHECK(::close(fd) == 0);
        CHECK(::close(pair.raw) == 0);
    }
    CHECK(open_fd_count() == baseline);

    {
        Pair pair = raw_pair();
        authenticate(pair.connection);
        const auto bytes = wire(1, expected);
        const int first = ::open("/dev/null", O_RDONLY);
        const int second = ::open("/dev/null", O_RDONLY);
        CHECK(first >= 0 && second >= 0);
        send_bytes_with_fds(pair.raw, bytes.data(), 20, &first, 1);
        send_bytes_with_fds(pair.raw, bytes.data() + 20, bytes.size() - 20, &second, 1);
        FdHandoffReceiver receiver;
        const FdHandoffResult result = receiver.receive_and_ack(
            pair.connection, expected,
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        CHECK(result.status == FdHandoffStatus::ExtraFd);
        CHECK(!receiver.adopted());
        CHECK(::close(first) == 0);
        CHECK(::close(second) == 0);
        CHECK(::close(pair.raw) == 0);
    }
    CHECK(open_fd_count() == baseline);
}

void test_fragmented_and_overlong_ack() {
    const HandoffRequest expected = request();
    for (const bool overlong : {false, true}) {
        int sockets[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
        Connection sender_connection(sockets[0]);
        authenticate(sender_connection);
        FdHandoffSender sender{HandoffFd(::open("/dev/null", O_RDONLY))};
        FdHandoffResult sender_result;
        std::thread sender_thread([&] {
            sender_result = sender.send(
                sender_connection, expected,
                std::chrono::steady_clock::now() + std::chrono::seconds(2));
        });
        std::array<uint8_t, kWireSize> request_bytes{};
        const int received_fd = receive_request_fd(sockets[1], request_bytes);
        CHECK(request_bytes == wire(1, expected));
        CHECK(::close(received_fd) == 0);
        const auto ack = wire(2, expected, 1);
        if (overlong) {
            std::array<uint8_t, kWireSize + 1> bytes{};
            std::copy(ack.begin(), ack.end(), bytes.begin());
            bytes.back() = 0x5a;
            CHECK(send_all(sockets[1], bytes.data(), bytes.size()));
        } else {
            CHECK(send_all(sockets[1], ack.data(), 19));
            ::usleep(20000);
            CHECK(send_all(sockets[1], ack.data() + 19, ack.size() - 19));
        }
        sender_thread.join();
        CHECK(sender_result.status ==
              (overlong ? FdHandoffStatus::TrailingData : FdHandoffStatus::Accepted));
        CHECK(!sender.owns_fd());
        CHECK(::close(sockets[1]) == 0);
    }
}

void test_queued_request_survives_half_close() {
    Pair pair = raw_pair();
    authenticate(pair.connection);
    const HandoffRequest expected = request();
    const auto bytes = wire(1, expected);
    const int fd = ::open("/dev/null", O_RDONLY);
    CHECK(fd >= 0);
    send_with_one_fd(pair.raw, bytes, &fd, 1);
    CHECK(::shutdown(pair.raw, SHUT_WR) == 0);
    FdHandoffReceiver receiver;
    const FdHandoffResult result = receiver.receive_and_ack(
        pair.connection, expected,
        std::chrono::steady_clock::now() + std::chrono::seconds(1));
    CHECK(result.status == FdHandoffStatus::Accepted);
    CHECK(receiver.adopted());
    std::array<uint8_t, kWireSize> ack{};
    CHECK(::recv(pair.raw, ack.data(), ack.size(), MSG_WAITALL) ==
          static_cast<ssize_t>(ack.size()));
    CHECK(ack == wire(2, expected, 1));
    CHECK(::close(fd) == 0);
    CHECK(::close(pair.raw) == 0);
}

#if defined(ICECC_P50_FD_HANDOFF_TEST_HOOKS)
void test_forced_positive_short_writes() {
    fd_handoff_test_set_max_send_chunk(7);
    test_happy_move_once();
    fd_handoff_test_set_max_send_chunk(0);
}
#endif

void test_submillisecond_absolute_deadline() {
    int fds[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(saturate_socket(fds[0]));
    Connection sender_connection(fds[0]);
    Connection peer(fds[1]);
    authenticate(sender_connection);
    FdHandoffSender sender{HandoffFd(::open("/dev/null", O_RDONLY))};
    const auto started = std::chrono::steady_clock::now();
    const FdHandoffResult result = sender.send(
        sender_connection, request(),
        started + std::chrono::microseconds(100));
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    CHECK(result.status == FdHandoffStatus::Timeout);
    CHECK(result.sender_state == FdHandoffSenderState::TimedOut);
    CHECK(elapsed < 1000);
}

void test_terminal_poll_bits_preserve_queued_reads() {
    {
        Pair pair = raw_pair();
        authenticate(pair.connection);
        const HandoffRequest expected = request();
        const auto bytes = wire(1, expected);
        const int fd = ::open("/dev/null", O_RDONLY);
        CHECK(fd >= 0);
        send_with_one_fd(pair.raw, bytes, &fd, 1);
        CHECK(::close(pair.raw) == 0);
        pair.raw = -1;
        struct pollfd descriptor{pair.connection_fd, POLLIN, 0};
        CHECK(::poll(&descriptor, 1, 1000) == 1);
        CHECK((descriptor.revents & (POLLIN | POLLHUP)) == (POLLIN | POLLHUP));
        FdHandoffReceiver receiver;
        const FdHandoffResult result = receiver.receive_and_ack(
            pair.connection, expected,
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        CHECK(result.status == FdHandoffStatus::Disconnected);
        CHECK(!receiver.adopted());
        CHECK(::close(fd) == 0);
    }

    {
        int fds[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        const auto ack = wire(2, request(), 1);
        CHECK(send_all(fds[1], ack.data(), ack.size()));
        CHECK(::close(fds[1]) == 0);
        struct pollfd descriptor{fds[0], POLLIN, 0};
        CHECK(::poll(&descriptor, 1, 1000) == 1);
        CHECK((descriptor.revents & (POLLIN | POLLHUP)) == (POLLIN | POLLHUP));
        CHECK(detail::wait_for_io(
                  fds[0], POLLIN,
                  std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
              detail::DeadlinePollResult::Ready);
        std::array<uint8_t, kWireSize> received{};
        CHECK(::recv(fds[0], received.data(), received.size(), MSG_WAITALL) ==
              static_cast<ssize_t>(received.size()));
        CHECK(received == ack);
        CHECK(::close(fds[0]) == 0);
    }

    {
        int fds[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        Connection sender_connection(fds[0]);
        Connection peer(fds[1]);
        authenticate(sender_connection);
        peer = Connection(-1);
        struct pollfd descriptor{fds[0], POLLOUT, 0};
        CHECK(::poll(&descriptor, 1, 1000) == 1);
        CHECK((descriptor.revents & (POLLOUT | POLLHUP)) == (POLLOUT | POLLHUP));
        FdHandoffSender sender{HandoffFd(::open("/dev/null", O_RDONLY))};
        const FdHandoffResult result = sender.send(
            sender_connection, request(),
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        CHECK(result.status == FdHandoffStatus::Disconnected);
        CHECK(!sender.owns_fd());
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
    test_fragmented_and_overlong_request();
    test_fragmented_rights_and_error_cleanup();
    test_fragmented_and_overlong_ack();
    test_queued_request_survives_half_close();
#if defined(ICECC_P50_FD_HANDOFF_TEST_HOOKS)
    test_forced_positive_short_writes();
#endif
    test_submillisecond_absolute_deadline();
    test_terminal_poll_bits_preserve_queued_reads();
    test_timeout_disconnect_auth();
    return 0;
}
