#include "p50_fd_handoff.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace icecc::p50::local {
namespace {

constexpr size_t kWireSize = 40;
constexpr uint16_t kWireVersion = 1;
constexpr uint16_t kRequestType = 1;
constexpr uint16_t kAckType = 2;
constexpr uint16_t kNackType = 3;
constexpr std::array<uint8_t, 4> kMagic{'P', '5', '0', 'F'};
constexpr int kMaxEintrRetries = 8;
constexpr size_t kControlBufferSize = 256;

#if defined(ICECC_P50_FD_HANDOFF_TEST_HOOKS)
std::atomic<size_t> g_test_max_send_chunk{0};
#endif

enum : uint32_t {
    kCodeAck = 1,
    kCodeNack = 2,
    kCodeStaleGeneration = 3,
    kCodeIdentityMismatch = 4,
    kCodeRequestMismatch = 5,
    kCodeDuplicate = 6,
    kCodeMalformed = 7,
};

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

bool valid_request(const HandoffRequest& request) {
    return request.identity.generation != 0 && request.identity.attempt != 0 &&
           request.request_id != 0;
}

std::array<uint8_t, kWireSize> encode_wire(uint16_t type, const HandoffRequest& request,
                                           uint32_t code) {
    std::array<uint8_t, kWireSize> wire{};
    std::copy(kMagic.begin(), kMagic.end(), wire.begin());
    put_u16(wire.data() + 4, kWireVersion);
    put_u16(wire.data() + 6, type);
    put_u32(wire.data() + 8, kWireSize);
    put_u64(wire.data() + 12, request.identity.generation);
    put_u64(wire.data() + 20, request.identity.attempt);
    put_u64(wire.data() + 28, request.request_id);
    put_u32(wire.data() + 36, code);
    return wire;
}

bool decode_wire(std::span<const uint8_t> wire, uint16_t expected_type,
                 HandoffRequest& request, uint32_t& code) {
    if (wire.size() != kWireSize || !std::equal(kMagic.begin(), kMagic.end(), wire.begin()) ||
        get_u16(wire.data() + 4) != kWireVersion ||
        get_u16(wire.data() + 6) != expected_type || get_u32(wire.data() + 8) != kWireSize)
        return false;
    request.identity.generation = get_u64(wire.data() + 12);
    request.identity.attempt = get_u64(wire.data() + 20);
    request.request_id = get_u64(wire.data() + 28);
    code = get_u32(wire.data() + 36);
    return valid_request(request) && (expected_type != kRequestType || code == 0);
}

int poll_until(int fd, short events, std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return 0;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        const long long ms = (us + 999) / 1000;
        const int timeout = static_cast<int>(std::min<long long>(ms, 2147483647LL));
        struct pollfd pfd{fd, static_cast<short>(events | POLLERR | POLLHUP), 0};
        const int result = ::poll(&pfd, 1, timeout);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
            return result;
        // A stream may report readable data and HUP together.  Consume the
        // queued bytes first; the following iteration observes clean EOF.
        if ((pfd.revents & events) != 0)
            return 1;
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return -1;
    }
}

FdHandoffStatus io_status(int poll_result) {
    return poll_result < 0 ? FdHandoffStatus::Disconnected : FdHandoffStatus::Timeout;
}

struct Received {
    FdHandoffStatus status = FdHandoffStatus::IoError;
    std::array<uint8_t, kWireSize> wire{};
    int fd = -1;
    size_t fd_count = 0;
    bool has_rights = false;
};

bool read_descriptor_flags(int fd, int& descriptor_flags) noexcept {
    for (int attempt = 0; attempt != kMaxEintrRetries; ++attempt) {
        descriptor_flags = ::fcntl(fd, F_GETFD);
        if (descriptor_flags >= 0)
            return true;
        if (errno != EINTR)
            return false;
    }
    return false;
}

