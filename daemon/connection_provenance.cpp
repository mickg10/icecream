#include "connection_provenance.h"

#include <cerrno>
#include <limits>
#include <sys/socket.h>

ConnectionLeaseRegistry::ConnectionLeaseRegistry(uint64_t daemon_generation,
                                                 uint64_t first_sequence) noexcept
    : daemon_generation_(daemon_generation == 0 ? 1 : daemon_generation),
      next_sequence_(first_sequence == 0 ? 1 : first_sequence) {}

CacheHandoffProjection project_cache_handoff(const ConnectionProvenance &provenance,
                                             uint32_t port, uint32_t protocol,
                                             uint32_t profile_mask) noexcept {
    if (!provenance.cache_eligible())
        return CacheHandoffProjection{};
    return CacheHandoffProjection{port, protocol, profile_mask};
}

std::optional<ConnectionProvenance>
ConnectionLeaseRegistry::allocate(ListenerKind listener, PeerCredentials peer) noexcept {
    if (sequence_exhausted_ || next_sequence_ == 0)
        return std::nullopt;

    const ConnectionLeaseId lease{daemon_generation_, next_sequence_};
    ConnectionProvenance provenance{lease, listener, peer};
    try {
        records_.emplace(lease, ConnectionLeaseRecord{lease, provenance, {}});
    } catch (...) {
        // Allocation failure is a cache-eligibility failure only.  The
        // caller may still construct a legacy wrapper without a lease.
        return std::nullopt;
    }

    if (next_sequence_ == std::numeric_limits<uint64_t>::max()) {
        next_sequence_ = 0;
        sequence_exhausted_ = true;
    } else {
        ++next_sequence_;
    }
    return provenance;
}

bool ConnectionLeaseRegistry::bind(ConnectionLeaseId lease, Client *client,
                                   MsgChannel *channel) noexcept {
    if (!lease.valid() || !client || !channel)
        return false;
    auto it = records_.find(lease);
    if (it == records_.end() || it->second.target.client || it->second.target.channel)
        return false;
    it->second.target = ConnectionLeaseTarget{client, channel};
    return true;
}

std::optional<ConnectionLeaseRecord>
ConnectionLeaseRegistry::lookup(ConnectionLeaseId lease) const noexcept {
    const auto it = records_.find(lease);
    if (it == records_.end() || !it->second.target.client || !it->second.target.channel)
        return std::nullopt;
    return it->second;
}

std::optional<ConnectionLeaseRecord>
ConnectionLeaseRegistry::revalidate(ConnectionLeaseId lease,
                                     PeerCredentials peer) const noexcept {
    const auto record = lookup(lease);
    if (!record || !(record->provenance.peer == peer) ||
        !record->provenance.cache_eligible())
        return std::nullopt;
    return record;
}

std::optional<ConnectionLeaseRecord>
ConnectionLeaseRegistry::revalidate(ConnectionLeaseId lease, Client *client,
                                     MsgChannel *channel,
                                     PeerCredentials peer) const noexcept {
    const auto record = lookup(lease);
    if (!record || record->target.client != client || record->target.channel != channel)
        return std::nullopt;
    if (!(record->provenance.peer == peer) || !record->provenance.cache_eligible())
        return std::nullopt;
    return record;
}

bool ConnectionLeaseRegistry::cancel(ConnectionLeaseId lease) noexcept {
    if (!lease.valid())
        return false;
    return records_.erase(lease) != 0;
}

#ifdef ICECC_CONNECTION_PROVENANCE_TEST_HOOKS
void ConnectionLeaseRegistry::set_next_sequence_for_test(uint64_t sequence) noexcept {
    next_sequence_ = sequence == 0 ? 1 : sequence;
    sequence_exhausted_ = false;
}
#endif

ListenerKind classify_listener(int accepted_listener_fd, int unix_listener_fd,
                               int tcp_loopback_listener_fd,
                               int tcp_remote_listener_fd) noexcept {
    if (accepted_listener_fd == unix_listener_fd && unix_listener_fd >= 0)
        return ListenerKind::UnixLocal;
    if (accepted_listener_fd == tcp_loopback_listener_fd && tcp_loopback_listener_fd >= 0)
        return ListenerKind::TcpLoopback;
    (void)tcp_remote_listener_fd;
    return ListenerKind::TcpRemote;
}

bool capture_unix_peer_credentials(int fd, PeerCredentials &out) noexcept {
    out = PeerCredentials{};
#if defined(__linux__) && defined(SO_PEERCRED)
    if (fd < 0)
        return false;
    struct ucred peer{};
    socklen_t peer_len = sizeof(peer);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0 ||
        peer_len < sizeof(peer))
        return false;
    out.uid = peer.uid;
    out.gid = peer.gid;
    out.pid = peer.pid;
    out.valid = true;
    return out.complete();
#else
    (void)fd;
    return false;
#endif
}
