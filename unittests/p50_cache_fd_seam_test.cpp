#include "../services/comm.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

void check(bool value, const char *text)
{
    if (!value)
        throw std::runtime_error(text);
}

#define CHECK(value) check((value), #value)

struct ChannelPair {
    MsgChannel *left = nullptr;
    MsgChannel *right = nullptr;
    ~ChannelPair()
    {
        delete left;
        delete right;
    }
};

ChannelPair make_pair(int protocol = PROTOCOL_VERSION_P50_CACHE_SESSION_R1)
{
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    ChannelPair pair;
    std::thread left([&] {
        pair.left = Service::createChannel(
            sockets[0], reinterpret_cast<sockaddr *>(&address), sizeof(address));
    });
    std::thread right([&] {
        pair.right = Service::createChannel(
            sockets[1], reinterpret_cast<sockaddr *>(&address), sizeof(address));
    });
    left.join();
    right.join();
    CHECK(pair.left != nullptr && pair.right != nullptr);
    pair.left->protocol = pair.right->protocol = protocol;
    return pair;
}

P50CacheSessionFdRequestFields request()
{
    return P50CacheSessionFdRequestFields{
        0x12345678, UINT64_C(0x1020304050607080),
        UINT64_C(0x8877665544332211), CACHE_PROFILE_ZSTD_TU};
}

P50CacheControlIdentity control_identity()
{
    return P50CacheControlIdentity{
        UINT64_C(0x1112131415161718),
        UINT64_C(0x2122232425262728),
        UINT64_C(4103), UINT64_C(3513)};
}

std::chrono::steady_clock::time_point deadline()
{
    return std::chrono::steady_clock::now() + std::chrono::seconds(2);
}

template <size_t N>
void put32(std::array<uint8_t, N> &wire, size_t offset, uint32_t value)
{
    wire[offset] = static_cast<uint8_t>(value >> 24);
    wire[offset + 1] = static_cast<uint8_t>(value >> 16);
    wire[offset + 2] = static_cast<uint8_t>(value >> 8);
    wire[offset + 3] = static_cast<uint8_t>(value);
}

template <size_t N>
void put64(std::array<uint8_t, N> &wire, size_t offset, uint64_t value)
{
    for (size_t i = 0; i != 8; ++i)
        wire[offset + i] = static_cast<uint8_t>(value >> (56 - i * 8));
}

std::array<uint8_t, P50_CACHE_FD_LEASE_BYTES> lease_wire(
    const P50CacheSessionFdRequestFields &value,
    P50CacheControlIdentity identity = control_identity())
{
    std::array<uint8_t, P50_CACHE_FD_LEASE_BYTES> wire{};
    put32(wire, 0, P50_CACHE_FD_LEASE_MAGIC);
    put32(wire, 4, P50_CACHE_FD_LEASE_VERSION);
    put32(wire, 8, value.wire_job_id);
    put64(wire, 12, value.assignment_epoch);
    put64(wire, 20, value.assignment_nonce);
    put32(wire, 28, value.profile);
    put64(wire, 32, identity.generation);
    put64(wire, 40, identity.attempt);
    put64(wire, 48, identity.peer_uid);
    put64(wire, 56, identity.peer_gid);
    return wire;
}

P51SourceLeaseRequestFields request_v4()
{
    return {0x12345678, UINT64_C(0x1020304050607080),
            UINT64_C(0x8877665544332211), CACHE_PROFILE_ZSTD_TU, 2, 4};
}

P51CacheControlIdentity control_identity_v4()
{
    P51CacheControlIdentity identity;
    identity.control_generation = UINT64_C(0x1112131415161718);
    identity.control_attempt = UINT64_C(0x2122232425262728);
    identity.peer_uid = 4103;
    identity.peer_gid = 3513;
    identity.c_store_generation = UINT64_C(0x3132333435363738);
    identity.derivation_version =
        icecc::p50::kStoreIdentityDerivationVersion;
    for (size_t i = 0; i != identity.c_store_guid.size(); ++i)
        identity.c_store_guid[i] = static_cast<uint8_t>(0x90 + i);
    identity.c_store_guid[icecc::p50::kStoreIdentityRoleByte] &=
        static_cast<uint8_t>(~icecc::p50::kStoreIdentityRoleMask);
    identity.c_store_guid[icecc::p50::kStoreIdentityRoleByte] |=
        icecc::p50::kStoreIdentityClientRole;
    return identity;
}

