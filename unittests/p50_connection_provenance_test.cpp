#include "../daemon/connection_provenance.h"

#include <cstdio>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int failures = 0;
#define CHECK(expr, text) do { \
    if (!(expr)) { std::fprintf(stderr, "FAILED - %s\n", text); ++failures; } \
    else std::fprintf(stderr, "ok - %s\n", text); \
} while (0)

void test_listener_and_credentials()
{
    CHECK(classify_listener(3, 3, 4, 5) == ListenerKind::UnixLocal,
          "AF_UNIX listener is classified UnixLocal");
    CHECK(classify_listener(4, 3, 4, 5) == ListenerKind::TcpLoopback,
          "loopback TCP listener is classified TcpLoopback");
    CHECK(classify_listener(5, 3, 4, 5) == ListenerKind::TcpRemote,
          "remote TCP listener is classified TcpRemote");

    int fds[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
          "AF_UNIX socketpair created for immediate credential capture");
    PeerCredentials peer;
    CHECK(capture_unix_peer_credentials(fds[0], peer) && peer.complete(),
          "complete SO_PEERCRED captured immediately");
    ::close(fds[0]);
    ::close(fds[1]);

    ConnectionLeaseRegistry registry(41);
    auto unix_lease = registry.allocate(ListenerKind::UnixLocal, peer);
    auto tcp_lease = registry.allocate(ListenerKind::TcpLoopback, PeerCredentials{});
    CHECK(unix_lease && unix_lease->cache_eligible(),
          "AF_UNIX complete-credential wrapper is cache eligible");
    CHECK(tcp_lease && !tcp_lease->cache_eligible(),
          "TCP wrapper remains legacy-usable but cache ineligible");
    const CacheHandoffProjection valid_projection = project_cache_handoff(
        *unix_lease, 17, 50, 3);
    CHECK(valid_projection.port == 17 && valid_projection.protocol == 50 &&
              valid_projection.profile_mask == 3,
          "AF_UNIX projection retains the exact valid cache triple");
    const CacheHandoffProjection tcp_projection = project_cache_handoff(
        *tcp_lease, 17, 50, 3);
    CHECK(tcp_projection.port == 0 && tcp_projection.protocol == 0 &&
              tcp_projection.profile_mask == 0,
          "TCP projection is canonically 0/0/0");
}

void test_delayed_callback_and_peer_binding()
{
    ConnectionLeaseRegistry registry(42);
    Client *client = reinterpret_cast<Client *>(static_cast<uintptr_t>(0x1000));
    MsgChannel *channel = reinterpret_cast<MsgChannel *>(static_cast<uintptr_t>(0x2000));
    PeerCredentials peer{1000, 1000, 1234, true};
    auto first = registry.allocate(ListenerKind::UnixLocal, peer);
    CHECK(first && registry.bind(first->lease, client, channel),
          "first wrapper bound by immutable lease value");
    CHECK(registry.lookup(first->lease).has_value(), "live lease lookup succeeds");
    CHECK(registry.revalidate(first->lease, client, channel, peer).has_value(),
          "second lookup/revalidation accepts exact target and credentials");
    CHECK(registry.revalidate(first->lease, peer).has_value(),
          "value-only revalidation API is available for source ingress");
    PeerCredentials other_pid = peer;
    other_pid.pid = 5678;
    CHECK(!registry.revalidate(first->lease, client, channel, other_pid).has_value(),
          "same UID with a different PID is refused");

    CHECK(registry.cancel(first->lease), "teardown cancels lease before deletion");
    CHECK(!registry.lookup(first->lease).has_value() &&
              !registry.revalidate(first->lease, client, channel, peer).has_value(),
          "delayed callback cannot revive a deleted wrapper");

    // Deliberately reuse both fd-adjacent object identities.  The fresh value
    // is distinct, and the old value remains rejected.
    auto second = registry.allocate(ListenerKind::UnixLocal, peer);
    CHECK(second && second->lease.connection_sequence != first->lease.connection_sequence,
          "pointer/fd reuse receives a fresh monotonic sequence");
    CHECK(registry.bind(second->lease, client, channel), "reused target binds fresh lease");
    CHECK(!registry.lookup(first->lease).has_value() && registry.lookup(second->lease).has_value(),
          "stale delayed value remains refused after target reuse");
}

void test_exhaustion_and_source_gate()
{
    ConnectionLeaseRegistry registry(43);
    registry.set_next_sequence_for_test(std::numeric_limits<uint64_t>::max());
    PeerCredentials peer{1000, 1000, 1234, true};
    auto last = registry.allocate(ListenerKind::UnixLocal, peer);
    CHECK(last && last->lease.valid(), "last nonzero sequence may be allocated once");
    CHECK(registry.sequence_exhausted() && !registry.allocate(ListenerKind::UnixLocal, peer),
          "sequence exhaustion closes only future cache eligibility");

    auto legacy = registry.allocate(ListenerKind::TcpRemote, PeerCredentials{});
    CHECK(!legacy, "exhausted registry does not mint a second cache lease");
    CHECK(registry.cancel(last->lease), "source/deletion gate removes the final lease");
    CHECK(registry.size() == 0, "registry has no live source after deletion gate");
}

} // namespace

int main()
{
    test_listener_and_credentials();
    test_delayed_callback_and_peer_binding();
    test_exhaustion_and_source_gate();
    return failures == 0 ? 0 : 1;
}
