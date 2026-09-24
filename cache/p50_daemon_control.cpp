#include "p50_daemon_control.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace icecc::p50::local {
namespace {

constexpr size_t kHandoffBytes = 40;
constexpr uint16_t kHandoffVersion = 1;
constexpr uint16_t kRequest = 1;
constexpr uint16_t kAck = 2;
constexpr uint32_t kAckCode = 1;
constexpr std::array<uint8_t, 4> kMagic{'P', '5', '0', 'F'};

void put16(uint8_t* p, uint16_t v) noexcept { p[0] = v >> 8; p[1] = v; }
void put32(uint8_t* p, uint32_t v) noexcept {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
void put64(uint8_t* p, uint64_t v) noexcept {
    for (size_t i = 0; i != 8; ++i) p[i] = static_cast<uint8_t>(v >> (56 - i * 8));
}
uint16_t get16(const uint8_t* p) noexcept { return uint16_t(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t* p) noexcept {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint64_t get64(const uint8_t* p) noexcept {
    uint64_t v = 0; for (size_t i = 0; i != 8; ++i) v = (v << 8) | p[i]; return v;
}

std::array<uint8_t, kHandoffBytes> handoff_wire(uint16_t type,
                                                 const ControlOperation& op,
                                                 uint32_t code) noexcept {
    std::array<uint8_t, kHandoffBytes> wire{};
    std::copy(kMagic.begin(), kMagic.end(), wire.begin());
    put16(wire.data() + 4, kHandoffVersion);
    put16(wire.data() + 6, type);
    put32(wire.data() + 8, kHandoffBytes);
    put64(wire.data() + 12, op.identity.generation);
    put64(wire.data() + 20, op.identity.attempt);
    put64(wire.data() + 28, op.request_id);
    put32(wire.data() + 36, code);
    return wire;
}

bool nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && (flags & O_NONBLOCK) != 0;
}

bool ensure_nonblocking(int fd) noexcept {
    const int flags = fd >= 0 ? ::fcntl(fd, F_GETFL) : -1;
    return flags >= 0 && ((flags & O_NONBLOCK) != 0 ||
                          ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

bool valid_limits(const DaemonControlLimits& limits) noexcept {
    return limits.syscalls_per_turn != 0 && limits.bytes_per_turn != 0;
}

} // namespace

const char* daemon_control_status_name(DaemonControlStatus status) noexcept {
    switch (status) {
    case DaemonControlStatus::Idle: return "idle";
    case DaemonControlStatus::InProgress: return "in-progress";
    case DaemonControlStatus::Complete: return "complete";
    case DaemonControlStatus::InvalidArgument: return "invalid-argument";
    case DaemonControlStatus::Timeout: return "timeout";
    case DaemonControlStatus::Disconnected: return "disconnected";
    case DaemonControlStatus::IoError: return "io-error";
    case DaemonControlStatus::Malformed: return "malformed";
    case DaemonControlStatus::CredentialFailure: return "credential-failure";
    case DaemonControlStatus::IdentityMismatch: return "identity-mismatch";
    case DaemonControlStatus::OperationMismatch: return "operation-mismatch";
    case DaemonControlStatus::Truncated: return "truncated";
    case DaemonControlStatus::ControlTruncated: return "control-truncated";
    case DaemonControlStatus::ExtraFd: return "extra-fd";
    case DaemonControlStatus::MissingFd: return "missing-fd";
    case DaemonControlStatus::TrailingData: return "trailing-data";
    }
    return "unknown";
}

DaemonControlOperation::~DaemonControlOperation() { close_fd(); }

DaemonControlOperation::DaemonControlOperation(DaemonControlOperation&& other) noexcept
    : fd_(other.fd_), transfer_fd_(other.transfer_fd_), own_fd_(other.own_fd_),
      rights_sent_(other.rights_sent_), peer_queried_(other.peer_queried_),
      phase_(other.phase_), status_(other.status_), deadline_(other.deadline_),
      limits_(other.limits_), credentials_(other.credentials_), operation_(other.operation_),
      connect_path_(std::move(other.connect_path_)),
      hello_(std::move(other.hello_)),
      control_(std::move(other.control_)), handoff_(other.handoff_), ack_(other.ack_),
      frame_read_(std::move(other.frame_read_)), frame_expected_(other.frame_expected_),
      offset_(other.offset_), ack_offset_(other.ack_offset_), last_calls_(other.last_calls_),
      last_bytes_(other.last_bytes_), peer_(other.peer_),
      lifecycle_mode_(other.lifecycle_mode_),
      lifecycle_goodbye_(std::move(other.lifecycle_goodbye_)),
      lifecycle_result_(other.lifecycle_result_),
      source_transfer_result_(other.source_transfer_result_) {
    other.fd_ = -1; other.transfer_fd_ = -1; other.own_fd_ = false;
    other.status_ = DaemonControlStatus::Idle;
}

DaemonControlOperation& DaemonControlOperation::operator=(DaemonControlOperation&& other) noexcept {
    if (this == &other) return *this;
    close_fd();
    fd_ = other.fd_; transfer_fd_ = other.transfer_fd_; own_fd_ = other.own_fd_;
    rights_sent_ = other.rights_sent_; peer_queried_ = other.peer_queried_;
    phase_ = other.phase_; status_ = other.status_; deadline_ = other.deadline_;
    limits_ = other.limits_; credentials_ = other.credentials_; operation_ = other.operation_;
    connect_path_ = std::move(other.connect_path_);
    hello_ = std::move(other.hello_);
    control_ = std::move(other.control_); handoff_ = other.handoff_; ack_ = other.ack_;
    frame_read_ = std::move(other.frame_read_); frame_expected_ = other.frame_expected_;
    offset_ = other.offset_; ack_offset_ = other.ack_offset_; last_calls_ = other.last_calls_;
    last_bytes_ = other.last_bytes_; peer_ = other.peer_;
    lifecycle_mode_ = other.lifecycle_mode_;
    lifecycle_goodbye_ = std::move(other.lifecycle_goodbye_);
    lifecycle_result_ = other.lifecycle_result_;
    source_transfer_result_ = other.source_transfer_result_;
    other.fd_ = -1; other.transfer_fd_ = -1; other.own_fd_ = false;
    other.status_ = DaemonControlStatus::Idle;
    return *this;
}

void DaemonControlOperation::close_fd() noexcept {
    if (fd_ >= 0 && own_fd_) ::close(fd_);
    fd_ = -1; own_fd_ = false;
    if (transfer_fd_ >= 0) ::close(transfer_fd_);
    transfer_fd_ = -1;
    connect_path_.clear();
}

void DaemonControlOperation::fail(DaemonControlStatus status) noexcept {
    status_ = status;
    close_fd();
    phase_ = Phase::None;
}

bool DaemonControlOperation::query_peer() noexcept {
    peer_ = query_peer_credentials(fd_);
    peer_queried_ = true;
    return peer_.has_value() && credentials_.uid.has_value() &&
           credentials_.gid.has_value() && peer_->uid == *credentials_.uid &&
           peer_->gid == *credentials_.gid &&
           (!credentials_.pid.has_value() || peer_->pid == *credentials_.pid);
}

DaemonControlStatus DaemonControlOperation::begin(
    const std::string& path, const ControlOperation& operation, int transfer_fd,
    const CredentialExpectation& credentials,
    std::chrono::steady_clock::time_point deadline, DaemonControlLimits limits) noexcept {
    const bool lifecycle = operation.kind == ControlOperationKind::InputLifecycle;
    if (path.empty() || path.size() > kMaxUnixPath ||
        ((lifecycle && transfer_fd != -1) || (!lifecycle && transfer_fd < 0)) ||
        !valid_limits(limits) || deadline <= std::chrono::steady_clock::now()) {
        return status_ = DaemonControlStatus::InvalidArgument;
    }
    int socket_type = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
    socket_type |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK
    socket_type |= SOCK_NONBLOCK;
#endif
    const int socket_fd = ::socket(AF_UNIX, socket_type, 0);
    if (socket_fd < 0) return status_ = DaemonControlStatus::IoError;
    if (!nonblocking(socket_fd)) {
        const int flags = ::fcntl(socket_fd, F_GETFL);
        if (flags < 0 || ::fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            ::close(socket_fd); return status_ = DaemonControlStatus::IoError;
        }
    }
    struct sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    const int result = ::connect(socket_fd, reinterpret_cast<sockaddr*>(&address),
                                 static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1));
    const int connect_errno = errno;
    const DaemonControlStatus started = begin_connected(socket_fd, operation, transfer_fd,
                                                        credentials, deadline, limits,
                                                        DaemonControlFdOwnership::Owned);
    if (started != DaemonControlStatus::InProgress) return started;
    if (result == 0) phase_ = Phase::WriteHello;
    else if (connect_errno == EINPROGRESS || connect_errno == EALREADY ||
             connect_errno == EAGAIN || connect_errno == EINTR)
        phase_ = Phase::Connecting;
    else { fail(DaemonControlStatus::Disconnected); }
    return status_;
}

DaemonControlStatus DaemonControlOperation::begin_connected(
    int nonblocking_fd, const ControlOperation& operation, int transfer_fd,
    const CredentialExpectation& credentials,
    std::chrono::steady_clock::time_point deadline, DaemonControlLimits limits,
    DaemonControlFdOwnership ownership) noexcept {
    const bool lifecycle = operation.kind == ControlOperationKind::InputLifecycle;
    if (nonblocking_fd < 0 ||
        ((lifecycle && transfer_fd != -1) || (!lifecycle && transfer_fd < 0)) ||
        !valid_limits(limits) || deadline <= std::chrono::steady_clock::now() ||
        !credentials.uid.has_value() || !credentials.gid.has_value() ||
        !credentials.pid.has_value()) {
        if (ownership == DaemonControlFdOwnership::Owned && nonblocking_fd >= 0)
            ::close(nonblocking_fd);
        return status_ = DaemonControlStatus::InvalidArgument;
    }
    std::vector<uint8_t> encoded;
    try {
        encoded = encode_control_operation(operation);
    } catch (...) {
        if (ownership == DaemonControlFdOwnership::Owned)
            ::close(nonblocking_fd);
        return status_ = DaemonControlStatus::InvalidArgument;
    }
    if (encoded.empty()) {
        if (ownership == DaemonControlFdOwnership::Owned)
            ::close(nonblocking_fd);
        return status_ = DaemonControlStatus::InvalidArgument;
    }
    close_fd();
    fd_ = nonblocking_fd; own_fd_ = ownership == DaemonControlFdOwnership::Owned;
    transfer_fd_ = transfer_fd;
    try {
        operation_ = operation; credentials_ = credentials; deadline_ = deadline;
        limits_ = limits;
        lifecycle_mode_ = lifecycle;
        hello_ = encode_frame(make_hello(PeerRole::Daemon, operation.identity));
        control_ = encode_frame(Frame{kProtocolVersion, MessageType::Data,
                                      operation.identity, encoded});
    } catch (...) {
        fail(DaemonControlStatus::InvalidArgument);
        return status_;
    }
    if (hello_.empty() || control_.empty()) {
        fail(DaemonControlStatus::InvalidArgument);
        return status_;
    }
    // The canonical P50F request codec reserves the code field and emits zero.
    // Operation kind is carried by the preceding canonical ControlOperation
    // frame and checked against expected_ on the receiving side.
    handoff_ = handoff_wire(kRequest, operation, 0);
    connect_path_.clear();
    phase_ = Phase::WriteHello; status_ = DaemonControlStatus::InProgress;
    offset_ = ack_offset_ = 0; frame_read_.clear(); frame_expected_ = 0;
    rights_sent_ = false; peer_queried_ = false; peer_.reset();
    lifecycle_goodbye_.clear();
    lifecycle_result_.reset();
    source_transfer_result_.reset();
    last_calls_ = last_bytes_ = 0;
    return status_;
}

DaemonControlStatus DaemonControlOperation::begin_authenticated(
    int nonblocking_fd, const ControlOperation& operation, int transfer_fd,
    const CredentialExpectation& credentials, Identity authenticated_identity,
    std::chrono::steady_clock::time_point deadline, DaemonControlLimits limits,
    DaemonControlFdOwnership ownership) noexcept {
    // This entry is called with a private duplicate of the source.  Adopt it
    // before validation so every return path has one unambiguous closer.
    close_fd();
    fd_ = nonblocking_fd;
    own_fd_ = ownership == DaemonControlFdOwnership::Owned;
    transfer_fd_ = transfer_fd;
    if (nonblocking_fd < 0 ||
        (operation.kind != ControlOperationKind::SourceTransfer &&
         operation.kind != ControlOperationKind::P51SourceTransfer) ||
        transfer_fd < 0 || transfer_fd == nonblocking_fd ||
        !valid_limits(limits) ||
        deadline <= std::chrono::steady_clock::now() ||
        operation.identity != authenticated_identity ||
        authenticated_identity.generation == 0 ||
        authenticated_identity.attempt == 0 ||
        !credentials.uid.has_value() || !credentials.gid.has_value()) {
        fail(DaemonControlStatus::InvalidArgument);
        return status_;
    }
    if (!ensure_nonblocking(nonblocking_fd)) {
        fail(DaemonControlStatus::InvalidArgument);
        return status_;
    }
    std::vector<uint8_t> encoded;
    try {
        encoded = encode_control_operation(operation);
    } catch (...) {
        fail(DaemonControlStatus::InvalidArgument);
        return status_;
    }
    if (encoded.empty()) {
        fail(DaemonControlStatus::InvalidArgument);
        return status_;
    }
    try {
        operation_ = operation;
        credentials_ = credentials;
        deadline_ = deadline;
        limits_ = limits;
        lifecycle_mode_ = false;
        hello_.clear();
        control_ = encode_frame(Frame{kProtocolVersion, MessageType::Data,
                                      operation.identity, encoded});
    } catch (...) {
        fail(DaemonControlStatus::InvalidArgument);
        return status_;
    }
    if (control_.empty()) {
        fail(DaemonControlStatus::InvalidArgument);
        return status_;
    }
    handoff_ = handoff_wire(kRequest, operation, 0);
    connect_path_.clear();
    phase_ = Phase::WriteControl;
    status_ = DaemonControlStatus::InProgress;
    offset_ = ack_offset_ = 0;
    frame_read_.clear();
    frame_expected_ = 0;
    rights_sent_ = false;
    peer_queried_ = false;
    peer_.reset();
    lifecycle_goodbye_.clear();
    lifecycle_result_.reset();
    source_transfer_result_.reset();
    // Authentication is part of admission for this entry, before any
    // operation bytes or source rights are emitted.
    if (!query_peer()) {
        fail(DaemonControlStatus::CredentialFailure);
        return status_;
    }
    last_calls_ = last_bytes_ = 0;
    return status_;
}

DaemonControlStatus DaemonControlOperation::begin_connecting(
    const std::string& path, int nonblocking_fd,
    const ControlOperation& operation, int transfer_fd,
    const CredentialExpectation& credentials,
    std::chrono::steady_clock::time_point deadline, DaemonControlLimits limits,
    DaemonControlFdOwnership ownership) noexcept {
    if (path.empty() || path.size() > kMaxUnixPath)
        return status_ = DaemonControlStatus::InvalidArgument;
    const DaemonControlStatus status = begin_connected(
        nonblocking_fd, operation, transfer_fd, credentials, deadline, limits,
        ownership);
    if (status != DaemonControlStatus::InProgress)
        return status;
    // The descriptor is intentionally left unconnected.  The next outer turn
    // owns exactly one connect completion action and enters WriteHello only
    // after that action succeeds.
    connect_path_ = path;
    phase_ = Phase::ConnectPending;
    return status_;
}

short DaemonControlOperation::desired_events() const noexcept {
    if (status_ != DaemonControlStatus::InProgress) return 0;
    if (phase_ == Phase::CheckHelloAckTrailing || phase_ == Phase::CheckAckTrailing ||
        phase_ == Phase::CheckLifecycleReplyTrailing ||
        phase_ == Phase::CheckSourceReplyTrailing)
        return POLLIN | POLLOUT;
    if (phase_ == Phase::ConnectPending || phase_ == Phase::Connecting ||
        phase_ == Phase::WriteHello ||
        phase_ == Phase::WriteControl || phase_ == Phase::WriteHandoff ||
        phase_ == Phase::WriteLifecycleGoodbye)
        return POLLOUT;
    return POLLIN;
}

bool DaemonControlOperation::write_bytes(size_t& offset, const std::vector<uint8_t>& bytes,
                                         size_t& calls, size_t& budget) noexcept {
    if (offset == bytes.size()) return true;
    const size_t amount = std::min(bytes.size() - offset, budget);
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_DONTWAIT
    flags |= MSG_DONTWAIT;
#endif
    const ssize_t count = ::send(fd_, bytes.data() + offset, amount, flags);
    ++calls;
    if (count > 0) { offset += static_cast<size_t>(count); budget -= static_cast<size_t>(count); return offset == bytes.size(); }
    if (count == 0) { fail(DaemonControlStatus::Disconnected); return false; }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return false;
    fail(errno == EPIPE || errno == ECONNRESET ? DaemonControlStatus::Disconnected
                                                : DaemonControlStatus::IoError);
    return false;
}

bool DaemonControlOperation::write_handoff(size_t& calls, size_t& budget) noexcept {
    if (offset_ == handoff_.size()) return true;
    const size_t amount = std::min(handoff_.size() - offset_, budget);
    struct iovec iov{handoff_.data() + offset_, amount};
    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int))> control{};
    struct msghdr message{}; message.msg_iov = &iov; message.msg_iovlen = 1;
    const bool attach_rights = !rights_sent_;
    if (attach_rights) {
        message.msg_control = control.data(); message.msg_controllen = control.size();
        auto* cmsg = reinterpret_cast<cmsghdr*>(control.data());
        cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &transfer_fd_, sizeof(transfer_fd_));
    }
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_DONTWAIT
    flags |= MSG_DONTWAIT;
#endif
    const ssize_t count = ::sendmsg(fd_, &message, flags);
    ++calls;
    if (count > 0) {
        if (attach_rights) rights_sent_ = true; // positive sendmsg is the sole authority.
        offset_ += static_cast<size_t>(count); budget -= static_cast<size_t>(count);
        return offset_ == handoff_.size();
    }
    if (count == 0) { fail(DaemonControlStatus::Disconnected); return false; }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return false;
    fail(errno == EPIPE || errno == ECONNRESET ? DaemonControlStatus::Disconnected
                                                : DaemonControlStatus::IoError);
    return false;
}

