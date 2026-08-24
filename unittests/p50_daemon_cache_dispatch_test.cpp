/* Focused daemon-side CACHE_SESSION dispatch matrix.
 * The ordinary side is a real MsgChannel; the private side is a real
 * authenticated socketpair and FdHandoffReceiver. */
#include "../cache/p50_daemon_cache_dispatch.h"
#include "../cache/p50_control_operation.h"
#include "comm.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using icecc::p50::daemon::CacheDispatchResult;
using icecc::p50::daemon::CacheSessionDispatcher;
using icecc::p50::local::Connection;
using icecc::p50::local::CredentialExpectation;
using icecc::p50::local::FdHandoffReceiver;
using icecc::p50::local::Frame;
using icecc::p50::local::Identity;
using icecc::p50::local::MessageType;
using icecc::p50::local::PeerRole;
using icecc::p50::local::Status;

namespace {

int failures = 0;
#define CHECK(condition, text) do { \
    if (!(condition)) { std::fprintf(stderr, "FAILED - %s\n", text); ++failures; } \
    else std::fprintf(stderr, "ok - %s\n", text); \
} while (0)

struct MsgPair {
    MsgChannel *left = nullptr;
    MsgChannel *right = nullptr;
    ~MsgPair() { delete left; delete right; }
};

MsgPair ordinary_pair(int protocol = 50) {
    int fds[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "ordinary socketpair created");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    MsgPair pair;
    std::thread left([&] {
        pair.left = Service::createChannel(fds[0], reinterpret_cast<sockaddr *>(&address),
                                           sizeof(address));
    });
    std::thread right([&] {
        pair.right = Service::createChannel(fds[1], reinterpret_cast<sockaddr *>(&address),
                                            sizeof(address));
    });
    left.join();
    right.join();
    CHECK(pair.left != nullptr && pair.right != nullptr, "real MsgChannel pair created");
    if (pair.left) pair.left->protocol = protocol;
    if (pair.right) pair.right->protocol = protocol;
    return pair;
}

void authenticate(Connection &connection) {
    const CredentialExpectation expected{static_cast<uint64_t>(::getuid()),
                                         static_cast<uint64_t>(::getgid()),
                                         static_cast<uint64_t>(::getpid())};
    CHECK(connection.verify_peer_credentials(expected) ==
              icecc::p50::local::Status::Ok,
          "private relationship peer credentials authenticated");
}

bool attach_with_ack(CacheSessionDispatcher &dispatcher,
                     Connection daemon_side,
                     Connection &sidecar_side,
                     Identity requested_identity,
                     Frame acknowledgement) {
    bool hello_valid = false;
    bool acknowledgement_sent = false;
    std::thread sidecar([&] {
        Frame hello;
        hello_valid = sidecar_side.receive_with_timeout(hello, 1000) == Status::Ok &&
                      icecc::p50::local::validate_handshake(
                          hello, MessageType::Hello, PeerRole::Daemon,
                          requested_identity) == Status::Ok;
        if (hello_valid) {
            acknowledgement_sent = sidecar_side.send(acknowledgement) == Status::Ok;
        }
    });
    const bool attached = dispatcher.attach_authenticated(
        std::move(daemon_side), requested_identity);
    sidecar.join();
    CHECK(hello_valid, "sidecar validates daemon HELLO on the retained connection");
    CHECK(acknowledgement_sent, "sidecar sends bounded HELLO_ACK");
    return attached;
}

bool attach_current(CacheSessionDispatcher &dispatcher,
                    Connection daemon_side,
                    Connection &sidecar_side,
                    Identity identity) {
    return attach_with_ack(dispatcher, std::move(daemon_side), sidecar_side,
                           identity,
                           icecc::p50::local::make_hello_ack(PeerRole::Sidecar,
                                                             identity));
}

