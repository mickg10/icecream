/* Focused Protocol-50 CACHE_SESSION dispatch matrix.
 *
 * Every control relationship below is accepted from a private AF_UNIX
 * OnDemandEndpoint.  The ordinary MsgChannel is a socketpair because it is
 * the stream whose release boundary is under test; no control relationship is
 * cached or attached by the dispatcher.
 */
#include "../cache/p50_daemon_cache_dispatch.h"
#include "../cache/p50_control_operation.h"
#include "../cache/p50_incarnation_identity.h"
#include "comm.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

using icecc::p50::daemon::CacheDispatchResult;
using icecc::p50::daemon::CacheSessionDispatcher;
using icecc::p50::daemon::OnDemandEndpoint;
using icecc::p50::local::Connection;
using icecc::p50::local::CredentialExpectation;
using icecc::p50::local::FdHandoffReceiver;
using icecc::p50::local::FdHandoffResult;
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
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
          "ordinary socketpair created");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    MsgPair pair;
    std::thread left([&] {
        pair.left = Service::createChannel(fds[0],
                                           reinterpret_cast<sockaddr *>(&address),
                                           sizeof(address));
    });
    std::thread right([&] {
        pair.right = Service::createChannel(fds[1],
                                            reinterpret_cast<sockaddr *>(&address),
                                            sizeof(address));
    });
    left.join();
    right.join();
    CHECK(pair.left != nullptr && pair.right != nullptr,
          "ordinary MsgChannel pair created");
    if (pair.left) pair.left->protocol = protocol;
    if (pair.right) pair.right->protocol = protocol;
    return pair;
}

void send_cache_session(MsgChannel *sender) {
    CHECK(sender != nullptr && sender->send_msg(CacheSessionMsg()),
          "P50 CACHE_SESSION sent on ordinary link");
}

struct EndpointFixture {
    explicit EndpointFixture(Identity identity) : identity(identity) {
        char template_path[] = "/tmp/icecc-dispatch-endpoint-XXXXXX";
        char *created = ::mkdtemp(template_path);
        CHECK(created != nullptr, "private endpoint directory created");
        if (created == nullptr)
            return;
        root = created;
        CHECK(::chmod(root.c_str(), 0700) == 0, "private endpoint directory is 0700");
        path = root + "/cache.sock";
        listener = icecc::p50::local::listen_unix(path, 4, &listen_status);
        CHECK(listener >= 0 && listen_status == Status::Ok,
              "private AF_UNIX listener created");
        struct stat info{};
        CHECK(listener >= 0 && ::lstat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode),
              "listener pathname is a socket");
        if (listener < 0 || ::lstat(path.c_str(), &info) != 0)
            return;

        endpoint.socket_path = path;
        endpoint.expected_peer.uid = static_cast<uint64_t>(::getuid());
        endpoint.expected_peer.gid = static_cast<uint64_t>(::getgid());
        // Keep this assignment out of the aggregate initializer: the source
        // gate must retain the exact PID credential binding as a visible field.
        endpoint.expected_peer.pid = static_cast<uint64_t>(::getpid());
        endpoint.lease_identity = identity;
        endpoint.f_store_guid = icecc::p50::f_store_guid_for_incarnation(identity);
        endpoint.socket_path_digest = icecc::digest128(path);
        endpoint.listener_device = info.st_dev;
        endpoint.listener_inode = info.st_ino;
    }

    ~EndpointFixture() {
        if (listener >= 0)
            (void)::close(listener);
        struct stat info{};
        if (!path.empty() && ::lstat(path.c_str(), &info) == 0 &&
            S_ISSOCK(info.st_mode) && info.st_dev == endpoint.listener_device &&
            info.st_ino == endpoint.listener_inode)
            (void)::unlink(path.c_str());
        if (!root.empty())
            (void)::rmdir(root.c_str());
    }

    Identity identity{};
    std::string root;
    std::string path;
    int listener = -1;
    Status listen_status = Status::InvalidArgument;
    OnDemandEndpoint endpoint;
};

struct PeerResult {
    bool accepted = false;
    bool credentials = false;
    bool hello = false;
    bool ack_sent = false;
    bool operation = false;
    FdHandoffResult handoff{};
    FdHandoffReceiver receiver;
};

enum class PeerAction { Handoff, CloseAfterAck, WaitAfterAck };