bool DaemonControlOperation::read_frame(size_t& calls, size_t& budget) noexcept {
    if (frame_expected_ == 0) {
        try {
            frame_read_.resize(kFrameHeaderSize);
        } catch (...) {
            fail(DaemonControlStatus::Malformed);
            return false;
        }
        frame_expected_ = kFrameHeaderSize;
    }
    // frame_read_ is resized to the total expected size; its initialized prefix
    // is tracked by offset_.  This keeps allocation bounded by the frozen max.
    const size_t remaining = frame_expected_ - offset_;
    const ssize_t count = ::recv(fd_, frame_read_.data() + offset_, std::min(remaining, budget), MSG_DONTWAIT);
    ++calls;
    if (count > 0) { offset_ += static_cast<size_t>(count); budget -= static_cast<size_t>(count); }
    else if (count == 0) { fail(offset_ == 0 ? DaemonControlStatus::Disconnected : DaemonControlStatus::Truncated); return false; }
    else if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return false;
    else { fail(DaemonControlStatus::IoError); return false; }
    if (offset_ == kFrameHeaderSize && frame_expected_ == kFrameHeaderSize) {
        const uint32_t payload = uint32_t(frame_read_[8]) << 24 | uint32_t(frame_read_[9]) << 16 |
                  uint32_t(frame_read_[10]) << 8 | frame_read_[11];
        if (payload > kMaxFramePayload) { fail(DaemonControlStatus::Malformed); return false; }
        frame_expected_ = kFrameHeaderSize + payload;
        try {
            frame_read_.resize(frame_expected_);
        } catch (...) {
            fail(DaemonControlStatus::Malformed);
            return false;
        }
    }
    if (offset_ != frame_expected_) return false;
    Frame frame;
    if (decode_frame(frame_read_, frame) != Status::Ok ||
        frame.type != MessageType::HelloAck || frame.identity != operation_.identity ||
        frame.payload.size() != 1 || frame.payload[0] != static_cast<uint8_t>(PeerRole::Sidecar)) {
        fail(DaemonControlStatus::IdentityMismatch); return false;
    }
    offset_ = 0; frame_read_.clear(); frame_expected_ = 0;
    phase_ = Phase::CheckHelloAckTrailing;
    return true;
}

