#include "p50_fork_fd_hygiene.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <cstdio>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#if defined(__linux__)
#  include <sys/syscall.h>
#  include <sys/types.h>
#  if defined(SYS_close_range)
#    include <linux/close_range.h>
#  endif
#endif

namespace icecc::p50::forkfd {

namespace {

#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
TestHooks hooks;
#endif

bool close_exact(int fd) noexcept {
    for (;;) {
        if (::close(fd) == 0)
            return true;
        if (errno == EINTR)
            continue;
        return false;
    }
}

bool capture_source_identity(int fd, SourceIdentity* identity) noexcept {
    if (identity == nullptr)
        return false;
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
        return false;
    SourceIdentity captured;
    captured.device = static_cast<uint64_t>(info.st_dev);
    captured.inode = static_cast<uint64_t>(info.st_ino);
    captured.mode = static_cast<uint64_t>(info.st_mode);
    captured.size = static_cast<uint64_t>(info.st_size);
#if defined(F_GET_SEALS) && defined(F_SEAL_WRITE) && defined(F_SEAL_GROW) && \
    defined(F_SEAL_SHRINK)
    errno = 0;
    const int seals = ::fcntl(fd, F_GET_SEALS);
    if (seals < 0)
        return false;
    const int immutable_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK;
    if ((seals & immutable_seals) != immutable_seals)
        return false;
    captured.has_seals = true;
    captured.seals = static_cast<uint64_t>(seals);
#else
    // Without kernel seal introspection there is no causal way to prove that
    // a regular-file handoff cannot be mutated between mint and sweep.
    return false;
#endif
    *identity = captured;
    return captured.valid();
}

bool source_identity_equal(const SourceIdentity& left,
                           const SourceIdentity& right) noexcept {
    return left.valid() && right.valid() && left.device == right.device &&
           left.inode == right.inode && left.mode == right.mode &&
           left.size == right.size && left.has_seals == right.has_seals &&
           (!left.has_seals || left.seals == right.seals);
}

bool source_identity_matches(int fd, const SourceIdentity& expected) noexcept {
    SourceIdentity actual;
    if (!capture_source_identity(fd, &actual))
        return false;
    return source_identity_equal(actual, expected);
}

// Linux's kcmp(KCMP_FILE) compares the kernel open-file descriptions behind
// two descriptor numbers.  dev/ino/mode/size/seals cannot do this: a caller
// can close the proof number and dup3() the borrowed handoff into that slot,
// producing the same immutable file identity.  Unsupported or denied kcmp is
// deliberately a failure, because retiring an unproven descriptor is unsafe.
enum class OpenFileComparison : uint8_t { Same, Different, Unavailable };

OpenFileComparison compare_open_file_description(int left, int right) noexcept {
#if defined(__linux__) && defined(SYS_kcmp)
    if (left < 0 || right < 0)
        return OpenFileComparison::Unavailable;
#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
    if (hooks.force_kcmp_errno != 0) {
        if (hooks.force_kcmp_after_calls > 0)
            --hooks.force_kcmp_after_calls;
        else {
            errno = hooks.force_kcmp_errno;
            return OpenFileComparison::Unavailable;
        }
    }
#endif
    errno = 0;
    const long result = ::syscall(SYS_kcmp, static_cast<long>(::getpid()),
                                  static_cast<long>(::getpid()), 0, left, right);
    if (result == 0)
        return OpenFileComparison::Same;
    if (errno == ENOSYS || errno == EPERM || errno == EINTR)
        return OpenFileComparison::Unavailable;
    return OpenFileComparison::Different;
#else
    (void)left;
    (void)right;
    return OpenFileComparison::Unavailable;
#endif
}

bool same_open_file_description(int left, int right) noexcept {
    return compare_open_file_description(left, right) ==
           OpenFileComparison::Same;
}

// Open /proc/self/fd rather than F_DUPFD: the former creates a new open-file
// description, while the latter is another descriptor for the same OFD as the
// caller's handoff.  The distinction is what makes proof-slot replacement
// observable at retirement.
#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
int open_independent_readonly(int fd) noexcept {
#if defined(__linux__)
    if (fd < 0)
        return -1;
    char path[64]{};
    const int length = std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(path))
        return -1;
    const int result = ::open(path, O_RDONLY | O_CLOEXEC);
    if (result < 0 || same_open_file_description(result, fd)) {
        if (result >= 0)
            (void)::close(result);
        return -1;
    }
    return result;
#else
    (void)fd;
    return -1;
#endif
}
#endif

// The proof and control handles passed here are private ownership handles.
// They are never exposed as a caller-reusable numeric slot.  Consequently a
// failed kcmp (including ENOSYS/EPERM/EINTR) cannot justify touching any
// caller descriptor, but it is still safe and mandatory to retire both
// private handles.  Do not clear the slots before close: a close failure must
// leave the still-owned number available for a later destructor/assignment.
bool retire_owned_proof(int* proof_fd, int* control_fd,
                        const SourceIdentity& expected) noexcept {
    if (proof_fd == nullptr || control_fd == nullptr)
        return false;
    const int proof = *proof_fd;
    const int control = *control_fd;
    if (proof >= 0 && proof == control) {
        const bool closed = close_exact(proof);
        if (closed)
            *proof_fd = *control_fd = -1;
        return false;
    }
    const bool proved = proof >= 0 && control >= 0 &&
                        compare_open_file_description(proof, control) ==
                            OpenFileComparison::Same &&
                        source_identity_matches(proof, expected);
    const bool proof_closed = proof < 0 || close_exact(proof);
    if (proof < 0 || proof_closed)
        *proof_fd = -1;
    const bool control_closed = control < 0 || close_exact(control);
    if (control < 0 || control_closed)
        *control_fd = -1;
    return proved && proof_closed && control_closed;
}

#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
int duplicate_owner_fd(int fd) noexcept {
#if defined(F_DUPFD_CLOEXEC)
    const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
    const int duplicate = ::fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
#endif
    if (duplicate < 0)
        return -1;
#if !defined(F_DUPFD_CLOEXEC)
    const int flags = ::fcntl(duplicate, F_GETFD);
    if (flags < 0 || ::fcntl(duplicate, F_SETFD, flags | FD_CLOEXEC) != 0) {
        (void)::close(duplicate);
        return -1;
    }
#endif
    return duplicate;
}
#endif

#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
constexpr uint64_t kTestOwnerCookie = UINT64_C(0x9d5f31a7c2e84b61);
#endif

} // namespace

