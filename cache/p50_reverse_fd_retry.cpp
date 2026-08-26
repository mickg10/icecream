#include "p50_reverse_fd_retry.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

namespace icecc::p50 {
namespace {

constexpr std::array<uint8_t, 4> kMagic{'P', '5', '0', 'R'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 52;
constexpr size_t kMaxWireSize = 64u * 1024u;
constexpr uint32_t kAck = 1;
constexpr uint32_t kExactReplay = 2;
constexpr uint32_t kNack = 3;
constexpr uint32_t kStale = 4;
constexpr uint32_t kConflict = 5;
constexpr uint32_t kWrongArm = 6;
constexpr uint32_t kExpired = 7;
constexpr uint32_t kCancelled = 8;
constexpr uint32_t kPhaseViolation = 9;
constexpr uint32_t kMalformed = 10;

void put_u16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}
void put_u32(uint8_t* p, uint32_t value) {
    p[0] = static_cast<uint8_t>(value >> 24);
    p[1] = static_cast<uint8_t>(value >> 16);
    p[2] = static_cast<uint8_t>(value >> 8);
    p[3] = static_cast<uint8_t>(value);
}
void put_u64(uint8_t* p, uint64_t value) {
    for (size_t i = 0; i != 8; ++i)
        p[i] = static_cast<uint8_t>(value >> (56 - i * 8));
}
uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) << 8 | p[1];
}
uint32_t get_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
}
uint64_t get_u64(const uint8_t* p) {
    uint64_t result = 0;
    for (size_t i = 0; i != 8; ++i)
        result = (result << 8) | p[i];
    return result;
}

std::vector<uint8_t> encode_wire(ReverseFdWireType type,
                                 const ReverseFdAttempt& attempt,
                                 uint32_t code) {
    const auto arm = type == ReverseFdWireType::Attempt
                         ? encode_source_arm(attempt.arm)
                         : std::vector<uint8_t>{};
    if (arm.empty() && type == ReverseFdWireType::Attempt)
        return {};
    if (arm.size() > kMaxWireSize - kHeaderSize)
        return {};
    std::vector<uint8_t> wire(kHeaderSize + arm.size());
    std::copy(kMagic.begin(), kMagic.end(), wire.begin());
    put_u16(wire.data() + 4, kVersion);
    put_u16(wire.data() + 6, static_cast<uint16_t>(type));
    put_u32(wire.data() + 8, static_cast<uint32_t>(wire.size()));
    put_u64(wire.data() + 12, attempt.identity.generation);
    put_u64(wire.data() + 20, attempt.identity.attempt);
    put_u64(wire.data() + 28, attempt.delivery_id);
    put_u64(wire.data() + 36, attempt.token);
    put_u32(wire.data() + 44, type == ReverseFdWireType::Attempt
                                  ? attempt.remaining_ms
                                  : code);
    put_u32(wire.data() + 48, static_cast<uint32_t>(arm.size()));
    std::copy(arm.begin(), arm.end(), wire.begin() + kHeaderSize);
    return wire;
}

bool decode_header(std::span<const uint8_t> wire, ReverseFdWireType& type,
                   ReverseFdAttempt& attempt, uint32_t& code, uint32_t& arm_size) {
    if (wire.size() < kHeaderSize || !std::equal(kMagic.begin(), kMagic.end(), wire.begin()) ||
        get_u16(wire.data() + 4) != kVersion)
        return false;
    const uint16_t raw_type = get_u16(wire.data() + 6);
    if (raw_type < static_cast<uint16_t>(ReverseFdWireType::Attempt) ||
        raw_type > static_cast<uint16_t>(ReverseFdWireType::Nack) ||
        get_u32(wire.data() + 8) != wire.size())
        return false;
    type = static_cast<ReverseFdWireType>(raw_type);
    attempt.identity.generation = get_u64(wire.data() + 12);
    attempt.identity.attempt = get_u64(wire.data() + 20);
    attempt.delivery_id = get_u64(wire.data() + 28);
    attempt.token = get_u64(wire.data() + 36);
    code = get_u32(wire.data() + 44);
    arm_size = get_u32(wire.data() + 48);
    return attempt.identity.generation != 0 && attempt.identity.attempt != 0 &&
           attempt.delivery_id != 0 && attempt.token != 0 &&
           arm_size <= wire.size() - kHeaderSize;
}