bool DaemonControlOperation::read_ack(size_t& calls, size_t& budget) noexcept {
    const ssize_t count = ::recv(fd_, ack_.data() + ack_offset_,
                                 std::min(kHandoffBytes - ack_offset_, budget), MSG_DONTWAIT);
    ++calls;
    if (count > 0) { ack_offset_ += static_cast<size_t>(count); budget -= static_cast<size_t>(count); }
    else if (count == 0) { fail(DaemonControlStatus::Truncated); return false; }
    else if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return false;
    else { fail(DaemonControlStatus::IoError); return false; }
    if (ack_offset_ != kHandoffBytes) return false;
    if (!validate_ack()) { fail(DaemonControlStatus::OperationMismatch); return false; }
    phase_ = Phase::CheckAckTrailing;
    return true;
}

bool DaemonControlOperation::read_lifecycle_reply(size_t& calls,
                                                   size_t& budget) noexcept {
    if (frame_expected_ == 0) {
        try {
            frame_read_.resize(kFrameHeaderSize);
        } catch (...) {
            fail(DaemonControlStatus::Malformed);
            return false;
        }
        frame_expected_ = kFrameHeaderSize;
    }
    if (budget == 0)
        return false;
    const size_t remaining = frame_expected_ - offset_;
    const ssize_t count = ::recv(
        fd_, frame_read_.data() + offset_, std::min(remaining, budget), MSG_DONTWAIT);
    ++calls;
    if (count > 0) {
        offset_ += static_cast<size_t>(count);
        budget -= static_cast<size_t>(count);
    } else if (count == 0) {
        fail(offset_ == 0 ? DaemonControlStatus::Disconnected
                          : DaemonControlStatus::Truncated);
        return false;
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        fail(DaemonControlStatus::IoError);
        return false;
    } else {
        return false;
    }

    if (offset_ == kFrameHeaderSize && frame_expected_ == kFrameHeaderSize) {
        const uint32_t payload = uint32_t(frame_read_[8]) << 24 |
                                 uint32_t(frame_read_[9]) << 16 |
                                 uint32_t(frame_read_[10]) << 8 |
                                 frame_read_[11];
        if (payload > kMaxFramePayload) {
            fail(DaemonControlStatus::Malformed);
            return false;
        }
        frame_expected_ = kFrameHeaderSize + payload;
        try {
            frame_read_.resize(frame_expected_);
        } catch (...) {
            fail(DaemonControlStatus::Malformed);
            return false;
        }
    }
    if (offset_ != frame_expected_)
        return false;

    Frame frame;
    ControlOperation observed;
    if (decode_frame(frame_read_, frame) != Status::Ok ||
        frame.type != MessageType::Data || frame.identity != operation_.identity ||
        !decode_control_operation(frame.payload, observed) ||
        observed.kind != ControlOperationKind::InputLifecycle ||
        observed.identity != operation_.identity ||
        observed.request_id != operation_.request_id ||
        observed.input != operation_.input || observed.owner != operation_.owner ||
        observed.lifecycle_action != operation_.lifecycle_action ||
        observed.f_store_generation != operation_.f_store_generation ||
        observed.f_store_guid != operation_.f_store_guid ||
        observed.immutable_size != operation_.immutable_size ||
        observed.immutable_digest != operation_.immutable_digest ||
        observed.retirement_id != operation_.retirement_id ||
        observed.replacement_owner != operation_.replacement_owner ||
        observed.absolute_deadline != operation_.absolute_deadline ||
        !observed.lifecycle_result.has_value()) {
        fail(DaemonControlStatus::OperationMismatch);
        return false;
    }
    lifecycle_result_ = observed.lifecycle_result;
    offset_ = 0;
    frame_read_.clear();
    frame_expected_ = 0;
    phase_ = Phase::CheckLifecycleReplyTrailing;
    return true;
}

