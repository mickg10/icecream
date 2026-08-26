#include "../cache/p50_daemon_control.h"
#include "../cache/p50_fd_handoff.h"

#include <chrono>
#include <array>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

CredentialExpectation credentials() {
    return CredentialExpectation{static_cast<uint64_t>(::getuid()),
                                 static_cast<uint64_t>(::getgid()),
                                 static_cast<uint64_t>(::getpid())};
}

std::array<uint8_t, 40> handoff_wire(const ControlOperation& value,
                                     uint16_t type = 1, uint32_t code = 0) {
    std::array<uint8_t, 40> wire{};
    wire[0] = 'P'; wire[1] = '5'; wire[2] = '0'; wire[3] = 'F';
    wire[5] = 1; wire[6] = static_cast<uint8_t>(type >> 8);
    wire[7] = static_cast<uint8_t>(type); wire[11] = 40;
    auto put64 = [&wire](size_t offset, uint64_t number) {
        for (size_t i = 0; i != 8; ++i)
            wire[offset + i] = static_cast<uint8_t>(number >> (56 - i * 8));
    };
    put64(12, value.identity.generation); put64(20, value.identity.attempt);
    put64(28, value.request_id);
    // Canonical p50_fd_handoff request codec: reserved request code is zero.
    wire[36] = static_cast<uint8_t>(code >> 24);
    wire[37] = static_cast<uint8_t>(code >> 16);
    wire[38] = static_cast<uint8_t>(code >> 8);
    wire[39] = static_cast<uint8_t>(code);
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

void send_bytes_with_fds(int socket, const uint8_t* bytes, size_t byte_count,
                         const int* fds, size_t fd_count) {
    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int) * 8)> control{};
    iovec iov{const_cast<uint8_t*>(bytes), byte_count};
    msghdr message{}; message.msg_iov = &iov; message.msg_iovlen = 1;
    if (fd_count != 0) {
        message.msg_control = control.data();
        message.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
        auto* cmsg = reinterpret_cast<cmsghdr*>(control.data());
        cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
        std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
    }
    CHECK(::sendmsg(socket, &message, 0) == static_cast<ssize_t>(byte_count));
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
                                 credentials(),
                                 std::chrono::steady_clock::now() + std::chrono::seconds(5),
                                 DaemonControlLimits{1, 7},
                                 DaemonControlFdOwnership::Borrowed) == DaemonControlStatus::InProgress);

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
        CHECK(encode_control_operation(decoded) == control.payload);
        nonblock(pair[1]);
        CHECK(receiver.begin_connected(pair[1], expected,
                                       std::chrono::steady_clock::now() + std::chrono::seconds(5),
                                       DaemonControlLimits{2, 7},
                                       DaemonControlFdOwnership::Borrowed) == DaemonControlStatus::InProgress);
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
        CHECK(sender.last_advance_syscalls() <= 1);
        CHECK(sender.last_advance_bytes() <= 7);
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
                                   std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                   DaemonControlLimits{},
                                   DaemonControlFdOwnership::Borrowed) ==
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