int poll_fd(int fd, short events,
            std::chrono::steady_clock::time_point deadline) noexcept {
    const auto result = local::detail::wait_for_io(fd, events, deadline);
    return result == local::detail::DeadlinePollResult::Ready
               ? 1
               : result == local::detail::DeadlinePollResult::Timeout ? 0 : -1;
}

bool send_bytes(int fd, std::span<const uint8_t> bytes, int attached_fd,
                std::chrono::steady_clock::time_point deadline) noexcept {
#if !defined(MSG_DONTWAIT)
    (void)fd; (void)bytes; (void)attached_fd; (void)deadline;
    return false;
#else
    size_t offset = 0;
    bool rights_sent = attached_fd < 0;
    while (offset != bytes.size()) {
        if (poll_fd(fd, POLLOUT, deadline) != 1)
            return false;
        const size_t chunk = bytes.size() - offset;
        struct iovec iov{const_cast<uint8_t*>(bytes.data() + offset), chunk};
        alignas(struct cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int))> control{};
        struct msghdr message{};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        if (!rights_sent) {
            message.msg_control = control.data();
            message.msg_controllen = control.size();
            auto* cmsg = reinterpret_cast<struct cmsghdr*>(control.data());
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(cmsg), &attached_fd, sizeof(attached_fd));
        }
        int flags = MSG_DONTWAIT;
#if defined(MSG_NOSIGNAL)
        flags |= MSG_NOSIGNAL;
#endif
        const ssize_t count = ::sendmsg(fd, &message, flags);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count <= 0)
            return false;
        if (!rights_sent)
            rights_sent = true;
        offset += static_cast<size_t>(count);
    }
    return std::chrono::steady_clock::now() < deadline;
#endif
}

bool read_plain(int fd, std::vector<uint8_t>& bytes,
                std::chrono::steady_clock::time_point deadline) noexcept {
#if !defined(MSG_DONTWAIT)
    (void)fd; (void)bytes; (void)deadline;
    return false;
#else
    size_t offset = 0;
    while (offset != bytes.size()) {
        if (poll_fd(fd, POLLIN, deadline) != 1)
            return false;
        const ssize_t count = ::recv(fd, bytes.data() + offset, bytes.size() - offset,
                                     MSG_DONTWAIT);
        if (count > 0) { offset += static_cast<size_t>(count); continue; }
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return false;
    }
    return true;
#endif
}

struct ReceivedWire {
    std::vector<uint8_t> bytes;
    int fd = -1;
    bool malformed = false;
};

