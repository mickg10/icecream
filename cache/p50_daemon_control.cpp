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
      limits_(other.limits_), operation_(other.operation_), hello_(std::move(other.hello_)),
      control_(std::move(other.control_)), handoff_(other.handoff_), ack_(other.ack_),
      frame_read_(std::move(other.frame_read_)), frame_expected_(other.frame_expected_),
      offset_(other.offset_), ack_offset_(other.ack_offset_), peer_(other.peer_) {
    other.fd_ = -1; other.transfer_fd_ = -1; other.own_fd_ = false;
    other.status_ = DaemonControlStatus::Idle;
}

DaemonControlOperation& DaemonControlOperation::operator=(DaemonControlOperation&& other) noexcept {
    if (this == &other) return *this;
    close_fd();
    fd_ = other.fd_; transfer_fd_ = other.transfer_fd_; own_fd_ = other.own_fd_;
    rights_sent_ = other.rights_sent_; peer_queried_ = other.peer_queried_;
    phase_ = other.phase_; status_ = other.status_; deadline_ = other.deadline_;
    limits_ = other.limits_; operation_ = other.operation_; hello_ = std::move(other.hello_);
    control_ = std::move(other.control_); handoff_ = other.handoff_; ack_ = other.ack_;
    frame_read_ = std::move(other.frame_read_); frame_expected_ = other.frame_expected_;
    offset_ = other.offset_; ack_offset_ = other.ack_offset_; peer_ = other.peer_;
    other.fd_ = -1; other.transfer_fd_ = -1; other.own_fd_ = false;
    other.status_ = DaemonControlStatus::Idle;
    return *this;
}

void DaemonControlOperation::close_fd() noexcept {
    if (fd_ >= 0 && own_fd_) ::close(fd_);
    fd_ = -1; own_fd_ = false;
    if (transfer_fd_ >= 0) ::close(transfer_fd_);
    transfer_fd_ = -1;
}

void DaemonControlOperation::fail(DaemonControlStatus status) noexcept {
    status_ = status;
    close_fd();
    phase_ = Phase::None;
}

bool DaemonControlOperation::query_peer() noexcept {
    peer_ = query_peer_credentials(fd_);
    peer_queried_ = true;
    return peer_.has_value();
}

