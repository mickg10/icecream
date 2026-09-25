#include "../cache/p50_daemon_control.h"
#include "../cache/p50_fd_handoff.h"
#include "../services/digest128.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <future>
#include <poll.h>
#include <stdexcept>
#include <span>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
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

ControlOperation source_operation() {
    P50SourceTransferRequest arm;
    arm.wire_job_id = 7;
    arm.assignment_epoch = 3;
    arm.assignment_nonce = 4;
    arm.selected_f_host = "worker.example";
    arm.selected_f_ordinary_port = 10245;
    arm.selected_f_cache_port = 10246;
    arm.cache_protocol = CACHE_WIRE_REVISION;
    arm.cache_profile = CACHE_PROFILE_ZSTD_TU;
    arm.logical_job = 19;
    arm.compiler_attempt = 20;
    arm.source_request_id = 23;
    arm.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    const auto clock = icecc::p50::sidecar::process_monotonic_clock_identity();
    const auto deadline = icecc::p50::sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(5),
        clock.clock_domain_id, clock.time_namespace_id);
    return make_source_transfer_operation(Identity{91, 17}, arm, deadline);
}

P51SourceArmFields p51_source_arm() {
    P50SourceArmFields source;
    source.wire_job_id = 7;
    source.assignment_epoch = 3;
    source.assignment_nonce = 4;
    source.selected_f_host = "worker.example";
    source.selected_f_ordinary_port = 10245;
    source.selected_f_cache_port = 10246;
    source.cache_protocol = 2;
    source.cache_profile = CACHE_PROFILE_ZSTD_TU;
    source.logical_job = 19;
    source.compiler_attempt = 20;
    source.c_store_generation = 31;
    source.c_store_derivation_version =
        icecc::p50::kStoreIdentityDerivationVersion;
    for (size_t i = 0; i != source.c_store_guid.size(); ++i)
        source.c_store_guid[i] = static_cast<uint8_t>(i + 1);
    source.c_store_guid[icecc::p50::kStoreIdentityRoleByte] &=
        static_cast<uint8_t>(~icecc::p50::kStoreIdentityRoleMask);
    source.source_request_id = 23;
    source.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    source.c_control_generation = 41;
    source.c_control_attempt = 42;
    return P51SourceArmFields{source, 4};
}

P51SourceArmedFields p51_source_armed(const P51SourceArmFields& arm) {
    P51SourceArmedFields result;
    result.arm = arm;
    result.f_control_generation = 51;
    result.f_control_attempt = 52;
    result.f_store_generation = 53;
    result.f_store_derivation_version =
        icecc::p50::kStoreIdentityDerivationVersion;
    for (size_t i = 0; i != result.f_store_guid.size(); ++i)
        result.f_store_guid[i] = static_cast<uint8_t>(i + 101);
    result.f_store_guid[icecc::p50::kStoreIdentityRoleByte] |=
        icecc::p50::kStoreIdentityRoleMask;
    result.arm_observation_id = 54;
    result.source_budget_msec = 5000;
    for (size_t i = 0; i != result.attempt_capability_1.bytes.size(); ++i) {
        result.attempt_capability_1.bytes[i] = static_cast<uint8_t>(i + 1);
        result.attempt_capability_2.bytes[i] = static_cast<uint8_t>(i + 33);
        result.reservation_id[i] = static_cast<uint8_t>(i + 65);
        result.logical_relationship_id[i] = static_cast<uint8_t>(i + 81);
    }
    result.relationship_epoch = 91;
    result.selected_revision = 2;
    result.selected_window = 4;
    return result;
}

