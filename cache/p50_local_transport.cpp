#include "p50_local_transport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>

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

#if defined(ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS)
thread_local ConnectAttemptTestHook connect_attempt_test_hook = nullptr;
#endif

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

Status read_exact(int fd, std::span<uint8_t> out, bool clean_eof) {
    size_t done = 0;
    while (done != out.size()) {
        const ssize_t count = ::read(fd, out.data() + done, out.size() - done);
        if (count == 0)
            return done == 0 && clean_eof ? Status::CleanEof : Status::Truncated;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return Status::IoError;
        }
        done += static_cast<size_t>(count);
    }
    return Status::Ok;
}

Status read_exact_until(int fd, std::span<uint8_t> out, bool clean_eof,
                        std::chrono::steady_clock::time_point deadline) {
#if !defined(MSG_DONTWAIT)
    // The bounded API cannot safely emulate per-call nonblocking I/O by
    // toggling O_NONBLOCK: that flag belongs to the shared open file
    // description and would race a concurrent reader.  Fail closed on a
    // platform without MSG_DONTWAIT rather than changing reader semantics.
    (void)fd;
    (void)out;
    (void)clean_eof;
    (void)deadline;
    return Status::IoError;
#else
    size_t done = 0;
    while (done != out.size()) {
        const auto waited = detail::wait_for_io(fd, POLLIN, deadline);
        if (waited == detail::DeadlinePollResult::Timeout)
            return Status::Timeout;
        if (waited == detail::DeadlinePollResult::Error)
            return Status::IoError;
        const ssize_t count =
            ::recv(fd, out.data() + done, out.size() - done, MSG_DONTWAIT);
        if (count == 0)
            return done == 0 && clean_eof ? Status::CleanEof : Status::Truncated;
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return Status::IoError;
        }
        done += static_cast<size_t>(count);
    }
    return Status::Ok;
#endif
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

Status write_all_until(int fd, std::span<const uint8_t> bytes,
                       std::chrono::steady_clock::time_point deadline) noexcept {
#if !defined(MSG_DONTWAIT)
    // See read_exact_until: per-call bounded I/O must not mutate O_NONBLOCK on
    // a descriptor shared with another Connection or another thread.
    (void)fd;
    (void)bytes;
    (void)deadline;
    return Status::IoError;
#else
    size_t done = 0;
    while (done != bytes.size()) {
        const auto waited = detail::wait_for_io(fd, POLLOUT, deadline);
        if (waited == detail::DeadlinePollResult::Timeout)
            return Status::Timeout;
        if (waited == detail::DeadlinePollResult::Error)
            return Status::IoError;

        int send_flags = MSG_DONTWAIT;
#if defined(MSG_NOSIGNAL)
        send_flags |= MSG_NOSIGNAL;
#endif
        const ssize_t count = ::send(fd, bytes.data() + done, bytes.size() - done, send_flags);
        if (count > 0) {
            done += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return Status::IoError;
    }

    return std::chrono::steady_clock::now() >= deadline ? Status::Timeout : Status::Ok;
#endif
}

bool set_cloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0)
        return false;
    if ((flags & FD_CLOEXEC) != 0)
        return true;
    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

int socket_cloexec() {
    int fd = -1;
#if defined(SOCK_CLOEXEC)
    fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 && errno == EINVAL)
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
#else
    fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
#endif
    if (fd < 0)
        return -1;
    if (!set_cloexec(fd)) {
        ::close(fd);
        errno = EIO;
        return -1;
    }
    return fd;
}

enum class ConnectRetryWaitResult {
    Continue,
    Timeout,
    Error,
};

ConnectRetryWaitResult wait_for_connect_retry(
    std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
        return ConnectRetryWaitResult::Timeout;

    // AF_UNIX EAGAIN/EINTR leaves no descriptor that can be waited on.  Probe
    // in short floor-based slices without adding a millisecond (or any other)
    // rounding constant to the caller's absolute deadline.
    const auto remaining = deadline - now;
    const int timeout_ms = remaining >= std::chrono::milliseconds(1) ? 1 : 0;
    const int ready = ::poll(nullptr, 0, timeout_ms);
    if (ready < 0 && errno != EINTR)
        return ConnectRetryWaitResult::Error;
    return std::chrono::steady_clock::now() >= deadline
               ? ConnectRetryWaitResult::Timeout
               : ConnectRetryWaitResult::Continue;
}