bool receive_cache_operation(Connection& sidecar, Identity identity,
                             uint64_t request_id);

std::thread serve_peer(EndpointFixture &fixture, Identity identity, uint64_t request_id,
                       Frame acknowledgement, PeerAction action, PeerResult &result,
                       std::chrono::milliseconds acknowledgement_delay = {}) {
    return std::thread([&fixture, identity, request_id,
                        acknowledgement = std::move(acknowledgement), action, &result,
                        acknowledgement_delay]() mutable {
        Status accept_status = Status::InvalidArgument;
        Connection connection = icecc::p50::local::accept_unix(fixture.listener, &accept_status);
        result.accepted = connection.valid() && accept_status == Status::Ok;
        if (!result.accepted)
            return;
        const CredentialExpectation expected{
            static_cast<uint64_t>(::getuid()), static_cast<uint64_t>(::getgid()),
            static_cast<uint64_t>(::getpid())};
        result.credentials = connection.verify_peer_credentials(expected) == Status::Ok;
        Frame hello;
        result.hello = result.credentials &&
            connection.receive_with_timeout(hello, 1000) == Status::Ok &&
            icecc::p50::local::validate_handshake(hello, MessageType::Hello,
                                                  PeerRole::Daemon, identity) == Status::Ok;
        if (!result.hello)
            return;
        if (acknowledgement_delay.count() != 0)
            std::this_thread::sleep_for(acknowledgement_delay);
        result.ack_sent = connection.send(acknowledgement) == Status::Ok;
        if (!result.ack_sent)
            return;
        result.operation = receive_cache_operation(connection, identity, request_id);
        if (!result.operation)
            return;
        if (action == PeerAction::CloseAfterAck) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return;
        }
        if (action == PeerAction::WaitAfterAck) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            return;
        }
        result.handoff = result.receiver.receive_and_ack(
            connection, icecc::p50::local::HandoffRequest{identity, request_id},
            std::chrono::steady_clock::now() + std::chrono::seconds(2));
        // Let the sender consume the ACK before this fresh relationship is
        // torn down; otherwise POLLHUP can race the ACK and look like a
        // terminal disconnect even after receiver adoption.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });
}