DeliveryOwnerToken::DeliveryOwnerToken(int expected_fd,
                                       uint64_t expected_delivery_id,
                                       int owned_proof_fd, int control_fd,
                                       SourceIdentity identity,
                                       uint64_t owner_cookie,
                                       int proof_slot_fd) noexcept
    : expected_fd_(expected_fd), expected_delivery_id_(expected_delivery_id),
      owned_proof_fd_(owned_proof_fd), control_fd_(control_fd),
      proof_slot_fd_(proof_slot_fd), identity_(identity),
      owner_cookie_(owner_cookie) {}

// A mint rejection normally leaves the owner available to its caller, but a
// malformed owner may have put the borrowed handoff number in one of its
// supposedly-private slots.  Remove only those aliased slots before the
// owner's natural destructor runs.  The handoff is caller-owned, so it must
// never be closed here; any remaining private handle still goes through the
// ordinary retirement path.
void DeliveryOwnerToken::disarm_rejected_alias(int handoff_fd) noexcept {
    const bool proof_alias = owned_proof_fd_ == handoff_fd;
    const bool control_alias = control_fd_ == handoff_fd;
    if (proof_alias)
        owned_proof_fd_ = -1;
    if (control_alias)
        control_fd_ = -1;
    if (proof_alias || control_alias) {
        (void)retire_owned_proof(&owned_proof_fd_, &control_fd_, identity_);
        expected_fd_ = -1;
        expected_delivery_id_ = 0;
        owner_cookie_ = 0;
    }
}

