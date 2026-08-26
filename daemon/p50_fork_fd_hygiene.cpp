#include "p50_fork_fd_hygiene.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
#  include <sys/syscall.h>
#  include <sys/types.h>
#  if defined(SYS_close_range)
#    include <linux/close_range.h>
#  endif
#endif

namespace icecc::p50::forkfd {
namespace {

constexpr int kFirstNonstandardFd = STDERR_FILENO + 1;
constexpr size_t kMaxEnumeratedFds = 65536;
constexpr size_t kMaxProcReads = 4096;
constexpr size_t kMaxProcBytes = size_t{16} << 20;

#if defined(ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS)
TestHooks hooks;
#endif

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
    return keep.source.has_value() && fd == keep.source->fd;
}

Failure validate_keep_set(const KeepSet& keep) noexcept {
    if (keep.stat_pipe_fd < kFirstNonstandardFd ||
        keep.client_fd < kFirstNonstandardFd ||
        keep.stat_pipe_fd == keep.client_fd)
        return Failure::InvalidKeepSet;
    if (keep.source.has_value()) {
        if (keep.source->fd < kFirstNonstandardFd ||
            keep.source->delivery_id == 0 ||
            keep.source->fd == keep.stat_pipe_fd ||
            keep.source->fd == keep.client_fd)
            return Failure::InvalidKeepSet;
    }

    const std::array<int, 3> fds = {
        keep.stat_pipe_fd, keep.client_fd,
        keep.source.has_value() ? keep.source->fd : -1};
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

    struct stat client_info{};
    if (::fstat(keep.client_fd, &client_info) != 0)
        return Failure::OwnershipFailure;
    if (!S_ISSOCK(client_info.st_mode))
        return Failure::TypeFailure;

    if (keep.source.has_value()) {
        struct stat source_info{};
        if (::fstat(keep.source->fd, &source_info) != 0)
            return Failure::OwnershipFailure;
        if (!S_ISREG(source_info.st_mode))
            return Failure::TypeFailure;

        // The attachment producer promises an immutable, CLOEXEC snapshot.
        // Do not turn an unexpected ordinary inherited fd into an accepted
        // source merely because it happens to be a regular file.
        const int flags = ::fcntl(keep.source->fd, F_GETFD);
        if (flags < 0)
            return Failure::OwnershipFailure;
        if ((flags & FD_CLOEXEC) == 0)
            return Failure::OwnershipFailure;
        const int access = ::fcntl(keep.source->fd, F_GETFL);
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
    if (::close(proc_fd) != 0 && errno != EBADF && failure == Failure::None)
        failure = Failure::EnumerationFailure;
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
        keep.source.has_value() ? static_cast<unsigned int>(keep.source->fd) : 0};
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

Result sweep(const KeepSet& keep) noexcept {
    const Failure validation = validate_keep_set(keep);
    if (validation != Failure::None)
        return {validation, 0};

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
#endif

} // namespace icecc::p50::forkfd