bool DaemonControlOperation::read_source_reply(size_t& calls,
                                                size_t& budget) noexcept {
    if (frame_expected_ == 0) {
        try {
            frame_read_.resize(kFrameHeaderSize);
        } catch (...) {
            fail(DaemonControlStatus::Malformed);
            return false;
        }
        frame_expected_ = kFrameHeaderSize;
    }
    if (budget == 0)
        return false;
    const size_t remaining = frame_expected_ - offset_;
    const ssize_t count = ::recv(fd_, frame_read_.data() + offset_,
                                 std::min(remaining, budget), MSG_DONTWAIT);
    ++calls;
    if (count > 0) {
        offset_ += static_cast<size_t>(count);
        budget -= static_cast<size_t>(count);
    } else if (count == 0) {
        fail(offset_ == 0 ? DaemonControlStatus::Disconnected
                          : DaemonControlStatus::Truncated);
        return false;
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        fail(DaemonControlStatus::IoError);
        return false;
    } else {
        return false;
    }
    if (offset_ == kFrameHeaderSize && frame_expected_ == kFrameHeaderSize) {
        const uint32_t payload = uint32_t(frame_read_[8]) << 24 |
                                 uint32_t(frame_read_[9]) << 16 |
                                 uint32_t(frame_read_[10]) << 8 |
                                 frame_read_[11];
        if (payload > kMaxFramePayload) {
            fail(DaemonControlStatus::Malformed);
            return false;
        }
        frame_expected_ = kFrameHeaderSize + payload;
        try {
            frame_read_.resize(frame_expected_);
        } catch (...) {
            fail(DaemonControlStatus::Malformed);
            return false;
        }
    }
    if (offset_ != frame_expected_)
        return false;

    Frame frame;
    ControlOperation observed;
    if (decode_frame(frame_read_, frame) != Status::Ok ||
        frame.type != MessageType::Data || frame.identity != operation_.identity ||
        !decode_control_operation(frame.payload, observed) ||
        observed.kind != operation_.kind ||
        observed.identity != operation_.identity ||
        observed.request_id != operation_.request_id ||
        observed.absolute_deadline != operation_.absolute_deadline ||
        (operation_.kind == ControlOperationKind::SourceTransfer &&
         (observed.source_arm != operation_.source_arm ||
          !observed.source_result.has_value())) ||
        (operation_.kind == ControlOperationKind::P51SourceTransfer &&
         (observed.p51_source_transfer != operation_.p51_source_transfer ||
          !observed.p51_source_transfer_result.has_value()))) {
        fail(DaemonControlStatus::OperationMismatch);
        return false;
    }
    source_transfer_result_ = operation_.kind ==
                                      ControlOperationKind::P51SourceTransfer
                                  ? observed.p51_source_transfer_result
                                  : observed.source_result;
    offset_ = 0;
    frame_read_.clear();
    frame_expected_ = 0;
    phase_ = Phase::CheckSourceReplyTrailing;
    return true;
}