DeliveryOwnerToken::DeliveryOwnerToken(DeliveryOwnerToken&& other) noexcept
    : expected_fd_(other.expected_fd_),
      expected_delivery_id_(other.expected_delivery_id_),
      owned_proof_fd_(other.owned_proof_fd_),
      control_fd_(other.control_fd_),
      proof_slot_fd_(other.proof_slot_fd_),
      identity_(other.identity_),
      owner_cookie_(other.owner_cookie_) {
    other.expected_fd_ = -1;
    other.expected_delivery_id_ = 0;
    other.owned_proof_fd_ = -1;
    other.control_fd_ = -1;
    other.proof_slot_fd_ = -1;
    other.identity_ = SourceIdentity{};
    other.owner_cookie_ = 0;
}

DeliveryOwnerToken& DeliveryOwnerToken::operator=(DeliveryOwnerToken&& other) noexcept {
    if (this != &other) {
        (void)retire_owned_proof(&owned_proof_fd_, &control_fd_, identity_);
        expected_fd_ = other.expected_fd_;
        expected_delivery_id_ = other.expected_delivery_id_;
        owned_proof_fd_ = other.owned_proof_fd_;
        control_fd_ = other.control_fd_;
        // proof_slot_fd_ is a caller-owned observation descriptor.  It is
        // deliberately not overwritten: the caller must retain access to
        // both the old destination slot and the moved-from source slot.
        identity_ = other.identity_;
        owner_cookie_ = other.owner_cookie_;
        other.expected_fd_ = -1;
        other.expected_delivery_id_ = 0;
        other.owned_proof_fd_ = -1;
        other.control_fd_ = -1;
        other.identity_ = SourceIdentity{};
        other.owner_cookie_ = 0;
    }
    return *this;
}

DeliveryOwnerToken::~DeliveryOwnerToken() {
    (void)retire_owned_proof(&owned_proof_fd_, &control_fd_, identity_);
}

ForkSourceLease::ForkSourceLease(int fd, uint64_t delivery_id, int expected_fd,
                                 uint64_t expected_delivery_id, int owner_fd,
                                 int control_fd, SourceIdentity identity,
                                 uint64_t owner_cookie,
                                 int proof_slot_fd) noexcept
    : borrowed_handoff_fd_(fd), delivery_id_(delivery_id), expected_fd_(expected_fd),
      expected_delivery_id_(expected_delivery_id), owned_proof_fd_(owner_fd),
      control_fd_(control_fd), proof_slot_fd_(proof_slot_fd),
      identity_(identity), owner_cookie_(owner_cookie) {}

ForkSourceLease::ForkSourceLease(ForkSourceLease&& other) noexcept
    : borrowed_handoff_fd_(other.borrowed_handoff_fd_), delivery_id_(other.delivery_id_),
      expected_fd_(other.expected_fd_),
      expected_delivery_id_(other.expected_delivery_id_),
      owned_proof_fd_(other.owned_proof_fd_),
      control_fd_(other.control_fd_),
      proof_slot_fd_(other.proof_slot_fd_),
      identity_(other.identity_),
      owner_cookie_(other.owner_cookie_) {
    other.borrowed_handoff_fd_ = -1;
    other.delivery_id_ = 0;
    other.expected_fd_ = -1;
    other.expected_delivery_id_ = 0;
    other.owned_proof_fd_ = -1;
    other.control_fd_ = -1;
    other.proof_slot_fd_ = -1;
    other.identity_ = SourceIdentity{};
    other.owner_cookie_ = 0;
}