std::array<uint8_t, P51_CACHE_FD_LEASE_V4_BYTES> lease_wire_v4(
    const P51SourceLeaseRequestFields &value,
    const P51CacheControlIdentity &identity = control_identity_v4())
{
    std::array<uint8_t, P51_CACHE_FD_LEASE_V4_BYTES> wire{};
    put32(wire, 0, P50_CACHE_FD_LEASE_MAGIC);
    put32(wire, 4, P51_CACHE_FD_LEASE_V4_VERSION);
    put32(wire, 8, value.wire_job_id);
    put64(wire, 12, value.assignment_epoch);
    put64(wire, 20, value.assignment_nonce);
    put32(wire, 28, value.profile);
    put64(wire, 32, identity.control_generation);
    put64(wire, 40, identity.control_attempt);
    put64(wire, 48, identity.peer_uid);
    put64(wire, 56, identity.peer_gid);
    put64(wire, 64, identity.c_store_generation);
    put64(wire, 72, identity.derivation_version);
    std::copy(identity.c_store_guid.begin(), identity.c_store_guid.end(),
              wire.begin() + 80);
    return wire;
}

int receive_raw_with_fd(int socket, uint8_t *bytes, size_t size)
{
    iovec iov{bytes, size};
    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int) * 4)> control{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.data();
    message.msg_controllen = control.size();
    const ssize_t received = ::recvmsg(socket, &message, MSG_WAITALL);
    int transferred = -1;
    size_t fd_count = 0;
    for (cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(sizeof(int)))
            continue;
        const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int *fds = reinterpret_cast<const int *>(CMSG_DATA(cmsg));
        for (size_t i = 0; i != count; ++i) {
            if (fd_count++ == 0)
                transferred = fds[i];
            else
                ::close(fds[i]);
        }
    }
    if (received != static_cast<ssize_t>(size) || fd_count != 1 ||
        (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) {
        if (transferred >= 0)
            ::close(transferred);
        return -1;
    }
    return transferred;
}

void test_legacy_v3_lease_fixture_is_byte_exact()
{
    /* Pin the deployable v3/64-byte reply before adding the separately
       versioned P51/v4 lease.  The future extension must not silently rewrite
       this R1 local record: its only v4-prefix difference is the version word. */
    constexpr std::array<uint8_t, P50_CACHE_FD_LEASE_BYTES> expected{
        0x50, 0x35, 0x46, 0x4c, 0x00, 0x00, 0x00, 0x03,
        0x12, 0x34, 0x56, 0x78,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x00, 0x00, 0x00, 0x02,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x07,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0xb9};
    CHECK(P50_CACHE_FD_LEASE_VERSION == 3);
    CHECK(lease_wire(request()) == expected);
}