ReceivedWire receive_wire(int fd, std::chrono::steady_clock::time_point deadline) noexcept {
#if !defined(MSG_DONTWAIT)
    (void)fd; (void)deadline;
    return {};
#else
    ReceivedWire result;
    std::vector<uint8_t> buffer(kMaxWireSize);
    size_t used = 0;
    size_t expected = 0;
    bool first = true;
    while (expected == 0 || used < expected) {
        if (poll_fd(fd, POLLIN, deadline) != 1) {
            result.malformed = true;
            break;
        }
        struct iovec iov{buffer.data() + used, buffer.size() - used};
        alignas(struct cmsghdr) std::array<uint8_t, CMSG_SPACE(sizeof(int) * 2)> control{};
        struct msghdr message{};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        if (first) {
            message.msg_control = control.data();
            message.msg_controllen = control.size();
        }
        const ssize_t count = ::recvmsg(fd, &message, MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count <= 0) { result.malformed = true; break; }
        if (first) {
            for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr;
                 cmsg = CMSG_NXTHDR(&message, cmsg)) {
                if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
                    cmsg->cmsg_len < CMSG_LEN(sizeof(int)) ||
                    (cmsg->cmsg_len - CMSG_LEN(0)) % sizeof(int) != 0) {
                    result.malformed = true;
                    continue;
                }
                const size_t count_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const auto* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
                for (size_t i = 0; i != count_fds; ++i) {
                    if (result.fd < 0 && i == 0)
                        result.fd = fds[i];
                    else
                        ::close(fds[i]);
                }
                if (count_fds != 1)
                    result.malformed = true;
            }
            if ((message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0)
                result.malformed = true;
        }
        first = false;
        used += static_cast<size_t>(count);
        if (used >= 12 && expected == 0) {
            if (!std::equal(kMagic.begin(), kMagic.end(), buffer.begin()) ||
                get_u32(buffer.data() + 8) < kHeaderSize ||
                get_u32(buffer.data() + 8) > kMaxWireSize) {
                result.malformed = true;
                break;
            }
            expected = get_u32(buffer.data() + 8);
            if (used > expected) { result.malformed = true; break; }
        }
        if (used == buffer.size() && used < expected) { result.malformed = true; break; }
    }
    if (result.fd >= 0 && result.malformed) {
        ::close(result.fd);
        result.fd = -1;
    }
    result.bytes.assign(buffer.begin(), buffer.begin() + used);
    return result;
#endif
}

ReverseFdDecision decision_from_code(uint32_t code) noexcept {
    switch (code) {
    case kAck: return ReverseFdDecision::Accepted;
    case kExactReplay: return ReverseFdDecision::ExactReplay;
    case kStale: return ReverseFdDecision::Stale;
    case kConflict: return ReverseFdDecision::Conflict;
    case kWrongArm: return ReverseFdDecision::WrongArm;
    case kExpired: return ReverseFdDecision::Expired;
    case kCancelled: return ReverseFdDecision::Cancelled;
    case kPhaseViolation: return ReverseFdDecision::PhaseViolation;
    default: return ReverseFdDecision::Malformed;
    }
}

uint32_t code_for_decision(ReverseFdDecision decision) noexcept {
    switch (decision) {
    case ReverseFdDecision::Accepted: return kAck;
    case ReverseFdDecision::ExactReplay: return kExactReplay;
    case ReverseFdDecision::Stale: return kStale;
    case ReverseFdDecision::Conflict: return kConflict;
    case ReverseFdDecision::WrongArm: return kWrongArm;
    case ReverseFdDecision::Expired: return kExpired;
    case ReverseFdDecision::Cancelled: return kCancelled;
    case ReverseFdDecision::PhaseViolation: return kPhaseViolation;
    default: return kNack;
    }
}

int make_sealed_memfd(std::span<const uint8_t> bytes) noexcept {
#if defined(__linux__) && defined(SYS_memfd_create) && defined(MFD_CLOEXEC) && \
    defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS) && defined(F_GET_SEALS) && \
    defined(F_SEAL_SEAL) && defined(F_SEAL_SHRINK) && defined(F_SEAL_GROW) && \
    defined(F_SEAL_WRITE)
    const int fd = static_cast<int>(::syscall(
        SYS_memfd_create, "icecc-p50-reverse-input", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0)
        return -1;
    size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count > 0) { offset += static_cast<size_t>(count); continue; }
        if (count < 0 && errno == EINTR) continue;
        ::close(fd); return -1;
    }
    const int seals = F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    if (::lseek(fd, 0, SEEK_SET) != 0 || ::fcntl(fd, F_ADD_SEALS, seals) < 0 ||
        ::fcntl(fd, F_GET_SEALS) < 0 || (::fcntl(fd, F_GET_SEALS) & seals) != seals) {
        ::close(fd); return -1;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || info.st_size < 0 ||
        static_cast<size_t>(info.st_size) != bytes.size()) {
        ::close(fd); return -1;
    }
    return fd;