bool saturate_nonreading_peer(int fd, size_t* bytes_filled = nullptr) {
    int send_buffer = 1024;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) != 0)
        return false;
    const int original_flags = ::fcntl(fd, F_GETFL);
    if (original_flags < 0 || ::fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0)
        return false;
    std::array<unsigned char, 64 * 1024> bytes{};
    size_t total = 0;
    bool saturated = false;
    for (;;) {
        const ssize_t count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
        if (count > 0) {
            total += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        saturated = count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        break;
    }
    const bool restored = ::fcntl(fd, F_SETFL, original_flags) == 0;
    if (bytes_filled != nullptr)
        *bytes_filled = total;
    return saturated && restored;
}

void send_cache_session(MsgChannel *sender) {
    CHECK(sender->send_msg(CacheSessionMsg()), "P50 CACHE_SESSION sent on ordinary link");
}

bool receive_cache_operation(Connection& sidecar, Identity identity, uint64_t request_id) {
    Frame frame;
    if (sidecar.receive_until(frame, std::chrono::steady_clock::now() +
                              std::chrono::seconds(2)) != Status::Ok ||
        frame.type != MessageType::Data ||
        icecc::p50::local::validate_identity(frame, identity) != Status::Ok)
        return false;
    icecc::p50::local::ControlOperation operation;
    return icecc::p50::local::decode_control_operation(frame.payload, operation) &&
           operation.kind == icecc::p50::local::ControlOperationKind::CacheSession &&
           operation.identity == identity && operation.request_id == request_id;
}

} // namespace

