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

ChannelPair make_pair()
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
    pair.left->protocol = pair.right->protocol = PROTOCOL_VERSION;
    return pair;
}

P50CacheSessionFdRequestFields request()
{
    return P50CacheSessionFdRequestFields{
        0x12345678, UINT64_C(0x1020304050607080),
        UINT64_C(0x8877665544332211), CACHE_PROFILE_ZSTD_TU};
}

void put32(std::array<uint8_t, P50_CACHE_FD_LEASE_BYTES> &wire,
           size_t offset, uint32_t value)
{
    wire[offset] = static_cast<uint8_t>(value >> 24);
    wire[offset + 1] = static_cast<uint8_t>(value >> 16);
    wire[offset + 2] = static_cast<uint8_t>(value >> 8);
    wire[offset + 3] = static_cast<uint8_t>(value);
}

void put64(std::array<uint8_t, P50_CACHE_FD_LEASE_BYTES> &wire,
           size_t offset, uint64_t value)
{
    for (size_t i = 0; i != 8; ++i)
        wire[offset + i] = static_cast<uint8_t>(value >> (56 - i * 8));
}

std::array<uint8_t, P50_CACHE_FD_LEASE_BYTES> lease_wire(
    const P50CacheSessionFdRequestFields &value)
{
    std::array<uint8_t, P50_CACHE_FD_LEASE_BYTES> wire{};
    put32(wire, 0, P50_CACHE_FD_LEASE_MAGIC);
    put32(wire, 4, P50_CACHE_FD_LEASE_VERSION);
    put32(wire, 8, value.wire_job_id);
    put64(wire, 12, value.assignment_epoch);
    put64(wire, 20, value.assignment_nonce);
    put32(wire, 28, value.profile);
    return wire;
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

void test_request_and_deleted_source()
{
    ChannelPair pair = make_pair();
    const auto expected = request();
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
    CHECK(pair.right->send_p50_cache_fd_reply(*typed, source));
    delete decoded;

    const int received = pair.left->receive_p50_cache_fd_reply(expected);
    CHECK(received >= 0);
    CHECK(::lseek(received, 0, SEEK_SET) == 0);
    char readback[sizeof(contents)]{};
    CHECK(::read(received, readback, sizeof(readback)) == sizeof(readback));
    CHECK(std::memcmp(readback, contents, sizeof(contents)) == 0);
    ::close(received);
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
        P50CacheSessionFdRequestMsg(wrong), source));
    CHECK(!pair.right->send_p50_cache_fd_reply(*typed, source));
    ::close(source);
    delete decoded;
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
        CHECK(pair.left->receive_p50_cache_fd_reply(expected) == -1);
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
    CHECK(pair.left->receive_p50_cache_fd_reply(expected) == -1);
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
    CHECK(pair.left->receive_p50_cache_fd_reply(expected) == -1);
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
    CHECK(!pair.right->send_p50_cache_fd_reply(*typed, source));
    ::close(source);
    delete decoded;
}

} // namespace

int main()
{
    try {
        test_request_and_deleted_source();
        test_wrong_echo_consumes_reply_arm();
        test_extra_fd_and_trailing_bytes_are_rejected();
        test_short_reply_is_rejected();
        test_dirty_sender_boundary_is_rejected();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "p50 cache fd seam test failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