int connect_once(int fd, const sockaddr* address, socklen_t address_length) noexcept {
#if defined(ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS)
    if (connect_attempt_test_hook != nullptr)
        return connect_attempt_test_hook(fd, address, address_length);
#endif
    return ::connect(fd, address, address_length);
}

int accept_cloexec(int listener_fd) {
    int fd = -1;
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    fd = ::accept4(listener_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0 && (errno == ENOSYS || errno == EINVAL))
        fd = ::accept(listener_fd, nullptr, nullptr);
#else
    fd = ::accept(listener_fd, nullptr, nullptr);
#endif
    if (fd < 0)
        return -1;
    // The fcntl path is the portability fallback for systems without
    // accept4/SOCK_CLOEXEC, and also verifies the atomic path's result.
    if (!set_cloexec(fd)) {
        ::close(fd);
        errno = EIO;
        return -1;
    }
    return fd;
}

bool configure_sigpipe_protection(int fd) {
#if defined(MSG_NOSIGNAL)
    (void)fd;
    return true;
#elif defined(SO_NOSIGPIPE)
    int enabled = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
#else
    (void)fd;
    return false;
#endif
}

Status set_status(Status value, Status* output) {
    if (output != nullptr)
        *output = value;
    return value;
}

bool fill_address(const std::string& path, sockaddr_un& address) {
    if (path.empty() || path.front() != '/' || path.find('\0') != std::string::npos ||
        path.size() > kMaxUnixPath)
        return false;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    return true;
}

bool private_parent(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return false;
    const std::string parent = slash == 0 ? "/" : path.substr(0, slash);
    struct stat info{};
    if (::lstat(parent.c_str(), &info) != 0 || !S_ISDIR(info.st_mode))
        return false;
    return info.st_uid == ::geteuid() && (info.st_mode & 07777) == 0700;
}

bool private_socket_node(const std::string& path) {
    struct stat info{};
    if (::lstat(path.c_str(), &info) != 0 || !S_ISSOCK(info.st_mode))
        return false;
    return info.st_uid == ::geteuid() && (info.st_mode & 07777) == 0600;
}

Status validate_header(const uint8_t* header, size_t size, uint32_t* payload_length) {
    if (size < kFrameHeaderSize)
        return Status::Truncated;
    if (!std::equal(kMagic.begin(), kMagic.end(), header))
        return Status::Malformed;
    if (get_u16(header + 4) != kProtocolVersion)
        return Status::UnsupportedVersion;
    if (!valid_type(get_u16(header + 6)))
        return Status::Malformed;
    const uint32_t length = get_u32(header + kPayloadLengthOffset);
    if (length > kMaxFramePayload)
        return Status::Oversize;
    if (payload_length != nullptr)
        *payload_length = length;
    return Status::Ok;
}

Status read_frame_until(int fd, Frame& frame,
                        std::chrono::steady_clock::time_point deadline) {
    std::array<uint8_t, kFrameHeaderSize> header{};
    Status status = read_exact_until(fd, header, true, deadline);
    if (status != Status::Ok)
        return status;
    uint32_t payload_length = 0;
    status = validate_header(header.data(), header.size(), &payload_length);
    if (status != Status::Ok)
        return status;
    std::vector<uint8_t> encoded(kFrameHeaderSize + payload_length);
    std::copy(header.begin(), header.end(), encoded.begin());
    status = read_exact_until(
        fd, std::span<uint8_t>(encoded).subspan(kFrameHeaderSize), false, deadline);
    if (status != Status::Ok)
        return status;
    return decode_frame(encoded, frame);
}

} // namespace

