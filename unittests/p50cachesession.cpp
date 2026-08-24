/* Protocol-50 ordinary-link CACHE_SESSION and descriptor handoff gate. */
#include "comm.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <algorithm>
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
    const Bytes expected_cache = frame(Msg::CACHE_SESSION);
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
    std::array<unsigned char, 8> expected{};
    const Bytes ping = frame(Msg::PING);
    std::copy(ping.begin(), ping.end(), expected.begin());
    std::array<unsigned char, 8> actual{};
    const ssize_t count = recv(send_pair.right->fd, actual.data(), actual.size(), 0);
    REQUIRE(count == ssize_t(actual.size()) && actual == expected,
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
    REQUIRE(pair.right->release_fd_if_input_empty() >= 0,
            "split-frame decode still permits a clean handoff");
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
        Msg *decoded = decode_cache_session(pair);
        delete decoded;
        REQUIRE(pair.right->send_msg(PingMsg(), MsgChannel::SendBulkOnly),
                "test queues a pending ordinary output frame");
        const int owned = pair.right->fd;
        REQUIRE(pair.right->release_fd_if_input_empty() == -1
                    && pair.right->fd == owned,
                "pending output/frame blocks descriptor release");
    }
}

int main()
{
    static_assert(Msg::CACHE_SESSION == UINT32_C(0x50f00000));
    test_successful_transfer_and_exact_once();
    test_other_message_refusal_and_arm_clear();
    test_protocol_gate_and_legacy_bytes();
    test_split_frame_reads();
    test_read_ahead_barriers();
    test_eof_and_pending_output_barriers();
    return failures ? 1 : 0;
}
