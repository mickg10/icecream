#include "../cache/p50_daemon_control.h"

#include <chrono>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace icecc::p50::local;

namespace {
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
#define CHECK(x) check((x), #x)

ControlOperation operation() {
    ControlOperation value;
    value.identity = Identity{91, 17};
    value.request_id = 23;
    value.kind = ControlOperationKind::CacheSession;
    return value;
}

std::array<uint8_t, 40> handoff_wire(const ControlOperation& value) {
    std::array<uint8_t, 40> wire{};
    wire[0] = 'P'; wire[1] = '5'; wire[2] = '0'; wire[3] = 'F';
    wire[5] = 1; wire[7] = 1; wire[11] = 40;
    auto put64 = [&wire](size_t offset, uint64_t number) {
        for (size_t i = 0; i != 8; ++i)
            wire[offset + i] = static_cast<uint8_t>(number >> (56 - i * 8));
    };
    put64(12, value.identity.generation); put64(20, value.identity.attempt);
    put64(28, value.request_id);
    wire[39] = static_cast<uint8_t>(value.kind);
    return wire;
}

void nonblock(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    CHECK(flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

void write_frame(int fd, const Frame& frame) {
    Status status = Status::InvalidArgument;
    const auto bytes = encode_frame(frame, &status);
    CHECK(status == Status::Ok && !bytes.empty());
    size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t count = ::send(fd, bytes.data() + offset, bytes.size() - offset,
#ifdef MSG_NOSIGNAL
                                     MSG_NOSIGNAL
#else
                                     0
#endif
        );
        CHECK(count > 0);
        offset += static_cast<size_t>(count);
    }
}

void test_incremental_handoff_and_fairness() {
    int pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    const ControlOperation expected = operation();
    const int payload = ::open("/dev/null", O_RDONLY);
    CHECK(payload >= 0);
    nonblock(pair[0]);

    DaemonControlOperation sender;
    CHECK(sender.begin_connected(pair[0], expected, payload,
                                 std::chrono::steady_clock::now() + std::chrono::seconds(2),
                                 DaemonControlLimits{1, 7}) == DaemonControlStatus::InProgress);

    DaemonControlHandoffReceiver receiver;
    std::thread peer([&] {
        Frame hello;
        CHECK(read_frame(pair[1], hello) == Status::Ok);
        CHECK(hello.type == MessageType::Hello && hello.identity == expected.identity);
        write_frame(pair[1], make_hello_ack(PeerRole::Sidecar, expected.identity));
        Frame control;
        CHECK(read_frame(pair[1], control) == Status::Ok);
        ControlOperation decoded;
        CHECK(control.type == MessageType::Data && decode_control_operation(control.payload, decoded));
        CHECK(decoded.kind == expected.kind && decoded.identity == expected.identity &&
              decoded.request_id == expected.request_id);
        nonblock(pair[1]);
        CHECK(receiver.begin_connected(pair[1], expected,
                                       std::chrono::steady_clock::now() + std::chrono::seconds(2),
                                       DaemonControlLimits{2, 7}) == DaemonControlStatus::InProgress);
        while (!receiver.done()) {
            pollfd pfd{pair[1], receiver.desired_events(), 0};
            const int ready = ::poll(&pfd, 1, 1000);
            CHECK(ready == 1);
            receiver.advance(std::chrono::steady_clock::now(), pfd.revents);
        }
        CHECK(receiver.status() == DaemonControlStatus::Complete);
        const int adopted = receiver.take_fd();
        CHECK(adopted >= 0);
        ::close(adopted);
    });
    while (!sender.done()) {
        pollfd pfd{pair[0], sender.desired_events(), 0};
        const int ready = ::poll(&pfd, 1, 1000);
        CHECK(ready == 1);
        sender.advance(std::chrono::steady_clock::now(), pfd.revents);
    }
    peer.join();
    CHECK(sender.status() == DaemonControlStatus::Complete);
    CHECK(sender.rights_sent());
    ::close(pair[1]);
}

void test_extra_fd_is_closed_and_rejected() {
    int pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    nonblock(pair[1]);
    const ControlOperation expected = operation();
    DaemonControlHandoffReceiver receiver;
    CHECK(receiver.begin_connected(pair[1], expected,
                                   std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
          DaemonControlStatus::InProgress);
    const int first = ::open("/dev/null", O_RDONLY);
    const int second = ::open("/dev/null", O_RDONLY);
    CHECK(first >= 0 && second >= 0);
    const auto wire = handoff_wire(expected);
    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int) * 2)> control{};
    iovec iov{const_cast<uint8_t*>(wire.data()), wire.size()};
    msghdr message{}; message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    auto* cmsg = reinterpret_cast<cmsghdr*>(control.data());
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 2);
    std::memcpy(CMSG_DATA(cmsg), &first, sizeof(first));
    std::memcpy(static_cast<uint8_t*>(CMSG_DATA(cmsg)) + sizeof(first), &second, sizeof(second));
    CHECK(::sendmsg(pair[0], &message, 0) == static_cast<ssize_t>(wire.size()));
    pollfd pfd{pair[1], POLLIN, 0};
    CHECK(::poll(&pfd, 1, 1000) == 1);
    receiver.advance(std::chrono::steady_clock::now(), pfd.revents);
    CHECK(receiver.status() == DaemonControlStatus::ExtraFd);
    ::close(first); ::close(second); ::close(pair[0]);
}

} // namespace

int main() {
    test_incremental_handoff_and_fairness();
    test_extra_fd_is_closed_and_rejected();
    return 0;
}