const char* status_name(Status status) noexcept {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid-argument";
    case Status::InvalidPath: return "invalid-path";
    case Status::IoError: return "io-error";
    case Status::ListenerNodeLeftForCleanup: return "listener-node-left-for-cleanup";
    case Status::CleanEof: return "clean-eof";
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
    case Status::SignalProtectionUnavailable: return "signal-protection-unavailable";
    case Status::Timeout: return "timeout";
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
    uint32_t payload_length = 0;
    const Status header_status = validate_header(bytes.data(), bytes.size(), &payload_length);
    if (header_status != Status::Ok)
        return header_status;
    if (bytes.size() < kFrameHeaderSize + payload_length)
        return Status::Truncated;
    if (bytes.size() != kFrameHeaderSize + payload_length)
        return Status::Malformed;

    Frame decoded;
    decoded.version = get_u16(bytes.data() + 4);
    decoded.type = static_cast<MessageType>(get_u16(bytes.data() + 6));
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
    Status status = read_exact(fd, header, true);
    if (status != Status::Ok)
        return status;
    uint32_t payload_length = 0;
    status = validate_header(header.data(), header.size(), &payload_length);
    if (status != Status::Ok)
        return status;
    std::vector<uint8_t> encoded(kFrameHeaderSize + payload_length);
    std::copy(header.begin(), header.end(), encoded.begin());
    status = read_exact(fd, std::span<uint8_t>(encoded).subspan(kFrameHeaderSize), false);
    if (status != Status::Ok)
        return status;
    return decode_frame(encoded, frame);
}

Connection::Connection(int fd) noexcept : fd_(fd), status_(Status::Ok) {
    if (fd_ < 0) {
        status_ = Status::InvalidArgument;
        return;
    }
    const bool cloexec_ok = set_cloexec(fd_);
    const bool sigpipe_ok = cloexec_ok && configure_sigpipe_protection(fd_);
    if (!sigpipe_ok) {
        status_ = !cloexec_ok ? Status::IoError : Status::SignalProtectionUnavailable;
        ::close(fd_);
        fd_ = -1;
    }
}

Connection::~Connection() { close(); }

bool Connection::cloexec() const noexcept {
    if (fd_ < 0)
        return false;
    const int flags = ::fcntl(fd_, F_GETFD);
    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

Status Connection::verify_peer_credentials(const CredentialExpectation& expected) const noexcept {
    peer_credentials_verified_ = false;
    if (fd_ < 0)
        return status_;
    const Status status = local::verify_peer_credentials(fd_, expected);
    peer_credentials_verified_ = status == Status::Ok;
    return status;
}

Status Connection::receive_with_timeout(Frame& frame, int timeout_ms) noexcept {
    if (fd_ < 0)
        return status_;
    if (timeout_ms <= 0)
        return Status::InvalidArgument;
    try {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        return receive_until(frame, deadline);
    } catch (...) {
        return Status::IoError;
    }
}

Connection::Connection(Connection&& other) noexcept
    : fd_(other.fd_), status_(other.status_), writing_(ATOMIC_FLAG_INIT),
      peer_credentials_verified_(other.peer_credentials_verified_) {
    other.fd_ = -1;
    other.status_ = Status::InvalidArgument;
    other.peer_credentials_verified_ = false;
}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        status_ = other.status_;
        peer_credentials_verified_ = other.peer_credentials_verified_;
        other.fd_ = -1;
        other.status_ = Status::InvalidArgument;
        other.peer_credentials_verified_ = false;
        writing_.clear(std::memory_order_release);
    }
    return *this;
}

void Connection::close() noexcept {
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
    peer_credentials_verified_ = false;
}

Status Connection::send(const Frame& frame) noexcept {
    if (writing_.test_and_set(std::memory_order_acquire))
        return Status::Busy;
    Status status = Status::IoError;
    try {
        if (fd_ < 0) {
            status = status_;
        } else {
            Status encode_status = Status::Ok;
            const std::vector<uint8_t> encoded = encode_frame(frame, &encode_status);
            status = encode_status == Status::Ok ? write_all(fd_, encoded) : encode_status;
        }
    } catch (...) {
        status = Status::IoError;
    }
    writing_.clear(std::memory_order_release);
    return status;
}

Status Connection::send_until(const Frame& frame,
                              std::chrono::steady_clock::time_point deadline) noexcept {
    if (writing_.test_and_set(std::memory_order_acquire))
        return Status::Busy;
    Status status = Status::IoError;
    try {
        if (fd_ < 0) {
            status = status_;
        } else if (std::chrono::steady_clock::now() >= deadline) {
            status = Status::Timeout;
        } else {
            Status encode_status = Status::Ok;
            const std::vector<uint8_t> encoded = encode_frame(frame, &encode_status);
            if (encode_status != Status::Ok) {
                status = encode_status;
            } else if (std::chrono::steady_clock::now() >= deadline) {
                status = Status::Timeout;
            } else {
                status = write_all_until(fd_, encoded, deadline);
            }
        }
    } catch (...) {
        status = Status::IoError;
    }
    writing_.clear(std::memory_order_release);
    return status;
}