bool DaemonControlOperation::write_lifecycle_goodbye(size_t& calls,
                                                      size_t& budget) noexcept {
    if (lifecycle_goodbye_.empty())
        return false;
    const size_t amount = std::min(lifecycle_goodbye_.size() - offset_, budget);
    if (amount == 0)
        return false;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_DONTWAIT
    flags |= MSG_DONTWAIT;
#endif
    const ssize_t count = ::send(fd_, lifecycle_goodbye_.data() + offset_, amount, flags);
    ++calls;
    if (count > 0) {
        offset_ += static_cast<size_t>(count);
        budget -= static_cast<size_t>(count);
        if (offset_ == lifecycle_goodbye_.size()) {
            status_ = DaemonControlStatus::Complete;
            close_fd();
            phase_ = Phase::None;
        }
        return true;
    }
    if (count == 0) {
        fail(DaemonControlStatus::Disconnected);
        return false;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        return false;
    fail(errno == EPIPE || errno == ECONNRESET ? DaemonControlStatus::Disconnected
                                                : DaemonControlStatus::IoError);
    return false;
}

bool DaemonControlOperation::check_stream_trailing(
    Phase next_phase, size_t& calls, size_t& budget) noexcept {
    if (calls >= limits_.syscalls_per_turn || budget == 0)
        return false;
    uint8_t byte = 0;
    int flags = 0;
#ifdef MSG_PEEK
    flags |= MSG_PEEK;
#endif
#ifdef MSG_DONTWAIT
    flags |= MSG_DONTWAIT;
#endif
    const ssize_t count = ::recv(fd_, &byte, sizeof(byte), flags);
    ++calls;
    if (count > 0) { fail(DaemonControlStatus::TrailingData); return false; }
    if (count == 0) { fail(DaemonControlStatus::Disconnected); return false; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) { phase_ = next_phase; return true; }
    if (errno == EINTR) return false;
    fail(DaemonControlStatus::IoError);
    return false;
}

bool DaemonControlOperation::validate_ack() noexcept {
    return std::equal(kMagic.begin(), kMagic.end(), ack_.begin()) &&
           get16(ack_.data() + 4) == kHandoffVersion && get16(ack_.data() + 6) == kAck &&
           get32(ack_.data() + 8) == kHandoffBytes &&
           get64(ack_.data() + 12) == operation_.identity.generation &&
           get64(ack_.data() + 20) == operation_.identity.attempt &&
           get64(ack_.data() + 28) == operation_.request_id &&
           get32(ack_.data() + 36) == kAckCode;
}

DaemonControlStatus DaemonControlOperation::advance(
    std::chrono::steady_clock::time_point now, short revents) noexcept {
    if (status_ != DaemonControlStatus::InProgress) return status_;
    last_calls_ = last_bytes_ = 0;
    if (now >= deadline_) { fail(DaemonControlStatus::Timeout); return status_; }
    // POLLHUP/ERR may be reported together with readable bytes.  Consume
    // those bytes first so a coalesced final frame is not discarded; only a
    // wake with no readable payload is an immediate disconnect.
    if (phase_ != Phase::ConnectPending &&
        ((revents & POLLNVAL) != 0 ||
         ((revents & (POLLERR | POLLHUP)) != 0 &&
          (revents & POLLIN) == 0))) {
        fail(DaemonControlStatus::Disconnected);
        return status_;
    }
    size_t calls = 0, budget = limits_.bytes_per_turn;
    // Exactly one fallible external operation per outer advance.  In-memory
    // phase transitions happen at the end of that operation and are resumed
    // by the next poll turn; this prevents a hidden send/recv/connect loop.
    if (limits_.syscalls_per_turn == 0 || budget == 0) {
        last_calls_ = 0;
        last_bytes_ = 0;
        return status_;
    }
    if (phase_ == Phase::ConnectPending) {
        if (connect_path_.empty() || connect_path_.size() >= sizeof(sockaddr_un::sun_path)) {
            fail(DaemonControlStatus::InvalidArgument);
            return status_;
        }
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, connect_path_.data(), connect_path_.size());
        address.sun_path[connect_path_.size()] = '\0';
        const int result = ::connect(
            fd_, reinterpret_cast<sockaddr*>(&address),
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                   connect_path_.size() + 1));
        ++calls;
        if (result == 0) {
            connect_path_.clear();
            phase_ = Phase::WriteHello;
        } else if (errno == EINPROGRESS || errno == EALREADY || errno == EAGAIN) {
            connect_path_.clear();
            phase_ = Phase::Connecting;
        } else if (errno != EINTR) {
            fail(DaemonControlStatus::Disconnected);
        }
    } else if (phase_ == Phase::Connecting) {
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
            ++calls;
            fail(DaemonControlStatus::IoError);
        } else {
            ++calls;
            if (error != 0)
                fail(DaemonControlStatus::Disconnected);
            else
                phase_ = Phase::WriteHello;
        }
    } else if (phase_ == Phase::WriteHello) {
        if (!peer_queried_) {
            if (!query_peer())
                fail(DaemonControlStatus::CredentialFailure);
            ++calls;
        } else {
            (void)write_bytes(offset_, hello_, calls, budget);
            if (status_ == DaemonControlStatus::InProgress &&
                offset_ == hello_.size()) {
                offset_ = 0;
                phase_ = Phase::ReadHelloAck;
            }
        }
    } else if (phase_ == Phase::ReadHelloAck) {
        (void)read_frame(calls, budget);
    } else if (phase_ == Phase::CheckHelloAckTrailing) {
        (void)check_stream_trailing(Phase::WriteControl, calls, budget);
    } else if (phase_ == Phase::WriteControl) {
        (void)write_bytes(offset_, control_, calls, budget);
        if (status_ == DaemonControlStatus::InProgress &&
            offset_ == control_.size()) {
            offset_ = 0;
            phase_ = lifecycle_mode_ ? Phase::ReadLifecycleReply : Phase::WriteHandoff;
        }
    } else if (phase_ == Phase::WriteHandoff) {
        (void)write_handoff(calls, budget);
        if (status_ == DaemonControlStatus::InProgress && offset_ == handoff_.size()) {
            offset_ = 0;
            phase_ = Phase::ReadAck;
        }
    } else if (phase_ == Phase::ReadAck) {
        (void)read_ack(calls, budget);
    } else if (phase_ == Phase::CheckAckTrailing) {
        if (operation_.kind == ControlOperationKind::SourceTransfer ||
            operation_.kind == ControlOperationKind::P51SourceTransfer) {
            phase_ = Phase::ReadSourceReply;
        } else if (check_stream_trailing(Phase::None, calls, budget)) {
            (void)::close(transfer_fd_);
            transfer_fd_ = -1;
            status_ = DaemonControlStatus::Complete;
            close_fd();
            phase_ = Phase::None;
        }
    } else if (phase_ == Phase::ReadLifecycleReply) {
        (void)read_lifecycle_reply(calls, budget);
    } else if (phase_ == Phase::CheckLifecycleReplyTrailing) {
        if (check_stream_trailing(Phase::WriteLifecycleGoodbye, calls, budget)) {
            try {
                lifecycle_goodbye_ = encode_frame(
                    Frame{kProtocolVersion, MessageType::Goodbye,
                          operation_.identity, {}});
            } catch (...) {
                fail(DaemonControlStatus::Malformed);
            }
            if (status_ == DaemonControlStatus::InProgress && lifecycle_goodbye_.empty())
                fail(DaemonControlStatus::Malformed);
            else if (status_ == DaemonControlStatus::InProgress) {
                offset_ = 0;
                phase_ = Phase::WriteLifecycleGoodbye;
            }
        }
    } else if (phase_ == Phase::ReadSourceReply) {
        (void)read_source_reply(calls, budget);
    } else if (phase_ == Phase::CheckSourceReplyTrailing) {
        if (check_stream_trailing(Phase::WriteLifecycleGoodbye, calls, budget)) {
            if (transfer_fd_ >= 0) {
                (void)::close(transfer_fd_);
                transfer_fd_ = -1;
            }
            try {
                lifecycle_goodbye_ = encode_frame(
                    Frame{kProtocolVersion, MessageType::Goodbye,
                          operation_.identity, {}});
            } catch (...) {
                fail(DaemonControlStatus::Malformed);
            }
            if (status_ == DaemonControlStatus::InProgress && lifecycle_goodbye_.empty())
                fail(DaemonControlStatus::Malformed);
            else if (status_ == DaemonControlStatus::InProgress) {
                offset_ = 0;
                phase_ = Phase::WriteLifecycleGoodbye;
            }
        }
    } else if (phase_ == Phase::WriteLifecycleGoodbye) {
        (void)write_lifecycle_goodbye(calls, budget);
    } else {
        fail(DaemonControlStatus::IoError);
    }
    last_calls_ = calls;
    last_bytes_ = limits_.bytes_per_turn - budget;
    return status_;
}