CacheSessionDispatcher make_dispatcher(const EndpointFixture &fixture,
                                       std::chrono::milliseconds timeout =
                                           std::chrono::milliseconds(250)) {
    return CacheSessionDispatcher(fixture.identity, fixture.endpoint, timeout);
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
    // Exact immutable READY lease identity: GUID, digest, listener node, and
    // all three OS credentials must be captured from this private endpoint.
    {
        EndpointFixture fixture({7, 11});
        CHECK(fixture.endpoint.valid(), "OnDemandEndpoint has complete immutable identity");
        CHECK(fixture.endpoint.lease_identity == fixture.identity,
              "endpoint lease identity is exact generation and attempt");
        CHECK(fixture.endpoint.f_store_guid ==
                  icecc::p50::f_store_guid_for_incarnation(fixture.identity),
              "endpoint F_STORE_GUID binds the incarnation");
        CHECK(fixture.endpoint.socket_path_digest == icecc::digest128(fixture.path),
              "endpoint digest binds the exact socket path");
        CHECK(fixture.endpoint.listener_device != 0 && fixture.endpoint.listener_inode != 0,
              "endpoint captures listener device and inode");
        CHECK(fixture.endpoint.expected_peer.uid == static_cast<uint64_t>(::getuid()) &&
                  fixture.endpoint.expected_peer.gid == static_cast<uint64_t>(::getgid()) &&
                  fixture.endpoint.expected_peer.pid == static_cast<uint64_t>(::getpid()),
              "endpoint captures exact peer UID GID and PID");
        CacheSessionDispatcher dispatcher(fixture.identity);
        CHECK(dispatcher.set_on_demand_endpoint(fixture.endpoint),
              "dispatcher installs the immutable on-demand endpoint lease");
        CHECK(dispatcher.available(), "dispatcher accepts the exact live endpoint lease");
    }

    // Missing endpoint never touches the ordinary stream or releases its fd.
    {
        MsgPair ordinary = ordinary_pair();
        CacheSessionDispatcher dispatcher({8, 1});
        send_cache_session(ordinary.left);
        Msg *decoded = ordinary.right->get_msg(2, true);
        const int owned = ordinary.right->fd;
        const auto outcome = decoded == nullptr
                                 ? icecc::p50::daemon::CacheDispatchOutcome{}
                                 : dispatcher.dispatch(*ordinary.right, 50,
                                                       static_cast<uint32_t>(*decoded));
        delete decoded;
        CHECK(outcome.result == CacheDispatchResult::SidecarUnavailable && !outcome.detached,
              "missing endpoint fails closed before release");
        CHECK(ordinary.right->fd == owned, "missing endpoint retains ordinary fd ownership");
    }

    // Successful handoff: accept, credential check, HELLO/ACK, release proof,
    // SCM_RIGHTS adoption, and ACK are all on one fresh relationship.
    {
        EndpointFixture fixture({9, 2});
        CacheSessionDispatcher dispatcher = make_dispatcher(fixture);
        MsgPair ordinary = ordinary_pair();
        PeerResult peer;
        std::thread server = serve_peer(
            fixture, fixture.identity, 1,
            icecc::p50::local::make_hello_ack(PeerRole::Sidecar, fixture.identity),
            PeerAction::Handoff, peer);
        send_cache_session(ordinary.left);
        Msg *decoded = ordinary.right->get_msg(2, true);
        const int old_fd = ordinary.right->fd;
        const auto outcome = dispatcher.dispatch(*ordinary.right, 50,
                                                  static_cast<uint32_t>(*decoded));
        delete decoded;
        server.join();
        CHECK(peer.accepted && peer.credentials && peer.hello && peer.ack_sent &&
                  peer.operation,
              "fresh endpoint relationship authenticates HELLO and operation");
        CHECK(outcome.result == CacheDispatchResult::Accepted && outcome.detached,
              "successful CACHE_SESSION releases exactly once");
        CHECK(outcome.request.identity == fixture.identity && outcome.request.request_id == 1,
              "successful handoff carries exact identity and request ID 1");
        CHECK(peer.receiver.adopted() &&
                  peer.handoff.status == icecc::p50::local::FdHandoffStatus::Accepted,
              "receiver adopts descriptor before sending ACK");
        CHECK(ordinary.right->fd == -1 && old_fd >= 0,
              "ordinary channel relinquishes descriptor only after release proof");
        auto adopted = peer.receiver.take_adopted_fd();
        CHECK(adopted.valid() && ::fcntl(adopted.get(), F_GETFD) >= 0,
              "adopted descriptor remains valid and owned by receiver");
        adopted.reset();
        CHECK(dispatcher.available(), "endpoint lease remains after successful TU");
    }

    // A following ordinary byte is a release barrier.  The control peer has
    // still completed a fresh handshake, but no descriptor may be detached.
    {
        EndpointFixture fixture({10, 3});
        CacheSessionDispatcher dispatcher = make_dispatcher(fixture);
        MsgPair ordinary = ordinary_pair();
        PeerResult peer;
        std::thread server = serve_peer(
            fixture, fixture.identity, 1,
            icecc::p50::local::make_hello_ack(PeerRole::Sidecar, fixture.identity),
            PeerAction::CloseAfterAck, peer);
        send_cache_session(ordinary.left);
        Msg *decoded = ordinary.right->get_msg(2, true);
        const unsigned char following = 0x43;
        CHECK(::send(ordinary.left->fd, &following, 1, MSG_NOSIGNAL) == 1,
              "following ordinary byte queued for release barrier");
        const int owned = ordinary.right->fd;
        const auto outcome = dispatcher.dispatch(*ordinary.right, 50,
                                                  static_cast<uint32_t>(*decoded));
        delete decoded;
        server.join();
        CHECK(peer.hello && peer.ack_sent && peer.operation,
              "release barrier uses a fresh authenticated operation relationship");
        CHECK(outcome.result == CacheDispatchResult::ReleaseRefused && !outcome.detached,
              "buffered ordinary byte refuses descriptor handoff");
        CHECK(ordinary.right->fd == owned, "release refusal retains ordinary fd");
        CHECK(dispatcher.available(), "release refusal retains endpoint lease");
    }

    // The handshake's ACK delay is bounded by the same absolute deadline as
    // connect and HELLO.  The endpoint remains available after this failure.
    {
        EndpointFixture fixture({11, 4});
        constexpr auto timeout = std::chrono::milliseconds(80);
        CacheSessionDispatcher dispatcher = make_dispatcher(fixture, timeout);
        MsgPair ordinary = ordinary_pair();
        PeerResult peer;
        std::thread server = serve_peer(
            fixture, fixture.identity, 1,
            icecc::p50::local::make_hello_ack(PeerRole::Sidecar, fixture.identity),
            PeerAction::CloseAfterAck, peer, std::chrono::milliseconds(180));
        send_cache_session(ordinary.left);
        Msg *decoded = ordinary.right->get_msg(2, true);
        const auto started = std::chrono::steady_clock::now();
        const auto outcome = dispatcher.dispatch(*ordinary.right, 50,
                                                  static_cast<uint32_t>(*decoded));
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        delete decoded;
        server.join();
        CHECK(!outcome.detached && outcome.result == CacheDispatchResult::SidecarUnavailable,
              "handshake timeout fails before release");
        CHECK(elapsed >= std::chrono::milliseconds(55) && elapsed < std::chrono::milliseconds(300),
              "HELLO and ACK share one absolute bounded deadline");
        CHECK(dispatcher.available(), "handshake timeout retains endpoint lease");
    }

    // Once release has happened, disconnect and no-response timeout are both
    // terminal handoff failures, while the immutable endpoint lease survives.
    for (const bool timeout : {false, true}) {
        EndpointFixture fixture({12, timeout ? 6u : 5u});
        CacheSessionDispatcher dispatcher = make_dispatcher(
            fixture, timeout ? std::chrono::milliseconds(35) : std::chrono::milliseconds(250));
        MsgPair ordinary = ordinary_pair();
        PeerResult peer;
        std::thread server = serve_peer(
            fixture, fixture.identity, 1,
            icecc::p50::local::make_hello_ack(PeerRole::Sidecar, fixture.identity),
            timeout ? PeerAction::WaitAfterAck : PeerAction::CloseAfterAck, peer);
        send_cache_session(ordinary.left);
        Msg *decoded = ordinary.right->get_msg(2, true);
        const auto outcome = dispatcher.dispatch(*ordinary.right, 50,
                                                  static_cast<uint32_t>(*decoded));
        delete decoded;
        server.join();
        CHECK(peer.hello && peer.ack_sent && peer.operation,
              "post-release terminal case completed HELLO/ACK and operation");
        CHECK(outcome.result == CacheDispatchResult::HandoffFailed && outcome.detached,
              timeout ? "post-release handoff timeout is fail-closed"
                      : "post-release peer disconnect is fail-closed");
        CHECK(dispatcher.available(), "post-release failure retains endpoint lease");
    }

    // Wrong/stale ACKs are rejected before the ordinary descriptor can be
    // released, including both stale generation and wrong attempt variants.
    for (const Identity wrong : {Identity{13, 8}, Identity{14, 7}}) {
        EndpointFixture fixture({13, 7});
        CacheSessionDispatcher dispatcher = make_dispatcher(fixture);
        MsgPair ordinary = ordinary_pair();
        PeerResult peer;
        std::thread server = serve_peer(
            fixture, fixture.identity, 1,
            icecc::p50::local::make_hello_ack(PeerRole::Sidecar, wrong),
            PeerAction::CloseAfterAck, peer);
        send_cache_session(ordinary.left);
        Msg *decoded = ordinary.right->get_msg(2, true);
        const int owned = ordinary.right->fd;
        const auto outcome = dispatcher.dispatch(*ordinary.right, 50,
                                                  static_cast<uint32_t>(*decoded));
        delete decoded;
        server.join();
        CHECK(peer.hello && peer.ack_sent, "wrong-ACK peer received the expected HELLO");
        CHECK(outcome.result == CacheDispatchResult::SidecarUnavailable && !outcome.detached,
              "stale or wrong ACK is rejected before release");
        CHECK(ordinary.right->fd == owned, "wrong ACK retains ordinary descriptor ownership");
    }

    // Exact path replacement, digest, GUID, device, and inode mismatches are
    // all rejected.  Replacement is never unlinked by the old lease owner.
    {
        EndpointFixture fixture({15, 1});
        const OnDemandEndpoint original = fixture.endpoint;
        OnDemandEndpoint bad_digest = original;
        bad_digest.socket_path_digest = icecc::digest128(original.socket_path + "-different");
        CHECK(!CacheSessionDispatcher(fixture.identity, bad_digest).available(),
              "wrong endpoint path digest is rejected");
        OnDemandEndpoint bad_guid = original;
        bad_guid.f_store_guid.bytes[0] ^= 1;
        CHECK(!CacheSessionDispatcher(fixture.identity, bad_guid).available(),
              "wrong endpoint F_STORE_GUID is rejected");
        OnDemandEndpoint bad_device = original;
        ++bad_device.listener_device;
        CHECK(!CacheSessionDispatcher(fixture.identity, bad_device).available(),
              "wrong endpoint listener device is rejected");
        OnDemandEndpoint bad_inode = original;
        ++bad_inode.listener_inode;
        CHECK(!CacheSessionDispatcher(fixture.identity, bad_inode).available(),
              "wrong endpoint listener inode is rejected");

        CHECK(::close(fixture.listener) == 0 && ::unlink(fixture.path.c_str()) == 0,
              "original endpoint removed for exact replacement test");
        fixture.listener = -1;
        // Keep a distinct socket node allocated so the replacement cannot
        // accidentally reuse the just-unlinked inode on filesystems that do
        // immediate inode recycling.
        const std::string guard_path = fixture.root + "/inode-guard.sock";
        Status guard_status = Status::InvalidArgument;
        const int guard = icecc::p50::local::listen_unix(guard_path, 1, &guard_status);
        CHECK(guard >= 0 && guard_status == Status::Ok,
              "inode guard preserves exact replacement identity distinction");
        Status replacement_status = Status::InvalidArgument;
        const int replacement = icecc::p50::local::listen_unix(
            fixture.path, 4, &replacement_status);
        CHECK(replacement >= 0 && replacement_status == Status::Ok,
              "replacement endpoint binds the exact old path");
        CHECK(!CacheSessionDispatcher(fixture.identity, original).available(),
              "exact path replacement with new dev/inode is rejected");
        if (replacement >= 0) {
            struct stat info{};
            CHECK(::lstat(fixture.path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode),
                  "replacement endpoint remains present after old lease rejection");
            (void)::close(replacement);
            (void)::unlink(fixture.path.c_str());
        }
        if (guard >= 0) {
            (void)::close(guard);
            (void)::unlink(guard_path.c_str());
        }
    }

    // P49 and ordinary jobs never enter the P50 CACHE_SESSION dispatcher.
    {
        MsgPair p49 = ordinary_pair(49);
        CacheSessionDispatcher dispatcher({16, 1});
        CHECK(dispatcher.dispatch(*p49.right, 49, 0x50f00000u).result ==
                  CacheDispatchResult::NotCacheSession,
              "P49 discriminator is rejected before cache dispatch");
        CHECK(dispatcher.dispatch(*p49.right, 50, 0x00000042u).result ==
                  CacheDispatchResult::NotCacheSession,
              "normal ordinary job is not misclassified as CACHE_SESSION");
    }

    // Two successive TUs use fresh accepted relationships and monotonically
    // numbered requests 1 and 2 while retaining one immutable endpoint lease.
    {
        EndpointFixture fixture({17, 2});
        CacheSessionDispatcher dispatcher = make_dispatcher(fixture);
        for (uint64_t request_id = 1; request_id <= 2; ++request_id) {
            MsgPair ordinary = ordinary_pair();
            PeerResult peer;
            std::thread server = serve_peer(
                fixture, fixture.identity, request_id,
                icecc::p50::local::make_hello_ack(PeerRole::Sidecar, fixture.identity),
                PeerAction::Handoff, peer);
            send_cache_session(ordinary.left);
            Msg *decoded = ordinary.right->get_msg(2, true);
            const auto outcome = dispatcher.dispatch(*ordinary.right, 50,
                                                      static_cast<uint32_t>(*decoded));
            delete decoded;
            server.join();
            CHECK(peer.hello && peer.ack_sent && peer.operation && peer.receiver.adopted() &&
                      peer.handoff.status == icecc::p50::local::FdHandoffStatus::Accepted,
                  "each TU establishes a fresh accepted relationship");
            CHECK(outcome.result == CacheDispatchResult::Accepted && outcome.detached &&
                      outcome.request.request_id == request_id,
                  request_id == 1 ? "fresh relationship request_id == 1"
                                  : "fresh relationship request_id == 2");
            auto adopted = peer.receiver.take_adopted_fd();
            adopted.reset();
            CHECK(dispatcher.available(), "successive TU retains endpoint lease");
        }
    }

    return failures == 0 ? 0 : 1;
}
