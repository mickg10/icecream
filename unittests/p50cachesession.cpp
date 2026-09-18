/* Protocol-50 ordinary-link CACHE_SESSION and descriptor handoff gate. */
#include "comm.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static int failures = 0;

#define REQUIRE(condition, text) do {                                  \
    if (condition) std::fprintf(stderr, "ok       - %s\n", text);      \
    else { std::fprintf(stderr, "FAILED   - %s\n", text); ++failures; } \
} while (0)

struct Pair {
    MsgChannel *left = nullptr;
    MsgChannel *right = nullptr;
    Pair() = default;
    Pair(const Pair &) = delete;
    Pair &operator=(const Pair &) = delete;
    Pair(Pair &&other) noexcept
        : left(other.left), right(other.right)
    {
        other.left = other.right = nullptr;
    }
    ~Pair() { delete left; delete right; }
};

static Pair make_pair(int protocol)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) std::exit(2);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    Pair pair;
    std::thread left([&] {
        pair.left = Service::createChannel(
            fds[0], reinterpret_cast<sockaddr *>(&address), sizeof(address));
    });
    std::thread right([&] {
        pair.right = Service::createChannel(
            fds[1], reinterpret_cast<sockaddr *>(&address), sizeof(address));
    });
    left.join();
    right.join();
    if (!pair.left || !pair.right) std::exit(2);
    pair.left->protocol = pair.right->protocol = protocol;
    return pair;
}

using Bytes = std::vector<unsigned char>;

static constexpr std::array<unsigned char, 8> kCacheSessionFixture{
    0x00, 0x00, 0x00, 0x04, 0x50, 0xf0, 0x00, 0x00};
static constexpr std::array<unsigned char, 4> kCacheSessionReadyFixture{
    0x50, 0xf0, 0x00, 0x01};
static constexpr std::array<unsigned char, 8> kPingFixture{
    0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x42};

static Bytes frame(Msg::Value type)
{
    const uint32_t length = htonl(4);
    const uint32_t wire_type = htonl(static_cast<uint32_t>(type));
    Bytes result(8);
    std::memcpy(result.data(), &length, sizeof(length));
    std::memcpy(result.data() + 4, &wire_type, sizeof(wire_type));
    return result;
}

static void send_bytes(int fd, const Bytes &bytes)
{
    size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t sent = send(fd, bytes.data() + offset,
                                  bytes.size() - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        std::perror("send");
        std::exit(2);
    }
}

static Msg *decode_cache_session(Pair &pair)
{
    send_bytes(pair.left->fd, frame(Msg::CACHE_SESSION));
    return pair.right->get_msg(2, true);
}

static void test_successful_transfer_and_exact_once()
{
    Pair pair = make_pair(50);
    Msg *decoded = decode_cache_session(pair);
    REQUIRE(decoded && *decoded == Msg::CACHE_SESSION
                && dynamic_cast<CacheSessionMsg *>(decoded) != nullptr,
            "P50 CACHE_SESSION decodes as the empty-payload discriminator");
    delete decoded;

    const int old_fd = pair.right->fd;
    const int released = pair.right->release_fd_if_input_empty();
    REQUIRE(released == old_fd && pair.right->fd == -1,
            "clean P50 CACHE_SESSION transfers the still-owned descriptor");
    REQUIRE(pair.right->release_fd_if_input_empty() == -1,
            "descriptor transfer is one-shot");

    /* Destroying the old parser must not close the transferred descriptor. */
    delete pair.right;
    pair.right = nullptr;
    const Bytes continuity{'C', 'W', 1};
    send_bytes(pair.left->fd, continuity);
    Bytes received(continuity.size());
    const ssize_t count = recv(released, received.data(), received.size(), 0);
    REQUIRE(count == ssize_t(received.size()) && received == continuity,
            "transferred fd remains usable after MsgChannel destruction");
    close(released);
}