ControlOperation p51_reservation_operation() {
    ControlOperation operation;
    operation.kind = ControlOperationKind::SourceReservation;
    operation.identity = Identity{91, 17};
    operation.request_id = 23;
    P51SourceReservationRequest request;
    request.arm = p51_source_arm();
    request.absolute_deadline = icecc::p50::sidecar::AbsoluteMonotonicDeadline{
        INT64_C(0x0102030405060708), UINT64_C(0x1112131415161718),
        UINT64_C(0x2122232425262728)};
    operation.p51_reservation = request;
    return operation;
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

void write_bytes(int fd, const uint8_t* bytes, size_t byte_count) {
    size_t offset = 0;
    while (offset != byte_count) {
        const ssize_t count = ::send(fd, bytes + offset, byte_count - offset,
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

int make_source_fd(std::span<const uint8_t> bytes) {
    const char* temporary_directory = std::getenv("TMPDIR");
    if (temporary_directory == nullptr || temporary_directory[0] == '\0')
        temporary_directory = std::getenv("ICEFARM_TMPDIR");
    CHECK(temporary_directory != nullptr && temporary_directory[0] != '\0');
    std::string path = std::string(temporary_directory) +
                       "/icecc-daemon-control-source-XXXXXX";
    std::vector<char> mutable_path(path.begin(), path.end());
    mutable_path.push_back('\0');
    const int fd = ::mkstemp(mutable_path.data());
    CHECK(fd >= 0);
    CHECK(::unlink(mutable_path.data()) == 0);
    size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset,
                                      bytes.size() - offset);
        CHECK(count > 0);
        offset += static_cast<size_t>(count);
    }
    CHECK(::lseek(fd, 0, SEEK_SET) == 0);
    return fd;
}

void write_frame(int fd, const Frame& frame) {
    Status status = Status::InvalidArgument;
    const auto bytes = encode_frame(frame, &status);
    CHECK(status == Status::Ok && !bytes.empty());
    write_bytes(fd, bytes.data(), bytes.size());
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

void test_source_transfer_reply_and_tu0() {
    int pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    nonblock(pair[0]);
    const ControlOperation expected = source_operation();
    const int payload = ::open("/dev/null", O_RDONLY);
    CHECK(payload >= 0);
    DaemonControlOperation sender;
    CHECK(sender.begin_authenticated(
              pair[0], expected, payload, credentials(),
              expected.identity,
              std::chrono::steady_clock::now() + std::chrono::seconds(5),
              DaemonControlLimits{2, 4096}, DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::InProgress);

    std::thread peer([&] {
        Frame control;
        CHECK(read_frame(pair[1], control) == Status::Ok);
        ControlOperation decoded;
        CHECK(control.type == MessageType::Data &&
              decode_control_operation(control.payload, decoded));
        CHECK(decoded.kind == expected.kind && decoded.identity == expected.identity &&
              decoded.request_id == expected.request_id &&
              decoded.source_arm == expected.source_arm &&
              decoded.absolute_deadline == expected.absolute_deadline);

        Connection connection(pair[1]);
        CHECK(connection.verify_peer_credentials(credentials()) == Status::Ok);
        FdHandoffReceiver receiver;
        const auto handoff = receiver.receive_and_ack(
            connection, HandoffRequest{expected.identity, expected.request_id},
            expected.absolute_deadline.as_steady_time_point());
        CHECK(handoff.status == FdHandoffStatus::Accepted);
        HandoffFd received = receiver.take_adopted_fd();
        CHECK(received.valid());
        received.reset();

        P50SourceTransferResult result;
        result.code = SourceTransferResultCode::Committed;
        result.attempts = 1;
        result.tu_seq = 0;
        result.raw_bytes = 1;
        result.raw_digest.bytes[0] = 9;
        result.c_store_guid.bytes[15] = 91;
        const auto response = make_source_transfer_reply_operation(expected, result);
        write_frame(pair[1], Frame{kProtocolVersion, MessageType::Data,
                                   expected.identity, encode_control_operation(response)});
        Frame goodbye;
        CHECK(read_frame(pair[1], goodbye) == Status::Ok);
        CHECK(goodbye.type == MessageType::Goodbye && goodbye.identity == expected.identity);
    });
    while (!sender.done()) {
        pollfd pfd{pair[0], sender.desired_events(), 0};
        const int ready = ::poll(&pfd, 1, 1000);
        CHECK(ready == 1);
        sender.advance(std::chrono::steady_clock::now(), pfd.revents);
    }
    peer.join();
    CHECK(sender.status() == DaemonControlStatus::Complete);
    CHECK(sender.source_transfer_result().has_value());
    CHECK(sender.source_transfer_result()->tu_seq == 0);
    ::close(pair[1]);
}

void test_source_fd_released_after_ack_before_reply(bool p51) {
    constexpr std::array<uint8_t, 9> expected_bytes{
        0x50, 0x35, 0x31, 0x00, 0x7f, 0x21, 0x22, 0x23, 0x24};
    const auto clock = icecc::p50::sidecar::process_monotonic_clock_identity();
    const auto deadline = icecc::p50::sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(5),
        clock.clock_domain_id, clock.time_namespace_id);
    ControlOperation expected;
    if (p51) {
        P51SourceArmFields arm = p51_source_arm();
        arm.source.source_request_id = 23;
        const P51SourceArmedFields armed = p51_source_armed(arm);
        expected = make_p51_source_transfer_operation(
            Identity{91, 17}, P51SourceTransferRequest{armed, deadline}, 23);
    } else {
        P50SourceTransferRequest arm;
        arm.wire_job_id = 7;
        arm.assignment_epoch = 3;
        arm.assignment_nonce = 4;
        arm.selected_f_host = "worker.example";
        arm.selected_f_ordinary_port = 10245;
        arm.selected_f_cache_port = 10246;
        arm.cache_protocol = CACHE_WIRE_REVISION;
        arm.cache_profile = CACHE_PROFILE_ZSTD_TU;
        arm.logical_job = 19;
        arm.compiler_attempt = 20;
        arm.source_request_id = 23;
        arm.source_mode = P50_SOURCE_MODE_ZSTD_TU;
        expected = make_source_transfer_operation(Identity{91, 17}, arm, deadline);
    }
    CHECK(!encode_control_operation(expected).empty());

    int pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    nonblock(pair[0]);
    const timeval peer_timeout{6, 0};
    CHECK(::setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &peer_timeout,
                       sizeof(peer_timeout)) == 0);
    const int source_fd = make_source_fd(expected_bytes);
    const int source_fd_number = source_fd;
    std::promise<void> ack_sent_promise;
    auto ack_sent = ack_sent_promise.get_future();
    std::promise<void> release_reply_promise;
    const std::shared_future<void> release_reply =
        release_reply_promise.get_future().share();
    std::exception_ptr peer_error;
    bool adopted_bytes_exact = false;
    std::optional<P50SourceTransferResult> completed_result;
    int replacement_fd = -1;

    {
        DaemonControlOperation sender;
        CHECK(sender.begin_authenticated(
                  pair[0], expected, source_fd, credentials(), expected.identity,
                  deadline.as_steady_time_point(), DaemonControlLimits{2, 4096},
                  DaemonControlFdOwnership::Owned) ==
              DaemonControlStatus::InProgress);
        pair[0] = -1; // sender owns the control socket now.

        std::thread peer([&] {
            try {
                Connection connection(pair[1]);
                Frame control;
                CHECK(read_frame(connection.native_handle(), control) == Status::Ok);
                ControlOperation decoded;
                CHECK(control.type == MessageType::Data &&
                      decode_control_operation(control.payload, decoded));
                CHECK(encode_control_operation(decoded) ==
                      encode_control_operation(expected));
                CHECK(connection.verify_peer_credentials(credentials()) == Status::Ok);
                FdHandoffReceiver receiver;
                const auto handoff = receiver.receive_and_ack(
                    connection, HandoffRequest{expected.identity, expected.request_id},
                    deadline.as_steady_time_point());
                CHECK(handoff.status == FdHandoffStatus::Accepted);
                HandoffFd received = receiver.take_adopted_fd();
                CHECK(received.valid());
                std::array<uint8_t, expected_bytes.size()> observed{};
                CHECK(::pread(received.get(), observed.data(), observed.size(), 0) ==
                      static_cast<ssize_t>(observed.size()));
                adopted_bytes_exact = observed == expected_bytes;
                ack_sent_promise.set_value();
                CHECK(release_reply.wait_for(std::chrono::seconds(4)) ==
                      std::future_status::ready);

                P50SourceTransferResult result;
                result.code = SourceTransferResultCode::Committed;
                result.attempts = 1;
                result.tu_seq = 0;
                result.raw_bytes = expected_bytes.size();
                result.raw_digest = icecc::digest128(expected_bytes);
                if (p51) {
                    result.c_store_guid.bytes =
                        expected.p51_source_transfer->armed.arm.source.c_store_guid;
                    const auto reply = make_p51_source_transfer_reply_operation(
                        expected, result);
                    write_frame(pair[1], Frame{kProtocolVersion, MessageType::Data,
                                               expected.identity,
                                               encode_control_operation(reply)});
                } else {
                    result.c_store_guid.bytes[0] = 1;
                    const auto reply = make_source_transfer_reply_operation(
                        expected, result);
                    write_frame(pair[1], Frame{kProtocolVersion, MessageType::Data,
                                               expected.identity,
                                               encode_control_operation(reply)});
                }
                Frame goodbye;
                CHECK(read_frame(pair[1], goodbye) == Status::Ok);
                CHECK(goodbye.type == MessageType::Goodbye &&
                      goodbye.identity == expected.identity);
            } catch (...) {
                peer_error = std::current_exception();
                try { ack_sent_promise.set_value(); } catch (...) {}
            }
        });

        const auto ack_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(2);
        while (ack_sent.wait_for(std::chrono::milliseconds(0)) !=
                   std::future_status::ready &&
               std::chrono::steady_clock::now() < ack_deadline &&
               sender.status() == DaemonControlStatus::InProgress) {
            pollfd descriptor{sender.native_handle(), sender.desired_events(), 0};
            const int ready = ::poll(&descriptor, 1, 10);
            sender.advance(std::chrono::steady_clock::now(),
                           ready > 0 ? descriptor.revents : short{0});
        }
        const bool ack_received =
            ack_sent.wait_for(std::chrono::milliseconds(0)) ==
            std::future_status::ready;

        const auto fd_release_deadline = std::chrono::steady_clock::now() +
                                         std::chrono::seconds(1);
        bool sender_fd_closed = false;
        while (ack_received && !sender_fd_closed &&
               std::chrono::steady_clock::now() < fd_release_deadline &&
               sender.status() == DaemonControlStatus::InProgress) {
            errno = 0;
            sender_fd_closed = ::fcntl(source_fd_number, F_GETFD) == -1 &&
                               errno == EBADF;
            if (sender_fd_closed)
                break;
            pollfd descriptor{sender.native_handle(), sender.desired_events(), 0};
            const int ready = ::poll(&descriptor, 1, 10);
            sender.advance(std::chrono::steady_clock::now(),
                           ready > 0 ? descriptor.revents : short{0});
        }
        errno = 0;
        sender_fd_closed = sender_fd_closed ||
            (::fcntl(source_fd_number, F_GETFD) == -1 && errno == EBADF);
        if (sender_fd_closed) {
            const int opened = ::open("/dev/null", O_RDONLY);
            if (opened == source_fd_number) {
                replacement_fd = opened;
            } else if (opened >= 0) {
                replacement_fd = ::dup2(opened, source_fd_number);
                (void)::close(opened);
            }
        }

        // Keep the real sidecar reply withheld until after we have proved the
        // sender's original descriptor is gone and reused its number.
        release_reply_promise.set_value();
        const auto result_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(3);
        while (sender.status() == DaemonControlStatus::InProgress &&
               std::chrono::steady_clock::now() < result_deadline) {
            pollfd descriptor{sender.native_handle(), sender.desired_events(), 0};
            const int ready = ::poll(&descriptor, 1, 10);
            sender.advance(std::chrono::steady_clock::now(),
                           ready > 0 ? descriptor.revents : short{0});
        }
        if (sender.source_transfer_result().has_value())
            completed_result = sender.source_transfer_result();
        peer.join();
        CHECK(sender.status() == DaemonControlStatus::Complete);
        pair[1] = -1;
        CHECK(ack_received);
        CHECK(sender_fd_closed);
        CHECK(replacement_fd == source_fd_number);
        CHECK(adopted_bytes_exact);
        CHECK(completed_result.has_value() && completed_result->valid());
        CHECK(completed_result->raw_bytes == expected_bytes.size());
        CHECK(completed_result->raw_digest == icecc::digest128(expected_bytes));
        CHECK(!peer_error);
    }

    // The completed operation's destructor must not close the numeric FD
    // reused after ACK.  The receiver still read the exact source bytes and
    // the delayed source reply completed normally for both R1 and R2.
    CHECK(replacement_fd == source_fd_number);
    CHECK(::fcntl(replacement_fd, F_GETFD) >= 0);
    CHECK(::close(replacement_fd) == 0);
}

void test_source_fd_remains_owned_until_pre_ack_cancel() {
    constexpr std::array<uint8_t, 3> bytes{'f', 'd', '0'};
    const ControlOperation expected = source_operation();
    int pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    nonblock(pair[0]);
    const int source_fd = make_source_fd(bytes);
    const int source_number = source_fd;
    {
        DaemonControlOperation sender;
        CHECK(sender.begin_connected(
                  pair[0], expected, source_fd, credentials(),
                  std::chrono::steady_clock::now() + std::chrono::seconds(5),
                  DaemonControlLimits{2, 4096},
                  DaemonControlFdOwnership::Owned) ==
              DaemonControlStatus::InProgress);
        pair[0] = -1;
        CHECK(::fcntl(source_number, F_GETFD) >= 0);
        // Destroying an operation before a valid ACK is cancellation: the
        // sidecar has not established ownership, so the sender still closes
        // its source descriptor during teardown.
    }
    errno = 0;
    CHECK(::fcntl(source_number, F_GETFD) == -1 && errno == EBADF);
    const int replacement = ::open("/dev/null", O_RDONLY);
    CHECK(replacement >= 0);
    int held = replacement;
    if (replacement != source_number) {
        held = ::dup2(replacement, source_number);
        CHECK(held == source_number);
        CHECK(::close(replacement) == 0);
    }
    CHECK(::fcntl(held, F_GETFD) >= 0);
    CHECK(::close(held) == 0);
    CHECK(::close(pair[1]) == 0);
}

void test_source_fd_closes_after_handoff_failure_without_ack() {
    constexpr std::array<uint8_t, 3> bytes{'f', 'd', '1'};
    const ControlOperation expected = source_operation();
    int pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    nonblock(pair[0]);
    const timeval peer_timeout{6, 0};
    CHECK(::setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &peer_timeout,
                       sizeof(peer_timeout)) == 0);
    const int source_fd = make_source_fd(bytes);
    const int source_number = source_fd;
    std::promise<void> control_seen_promise;
    auto control_seen = control_seen_promise.get_future();
    std::promise<void> release_peer_close_promise;
    const std::shared_future<void> release_peer_close =
        release_peer_close_promise.get_future().share();
    std::exception_ptr peer_error;
    std::thread peer([&] {
        try {
            Connection connection(pair[1]);
            Frame hello;
            CHECK(read_frame(pair[1], hello) == Status::Ok);
            CHECK(hello.type == MessageType::Hello &&
                  hello.identity == expected.identity);
            write_frame(pair[1], make_hello_ack(
                PeerRole::Sidecar, expected.identity));
            Frame control;
            CHECK(read_frame(pair[1], control) == Status::Ok);
            ControlOperation decoded;
            CHECK(control.type == MessageType::Data &&
                  decode_control_operation(control.payload, decoded));
            CHECK(encode_control_operation(decoded) ==
                  encode_control_operation(expected));
            control_seen_promise.set_value();
            CHECK(release_peer_close.wait_for(std::chrono::seconds(4)) ==
                  std::future_status::ready);
            // Close after the sender reports the descriptor rights sent, but
            // before any ACK is emitted. This is a real failed handoff path.
        } catch (...) {
            peer_error = std::current_exception();
            try { control_seen_promise.set_value(); } catch (...) {}
        }
    });

    DaemonControlStatus final_status = DaemonControlStatus::InProgress;
    bool rights_sent = false;
    {
        DaemonControlOperation sender;
        CHECK(sender.begin_connected(
                  pair[0], expected, source_fd, credentials(),
                  std::chrono::steady_clock::now() + std::chrono::seconds(5),
                  DaemonControlLimits{2, 4096},
                  DaemonControlFdOwnership::Owned) ==
              DaemonControlStatus::InProgress);
        pair[0] = -1;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(4);
        while (control_seen.wait_for(std::chrono::milliseconds(0)) !=
                   std::future_status::ready &&
               sender.status() == DaemonControlStatus::InProgress &&
               std::chrono::steady_clock::now() < deadline) {
            pollfd descriptor{sender.native_handle(), sender.desired_events(), 0};
            const int ready = ::poll(&descriptor, 1, 10);
            sender.advance(std::chrono::steady_clock::now(),
                           ready > 0 ? descriptor.revents : short{0});
        }
        while (!sender.rights_sent() &&
               sender.status() == DaemonControlStatus::InProgress &&
               std::chrono::steady_clock::now() < deadline) {
            pollfd descriptor{sender.native_handle(), sender.desired_events(), 0};
            const int ready = ::poll(&descriptor, 1, 10);
            sender.advance(std::chrono::steady_clock::now(),
                           ready > 0 ? descriptor.revents : short{0});
        }
        rights_sent = sender.rights_sent();
        release_peer_close_promise.set_value();
        while (sender.status() == DaemonControlStatus::InProgress &&
               std::chrono::steady_clock::now() < deadline) {
            pollfd descriptor{sender.native_handle(), sender.desired_events(), 0};
            const int ready = ::poll(&descriptor, 1, 10);
            sender.advance(std::chrono::steady_clock::now(),
                           ready > 0 ? descriptor.revents : short{0});
        }
        final_status = sender.status();
        peer.join();
    }
    CHECK(control_seen.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::ready);
    CHECK(rights_sent);
    CHECK(final_status != DaemonControlStatus::InProgress &&
          final_status != DaemonControlStatus::Complete);
    CHECK(!peer_error);
    errno = 0;
    CHECK(::fcntl(source_number, F_GETFD) == -1 && errno == EBADF);
    const int replacement = ::open("/dev/null", O_RDONLY);
    CHECK(replacement >= 0);
    int held = replacement;
    if (replacement != source_number) {
        held = ::dup2(replacement, source_number);
        CHECK(held == source_number);
        CHECK(::close(replacement) == 0);
    }
    CHECK(::fcntl(held, F_GETFD) >= 0);
    CHECK(::close(held) == 0);
}

void test_p51_source_reservation_v7_codec() {
    const ControlOperation request = p51_reservation_operation();
    const auto wire = encode_control_operation(request);
    CHECK(request.p51_reservation.has_value());
    CHECK(request.p51_reservation->arm.valid());
    CHECK(wire.size() == kP51SourceReservationOperationBytes);
    CHECK(wire[0] == 0 && wire[1] == 7 &&
          wire[2] == 0 && wire[3] == 6 &&
          wire[4] == 0 && wire[5] == 0 && wire[6] == 4 && wire[7] == 0);
    CHECK(wire[32] == 0 && wire[33] == 0 &&
          std::all_of(wire.begin() + 34, wire.begin() + 40,
                      [](uint8_t value) { return value == 0; }));
    const std::array<uint8_t, 24> deadline_fixture{
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
    CHECK(std::equal(deadline_fixture.begin(), deadline_fixture.end(),
                     wire.begin() + 40));
    const std::array<uint8_t, 22> arm_prefix_fixture{
        0x00, 0x00, 0x00, 0x07,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
        0x00, 0x0e};
    CHECK(std::equal(arm_prefix_fixture.begin(), arm_prefix_fixture.end(),
                     wire.begin() + 64));
    CHECK(std::equal(request.p51_reservation->arm.source.selected_f_host.begin(),
                     request.p51_reservation->arm.source.selected_f_host.end(),
                     wire.begin() + 86));
    CHECK(wire[344] == 0x00 && wire[345] == 0x00 &&
          wire[346] == 0x28 && wire[347] == 0x05);
    CHECK(wire[352] == 0 && wire[353] == 0 &&
          wire[354] == 0 && wire[355] == 2);
    CHECK(wire[436] == 0 && wire[437] == 0 &&
          wire[438] == 0 && wire[439] == 4);

    ControlOperation decoded;
    CHECK(decode_control_operation(wire, decoded));
    CHECK(decoded.kind == ControlOperationKind::SourceReservation);
    CHECK(decoded.identity == request.identity &&
          decoded.request_id == request.request_id);
    CHECK(decoded.p51_reservation == request.p51_reservation);
    CHECK(!decoded.p51_reservation_result.has_value());

    ControlOperation success = request;
    success.p51_reservation_result = P51SourceReservationResult{
        0, p51_source_armed(request.p51_reservation->arm)};
    const auto success_wire = encode_control_operation(success);
    CHECK(success_wire.size() == kP51SourceReservationOperationBytes);
    CHECK(success_wire[32] == 0 && success_wire[33] == 1);
    ControlOperation success_decoded;
    CHECK(decode_control_operation(success_wire, success_decoded));
    CHECK(success_decoded.p51_reservation == success.p51_reservation);
    CHECK(success_decoded.p51_reservation_result ==
          success.p51_reservation_result);

    ControlOperation refused = request;
    refused.p51_reservation_result = P51SourceReservationResult{0x5001,
                                                                std::nullopt};
    const auto refused_wire = encode_control_operation(refused);
    CHECK(refused_wire.size() == kP51SourceReservationOperationBytes);
    CHECK(refused_wire[32] == 0 && refused_wire[33] == 2 &&
          refused_wire[34] == 0x50 && refused_wire[35] == 0x01);
    ControlOperation refused_decoded;
    CHECK(decode_control_operation(refused_wire, refused_decoded));
    CHECK(refused_decoded.p51_reservation_result ==
          refused.p51_reservation_result);

    CHECK(!decode_control_operation(
        std::span<const uint8_t>(wire.data(), wire.size() - 1), decoded));
    auto oversized = wire;
    oversized.push_back(0);
    CHECK(!decode_control_operation(oversized, decoded));
    auto bad_header = wire;
    bad_header[1] = 6;
    CHECK(!decode_control_operation(bad_header, decoded));
    auto nonzero_header_padding = wire;
    nonzero_header_padding[36] = 1;
    CHECK(!decode_control_operation(nonzero_header_padding, decoded));
    auto noncanonical_host_padding = wire;
    noncanonical_host_padding[100] = 1;
    CHECK(!decode_control_operation(noncanonical_host_padding, decoded));
    auto nonzero_arm_padding = wire;
    nonzero_arm_padding[440] = 1;
    CHECK(!decode_control_operation(nonzero_arm_padding, decoded));
    auto invalid_phase = wire;
    invalid_phase[33] = 3;
    CHECK(!decode_control_operation(invalid_phase, decoded));
    auto request_with_error = wire;
    request_with_error[35] = 1;
    CHECK(!decode_control_operation(request_with_error, decoded));
    auto success_with_error = success_wire;
    success_with_error[35] = 1;
    CHECK(!decode_control_operation(success_with_error, decoded));
    auto refusal_without_error = refused_wire;
    refusal_without_error[34] = refusal_without_error[35] = 0;
    CHECK(!decode_control_operation(refusal_without_error, decoded));
    auto success_padding = success_wire;
    success_padding.back() = 1;
    CHECK(!decode_control_operation(success_padding, decoded));
    auto armed_padding = success_wire;
    armed_padding[456 + 140] = 1;
    CHECK(!decode_control_operation(armed_padding, decoded));

    ControlOperation inconsistent_echo = success;
    ++inconsistent_echo.p51_reservation_result->armed->arm.requested_window;
    CHECK(encode_control_operation(inconsistent_echo).empty());
    auto invalid_request = request;
    invalid_request.p51_reservation->arm.requested_window = 31;
    CHECK(encode_control_operation(invalid_request).empty());
    // The wire codec preserves absolute clock tuples exactly; matching the
    // local clock and checking expiry belong to the reservation service.
}

void test_p51_source_transfer_downselected_window_v7_codec() {
    const auto clock = icecc::p50::sidecar::process_monotonic_clock_identity();
    const auto deadline =
        icecc::p50::sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            clock.clock_domain_id, clock.time_namespace_id);
    P51SourceArmFields arm = p51_source_arm();
    arm.requested_window = 30;

    for (const uint32_t selected_window : {1u, 30u}) {
        P51SourceTransferRequest request;
        request.armed = p51_source_armed(arm);
        request.armed.selected_window = selected_window;
        request.absolute_deadline = deadline;
        CHECK(request.armed.valid());
        const ControlOperation operation = make_p51_source_transfer_operation(
            Identity{91, 17}, request, arm.source.source_request_id);
        const auto wire = encode_control_operation(operation);
        CHECK(wire.size() == kP51SourceReservationOperationBytes);
        ControlOperation decoded;
        check(decode_control_operation(wire, decoded),
              selected_window == 1 ? "P51 transfer selected window one decodes"
                                   : "P51 transfer selected window thirty decodes");
        CHECK(decoded.kind == ControlOperationKind::P51SourceTransfer);
        CHECK(decoded.p51_source_transfer == request);

        P50SourceTransferResult error;
        error.code = SourceTransferResultCode::Error;
        error.error_code = 9;
        error.attempts = 1;
        const ControlOperation reply =
            make_p51_source_transfer_reply_operation(operation, error);
        const auto reply_wire = encode_control_operation(reply);
        CHECK(reply_wire.size() == kP51SourceReservationOperationBytes);
        ControlOperation decoded_reply;
        CHECK(decode_control_operation(reply_wire, decoded_reply));
        CHECK(decoded_reply.p51_source_transfer == request);
        CHECK(decoded_reply.p51_source_transfer_result == error);

        P50SourceTransferResult committed_empty;
        committed_empty.code = SourceTransferResultCode::Committed;
        committed_empty.attempts = 1;
        committed_empty.tu_seq = 0;
        committed_empty.raw_bytes = 0;
        committed_empty.raw_digest = icecc::digest128(
            std::span<const uint8_t>{});
        committed_empty.c_store_guid.bytes = arm.source.c_store_guid;
        CHECK(committed_empty.valid());
        const auto committed_reply = make_p51_source_transfer_reply_operation(
            operation, committed_empty);
        const auto committed_wire = encode_control_operation(committed_reply);
        ControlOperation decoded_committed;
        CHECK(decode_control_operation(committed_wire, decoded_committed));
        CHECK(decoded_committed.p51_source_transfer_result == committed_empty);

        auto store_u32 = [](std::vector<uint8_t>& bytes, size_t offset,
                            uint32_t value) {
            bytes[offset] = static_cast<uint8_t>(value >> 24);
            bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
            bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
            bytes[offset + 3] = static_cast<uint8_t>(value);
        };
        constexpr size_t kP51ReplyOffset = 456;
        for (const auto& [offset, invalid_value] : {
                 std::pair<size_t, uint32_t>{kP51ReplyOffset + 136, 0},
                 std::pair<size_t, uint32_t>{kP51ReplyOffset + 136, 31},
                 std::pair<size_t, uint32_t>{kP51ReplyOffset + 132, 3}}) {
            auto malformed = reply_wire;
            store_u32(malformed, offset, invalid_value);
            ControlOperation rejected;
            CHECK(!decode_control_operation(malformed, rejected));
        }
    }

    P51SourceTransferRequest invalid;
    invalid.armed = p51_source_armed(arm);
    invalid.absolute_deadline = deadline;
    invalid.armed.selected_window = 0;
    CHECK(encode_control_operation(make_p51_source_transfer_operation(
              Identity{91, 17}, invalid, arm.source.source_request_id)).empty());
    invalid.armed.selected_window = 31;
    CHECK(encode_control_operation(make_p51_source_transfer_operation(
              Identity{91, 17}, invalid, arm.source.source_request_id)).empty());
    invalid.armed.selected_window = 1;
    invalid.armed.selected_revision = 3;
    CHECK(encode_control_operation(make_p51_source_transfer_operation(
              Identity{91, 17}, invalid, arm.source.source_request_id)).empty());
    invalid.armed.selected_revision = CACHE_WIRE_REVISION_R2;
    CHECK(encode_control_operation(make_p51_source_transfer_operation(
              Identity{91, 17}, invalid,
              arm.source.source_request_id + 1)).empty());
    std::puts("P51_SOURCE_TRANSFER v7 down-selected-window request/reply: ok");
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
            Status status = Status::InvalidArgument;
            auto reply = encode_frame(
                make_hello_ack(PeerRole::Sidecar, expected.identity), &status);
            CHECK(status == Status::Ok && !reply.empty());
            const uint8_t trailing = 0xA5;
            reply.push_back(trailing);
            write_bytes(pair[1], reply.data(), reply.size());
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
            const uint8_t trailing = 0x5A;
            std::array<uint8_t, 41> reply{};
            std::copy(ack.begin(), ack.end(), reply.begin());
            reply.back() = trailing;
            write_bytes(pair[1], reply.data(), reply.size());
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

void test_connect_pending_ignores_preconnect_hup() {
    const std::string path =
        "/tmp/p50-daemon-control-" + std::to_string(::getpid()) + ".sock";
    (void)::unlink(path.c_str());
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    CHECK(path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&address),
                 static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                        path.size() + 1)) == 0);
    CHECK(::listen(listener, 1) == 0);

    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(client >= 0);
    nonblock(client);
    const int payload = ::open("/dev/null", O_RDONLY);
    CHECK(payload >= 0);
    DaemonControlOperation sender;
    CHECK(sender.begin_connecting(
              path, client, operation(), payload, credentials(),
              std::chrono::steady_clock::now() + std::chrono::seconds(1),
              DaemonControlLimits{1, 4096},
              DaemonControlFdOwnership::Owned) ==
          DaemonControlStatus::InProgress);

    CHECK(sender.advance(std::chrono::steady_clock::now(),
                         POLLOUT | POLLHUP) ==
          DaemonControlStatus::InProgress);
    CHECK(sender.last_advance_syscalls() == 1);
    const int accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);
    ::close(accepted);
    ::close(listener);
    (void)::unlink(path.c_str());
}

