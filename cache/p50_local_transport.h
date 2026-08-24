#pragma once

// Protocol-50 local transport (S2 groundwork).
//
// This is deliberately a small, synchronous boundary.  A relationship owns
// exactly one SingleWriter object; callers must not write the descriptor by
// any other means.  SingleWriter rejects a second write while one is in
// progress, so a future async adapter can put its bounded queue in front of
// this boundary without allowing frame interleaving.

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <compare>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <sys/un.h>
#include <utility>
#include <vector>

namespace icecc::p50::local {

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

enum class Status {
    Ok = 0,
    InvalidArgument,
    InvalidPath,
    IoError,
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
Status write_frame(int fd, const Frame& frame);

class SingleWriter {
public:
    explicit SingleWriter(int fd) noexcept : fd_(fd) {}
    SingleWriter(const SingleWriter&) = delete;
    SingleWriter& operator=(const SingleWriter&) = delete;

    // A concurrent caller gets Busy.  There is intentionally no implicit
    // queue: one bounded queue and one writer belong to the relationship.
    Status send(const Frame& frame) noexcept;

private:
    int fd_ = -1;
    std::atomic_flag writing_ = ATOMIC_FLAG_INIT;
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
// path; callers must use a private runtime directory and remove their socket
// path during supervisor shutdown.
int listen_unix(const std::string& path, int backlog, Status* status = nullptr) noexcept;
int connect_unix(const std::string& path, Status* status = nullptr) noexcept;
int accept_unix(int listener_fd, Status* status = nullptr) noexcept;

} // namespace icecc::p50::local