static void test_outbound_transfer_and_exact_once()
{
    Pair pair = make_pair(50);
    const int client_fd = pair.left->fd;
    REQUIRE(pair.left->send_msg(CacheSessionMsg()),
            "client flushes the exact P50 CACHE_SESSION boundary");

    int released_server = -1;
    bool server_decoded = false;
    bool no_early_cachewire = false;
    bool ready_sent = false;
    std::thread sidecar([&] {
        Msg *decoded = pair.right->get_msg(2, true);
        server_decoded = decoded && *decoded == Msg::CACHE_SESSION;
        delete decoded;
        released_server = pair.right->release_fd_if_input_empty();
        if (released_server < 0)
            return;
        unsigned char byte = 0;
        const ssize_t early = recv(released_server, &byte, sizeof(byte),
                                   MSG_PEEK | MSG_DONTWAIT);
        no_early_cachewire =
            early < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        ready_sent = send_cache_session_ready(
            released_server,
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
    });
    const int released_client =
        pair.left->release_fd_after_cache_session_ready(
            std::chrono::steady_clock::now() + std::chrono::seconds(2));
    sidecar.join();
    REQUIRE(server_decoded && released_server >= 0 && ready_sent,
            "F decodes, owns, and acknowledges the exact detached descriptor");
    REQUIRE(no_early_cachewire,
            "client emits no CacheWire byte before the sidecar READY witness");
    REQUIRE(released_client == client_fd && pair.left->fd == -1,
            "exact sidecar READY transfers the client descriptor");
    REQUIRE(pair.left->release_fd_after_cache_session_ready(
                std::chrono::steady_clock::now() + std::chrono::milliseconds(10)) == -1,
            "outbound descriptor transfer is one-shot");

    delete pair.left;
    pair.left = nullptr;
    delete pair.right;
    pair.right = nullptr;
    const Bytes cachewire{'P', '5', '0'};
    send_bytes(released_client, cachewire);
    Bytes observed(cachewire.size());
    REQUIRE(recv(released_server, observed.data(), observed.size(), 0) ==
                static_cast<ssize_t>(observed.size()) && observed == cachewire,
            "both transferred descriptors preserve exact CacheWire continuity");
    close(released_client);
    close(released_server);
}

static void test_outbound_release_barriers()
{
    {
        Pair pair = make_pair(50);
        REQUIRE(pair.left->send_msg(CacheSessionMsg(), MsgChannel::SendBulkOnly),
                "test queues an outbound CACHE_SESSION without flushing");
        const int owned = pair.left->fd;
        REQUIRE(pair.left->release_fd_after_cache_session_ready(
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(10)) == -1 &&
                    pair.left->fd == owned,
                "queued outbound bytes never arm descriptor release");
        REQUIRE(pair.left->flush_pending(),
                "queued outbound CACHE_SESSION can drain normally");
        REQUIRE(pair.left->release_fd_after_cache_session_ready(
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(10)) == -1,
                "a later generic flush cannot resurrect the send arm");
    }
    {
        Pair pair = make_pair(50);
        REQUIRE(pair.left->send_msg(PingMsg(), MsgChannel::SendBulkOnly),
                "test queues an earlier ordinary frame");
        REQUIRE(!pair.left->send_msg(CacheSessionMsg()),
                "CACHE_SESSION is refused behind earlier queued output");
        REQUIRE(pair.left->release_fd_after_cache_session_ready(
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(10)) == -1,
                "refused mixed output retains descriptor ownership");
    }
    {
        Pair pair = make_pair(50);
        REQUIRE(pair.left->send_msg(CacheSessionMsg()),
                "test arms outbound release before a parser use");
        REQUIRE(pair.left->get_msg(0, true) == nullptr,
                "a nonblocking receive attempt observes no reply");
        REQUIRE(pair.left->release_fd_after_cache_session_ready(
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(10)) == -1,
                "any later receive attempt clears the outbound arm");
    }
    {
        Pair pair = make_pair(50);
        REQUIRE(pair.left->send_msg(CacheSessionMsg()),
                "test arms outbound release before peer EOF");
        shutdown(pair.right->fd, SHUT_WR);
        REQUIRE(pair.left->release_fd_after_cache_session_ready(
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(100)) == -1,
                "peer EOF blocks outbound descriptor release");
    }
}

static void test_ready_wire_and_failure_boundaries()
{
    {
        int sockets[2] = {-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
                "READY fixture socketpair is available");
        REQUIRE(send_cache_session_ready(
                    sockets[0], std::chrono::steady_clock::now() + std::chrono::seconds(1)),
                "sidecar sends the fixed READY transition token");
        std::array<unsigned char, 4> observed{};
        REQUIRE(recv(sockets[1], observed.data(), observed.size(), 0) ==
                    static_cast<ssize_t>(observed.size()) &&
                    observed == kCacheSessionReadyFixture,
                "READY has the exact raw network-order wire fixture");
        close(sockets[0]);
        close(sockets[1]);
    }
    {
        int sockets[2] = {-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
                "closed-peer READY socketpair is available");
        close(sockets[1]);
        REQUIRE(!send_cache_session_ready(
                    sockets[0], std::chrono::steady_clock::now() + std::chrono::seconds(1)),
                "READY send fails without SIGPIPE when the client is gone");
        close(sockets[0]);
    }
    {
        Pair pair = make_pair(50);
        REQUIRE(pair.left->send_msg(CacheSessionMsg()),
                "missing-READY row flushes CACHE_SESSION");
        Msg *decoded = pair.right->get_msg(2, true);
        delete decoded;
        const int sidecar_fd = pair.right->release_fd_if_input_empty();
        const int owned = pair.left->fd;
        const auto started = std::chrono::steady_clock::now();
        REQUIRE(pair.left->release_fd_after_cache_session_ready(
                    started + std::chrono::milliseconds(30)) == -1 &&
                    pair.left->fd == owned &&
                    std::chrono::steady_clock::now() - started <
                        std::chrono::milliseconds(250),
                "missing READY fails by the unchanged absolute deadline and retains ownership");
        REQUIRE(send_cache_session_ready(
                    sidecar_fd,
                    std::chrono::steady_clock::now() + std::chrono::seconds(1)),
                "late READY can still be placed on the failed connection");
        REQUIRE(pair.left->release_fd_after_cache_session_ready(
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(100)) == -1 &&
                    pair.left->fd == owned,
                "late READY cannot resurrect the consumed one-shot arm");
        close(sidecar_fd);
    }
    {
        Pair pair = make_pair(50);
        REQUIRE(pair.left->send_msg(CacheSessionMsg()),
                "wrong-READY row flushes CACHE_SESSION");
        Msg *decoded = pair.right->get_msg(2, true);
        delete decoded;
        const int sidecar_fd = pair.right->release_fd_if_input_empty();
        uint32_t wrong = htonl(CACHE_SESSION_READY_MAGIC ^ UINT32_C(1));
        send_bytes(sidecar_fd,
                   Bytes(reinterpret_cast<unsigned char *>(&wrong),
                         reinterpret_cast<unsigned char *>(&wrong) + sizeof(wrong)));
        const int owned = pair.left->fd;
        REQUIRE(pair.left->release_fd_after_cache_session_ready(
                    std::chrono::steady_clock::now() + std::chrono::seconds(1)) == -1 &&
                    pair.left->fd == owned,
                "wrong READY value fails closed and retains client ownership");
        close(sidecar_fd);
    }
    {
        Pair pair = make_pair(50);
        REQUIRE(pair.left->send_msg(CacheSessionMsg()),
                "partial-READY row flushes CACHE_SESSION");
        Msg *decoded = pair.right->get_msg(2, true);
        delete decoded;
        const int sidecar_fd = pair.right->release_fd_if_input_empty();
        send_bytes(sidecar_fd, Bytes(kCacheSessionReadyFixture.begin(),
                                     kCacheSessionReadyFixture.begin() + 2));
        const int owned = pair.left->fd;
        REQUIRE(pair.left->release_fd_after_cache_session_ready(
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(30)) == -1 &&
                    pair.left->fd == owned,
                "partial READY is bounded, consumed once, and fails closed");
        close(sidecar_fd);
    }
    {
        Pair pair = make_pair(50);
        REQUIRE(pair.left->send_msg(CacheSessionMsg()),
                "READY-read-ahead row flushes CACHE_SESSION");
        Msg *decoded = pair.right->get_msg(2, true);
        delete decoded;
        const int sidecar_fd = pair.right->release_fd_if_input_empty();
        Bytes joined(kCacheSessionReadyFixture.begin(), kCacheSessionReadyFixture.end());
        joined.push_back(0xaa);
        send_bytes(sidecar_fd, joined);
        const int owned = pair.left->fd;
        const int released = pair.left->release_fd_after_cache_session_ready(
            std::chrono::steady_clock::now() + std::chrono::seconds(1));
        REQUIRE(released == owned && pair.left->fd == -1,
                "READY transfers ownership when the endpoint has already written CacheWire");
        unsigned char queued = 0;
        REQUIRE(recv(released, &queued, sizeof(queued), 0) == 1 && queued == 0xaa,
                "queued post-READY CacheWire bytes remain for the endpoint parser");
        close(released);
        close(sidecar_fd);
    }
}

static void test_absolute_deadline_protocol_negotiation()
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) std::exit(2);
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0)
        std::exit(2);
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                    &address_size) != 0)
        std::exit(2);

    std::thread stalled_peer([listener] {
        const int accepted = accept(listener, nullptr, nullptr);
        if (accepted >= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            close(accepted);
        }
        close(listener);
    });
    const auto started = std::chrono::steady_clock::now();
    MsgChannel *channel = Service::createChannelUntil(
        "127.0.0.1", ntohs(address.sin_port),
        started + std::chrono::milliseconds(40));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(channel == nullptr && elapsed < std::chrono::milliseconds(150),
            "absolute channel deadline bounds ordinary protocol negotiation");
    delete channel;
    stalled_peer.join();
}