#else
    (void)bytes;
    return -1;
#endif
}

}  // namespace

bool ReverseFdAttempt::valid() const noexcept {
    return identity.generation != 0 && identity.attempt != 0 && delivery_id != 0 &&
           token != 0 && remaining_ms <= kMaxReverseFdRemainingMs &&
           arm.valid();
}

const char* reverse_fd_decision_name(ReverseFdDecision value) noexcept {
    switch (value) {
    case ReverseFdDecision::Invalid: return "invalid";
    case ReverseFdDecision::Accepted: return "accepted";
    case ReverseFdDecision::ExactReplay: return "exact-replay";
    case ReverseFdDecision::Stale: return "stale";
    case ReverseFdDecision::Conflict: return "conflict";
    case ReverseFdDecision::WrongArm: return "wrong-arm";
    case ReverseFdDecision::WrongIdentity: return "wrong-identity";
    case ReverseFdDecision::Expired: return "expired";
    case ReverseFdDecision::Cancelled: return "cancelled";
    case ReverseFdDecision::PhaseViolation: return "phase-violation";
    case ReverseFdDecision::Malformed: return "malformed";
    case ReverseFdDecision::IoError: return "io-error";
    }
    return "unknown";
}

std::vector<uint8_t> encode_reverse_fd_attempt(const ReverseFdAttempt& attempt) {
    if (!attempt.valid())
        return {};
    return encode_wire(ReverseFdWireType::Attempt, attempt, 0);
}

std::optional<ReverseFdAttempt> decode_reverse_fd_attempt(std::span<const uint8_t> wire) {
    ReverseFdWireType type{};
    ReverseFdAttempt result;
    uint32_t code = 0;
    uint32_t arm_size = 0;
    if (!decode_header(wire, type, result, code, arm_size) ||
        type != ReverseFdWireType::Attempt || code == 0 || arm_size == 0 ||
        arm_size != wire.size() - kHeaderSize || code > kMaxReverseFdRemainingMs)
        return std::nullopt;
    result.remaining_ms = code;
    const auto arm = decode_source_arm(wire.subspan(kHeaderSize, arm_size));
    if (!arm.has_value())
        return std::nullopt;
    result.arm = *arm;
    return result.valid() ? std::optional<ReverseFdAttempt>(result) : std::nullopt;
}

ReverseFdOwner::ReverseFdOwner(ReverseFdMaster master, ReverseFdAttempt identity,
                               TimePoint original_deadline) noexcept
    : master_(std::move(master)), identity_(std::move(identity)),
      deadline_(original_deadline) {}

std::optional<ReverseFdOwner> ReverseFdOwner::stage(
    std::span<const uint8_t> bytes, ReverseFdAttempt identity,
    TimePoint original_deadline) noexcept {
    if (bytes.empty() || !identity.valid() || original_deadline <= Clock::now() ||
        original_deadline - Clock::now() > std::chrono::milliseconds(kMaxReverseFdRemainingMs))
        return std::nullopt;
    const int fd = make_sealed_memfd(bytes);
    if (fd < 0)
        return std::nullopt;
    ReverseFdMaster master{local::HandoffFd(fd)};
    if (!master.valid() || !master.cloexec())
        return std::nullopt;
    return ReverseFdOwner(std::move(master), std::move(identity), original_deadline);
}

int ReverseFdOwner::duplicate_master() const noexcept {
    if (!master_.valid())
        return -1;
#if defined(F_DUPFD_CLOEXEC)
    int duplicate = ::fcntl(master_.get(), F_DUPFD_CLOEXEC, 0);
#else
    int duplicate = ::dup(master_.get());
    if (duplicate >= 0) {
        const int flags = ::fcntl(duplicate, F_GETFD);
        if (flags < 0 || ::fcntl(duplicate, F_SETFD, flags | FD_CLOEXEC) < 0) {
            ::close(duplicate);
            duplicate = -1;
        }
    }
#endif
    if (duplicate < 0)
        return -1;
    const int flags = ::fcntl(duplicate, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC) == 0) {
        ::close(duplicate);
        return -1;
    }
    return duplicate;
}

