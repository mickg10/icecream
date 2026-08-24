#include "p50_local_transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace icecc::p50::local {
namespace {

constexpr std::array<uint8_t, 4> kMagic{'P', '5', '0', 'L'};
constexpr size_t kPayloadLengthOffset = 8;
constexpr size_t kGenerationOffset = 12;
constexpr size_t kAttemptOffset = 20;

void put_u16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
}

void put_u32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

void put_u64(uint8_t* out, uint64_t value) {
    for (size_t i = 0; i != 8; ++i)
        out[i] = static_cast<uint8_t>(value >> (56 - i * 8));
}

uint16_t get_u16(const uint8_t* in) {
    return static_cast<uint16_t>(in[0] << 8 | in[1]);
}

uint32_t get_u32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) << 24 | static_cast<uint32_t>(in[1]) << 16 |
           static_cast<uint32_t>(in[2]) << 8 | static_cast<uint32_t>(in[3]);
}

uint64_t get_u64(const uint8_t* in) {
    uint64_t value = 0;
    for (size_t i = 0; i != 8; ++i)
        value = (value << 8) | in[i];
    return value;
}

bool valid_type(uint16_t type) {
    return type >= static_cast<uint16_t>(MessageType::Hello) &&
           type <= static_cast<uint16_t>(MessageType::Goodbye);
}

Status read_exact(int fd, std::span<uint8_t> out) {
    size_t done = 0;
    while (done != out.size()) {
        const ssize_t count = ::read(fd, out.data() + done, out.size() - done);
        if (count == 0)
            return Status::Truncated;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return Status::IoError;
        }
        done += static_cast<size_t>(count);
    }
    return Status::Ok;
}

Status write_all(int fd, std::span<const uint8_t> bytes) {
    size_t done = 0;
    while (done != bytes.size()) {
        int send_flags = 0;
#if defined(MSG_NOSIGNAL)
        send_flags = MSG_NOSIGNAL;
#endif
        const ssize_t count = ::send(fd, bytes.data() + done, bytes.size() - done, send_flags);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return Status::IoError;
        }
        if (count == 0)
            return Status::IoError;
        done += static_cast<size_t>(count);
    }
    return Status::Ok;
}

Status set_status(Status value, Status* output) {
    if (output != nullptr)
        *output = value;
    return value;
}

bool fill_address(const std::string& path, sockaddr_un& address) {
    if (path.empty() || path.size() > kMaxUnixPath)
        return false;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    return true;
}

} // namespace

const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid-argument";
    case Status::InvalidPath: return "invalid-path";
    case Status::IoError: return "io-error";
    case Status::Truncated: return "truncated";
    case Status::Malformed: return "malformed";
    case Status::Oversize: return "oversize";
    case Status::UnsupportedVersion: return "unsupported-version";
    case Status::StaleGeneration: return "stale-generation";
    case Status::IdentityMismatch: return "identity-mismatch";
    case Status::WrongRole: return "wrong-role";
    case Status::PeerCredentialUnavailable: return "peer-credential-unavailable";
    case Status::PeerCredentialMismatch: return "peer-credential-mismatch";
    case Status::Busy: return "busy";
    }
    return "unknown";
}

std::vector<uint8_t> encode_frame(const Frame& frame, Status* status) {
    if (frame.version != kProtocolVersion || !valid_type(static_cast<uint16_t>(frame.type)) ||
        frame.payload.size() > kMaxFramePayload) {
        set_status(frame.version != kProtocolVersion ? Status::UnsupportedVersion
                                                       : frame.payload.size() > kMaxFramePayload
                                                             ? Status::Oversize
                                                             : Status::InvalidArgument,
                   status);
        return {};
    }

    std::vector<uint8_t> encoded(kFrameHeaderSize + frame.payload.size());
    std::copy(kMagic.begin(), kMagic.end(), encoded.begin());
    put_u16(encoded.data() + 4, frame.version);
    put_u16(encoded.data() + 6, static_cast<uint16_t>(frame.type));
    put_u32(encoded.data() + kPayloadLengthOffset,
            static_cast<uint32_t>(frame.payload.size()));
    put_u64(encoded.data() + kGenerationOffset, frame.identity.generation);
    put_u64(encoded.data() + kAttemptOffset, frame.identity.attempt);
    std::copy(frame.payload.begin(), frame.payload.end(), encoded.begin() + kFrameHeaderSize);
    set_status(Status::Ok, status);
    return encoded;
}

Status decode_frame(std::span<const uint8_t> bytes, Frame& frame) {
    if (bytes.size() < kFrameHeaderSize)
        return Status::Truncated;
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
        return Status::Malformed;

    const uint16_t version = get_u16(bytes.data() + 4);
    if (version != kProtocolVersion)
        return Status::UnsupportedVersion;
    const uint16_t raw_type = get_u16(bytes.data() + 6);
    if (!valid_type(raw_type))
        return Status::Malformed;
    const uint32_t payload_length = get_u32(bytes.data() + kPayloadLengthOffset);
    if (payload_length > kMaxFramePayload)
        return Status::Oversize;
    if (bytes.size() < kFrameHeaderSize + payload_length)
        return Status::Truncated;
    if (bytes.size() != kFrameHeaderSize + payload_length)
        return Status::Malformed;

    Frame decoded;
    decoded.version = version;
    decoded.type = static_cast<MessageType>(raw_type);
    decoded.identity.generation = get_u64(bytes.data() + kGenerationOffset);
    decoded.identity.attempt = get_u64(bytes.data() + kAttemptOffset);
    decoded.payload.assign(bytes.begin() + kFrameHeaderSize, bytes.end());
    frame = std::move(decoded);
    return Status::Ok;
}

