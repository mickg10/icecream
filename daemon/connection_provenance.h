#pragma once

// Bounded provenance for an ordinary daemon wrapper connection.  The lease is
// a value: asynchronous work carries only ConnectionLeaseId and re-enters the
// event loop through ConnectionLeaseRegistry.  No lease is recycled, even
// when a file descriptor or Client allocation address is reused.

#include <cstdint>
#include <optional>
#include <sys/types.h>
#include <unordered_map>

class Client;
class MsgChannel;

enum class ListenerKind : uint8_t {
    UnixLocal = 0,
    TcpLoopback,
    TcpRemote,
};

struct PeerCredentials {
    uid_t uid = 0;
    gid_t gid = 0;
    pid_t pid = 0;
    bool valid = false;

    bool complete() const noexcept { return valid && pid > 0; }
    bool operator==(const PeerCredentials&) const = default;
};

struct ConnectionLeaseId {
    uint64_t daemon_generation = 0;
    uint64_t connection_sequence = 0;

    bool valid() const noexcept {
        return daemon_generation != 0 && connection_sequence != 0;
    }
    bool operator==(const ConnectionLeaseId&) const = default;
};

struct ConnectionProvenance {
    ConnectionLeaseId lease{};
    ListenerKind listener = ListenerKind::TcpRemote;
    PeerCredentials peer{};

    bool cache_eligible() const noexcept {
        return lease.valid() && listener == ListenerKind::UnixLocal && peer.complete();
    }
};

struct CacheHandoffProjection {
    uint32_t port = 0;
    uint32_t protocol = 0;
    uint32_t profile_mask = 0;
};

CacheHandoffProjection project_cache_handoff(const ConnectionProvenance &provenance,
                                             uint32_t port, uint32_t protocol,
                                             uint32_t profile_mask) noexcept;

struct ConnectionLeaseTarget {
    Client *client = nullptr;
    MsgChannel *channel = nullptr;
};

struct ConnectionLeaseRecord {
    ConnectionLeaseId lease{};
    ConnectionProvenance provenance{};
    ConnectionLeaseTarget target{};
};

class ConnectionLeaseRegistry {
public:
    // Generation is immutable for the lifetime of the daemon.  A zero value
    // is replaced with a nonzero value so a partially initialized daemon can
    // never mint an apparently valid lease.
    explicit ConnectionLeaseRegistry(uint64_t daemon_generation,
                                     uint64_t first_sequence = 1) noexcept;

    ConnectionLeaseRegistry(const ConnectionLeaseRegistry&) = delete;
    ConnectionLeaseRegistry& operator=(const ConnectionLeaseRegistry&) = delete;

    std::optional<ConnectionProvenance> allocate(ListenerKind listener,
                                                 PeerCredentials peer) noexcept;
    bool bind(ConnectionLeaseId lease, Client *client, MsgChannel *channel) noexcept;

    // The first lookup is intentionally read-only.  The second API verifies
    // both the value lease and exact live object identities before a future
    // one-use SCM_RIGHTS source ingress may consume it.
    std::optional<ConnectionLeaseRecord> lookup(ConnectionLeaseId lease) const noexcept;
    std::optional<ConnectionLeaseRecord> revalidate(ConnectionLeaseId lease,
                                                     PeerCredentials peer) const noexcept;
    std::optional<ConnectionLeaseRecord> revalidate(ConnectionLeaseId lease,
                                                     Client *client,
                                                     MsgChannel *channel,
                                                     PeerCredentials peer) const noexcept;

    // Removal is performed before Client/channel destruction.  Repeated
    // cancellation is harmless and stale delayed callbacks then fail closed.
    bool cancel(ConnectionLeaseId lease) noexcept;

    [[nodiscard]] uint64_t daemon_generation() const noexcept { return daemon_generation_; }
    [[nodiscard]] uint64_t next_sequence() const noexcept { return next_sequence_; }
    [[nodiscard]] bool sequence_exhausted() const noexcept { return sequence_exhausted_; }
    [[nodiscard]] size_t size() const noexcept { return records_.size(); }

#ifdef ICECC_CONNECTION_PROVENANCE_TEST_HOOKS
    void set_next_sequence_for_test(uint64_t sequence) noexcept;
#endif

private:
    struct Hash {
        size_t operator()(ConnectionLeaseId lease) const noexcept {
            size_t h = std::hash<uint64_t>{}(lease.daemon_generation);
            h ^= std::hash<uint64_t>{}(lease.connection_sequence) +
                 static_cast<size_t>(0x9e3779b9u) + (h << 6) + (h >> 2);
            return h;
        }
    };

    uint64_t daemon_generation_ = 0;
    uint64_t next_sequence_ = 1;
    bool sequence_exhausted_ = false;
    std::unordered_map<ConnectionLeaseId, ConnectionLeaseRecord, Hash> records_;
};

// Listener classification is based on the listener that accepted the fd, not
// on the peer's address.  This keeps loopback TCP legacy-compatible while
// making only AF_UNIX + complete credentials cache eligible.
ListenerKind classify_listener(int accepted_listener_fd, int unix_listener_fd,
                               int tcp_loopback_listener_fd,
                               int tcp_remote_listener_fd) noexcept;

// Must be called immediately after accept() for AF_UNIX.  Failure is not an
// accept failure: the wrapper remains usable through the legacy path, but its
// cache eligibility is permanently closed.
bool capture_unix_peer_credentials(int fd, PeerCredentials &out) noexcept;