bool mark_cloexec(int fd) noexcept {
    int descriptor_flags = -1;
    if (!read_descriptor_flags(fd, descriptor_flags))
        return false;
    if ((descriptor_flags & FD_CLOEXEC) != 0)
        return true;
    for (int attempt = 0; attempt != kMaxEintrRetries; ++attempt) {
        if (::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) == 0)
            return true;
        if (errno != EINTR)
            return false;
    }
    return false;
}

FdHandoffStatus reject_queued_trailing_byte(int fd) noexcept {
    struct pollfd pfd{fd, POLLIN, 0};
    int ready = -1;
    for (int attempt = 0; attempt != kMaxEintrRetries; ++attempt) {
        ready = ::poll(&pfd, 1, 0);
        if (ready >= 0 || errno != EINTR)
            break;
    }
    if (ready < 0)
        return FdHandoffStatus::IoError;
    if (ready == 0 || (pfd.revents & POLLIN) == 0)
        return FdHandoffStatus::Accepted;

    uint8_t byte = 0;
    int flags = MSG_PEEK;
#if defined(MSG_DONTWAIT)
    flags |= MSG_DONTWAIT;
#endif
    for (int attempt = 0; attempt != kMaxEintrRetries; ++attempt) {
        const ssize_t count = ::recv(fd, &byte, sizeof(byte), flags);
        if (count > 0)
            return FdHandoffStatus::TrailingData;
        if (count == 0)
            return FdHandoffStatus::Accepted;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return FdHandoffStatus::Accepted;
        if (errno != EINTR)
            return FdHandoffStatus::IoError;
    }
    return FdHandoffStatus::IoError;
}

void close_received(Received& received) {
    if (received.fd >= 0) {
        ::close(received.fd);
        received.fd = -1;
    }
}

void record_control(Received& received, struct msghdr& message) noexcept {
    const auto fail = [&received](FdHandoffStatus status) {
        if (received.status == FdHandoffStatus::Accepted)
            received.status = status;
    };
    if ((message.msg_flags & MSG_TRUNC) != 0)
        fail(FdHandoffStatus::MessageTruncated);
    if ((message.msg_flags & MSG_CTRUNC) != 0)
        fail(FdHandoffStatus::ControlTruncated);

    const auto* control_begin = static_cast<const uint8_t*>(message.msg_control);
    const auto* control_end = control_begin + message.msg_controllen;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        const auto* cmsg_begin = reinterpret_cast<const uint8_t*>(cmsg);
        const auto* data_begin = reinterpret_cast<const uint8_t*>(CMSG_DATA(cmsg));
        const bool header_in_bounds = cmsg_begin >= control_begin && cmsg_begin <= control_end &&
                                      static_cast<size_t>(control_end - cmsg_begin) >=
                                          sizeof(struct cmsghdr);
        const bool length_in_bounds = header_in_bounds && cmsg->cmsg_len >= CMSG_LEN(0) &&
                                      cmsg->cmsg_len <=
                                          static_cast<size_t>(control_end - cmsg_begin);
        const size_t available = data_begin >= control_begin && data_begin <= control_end
                                     ? static_cast<size_t>(control_end - data_begin)
                                     : 0;
        if (!length_in_bounds) {
            if (header_in_bounds && cmsg->cmsg_level == SOL_SOCKET &&
                cmsg->cmsg_type == SCM_RIGHTS) {
                const size_t possible_fds = available / sizeof(int);
                const auto* fds = reinterpret_cast<const int*>(data_begin);
                for (size_t i = 0; i != possible_fds; ++i)
                    ::close(fds[i]);
                received.fd_count += possible_fds;
            }
            fail(FdHandoffStatus::Malformed);
            continue;
        }
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            fail(FdHandoffStatus::UnexpectedControl);
            continue;
        }
        received.has_rights = true;
        const size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes == 0 || bytes % sizeof(int) != 0 || bytes > available) {
            const auto* fds = reinterpret_cast<const int*>(data_begin);
            const size_t possible_fds = std::min(bytes / sizeof(int), available / sizeof(int));
            for (size_t i = 0; i != possible_fds; ++i)
                ::close(fds[i]);
            received.fd_count += possible_fds;
            fail(FdHandoffStatus::Malformed);
            continue;
        }
        const size_t count_fds = bytes / sizeof(int);
        const auto* fds = reinterpret_cast<const int*>(data_begin);
        for (size_t i = 0; i != count_fds; ++i) {
            if (received.fd_count == 0)
                received.fd = fds[i];
            else
                ::close(fds[i]);
            ++received.fd_count;
        }
        if (count_fds != 1)
            fail(FdHandoffStatus::ExtraFd);
    }
}