DaemonControlHandoffReceiver::~DaemonControlHandoffReceiver() { close_all(); }

void DaemonControlHandoffReceiver::close_all() noexcept {
    if (fd_ >= 0 && own_fd_) ::close(fd_);
    fd_ = -1; own_fd_ = false;
    if (accepted_fd_ >= 0) ::close(accepted_fd_);
    accepted_fd_ = -1;
}

void DaemonControlHandoffReceiver::fail(DaemonControlStatus status) noexcept {
    status_ = status; close_all();
}

DaemonControlStatus DaemonControlHandoffReceiver::begin_connected(
    int nonblocking_fd, const ControlOperation& expected,
    std::chrono::steady_clock::time_point deadline, DaemonControlLimits limits,
    DaemonControlFdOwnership ownership) noexcept {
    bool encoded_valid = false;
    try {
        encoded_valid = !encode_control_operation(expected).empty();
    } catch (...) {
        encoded_valid = false;
    }
    if (nonblocking_fd < 0 || !nonblocking(nonblocking_fd) || !valid_limits(limits) ||
        deadline <= std::chrono::steady_clock::now() || !encoded_valid) {
        if (ownership == DaemonControlFdOwnership::Owned && nonblocking_fd >= 0)
            ::close(nonblocking_fd);
        return status_ = DaemonControlStatus::InvalidArgument;
    }
    close_all(); fd_ = nonblocking_fd; expected_ = expected; deadline_ = deadline;
    own_fd_ = ownership == DaemonControlFdOwnership::Owned;
    // A handoff receiver owns one fallible outer-loop action per advance:
    // either one recvmsg or one ACK send.  Keep the caller's byte budget,
    // but clamp a legacy multi-syscall value to the lifecycle action quota.
    limits_ = limits;
    limits_.syscalls_per_turn = 1;
    status_ = DaemonControlStatus::InProgress; offset_ = 0; ack_offset_ = 0;
    have_rights_ = false; trailing_checked_ = false; fd_count_ = 0;
    datagram_ = false;
    int socket_type = 0;
    socklen_t socket_type_length = sizeof(socket_type);
    if (::getsockopt(nonblocking_fd, SOL_SOCKET, SO_TYPE, &socket_type,
                     &socket_type_length) == 0)
        datagram_ = socket_type == SOCK_DGRAM || socket_type == SOCK_SEQPACKET;
    last_calls_ = 0; last_bytes_ = 0; return status_;
}