static void test_absolute_deadline_owns_tcp_user_timeout()
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) std::exit(2);
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0)
        std::exit(2);
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                    &address_size) != 0)
        std::exit(2);

    std::thread peer([listener] {
        sockaddr_in remote{};
        socklen_t remote_size = sizeof(remote);
        const int accepted = accept(
            listener, reinterpret_cast<sockaddr *>(&remote), &remote_size);
        MsgChannel *channel = accepted >= 0
                                  ? Service::createChannel(
                                        accepted,
                                        reinterpret_cast<sockaddr *>(&remote),
                                        remote_size)
                                  : nullptr;
        delete channel;
        close(listener);
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    MsgChannel *channel = Service::createChannelUntil(
        "127.0.0.1", ntohs(address.sin_port), deadline);
    REQUIRE(channel != nullptr,
            "absolute-deadline TCP channel completes protocol negotiation");
#ifdef TCP_USER_TIMEOUT
    int timeout_msec = 0;
    socklen_t timeout_size = sizeof(timeout_msec);
    const bool timeout_read =
        channel != nullptr &&
        getsockopt(channel->fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
                   &timeout_msec, &timeout_size) == 0;
    const auto remaining_msec = std::chrono::duration_cast<
        std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                                    .count();
    REQUIRE(timeout_read && timeout_msec > remaining_msec &&
                timeout_msec > 15000,
            "absolute deadline cannot be pre-empted by the ordinary TCP user timeout");
#else
    REQUIRE(channel != nullptr,
            "platform without TCP_USER_TIMEOUT retains the absolute application deadline");
#endif
    delete channel;
    peer.join();
}

static void test_ordinary_tcp_timeout_and_application_extension()
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) std::exit(2);
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0)
        std::exit(2);
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                    &address_size) != 0)
        std::exit(2);

    std::thread peer([listener] {
        sockaddr_in remote{};
        socklen_t remote_size = sizeof(remote);
        const int accepted = accept(
            listener, reinterpret_cast<sockaddr *>(&remote), &remote_size);
        MsgChannel *channel = accepted >= 0
                                  ? Service::createChannel(
                                        accepted,
                                        reinterpret_cast<sockaddr *>(&remote),
                                        remote_size)
                                  : nullptr;
        delete channel;
        close(listener);
    });

    MsgChannel *channel = Service::createChannel(
        "127.0.0.1", ntohs(address.sin_port), 10);
    REQUIRE(channel != nullptr, "ordinary TCP channel completes admission");
#ifdef TCP_USER_TIMEOUT
    int timeout_msec = 0;
    socklen_t timeout_size = sizeof(timeout_msec);
    REQUIRE(channel != nullptr &&
                getsockopt(channel->fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
                           &timeout_msec, &timeout_size) == 0 &&
                timeout_msec == ICECC_TCP_USER_TIMEOUT_MSEC &&
                timeout_msec > ICECC_DEFERRED_SEND_TIMEOUT_MSEC &&
                timeout_msec > MAX_SCHEDULER_PING * 1000,
            "ordinary TCP timeout cannot pre-empt scheduler liveness owners");

    const auto application_deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes(12);
    REQUIRE(channel != nullptr &&
                channel->setTcpUserTimeoutUntil(application_deadline),
            "an admitted channel accepts the remote-operation deadline");
    timeout_msec = 0;
    timeout_size = sizeof(timeout_msec);
    REQUIRE(channel != nullptr &&
                getsockopt(channel->fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
                           &timeout_msec, &timeout_size) == 0 &&
                timeout_msec > 12 * 60 * 1000,
            "remote-operation timeout remains beyond a 12-minute result wait");