void test_v4_lease_fixture_and_scm_rights_roundtrip()
{
    static_assert(P50_CACHE_FD_LEASE_V3_VERSION == 3);
    static_assert(P50_CACHE_FD_LEASE_V3_BYTES == 64);
    static_assert(P51_CACHE_FD_LEASE_V4_VERSION == 4);
    static_assert(P51_CACHE_FD_LEASE_V4_BYTES == 96);
    const auto expected = lease_wire_v4(request_v4());
    const std::array<uint8_t, P51_CACHE_FD_LEASE_V4_BYTES> golden{
        0x50, 0x35, 0x46, 0x4c, 0x00, 0x00, 0x00, 0x04,
        0x12, 0x34, 0x56, 0x78,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x00, 0x00, 0x00, 0x02,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x07,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d, 0xb9,
        0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x10, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
        0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f};
    CHECK(expected == golden);

    ChannelPair pair = make_pair(51);
    const auto request_fields = request_v4();
    P51SourceLeaseRequestMsg outbound(request_fields);
    CHECK(pair.left->send_msg(outbound));
    Msg *decoded = pair.right->get_msg(2, true);
    auto *typed = dynamic_cast<P51SourceLeaseRequestMsg *>(decoded);
    CHECK(typed != nullptr && typed->request == request_fields);
    P51CacheFdReplyTicket ticket = typed == nullptr
        ? P51CacheFdReplyTicket{}
        : pair.right->take_p51_cache_fd_reply_ticket(*typed);
    CHECK(ticket.valid());

    char path[] = "/tmp/icecc-p51-fd-XXXXXX";
    const int source = ::mkstemp(path);
    CHECK(source >= 0);
    const char contents[] = "v4 lease descriptor survives unlink";
    CHECK(::write(source, contents, sizeof(contents)) ==
          static_cast<ssize_t>(sizeof(contents)));
    CHECK(::unlink(path) == 0);
    CHECK(pair.right->send_p51_cache_fd_reply(
        std::move(ticket), control_identity_v4(), source, deadline()));
    delete decoded;

    std::array<uint8_t, P51_CACHE_FD_LEASE_V4_BYTES> received_wire{};
    const int received = receive_raw_with_fd(
        pair.left->fd, received_wire.data(), received_wire.size());
    CHECK(received_wire == golden);
    CHECK(received >= 0);
    if (received >= 0) {
        CHECK(::lseek(received, 0, SEEK_SET) == 0);
        char readback[sizeof(contents)]{};
        CHECK(::read(received, readback, sizeof(readback)) ==
              static_cast<ssize_t>(sizeof(readback)));
        CHECK(std::memcmp(readback, contents, sizeof(contents)) == 0);
        ::close(received);
    }
    CHECK(::fcntl(source, F_GETFD) == -1 && errno == EBADF);
}

void send_raw(int socket, const uint8_t *bytes, size_t size,
              const int *fds, size_t fd_count)
{
    iovec iov{const_cast<uint8_t *>(bytes), size};
    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int) * 4)> control{};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (fd_count != 0) {
        message.msg_control = control.data();
        message.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
        auto *cmsg = reinterpret_cast<cmsghdr *>(control.data());
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
        std::memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
    }
    CHECK(::sendmsg(socket, &message, MSG_NOSIGNAL) ==
          static_cast<ssize_t>(size));
}

void send_plain(int socket, const uint8_t *bytes, size_t size)
{
    size_t offset = 0;
    while (offset != size) {
        const ssize_t sent = ::send(socket, bytes + offset, size - offset,
                                    MSG_NOSIGNAL);
        CHECK(sent > 0);
        offset += static_cast<size_t>(sent);
    }
}

void test_v4_lease_receiver_binds_full_request_and_exact_shape()
{
    const auto request_fields = request_v4();
    const auto valid_wire = lease_wire_v4(request_fields);
    const auto probe = [&](const P51SourceLeaseRequestFields &expected,
                           const uint8_t *bytes, size_t size) {
        ChannelPair pair = make_pair(51);
        CHECK(pair.left->send_msg(P51SourceLeaseRequestMsg(request_fields)));
        Msg *decoded = pair.right->get_msg(2, true);
        CHECK(dynamic_cast<P51SourceLeaseRequestMsg *>(decoded) != nullptr);
        const int source = ::open("/dev/null", O_RDONLY);
        CHECK(source >= 0);
        send_raw(pair.right->fd, bytes, size, &source, 1);
        P51CacheControlIdentity observed;
        const int received = pair.left->receive_p51_cache_fd_reply(
            expected, observed, deadline());
        ::close(source);
        delete decoded;
        return std::pair<int, P51CacheControlIdentity>{received, observed};
    };

    auto [received, identity] =
        probe(request_fields, valid_wire.data(), valid_wire.size());
    CHECK(received >= 0 && identity == control_identity_v4());
    if (received >= 0)
        ::close(received);

    auto wrong_window = request_fields;
    ++wrong_window.requested_window;
    auto [wrong_fd, wrong_identity] =
        probe(wrong_window, valid_wire.data(), valid_wire.size());
    CHECK(wrong_fd == -1 && !wrong_identity.valid());

    const auto legacy_v3 = lease_wire(request());
    auto [legacy_fd, legacy_identity] =
        probe(request_fields, legacy_v3.data(), legacy_v3.size());
    CHECK(legacy_fd == -1 && !legacy_identity.valid());

    auto wrong_version = valid_wire;
    wrong_version[7] = static_cast<uint8_t>(P50_CACHE_FD_LEASE_V3_VERSION);
    auto [wrong_version_fd, wrong_version_identity] =
        probe(request_fields, wrong_version.data(), wrong_version.size());
    CHECK(wrong_version_fd == -1 && !wrong_version_identity.valid());

    auto [short_fd, short_identity] = probe(
        request_fields, valid_wire.data(), valid_wire.size() - 1);
    CHECK(short_fd == -1 && !short_identity.valid());

    std::array<uint8_t, P51_CACHE_FD_LEASE_V4_BYTES + 1> overlong{};
    std::copy(valid_wire.begin(), valid_wire.end(), overlong.begin());
    auto [long_fd, long_identity] =
        probe(request_fields, overlong.data(), overlong.size());
    CHECK(long_fd == -1 && !long_identity.valid());
}