DaemonControlStatus DaemonControlOperation::begin(
    const std::string& path, const ControlOperation& operation, int transfer_fd,
    std::chrono::steady_clock::time_point deadline, DaemonControlLimits limits) noexcept {
    if (path.empty() || path.size() > kMaxUnixPath || transfer_fd < 0 ||
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
    const DaemonControlStatus started = begin_connected(socket_fd, operation, transfer_fd,
                                                        deadline, limits);
    if (started != DaemonControlStatus::InProgress) { ::close(socket_fd); return started; }
    own_fd_ = true;
    if (result == 0) phase_ = Phase::WriteHello;
    else if (errno == EINPROGRESS || errno == EALREADY || errno == EAGAIN) phase_ = Phase::Connecting;
    else { fail(DaemonControlStatus::Disconnected); }
    return status_;
}

DaemonControlStatus DaemonControlOperation::begin_connected(
    int nonblocking_fd, const ControlOperation& operation, int transfer_fd,
    std::chrono::steady_clock::time_point deadline, DaemonControlLimits limits) noexcept {
    if (nonblocking_fd < 0 || transfer_fd < 0 || !nonblocking(nonblocking_fd) ||
        !valid_limits(limits) || deadline <= std::chrono::steady_clock::now())
        return status_ = DaemonControlStatus::InvalidArgument;
    const std::vector<uint8_t> encoded = encode_control_operation(operation);
    if (encoded.empty()) return status_ = DaemonControlStatus::InvalidArgument;
    close_fd();
    fd_ = nonblocking_fd; own_fd_ = false; transfer_fd_ = transfer_fd;
    operation_ = operation; deadline_ = deadline; limits_ = limits;
    hello_ = encode_frame(make_hello(PeerRole::Daemon, operation.identity));
    control_ = encode_frame(Frame{kProtocolVersion, MessageType::Data,
                                  operation.identity, encoded});
    if (hello_.empty() || control_.empty()) { fail(DaemonControlStatus::InvalidArgument); return status_; }
    handoff_ = handoff_wire(kRequest, operation,
                            static_cast<uint32_t>(operation.kind));
    phase_ = Phase::WriteHello; status_ = DaemonControlStatus::InProgress;
    offset_ = ack_offset_ = 0; frame_read_.clear(); frame_expected_ = 0;
    rights_sent_ = false; peer_queried_ = false; peer_.reset();
    return status_;
}

short DaemonControlOperation::desired_events() const noexcept {
    if (status_ != DaemonControlStatus::InProgress) return 0;
    if (phase_ == Phase::Connecting || phase_ == Phase::WriteHello ||
        phase_ == Phase::WriteControl || phase_ == Phase::WriteHandoff)
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
        frame_read_.resize(kFrameHeaderSize);
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
        frame_expected_ = kFrameHeaderSize + payload; frame_read_.resize(frame_expected_);
    }
    if (offset_ != frame_expected_) return false;
    Frame frame;
    if (decode_frame(frame_read_, frame) != Status::Ok ||
        frame.type != MessageType::HelloAck || frame.identity != operation_.identity ||
        frame.payload.size() != 1 || frame.payload[0] != static_cast<uint8_t>(PeerRole::Sidecar)) {
        fail(DaemonControlStatus::IdentityMismatch); return false;
    }
    offset_ = 0; frame_read_.clear(); frame_expected_ = 0; phase_ = Phase::WriteControl;
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
    ::close(transfer_fd_); transfer_fd_ = -1; status_ = DaemonControlStatus::Complete;
    close_fd(); phase_ = Phase::None; return true;
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
    if (now >= deadline_) { fail(DaemonControlStatus::Timeout); return status_; }
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) { fail(DaemonControlStatus::Disconnected); return status_; }
    size_t calls = 0, budget = limits_.bytes_per_turn;
    while (calls < limits_.syscalls_per_turn && budget != 0 && status_ == DaemonControlStatus::InProgress) {
        if (phase_ == Phase::Connecting) {
            int error = 0; socklen_t length = sizeof(error);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) != 0) { ++calls; fail(DaemonControlStatus::IoError); break; }
            ++calls;
            if (error != 0) { fail(DaemonControlStatus::Disconnected); break; }
            phase_ = Phase::WriteHello; continue;
        }
        if (phase_ == Phase::WriteHello) {
            if (!peer_queried_) {
                if (!query_peer()) { ++calls; fail(DaemonControlStatus::CredentialFailure); break; }
                ++calls;
            }
            if (!write_bytes(offset_, hello_, calls, budget)) break;
            offset_ = 0; phase_ = Phase::ReadHelloAck; continue;
        }
        if (phase_ == Phase::ReadHelloAck) { if (!read_frame(calls, budget)) break; continue; }
        if (phase_ == Phase::WriteControl) { if (!write_bytes(offset_, control_, calls, budget)) break; offset_ = 0; phase_ = Phase::WriteHandoff; continue; }
        if (phase_ == Phase::WriteHandoff) { if (!write_handoff(calls, budget)) break; offset_ = 0; phase_ = Phase::ReadAck; continue; }
        if (phase_ == Phase::ReadAck) { read_ack(calls, budget); break; }
        fail(DaemonControlStatus::IoError);
    }
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
    std::chrono::steady_clock::time_point deadline, DaemonControlLimits limits) noexcept {
    if (nonblocking_fd < 0 || !nonblocking(nonblocking_fd) || !valid_limits(limits) ||
        deadline <= std::chrono::steady_clock::now() || encode_control_operation(expected).empty())
        return status_ = DaemonControlStatus::InvalidArgument;
    close_all(); fd_ = nonblocking_fd; expected_ = expected; deadline_ = deadline;
    limits_ = limits; status_ = DaemonControlStatus::InProgress; offset_ = 0; ack_offset_ = 0;
    have_rights_ = false; fd_count_ = 0; calls_ = 0; return status_;
}