#else
    REQUIRE(channel != nullptr && channel->setTcpUserTimeoutUntil(
                std::chrono::steady_clock::now() + std::chrono::minutes(12)),
            "application deadline remains valid without TCP_USER_TIMEOUT");
#endif
    delete channel;
    peer.join();
}

static void test_same_endpoint_retry_after_complete_slice()
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) std::exit(2);
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener, 4) != 0)
        std::exit(2);
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                    &address_size) != 0)
        std::exit(2);

    std::thread peer([listener] {
        // The first TCP handshake completes, but ordinary protocol
        // negotiation is blackholed past its per-socket slice.
        const int stalled = accept(listener, nullptr, nullptr);
        if (stalled >= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            close(stalled);
        }

        pollfd descriptor{listener, POLLIN, 0};
        int ready = -1;
        do {
            ready = poll(&descriptor, 1, 2000);
        } while (ready < 0 && errno == EINTR);
        sockaddr_in remote{};
        socklen_t remote_size = sizeof(remote);
        const int accepted = ready > 0
                                 ? accept(listener,
                                          reinterpret_cast<sockaddr *>(&remote),
                                          &remote_size)
                                 : -1;
        MsgChannel *channel = accepted >= 0
                                  ? Service::createChannel(
                                        accepted,
                                        reinterpret_cast<sockaddr *>(&remote),
                                        remote_size)
                                  : nullptr;
        delete channel;
        close(listener);
    });

    const auto started = std::chrono::steady_clock::now();
    MsgChannel *channel = Service::createChannelRetryUntil(
        "127.0.0.1", ntohs(address.sin_port),
        started + std::chrono::seconds(20),
        std::chrono::milliseconds(80));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(channel != nullptr && elapsed >= std::chrono::milliseconds(80) &&
                elapsed < std::chrono::milliseconds(300),
            "a slice-expired socket reconnects to the same endpoint inside one deadline");
#ifdef TCP_USER_TIMEOUT
    int timeout_msec = 0;
    socklen_t timeout_size = sizeof(timeout_msec);
    REQUIRE(channel != nullptr &&
                getsockopt(channel->fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
                           &timeout_msec, &timeout_size) == 0 &&
                timeout_msec > 15000,
            "a slice-expired retry restores the successful socket to the outer deadline");
#endif
    delete channel;
    peer.join();
}

static void test_same_endpoint_retry_immediate_success_owns_outer_timeout()
{
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) std::exit(2);
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0)
        std::exit(2);
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                    &address_size) != 0)
        std::exit(2);

    std::thread peer([listener] {
        sockaddr_in remote{};
        socklen_t remote_size = sizeof(remote);
        const int accepted = accept(
            listener, reinterpret_cast<sockaddr *>(&remote), &remote_size);
        MsgChannel *channel = accepted >= 0
                                  ? Service::createChannel(
                                        accepted,
                                        reinterpret_cast<sockaddr *>(&remote),
                                        remote_size)
                                  : nullptr;
        delete channel;
        close(listener);
    });

    MsgChannel *channel = Service::createChannelRetryUntil(
        "127.0.0.1", ntohs(address.sin_port),
        std::chrono::steady_clock::now() + std::chrono::seconds(20),
        std::chrono::milliseconds(80));
    REQUIRE(channel != nullptr,
            "same-endpoint retry helper preserves immediate success");
#ifdef TCP_USER_TIMEOUT
    int timeout_msec = 0;
    socklen_t timeout_size = sizeof(timeout_msec);
    REQUIRE(channel != nullptr &&
                getsockopt(channel->fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
                           &timeout_msec, &timeout_size) == 0 &&
                timeout_msec > 15000,
            "immediate retry-helper success owns the outer TCP timeout");
#endif
    delete channel;
    peer.join();
}