void test_canonical_request_codec_both_directions() {
    const ControlOperation expected = operation();
    const HandoffRequest request{expected.identity, expected.request_id};
    const auto expected_wire = handoff_wire(expected);

    // Capture bytes emitted by the frozen canonical FdHandoffSender and
    // compare the complete 40-byte request, including reserved code == 0.
    int pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    Connection canonical_sender_connection(pair[0]);
    CHECK(canonical_sender_connection.verify_peer_credentials(credentials()) == Status::Ok);
    const int payload = ::open("/dev/null", O_RDONLY);
    CHECK(payload >= 0);
    FdHandoffSender canonical_sender{HandoffFd(payload)};
    FdHandoffResult sender_result;
    std::thread sender_thread([&] {
        sender_result = canonical_sender.send(
            canonical_sender_connection, request,
            std::chrono::steady_clock::now() + std::chrono::seconds(5));
    });
    std::array<uint8_t, 40> captured{};
    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int))> control{};
    iovec iov{captured.data(), captured.size()};
    msghdr message{}; message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    CHECK(::recvmsg(pair[1], &message, 0) == static_cast<ssize_t>(captured.size()));
    CHECK(std::equal(captured.begin(), captured.end(), expected_wire.begin()));
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            int transferred = -1;
            std::memcpy(&transferred, CMSG_DATA(cmsg), sizeof(transferred));
            if (transferred >= 0) ::close(transferred);
        }
    }
    const auto ack = handoff_wire(expected, 2, 1);
    size_t offset = 0;
    while (offset != ack.size()) {
        const ssize_t count = ::send(pair[1], ack.data() + offset, ack.size() - offset, 0);
        CHECK(count > 0); offset += static_cast<size_t>(count);
    }
    sender_thread.join();
    CHECK(sender_result.status == FdHandoffStatus::Accepted);
    ::close(pair[1]);

    // Send our request bytes to the frozen canonical receiver and require it
    // to decode/adopt the same request identity and reserved code.
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    Connection canonical_receiver_connection(pair[1]);
    CHECK(canonical_receiver_connection.verify_peer_credentials(credentials()) == Status::Ok);
    const int received_payload = ::open("/dev/null", O_RDONLY);
    CHECK(received_payload >= 0);
    send_bytes_with_fds(pair[0], expected_wire.data(), expected_wire.size(),
                        &received_payload, 1);
    FdHandoffReceiver canonical_receiver;
    const FdHandoffResult receiver_result = canonical_receiver.receive_and_ack(
        canonical_receiver_connection, request,
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    CHECK(receiver_result.status == FdHandoffStatus::Accepted);
    std::array<uint8_t, 40> canonical_ack{};
    size_t ack_offset = 0;
    while (ack_offset != canonical_ack.size()) {
        const ssize_t count = ::recv(pair[0], canonical_ack.data() + ack_offset,
                                     canonical_ack.size() - ack_offset, 0);
        CHECK(count > 0);
        ack_offset += static_cast<size_t>(count);
    }
    CHECK(std::equal(canonical_ack.begin(), canonical_ack.end(),
                     handoff_wire(expected, 2, 1).begin()));
    HandoffFd adopted = canonical_receiver.take_adopted_fd();
    CHECK(adopted.valid());
    adopted.reset();
    ::close(received_payload); ::close(pair[0]);
}