short DaemonControlHandoffReceiver::desired_events() const noexcept {
    return status_ == DaemonControlStatus::InProgress
               ? (accepted_fd_ >= 0 && offset_ == wire_.size() ? POLLOUT : POLLIN)
               : 0;
}

bool DaemonControlHandoffReceiver::validate_wire() noexcept {
    return std::equal(kMagic.begin(), kMagic.end(), wire_.begin()) &&
           get16(wire_.data() + 4) == kHandoffVersion && get16(wire_.data() + 6) == kRequest &&
           get32(wire_.data() + 8) == kHandoffBytes &&
           get64(wire_.data() + 12) == expected_.identity.generation &&
           get64(wire_.data() + 20) == expected_.identity.attempt &&
           get64(wire_.data() + 28) == expected_.request_id &&
           get32(wire_.data() + 36) == static_cast<uint32_t>(expected_.kind);
}

DaemonControlStatus DaemonControlHandoffReceiver::advance(
    std::chrono::steady_clock::time_point now, short revents) noexcept {
    if (status_ != DaemonControlStatus::InProgress) return status_;
    if (now >= deadline_) { fail(DaemonControlStatus::Timeout); return status_; }
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) { fail(DaemonControlStatus::Disconnected); return status_; }
    if (accepted_fd_ >= 0 && offset_ == wire_.size()) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
#ifdef MSG_DONTWAIT
        flags |= MSG_DONTWAIT;
#endif
        const auto ack = handoff_wire(kAck, expected_, kAckCode);
        const ssize_t sent = ::send(fd_, ack.data() + ack_offset_, ack.size() - ack_offset_, flags);
        if (sent > 0) { ack_offset_ += static_cast<size_t>(sent); if (ack_offset_ == ack.size()) status_ = DaemonControlStatus::Complete; }
        else if (sent < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) fail(DaemonControlStatus::Disconnected);
        return status_;
    }
    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int) * 4)> control{};
    iovec iov{wire_.data() + offset_, wire_.size() - offset_};
    msghdr message{}; message.msg_iov = &iov; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    int flags = MSG_DONTWAIT;
#ifdef MSG_CMSG_CLOEXEC
    flags |= MSG_CMSG_CLOEXEC;
#endif
    const ssize_t received = ::recvmsg(fd_, &message, flags);
    if (received > 0) offset_ += static_cast<size_t>(received);
    else if (received == 0) { fail(DaemonControlStatus::Truncated); return status_; }
    else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) { fail(DaemonControlStatus::IoError); return status_; }
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
    if ((message.msg_flags & MSG_CTRUNC) != 0) { fail(DaemonControlStatus::ControlTruncated); return status_; }
    if (fd_count_ > 1) { fail(DaemonControlStatus::ExtraFd); return status_; }
    if (offset_ != wire_.size()) return status_;
    if (!have_rights_) { fail(DaemonControlStatus::MissingFd); return status_; }
    if (!validate_wire()) { fail(DaemonControlStatus::OperationMismatch); return status_; }
    // The descriptor is now retained as an admitted value until the ACK is sent.
    ack_offset_ = 0; return status_;
}

int DaemonControlHandoffReceiver::take_fd() noexcept {
    if (status_ != DaemonControlStatus::Complete || accepted_fd_ < 0) return -1;
    const int result = accepted_fd_; accepted_fd_ = -1; return result;
}

void DaemonControlPollAdapter::add(DaemonControlOperation* operation) noexcept {
    if (operation != nullptr && std::find(operations_.begin(), operations_.end(), operation) == operations_.end())
        operations_.push_back(operation);
}

size_t DaemonControlPollAdapter::advance_ready(
    std::chrono::steady_clock::time_point now, const std::vector<short>& revents) noexcept {
    if (operations_.empty()) return 0;
    size_t advanced = 0;
    const size_t count = operations_.size();
    for (size_t i = 0; i != count; ++i) {
        const size_t index = (cursor_ + i) % count;
        if (index < revents.size() && operations_[index]->status() == DaemonControlStatus::InProgress) {
            operations_[index]->advance(now, revents[index]); ++advanced;
        }
    }
    cursor_ = (cursor_ + 1) % count;
    return advanced;
}

} // namespace icecc::p50::local