static void test_same_endpoint_retry_preserves_immediate_failure()
{
    const int holder = socket(AF_INET, SOCK_STREAM, 0);
    if (holder < 0) std::exit(2);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(holder, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
        std::exit(2);
    socklen_t address_size = sizeof(address);
    if (getsockname(holder, reinterpret_cast<sockaddr *>(&address),
                    &address_size) != 0)
        std::exit(2);
    close(holder);

    const auto started = std::chrono::steady_clock::now();
    MsgChannel *channel = Service::createChannelRetryUntil(
        "127.0.0.1", ntohs(address.sin_port),
        started + std::chrono::milliseconds(500),
        std::chrono::milliseconds(80));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(channel == nullptr && elapsed < std::chrono::milliseconds(200),
            "an immediate definitive refusal does not spin until the outer deadline");
    delete channel;
}

static void test_adaptive_connection_budget(bool hedged = true)
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    // Independently serviced sockets: a slow greeting on every connection,
    // a blackholed first connection, all blackholed, immediate success, and
    // a viable first greeting that arrives after the retry's remaining budget.
    // The last case must preserve progress: restarting at 750ms cannot finish
    // another 1500ms handshake inside the original 2000ms deadline.
    for (int mode = 0; mode != (hedged ? 8 : 4); ++mode) {
        const int listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (listener < 0 ||
            bind(listener, reinterpret_cast<sockaddr *>(&address), size) ||
            listen(listener, 16) ||
            getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size))
            std::exit(2);
        std::atomic<bool> stop{false};
        std::atomic<unsigned> accepts{0};
        std::atomic<unsigned> stalled_closed{0};
        std::thread peer([&] {
            std::vector<std::thread> workers;
            while (!stop.load()) {
                pollfd descriptor{listener, POLLIN, 0};
                if (poll(&descriptor, 1, 10) <= 0)
                    continue;
                sockaddr_in remote{};
                socklen_t remote_size = sizeof(remote);
                const int fd = accept(listener,
                    reinterpret_cast<sockaddr *>(&remote), &remote_size);
                if (fd < 0)
                    continue;
                const unsigned index = ++accepts;
                workers.emplace_back([&, fd, remote, remote_size, index]() mutable {
                    if ((mode == 5 && index == 1) || mode == 6 ||
                        (mode == 7 && index == 1)) {
                        std::this_thread::sleep_for(150ms);
                        unsigned char proposal[4];
                        // Drain the proposal so this is clean EOF, not RST
                        // caused by closing with unread application input.
                        const ssize_t received = recv(fd, proposal, sizeof(proposal),
                                                      MSG_DONTWAIT);
                        (void)received;
                        if (mode == 7) {
                            const unsigned char invalid_version[4]{};
                            (void)send(fd, invalid_version, sizeof(invalid_version), 0);
                            std::this_thread::sleep_for(50ms);
                        }
                        close(fd);
                        return;
                    }
                    if (mode == 2 || (mode == 1 && index == 1)) {
                        // Observe client cleanup rather than closing the peer
                        // when the test ends, which would conceal leaked losers.
                        const auto close_deadline = Clock::now() + 2s;
                        while (Clock::now() < close_deadline) {
                            unsigned char greeting[16];
                            const ssize_t count = recv(fd, greeting, sizeof(greeting),
                                                       MSG_DONTWAIT);
                            if (count == 0) {
                                ++stalled_closed;
                                break;
                            }
                            if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                                errno != EINTR)
                                break;
                            std::this_thread::sleep_for(5ms);
                        }
                        close(fd);
                        return;
                    }
                    std::this_thread::sleep_for(
                        mode == 4 ? 1500ms : (mode == 0 || mode >= 5) ? 250ms : 10ms);
                    delete Service::createChannel(fd,
                        reinterpret_cast<sockaddr *>(&remote), remote_size);
                });
            }
            for (auto &worker : workers)
                worker.join();
        });
        const auto started = Clock::now();
        const auto deadline = started + (mode == 4 ? 2s : 1s);
        MsgChannel *channel = Service::createChannelRetryUntil(
            "127.0.0.1", ntohs(address.sin_port), deadline,
            mode == 4 ? 750ms : 100ms,
            hedged ? Service::ChannelRetryPolicy::HedgeAfterFirst
                   : Service::ChannelRetryPolicy::RemainingAfterFirst);
        const auto elapsed = Clock::now() - started;
        REQUIRE((channel != nullptr) == (mode != 2 && mode != 6 && mode != 7),
                "adaptive factory admits slow/live peers and refuses all-stalled peers");
        REQUIRE(mode == 4 ? elapsed >= 1400ms && elapsed < 1900ms
                         : mode == 2 ? elapsed >= 900ms && elapsed < 1400ms
                          : elapsed < 900ms,
                "adaptive factory preserves the unchanged absolute deadline");
#ifdef TCP_USER_TIMEOUT
        if (channel) {
            int timeout_msec = 0;
            socklen_t timeout_size = sizeof(timeout_msec);
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(deadline - Clock::now()).count();
            REQUIRE(getsockopt(channel->fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
                               &timeout_msec, &timeout_size) == 0 &&
                        timeout_msec > remaining + 900,
                    "adaptive success restores the outer socket timeout");
        }
#endif
        delete channel;
        stop = true;
        peer.join();
        close(listener);
        REQUIRE(accepts == (mode == 3 ? 1U : 2U),
                "adaptive factory uses at most one fresh same-endpoint connection");
        if (mode == 1 || mode == 2)
            REQUIRE(stalled_closed == (mode == 1 ? 1U : 2U),
                    "factory closes stalled loser and every deadline-expired socket");
    }

    // Keep a non-listening port reserved, avoiding an ephemeral-port reuse race.
    const int holder = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    if (holder < 0 || bind(holder, reinterpret_cast<sockaddr *>(&address), size) ||
        getsockname(holder, reinterpret_cast<sockaddr *>(&address), &size))
        std::exit(2);
    const auto started = Clock::now();
    MsgChannel *channel = Service::createChannelRetryUntil(
        "127.0.0.1", ntohs(address.sin_port), started + 1s, 100ms,
        Service::ChannelRetryPolicy::HedgeAfterFirst);
    REQUIRE(!channel && Clock::now() - started < 500ms,
            "adaptive immediate refusal does not consume or renew the budget");
    delete channel;
    close(holder);
}

static void test_other_message_refusal_and_arm_clear()
{
    Pair pair = make_pair(50);
    REQUIRE(pair.left->send_msg(PingMsg()), "ordinary PING remains sendable on P50");
    Msg *ping = pair.right->get_msg(2, true);
    REQUIRE(ping && *ping == Msg::PING,
            "ordinary PING decodes at a clean boundary");
    delete ping;
    const int owned = pair.right->fd;
    REQUIRE(pair.right->release_fd_if_input_empty() == -1 && pair.right->fd == owned,
            "a clean boundary without CACHE_SESSION cannot detach");
}