void test_v4_reply_backpressure_is_bounded_and_consumes_fd()
{
    ChannelPair pair = make_pair(51);
    const auto request_fields = request_v4();
    CHECK(pair.left->send_msg(P51SourceLeaseRequestMsg(request_fields)));
    Msg *decoded = pair.right->get_msg(2, true);
    auto *typed = dynamic_cast<P51SourceLeaseRequestMsg *>(decoded);
    CHECK(typed != nullptr);
    P51CacheFdReplyTicket ticket = typed == nullptr
        ? P51CacheFdReplyTicket{}
        : pair.right->take_p51_cache_fd_reply_ticket(*typed);
    CHECK(ticket.valid());

    int send_buffer = 1024;
    CHECK(::setsockopt(pair.right->fd, SOL_SOCKET, SO_SNDBUF, &send_buffer,
                       sizeof(send_buffer)) == 0);
    const std::array<uint8_t, 1024> filler{};
    size_t filled = 0;
    for (;;) {
        const ssize_t count = ::send(pair.right->fd, filler.data(),
                                     filler.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count > 0) {
            filled += static_cast<size_t>(count);
            continue;
        }
        CHECK(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        break;
    }
    CHECK(filled != 0);

    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    const auto started = std::chrono::steady_clock::now();
    const bool sent = pair.right->send_p51_cache_fd_reply(
        std::move(ticket), control_identity_v4(), source, deadline());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(!sent);
    CHECK(elapsed < std::chrono::milliseconds(100));
    CHECK(!ticket.valid());
    CHECK(::fcntl(source, F_GETFD) == -1 && errno == EBADF);
    delete decoded;
}

void test_v4_partial_reply_invalidates_receive_ticket()
{
    ChannelPair pair = make_pair(51);
    const auto request_fields = request_v4();
    CHECK(pair.left->send_msg(P51SourceLeaseRequestMsg(request_fields)));
    Msg *decoded = pair.right->get_msg(2, true);
    auto *typed = dynamic_cast<P51SourceLeaseRequestMsg *>(decoded);
    CHECK(typed != nullptr);
    P51CacheFdReplyTicket ticket = typed == nullptr
        ? P51CacheFdReplyTicket{}
        : pair.right->take_p51_cache_fd_reply_ticket(*typed);
    CHECK(ticket.valid());

    const auto wire = lease_wire_v4(request_fields);
    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    send_raw(pair.right->fd, wire.data(), wire.size() / 2, &source, 1);
    const auto short_deadline = std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(20);
    P51CacheControlIdentity observed;
    CHECK(pair.left->receive_p51_cache_fd_reply(
              request_fields, observed, short_deadline) == -1);
    CHECK(!observed.valid());
    send_plain(pair.right->fd, wire.data() + wire.size() / 2,
               wire.size() - wire.size() / 2);
    const auto retry_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(1);
    CHECK(pair.left->receive_p51_cache_fd_reply(
              request_fields, observed, retry_deadline) == -1);
    CHECK(!observed.valid());
    ::close(source);
    delete decoded;
}

void test_request_and_deleted_source()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
    P50CacheControlIdentity observed;
    CHECK(pair.left->receive_p50_cache_fd_reply(
              expected, observed, deadline()) == -1);
    CHECK(!observed.valid());
    P50CacheSessionFdRequestMsg outbound(expected);
    CHECK(pair.left->send_msg(outbound));
    Msg *decoded = pair.right->get_msg(2, true);
    auto *typed = dynamic_cast<P50CacheSessionFdRequestMsg *>(decoded);
    CHECK(typed != nullptr && typed->request == expected);

    char path[] = "/tmp/icecc-p50-fd-XXXXXX";
    const int source = ::mkstemp(path);
    CHECK(source >= 0);
    const char contents[] = "source survives unlink";
    CHECK(::write(source, contents, sizeof(contents)) == sizeof(contents));
    CHECK(::unlink(path) == 0);
    CHECK(pair.right->send_p50_cache_fd_reply(
        *typed, control_identity(), source, deadline()));
    delete decoded;

    const int received = pair.left->receive_p50_cache_fd_reply(
        expected, observed, deadline());
    CHECK(received >= 0);
    CHECK(observed == control_identity());
    CHECK(::lseek(received, 0, SEEK_SET) == 0);
    char readback[sizeof(contents)]{};
    CHECK(::read(received, readback, sizeof(readback)) == sizeof(readback));
    CHECK(std::memcmp(readback, contents, sizeof(contents)) == 0);
    ::close(received);
    CHECK(pair.left->receive_p50_cache_fd_reply(
              expected, observed, deadline()) == -1);
    CHECK(!observed.valid());
}