void test_nonzero_reserved_code_rejected() {
    int pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    nonblock(pair[1]);
    const ControlOperation expected = operation();
    DaemonControlHandoffReceiver receiver;
    CHECK(receiver.begin_connected(pair[1], expected,
                                   std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                   DaemonControlLimits{},
                                   DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::InProgress);
    auto wire = handoff_wire(expected);
    wire[39] = 1;
    const int payload = ::open("/dev/null", O_RDONLY);
    CHECK(payload >= 0);
    send_bytes_with_fds(pair[0], wire.data(), wire.size(), &payload, 1);
    pollfd pfd{pair[1], POLLIN, 0};
    CHECK(::poll(&pfd, 1, 1000) == 1);
    receiver.advance(std::chrono::steady_clock::now(), pfd.revents);
    CHECK(receiver.status() == DaemonControlStatus::OperationMismatch);
    ::close(payload); ::close(pair[0]);
}

void test_client_frame_trailing_rejected() {
    const ControlOperation expected = operation();
    {
        int pair[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        nonblock(pair[0]);
        const int payload = ::open("/dev/null", O_RDONLY);
        CHECK(payload >= 0);
        DaemonControlOperation sender;
        CHECK(sender.begin_connected(pair[0], expected, payload, credentials(),
                                     std::chrono::steady_clock::now() + std::chrono::seconds(5),
                                     DaemonControlLimits{},
                                     DaemonControlFdOwnership::Borrowed) ==
              DaemonControlStatus::InProgress);
        std::thread peer([&] {
            Frame hello;
            CHECK(read_frame(pair[1], hello) == Status::Ok);
            write_frame(pair[1], make_hello_ack(PeerRole::Sidecar, expected.identity));
            const uint8_t trailing = 0xA5;
            CHECK(::send(pair[1], &trailing, sizeof(trailing), 0) == 1);
        });
        while (!sender.done()) {
            pollfd pfd{pair[0], sender.desired_events(), 0};
            CHECK(::poll(&pfd, 1, 1000) == 1);
            sender.advance(std::chrono::steady_clock::now(), pfd.revents);
        }
        peer.join();
        CHECK(sender.status() == DaemonControlStatus::TrailingData);
        ::close(pair[0]); ::close(pair[1]);
    }
    {
        int pair[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        nonblock(pair[0]);
        const int payload = ::open("/dev/null", O_RDONLY);
        CHECK(payload >= 0);
        DaemonControlOperation sender;
        CHECK(sender.begin_connected(pair[0], expected, payload, credentials(),
                                     std::chrono::steady_clock::now() + std::chrono::seconds(5),
                                     DaemonControlLimits{},
                                     DaemonControlFdOwnership::Borrowed) ==
              DaemonControlStatus::InProgress);
        std::thread peer([&] {
            Frame hello;
            CHECK(read_frame(pair[1], hello) == Status::Ok);
            write_frame(pair[1], make_hello_ack(PeerRole::Sidecar, expected.identity));
            Frame control;
            CHECK(read_frame(pair[1], control) == Status::Ok);
            std::array<uint8_t, 40> request{};
            alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int))> rights{};
            iovec iov{request.data(), request.size()};
            msghdr message{}; message.msg_iov = &iov; message.msg_iovlen = 1;
            message.msg_control = rights.data(); message.msg_controllen = rights.size();
            CHECK(::recvmsg(pair[1], &message, 0) == static_cast<ssize_t>(request.size()));
            for (cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
                 cmsg = CMSG_NXTHDR(&message, cmsg)) {
                if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                    cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
                    int transferred = -1;
                    std::memcpy(&transferred, CMSG_DATA(cmsg), sizeof(transferred));
                    if (transferred >= 0) ::close(transferred);
                }
            }
            const auto ack = handoff_wire(expected, 2, 1);
            size_t offset = 0;
            while (offset != ack.size()) {
                const ssize_t count = ::send(pair[1], ack.data() + offset,
                                             ack.size() - offset, 0);
                CHECK(count > 0);
                offset += static_cast<size_t>(count);
            }
            const uint8_t trailing = 0x5A;
            CHECK(::send(pair[1], &trailing, sizeof(trailing), 0) == 1);
        });
        while (!sender.done()) {
            pollfd pfd{pair[0], sender.desired_events(), 0};
            CHECK(::poll(&pfd, 1, 1000) == 1);
            sender.advance(std::chrono::steady_clock::now(), pfd.revents);
        }
        peer.join();
        CHECK(sender.status() == DaemonControlStatus::TrailingData);
        ::close(pair[0]); ::close(pair[1]);
    }
}