ReverseFdDecision ReverseFdOwner::send_attempt(local::Connection& connection) noexcept {
    if (cancelled_ || !valid() || !connection.valid() || !connection.peer_credentials_verified())
        return cancelled_ ? ReverseFdDecision::Cancelled : ReverseFdDecision::Invalid;
    const auto now = Clock::now();
    if (now >= deadline_) {
        expire(now);
        return ReverseFdDecision::Expired;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now);
    if (remaining.count() <= 0 || remaining.count() > kMaxReverseFdRemainingMs)
        return ReverseFdDecision::Expired;
    const int duplicate = duplicate_master();
    if (duplicate < 0)
        return ReverseFdDecision::IoError;
    ReverseFdAttempt attempt = identity_;
    attempt.remaining_ms = static_cast<uint32_t>(remaining.count());
    const auto wire = encode_reverse_fd_attempt(attempt);
    const bool sent = !wire.empty() && send_bytes(connection.native_handle(), wire, duplicate,
                                                   deadline_);
    ::close(duplicate);
    if (!sent) {
        if (Clock::now() >= deadline_)
            expire(Clock::now());
        return ReverseFdDecision::IoError;
    }
    std::vector<uint8_t> response(kHeaderSize);
    if (!read_plain(connection.native_handle(), response, deadline_)) {
        if (Clock::now() >= deadline_)
            expire(Clock::now());
        return ReverseFdDecision::IoError;
    }
    ReverseFdWireType type{};
    ReverseFdAttempt echoed{};
    uint32_t code = 0;
    uint32_t arm_size = 0;
    if (!decode_header(response, type, echoed, code, arm_size) ||
        (type != ReverseFdWireType::Ack && type != ReverseFdWireType::Nack) ||
        arm_size != 0 || echoed.identity != identity_.identity ||
        echoed.delivery_id != identity_.delivery_id || echoed.token != identity_.token)
        return ReverseFdDecision::Malformed;
    return decision_from_code(code);
}

void ReverseFdOwner::cancel() noexcept {
    cancelled_ = true;
    master_.reset();
}

void ReverseFdOwner::expire(TimePoint now) noexcept {
    if (now >= deadline_) {
        cancelled_ = true;
        master_.reset();
    }
}

ReverseFdDecision ReverseFdReceiverLedger::arm_input(const P50SourceArm& arm,
                                                     TimePoint now,
                                                     TimePoint deadline) noexcept {
    if (service_incarnation_ == 0 || !arm.valid() || state_ != ReverseFdReceiverState::Idle ||
        deadline < now)
        return ReverseFdDecision::Invalid;
    offered_arm_ = ReverseFdAttempt{local::Identity{1, 1}, 1, 1, 1, arm};
    deadline_ = deadline;
    state_ = ReverseFdReceiverState::WaitP50Input;
    return ReverseFdDecision::Accepted;
}

bool ReverseFdReceiverLedger::exact_accepted(const ReverseFdAttempt& attempt) const noexcept {
    return accepted_.has_value() && accepted_->identity == attempt.identity &&
           accepted_->delivery_id == attempt.delivery_id && accepted_->token == attempt.token &&
           accepted_->arm == attempt.arm;
}

void ReverseFdReceiverLedger::retire_fd() noexcept { compiler_fd_.reset(); }