void test_wrong_echo_consumes_reply_arm()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
    P50CacheSessionFdRequestMsg outbound(expected);
    CHECK(pair.left->send_msg(outbound));
    Msg *decoded = pair.right->get_msg(2, true);
    auto *typed = dynamic_cast<P50CacheSessionFdRequestMsg *>(decoded);
    CHECK(typed != nullptr);
    auto wrong = expected;
    ++wrong.wire_job_id;
    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    CHECK(!pair.right->send_p50_cache_fd_reply(
        P50CacheSessionFdRequestMsg(wrong), control_identity(), source,
        deadline()));
    CHECK(::fcntl(source, F_GETFD) == -1 && errno == EBADF);
    const int replacement = ::open("/dev/null", O_RDONLY);
    CHECK(replacement >= 0);
    CHECK(!pair.right->send_p50_cache_fd_reply(
        *typed, control_identity(), replacement, deadline()));
    CHECK(::fcntl(replacement, F_GETFD) == -1 && errno == EBADF);
    delete decoded;
}

void test_control_identity_is_required()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
    P50CacheSessionFdRequestMsg outbound(expected);
    CHECK(pair.left->send_msg(outbound));
    Msg *decoded = pair.right->get_msg(2, true);
    auto *typed = dynamic_cast<P50CacheSessionFdRequestMsg *>(decoded);
    CHECK(typed != nullptr);
    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    CHECK(!pair.right->send_p50_cache_fd_reply(
        *typed, P50CacheControlIdentity{}, source, deadline()));
    CHECK(::fcntl(source, F_GETFD) == -1 && errno == EBADF);
    delete decoded;

    ChannelPair raw_pair = make_pair();
    P50CacheSessionFdRequestMsg raw_outbound(expected);
    CHECK(raw_pair.left->send_msg(raw_outbound));
    Msg *raw_decoded = raw_pair.right->get_msg(2, true);
    CHECK(dynamic_cast<P50CacheSessionFdRequestMsg *>(raw_decoded) != nullptr);
    const auto wire = lease_wire(expected, P50CacheControlIdentity{});
    const int raw_source = ::open("/dev/null", O_RDONLY);
    CHECK(raw_source >= 0);
    send_raw(raw_pair.right->fd, wire.data(), wire.size(), &raw_source, 1);
    P50CacheControlIdentity observed;
    CHECK(raw_pair.left->receive_p50_cache_fd_reply(
              expected, observed, deadline()) == -1);
    CHECK(!observed.valid());
    ::close(raw_source);
    delete raw_decoded;
}

