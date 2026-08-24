#pragma once

// Protocol-50 local transport (S2 groundwork).
//
// This is deliberately a small, synchronous boundary.  A relationship owns
// exactly one move-only Connection; callers use its send() method and cannot
// accidentally create a second framed writer for the same relationship.

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <compare>
#include <functional>
#include <limits>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <sys/un.h>
#include <utility>
#include <vector>

namespace icecc::p50::local {

namespace detail {

enum class DeadlinePollResult {
    Ready,
    Timeout,
    Error,
};

// Shared by framed local transport and descriptor handoff.  Poll timeout
// conversion floors to milliseconds and probes with zero below one
// millisecond; it never adds a rounding constant to a duration.  Terminal
// poll bits are checked before requested readiness in both directions, so a
// POLLERR/POLLHUP/POLLNVAL indication cannot be masked by POLLIN or POLLOUT.
inline DeadlinePollResult wait_for_io(
    int fd, short events,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (fd < 0)
        return DeadlinePollResult::Error;

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return DeadlinePollResult::Timeout;

        const auto remaining = deadline - now;
        int poll_timeout = 0;
        const auto max_poll_duration =
            std::chrono::milliseconds(std::numeric_limits<int>::max());
        if (remaining >= max_poll_duration) {
            poll_timeout = std::numeric_limits<int>::max();
        } else {
            const auto whole_milliseconds =
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            poll_timeout = whole_milliseconds <= 0
                               ? 0
                               : static_cast<int>(whole_milliseconds);
        }

        struct pollfd descriptor{fd, events, 0};
        const int ready = ::poll(&descriptor, 1, poll_timeout);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return DeadlinePollResult::Error;
        }
        if (ready == 0)
            continue;

        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return DeadlinePollResult::Error;
        if ((descriptor.revents & events) != 0)
            return DeadlinePollResult::Ready;
        return DeadlinePollResult::Error;
    }
}

} // namespace detail

inline constexpr uint16_t kProtocolVersion = 1;
inline constexpr size_t kFrameHeaderSize = 28;
inline constexpr size_t kMaxFramePayload = 64u * 1024u;
inline constexpr size_t kMaxUnixPath = sizeof(sockaddr_un::sun_path) - 1;

enum class MessageType : uint16_t {
    Hello = 1,
    HelloAck = 2,
    Data = 3,
    Goodbye = 4,
};

enum class PeerRole : uint8_t {
    Daemon = 1,
    Sidecar = 2,
};

struct Identity {
    uint64_t generation = 0;
    uint64_t attempt = 0;
    auto operator<=>(const Identity&) const = default;
};

struct Frame {
    uint16_t version = kProtocolVersion;
    MessageType type = MessageType::Data;
    Identity identity{};
    std::vector<uint8_t> payload;
    auto operator<=>(const Frame&) const = default;
};

struct CredentialExpectation;
class FdHandoffSender;
class FdHandoffReceiver;

enum class Status {
    Ok = 0,
    InvalidArgument,
    InvalidPath,
    IoError,
    ListenerNodeLeftForCleanup,
    CleanEof,
    Truncated,
    Malformed,
    Oversize,
    UnsupportedVersion,
    StaleGeneration,
    IdentityMismatch,
    WrongRole,
    PeerCredentialUnavailable,
    PeerCredentialMismatch,
    Busy,
    SignalProtectionUnavailable,
    Timeout,
};

const char* status_name(Status status) noexcept;

// Encodes exactly one frame.  The result is empty and status is set on error.
std::vector<uint8_t> encode_frame(const Frame& frame, Status* status = nullptr);

// Decodes exactly one frame.  Extra bytes are rejected rather than silently
// treated as a second frame; stream callers must preserve those bytes for the
// next read themselves.
Status decode_frame(std::span<const uint8_t> bytes, Frame& frame);

// Blocking exact-I/O helpers.  They never allocate based on an unbounded wire
// value: the advertised payload length is checked before allocation.
Status read_frame(int fd, Frame& frame);