Status Connection::receive_until(Frame& frame,
                                 std::chrono::steady_clock::time_point deadline) noexcept {
    if (fd_ < 0)
        return status_;
    try {
        return read_frame_until(fd_, frame, deadline);
    } catch (...) {
        return Status::IoError;
    }
}

Status Connection::receive(Frame& frame) noexcept {
    return fd_ < 0 ? status_ : read_frame(fd_, frame);
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

#if defined(ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS)
static int listen_unix_impl(const std::string& path, int backlog, Status* status,
                            ListenPostBindTestHook hook) noexcept {
#else
static int listen_unix_impl(const std::string& path, int backlog, Status* status) noexcept {
#endif
    sockaddr_un address{};
    if (backlog < 1 || !fill_address(path, address) || !private_parent(path)) {
        set_status(path.empty() || path.front() != '/' || path.find('\0') != std::string::npos ||
                           path.size() > kMaxUnixPath || !private_parent(path)
                       ? Status::InvalidPath
                                                               : Status::InvalidArgument,
                   status);
        return -1;
    }
    const int fd = socket_cloexec();
    if (fd < 0) {
        set_status(Status::IoError, status);
        return -1;
    }
    // Refuse to replace an existing endpoint.  The supervisor owns cleanup;
    // silently unlinking here could disconnect a live sidecar.
    const socklen_t address_length =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), address_length) != 0) {
        ::close(fd);
        set_status(Status::IoError, status);
        return -1;
    }
    // bind(2) creates the node using the process umask.  Establish and verify
    // the exact private node mode before exposing the listener.
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0 || !private_socket_node(path) ||
        ::listen(fd, backlog) != 0) {
        ::close(fd);
        // bind succeeded, so the pathname may still designate a live node (or
        // may have been replaced by its same-UID owner).  Never unlink by
        // pathname here: the supervisor's identity-safe directory cleanup
        // owns removal of failed nodes.
        set_status(Status::ListenerNodeLeftForCleanup, status);
        return -1;
    }
#if defined(ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS)
    if (hook != nullptr && hook(path.c_str())) {
        ::close(fd);
        set_status(Status::ListenerNodeLeftForCleanup, status);
        return -1;
    }
#endif
    set_status(Status::Ok, status);
    return fd;
}

int listen_unix(const std::string& path, int backlog, Status* status) noexcept {
#if defined(ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS)
    return listen_unix_impl(path, backlog, status, nullptr);
#else
    return listen_unix_impl(path, backlog, status);
#endif
}

#if defined(ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS)
int listen_unix_with_test_hook(const std::string& path, int backlog, Status* status,
                               ListenPostBindTestHook hook) noexcept {
    return listen_unix_impl(path, backlog, status, hook);
}
#endif

Connection connect_unix(const std::string& path, Status* status) noexcept {
    sockaddr_un address{};
    if (!fill_address(path, address) || !private_parent(path) || !private_socket_node(path)) {
        set_status(Status::InvalidPath, status);
        return Connection(-1);
    }
    const int fd = socket_cloexec();
    if (fd < 0) {
        set_status(Status::IoError, status);
        return Connection(-1);
    }
    const socklen_t address_length =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), address_length) != 0) {
        ::close(fd);
        set_status(Status::IoError, status);
        return Connection(-1);
    }
    Connection connection(fd);
    if (!connection.valid()) {
        set_status(connection.status(), status);
        return connection;
    }
    set_status(Status::Ok, status);
    return connection;
}