void test_receiver_quota_and_eagain() {
    int pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    nonblock(pair[1]);
    const ControlOperation expected = operation();
    DaemonControlHandoffReceiver receiver;
    CHECK(receiver.begin_connected(pair[1], expected,
                                   std::chrono::steady_clock::now() + std::chrono::seconds(5),
                                   DaemonControlLimits{1, 1},
                                   DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::InProgress);
    receiver.advance(std::chrono::steady_clock::now(), POLLIN);
    CHECK(receiver.status() == DaemonControlStatus::InProgress);
    CHECK(receiver.last_advance_syscalls() == 1);
    CHECK(receiver.last_advance_bytes() == 0);
    const int payload = ::open("/dev/null", O_RDONLY);
    CHECK(payload >= 0);
    const auto wire = handoff_wire(expected);
    send_bytes_with_fds(pair[0], wire.data(), wire.size(), &payload, 1);
    while (!receiver.done()) {
        pollfd pfd{pair[1], receiver.desired_events(), 0};
        CHECK(::poll(&pfd, 1, 1000) == 1);
        receiver.advance(std::chrono::steady_clock::now(), pfd.revents);
        CHECK(receiver.last_advance_syscalls() <= 1);
        CHECK(receiver.last_advance_bytes() <= 1);
        CHECK(receiver.wire_bytes_received() <= 40);
    }
    CHECK(receiver.status() == DaemonControlStatus::Complete);
    const int adopted = receiver.take_fd();
    CHECK(adopted >= 0); ::close(adopted);
    ::close(payload); ::close(pair[0]);
}

void test_trailing_and_control_truncation() {
    const ControlOperation expected = operation();
    {
        int pair[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        nonblock(pair[1]);
        DaemonControlHandoffReceiver receiver;
        CHECK(receiver.begin_connected(pair[1], expected,
                                       std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                       DaemonControlLimits{},
                                       DaemonControlFdOwnership::Borrowed) ==
              DaemonControlStatus::InProgress);
        const int payload = ::open("/dev/null", O_RDONLY);
        CHECK(payload >= 0);
        auto wire = handoff_wire(expected);
        std::array<uint8_t, 41> overlong{};
        std::copy(wire.begin(), wire.end(), overlong.begin()); overlong.back() = 0xA5;
        send_bytes_with_fds(pair[0], overlong.data(), overlong.size(), &payload, 1);
        pollfd pfd{pair[1], POLLIN, 0}; CHECK(::poll(&pfd, 1, 1000) == 1);
        receiver.advance(std::chrono::steady_clock::now(), pfd.revents);
        CHECK(receiver.status() == DaemonControlStatus::TrailingData);
        ::close(payload); ::close(pair[0]);
    }
    {
        int pair[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
        nonblock(pair[1]);
        DaemonControlHandoffReceiver receiver;
        CHECK(receiver.begin_connected(pair[1], expected,
                                       std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                       DaemonControlLimits{},
                                       DaemonControlFdOwnership::Borrowed) ==
              DaemonControlStatus::InProgress);
        std::array<int, 5> payloads{};
        for (int& payload : payloads) { payload = ::open("/dev/null", O_RDONLY); CHECK(payload >= 0); }
        const auto wire = handoff_wire(expected);
        send_bytes_with_fds(pair[0], wire.data(), wire.size(), payloads.data(), payloads.size());
        pollfd pfd{pair[1], POLLIN, 0}; CHECK(::poll(&pfd, 1, 1000) == 1);
        receiver.advance(std::chrono::steady_clock::now(), pfd.revents);
        CHECK(receiver.status() == DaemonControlStatus::ControlTruncated);
        for (const int payload : payloads) ::close(payload);
        ::close(pair[0]);
    }
}

void test_datagram_msg_truncation_and_deadline() {
    int pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0);
    nonblock(pair[1]);
    const ControlOperation expected = operation();
    DaemonControlHandoffReceiver receiver;
    CHECK(receiver.begin_connected(pair[1], expected,
                                   std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                   DaemonControlLimits{},
                                   DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::InProgress);
    std::array<uint8_t, 41> bytes{};
    const auto wire = handoff_wire(expected);
    std::copy(wire.begin(), wire.end(), bytes.begin());
    const int payload = ::open("/dev/null", O_RDONLY); CHECK(payload >= 0);
    send_bytes_with_fds(pair[0], bytes.data(), bytes.size(), &payload, 1);
    pollfd pfd{pair[1], POLLIN, 0}; CHECK(::poll(&pfd, 1, 1000) == 1);
    receiver.advance(std::chrono::steady_clock::now(), pfd.revents);
    CHECK(receiver.status() == DaemonControlStatus::Truncated);
    ::close(payload); ::close(pair[0]);

    int deadline_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, deadline_pair) == 0);
    nonblock(deadline_pair[1]);
    DaemonControlHandoffReceiver deadline_receiver;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    CHECK(deadline_receiver.begin_connected(deadline_pair[1], expected, deadline,
                                            DaemonControlLimits{},
                                            DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::InProgress);
    CHECK(deadline_receiver.advance(deadline, 0) == DaemonControlStatus::Timeout);
    ::close(deadline_pair[0]);
}

void test_credentials_and_owned_fd_lifetime() {
    int pair[2] = {-1, -1}; CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    nonblock(pair[0]);
    const int payload = ::open("/dev/null", O_RDONLY); CHECK(payload >= 0);
    CredentialExpectation mismatch = credentials();
    mismatch.uid = *mismatch.uid + 1;
    DaemonControlOperation sender;
    CHECK(sender.begin_connected(pair[0], operation(), payload, mismatch,
                                 std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                 DaemonControlLimits{},
                                 DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::InProgress);
    CHECK(sender.advance(std::chrono::steady_clock::now(), POLLOUT) ==
          DaemonControlStatus::CredentialFailure);
    pollfd peer{pair[1], POLLIN, 0}; CHECK(::poll(&peer, 1, 0) == 0);
    ::close(pair[0]); ::close(pair[1]);

    int owned_pair[2] = {-1, -1}; CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, owned_pair) == 0);
    nonblock(owned_pair[1]);
    const int owned_number = owned_pair[1];
    {
        DaemonControlHandoffReceiver owned;
        CHECK(owned.begin_connected(owned_pair[1], operation(),
                                    std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                    DaemonControlLimits{},
                                    DaemonControlFdOwnership::Owned) ==
              DaemonControlStatus::InProgress);
    }
    const int reused = ::open("/dev/null", O_RDONLY);
    CHECK(reused >= 0);
    CHECK(::fcntl(reused, F_GETFD) >= 0);
    if (reused == owned_number) CHECK(::fcntl(reused, F_GETFL) >= 0);
    ::close(reused); ::close(owned_pair[0]);

    int borrowed_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, borrowed_pair) == 0);
    nonblock(borrowed_pair[1]);
    {
        DaemonControlHandoffReceiver borrowed;
        CHECK(borrowed.begin_connected(borrowed_pair[1], operation(),
                                       std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                       DaemonControlLimits{},
                                       DaemonControlFdOwnership::Borrowed) ==
              DaemonControlStatus::InProgress);
    }
    CHECK(::fcntl(borrowed_pair[1], F_GETFD) >= 0);
    ::close(borrowed_pair[0]); ::close(borrowed_pair[1]);
}