ForkSourceLease& ForkSourceLease::operator=(ForkSourceLease&& other) noexcept {
    if (this != &other) {
        // borrowed_handoff_fd_ belongs to the delivery caller.  Replacing a
        // lease must never close that caller-owned descriptor.
        (void)retire_owned_proof(&owned_proof_fd_, &control_fd_, identity_);
        borrowed_handoff_fd_ = other.borrowed_handoff_fd_;
        delivery_id_ = other.delivery_id_;
        expected_fd_ = other.expected_fd_;
        expected_delivery_id_ = other.expected_delivery_id_;
        owned_proof_fd_ = other.owned_proof_fd_;
        control_fd_ = other.control_fd_;
        // proof_slot_fd_ is caller-owned test state; retain both sides so a
        // move assignment cannot orphan the destination's open slot.
        identity_ = other.identity_;
        owner_cookie_ = other.owner_cookie_;
        other.borrowed_handoff_fd_ = -1;
        other.delivery_id_ = 0;
        other.expected_fd_ = -1;
        other.expected_delivery_id_ = 0;
        other.owned_proof_fd_ = -1;
        other.control_fd_ = -1;
        other.identity_ = SourceIdentity{};
        other.owner_cookie_ = 0;
    }
    return *this;
}

ForkSourceLease::~ForkSourceLease() {
    (void)retire_owned_proof(&owned_proof_fd_, &control_fd_, identity_);
}

bool ForkSourceLease::retire_identity_proof() noexcept {
    return retire_owned_proof(&owned_proof_fd_, &control_fd_, identity_);
}

bool ForkSourceLease::identity_matches_current() const noexcept {
    return valid() && same_open_file_description(owned_proof_fd_, control_fd_) &&
           source_identity_matches(owned_proof_fd_, identity_) &&
           source_identity_matches(borrowed_handoff_fd_, identity_);
}

std::optional<ForkSourceLease>
mint_fork_source_lease(DeliveryOwnerToken&& owner, int fd,
                       uint64_t delivery_id) noexcept {
    const OpenFileComparison proof_control =
        compare_open_file_description(owner.owned_proof_fd_, owner.control_fd_);
    const OpenFileComparison proof_handoff =
        compare_open_file_description(owner.owned_proof_fd_, fd);
    const OpenFileComparison control_handoff =
        compare_open_file_description(owner.control_fd_, fd);
    const bool rejected =
        owner.owner_cookie_ == 0 || owner.expected_fd_ < 0 ||
        owner.owned_proof_fd_ < 0 || owner.control_fd_ < 0 ||
        owner.owned_proof_fd_ == owner.control_fd_ ||
        owner.owned_proof_fd_ == fd || owner.control_fd_ == fd ||
        !owner.identity_.valid() ||
        owner.expected_delivery_id_ == 0 || fd != owner.expected_fd_ ||
        delivery_id != owner.expected_delivery_id_ ||
        proof_control != OpenFileComparison::Same ||
        proof_handoff != OpenFileComparison::Different ||
        control_handoff != OpenFileComparison::Different ||
        !source_identity_matches(owner.owned_proof_fd_, owner.identity_) ||
        !source_identity_matches(fd, owner.identity_);
    if (rejected) {
        if (owner.owned_proof_fd_ == fd || owner.control_fd_ == fd)
            owner.disarm_rejected_alias(fd);
        return std::nullopt;
    }
    ForkSourceLease result(fd, delivery_id, owner.expected_fd_,
                           owner.expected_delivery_id_, owner.owned_proof_fd_,
                           owner.control_fd_, owner.identity_,
                           owner.owner_cookie_, owner.proof_slot_fd_);
    owner.expected_fd_ = -1;
    owner.expected_delivery_id_ = 0;
    owner.owned_proof_fd_ = -1;
    owner.control_fd_ = -1;
    owner.proof_slot_fd_ = -1;
    owner.identity_ = SourceIdentity{};
    owner.owner_cookie_ = 0;
    return result;
}