short DaemonControlHandoffReceiver::desired_events() const noexcept {
    return status_ == DaemonControlStatus::InProgress
               ? (accepted_fd_ >= 0 && offset_ == wire_.size()
                      ? (trailing_checked_ ? POLLOUT : POLLIN | POLLOUT)
                      : POLLIN)
               : 0;
}

bool DaemonControlHandoffReceiver::validate_wire() noexcept {
    return std::equal(kMagic.begin(), kMagic.end(), wire_.begin()) &&
           get16(wire_.data() + 4) == kHandoffVersion && get16(wire_.data() + 6) == kRequest &&
           get32(wire_.data() + 8) == kHandoffBytes &&
           get64(wire_.data() + 12) == expected_.identity.generation &&
           get64(wire_.data() + 20) == expected_.identity.attempt &&
           get64(wire_.data() + 28) == expected_.request_id &&
           get32(wire_.data() + 36) == 0;
}

bool DaemonControlHandoffReceiver::check_trailing(size_t& calls, size_t& budget) noexcept {
    if (calls >= limits_.syscalls_per_turn || budget == 0)
        return false;
    uint8_t byte = 0;
    int flags = 0;
#ifdef MSG_PEEK
    flags |= MSG_PEEK;
#endif
#ifdef MSG_DONTWAIT
    flags |= MSG_DONTWAIT;
#endif
    const ssize_t count = ::recv(fd_, &byte, sizeof(byte), flags);
    ++calls;
    if (count > 0) { fail(DaemonControlStatus::TrailingData); return false; }
    if (count == 0) { fail(DaemonControlStatus::Disconnected); return false; }
    if (errno == EAGAIN || errno == EWOULDBLOCK) { trailing_checked_ = true; return true; }
    if (errno == EINTR) return false;
    fail(DaemonControlStatus::IoError);
    return false;
}