void test_poll_adapter_relevance_fairness_and_removal() {
    int first_pair[2] = {-1, -1}; int second_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, first_pair) == 0);
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, second_pair) == 0);
    nonblock(first_pair[0]); nonblock(second_pair[0]);
    const int first_payload = ::open("/dev/null", O_RDONLY);
    const int second_payload = ::open("/dev/null", O_RDONLY);
    CHECK(first_payload >= 0 && second_payload >= 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    DaemonControlOperation first; DaemonControlOperation second;
    CHECK(first.begin_connected(first_pair[0], operation(), first_payload, credentials(), deadline,
                                DaemonControlLimits{}, DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::InProgress);
    ControlOperation second_operation = operation(); second_operation.request_id = 24;
    CHECK(second.begin_connected(second_pair[0], second_operation, second_payload, credentials(), deadline,
                                 DaemonControlLimits{}, DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::InProgress);
    DaemonControlPollAdapter adapter;
    const auto first_registration = adapter.add(first);
    const auto second_registration = adapter.add(second);
    CHECK(first_registration != second_registration && adapter.size() == 2);
    CHECK(adapter.advance_ready(std::chrono::steady_clock::now(), {0, 0}) == 0);
    CHECK(adapter.advance_ready(std::chrono::steady_clock::now(), {POLLIN, POLLIN}) == 0);
    CHECK(adapter.advance_ready(std::chrono::steady_clock::now(), {POLLOUT, POLLOUT}) == 2);
    CHECK(adapter.advance_ready(std::chrono::steady_clock::now(), {POLLHUP, POLLHUP}) == 2);
    CHECK(adapter.remove(first_registration));
    CHECK(!adapter.remove(first_registration));
    CHECK(adapter.size() == 1);
    CHECK(adapter.remove(second_registration));
    CHECK(adapter.size() == 0);
    ::close(first_pair[0]); ::close(first_pair[1]);
    ::close(second_pair[0]); ::close(second_pair[1]);

    int removed_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, removed_pair) == 0);
    nonblock(removed_pair[0]);
    const int removed_payload = ::open("/dev/null", O_RDONLY);
    CHECK(removed_payload >= 0);
    auto* removed = new DaemonControlOperation();
    CHECK(removed->begin_connected(removed_pair[0], operation(), removed_payload,
                                   credentials(),
                                   std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                   DaemonControlLimits{},
                                   DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::InProgress);
    const auto removed_registration = adapter.add(*removed);
    CHECK(adapter.remove(removed_registration));
    delete removed;
    CHECK(adapter.advance_ready(std::chrono::steady_clock::now(), {}) == 0);
    ::close(removed_pair[0]); ::close(removed_pair[1]);
}

} // namespace

int main() {
    test_incremental_handoff_and_fairness();
    test_extra_fd_is_closed_and_rejected();
    test_canonical_request_codec_both_directions();
    test_nonzero_reserved_code_rejected();
    test_client_frame_trailing_rejected();
    test_receiver_quota_and_eagain();
    test_trailing_and_control_truncation();
    test_datagram_msg_truncation_and_deadline();
    test_credentials_and_owned_fd_lifetime();
    test_poll_adapter_relevance_fairness_and_removal();
    return 0;
}