Status read_frame(int fd, Frame& frame) {
    if (fd < 0)
        return Status::InvalidArgument;
    std::array<uint8_t, kFrameHeaderSize> header{};
    Status status = read_exact(fd, header);
    if (status != Status::Ok)
        return status;
    const uint32_t payload_length = get_u32(header.data() + kPayloadLengthOffset);
    if (payload_length > kMaxFramePayload)
        return Status::Oversize;
    std::vector<uint8_t> encoded(kFrameHeaderSize + payload_length);
    std::copy(header.begin(), header.end(), encoded.begin());
    status = read_exact(fd, std::span<uint8_t>(encoded).subspan(kFrameHeaderSize));
    if (status != Status::Ok)
        return status;
    return decode_frame(encoded, frame);
}

Status write_frame(int fd, const Frame& frame) {
    if (fd < 0)
        return Status::InvalidArgument;
    Status status = Status::Ok;
    const std::vector<uint8_t> encoded = encode_frame(frame, &status);
    return status == Status::Ok ? write_all(fd, encoded) : status;
}

Status SingleWriter::send(const Frame& frame) noexcept {
    if (writing_.test_and_set(std::memory_order_acquire))
        return Status::Busy;
    Status status = Status::IoError;
    try {
        status = write_frame(fd_, frame);
    } catch (...) {
        status = Status::IoError;
    }
    writing_.clear(std::memory_order_release);
    return status;
}

Frame make_hello(PeerRole role, Identity identity) {
    return Frame{kProtocolVersion, MessageType::Hello, identity,
                 {static_cast<uint8_t>(role)}};
}

Frame make_hello_ack(PeerRole role, Identity identity) {
    return Frame{kProtocolVersion, MessageType::HelloAck, identity,
                 {static_cast<uint8_t>(role)}};
}

Status validate_identity(const Frame& frame, Identity expected_identity) {
    if (frame.version != kProtocolVersion)
        return Status::UnsupportedVersion;
    if (frame.identity.generation != expected_identity.generation)
        return Status::StaleGeneration;
    if (frame.identity.attempt != expected_identity.attempt)
        return Status::IdentityMismatch;
    return Status::Ok;
}

Status validate_handshake(const Frame& frame, MessageType expected_type,
                          PeerRole expected_peer, Identity expected_identity) {
    if (frame.version != kProtocolVersion)
        return Status::UnsupportedVersion;
    if (frame.type != expected_type || frame.payload.size() != 1)
        return Status::Malformed;
    const Status identity_status = validate_identity(frame, expected_identity);
    if (identity_status != Status::Ok)
        return identity_status;
    if (frame.payload.front() != static_cast<uint8_t>(expected_peer))
        return Status::WrongRole;
    return Status::Ok;
}

std::optional<PeerCredential> query_peer_credentials(int fd) noexcept {
    if (fd < 0)
        return std::nullopt;
#if defined(__linux__) && defined(SO_PEERCRED)
    struct LinuxPeerCred {
        pid_t pid;
        uid_t uid;
        gid_t gid;
    } credentials{};
    socklen_t length = sizeof(credentials);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 ||
        length < sizeof(credentials))
        return std::nullopt;
    return PeerCredential{static_cast<uint64_t>(credentials.uid),
                          static_cast<uint64_t>(credentials.gid),
                          static_cast<uint64_t>(credentials.pid)};
#else
    (void)fd;
    return std::nullopt;
#endif
}

Status verify_peer_credentials(int fd, const CredentialExpectation& expected,
                               const PeerCredentialProvider& provider) {
    if (fd < 0 || !expected.specified())
        return Status::InvalidArgument;
    const std::optional<PeerCredential> actual =
        provider ? provider(fd) : query_peer_credentials(fd);
    if (!actual.has_value())
        return Status::PeerCredentialUnavailable;
    if ((expected.uid && *expected.uid != actual->uid) ||
        (expected.gid && *expected.gid != actual->gid) ||
        (expected.pid && *expected.pid != actual->pid))
        return Status::PeerCredentialMismatch;
    return Status::Ok;
}

int listen_unix(const std::string& path, int backlog, Status* status) noexcept {
    sockaddr_un address{};
    if (backlog < 1 || !fill_address(path, address)) {
        set_status(path.empty() || path.size() > kMaxUnixPath ? Status::InvalidPath
                                                               : Status::InvalidArgument,
                   status);
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        set_status(Status::IoError, status);
        return -1;
    }
    // Refuse to replace an existing endpoint.  The supervisor owns cleanup;
    // silently unlinking here could disconnect a live sidecar.
    const socklen_t address_length =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), address_length) != 0 ||
        ::listen(fd, backlog) != 0) {
        ::close(fd);
        set_status(Status::IoError, status);
        return -1;
    }
    set_status(Status::Ok, status);
    return fd;
}

int connect_unix(const std::string& path, Status* status) noexcept {
    sockaddr_un address{};
    if (!fill_address(path, address)) {
        set_status(Status::InvalidPath, status);
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        set_status(Status::IoError, status);
        return -1;
    }
    const socklen_t address_length =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), address_length) != 0) {
        ::close(fd);
        set_status(Status::IoError, status);
        return -1;
    }
    set_status(Status::Ok, status);
    return fd;
}

int accept_unix(int listener_fd, Status* status) noexcept {
    if (listener_fd < 0) {
        set_status(Status::InvalidArgument, status);
        return -1;
    }
    const int fd = ::accept(listener_fd, nullptr, nullptr);
    if (fd < 0) {
        set_status(Status::IoError, status);
        return -1;
    }
    set_status(Status::Ok, status);
    return fd;
}

} // namespace icecc::p50::local