namespace {

constexpr int kFirstNonstandardFd = STDERR_FILENO + 1;
constexpr size_t kMaxEnumeratedFds = 65536;
constexpr size_t kMaxProcReads = 4096;
constexpr size_t kMaxProcBytes = size_t{16} << 20;
bool injected_proc_failure() noexcept {
#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
    return hooks.force_proc_failure;
#else
    return false;
#endif
}

bool injected_parse_failure() noexcept {
#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
    return hooks.force_parse_failure;
#else
    return false;
#endif
}

bool close_one(int fd) noexcept {
#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
    if (fd == hooks.fail_close_fd) {
        errno = EIO;
        return false;
    }
#endif
    for (;;) {
        if (::close(fd) == 0)
            return true;
        if (errno == EINTR)
            continue;
        // The one-task pre-fork guard removes concurrent descriptor churn.
        // Treat EBADF like every other close error: the exact sweep is not
        // proved when an inventory member cannot be closed.
        return false;
    }
}

bool set_cloexec(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0)
        return false;
    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool is_kept(int fd, const KeepSet& keep) noexcept {
    if (fd == keep.stat_pipe_fd || fd == keep.client_fd)
        return true;
    return keep.source.has_value() && fd == keep.source->fd();
}

Failure validate_keep_set(const KeepSet& keep) noexcept {
    if (keep.stat_pipe_fd < kFirstNonstandardFd ||
        keep.client_fd < kFirstNonstandardFd ||
        keep.stat_pipe_fd == keep.client_fd)
        return Failure::InvalidKeepSet;
    if (keep.source_required != keep.source.has_value())
        return Failure::InvalidKeepSet;
    if (keep.source.has_value()) {
        if (!keep.source->valid() || !keep.source->identity_matches_current() ||
            keep.source->fd() < kFirstNonstandardFd ||
            keep.source->delivery_id() == 0 ||
            keep.source->fd() == keep.stat_pipe_fd ||
            keep.source->fd() == keep.client_fd)
            return Failure::InvalidKeepSet;
        if (keep.expected_source_fd.has_value() &&
            keep.source->fd() != *keep.expected_source_fd)
            return Failure::InvalidKeepSet;
    } else if (keep.expected_source_fd.has_value()) {
        return Failure::InvalidKeepSet;
    }

    const std::array<int, 3> fds = {
        keep.stat_pipe_fd, keep.client_fd,
        keep.source.has_value() ? keep.source->fd() : -1};
    for (const int fd : fds) {
        if (fd < 0)
            continue;
        if (::fcntl(fd, F_GETFD) < 0)
            return Failure::OwnershipFailure;
    }

    struct stat stat_pipe_info{};
    if (::fstat(keep.stat_pipe_fd, &stat_pipe_info) != 0)
        return Failure::OwnershipFailure;
    if (!S_ISFIFO(stat_pipe_info.st_mode))
        return Failure::TypeFailure;
    const int stat_flags = ::fcntl(keep.stat_pipe_fd, F_GETFL);
    if (stat_flags < 0)
        return Failure::OwnershipFailure;
    if ((stat_flags & O_ACCMODE) != O_WRONLY)
        return Failure::OwnershipFailure;

    struct stat client_info{};
    if (::fstat(keep.client_fd, &client_info) != 0)
        return Failure::OwnershipFailure;
    if (!S_ISSOCK(client_info.st_mode))
        return Failure::TypeFailure;
    const int client_flags = ::fcntl(keep.client_fd, F_GETFL);
    if (client_flags < 0 || (client_flags & O_ACCMODE) != O_RDWR)
        return Failure::OwnershipFailure;
    int client_type = 0;
    socklen_t client_type_size = sizeof(client_type);
    if (::getsockopt(keep.client_fd, SOL_SOCKET, SO_TYPE, &client_type,
                     &client_type_size) != 0 || client_type != SOCK_STREAM)
        return Failure::TypeFailure;
    sockaddr_storage peer{};
    socklen_t peer_size = sizeof(peer);
    if (::getpeername(keep.client_fd, reinterpret_cast<sockaddr*>(&peer),
                      &peer_size) != 0)
        return Failure::OwnershipFailure;

    if (keep.source.has_value()) {
        struct stat source_info{};
        if (::fstat(keep.source->fd(), &source_info) != 0)
            return Failure::OwnershipFailure;
        if (!S_ISREG(source_info.st_mode))
            return Failure::TypeFailure;

        // The attachment producer promises an immutable, CLOEXEC snapshot.
        // Do not turn an unexpected ordinary inherited fd into an accepted
        // source merely because it happens to be a regular file.
        const int flags = ::fcntl(keep.source->fd(), F_GETFD);
        if (flags < 0)
            return Failure::OwnershipFailure;
        if ((flags & FD_CLOEXEC) == 0)
            return Failure::OwnershipFailure;
        const int access = ::fcntl(keep.source->fd(), F_GETFL);
        if (access < 0 || (access & O_ACCMODE) != O_RDONLY)
            return Failure::OwnershipFailure;
    }

    // All surviving channels are made CLOEXEC before any compiler subtree is
    // launched.  This is a postcondition, not the first-fork sweep itself.
    for (const int fd : fds) {
        if (fd >= 0 && !set_cloexec(fd))
            return Failure::OwnershipFailure;
    }
    return Failure::None;
}

#if defined(__linux__)
struct LinuxDirent64 {
    uint64_t ino;
    int64_t off;
    unsigned short reclen;
    unsigned char type;
    char name[];
};

bool parse_fd_name(const char* name, size_t length, int* fd) noexcept {
    if (length == 0 || length > 10 ||
        (name[0] == '0' && length != 1))
        return false;
    uint64_t value = 0;
    for (size_t i = 0; i != length; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + static_cast<uint64_t>(c - '0');
        if (value > static_cast<uint64_t>(std::numeric_limits<int>::max()))
            return false;
    }
    *fd = static_cast<int>(value);
    return true;
}

Result proc_fallback(const KeepSet& keep) noexcept {
    if (injected_proc_failure())
        return {Failure::EnumerationFailure, 0};

    const int proc_fd = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (proc_fd < 0)
        return {Failure::EnumerationFailure, 0};

    std::array<int, kMaxEnumeratedFds> inventory{};
    size_t inventory_size = 0;
    std::array<char, 16 * 1024> buffer{};
    size_t reads = 0;
    size_t bytes = 0;
    Failure failure = Failure::None;
    for (;;) {
        if (++reads > kMaxProcReads) {
            failure = Failure::EnumerationFailure;
            break;
        }
        const long count = ::syscall(SYS_getdents64, proc_fd, buffer.data(), buffer.size());
        if (count == 0)
            break;
        if (count < 0) {
            if (errno == EINTR) {
                --reads;
                continue;
            }
            failure = Failure::EnumerationFailure;
            break;
        }
        bytes += static_cast<size_t>(count);
        if (bytes > kMaxProcBytes) {
            failure = Failure::EnumerationFailure;
            break;
        }
        size_t offset = 0;
        while (offset < static_cast<size_t>(count)) {
            if (static_cast<size_t>(count) - offset < sizeof(LinuxDirent64)) {
                failure = Failure::ParseFailure;
                break;
            }
            auto* entry = reinterpret_cast<const LinuxDirent64*>(buffer.data() + offset);
            if (entry->reclen < sizeof(LinuxDirent64) ||
                offset + entry->reclen > static_cast<size_t>(count)) {
                failure = Failure::ParseFailure;
                break;
            }
            const size_t name_capacity = entry->reclen - offsetof(LinuxDirent64, name);
            size_t name_length = 0;
            while (name_length < name_capacity && entry->name[name_length] != '\0')
                ++name_length;
            if (name_length == name_capacity) {
                failure = Failure::ParseFailure;
                break;
            }
            if (!(name_length == 1 && entry->name[0] == '.') &&
                !(name_length == 2 && entry->name[0] == '.' && entry->name[1] == '.')) {
                int fd = -1;
                if (injected_parse_failure() ||
                    !parse_fd_name(entry->name, name_length, &fd)) {
                    failure = Failure::ParseFailure;
                    break;
                }
                if (fd != proc_fd && !is_kept(fd, keep)) {
                    if (inventory_size == kMaxEnumeratedFds) {
                        failure = Failure::EnumerationFailure;
                        break;
                    }
                    inventory[inventory_size++] = fd;
                }
            }
            offset += entry->reclen;
        }
        if (failure != Failure::None)
            break;
    }
    bool proc_closed = false;
#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
    if (hooks.force_proc_close_ebadf) {
        errno = EBADF;
    } else
#endif
    {
        for (;;) {
            if (::close(proc_fd) == 0) {
                proc_closed = true;
                break;
            }
            if (errno == EINTR)
                continue;
            break;
        }
    }
    if (!proc_closed && failure == Failure::None)
        failure = Failure::CloseFailure;
    if (failure != Failure::None)
        return {failure, 0};

    size_t closed = 0;
    for (size_t i = 0; i != inventory_size; ++i) {
        if (!close_one(inventory[i]))
            return {Failure::CloseFailure, closed};
        ++closed;
    }
    return {Failure::None, closed};
}

Result close_range_sweep(const KeepSet& keep) noexcept {
#if defined(SYS_close_range)
#  if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
    if (hooks.force_close_range_failure)
        return {Failure::CloseFailure, 0};
    const bool unsupported = hooks.force_close_range_unsupported;
#  else
    const bool unsupported = false;
#  endif
    if (unsupported)
        return {Failure::BackendUnavailable, 0};

    std::array<unsigned int, 3> kept = {
        static_cast<unsigned int>(keep.stat_pipe_fd),
        static_cast<unsigned int>(keep.client_fd),
        keep.source.has_value() ? static_cast<unsigned int>(keep.source->fd()) : 0};
    const size_t kept_count = keep.source.has_value() ? 3 : 2;
    std::sort(kept.begin(), kept.begin() + kept_count);
    unsigned int first = kFirstNonstandardFd;
    size_t closed = 0;
    for (size_t i = 0; i != kept_count; ++i) {
        const unsigned int last = kept[i] - 1;
        if (first <= last) {
            if (::syscall(SYS_close_range, first, last, 0u) != 0) {
                if (errno == ENOSYS || errno == EINVAL || errno == EPERM)
                    return {Failure::BackendUnavailable, closed};
                return {Failure::CloseFailure, closed};
            }
            closed += static_cast<size_t>(last - first + 1);
        }
        first = kept[i] + 1;
    }
    if (first <= std::numeric_limits<unsigned int>::max()) {
        if (::syscall(SYS_close_range, first, std::numeric_limits<unsigned int>::max(), 0u) != 0) {
            if (errno == ENOSYS || errno == EINVAL || errno == EPERM)
                return {Failure::BackendUnavailable, closed};
            return {Failure::CloseFailure, closed};
        }
    }
    return {Failure::None, closed};
#else
    (void)keep;
    return {Failure::BackendUnavailable, 0};
#endif
}
#endif

Result bounded_fallback(const KeepSet& keep) noexcept {
#if defined(__linux__)
    return proc_fallback(keep);
#else
    const long configured = ::sysconf(_SC_OPEN_MAX);
    if (configured <= kFirstNonstandardFd || configured > 65536)
        return {Failure::EnumerationFailure, 0};
    size_t closed = 0;
    for (int fd = kFirstNonstandardFd; fd < configured; ++fd) {
        if (is_kept(fd, keep))
            continue;
        errno = 0;
        if (::fcntl(fd, F_GETFD) < 0) {
            if (errno == EBADF)
                continue;
            return {Failure::EnumerationFailure, closed};
        }
        if (!close_one(fd))
            return {Failure::CloseFailure, closed};
        ++closed;
    }
    return {Failure::None, closed};
#endif
}

} // namespace