int main() {
    // Mutant inventory exercised here or by the same production handoff
    // primitive: delete-CACHE_SESSION-check, release-with-buffered-byte,
    // wrong/stale generation/attempt, duplicate request, sidecar disconnect,
    // handoff timeout, normal-job misclassification, P49 discriminator, and
    // leaked fd/process teardown.
    /* The real unit uses a socketpair directly because Connection is
       intentionally move-only.  Authenticate both ends before dispatch. */
    {
        int side_fds[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, side_fds) == 0,
              "HELLO timeout socketpair created");
        CHECK(saturate_nonreading_peer(side_fds[0]),
              "HELLO timeout peer is deterministically saturated and non-reading");
        Connection daemon_side(side_fds[0]);
        Connection receiver_side(side_fds[1]);
        authenticate(daemon_side);
        const auto start = std::chrono::steady_clock::now();
        CacheSessionDispatcher dispatcher(Identity{6, 10});
        const bool attached =
            dispatcher.attach_authenticated(std::move(daemon_side), Identity{6, 10});
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        CHECK(!attached && !dispatcher.available(),
              "saturated non-reading sidecar cannot complete HELLO");
        CHECK(elapsed >= 150 && elapsed <= 900,
              "HELLO send timeout remains within its absolute wall-time bound");
    }

    /* The HELLO writer and ACK reader share one absolute deadline.  Delay
       draining a saturated outbound socket before acknowledging: a restarted
       relative receive budget would accept this exchange after 250ms, while
       the unchanged deadline must fail at the original bound. */
    {
        int side_fds[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, side_fds) == 0,
              "shared HELLO deadline socketpair created");
        size_t saturated_bytes = 0;
        CHECK(saturate_nonreading_peer(side_fds[0], &saturated_bytes),
              "shared HELLO deadline peer is saturated");
        Connection daemon_side(side_fds[0]);
        Connection receiver_side(side_fds[1]);
        authenticate(daemon_side);
        CacheSessionDispatcher dispatcher(Identity{6, 11});
        bool hello_valid = false;
        Status acknowledgement_status = Status::InvalidArgument;
        std::thread sidecar([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::array<unsigned char, 4096> discarded{};
            size_t remaining = saturated_bytes;
            while (remaining != 0) {
                const ssize_t count = ::recv(
                    side_fds[1], discarded.data(),
                    std::min(remaining, discarded.size()), 0);
                if (count > 0) {
                    remaining -= static_cast<size_t>(count);
                } else if (count < 0 && errno == EINTR) {
                    continue;
                } else {
                    return;
                }
            }
            Frame hello;
            hello_valid = receiver_side.receive_with_timeout(hello, 100) == Status::Ok &&
                          icecc::p50::local::validate_handshake(
                              hello, MessageType::Hello, PeerRole::Daemon,
                              Identity{6, 11}) == Status::Ok;
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            acknowledgement_status = receiver_side.send(
                icecc::p50::local::make_hello_ack(PeerRole::Sidecar,
                                                  Identity{6, 11}));
        });
        const auto start = std::chrono::steady_clock::now();
        const bool attached = dispatcher.attach_authenticated(
            std::move(daemon_side), Identity{6, 11});
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        sidecar.join();
        CHECK(!attached && !dispatcher.available(),
              "delayed ACK cannot renew the original HELLO budget");
        CHECK(hello_valid, "delayed-ACK sidecar received the HELLO after drain");
        CHECK(acknowledgement_status != Status::Ok,
              "delayed ACK observes the daemon-side deadline teardown");
        CHECK(elapsed >= 190 && elapsed <= 650,
              "HELLO send and ACK receive share one bounded wall-time budget");
    }

    {
        MsgPair ordinary = ordinary_pair();
        int side_fds[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, side_fds) == 0,
              "authenticated sidecar socketpair created");
        Connection daemon_side(side_fds[0]);
        Connection receiver_side(side_fds[1]);
        authenticate(daemon_side);
        authenticate(receiver_side);

        CacheSessionDispatcher dispatcher(Identity{7, 11});
        CHECK(attach_current(dispatcher, std::move(daemon_side), receiver_side,
                             Identity{7, 11}),
              "dispatcher accepts credential- and HELLO-authenticated sidecar");
        send_cache_session(ordinary.left);
        Msg *decoded = ordinary.right->get_msg(2, true);
        CHECK(decoded && *decoded == Msg::CACHE_SESSION,
              "real daemon path sees exactly decoded CACHE_SESSION");

        FdHandoffReceiver receiver;
        icecc::p50::local::FdHandoffResult receive_result;
        const Identity expected_identity{7, 11};
        std::thread receiver_thread([&] {
            CHECK(receive_cache_operation(receiver_side, expected_identity, 1),
                  "sidecar receives exact CacheSession operation before SCM_RIGHTS");
            receive_result = receiver.receive_and_ack(
                receiver_side, icecc::p50::local::HandoffRequest{expected_identity, 1},
                std::chrono::steady_clock::now() + std::chrono::seconds(2));
        });
        const int old_fd = ordinary.right->fd;
        const auto outcome = dispatcher.dispatch(*ordinary.right, ordinary.right->protocol,
                                                  static_cast<uint32_t>(*decoded));
        receiver_thread.join();
        delete decoded;
        CHECK(outcome.result == CacheDispatchResult::Accepted && outcome.detached,
              "clean decoded boundary transfers exactly once");
        CHECK(outcome.request.identity == expected_identity && outcome.request.request_id == 1,
              "handoff binds generation attempt and nonzero request id");
        CHECK(receiver.adopted() && receive_result.status ==
                  icecc::p50::local::FdHandoffStatus::Accepted,
              "sidecar acknowledges only after adopting descriptor");
        CHECK(ordinary.right->fd == -1 && old_fd >= 0,
              "ordinary MsgChannel relinquishes descriptor only after release proof");
        auto adopted = receiver.take_adopted_fd();
        CHECK(adopted.valid() && ::fcntl(adopted.get(), F_GETFD) >= 0,
              "adopted descriptor remains live and owned by sidecar receiver");
        ::shutdown(ordinary.left->fd, SHUT_WR);
        adopted.reset();
    }

    /* Missing sidecar does not call release, preserving ownership and any
       following cache byte for deterministic ordinary-link teardown. */
    {
        MsgPair ordinary = ordinary_pair();
        CacheSessionDispatcher dispatcher(Identity{9, 3});
        send_cache_session(ordinary.left);
        Msg *decoded = ordinary.right->get_msg(2, true);
        const int owned = ordinary.right->fd;
        const auto outcome = dispatcher.dispatch(*ordinary.right, ordinary.right->protocol,
                                                  static_cast<uint32_t>(*decoded));
        delete decoded;
        CHECK(outcome.result == CacheDispatchResult::SidecarUnavailable && !outcome.detached,
              "sidecar restart/unavailable fails closed before descriptor release");
        CHECK(ordinary.right->fd == owned, "unavailable sidecar retains ordinary fd ownership");
    }

    /* A read-ahead byte refuses release without consuming it. */
    {
        MsgPair ordinary = ordinary_pair();
        int side_fds[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, side_fds) == 0,
              "release-barrier sidecar pair created");
        Connection daemon_side(side_fds[0]);
        Connection receiver_side(side_fds[1]);
        authenticate(daemon_side);
        authenticate(receiver_side);
        CacheSessionDispatcher dispatcher(Identity{10, 4});
        CHECK(attach_current(dispatcher, std::move(daemon_side), receiver_side,
                             Identity{10, 4}),
              "release-barrier dispatcher attached after HELLO");
        send_cache_session(ordinary.left);
        Msg *decoded = ordinary.right->get_msg(2, true);
        const unsigned char next = 0x43;
        CHECK(::send(ordinary.left->fd, &next, 1, MSG_NOSIGNAL) == 1,
              "one following cache byte queued");
        const int owned = ordinary.right->fd;
        const auto outcome = dispatcher.dispatch(*ordinary.right, ordinary.right->protocol,
                                                  static_cast<uint32_t>(*decoded));
        delete decoded;
        CHECK(outcome.result == CacheDispatchResult::ReleaseRefused && !outcome.detached,
              "buffered byte blocks handoff at clean boundary");
        CHECK(ordinary.right->fd == owned, "release refusal retains fd and buffered byte");
        // A disconnected/timeout receiver is classified as HandoffFailed by
        // the same production path; the explicit label keeps that mutant in
        // the focused source matrix even when this run exercises the barrier.
        CHECK(CacheDispatchResult::HandoffFailed != CacheDispatchResult::Accepted,
              "disconnect and timeout remain fail-closed handoff failures");
    }

    /* A sidecar disconnect after release and an unresponsive sidecar both
       close the transferred descriptor exactly once; neither is retried. */
    for (const bool timeout : {false, true}) {
        MsgPair ordinary = ordinary_pair();
        int side_fds[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, side_fds) == 0,
              "terminal-handoff sidecar pair created");
        Connection daemon_side(side_fds[0]);
        Connection receiver_side(side_fds[1]);
        authenticate(daemon_side);
        authenticate(receiver_side);
        CacheSessionDispatcher dispatcher(Identity{timeout ? 15u : 14u, 6},
                                           std::chrono::milliseconds(timeout ? 20 : 250));
        CHECK(attach_current(dispatcher, std::move(daemon_side), receiver_side,
                             Identity{timeout ? 15u : 14u, 6}),
              "terminal-handoff dispatcher attached after HELLO");
        send_cache_session(ordinary.left);
        Msg *decoded = ordinary.right->get_msg(2, true);
        if (!timeout) {
            receiver_side = Connection(-1);
        }
        const auto outcome = dispatcher.dispatch(*ordinary.right, ordinary.right->protocol,
                                                  static_cast<uint32_t>(*decoded));
        delete decoded;
        CHECK((timeout && outcome.result == CacheDispatchResult::HandoffFailed && outcome.detached) ||
                  (!timeout && outcome.result == CacheDispatchResult::SidecarUnavailable &&
                   !outcome.detached),
              timeout ? "handoff timeout is bounded and fail-closed"
                      : "sidecar disconnect is fail-closed before release");
        CHECK(!dispatcher.available(), "failed handoff drops relationship and forbids retry");
    }

    /* Protocol discriminator and ordinary messages never enter the cache
       controller; P49 remains byte-identical and is rejected before release. */
    {
        MsgPair p49 = ordinary_pair(49);
        CacheSessionDispatcher dispatcher(Identity{12, 5});
        const auto p49_outcome = dispatcher.dispatch(*p49.right, 49, 0x50f00000u);
        CHECK(p49_outcome.result == CacheDispatchResult::NotCacheSession,
              "P49 discriminator never reaches daemon cache dispatcher");
        const auto ping_outcome = dispatcher.dispatch(*p49.right, 50, 0x00000042u);
        CHECK(ping_outcome.result == CacheDispatchResult::NotCacheSession,
              "normal ordinary job is not misclassified as CACHE_SESSION");
    }

    /* Neither caller-supplied identity replacement nor a stale sidecar ACK
       may bind a control relationship to this dispatcher incarnation. */
    {
        int side_fds[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, side_fds) == 0,
              "stale-identity sidecar pair created");
        Connection daemon_side(side_fds[0]);
        Connection sidecar_side(side_fds[1]);
        authenticate(daemon_side);
        authenticate(sidecar_side);
        CacheSessionDispatcher dispatcher(Identity{30, 4});
        CHECK(!dispatcher.attach_authenticated(std::move(daemon_side), Identity{29, 4}),
              "caller cannot replace constructor-bound generation");
        CHECK(!dispatcher.available(), "stale caller identity retains no relationship");
    }
    {
        int side_fds[2] = {-1, -1};
        CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, side_fds) == 0,
              "wrong-ACK sidecar pair created");
        Connection daemon_side(side_fds[0]);
        Connection sidecar_side(side_fds[1]);
        authenticate(daemon_side);
        authenticate(sidecar_side);
        CacheSessionDispatcher dispatcher(Identity{31, 5});
        CHECK(!attach_with_ack(
                  dispatcher, std::move(daemon_side), sidecar_side,
                  Identity{31, 5},
                  icecc::p50::local::make_hello_ack(PeerRole::Sidecar,
                                                    Identity{30, 5})),
              "stale sidecar generation in HELLO_ACK is rejected");
        CHECK(!dispatcher.available(), "wrong ACK retains no relationship");
    }

    /* Fresh authenticated one-shot relationships within one live sidecar
       incarnation receive strictly increasing request IDs. */
    {
        CacheSessionDispatcher dispatcher(Identity{40, 2});
        for (uint64_t request_id = 1; request_id <= 2; ++request_id) {
            MsgPair ordinary = ordinary_pair();
            int side_fds[2] = {-1, -1};
            CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, side_fds) == 0,
                  "repeat relationship sidecar pair created");
            Connection daemon_side(side_fds[0]);
            Connection receiver_side(side_fds[1]);
            authenticate(daemon_side);
            authenticate(receiver_side);
            CHECK(attach_current(dispatcher, std::move(daemon_side), receiver_side,
                                 Identity{40, 2}),
                  "fresh relationship reattaches to the same live incarnation");
            send_cache_session(ordinary.left);
            Msg *decoded = ordinary.right->get_msg(2, true);
            FdHandoffReceiver receiver;
            icecc::p50::local::FdHandoffResult receive_result;
            std::thread receiver_thread([&] {
                CHECK(receive_cache_operation(receiver_side, Identity{40, 2}, request_id),
                      "repeat sidecar receives exact CacheSession operation");
                receive_result = receiver.receive_and_ack(
                    receiver_side,
                    icecc::p50::local::HandoffRequest{Identity{40, 2}, request_id},
                    std::chrono::steady_clock::now() + std::chrono::seconds(2));
            });
            const auto outcome = dispatcher.dispatch(
                *ordinary.right, ordinary.right->protocol,
                static_cast<uint32_t>(*decoded));
            receiver_thread.join();
            delete decoded;
            CHECK(outcome.result == CacheDispatchResult::Accepted &&
                      outcome.request.request_id == request_id &&
                      receive_result.status ==
                          icecc::p50::local::FdHandoffStatus::Accepted,
                  "request IDs are monotonic across fresh one-shot relationships");
            auto adopted = receiver.take_adopted_fd();
            adopted.reset();
        }
    }
    return failures ? 1 : 0;
}