Received receive_wire(int connection_fd, bool expect_fd,
                      std::chrono::steady_clock::time_point deadline) {
    Received received;
    received.status = FdHandoffStatus::Accepted;
    size_t offset = 0;
    while (offset != received.wire.size() && received.status == FdHandoffStatus::Accepted) {
        const int ready = poll_until(connection_fd, POLLIN, deadline);
        if (ready != 1) {
            if (ready == 0)
                received.status = FdHandoffStatus::Timeout;
            else
                received.status = offset == 0 ? FdHandoffStatus::Disconnected
                                              : FdHandoffStatus::Truncated;
            break;
        }

        // Leave enough room to classify non-SCM_RIGHTS ancillary records rather
        // than reporting a platform-dependent MSG_CTRUNC for otherwise bounded
        // credentials/control payloads.  SCM_RIGHTS is still limited below to
        // exactly one descriptor and every received extra descriptor is closed.
        alignas(struct cmsghdr) std::array<uint8_t, kControlBufferSize> control{};
        struct iovec iov{received.wire.data() + offset, received.wire.size() - offset};
        struct msghdr message{};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        int flags = 0;
#if defined(MSG_CMSG_CLOEXEC)
        flags |= MSG_CMSG_CLOEXEC;
#endif
#if defined(MSG_DONTWAIT)
        flags |= MSG_DONTWAIT;
#endif
        ssize_t count = ::recvmsg(connection_fd, &message, flags);
#if defined(MSG_CMSG_CLOEXEC)
        if (count < 0 && errno == EINVAL) {
            message.msg_controllen = control.size();
            message.msg_flags = 0;
            count = ::recvmsg(connection_fd, &message,
#if defined(MSG_DONTWAIT)
                              MSG_DONTWAIT
#else
                              0
#endif
            );
        }
#endif
        if (count == 0) {
            received.status = offset == 0 ? FdHandoffStatus::Disconnected
                                          : FdHandoffStatus::Truncated;
            break;
        }
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            received.status = FdHandoffStatus::IoError;
            break;
        }

        record_control(received, message);
        const size_t remaining = received.wire.size() - offset;
        if (static_cast<size_t>(count) > remaining) {
            offset = received.wire.size();
            if (received.status == FdHandoffStatus::Accepted)
                received.status = FdHandoffStatus::MessageTruncated;
            break;
        }
        offset += static_cast<size_t>(count);
    }

    if (received.status == FdHandoffStatus::Accepted && offset == received.wire.size())
        received.status = reject_queued_trailing_byte(connection_fd);
    if (received.status == FdHandoffStatus::Accepted && expect_fd && !received.has_rights)
        received.status = FdHandoffStatus::MissingFd;
    if (received.status == FdHandoffStatus::Accepted && !expect_fd && received.has_rights)
        received.status = FdHandoffStatus::UnexpectedControl;
    if (received.status == FdHandoffStatus::Accepted && received.fd_count == 0 &&
        received.has_rights)
        received.status = FdHandoffStatus::Malformed;
    if (received.status == FdHandoffStatus::Accepted && received.fd_count > 1)
        received.status = FdHandoffStatus::ExtraFd;
    if (received.status == FdHandoffStatus::Accepted && received.fd >= 0 &&
        !mark_cloexec(received.fd))
        received.status = FdHandoffStatus::AdoptionFailed;
    if (received.status != FdHandoffStatus::Accepted)
        close_received(received);
    return received;
}