void test_extra_fd_and_trailing_bytes_are_rejected()
{
    for (int extra = 0; extra != 2; ++extra) {
        ChannelPair pair = make_pair();
        const auto expected = request();
        const auto wire = lease_wire(expected);
        const int first = ::open("/dev/null", O_RDONLY);
        const int second = ::open("/dev/null", O_RDONLY);
        CHECK(first >= 0 && second >= 0);
        if (extra == 0) {
            const uint8_t trailing = 0x7f;
            std::array<uint8_t, P50_CACHE_FD_LEASE_BYTES + 1> bytes{};
            std::copy(wire.begin(), wire.end(), bytes.begin());
            bytes.back() = trailing;
            send_raw(pair.right->fd, bytes.data(), bytes.size(), &first, 1);
        } else {
            const int fds[2] = {first, second};
            send_raw(pair.right->fd, wire.data(), wire.size(), fds, 2);
        }
        P50CacheControlIdentity observed;
        CHECK(pair.left->receive_p50_cache_fd_reply(
                  expected, observed, deadline()) == -1);
        CHECK(!observed.valid());
        ::close(first);
        ::close(second);
    }

    ChannelPair pair = make_pair();
    const auto expected = request();
    auto wrong = lease_wire(expected);
    ++wrong[8];
    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    send_raw(pair.right->fd, wrong.data(), wrong.size(), &source, 1);
    P50CacheControlIdentity observed;
    CHECK(pair.left->receive_p50_cache_fd_reply(
              expected, observed, deadline()) == -1);
    ::close(source);
}

void test_short_reply_is_rejected()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
    const auto wire = lease_wire(expected);
    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    send_raw(pair.right->fd, wire.data(), 7, &source, 1);
    P50CacheControlIdentity observed;
    CHECK(pair.left->receive_p50_cache_fd_reply(
              expected, observed, deadline()) == -1);
    ::close(source);
}

void test_dirty_sender_boundary_is_rejected()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
    P50CacheSessionFdRequestMsg outbound(expected);
    CHECK(pair.left->send_msg(outbound));
    Msg *decoded = pair.right->get_msg(2, true);
    auto *typed = dynamic_cast<P50CacheSessionFdRequestMsg *>(decoded);
    CHECK(typed != nullptr);
    const uint8_t dirty = 0x42;
    CHECK(::send(pair.left->fd, &dirty, sizeof(dirty), MSG_NOSIGNAL) == 1);
    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    CHECK(!pair.right->send_p50_cache_fd_reply(
        *typed, control_identity(), source, deadline()));
    CHECK(::fcntl(source, F_GETFD) == -1 && errno == EBADF);
    ::close(source);
    delete decoded;
}

void test_chunked_reply_and_receive_deadline()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
    P50CacheSessionFdRequestMsg outbound(expected);
    CHECK(pair.left->send_msg(outbound));
    Msg *decoded = pair.right->get_msg(2, true);
    CHECK(dynamic_cast<P50CacheSessionFdRequestMsg *>(decoded) != nullptr);

    const auto wire = lease_wire(expected);
    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    std::thread writer([&] {
        send_raw(pair.right->fd, wire.data(), 7, &source, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        send_plain(pair.right->fd, wire.data() + 7, wire.size() - 7);
    });
    P50CacheControlIdentity observed;
    const int received = pair.left->receive_p50_cache_fd_reply(
        expected, observed, deadline());
    writer.join();
    CHECK(received >= 0);
    CHECK(observed == control_identity());
    ::close(received);
    ::close(source);
    delete decoded;
}

void test_receive_eagain_consumes_arm()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
    P50CacheSessionFdRequestMsg outbound(expected);
    CHECK(pair.left->send_msg(outbound));
    Msg *decoded = pair.right->get_msg(2, true);
    auto *typed = dynamic_cast<P50CacheSessionFdRequestMsg *>(decoded);
    CHECK(typed != nullptr);
    P50CacheControlIdentity observed;
    CHECK(pair.left->receive_p50_cache_fd_reply(
              expected, observed, std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(20)) == -1);
    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    CHECK(pair.right->send_p50_cache_fd_reply(
        *typed, control_identity(), source, deadline()));
    CHECK(pair.left->receive_p50_cache_fd_reply(
              expected, observed, deadline()) == -1);
    delete decoded;
}