Result sweep(KeepSet& keep) noexcept {
    const Failure validation = validate_keep_set(keep);
    if (validation != Failure::None)
        return {validation, 0};
    if (keep.source.has_value() && !keep.source->retire_identity_proof())
        return {Failure::CloseFailure, 0};

#if defined(__linux__)
    const Result range = close_range_sweep(keep);
    if (range.ok())
        return range;
    if (range.failure != Failure::BackendUnavailable)
        return range;
    return bounded_fallback(keep);
#else
    return bounded_fallback(keep);
#endif
}

const char* failure_name(Failure failure) noexcept {
    switch (failure) {
    case Failure::None: return "none";
    case Failure::InvalidKeepSet: return "invalid-keep-set";
    case Failure::OwnershipFailure: return "ownership-failure";
    case Failure::TypeFailure: return "type-failure";
    case Failure::EnumerationFailure: return "enumeration-failure";
    case Failure::ParseFailure: return "parse-failure";
    case Failure::CloseFailure: return "close-failure";
    case Failure::BackendUnavailable: return "backend-unavailable";
    }
    return "unknown";
}

#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
void set_test_hooks(TestHooks value) noexcept { hooks = value; }
void reset_test_hooks() noexcept { hooks = TestHooks{}; }

std::optional<DeliveryOwnerToken>
test_make_delivery_owner(int expected_fd, uint64_t expected_delivery_id) noexcept {
    if (expected_fd < 0 || expected_delivery_id == 0)
        return std::nullopt;
    SourceIdentity identity;
    if (!capture_source_identity(expected_fd, &identity))
        return std::nullopt;
    const int owner_fd = open_independent_readonly(expected_fd);
    const int control_fd = owner_fd >= 0 ? duplicate_owner_fd(owner_fd) : -1;
    const int proof_slot_fd = owner_fd >= 0 ? open_independent_readonly(expected_fd) : -1;
    if (owner_fd < 0 || control_fd < 0 || proof_slot_fd < 0 ||
        !same_open_file_description(owner_fd, control_fd) ||
        same_open_file_description(owner_fd, proof_slot_fd) ||
        !source_identity_matches(owner_fd, identity)) {
        if (owner_fd >= 0)
            (void)::close(owner_fd);
        if (control_fd >= 0)
            (void)::close(control_fd);
        if (proof_slot_fd >= 0)
            (void)::close(proof_slot_fd);
        return std::nullopt;
    }
    return DeliveryOwnerToken(expected_fd, expected_delivery_id, owner_fd,
                              control_fd, identity, kTestOwnerCookie,
                              proof_slot_fd);
}