FdHandoffStatus response_status(uint16_t type, uint32_t code) {
    if (type == kAckType && code == kCodeAck)
        return FdHandoffStatus::Accepted;
    if (type != kNackType)
        return FdHandoffStatus::AckMismatch;
    switch (code) {
    case kCodeNack: return FdHandoffStatus::Nack;
    case kCodeStaleGeneration: return FdHandoffStatus::StaleGeneration;
    case kCodeIdentityMismatch: return FdHandoffStatus::IdentityMismatch;
    case kCodeRequestMismatch: return FdHandoffStatus::RequestMismatch;
    case kCodeDuplicate: return FdHandoffStatus::DuplicateRequest;
    case kCodeMalformed: return FdHandoffStatus::Malformed;
    default: return FdHandoffStatus::Nack;
    }
}

FdHandoffStatus send_wire(int connection_fd, const std::array<uint8_t, kWireSize>& wire,
                          int fd, std::chrono::steady_clock::time_point deadline) {
    size_t offset = 0;
    bool rights_sent = false;
    while (offset != wire.size()) {
        const int ready = poll_until(connection_fd, POLLOUT, deadline);
        if (ready != 1)
            return io_status(ready);
        size_t chunk = wire.size() - offset;
#if defined(ICECC_P50_FD_HANDOFF_TEST_HOOKS)
        const size_t test_chunk = g_test_max_send_chunk.load(std::memory_order_relaxed);
        if (test_chunk != 0)
            chunk = std::min(chunk, test_chunk);
#endif
        struct iovec iov{const_cast<uint8_t*>(wire.data() + offset), chunk};
        alignas(struct cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int))> control{};
        struct msghdr message{};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        const bool attach_rights = fd >= 0 && !rights_sent;
        if (attach_rights) {
            message.msg_control = control.data();
            message.msg_controllen = control.size();
            auto* cmsg = reinterpret_cast<struct cmsghdr*>(control.data());
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
        }
        int flags = 0;
#if defined(MSG_NOSIGNAL)
        flags |= MSG_NOSIGNAL;
#endif
#if defined(MSG_DONTWAIT)
        flags |= MSG_DONTWAIT;
#endif
        const ssize_t count = ::sendmsg(connection_fd, &message, flags);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count > 0) {
            if (static_cast<size_t>(count) > chunk)
                return FdHandoffStatus::IoError;
            if (attach_rights)
                rights_sent = true;
            offset += static_cast<size_t>(count);
            continue;
        }
        if (count == 0)
            return FdHandoffStatus::Disconnected;
        return errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN
                   ? FdHandoffStatus::Disconnected
                   : FdHandoffStatus::IoError;
    }
    return FdHandoffStatus::Accepted;
}

FdHandoffResult result(FdHandoffStatus status, FdHandoffSenderState state) {
    return FdHandoffResult{status, state};
}

FdHandoffStatus rejection_for(const HandoffRequest& actual, const HandoffRequest& expected) {
    if (actual.identity.generation != expected.identity.generation)
        return FdHandoffStatus::StaleGeneration;
    if (actual.identity.attempt != expected.identity.attempt)
        return FdHandoffStatus::IdentityMismatch;
    if (actual.request_id != expected.request_id)
        return FdHandoffStatus::RequestMismatch;
    return FdHandoffStatus::Nack;
}

uint32_t nack_code(FdHandoffStatus status) {
    switch (status) {
    case FdHandoffStatus::StaleGeneration: return kCodeStaleGeneration;
    case FdHandoffStatus::IdentityMismatch: return kCodeIdentityMismatch;
    case FdHandoffStatus::RequestMismatch: return kCodeRequestMismatch;
    case FdHandoffStatus::DuplicateRequest: return kCodeDuplicate;
    case FdHandoffStatus::Malformed: return kCodeMalformed;
    default: return kCodeNack;
    }
}

} // namespace

HandoffFd::HandoffFd(int fd) noexcept : fd_(fd) {
    if (fd_ >= 0 && !mark_cloexec(fd_)) {
        ::close(fd_);
        fd_ = -1;
    }
}

HandoffFd::~HandoffFd() { reset(); }