class Connection {
public:
    // Takes ownership of fd.  On failure (including unavailable SIGPIPE
    // protection) the descriptor is closed and the connection is invalid.
    explicit Connection(int fd) noexcept;
    ~Connection();
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    // Borrowed descriptor identity for a coordinating owner.  The caller
    // must not close or use the returned descriptor after this Connection is
    // destroyed; it is intended only for dup()/shutdown() cancellation.
    [[nodiscard]] int native_handle() const noexcept { return fd_; }
    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] bool cloexec() const noexcept;

    // Verifies the peer while retaining descriptor ownership in this object.
    // Callers do not need (and cannot borrow) a second raw descriptor.
    Status verify_peer_credentials(const CredentialExpectation& expected) const noexcept;
    [[nodiscard]] bool peer_credentials_verified() const noexcept {
        return peer_credentials_verified_;
    }

    // Reads one exact frame under a single absolute wall-time bound covering
    // the header and payload together.  A peer cannot renew the budget by
    // trickling individual bytes, so one control connection cannot prevent
    // bounded signal-driven shutdown or listener progress.
    Status receive_with_timeout(Frame& frame, int timeout_ms) noexcept;

    // A concurrent caller gets Busy.  There is intentionally no implicit
    // queue: one bounded queue and one writer belong to the relationship.
    Status send(const Frame& frame) noexcept;

    // Sends one complete encoded frame under one absolute wall-time budget.
    // The frame is encoded before any bytes are written, and every partial
    // write reuses the same deadline.  A timeout never waits for a blocking
    // write to drain and never releases the one-writer gate early.
    Status send_until(const Frame& frame,
                      std::chrono::steady_clock::time_point deadline) noexcept;

    // Reads one complete encoded frame under the supplied absolute deadline.
    // The same deadline covers both the fixed header and the payload, so a
    // peer cannot extend the operation by sending a partial frame slowly.
    Status receive_until(Frame& frame,
                         std::chrono::steady_clock::time_point deadline) noexcept;
    Status receive(Frame& frame) noexcept;

private:
    friend class FdHandoffSender;
    friend class FdHandoffReceiver;
    void close() noexcept;
    int fd_ = -1;
    Status status_ = Status::InvalidArgument;
    std::atomic_flag writing_ = ATOMIC_FLAG_INIT;
    mutable bool peer_credentials_verified_ = false;
};

Frame make_hello(PeerRole role, Identity identity);
Frame make_hello_ack(PeerRole role, Identity identity);

// Checks the connection generation first, then the logical attempt.  This is
// usable for every message type, not only the initial handshake.
Status validate_identity(const Frame& frame, Identity expected_identity);

// Validates the role and both identity components.  A generation mismatch is
// reported separately so callers can distinguish stale reconnects from a
// wrong attempt on the current generation.
Status validate_handshake(const Frame& frame, MessageType expected_type,
                          PeerRole expected_peer, Identity expected_identity);

struct PeerCredential {
    uint64_t uid = 0;
    uint64_t gid = 0;
    uint64_t pid = 0;
    auto operator<=>(const PeerCredential&) const = default;
};

struct CredentialExpectation {
    std::optional<uint64_t> uid;
    std::optional<uint64_t> gid;
    std::optional<uint64_t> pid;

    [[nodiscard]] bool specified() const noexcept {
        return uid.has_value() || gid.has_value() || pid.has_value();
    }
};

using PeerCredentialProvider =
    std::function<std::optional<PeerCredential>(int fd)>;

// On Linux, the default provider uses SO_PEERCRED.  On platforms without a
// supported peer-credential primitive it returns nullopt (fail closed).
std::optional<PeerCredential> query_peer_credentials(int fd) noexcept;

// The provider is a test/integration seam.  An omitted provider always uses
// the OS query and therefore cannot accidentally turn unsupported platforms
// into an accepted connection.
Status verify_peer_credentials(int fd, const CredentialExpectation& expected,
                               const PeerCredentialProvider& provider = {});

// Portable AF_UNIX/SOCK_STREAM setup helpers.  They never unlink an existing
// path or a node after bind.  If bind succeeds but chmod, node validation, or
// listen fails, listen_unix returns ListenerNodeLeftForCleanup and leaves the
// node for identity-safe private-directory cleanup by its owner.
int listen_unix(const std::string& path, int backlog, Status* status = nullptr) noexcept;
Connection connect_unix(const std::string& path, Status* status = nullptr) noexcept;
// Connects to a private listener under one absolute steady-clock deadline.
// The returned descriptor is blocking and CLOEXEC, just like connect_unix().
// A pending nonblocking connect uses detail::wait_for_io() and never changes
// the flags of a descriptor shared with another Connection.
Connection connect_unix_until(
    const std::string& path, std::chrono::steady_clock::time_point deadline,
    Status* status = nullptr) noexcept;
Connection accept_unix(int listener_fd, Status* status = nullptr) noexcept;

#if defined(ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS)
// Compile-time-only test seam.  The production library does not declare or
// emit this entry point; the focused transport test compiles the transport
// source itself with ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS defined.
using ListenPostBindTestHook = bool (*)(const char* path) noexcept;
int listen_unix_with_test_hook(const std::string& path, int backlog,
                               Status* status, ListenPostBindTestHook hook) noexcept;
#endif

} // namespace icecc::p50::local