void test_authenticated_entry_consumes_transfer_on_every_return() {
    int invalid_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, invalid_pair) == 0);
    nonblock(invalid_pair[0]);
    const int invalid_payload = ::open("/dev/null", O_RDONLY);
    CHECK(invalid_payload >= 0);
    const ControlOperation expected = source_operation();
    DaemonControlOperation invalid;
    CHECK(invalid.begin_authenticated(
              invalid_pair[0], expected, invalid_payload, credentials(),
              Identity{expected.identity.generation,
                       expected.identity.attempt + 1},
              std::chrono::steady_clock::now() + std::chrono::seconds(1),
              DaemonControlLimits{}, DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::InvalidArgument);
    errno = 0;
    CHECK(::fcntl(invalid_payload, F_GETFD) == -1 && errno == EBADF);
    CHECK(::fcntl(invalid_pair[0], F_GETFD) >= 0);
    ::close(invalid_pair[0]);
    ::close(invalid_pair[1]);

    int credential_pair[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, credential_pair) == 0);
    nonblock(credential_pair[0]);
    const int credential_payload = ::open("/dev/null", O_RDONLY);
    CHECK(credential_payload >= 0);
    CredentialExpectation mismatch = credentials();
    mismatch.uid = *mismatch.uid + 1;
    DaemonControlOperation rejected;
    CHECK(rejected.begin_authenticated(
              credential_pair[0], expected, credential_payload, mismatch,
              expected.identity,
              std::chrono::steady_clock::now() + std::chrono::seconds(1),
              DaemonControlLimits{}, DaemonControlFdOwnership::Borrowed) ==
          DaemonControlStatus::CredentialFailure);
    errno = 0;
    CHECK(::fcntl(credential_payload, F_GETFD) == -1 && errno == EBADF);
    CHECK(::fcntl(credential_pair[0], F_GETFD) >= 0);
    ::close(credential_pair[0]);
    ::close(credential_pair[1]);
}

} // namespace

int main() {
    test_incremental_handoff_and_fairness();
    test_extra_fd_is_closed_and_rejected();
    test_source_transfer_reply_and_tu0();
    test_source_fd_released_after_ack_before_reply(false);
    test_source_fd_released_after_ack_before_reply(true);
    test_source_fd_remains_owned_until_pre_ack_cancel();
    test_source_fd_closes_after_handoff_failure_without_ack();
    test_p51_source_reservation_v7_codec();
    test_p51_source_transfer_downselected_window_v7_codec();
    test_canonical_request_codec_both_directions();
    test_nonzero_reserved_code_rejected();
    test_client_frame_trailing_rejected();
    test_receiver_quota_and_eagain();
    test_trailing_and_control_truncation();
    test_datagram_msg_truncation_and_deadline();
    test_credentials_and_owned_fd_lifetime();
    test_poll_adapter_relevance_fairness_and_removal();
    test_connect_pending_ignores_preconnect_hup();
    test_authenticated_entry_consumes_transfer_on_every_return();
    return 0;
}