static void test_protocol_gate_and_legacy_bytes()
{
    Pair p50 = make_pair(50);
    REQUIRE(p50.left->send_msg(CacheSessionMsg()),
            "P50 CACHE_SESSION encode succeeds");
    const Bytes expected_cache(kCacheSessionFixture.begin(), kCacheSessionFixture.end());
    Bytes actual_cache(expected_cache.size());
    const ssize_t cache_count = recv(p50.right->fd, actual_cache.data(),
                                     actual_cache.size(), 0);
    REQUIRE(cache_count == ssize_t(actual_cache.size())
                && actual_cache == expected_cache,
            "P50 CACHE_SESSION has the exact empty-payload wire fixture");

    for (const int version : {43, 48, 49}) {
        Pair old = make_pair(version);
        const int owned = old.left->fd;
        REQUIRE(!old.left->send_msg(CacheSessionMsg()) && old.left->fd == owned,
                version == 49
                    ? "P49 CACHE_SESSION encode is refused before composing bytes"
                    : "pre-P50 CACHE_SESSION encode is refused before composing bytes");
    }

    Pair send_pair = make_pair(49);

    /* Retained P49 ordinary bytes still have their historical shape after the
       rejected private message. */
    REQUIRE(send_pair.left->send_msg(PingMsg()),
            "P49 legacy PING remains sendable after CACHE_SESSION refusal");
    std::array<unsigned char, 8> actual{};
    const ssize_t count = recv(send_pair.right->fd, actual.data(), actual.size(), 0);
    REQUIRE(count == ssize_t(actual.size()) && actual == kPingFixture,
            "P49 retained legacy frame bytes are unchanged");

    for (const int version : {43, 48, 49}) {
        Pair decode_pair = make_pair(version);
        send_bytes(decode_pair.left->fd, frame(Msg::CACHE_SESSION));
        Msg *decoded = decode_pair.right->get_msg(2, true);
        REQUIRE(decoded == nullptr,
                version == 49
                    ? "P49 decoder rejects the Protocol-50-only discriminator"
                    : "pre-P50 decoder rejects the Protocol-50-only discriminator");
        delete decoded;
    }

    Pair malformed_pair = make_pair(50);
    Bytes malformed = frame(Msg::CACHE_SESSION);
    const uint32_t malformed_length = htonl(5);
    std::memcpy(malformed.data(), &malformed_length, sizeof(malformed_length));
    malformed.push_back(0);
    send_bytes(malformed_pair.left->fd, malformed);
    Msg *malformed_decoded = malformed_pair.right->get_msg(2, true);
    REQUIRE(malformed_decoded == nullptr,
            "P50 decoder rejects a CACHE_SESSION payload beyond its discriminator");
    delete malformed_decoded;
}

static void test_split_frame_reads()
{
    Pair pair = make_pair(50);
    const Bytes bytes = frame(Msg::CACHE_SESSION);
    send_bytes(pair.left->fd, Bytes(bytes.begin(), bytes.begin() + 3));
    REQUIRE(pair.right->get_msg(0, true) == nullptr,
            "split ordinary frame is not decoded before its final bytes");
    send_bytes(pair.left->fd, Bytes(bytes.begin() + 3, bytes.end()));
    Msg *decoded = pair.right->get_msg(2, true);
    REQUIRE(decoded && *decoded == Msg::CACHE_SESSION,
            "split ordinary frame is accepted once complete");
    delete decoded;
    const int released = pair.right->release_fd_if_input_empty();
    REQUIRE(released >= 0,
            "split-frame decode still permits a clean handoff");
    if (released >= 0)
        close(released);
}

static void test_read_ahead_barriers()
{
    const Bytes cache = frame(Msg::CACHE_SESSION);
    const Bytes ping = frame(Msg::PING);

    /* One early CacheWire byte is buffered after CACHE_SESSION.  Completing
       the ordinary frame later proves that the refusal retained that exact
       byte rather than dropping it. */
    {
        Pair pair = make_pair(50);
        Bytes joined = cache;
        joined.push_back(ping.front());
        send_bytes(pair.left->fd, joined);
        Msg *decoded = pair.right->get_msg(2, true);
        delete decoded;
        const int owned = pair.right->fd;
        REQUIRE(pair.right->release_fd_if_input_empty() == -1
                    && pair.right->fd == owned,
                "one early CacheWire byte blocks detach and retains ownership");
        send_bytes(pair.left->fd, Bytes(ping.begin() + 1, ping.end()));
        Msg *next = pair.right->get_msg(2, true);
        REQUIRE(next && *next == Msg::PING,
                "the exact buffered byte survives the failed handoff");
        delete next;
    }

    /* Decode first, then put exactly one byte in the kernel queue.  No parser
       read occurs between these operations, so this independently exercises
       the non-consuming MSG_PEEK barrier rather than the internal buffer. */
    {
        Pair pair = make_pair(50);
        Msg *decoded = decode_cache_session(pair);
        delete decoded;
        send_bytes(pair.left->fd, Bytes{ping.front()});
        const int owned = pair.right->fd;
        REQUIRE(pair.right->release_fd_if_input_empty() == -1
                    && pair.right->fd == owned,
                "one kernel-queued CacheWire byte blocks detach without consumption");
        send_bytes(pair.left->fd, Bytes(ping.begin() + 1, ping.end()));
        Msg *next = pair.right->get_msg(2, true);
        REQUIRE(next && *next == Msg::PING,
                "the kernel-queued barrier byte remains exact after refusal");
        delete next;
    }

    /* Partial and complete next ordinary frames are both barriers. */
    {
        Pair pair = make_pair(50);
        Bytes joined = cache;
        joined.insert(joined.end(), ping.begin(), ping.begin() + 2);
        send_bytes(pair.left->fd, joined);
        Msg *decoded = pair.right->get_msg(2, true);
        delete decoded;
        const int owned = pair.right->fd;
        REQUIRE(pair.right->release_fd_if_input_empty() == -1
                    && pair.right->fd == owned,
                "partial next ordinary frame blocks detach");
        send_bytes(pair.left->fd, Bytes(ping.begin() + 2, ping.end()));
        Msg *next = pair.right->get_msg(2, true);
        delete next;
    }
    {
        Pair pair = make_pair(50);
        Bytes joined = cache;
        joined.insert(joined.end(), ping.begin(), ping.end());
        send_bytes(pair.left->fd, joined);
        Msg *decoded = pair.right->get_msg(2, true);
        delete decoded;
        const int owned = pair.right->fd;
        REQUIRE(pair.right->release_fd_if_input_empty() == -1
                    && pair.right->fd == owned,
                "complete next ordinary frame blocks detach");
        Msg *next = pair.right->get_msg(2, true);
        REQUIRE(next && *next == Msg::PING,
                "decoding the next ordinary frame clears the release arm");
        delete next;
        REQUIRE(pair.right->release_fd_if_input_empty() == -1,
                "a later clean boundary remains unable to detach");
    }
}