Connection connect_unix_until(const std::string& path,
                              std::chrono::steady_clock::time_point deadline,
                              Status* status) noexcept {
    sockaddr_un address{};
    if (!fill_address(path, address) || !private_parent(path) || !private_socket_node(path)) {
        set_status(Status::InvalidPath, status);
        return Connection(-1);
    }
    constexpr unsigned kMaxAdmissionAttempts = 4096;
    for (unsigned attempt = 0; attempt != kMaxAdmissionAttempts; ++attempt) {
        if (std::chrono::steady_clock::now() >= deadline) {
            set_status(Status::Timeout, status);
            return Connection(-1);
        }
        // EAGAIN/EINTR closes the old descriptor before this fresh attempt;
        // revalidate both pathname ownership predicates every time so a node
        // replacement cannot be inherited across the retry boundary.
        if (!fill_address(path, address) || !private_parent(path) || !private_socket_node(path)) {
            set_status(Status::InvalidPath, status);
            return Connection(-1);
        }
        const int fd = socket_cloexec();
        if (fd < 0) {
            set_status(Status::IoError, status);
            return Connection(-1);
        }

        const int original_flags = ::fcntl(fd, F_GETFL);
        if (original_flags < 0 ||
            ::fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
            ::close(fd);
            set_status(Status::IoError, status);
            return Connection(-1);
        }
        const socklen_t address_length =
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
        const int connect_result = connect_once(
            fd, reinterpret_cast<const sockaddr*>(&address), address_length);
        const int connect_error = errno;

        if (connect_result < 0 &&
            (connect_error == EINPROGRESS || connect_error == EAGAIN ||
             connect_error == EWOULDBLOCK || connect_error == EINTR)) {
            if (connect_error == EAGAIN || connect_error == EWOULDBLOCK ||
                connect_error == EINTR) {
                ::close(fd);
                const auto retry_wait = wait_for_connect_retry(deadline);
                if (retry_wait == ConnectRetryWaitResult::Timeout) {
                    set_status(Status::Timeout, status);
                    return Connection(-1);
                }
                if (retry_wait == ConnectRetryWaitResult::Error) {
                    set_status(Status::IoError, status);
                    return Connection(-1);
                }
                continue;
            }

            const auto waited = detail::wait_for_io(fd, POLLOUT, deadline);
            if (waited == detail::DeadlinePollResult::Timeout) {
                ::close(fd);
                set_status(Status::Timeout, status);
                return Connection(-1);
            }
            if (waited == detail::DeadlinePollResult::Error) {
                ::close(fd);
                set_status(Status::IoError, status);
                return Connection(-1);
            }
        } else if (connect_result < 0) {
            ::close(fd);
            set_status(Status::IoError, status);
            return Connection(-1);
        }

        int socket_error = 0;
        socklen_t socket_error_length = sizeof(socket_error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                         &socket_error_length) != 0 ||
            socket_error != 0) {
            ::close(fd);
            set_status(Status::IoError, status);
            return Connection(-1);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::close(fd);
            set_status(Status::Timeout, status);
            return Connection(-1);
        }
        if (::fcntl(fd, F_SETFL, original_flags & ~O_NONBLOCK) != 0) {
            ::close(fd);
            set_status(Status::IoError, status);
            return Connection(-1);
        }

        Connection connection(fd);
        if (!connection.valid()) {
            set_status(connection.status(), status);
            return connection;
        }
        set_status(Status::Ok, status);
        return connection;
    }

    // A bounded admission retry count is a distinct fail-closed condition;
    // unlike elapsed wall time, this is not reported as a deadline timeout.
    set_status(Status::IoError, status);
    return Connection(-1);
}

#if defined(ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS)
Connection connect_unix_until_with_test_hook(
    const std::string& path, std::chrono::steady_clock::time_point deadline,
    Status* status, ConnectAttemptTestHook hook) noexcept {
    const ConnectAttemptTestHook previous = connect_attempt_test_hook;
    connect_attempt_test_hook = hook;
    Connection result = connect_unix_until(path, deadline, status);
    connect_attempt_test_hook = previous;
    return result;
}
#endif
Connection accept_unix(int listener_fd, Status* status) noexcept {
    if (listener_fd < 0) {
        set_status(Status::InvalidArgument, status);
        return Connection(-1);
    }
    int fd = -1;
    do {
        fd = accept_cloexec(listener_fd);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        set_status(Status::IoError, status);
        return Connection(-1);
    }
    Connection connection(fd);
    if (!connection.valid()) {
        set_status(connection.status(), status);
        return connection;
    }
    set_status(Status::Ok, status);
    return connection;
}

} // namespace icecc::p50::local