void test_sender_deadline_consumes_fd()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
    P50CacheSessionFdRequestMsg outbound(expected);
    CHECK(pair.left->send_msg(outbound));
    Msg *decoded = pair.right->get_msg(2, true);
    auto *typed = dynamic_cast<P50CacheSessionFdRequestMsg *>(decoded);
    CHECK(typed != nullptr);
    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    CHECK(!pair.right->send_p50_cache_fd_reply(
        *typed, control_identity(), source,
        std::chrono::steady_clock::now()));
    CHECK(::fcntl(source, F_GETFD) == -1 && errno == EBADF);
    delete decoded;
}

void test_deferred_reply_ticket_survives_only_empty_probe()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
    P50CacheSessionFdRequestMsg outbound(expected);
    CHECK(pair.left->send_msg(outbound));
    Msg *decoded = pair.right->get_msg(2, true);
    auto *typed = dynamic_cast<P50CacheSessionFdRequestMsg *>(decoded);
    CHECK(typed != nullptr);
    P50CacheFdReplyTicket ticket =
        pair.right->take_p50_cache_fd_reply_ticket(*typed);
    CHECK(ticket.valid());
    delete decoded;

    /* iceccd's ordinary drain loop performs this exact no-byte probe after
       every handled frame.  It must not revoke an explicitly retained ticket. */
    CHECK(pair.right->read_a_bit());
    const int source = ::open("/dev/null", O_RDONLY);
    CHECK(source >= 0);
    CHECK(pair.right->send_p50_cache_fd_reply(
        std::move(ticket), control_identity(), source, deadline()));
    P50CacheControlIdentity observed;
    const int received = pair.left->receive_p50_cache_fd_reply(
        expected, observed, deadline());
    CHECK(received >= 0 && observed == control_identity());
    ::close(received);

    ChannelPair dirty_pair = make_pair();
    P50CacheSessionFdRequestMsg dirty_outbound(expected);
    CHECK(dirty_pair.left->send_msg(dirty_outbound));
    Msg *dirty_decoded = dirty_pair.right->get_msg(2, true);
    auto *dirty_typed =
        dynamic_cast<P50CacheSessionFdRequestMsg *>(dirty_decoded);
    CHECK(dirty_typed != nullptr);
    P50CacheFdReplyTicket dirty_ticket =
        dirty_pair.right->take_p50_cache_fd_reply_ticket(*dirty_typed);
    CHECK(dirty_ticket.valid());
    delete dirty_decoded;
    CHECK(dirty_pair.left->send_msg(PingMsg()));
    const int dirty_source = ::open("/dev/null", O_RDONLY);
    CHECK(dirty_source >= 0);
    CHECK(!dirty_pair.right->send_p50_cache_fd_reply(
        std::move(dirty_ticket), control_identity(), dirty_source, deadline()));
    CHECK(::fcntl(dirty_source, F_GETFD) == -1 && errno == EBADF);
}

void test_ordinary_mutation_clears_receive_arm()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
    P50CacheSessionFdRequestMsg outbound(expected);
    CHECK(pair.left->send_msg(outbound));
    PingMsg ping;
    CHECK(pair.left->send_msg(ping));
    P50CacheControlIdentity observed;
    CHECK(pair.left->receive_p50_cache_fd_reply(
              expected, observed, deadline()) == -1);
}

} // namespace

int main()
{
    try {
        test_legacy_v3_lease_fixture_is_byte_exact();
        test_v4_lease_fixture_and_scm_rights_roundtrip();
        test_v4_lease_receiver_binds_full_request_and_exact_shape();
        test_v4_reply_backpressure_is_bounded_and_consumes_fd();
        test_v4_partial_reply_invalidates_receive_ticket();
        test_request_and_deleted_source();
        test_wrong_echo_consumes_reply_arm();
        test_control_identity_is_required();
        test_extra_fd_and_trailing_bytes_are_rejected();
        test_short_reply_is_rejected();
        test_dirty_sender_boundary_is_rejected();
        test_chunked_reply_and_receive_deadline();
        test_receive_eagain_consumes_arm();
        test_sender_deadline_consumes_fd();
        test_deferred_reply_ticket_survives_only_empty_probe();
        test_ordinary_mutation_clears_receive_arm();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "p50 cache fd seam test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