int test_delivery_owner_proof_fd(const DeliveryOwnerToken& owner) noexcept {
    return owner.proof_slot_fd_;
}

int test_fork_source_lease_proof_fd(const ForkSourceLease& lease) noexcept {
    return lease.proof_slot_fd_;
}

std::optional<DeliveryOwnerToken>
test_make_delivery_owner_alias(int expected_fd,
                               uint64_t expected_delivery_id) noexcept {
    auto owner = test_make_delivery_owner(expected_fd, expected_delivery_id);
    if (!owner.has_value())
        return std::nullopt;
    const int old_proof = owner->owned_proof_fd_;
    const int old_control = owner->control_fd_;
    if (old_proof >= 0)
        (void)::close(old_proof);
    if (old_control >= 0)
        (void)::close(old_control);
    const int control = duplicate_owner_fd(expected_fd);
    if (control < 0)
        return std::nullopt;
    owner->owned_proof_fd_ = expected_fd;
    owner->control_fd_ = control;
    return owner;
}

std::optional<DeliveryOwnerToken>
test_make_delivery_owner_control_alias(int expected_fd,
                                       uint64_t expected_delivery_id) noexcept {
    auto owner = test_make_delivery_owner(expected_fd, expected_delivery_id);
    if (!owner.has_value())
        return std::nullopt;
    const int old_proof = owner->owned_proof_fd_;
    const int old_control = owner->control_fd_;
    if (old_proof >= 0)
        (void)::close(old_proof);
    if (old_control >= 0)
        (void)::close(old_control);
    const int proof = open_independent_readonly(expected_fd);
    const int control = duplicate_owner_fd(expected_fd);
    if (proof < 0 || control < 0) {
        if (proof >= 0)
            (void)::close(proof);
        if (control >= 0)
            (void)::close(control);
        return std::nullopt;
    }
    owner->owned_proof_fd_ = proof;
    owner->control_fd_ = control;
    return owner;
}

#endif

} // namespace icecc::p50::forkfd