DaemonControlStatus DaemonControlHandoffReceiver::advance(
    std::chrono::steady_clock::time_point now, short revents) noexcept {
    if (status_ != DaemonControlStatus::InProgress) return status_;
    last_calls_ = last_bytes_ = 0;
    if (now >= deadline_) { fail(DaemonControlStatus::Timeout); return status_; }
    if ((revents & POLLNVAL) != 0 ||
        ((revents & (POLLERR | POLLHUP)) != 0 &&
         (revents & POLLIN) == 0)) {
        fail(DaemonControlStatus::Disconnected);
        return status_;
    }
    size_t calls = 0;
    size_t budget = limits_.bytes_per_turn;
    if (accepted_fd_ >= 0 && offset_ == wire_.size() && !trailing_checked_) {
        if (!check_trailing(calls, budget)) {
            last_calls_ = calls;
            return status_;
        }
        // The trailing-byte probe is one fallible receive action.  The
        // bounded syscall guard below prevents the ACK send from joining
        // this turn; the next poll turn owns the write.  This matters for a
        // coalesced handoff where the old implementation could perform a
        // receive and a send under one adapter advance.
        last_calls_ = calls;
        last_bytes_ = limits_.bytes_per_turn - budget;
    }
    if (calls >= limits_.syscalls_per_turn || budget == 0) {
        last_calls_ = calls;
        return status_;
    }
    if (accepted_fd_ >= 0 && offset_ == wire_.size() && trailing_checked_) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_DONTWAIT
        flags |= MSG_DONTWAIT;
#endif
        const auto ack = handoff_wire(kAck, expected_, kAckCode);
        const size_t amount = std::min(ack.size() - ack_offset_, budget);
        const ssize_t sent = ::send(fd_, ack.data() + ack_offset_, amount, flags);
        ++calls;
        if (sent > 0) {
            ack_offset_ += static_cast<size_t>(sent); budget -= static_cast<size_t>(sent);
            last_bytes_ += static_cast<size_t>(sent);
            if (ack_offset_ == ack.size()) status_ = DaemonControlStatus::Complete;
        }
        else if (sent < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) fail(DaemonControlStatus::Disconnected);
        last_calls_ = calls;
        return status_;
    }
    if (calls >= limits_.syscalls_per_turn || budget == 0) {
        last_calls_ = calls;
        return status_;
    }
    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int) * 4)> control{};
    const size_t remaining = wire_.size() - offset_;
    const size_t amount = std::min(wire_.size() - offset_, budget);
    // A stream peer may coalesce one or more bytes after the fixed handoff
    // packet.  When the per-turn byte budget permits, receive one sentinel
    // byte in the same recvmsg call so an overlong packet is rejected without
    // adding a second external action to this turn.  The bounded scratch byte
    // is never admitted into wire_; it exists only for the trailing fence.
    std::array<uint8_t, kHandoffBytes + 1> chunk{};
    const size_t request_bytes = !datagram_ && remaining < budget
                                     ? remaining + 1
                                     : amount;
    iovec iov{chunk.data(), request_bytes};
    msghdr message{}; message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    int flags = MSG_DONTWAIT;
#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    const ssize_t received = ::recvmsg(fd_, &message, flags);
    ++calls;
    last_calls_ = calls;
    if (received > 0) {
        const size_t received_bytes = static_cast<size_t>(received);
        const size_t copied = std::min(received_bytes, remaining);
        std::copy_n(chunk.data(), copied, wire_.data() + offset_);
        offset_ += copied;
        budget -= received_bytes;
        last_bytes_ = limits_.bytes_per_turn - budget;
    }
    else if (received == 0) { fail(DaemonControlStatus::Truncated); return status_; }
    else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) { fail(DaemonControlStatus::IoError); return status_; }
    if (received < 0) {
        last_calls_ = calls;
        return status_;
    }
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr; cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0) || ((cmsg->cmsg_len - CMSG_LEN(0)) % sizeof(int)) != 0) {
            fail(DaemonControlStatus::Malformed); return status_;
        }
        const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const auto* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        fd_count_ += count;
        for (size_t i = 0; i != count; ++i) {
            if (accepted_fd_ < 0 && fd_count_ == 1) accepted_fd_ = fds[i];
            else ::close(fds[i]);
        }
        have_rights_ = true;
    }
    if ((message.msg_flags & MSG_TRUNC) != 0) { fail(DaemonControlStatus::Truncated); return status_; }
    if ((message.msg_flags & MSG_CTRUNC) != 0) { fail(DaemonControlStatus::ControlTruncated); return status_; }
    if (fd_count_ > 1) { fail(DaemonControlStatus::ExtraFd); return status_; }
    if (static_cast<size_t>(received) > remaining) {
        fail(DaemonControlStatus::TrailingData);
        return status_;
    }
    if (offset_ != wire_.size()) return status_;
    if (!have_rights_) { fail(DaemonControlStatus::MissingFd); return status_; }
    if (!validate_wire()) { fail(DaemonControlStatus::OperationMismatch); return status_; }
    // The descriptor is now retained as an admitted value until the ACK is sent.
    ack_offset_ = 0;
    last_calls_ = calls;
    return status_;
}

int DaemonControlHandoffReceiver::take_fd() noexcept {
    if (status_ != DaemonControlStatus::Complete || accepted_fd_ < 0) return -1;
    const int result = accepted_fd_; accepted_fd_ = -1; return result;
}

DaemonControlPollAdapter::Registration DaemonControlPollAdapter::add(
    DaemonControlOperation& operation) noexcept {
    for (const Entry& entry : operations_)
        if (entry.operation == &operation) return entry.registration;
    const Registration registration = next_registration_++;
    operations_.push_back(Entry{registration, &operation});
    return registration;
}

bool DaemonControlPollAdapter::remove(Registration registration) noexcept {
    const auto found = std::find_if(
        operations_.begin(), operations_.end(),
        [registration](const Entry& entry) { return entry.registration == registration; });
    if (found == operations_.end()) return false;
    const size_t index = static_cast<size_t>(found - operations_.begin());
    operations_.erase(found);
    if (operations_.empty()) { cursor_ = 0; return true; }
    if (cursor_ > index) --cursor_;
    if (cursor_ >= operations_.size()) cursor_ = 0;
    return true;
}

size_t DaemonControlPollAdapter::advance_ready(
    std::chrono::steady_clock::time_point now, const std::vector<short>& revents) noexcept {
    if (operations_.empty()) return 0;
    size_t advanced = 0;
    const size_t count = operations_.size();
    for (size_t i = 0; i != count; ++i) {
        const size_t index = (cursor_ + i) % count;
        DaemonControlOperation& operation = *operations_[index].operation;
        const short requested = operation.desired_events();
        const short terminal = POLLERR | POLLHUP | POLLNVAL;
        const bool ready = index < revents.size() &&
                           ((revents[index] & requested) != 0 ||
                            (revents[index] & terminal) != 0);
        if (operation.status() == DaemonControlStatus::InProgress &&
            (ready || operation.deadline_expired(now))) {
            operation.advance(now, ready ? revents[index] : 0); ++advanced;
        }
    }
    cursor_ = (cursor_ + 1) % count;
    return advanced;
}

} // namespace icecc::p50::local