HandoffFd::HandoffFd(HandoffFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

HandoffFd& HandoffFd::operator=(HandoffFd&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

bool HandoffFd::cloexec() const noexcept {
    int flags = -1;
    if (fd_ < 0 || !read_descriptor_flags(fd_, flags))
        return false;
    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

int HandoffFd::release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void HandoffFd::reset() noexcept {
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
}

const char* fd_handoff_status_name(FdHandoffStatus status) noexcept {
    switch (status) {
    case FdHandoffStatus::Accepted: return "accepted";
    case FdHandoffStatus::Nack: return "nack";
    case FdHandoffStatus::InvalidArgument: return "invalid-argument";
    case FdHandoffStatus::NotAuthenticated: return "not-authenticated";
    case FdHandoffStatus::AlreadySent: return "already-sent";
    case FdHandoffStatus::AlreadyConsumed: return "already-consumed";
    case FdHandoffStatus::Timeout: return "timeout";
    case FdHandoffStatus::Disconnected: return "disconnected";
    case FdHandoffStatus::IoError: return "io-error";
    case FdHandoffStatus::Malformed: return "malformed";
    case FdHandoffStatus::Truncated: return "truncated";
    case FdHandoffStatus::MessageTruncated: return "message-truncated";
    case FdHandoffStatus::ControlTruncated: return "control-truncated";
    case FdHandoffStatus::TrailingData: return "trailing-data";
    case FdHandoffStatus::MissingFd: return "missing-fd";
    case FdHandoffStatus::ExtraFd: return "extra-fd";
    case FdHandoffStatus::UnexpectedControl: return "unexpected-control";
    case FdHandoffStatus::StaleGeneration: return "stale-generation";
    case FdHandoffStatus::IdentityMismatch: return "identity-mismatch";
    case FdHandoffStatus::RequestMismatch: return "request-mismatch";
    case FdHandoffStatus::DuplicateRequest: return "duplicate-request";
    case FdHandoffStatus::AckMismatch: return "ack-mismatch";
    case FdHandoffStatus::AdoptionFailed: return "adoption-failed";
    }
    return "unknown";
}

FdHandoffSender::FdHandoffSender(HandoffFd fd) noexcept : fd_(std::move(fd)) {}

FdHandoffResult FdHandoffSender::send(Connection& connection, const HandoffRequest& request,
                                      std::chrono::steady_clock::time_point deadline) noexcept {
    if (state_ != FdHandoffSenderState::Prepared)
        return result(FdHandoffStatus::AlreadySent, state_);
    if (!fd_.valid() || !valid_request(request) || !connection.valid()) {
        fd_.reset();
        state_ = FdHandoffSenderState::Rejected;
        return result(FdHandoffStatus::InvalidArgument, state_);
    }
    if (!connection.peer_credentials_verified()) {
        fd_.reset();
        state_ = FdHandoffSenderState::Rejected;
        return result(FdHandoffStatus::NotAuthenticated, state_);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        fd_.reset();
        state_ = FdHandoffSenderState::TimedOut;
        return result(FdHandoffStatus::Timeout, state_);
    }
    const auto wire = encode_wire(kRequestType, request, 0);
    const FdHandoffStatus send_status = send_wire(connection.fd_, wire, fd_.get(), deadline);
    if (send_status != FdHandoffStatus::Accepted) {
        fd_.reset();
        state_ = send_status == FdHandoffStatus::Timeout ? FdHandoffSenderState::TimedOut
                                                          : FdHandoffSenderState::Disconnected;
        return result(send_status, state_);
    }
    state_ = FdHandoffSenderState::Sent;
    const Received response = receive_wire(connection.fd_, false, deadline);
    if (response.fd >= 0)
        ::close(response.fd);
    if (response.status != FdHandoffStatus::Accepted) {
        fd_.reset();
        state_ = response.status == FdHandoffStatus::Timeout
                      ? FdHandoffSenderState::TimedOut
                      : response.status == FdHandoffStatus::Disconnected
                            ? FdHandoffSenderState::Disconnected
                            : FdHandoffSenderState::Rejected;
        return result(response.status, state_);
    }
    HandoffRequest echoed{};
    uint32_t code = 0;
    const uint16_t type = get_u16(response.wire.data() + 6);
    if (!decode_wire(response.wire, type, echoed, code) || echoed != request) {
        fd_.reset();
        state_ = FdHandoffSenderState::Rejected;
        return result(echoed.identity.generation != request.identity.generation
                          ? FdHandoffStatus::StaleGeneration
                          : FdHandoffStatus::AckMismatch,
                      state_);
    }
    const FdHandoffStatus status = response_status(type, code);
    fd_.reset();
    if (status == FdHandoffStatus::Accepted) {
        state_ = FdHandoffSenderState::Acked;
        return result(status, state_);
    }
    state_ = FdHandoffSenderState::Nacked;
    return result(status, state_);
}

FdHandoffResult FdHandoffReceiver::receive_and_ack(
    Connection& connection, const HandoffRequest& expected,
    std::chrono::steady_clock::time_point deadline) noexcept {
    if (!connection.valid() || !valid_request(expected))
        return result(FdHandoffStatus::InvalidArgument, FdHandoffSenderState::Rejected);
    if (!connection.peer_credentials_verified())
        return result(FdHandoffStatus::NotAuthenticated, FdHandoffSenderState::Rejected);
    Received received = receive_wire(connection.fd_, true, deadline);
    if (received.status != FdHandoffStatus::Accepted) {
        close_received(received);
        return result(received.status, FdHandoffSenderState::Rejected);
    }
    HandoffRequest actual{};
    uint32_t code = 0;
    if (!decode_wire(received.wire, kRequestType, actual, code)) {
        close_received(received);
        return result(FdHandoffStatus::Malformed, FdHandoffSenderState::Rejected);
    }
    FdHandoffStatus status = FdHandoffStatus::Accepted;
    if (consumed_)
        status = FdHandoffStatus::AlreadyConsumed;
    else if (seen_request_.has_value() && seen_request_.value() == actual)
        status = FdHandoffStatus::DuplicateRequest;
    else if (actual != expected)
        status = rejection_for(actual, expected);
    if (status != FdHandoffStatus::Accepted) {
        close_received(received);
        seen_request_ = actual;
        const auto nack = encode_wire(kNackType, actual, nack_code(status));
        const FdHandoffStatus nack_status = send_wire(connection.fd_, nack, -1, deadline);
        if (nack_status != FdHandoffStatus::Accepted)
            return result(nack_status, nack_status == FdHandoffStatus::Timeout
                                           ? FdHandoffSenderState::TimedOut
                                           : FdHandoffSenderState::Disconnected);
        return result(status, FdHandoffSenderState::Nacked);
    }
    HandoffFd received_fd(received.fd);
    received.fd = -1;
    if (!received_fd.valid() || !received_fd.cloexec())
        return result(FdHandoffStatus::AdoptionFailed, FdHandoffSenderState::Rejected);
    adopted_ = std::move(received_fd);
    consumed_ = true;
    seen_request_ = actual;
    const auto ack = encode_wire(kAckType, actual, kCodeAck);
    const FdHandoffStatus ack_status = send_wire(connection.fd_, ack, -1, deadline);
    if (ack_status != FdHandoffStatus::Accepted)
        return result(ack_status, ack_status == FdHandoffStatus::Timeout
                                     ? FdHandoffSenderState::TimedOut
                                     : FdHandoffSenderState::Disconnected);
    return result(FdHandoffStatus::Accepted, FdHandoffSenderState::Acked);
}

HandoffFd FdHandoffReceiver::take_adopted_fd() noexcept { return std::move(adopted_); }

#if defined(ICECC_P50_FD_HANDOFF_TEST_HOOKS)
void fd_handoff_test_set_max_send_chunk(size_t bytes) noexcept {
    g_test_max_send_chunk.store(bytes, std::memory_order_relaxed);
}
#endif

} // namespace icecc::p50::local