static void test_eof_and_pending_output_barriers()
{
    {
        Pair pair = make_pair(50);
        send_bytes(pair.left->fd, frame(Msg::CACHE_SESSION));
        Msg *decoded = pair.right->get_msg(2, true);
        delete decoded;
        shutdown(pair.left->fd, SHUT_WR);
        const int owned = pair.right->fd;
        REQUIRE(pair.right->release_fd_if_input_empty() == -1
                    && pair.right->fd == owned,
                "peer EOF blocks descriptor release");
    }
    {
        Pair pair = make_pair(50);
        REQUIRE(pair.right->send_msg(PingMsg(), MsgChannel::SendBulkOnly),
                "test queues output before the inbound handoff marker");
        Msg *decoded = decode_cache_session(pair);
        delete decoded;
        const int owned = pair.right->fd;
        REQUIRE(pair.right->release_fd_if_input_empty() == -1
                    && pair.right->fd == owned,
                "preexisting pending output/frame blocks descriptor release");
    }
    {
        Pair pair = make_pair(50);
        Msg *decoded = decode_cache_session(pair);
        delete decoded;
        REQUIRE(pair.right->send_msg(PingMsg(), MsgChannel::SendBulkOnly),
                "a later ordinary send is queued after CACHE_SESSION");
        REQUIRE(pair.right->flush_pending(),
                "later ordinary output can be fully drained for the arm test");
        const int owned = pair.right->fd;
        REQUIRE(pair.right->release_fd_if_input_empty() == -1
                    && pair.right->fd == owned,
                "a later ordinary send permanently clears the handoff arm");
    }
}

static void test_nonblocking_accepted_protocol_admission()
{
    int sockets[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
            "accepted-protocol socketpair is available");
    if (sockets[0] < 0 || sockets[1] < 0)
        return;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto started = std::chrono::steady_clock::now();
    MsgChannel *accepted = Service::createChannelAccepted(
        sockets[0], reinterpret_cast<sockaddr *>(&address), sizeof(address));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    REQUIRE(accepted != nullptr && elapsed < 100,
            "accepted factory returns without waiting for peer protocol bytes");
    if (!accepted) {
        close(sockets[1]);
        return;
    }
    REQUIRE(accepted->protocol_admission_state() ==
                MsgChannel::ProtocolAdmissionState::Pending,
            "silent accepted peer remains pending rather than becoming a client");

    std::array<unsigned char, 4> daemon_version{};
    pollfd offer_poll{sockets[1], POLLIN, 0};
    const bool offer_ready = poll(&offer_poll, 1, 100) == 1 &&
        (offer_poll.revents & POLLIN) != 0;
    const ssize_t version_count = offer_ready
        ? recv(sockets[1], daemon_version.data(), daemon_version.size(), 0)
        : -1;
    REQUIRE(offer_ready &&
                version_count == static_cast<ssize_t>(daemon_version.size()) &&
                daemon_version[0] == PROTOCOL_VERSION,
            "nonblocking accepted factory emits the ordinary protocol offer");

    const std::array<unsigned char, 8> peer_protocol{
        PROTOCOL_VERSION, 0, 0, 0, PROTOCOL_VERSION, 0, 0, 0};
    REQUIRE(send(sockets[1], peer_protocol.data(), 2, MSG_NOSIGNAL) == 2,
            "partial peer protocol prefix is sent");
    REQUIRE(accepted->read_a_bit() &&
                accepted->protocol_admission_state() ==
                    MsgChannel::ProtocolAdmissionState::Pending,
            "partial peer protocol remains pending after one nonblocking step");
    REQUIRE(send(sockets[1], peer_protocol.data() + 2,
                 peer_protocol.size() - 2, MSG_NOSIGNAL) ==
                static_cast<ssize_t>(peer_protocol.size() - 2),
            "remaining peer protocol bytes are sent");
    REQUIRE(accepted->read_a_bit() &&
                accepted->protocol_admission_state() ==
                    MsgChannel::ProtocolAdmissionState::Ready,
            "incremental protocol exchange becomes promotion-ready");
    REQUIRE(accepted->finish_protocol_admission(),
            "ready handshake explicitly crosses the client-admission boundary");

    delete accepted;
    close(sockets[1]);

    int malformed_sockets[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, malformed_sockets) == 0,
            "malformed accepted-protocol socketpair is available");
    if (malformed_sockets[0] < 0 || malformed_sockets[1] < 0)
        return;
    MsgChannel *malformed = Service::createChannelAccepted(
        malformed_sockets[0], reinterpret_cast<sockaddr *>(&address),
        sizeof(address));
    std::array<unsigned char, 4> malformed_offer{};
    pollfd malformed_poll{malformed_sockets[1], POLLIN, 0};
    const bool malformed_offer_ready = malformed &&
        poll(&malformed_poll, 1, 100) == 1 &&
        (malformed_poll.revents & POLLIN) != 0;
    if (malformed_offer_ready) {
        (void)recv(malformed_sockets[1], malformed_offer.data(),
                   malformed_offer.size(), 0);
    }
    const std::array<unsigned char, 4> invalid_protocol{1, 0, 0, 0};
    const bool invalid_sent = send(
        malformed_sockets[1], invalid_protocol.data(), invalid_protocol.size(),
        MSG_NOSIGNAL) == static_cast<ssize_t>(invalid_protocol.size());
    const auto malformed_started = std::chrono::steady_clock::now();
    const bool malformed_read = malformed && malformed->read_a_bit();
    const auto malformed_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - malformed_started).count();
    REQUIRE(malformed_offer_ready && invalid_sent && !malformed_read &&
                malformed_elapsed < 100 &&
                malformed->protocol_admission_state() ==
                    MsgChannel::ProtocolAdmissionState::Failed,
            "malformed async protocol fails without entering a blocking error path");
    delete malformed;
    close(malformed_sockets[1]);
}