ReverseFdDecision ReverseFdReceiverLedger::accept(ReverseFdAttempt attempt,
                                                   local::HandoffFd fd,
                                                   TimePoint now) noexcept {
    if (!attempt.valid() || !fd.valid())
        return ReverseFdDecision::Invalid;
    if (state_ == ReverseFdReceiverState::Expired || state_ == ReverseFdReceiverState::Cancelled ||
        state_ == ReverseFdReceiverState::Closed)
        return state_ == ReverseFdReceiverState::Expired ? ReverseFdDecision::Expired
               : ReverseFdDecision::Cancelled;
    if (attempt.delivery_id < high_water_)
        return ReverseFdDecision::Stale;
    if (attempt.delivery_id == high_water_ && accepted_.has_value())
        return exact_accepted(attempt) ? ReverseFdDecision::ExactReplay
                                       : ReverseFdDecision::Conflict;
    if (state_ != ReverseFdReceiverState::WaitP50Input || !offered_arm_.has_value())
        return ReverseFdDecision::PhaseViolation;
    if (now >= deadline_) {
        retire_fd();
        state_ = ReverseFdReceiverState::Expired;
        return ReverseFdDecision::Expired;
    }
    if (attempt.arm != offered_arm_->arm)
        return ReverseFdDecision::WrongArm;
    if (attempt.delivery_id == high_water_) {
        return exact_accepted(attempt) ? ReverseFdDecision::ExactReplay
                                       : ReverseFdDecision::Conflict;
    }
    high_water_ = attempt.delivery_id;
    accepted_ = attempt;
    compiler_fd_ = std::move(fd);
    state_ = ReverseFdReceiverState::ToCompile;
    ++transition_count_;
    ++fork_count_;
    return ReverseFdDecision::Accepted;
}

bool ReverseFdReceiverLedger::ackable(ReverseFdDecision result) const noexcept {
    return result == ReverseFdDecision::Accepted || result == ReverseFdDecision::ExactReplay ||
           result == ReverseFdDecision::Stale || result == ReverseFdDecision::Conflict ||
           result == ReverseFdDecision::WrongArm || result == ReverseFdDecision::Expired ||
           result == ReverseFdDecision::Cancelled || result == ReverseFdDecision::PhaseViolation;
}

local::HandoffFd ReverseFdReceiverLedger::take_for_compile() noexcept {
    if (state_ != ReverseFdReceiverState::ToCompile || !compiler_fd_.valid())
        return {};
    return std::move(compiler_fd_);
}

void ReverseFdReceiverLedger::cancel() noexcept {
    retire_fd();
    state_ = ReverseFdReceiverState::Cancelled;
}

void ReverseFdReceiverLedger::close() noexcept {
    retire_fd();
    state_ = ReverseFdReceiverState::Closed;
}

ReverseFdDecision receive_reverse_fd_attempt(
    local::Connection& connection, ReverseFdReceiverLedger& ledger,
    std::chrono::steady_clock::time_point io_deadline) noexcept {
    if (!connection.valid() || !connection.peer_credentials_verified())
        return ReverseFdDecision::Invalid;
    ReceivedWire received = receive_wire(connection.native_handle(), io_deadline);
    if (received.fd < 0 || received.malformed)
        return ReverseFdDecision::Malformed;
    const auto attempt = decode_reverse_fd_attempt(received.bytes);
    local::HandoffFd fd(received.fd);
    received.fd = -1;
    if (!attempt.has_value())
        return ReverseFdDecision::Malformed;
    const ReverseFdDecision result = ledger.accept(*attempt, std::move(fd),
                                                   std::chrono::steady_clock::now());
    const auto ack_wire = encode_wire(result == ReverseFdDecision::Accepted ||
                                      result == ReverseFdDecision::ExactReplay
                                          ? ReverseFdWireType::Ack
                                          : ReverseFdWireType::Nack,
                                      *attempt, code_for_decision(result));
    if (ledger.ackable(result) &&
        !send_bytes(connection.native_handle(), ack_wire, -1, io_deadline))
        return ReverseFdDecision::IoError;
    return result;
}

}  // namespace icecc::p50