static volatile sig_atomic_t deadline_interruptions = 0;
static void interrupt_deadline_wait(int)
{
    deadline_interruptions = 1;
}

static void test_absolute_message_deadline()
{
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;
    {
        Pair pair = make_pair(50);
        Bytes malformed(sizeof(uint32_t));
        const uint32_t oversized = htonl(2 * 1024 * 1024);
        std::memcpy(malformed.data(), &oversized, sizeof(oversized));
        (void)send(pair.left->fd, malformed.data(), malformed.size(), MSG_NOSIGNAL);
        const auto start = Clock::now();
        Msg *message = pair.right->get_msg_until(start + 100ms);
        REQUIRE(!message && Clock::now() - start < 200ms,
                "malformed deadline reply cannot start an ordinary status wait");
        delete message;
    }
    {
        Pair pair = make_pair(50);
        std::thread sender([&] {
            std::this_thread::sleep_for(30ms);
            (void)pair.left->send_msg(PingMsg());
        });
        Msg *message = pair.right->get_msg_until(Clock::now() + 500ms);
        REQUIRE(dynamic_cast<PingMsg *>(message) != nullptr,
                "absolute receive uses a positive subsecond budget");
        delete message;
        sender.join();
    }
    {
        Pair pair = make_pair(50);
        std::thread sender([&] {
            for (unsigned char byte : kPingFixture) {
                std::this_thread::sleep_for(30ms);
                (void)send(pair.left->fd, &byte, 1, MSG_NOSIGNAL);
            }
        });
        const auto start = Clock::now();
        Msg *message = pair.right->get_msg_until(start + 100ms);
        const auto elapsed = Clock::now() - start;
        REQUIRE(!message && elapsed >= 90ms && elapsed < 200ms,
                "partial frames cannot renew the absolute receive deadline");
        delete message;
        sender.join();
        message = pair.right->get_msg_until(Clock::now() + 100ms);
        REQUIRE(dynamic_cast<PingMsg *>(message) != nullptr,
                "timed out receive preserves partial input for its owner");
        delete message;
    }
    {
        Pair pair = make_pair(50);
        (void)pair.left->send_msg(PingMsg());
        Msg *message = pair.right->get_msg_until(Clock::now() - 1ms);
        REQUIRE(!message, "expired deadline refuses even a queued reply");
        delete message;
    }
    {
        Pair pair = make_pair(50);
        delete decode_cache_session(pair);
        const int owned = pair.right->fd;
        delete pair.right->get_msg_until(Clock::now() - 1ms);
        REQUIRE(pair.right->release_fd_if_input_empty() == -1 &&
                    pair.right->fd == owned,
                "expired receive invalidates prior cache-session handoff authority");
    }
    {
        Pair pair = make_pair(50);
        struct sigaction action{}, previous{};
        action.sa_handler = interrupt_deadline_wait;
        sigemptyset(&action.sa_mask);
        const bool installed = sigaction(SIGUSR1, &action, &previous) == 0;
        REQUIRE(installed, "deadline interruption handler installs");
        if (!installed)
            return;
        const pthread_t receiver = pthread_self();
        deadline_interruptions = 0;
        std::thread interrupter([receiver] {
            for (int i = 0; i < 6; ++i) {
                std::this_thread::sleep_for(20ms);
                (void)pthread_kill(receiver, SIGUSR1);
            }
        });
        const auto start = Clock::now();
        Msg *message = pair.right->get_msg_until(start + 100ms);
        const auto elapsed = Clock::now() - start;
        REQUIRE(!message && deadline_interruptions &&
                    elapsed >= 90ms && elapsed < 180ms,
                "interrupted waits do not renew the absolute deadline");
        delete message;
        interrupter.join();
        (void)sigaction(SIGUSR1, &previous, nullptr);
    }
}

int main()
{
    static_assert(Msg::CACHE_SESSION == UINT32_C(0x50f00000));
    static_assert(CACHE_SESSION_READY_MAGIC == UINT32_C(0x50f00001));
    static_assert(Msg::PING == UINT32_C(0x00000042));
    test_successful_transfer_and_exact_once();
    test_outbound_transfer_and_exact_once();
    test_outbound_release_barriers();
    test_ready_wire_and_failure_boundaries();
    test_absolute_deadline_protocol_negotiation();
    test_absolute_deadline_owns_tcp_user_timeout();
    test_ordinary_tcp_timeout_and_application_extension();
    test_same_endpoint_retry_after_complete_slice();
    test_same_endpoint_retry_immediate_success_owns_outer_timeout();
    test_same_endpoint_retry_preserves_immediate_failure();
    test_adaptive_connection_budget();
    test_adaptive_connection_budget(false);
    test_other_message_refusal_and_arm_clear();
    test_protocol_gate_and_legacy_bytes();
    test_split_frame_reads();
    test_read_ahead_barriers();
    test_eof_and_pending_output_barriers();
    test_nonblocking_accepted_protocol_admission();
    test_absolute_message_deadline();
    return failures ? 1 : 0;
}
