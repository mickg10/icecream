#include "p50_sidecar_supervisor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <string_view>
#include <cstdlib>

#include "services/digest128.h"

extern char** environ;

namespace icecc::p50::sidecar {
namespace {

constexpr char kReadyMessage[] = "READY\n";
constexpr size_t kReadyMessageSize = sizeof(kReadyMessage) - 1;
constexpr size_t kMaxReadyBytes = 1024;
constexpr int kPollSliceMilliseconds = 20;
constexpr int kReadyLeaseLivenessBarrierMilliseconds = 5;
constexpr int kMaxFallbackFd = 8192;
constexpr size_t kMaxProcFdBytes = 1u << 20;
constexpr size_t kMaxProcFdReads = 256;
constexpr int kMaxEintrRetries = 8;
constexpr uint32_t kExecSessionMarker = 0x50355353u; // "P5SS"
constexpr uint8_t kLaunchPermission = 0x50u;
constexpr int64_t kMaxConfiguredMilliseconds = 7LL * 24 * 60 * 60 * 1000;
constexpr uint32_t kMaxConfiguredAttempts = 1u << 20;

bool write_record(int fd, const void* data, size_t size) noexcept {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written = 0;
    int interrupted = 0;
    while (written != size) {
        const ssize_t result = ::write(fd, bytes + written, size - written);
        if (result > 0) {
            written += static_cast<size_t>(result);
            interrupted = 0;
            continue;
        }
        if (result < 0 && errno == EINTR && interrupted++ != kMaxEintrRetries)
            continue;
        return false;
    }
    return true;
}

bool write_errno_record(int fd, int error) noexcept {
    return write_record(fd, &error, sizeof(error));
}

bool write_session_marker(int fd) noexcept {
    return write_record(fd, &kExecSessionMarker, sizeof(kExecSessionMarker));
}

bool read_launch_permission(int fd) noexcept {
    uint8_t permission = 0;
    int interrupted = 0;
    for (;;) {
        const ssize_t result = ::read(fd, &permission, sizeof(permission));
        if (result == static_cast<ssize_t>(sizeof(permission)))
            return permission == kLaunchPermission;
        if (result < 0 && errno == EINTR && interrupted++ != kMaxEintrRetries)
            continue;
        return false;
    }
}

bool send_launch_permission(int fd) noexcept {
    int interrupted = 0;
    for (;;) {
        const ssize_t result = ::send(fd, &kLaunchPermission,
                                      sizeof(kLaunchPermission), MSG_NOSIGNAL);
        if (result == static_cast<ssize_t>(sizeof(kLaunchPermission)))
            return true;
        if (result < 0 && errno == EINTR && interrupted++ != kMaxEintrRetries)
            continue;
        return false;
    }
}

template <typename Integer>
void increment_saturating(Integer& value) noexcept {
    if (value != std::numeric_limits<Integer>::max())
        ++value;
}

bool set_cloexec(int fd, bool enabled) noexcept {
    int flags = -1;
    int interrupted = 0;
    for (;;) {
        flags = ::fcntl(fd, F_GETFD);
        if (flags >= 0 || errno != EINTR || interrupted++ == kMaxEintrRetries)
            break;
    }
    if (flags < 0)
        return false;
    const int wanted = enabled ? flags | FD_CLOEXEC : flags & ~FD_CLOEXEC;
    if (wanted == flags)
        return true;
    interrupted = 0;
    for (;;) {
        const int result = ::fcntl(fd, F_SETFD, wanted);
        if (result == 0 || errno != EINTR || interrupted++ == kMaxEintrRetries)
            return result == 0;
    }
}

bool make_pipe(int fds[2]) noexcept {
#if defined(__linux__) && defined(O_CLOEXEC)
    if (::pipe2(fds, O_CLOEXEC) == 0)
        return true;
    if (errno != ENOSYS && errno != EINVAL)
        return false;
#endif
    if (::pipe(fds) != 0)
        return false;
    if (!set_cloexec(fds[0], true) || !set_cloexec(fds[1], true)) {
        ::close(fds[0]);
        ::close(fds[1]);
        fds[0] = fds[1] = -1;
        return false;
    }
    return true;
}

bool make_launch_gate(int fds[2]) noexcept {
#if defined(SOCK_CLOEXEC)
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0)
        return true;
    if (errno != EINVAL)
        return false;
#endif
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return false;
    if (!set_cloexec(fds[0], true) || !set_cloexec(fds[1], true)) {
        (void)::close(fds[0]);
        (void)::close(fds[1]);
        fds[0] = fds[1] = -1;
        return false;
    }
    return true;
}

bool set_nonblocking(int fd) noexcept {
    int flags = -1;
    int interrupted = 0;
    for (;;) {
        flags = ::fcntl(fd, F_GETFL);
        if (flags >= 0 || errno != EINTR || interrupted++ == kMaxEintrRetries)
            break;
    }
    if (flags < 0 || (flags & O_NONBLOCK) != 0)
        return flags >= 0;
    interrupted = 0;
    for (;;) {
        const int result = ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (result == 0 || errno != EINTR || interrupted++ == kMaxEintrRetries)
            return result == 0;
    }
}

int fallback_fd_limit() noexcept {
    // Linux uses the /proc descriptor directory when close_range is not
    // available.  Other POSIX targets get a deliberately small bounded scan;
    // querying OPEN_MAX is only a pre-fork capability check, never a
    // post-fork readiness scan.  If the target's descriptor ceiling exceeds
    // that bound, fail closed rather than leak an ambient descriptor.
#if defined(__linux__) && defined(SYS_openat) && defined(SYS_getdents64) && \
    defined(O_DIRECTORY) && defined(O_CLOEXEC)
    return kMaxFallbackFd;
#else
    const long value = ::sysconf(_SC_OPEN_MAX);
    if (value < 3 || value > kMaxFallbackFd)
        return -1;
    return static_cast<int>(value);
#endif
}

#if defined(__linux__) && defined(SYS_openat) && defined(SYS_getdents64) && \
    defined(O_DIRECTORY) && defined(O_CLOEXEC)
struct LinuxDirent64 {
    uint64_t inode;
    int64_t offset;
    unsigned short record_length;
    unsigned char type;
    char name[1];
};

bool parse_proc_fd_name(const char* name, size_t size, int* fd) noexcept {
    if (size == 0 || (name[0] < '0' || name[0] > '9'))
        return false;
    int value = 0;
    for (size_t index = 0; index != size && name[index] != '\0'; ++index) {
        if (name[index] < '0' || name[index] > '9')
            return false;
        const int digit = name[index] - '0';
        if (value > (std::numeric_limits<int>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    if (value < 3)
        return false;
    *fd = value;
    return true;
}

bool mark_proc_fds_cloexec() noexcept {
    const long opened = ::syscall(SYS_openat, AT_FDCWD, "/proc/self/fd",
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (opened < 0 || opened > std::numeric_limits<int>::max())
        return false;
    int proc_fd = static_cast<int>(opened);
    std::array<unsigned char, 4096> buffer{};
    size_t total_bytes = 0;
    for (size_t read_count = 0; read_count != kMaxProcFdReads; ++read_count) {
        long count = -1;
        int interrupted = 0;
        for (;;) {
            count = ::syscall(SYS_getdents64, proc_fd, buffer.data(), buffer.size());
            if (count >= 0 || errno != EINTR || interrupted++ == kMaxEintrRetries)
                break;
        }
        if (count == 0) {
            int ignored = ::close(proc_fd);
            (void)ignored;
            return true;
        }
        if (count < 0 || static_cast<size_t>(count) > buffer.size() ||
            total_bytes > kMaxProcFdBytes - static_cast<size_t>(count)) {
            int ignored = ::close(proc_fd);
            (void)ignored;
            return false;
        }
        total_bytes += static_cast<size_t>(count);
        size_t offset = 0;
        while (offset != static_cast<size_t>(count)) {
            constexpr size_t name_offset = offsetof(LinuxDirent64, name);
            const size_t remaining = static_cast<size_t>(count) - offset;
            if (remaining < name_offset + 1)
                goto malformed_directory;
            unsigned short record_length_value = 0;
            std::memcpy(&record_length_value, buffer.data() + offset +
                                                   offsetof(LinuxDirent64, record_length),
                        sizeof(record_length_value));
            const size_t record_length = record_length_value;
            if (record_length < name_offset + 1 || record_length > remaining)
                goto malformed_directory;
            const size_t name_bytes = record_length - name_offset;
            int fd = -1;
            const char* entry_name = reinterpret_cast<const char*>(buffer.data() + offset +
                                                                     name_offset);
            if (parse_proc_fd_name(entry_name, name_bytes, &fd) && !set_cloexec(fd, true)) {
                if (errno != EBADF)
                    goto malformed_directory;
            }
            offset += record_length;
        }
        continue;

    malformed_directory:
        int ignored = ::close(proc_fd);
        (void)ignored;
        return false;
    }
    int ignored = ::close(proc_fd);
    (void)ignored;
    return false;
}
#endif

bool mark_child_fds_cloexec(int fallback_limit) noexcept {
#if defined(__linux__) && defined(SYS_close_range) && \
    !defined(ICECC_P50_FORCE_FD_FALLBACK)
    constexpr unsigned int kCloseRangeCloexec = 1u << 2;
    const long result = ::syscall(SYS_close_range, 3u, UINT_MAX, kCloseRangeCloexec);
    if (result == 0)
        return true;
    if (errno != ENOSYS && errno != EINVAL && errno != EPERM)
        return false;
#endif
#if defined(__linux__) && defined(SYS_openat) && defined(SYS_getdents64) && \
    defined(O_DIRECTORY) && defined(O_CLOEXEC)
    (void)fallback_limit;
    return mark_proc_fds_cloexec();
#else
    if (fallback_limit < 3)
        return false;
    for (int fd = 3; fd < fallback_limit; ++fd) {
        int flags = -1;
        int interrupted = 0;
        for (;;) {
            flags = ::fcntl(fd, F_GETFD);
            if (flags >= 0 || errno != EINTR || interrupted++ == kMaxEintrRetries)
                break;
        }
        if (flags < 0) {
            if (errno == EBADF)
                continue;
            return false;
        }
        if ((flags & FD_CLOEXEC) == 0 && !set_cloexec(fd, true))
            return false;
    }
    return true;
#endif
}

bool command_is_valid(const Config& config) noexcept {
    if (config.executable.empty() || config.executable.front() != '/' ||
        config.executable.find('\0') != std::string::npos ||
        config.readiness_timeout.count() < 0 || config.shutdown_timeout.count() < 0 ||
        config.restart_window.count() <= 0 ||
        config.readiness_timeout.count() > kMaxConfiguredMilliseconds ||
        config.shutdown_timeout.count() > kMaxConfiguredMilliseconds ||
        config.restart_window.count() > kMaxConfiguredMilliseconds ||
        config.max_restarts > kMaxConfiguredAttempts ||
        config.max_attempts_per_recovery == 0 ||
        config.max_attempts_per_recovery > kMaxConfiguredAttempts)
        return false;
    for (const std::string& argument : config.arguments) {
        if (argument.find('\0') != std::string::npos)
            return false;
    }
    struct stat info{};
    if (::stat(config.executable.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
        ::access(config.executable.c_str(), X_OK) != 0)
        return false;
    if (config.lease_root.empty())
        return true;
    struct stat root{};
    return config.launch_identities != nullptr && config.lease_root.front() == '/' &&
           config.lease_root.size() <= local::kMaxUnixPath &&
           config.lease_root.find_first_of(" \t\r\n") == std::string::npos &&
           ::lstat(config.lease_root.c_str(), &root) == 0 && S_ISDIR(root.st_mode) &&
           root.st_uid == ::geteuid() && (root.st_mode & 07777) == 0700;
}

std::string bytes_hex(std::span<const uint8_t> bytes);

bool parse_uint64(std::string_view text, uint64_t& value) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0'))
        return false;
    value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9' ||
            value > (std::numeric_limits<uint64_t>::max() -
                     static_cast<uint64_t>(character - '0')) / 10)
            return false;
        value = value * 10 + static_cast<uint64_t>(character - '0');
    }
    return true;
}

bool parse_ready_lease(std::string_view wire, const ReadyLease& expected,
                       pid_t child, ReadyLease& actual) noexcept {
    if (wire.size() < 10 || wire.back() != '\n' || wire.find('\0') != std::string_view::npos)
        return false;
    wire.remove_suffix(1);
    std::array<std::string_view, 11> fields{};
    size_t begin = 0;
    for (size_t index = 0; index != fields.size(); ++index) {
        if (begin >= wire.size())
            return false;
        const size_t end = wire.find(' ', begin);
        fields[index] = wire.substr(begin, end == std::string_view::npos
                                             ? wire.size() - begin
                                             : end - begin);
        if (fields[index].empty())
            return false;
        if (end == std::string_view::npos) {
            if (index + 1 != fields.size())
                return false;
            begin = wire.size();
            break;
        }
        if (index + 1 == fields.size() || end + 1 >= wire.size() ||
            wire[end + 1] == ' ')
            return false;
        begin = end + 1;
    }
    if (begin != wire.size())
        return false;
    if (fields[0] != "READY" || fields[1] != "v2")
        return false;
    static constexpr std::array<std::string_view, 9> keys = {
        "generation", "attempt", "pid", "C_STORE_GUID", "F_STORE_GUID",
        "PATH", "DIGEST", "DEV", "INO"};
    std::array<std::string_view, 9> values{};
    for (size_t index = 0; index != keys.size(); ++index) {
        const std::string_view field = fields[index + 2];
        const size_t equals = field.find('=');
        if (equals != keys[index].size() ||
            field.substr(0, equals) != keys[index] || equals + 1 >= field.size())
            return false;
        values[index] = field.substr(equals + 1);
    }
    uint64_t generation = 0, attempt = 0, pid = 0, device = 0, inode = 0;
    const bool scalar_ok = parse_uint64(values[0], generation) &&
                           parse_uint64(values[1], attempt) &&
                           parse_uint64(values[2], pid) &&
                           parse_uint64(values[7], device) &&
                           parse_uint64(values[8], inode);
    const bool identity_ok = pid == static_cast<uint64_t>(child) &&
                             generation == expected.identity.generation &&
                             attempt == expected.identity.attempt && device != 0 && inode != 0;
    const bool guid_ok = values[3] == bytes_hex(std::span<const uint8_t>(
                                      expected.c_store_guid.bytes.data(),
                                      expected.c_store_guid.bytes.size())) &&
                         values[4] == bytes_hex(std::span<const uint8_t>(
                                      expected.f_store_guid.bytes.data(),
                                      expected.f_store_guid.bytes.size()));
    const bool path_ok = values[5] == expected.socket_path &&
                         values[6] == digest128_hex(expected.socket_path_digest);
    if (!scalar_ok || !identity_ok || !guid_ok || !path_ok)
        return false;
    struct stat socket_info{};
    if (::lstat(expected.socket_path.c_str(), &socket_info) != 0 ||
        !S_ISSOCK(socket_info.st_mode) ||
        socket_info.st_dev != static_cast<dev_t>(device) ||
        socket_info.st_ino != static_cast<ino_t>(inode) ||
        socket_info.st_uid != ::geteuid() || (socket_info.st_mode & 07777) != 0600)
        return false;
    actual = expected;
    actual.pid = child;
    actual.listener_device = static_cast<dev_t>(device);
    actual.listener_inode = static_cast<ino_t>(inode);
    return actual.valid();
}

std::string bytes_hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

void close_if_open(int& fd) noexcept {
    if (fd >= 0) {
        // Do not retry close after EINTR: on Linux the descriptor is already
        // closed, and retrying could close an unrelated descriptor reused by
        // another owner.  The caller's ownership is cleared unconditionally.
        const int ignored = ::close(fd);
        (void)ignored;
    }
    fd = -1;
}

// renameat2(RENAME_NOREPLACE) is the capture primitive for lease teardown.
// A pathname is first moved out of the owner's namespace, then its captured
// inode is verified before removal.  If the entry changed, restoration is
// attempted only with NOREPLACE; a concurrent replacement therefore causes a
// deliberate leak rather than deletion of an unrelated object.
bool capture_and_remove_at(int parent_fd, std::string_view name,
                           const struct stat& expected, bool directory) noexcept {
#if defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int kRenameNoReplace = 1u;
    static std::atomic<uint64_t> sequence{1};
    try {
        const std::string source(name);
        struct stat before{};
        if (::fstatat(parent_fd, source.c_str(), &before, AT_SYMLINK_NOFOLLOW) != 0)
            return errno == ENOENT;
        const mode_t expected_type = expected.st_mode & S_IFMT;
        if ((before.st_mode & S_IFMT) != expected_type || before.st_dev != expected.st_dev ||
            before.st_ino != expected.st_ino)
            return false;
        std::string captured;
        bool captured_entry = false;
        for (unsigned attempt = 0; attempt != 32; ++attempt) {
            captured = ".icecc-lease-capture-" + std::to_string(::getpid()) + "-" +
                       std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
            const long renamed = ::syscall(SYS_renameat2, parent_fd,
                                           source.c_str(), parent_fd,
                                           captured.c_str(), kRenameNoReplace);
            if (renamed == 0) {
                captured_entry = true;
                break;
            }
            if (errno == EEXIST)
                continue;
            // ENOSYS/EINVAL (and denied syscall policies) are deliberately
            // not replaced with unlink/rmdir fallbacks: unsupported atomic
            // capture means the only safe result is to leak the lease.
            if (errno == ENOENT)
                return true;
            return false;
        }
        if (!captured_entry)
            return false;
        struct stat captured_info{};
        if (::fstatat(parent_fd, captured.c_str(), &captured_info,
                      AT_SYMLINK_NOFOLLOW) != 0 ||
            (captured_info.st_mode & S_IFMT) != expected_type ||
            captured_info.st_dev != expected.st_dev ||
            captured_info.st_ino != expected.st_ino) {
            // The capture was not ours.  Restore only when the original name
            // is still vacant; otherwise leave the unique capture behind.
            (void)::syscall(SYS_renameat2, parent_fd, captured.c_str(), parent_fd,
                            source.c_str(), kRenameNoReplace);
            return false;
        }
        const int remove_flags = directory ? AT_REMOVEDIR : 0;
        if (::unlinkat(parent_fd, captured.c_str(), remove_flags) == 0 || errno == ENOENT)
            return true;
        // Removal failed (for example, a non-empty directory).  Preserve the
        // object and restore it only if doing so cannot overwrite a newcomer.
        (void)::syscall(SYS_renameat2, parent_fd, captured.c_str(), parent_fd,
                        source.c_str(), kRenameNoReplace);
    } catch (...) {
        // Cleanup is noexcept and must never turn allocation failure into a
        // blind pathname deletion.
        return false;
    }
    return false;
#else
    (void)parent_fd;
    (void)name;
    (void)expected;
    (void)directory;
    return false;
#endif
}

bool split_parent_path(std::string_view path, std::string& parent,
                       std::string& basename) noexcept {
    try {
        const size_t slash = path.rfind('/');
        if (slash == std::string_view::npos || slash + 1 >= path.size())
            return false;
        parent = slash == 0 ? "/" : std::string(path.substr(0, slash));
        basename = path.substr(slash + 1);
        return !basename.empty() && basename.find('/') == std::string::npos;
    } catch (...) {
        return false;
    }
}

bool cleanup_lease_paths(const ReadyLease& lease) noexcept {
    if (!detail::canonical_absolute_lease_path(lease.private_directory) ||
        lease.directory_device == 0 || lease.directory_inode == 0 ||
        lease.private_directory.size() + sizeof("/cache.sock") - 1 > local::kMaxUnixPath ||
        lease.socket_path.size() != lease.private_directory.size() + sizeof("/cache.sock") - 1 ||
        lease.socket_path.compare(0, lease.private_directory.size(), lease.private_directory) != 0 ||
        lease.socket_path[lease.private_directory.size()] != '/' ||
        lease.socket_path.compare(lease.private_directory.size() + 1,
                                  sizeof("cache.sock") - 1, "cache.sock") != 0)
        return false;

    const int lease_fd = ::open(lease.private_directory.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (lease_fd < 0)
        return false;
    struct stat directory_info{};
    const bool directory_matches = ::fstat(lease_fd, &directory_info) == 0 &&
                                    S_ISDIR(directory_info.st_mode) &&
                                    directory_info.st_dev == lease.directory_device &&
                                    directory_info.st_ino == lease.directory_inode;
    if (!directory_matches) {
        (void)::close(lease_fd);
        return false;
    }

    bool socket_removed = true;
    if (lease.listener_device != 0 || lease.listener_inode != 0) {
        if (lease.listener_device == 0 || lease.listener_inode == 0) {
            (void)::close(lease_fd);
            return false;
        }
        struct stat socket_expected{};
        socket_expected.st_mode = S_IFSOCK;
        socket_expected.st_dev = lease.listener_device;
        socket_expected.st_ino = lease.listener_inode;
        socket_removed = capture_and_remove_at(lease_fd, "cache.sock", socket_expected, false);
    }
    (void)::close(lease_fd);
    if (!socket_removed)
        return false;

    std::string parent;
    std::string basename;
    if (!split_parent_path(lease.private_directory, parent, basename))
        return false;
    const int parent_fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                                       O_NOFOLLOW);
    if (parent_fd < 0)
        return false;
    struct stat directory_expected{};
    directory_expected.st_mode = S_IFDIR;
    directory_expected.st_dev = lease.directory_device;
    directory_expected.st_ino = lease.directory_inode;
    const bool directory_removed =
        capture_and_remove_at(parent_fd, basename, directory_expected, true);
    (void)::close(parent_fd);
    return directory_removed;
}

} // namespace

const char* state_name(State state) noexcept {
    switch (state) {
    case State::Stopped: return "stopped";
    case State::Starting: return "starting";
    case State::Ready: return "ready";
    case State::DegradedLegacy: return "degraded-legacy";
    case State::Stopping: return "stopping";
    }
    return "unknown";
}

const char* failure_name(Failure failure) noexcept {
    switch (failure) {
    case Failure::None: return "none";
    case Failure::InvalidConfiguration: return "invalid-configuration";
    case Failure::Exec: return "exec";
    case Failure::ReadinessTimeout: return "readiness-timeout";
    case Failure::PreReadyExit: return "pre-ready-exit";
    case Failure::InvalidReady: return "invalid-ready";
    case Failure::PostReadyExit: return "post-ready-exit";
    case Failure::RestartExhausted: return "restart-exhausted";
    }
    return "unknown";
}

Supervisor::Supervisor(Config config) : config_(std::move(config)) {}

Supervisor::~Supervisor() { shutdown(); }

LaunchIdentityAllocator::LaunchIdentityAllocator(uint64_t generation,
                                                 uint64_t first_attempt) noexcept
    : generation_(generation), next_attempt_(first_attempt) {
    // Zero is reserved by the wire contract.  MAX is also rejected rather
    // than permitting an allocation whose successor would wrap and become
    // indistinguishable from an uninitialized allocator.
    if (generation_ == 0 || generation_ == std::numeric_limits<uint64_t>::max() ||
        next_attempt_ == 0 || next_attempt_ == std::numeric_limits<uint64_t>::max()) {
        generation_ = 0;
        next_attempt_ = 0;
    }
}

std::optional<LaunchIncarnation> LaunchIdentityAllocator::allocate() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation_ == 0 || next_attempt_ == 0 ||
        next_attempt_ == std::numeric_limits<uint64_t>::max())
        return std::nullopt;
    const local::Identity identity{generation_, next_attempt_++};
    LaunchIncarnation incarnation{identity, c_store_guid_for_incarnation(identity),
                                  f_store_guid_for_incarnation(identity)};
    if (!incarnation.valid())
        return std::nullopt;
    return incarnation;
}

bool Supervisor::prepare_lease() noexcept {
    if (config_.lease_root.empty())
        return true;
    try {
        if (config_.launch_identities == nullptr)
            return false;
        const std::optional<LaunchIncarnation> incarnation =
            config_.launch_identities->allocate();
        if (!incarnation.has_value())
            return false;
        std::string pattern = config_.lease_root + "/g" +
                              std::to_string(incarnation->identity.generation) + "-a" +
                              std::to_string(incarnation->identity.attempt) + "-XXXXXX";
        std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
        mutable_pattern.push_back('\0');
        char* directory = ::mkdtemp(mutable_pattern.data());
        if (directory == nullptr)
            return false;
        struct stat directory_info{};
        if (::lstat(directory, &directory_info) != 0 || !S_ISDIR(directory_info.st_mode) ||
            directory_info.st_uid != ::geteuid() || (directory_info.st_mode & 07777) != 0700) {
            // No trusted directory identity exists on this path.  Leave it
            // for external recovery rather than raw pathname deletion.
            return false;
        }
        ReadyLease lease;
        lease.identity = incarnation->identity;
        lease.c_store_guid = incarnation->c_store_guid;
        lease.f_store_guid = incarnation->f_store_guid;
        lease.private_directory = directory;
        lease.socket_path = lease.private_directory + "/cache.sock";
        if (lease.socket_path.size() > local::kMaxUnixPath) {
            return false;
        }
        lease.socket_path_digest = digest128(lease.socket_path);
        lease.directory_device = directory_info.st_dev;
        lease.directory_inode = directory_info.st_ino;
        const auto cleanup_failed_setup = [&]() noexcept {
            close_if_open(pending_listener_fd_);
            // cleanup_lease_paths opens and verifies the held parent/directory
            // identities and only removes a socket whose recorded pathname
            // tuple is still exact.  Unknown socket identity deliberately
            // leaves the fresh directory behind rather than unlinking by
            // name.
            (void)cleanup_lease_paths(lease);
        };
        local::Status listener_status = local::Status::Ok;
        pending_listener_fd_ = local::listen_unix(lease.socket_path, 1, &listener_status);
        if (pending_listener_fd_ < 0 || listener_status != local::Status::Ok) {
            cleanup_failed_setup();
            return false;
        }
        struct stat listener_fd_info{};
        if (::fstat(pending_listener_fd_, &listener_fd_info) != 0 ||
            !S_ISSOCK(listener_fd_info.st_mode) || listener_fd_info.st_dev == 0 ||
            listener_fd_info.st_ino == 0) {
            cleanup_failed_setup();
            return false;
        }
        // The service publishes lstat(path), not fstat(inherited-fd): Linux
        // intentionally gives those two AF_UNIX identities different inode
        // tuples.  Validate the open descriptor above, then lease the exact
        // pathname dentry and its parent directory captured after bind.
        struct stat pathname_info{};
        if (::lstat(lease.socket_path.c_str(), &pathname_info) != 0 ||
            !S_ISSOCK(pathname_info.st_mode) || pathname_info.st_dev == 0 ||
            pathname_info.st_ino == 0) {
            cleanup_failed_setup();
            return false;
        }
        lease.listener_device = pathname_info.st_dev;
        lease.listener_inode = pathname_info.st_ino;
        pending_lease_ = std::move(lease);
        return true;
    } catch (...) {
        close_if_open(pending_listener_fd_);
        cleanup_lease(pending_lease_);
        return false;
    }
}

void Supervisor::cleanup_lease(std::optional<ReadyLease>& lease) noexcept {
    if (!lease.has_value())
        return;
    // A replacement pathname or inode is left for its owner.  Unsupported
    // atomic capture, a changed parent, and any failed restore all fail closed
    // as a leak; this function never falls back to lstat->unlink/rmdir.
    (void)cleanup_lease_paths(*lease);
    lease.reset();
}

bool Supervisor::valid_config(const Config& config) noexcept {
    return command_is_valid(config);
}

void Supervisor::classify(Failure failure) noexcept {
    last_failure_ = failure;
    switch (failure) {
    case Failure::Exec: increment_saturating(counters_.exec_failures); break;
    case Failure::ReadinessTimeout: increment_saturating(counters_.readiness_timeouts); break;
    case Failure::PreReadyExit: increment_saturating(counters_.pre_ready_exits); break;
    case Failure::InvalidReady: increment_saturating(counters_.invalid_ready_messages); break;
    case Failure::PostReadyExit: increment_saturating(counters_.post_ready_exits); break;
    default: break;
    }
}

void Supervisor::close_pipes() noexcept {
    close_if_open(ready_read_);
    close_if_open(exec_read_);
}

bool Supervisor::has_private_fds() const noexcept {
    // This public lifecycle observation predates pidfd and intentionally
    // reports only the readiness/exec channels: a Ready supervisor has closed
    // both even though it retains its internal exact child handle.
    return ready_read_ >= 0 || exec_read_ >= 0;
}

void Supervisor::reap_blocking() noexcept {
    if (child_pid_ < 0) {
        close_if_open(child_pidfd_);
        return;
    }
    // Once a pidfd has been acquired, never use the reusable numeric PID for
    // reaping.  An external SIGCHLD reaper may consume this child between a
    // kill/exit observation and this call; a later fork could then reuse the
    // numeric PID, and waitpid(child_pid_) would be able to reap that
    // unrelated child.  P_PIDFD keeps the reap bound to this incarnation.
#if defined(__linux__) && defined(WEXITED)
    if (child_pidfd_ >= 0) {
        constexpr idtype_t kPidfdIdType = static_cast<idtype_t>(3);
        for (;;) {
            siginfo_t information{};
            const int result = ::waitid(kPidfdIdType,
                                        static_cast<id_t>(child_pidfd_),
                                        &information, WEXITED);
            if (result == 0 || (result < 0 && errno == ECHILD))
                break;
            if (errno != EINTR)
                break;
        }
        child_pid_ = -1;
        close_if_open(child_pidfd_);
        return;
    }
#endif
    int status = 0;
    for (;;) {
        const pid_t result = ::waitpid(child_pid_, &status, 0);
        if (result == child_pid_)
            break;
        if (result < 0 && errno == EINTR)
            continue;
        // ECHILD means another owner already reaped it; do not loop forever.
        break;
    }
    child_pid_ = -1;
    close_if_open(child_pidfd_);
}

enum class SignalResult : uint8_t {
    Sent,
    Gone,
    Failed,
};

int open_child_handle(pid_t child) noexcept {
#if defined(__linux__) && defined(SYS_pidfd_open)
    int interrupted = 0;
    for (;;) {
        const long result = ::syscall(SYS_pidfd_open, child, 0u);
        if (result >= 0 && result <= std::numeric_limits<int>::max()) {
            const int fd = static_cast<int>(result);
            if (set_cloexec(fd, true))
                return fd;
            const int error = errno;
            (void)::close(fd);
            errno = error;
            return -1;
        }
        if (result >= 0) {
            (void)::close(static_cast<int>(result));
            errno = EOVERFLOW;
            return -1;
        }
        if (errno != EINTR || interrupted++ == kMaxEintrRetries)
            return -1;
    }
#else
    (void)child;
    errno = ENOSYS;
    return -1;
#endif
}

SignalResult signal_child_handle(int pidfd, int signal) noexcept {
#if defined(__linux__) && defined(SYS_pidfd_send_signal)
    if (pidfd < 0)
        return SignalResult::Failed;
    int interrupted = 0;
    for (;;) {
        const long result = ::syscall(SYS_pidfd_send_signal, pidfd, signal,
                                      nullptr, 0u);
        if (result == 0)
            return SignalResult::Sent;
        if (errno == ESRCH)
            return SignalResult::Gone;
        if (errno != EINTR || interrupted++ == kMaxEintrRetries)
            return SignalResult::Failed;
    }
#else
    (void)pidfd;
    (void)signal;
    return SignalResult::Failed;
#endif
}

bool exact_child_handles_supported() noexcept {
    const int self_handle = open_child_handle(::getpid());
    if (self_handle < 0)
        return false;
    const SignalResult result = signal_child_handle(self_handle, 0);
    (void)::close(self_handle);
    return result == SignalResult::Sent;
}

enum class ChildAnchorResult : uint8_t {
    Stopped,
    Exited,
    Failed,
};

ChildAnchorResult stop_child_handle(
    int pidfd, pid_t expected_child,
    std::chrono::milliseconds timeout) noexcept {
#if defined(__linux__) && defined(WNOWAIT)
    if (pidfd < 0 || expected_child <= 1)
        return ChildAnchorResult::Failed;
    const SignalResult stopped = signal_child_handle(pidfd, SIGSTOP);
    if (stopped == SignalResult::Gone)
        return ChildAnchorResult::Exited;
    if (stopped != SignalResult::Sent)
        return ChildAnchorResult::Failed;

    // Linux assigns idtype value 3 to P_PIDFD.  Use the value explicitly so
    // this exact-handle path still builds against libc headers predating the
    // spelling while requiring a kernel that already passed pidfd probes.
    constexpr idtype_t kPidfdIdType = static_cast<idtype_t>(3);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        siginfo_t information{};
        const int result = ::waitid(kPidfdIdType, static_cast<id_t>(pidfd),
                                    &information,
                                    WSTOPPED | WEXITED | WNOHANG | WNOWAIT);
        if (result == 0 && information.si_pid == expected_child) {
            if (information.si_code == CLD_STOPPED)
                return ChildAnchorResult::Stopped;
            if (information.si_code == CLD_EXITED ||
                information.si_code == CLD_KILLED ||
                information.si_code == CLD_DUMPED)
                return ChildAnchorResult::Exited;
            return ChildAnchorResult::Failed;
        }
        if (result < 0) {
            if (errno == EINTR)
                continue;
            // ECHILD includes an external reap.  That destroys the numeric
            // process-group anchor, so it is never treated as safe absence.
            return ChildAnchorResult::Failed;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return ChildAnchorResult::Failed;
        (void)::poll(nullptr, 0, 1);
    }
#else
    (void)pidfd;
    (void)expected_child;
    (void)timeout;
    return ChildAnchorResult::Failed;
#endif
}

bool child_handle_has_exited(int pidfd) noexcept {
    if (pidfd < 0)
        return false;
    struct pollfd descriptor{pidfd, POLLIN | POLLHUP, 0};
    int interrupted = 0;
    for (;;) {
        const int result = ::poll(&descriptor, 1, 0);
        if (result > 0)
            return (descriptor.revents & (POLLIN | POLLHUP)) != 0;
        if (result == 0)
            return false;
        if (errno != EINTR || interrupted++ == kMaxEintrRetries)
            return false;
    }
}

SignalResult signal_target(pid_t target, int signal) noexcept {
    if (target == 0 || target == std::numeric_limits<pid_t>::min())
        return SignalResult::Failed;
    int interrupted = 0;
    for (;;) {
        long result = -1;
#if defined(__linux__) && defined(SYS_kill)
        // Use the syscall entry point so a libc kill() interposer cannot make
        // a persistent EINTR look like a successful descendant teardown.
        result = ::syscall(SYS_kill, target, signal);
#else
        result = ::kill(target, signal);
#endif
        if (result == 0)
            return SignalResult::Sent;
        if (errno == ESRCH)
            return SignalResult::Gone;
        if (errno != EINTR || interrupted++ == kMaxEintrRetries)
            return SignalResult::Failed;
    }
}

bool signal_established(SignalResult result) noexcept {
    return result == SignalResult::Sent || result == SignalResult::Gone;
}

bool group_exists(pid_t process_group) noexcept {
    if (process_group <= 1)
        return false;
    // Unknown/permission results are treated as alive.  That is conservative:
    // teardown will try KILL, but will never block-reap solely on an uncertain
    // group probe.
    return signal_target(-process_group, 0) != SignalResult::Gone;
}

bool child_has_exited(pid_t child) noexcept {
    if (child < 0)
        return true;
#if defined(WNOWAIT)
    siginfo_t info{};
    int interrupted = 0;
    for (;;) {
        const int result = ::waitid(P_PID, static_cast<id_t>(child), &info,
                                    WEXITED | WNOHANG | WNOWAIT);
        if (result == 0) {
            if (info.si_pid == child)
                return true;
            return false;
        }
        if (errno == EINTR && interrupted++ != kMaxEintrRetries)
            continue;
        return errno == ECHILD;
    }
#else
    int status = 0;
    const pid_t result = ::waitpid(child, &status, WNOHANG);
    return result == child || (result < 0 && errno == ECHILD);
#endif
}

bool child_is_in_group(pid_t child, pid_t process_group) noexcept {
    int interrupted = 0;
    for (;;) {
        const pid_t observed = ::getpgid(child);
        if (observed >= 0)
            return observed == process_group;
        // ESRCH can mean another owner reaped the direct child.  Its numeric
        // PID and former PGID may already have been reused, so absence is not
        // proof that either numeric identity is still ours.  Fail closed and
        // leave the unique lease for manual recovery.
        if (errno == ESRCH)
            return false;
        if (errno != EINTR || interrupted++ == kMaxEintrRetries)
            return false;
    }
}

bool child_owns_session(pid_t child) noexcept {
    if (child <= 1)
        return false;
    int interrupted = 0;
    for (;;) {
        const pid_t session = ::getsid(child);
        if (session >= 0)
            return session == child && child_is_in_group(child, child);
        if (errno != EINTR || interrupted++ == kMaxEintrRetries)
            return false;
    }
}

bool Supervisor::child_has_exited_exact(pid_t expected_child) const noexcept {
    if (expected_child < 0 || expected_child != child_pid_)
        return true;
    if (child_pidfd_ >= 0)
        return child_handle_has_exited(child_pidfd_);
    return child_has_exited(expected_child);
}

bool Supervisor::terminate_group() noexcept {
    const pid_t expected_child = child_pid_;
    const pid_t expected_group = process_group_;
    const bool claimed_group = process_group_owned_ && expected_group > 1;

    if (expected_child < 0) {
        // Once the exact leader owner has disappeared, a numeric PGID can no
        // longer authorize a signal.  An unclaimed residue is harmless; a
        // claimed residue is uncertain and forbids cleanup/restart.
        const bool clean = !claimed_group;
        process_group_ = -1;
        process_group_owned_ = false;
        close_if_open(child_pidfd_);
        return clean;
    }

    const ChildAnchorResult anchor = stop_child_handle(
        child_pidfd_, expected_child, config_.shutdown_timeout);
    const bool group_anchored =
        anchor == ChildAnchorResult::Stopped && claimed_group &&
        child_is_in_group(expected_child, expected_group);

    if (!group_anchored) {
        // Never send a nonzero signal to an unanchored numeric PGID.  The
        // pidfd can still terminate the exact direct child.  A normal
        // one-process service can be proved gone by group absence afterward;
        // surviving helpers or a reused group force DegradedLegacy and leak
        // the unique lease rather than risking an unrelated process.
        const SignalResult direct_kill = signal_child_handle(child_pidfd_, SIGKILL);
        // Linux may report pidfd_send_signal success for an unreaped zombie;
        // that establishes teardown but is not a forced kill of a live task.
        if (anchor != ChildAnchorResult::Exited &&
            direct_kill == SignalResult::Sent)
            increment_saturating(counters_.forced_kills);
        if (signal_established(direct_kill) ||
            child_handle_has_exited(child_pidfd_))
            reap_blocking();
        const bool direct_dead = child_pid_ < 0;
        const bool group_dead = !claimed_group || !group_exists(expected_group);
        process_group_ = -1;
        process_group_owned_ = false;
        close_if_open(child_pidfd_);
        return direct_dead && group_dead;
    }

    // SIGSTOP plus waitid(P_PIDFD, WSTOPPED|WNOWAIT) keeps the exact leader
    // alive and unreapable while the numeric group is used.  Helpers receive a
    // bounded TERM grace period; the stopped leader retains TERM pending and
    // anchors the PGID until the group KILL has been issued.
    (void)signal_target(-expected_group, SIGTERM);
    const auto grace_deadline = std::chrono::steady_clock::now() +
                                config_.shutdown_timeout;
    while (std::chrono::steady_clock::now() < grace_deadline)
        (void)::poll(nullptr, 0, 1);

    const SignalResult group_kill = signal_target(-expected_group, SIGKILL);
    const SignalResult direct_kill = signal_child_handle(child_pidfd_, SIGKILL);
    const bool kill_established = signal_established(group_kill) ||
                                  signal_established(direct_kill);
    if (group_kill == SignalResult::Sent || direct_kill == SignalResult::Sent)
        increment_saturating(counters_.forced_kills);
    if (kill_established || child_handle_has_exited(child_pidfd_))
        reap_blocking();

    bool group_dead = false;
    const auto group_deadline = std::chrono::steady_clock::now() +
                                config_.shutdown_timeout;
    for (;;) {
        if (!group_exists(expected_group)) {
            group_dead = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= group_deadline)
            break;
        (void)::poll(nullptr, 0, 1);
    }

    const bool direct_dead = child_pid_ < 0;
    process_group_ = -1;
    process_group_owned_ = false;
    close_if_open(child_pidfd_);
    return direct_dead && group_dead;
}

bool Supervisor::terminate_child() noexcept {
    return terminate_group();
}

/*
 * The group is killed before a post-ready restart as well as during explicit
 * shutdown.  This prevents a helper forked by a cache service from surviving
 * the direct-child reap and becoming an orphan of the daemon.
 */
void Supervisor::shutdown() noexcept {
    if (child_pid_ < 0 && !process_group_owned_ && !has_private_fds() &&
        pending_listener_fd_ < 0) {
        if (state_ != State::DegradedLegacy)
            state_ = State::Stopped;
        return;
    }
    increment_saturating(counters_.shutdowns);
    state_ = State::Stopping;
    close_if_open(pending_listener_fd_);
    const bool proven_dead = terminate_child();
    if (proven_dead) {
        cleanup_lease(current_lease_);
        cleanup_lease(pending_lease_);
    } else {
        // Dropping metadata is safer than unlinking a path whose process group
        // may still be live.  The unique directory is intentionally leaked for
        // external/manual recovery rather than reused by another incarnation.
        current_lease_.reset();
        pending_lease_.reset();
    }
    close_pipes();
    // Closing the exact task reference cannot affect the child.  Do it even
    // on an uncertain final path so Supervisor destruction never leaks an FD.
    close_if_open(child_pidfd_);
    if (proven_dead) {
        state_ = State::Stopped;
        last_failure_ = Failure::None;
    } else {
        state_ = State::DegradedLegacy;
        last_failure_ = Failure::RestartExhausted;
    }
}

bool Supervisor::reserve_restart() noexcept {
    const auto now = std::chrono::steady_clock::now();
    restart_times_.erase(
        std::remove_if(restart_times_.begin(), restart_times_.end(), [&](auto timestamp) {
            return now - timestamp >= config_.restart_window;
        }),
        restart_times_.end());
    if (restart_times_.size() >= config_.max_restarts)
        return false;
    restart_times_.push_back(now);
    increment_saturating(counters_.restarts);
    return true;
}

bool Supervisor::launch_and_wait(bool restart) noexcept {
    // A failed readiness wait may already have reaped the direct child while
    // its helpers remain in the owned process group.  Teardown must therefore
    // precede creation of every retry's pipes and may never overwrite the old
    // PGID with the new child's PID.
    if ((child_pid_ >= 0 || process_group_owned_) && !terminate_group()) {
        last_failure_ = Failure::RestartExhausted;
        state_ = State::DegradedLegacy;
        current_lease_.reset();
        close_if_open(pending_listener_fd_);
        pending_lease_.reset();
        return false;
    }
    close_pipes();
    if (!process_group_owned_)
        process_group_ = -1;
    if (restart && !reserve_restart()) {
        last_failure_ = Failure::RestartExhausted;
        state_ = State::DegradedLegacy;
        return false;
    }
    if (!prepare_lease()) {
        classify(Failure::Exec);
        return false;
    }
    int ready_pipe[2] = {-1, -1};
    int exec_pipe[2] = {-1, -1};
    int launch_gate[2] = {-1, -1};
    if (!make_pipe(ready_pipe) || !make_pipe(exec_pipe) ||
        !make_launch_gate(launch_gate)) {
        close_if_open(ready_pipe[0]);
        close_if_open(ready_pipe[1]);
        close_if_open(exec_pipe[0]);
        close_if_open(exec_pipe[1]);
        close_if_open(launch_gate[0]);
        close_if_open(launch_gate[1]);
        classify(Failure::Exec);
        close_if_open(pending_listener_fd_);
        cleanup_lease(pending_lease_);
        return false;
    }
    if (!set_nonblocking(ready_pipe[0]) || !set_nonblocking(exec_pipe[0])) {
        close_if_open(ready_pipe[0]);
        close_if_open(ready_pipe[1]);
        close_if_open(exec_pipe[0]);
        close_if_open(exec_pipe[1]);
        close_if_open(launch_gate[0]);
        close_if_open(launch_gate[1]);
        classify(Failure::Exec);
        close_if_open(pending_listener_fd_);
        cleanup_lease(pending_lease_);
        return false;
    }

    std::vector<std::string> environment_storage;
    std::vector<char*> environment;
    const std::string ready_env = std::string(kReadyFdEnvironment) + "=" +
                                  std::to_string(ready_pipe[1]);
    bool replaced = false;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string value(*entry);
        const bool managed = value.rfind(std::string(kReadyFdEnvironment) + "=", 0) == 0 ||
                             value.rfind(std::string(kListenerFdEnvironment) + "=", 0) == 0 ||
                             value.rfind("ICECC_CACHE_SERVICE_READY_FORMAT=", 0) == 0 ||
                             value.rfind("ICECC_CACHE_SERVICE_EXPECTED_GENERATION=", 0) == 0 ||
                             value.rfind("ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT=", 0) == 0 ||
                             value.rfind("ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID=", 0) == 0 ||
                             value.rfind("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID=", 0) == 0 ||
                             value.rfind("ICECC_CACHE_SERVICE_EXPECTED_SOCKET=", 0) == 0 ||
                             value.rfind("ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST=", 0) == 0;
        if (managed) {
            if (!replaced) {
                environment_storage.push_back(ready_env);
                replaced = true;
            }
        } else {
            environment_storage.push_back(value);
        }
    }
    if (!replaced)
        environment_storage.push_back(ready_env);
    if (pending_lease_.has_value()) {
        environment_storage.push_back(std::string(kListenerFdEnvironment) + "=" +
                                      std::to_string(pending_listener_fd_));
        environment_storage.push_back("ICECC_CACHE_SERVICE_READY_FORMAT=2");
        environment_storage.push_back("ICECC_CACHE_SERVICE_EXPECTED_GENERATION=" +
                                      std::to_string(pending_lease_->identity.generation));
        environment_storage.push_back("ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT=" +
                                      std::to_string(pending_lease_->identity.attempt));
        environment_storage.push_back("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID=" +
                                      bytes_hex(std::span<const uint8_t>(
                                          pending_lease_->f_store_guid.bytes.data(),
                                          pending_lease_->f_store_guid.bytes.size())));
        environment_storage.push_back("ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID=" +
                                      bytes_hex(std::span<const uint8_t>(
                                          pending_lease_->c_store_guid.bytes.data(),
                                          pending_lease_->c_store_guid.bytes.size())));
        environment_storage.push_back("ICECC_CACHE_SERVICE_EXPECTED_SOCKET=" +
                                      pending_lease_->socket_path);
        environment_storage.push_back("ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST=" +
                                      digest128_hex(pending_lease_->socket_path_digest));
    }
    for (std::string& value : environment_storage)
        environment.push_back(value.data());
    environment.push_back(nullptr);

    std::vector<std::string> argv_storage;
    argv_storage.reserve(config_.arguments.size() + 1);
    argv_storage.push_back(config_.executable);
    for (const std::string& argument : config_.arguments)
        argv_storage.push_back(argument);
    std::vector<char*> argv;
    for (std::string& value : argv_storage)
        argv.push_back(value.data());
    argv.push_back(nullptr);

    // Prepare the bounded fallback limit before fork.  Linux normally uses
    // close_range(CLOSE_RANGE_CLOEXEC); the fallback is only entered when
    // that syscall is unavailable.
    const int ambient_fd_limit = fallback_fd_limit();
    const int inherited_listener_fd = pending_listener_fd_;

    const pid_t pid = ::fork();
    if (pid == 0) {
        // The ready write end is intentionally the sole inherited supervisor
        // descriptor.  The exec-status write end remains CLOEXEC: EOF means
        // execve succeeded, while a short errno record means it failed.
        close_if_open(ready_pipe[0]);
        close_if_open(exec_pipe[0]);
        close_if_open(launch_gate[1]);
        // The child cannot execute, exit, or create descendants until the
        // parent has acquired its exact pidfd.  EOF is a fail-closed refusal.
        const bool permitted = read_launch_permission(launch_gate[0]);
        close_if_open(launch_gate[0]);
        if (!permitted)
            _exit(127);
        // A fresh session makes this launch's process group non-joinable by
        // unrelated siblings in the daemon's session.  Descendants inherit
        // it unless they deliberately detach themselves.
        if (::setsid() < 0) {
            const int error = errno;
            (void)write_errno_record(exec_pipe[1], error);
            _exit(127);
        }
        if (!write_session_marker(exec_pipe[1]))
            _exit(127);
        if (!mark_child_fds_cloexec(ambient_fd_limit)) {
            const int error = errno == 0 ? EMFILE : errno;
            (void)write_errno_record(exec_pipe[1], error);
            _exit(127);
        }
        if (!set_cloexec(ready_pipe[1], false)) {
            const int error = errno == 0 ? EBADF : errno;
            (void)write_errno_record(exec_pipe[1], error);
            _exit(127);
        }
        if (inherited_listener_fd >= 0 &&
            !set_cloexec(inherited_listener_fd, false)) {
            const int error = errno == 0 ? EBADF : errno;
            (void)write_errno_record(exec_pipe[1], error);
            _exit(127);
        }
        ::execve(config_.executable.c_str(), argv.data(), environment.data());
        const int error = errno;
        (void)write_errno_record(exec_pipe[1], error);
        _exit(127);
    }
    close_if_open(ready_pipe[1]);
    close_if_open(exec_pipe[1]);
    close_if_open(launch_gate[0]);
    // The daemon's copy must not remain a second listener owner.  The child
    // has the only non-CLOEXEC copy after the pre-exec boundary.
    close_if_open(pending_listener_fd_);
    if (pid < 0) {
        close_if_open(ready_pipe[0]);
        close_if_open(exec_pipe[0]);
        close_if_open(launch_gate[1]);
        classify(Failure::Exec);
        cleanup_lease(pending_lease_);
        return false;
    }

    // The child is blocked on launch_gate, so it cannot disappear before this
    // exact handle is acquired.  Failure closes the gate and the child exits
    // itself; no numeric-PID signal or unowned process is permitted.
    const int pidfd = open_child_handle(pid);
    child_pid_ = pid;
    child_pidfd_ = pidfd;
    process_group_ = -1;
    process_group_owned_ = false;
    ready_read_ = ready_pipe[0];
    exec_read_ = exec_pipe[0];
    increment_saturating(counters_.launches);
    state_ = State::Starting;
    if (pidfd < 0) {
        close_if_open(launch_gate[1]);
        close_pipes();
        reap_blocking();
        classify(Failure::Exec);
        cleanup_lease(pending_lease_);
        return false;
    }
    if (!send_launch_permission(launch_gate[1])) {
        close_if_open(launch_gate[1]);
        close_pipes();
        reap_blocking();
        classify(Failure::Exec);
        cleanup_lease(pending_lease_);
        return false;
    }
    close_if_open(launch_gate[1]);
    if (wait_for_ready())
        return true;
    close_pipes();
    const bool proven_dead = terminate_child();
    if (proven_dead) {
        cleanup_lease(pending_lease_);
    } else {
        pending_lease_.reset();
        // An uncertain former group is a terminal incarnation fence, even if
        // the exact direct child was safely reaped through its pidfd.  A retry
        // must not create a replacement beside possibly surviving helpers.
        last_failure_ = Failure::RestartExhausted;
        state_ = State::DegradedLegacy;
    }
    return false;
}

bool Supervisor::wait_for_ready() noexcept {
    const pid_t expected_child = child_pid_;
    std::string ready;
    ready.reserve(kMaxReadyBytes);
    std::array<uint8_t, sizeof(uint32_t) + sizeof(int)> exec_status{};
    size_t exec_status_bytes = 0;
    bool ready_eof = false;
    bool invalid_ready = false;
    bool exec_succeeded = false;
    const auto deadline = std::chrono::steady_clock::now() + config_.readiness_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd fds[2] = {{ready_read_, POLLIN | POLLHUP | POLLERR, 0},
                                {exec_read_, POLLIN | POLLHUP | POLLERR, 0}};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int timeout = static_cast<int>(std::clamp<long long>(
            remaining.count(), 1, kPollSliceMilliseconds));
        const int polled = ::poll(fds, 2, timeout);
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            classify(Failure::PreReadyExit);
            return false;
        }
        if (polled > 0) {
            if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
                for (;;) {
                    const ssize_t bytes = ::read(
                        exec_read_, exec_status.data() + exec_status_bytes,
                        exec_status.size() - exec_status_bytes);
                    if (bytes > 0) {
                        exec_status_bytes += static_cast<size_t>(bytes);
                        if (exec_status_bytes >= sizeof(uint32_t)) {
                            uint32_t marker = 0;
                            std::memcpy(&marker, exec_status.data(), sizeof(marker));
                            if (marker != kExecSessionMarker) {
                                classify(Failure::Exec);
                                return false;
                            }
                            if (!process_group_owned_) {
                                // The private marker follows successful
                                // setsid().  While the exact child is live,
                                // independently confirm SID==PGID==PID.  If it
                                // crossed the exit edge during the check, the
                                // authenticated marker still records the
                                // claimed group, but teardown may never signal
                                // it without a live STOP anchor.
                                if (!child_handle_has_exited(child_pidfd_) &&
                                    !child_owns_session(expected_child) &&
                                    !child_handle_has_exited(child_pidfd_)) {
                                    classify(Failure::Exec);
                                    return false;
                                }
                                process_group_ = expected_child;
                                process_group_owned_ = expected_child > 1;
                            } else if (process_group_ != expected_child) {
                                classify(Failure::Exec);
                                return false;
                            }
                        }
                        if (exec_status_bytes == exec_status.size()) {
                            classify(Failure::Exec);
                            return false;
                        }
                        continue;
                    }
                    if (bytes == 0) {
                        if (exec_status_bytes == sizeof(uint32_t)) {
                            exec_succeeded = true;
                            close_if_open(exec_read_);
                        } else {
                            classify(Failure::Exec);
                            return false;
                        }
                    } else if (errno == EINTR) {
                        continue;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        classify(Failure::Exec);
                        return false;
                    }
                    break;
                }
            }
            if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                for (;;) {
                    char bytes[32]{};
                    const ssize_t count = ::read(ready_read_, bytes, sizeof(bytes));
                    if (count > 0) {
                        ready.append(bytes, static_cast<size_t>(count));
                        const std::string_view prefix = pending_lease_.has_value()
                                                            ? std::string_view("READY v2 ")
                                                            : std::string_view(kReadyMessage);
                        const size_t prefix_bytes =
                            std::min(ready.size(), prefix.size());
                        if (ready.size() > kMaxReadyBytes ||
                            std::string_view(ready).substr(0, prefix_bytes) !=
                                prefix.substr(0, prefix_bytes)) {
                            // Keep draining the exec-status side long enough
                            // to observe the group proof before cleanup.  A
                            // malformed READY frame must not erase evidence
                            // needed to tear down a helper after child reap.
                            invalid_ready = true;
                            close_if_open(ready_read_);
                            break;
                        }
                        continue;
                    }
                    if (count == 0) {
                        ready_eof = true;
                        close_if_open(ready_read_);
                        if (pending_lease_.has_value()) {
                            ReadyLease parsed;
                            if (!parse_ready_lease(ready, *pending_lease_, expected_child, parsed))
                                invalid_ready = true;
                            else
                                *pending_lease_ = std::move(parsed);
                        } else if (ready.size() != kReadyMessageSize) {
                            invalid_ready = true;
                        }
                        break;
                    }
                    if (errno == EINTR)
                        continue;
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        classify(Failure::PreReadyExit);
                        return false;
                    }
                    break;
                }
            }
        }

        if (invalid_ready && process_group_owned_) {
            classify(Failure::InvalidReady);
            return false;
        }
        if (ready_eof && exec_succeeded) {
            if (invalid_ready) {
                classify(Failure::InvalidReady);
                return false;
            }
            // A structured READY frame is a lease on a live incarnation, not
            // a tombstone proving that a child once bound the path.  Refuse a
            // child which exited after publishing READY but before promotion;
            // poll() must never expose its stale pathname as current.
            if (pending_lease_.has_value()) {
                // EOF can be observed a few instructions before an immediately
                // exiting child becomes waitable.  Give that terminal edge one
                // small bounded scheduling barrier, then require the launch to
                // remain live before publishing its lease.
                (void)::poll(nullptr, 0, kReadyLeaseLivenessBarrierMilliseconds);
                if (child_has_exited_exact(expected_child)) {
                    classify(Failure::PreReadyExit);
                    return false;
                }
            }
            close_pipes();
            if (pending_lease_.has_value()) {
                current_lease_ = std::move(pending_lease_);
                pending_lease_.reset();
            }
            state_ = State::Ready;
            last_failure_ = Failure::None;
            return true;
        }

        if (child_pid_ >= 0 && child_has_exited_exact(expected_child)) {
            // Keep the exited child unreaped for exact-handle classification.
            // terminate_group() will never signal its numeric PGID because an
            // exited leader cannot supply the required live STOP anchor.
            if (invalid_ready)
                continue;
            // A child may exit immediately after writing a complete READY
            // frame.  EOF plus the exec marker remains the deciding proof;
            // otherwise keep waiting for a possible pipe HUP or classify the
            // malformed/pre-ready exit below.
            if (ready.size() != kReadyMessageSize || !ready_eof) {
                if (!ready_eof)
                    continue;
                classify(Failure::PreReadyExit);
                return false;
            }
        }
    }
    classify(invalid_ready ? Failure::InvalidReady : Failure::ReadinessTimeout);
    return false;
}

bool Supervisor::start() noexcept {
    if (state_ == State::Ready)
        return true;
    if (state_ == State::DegradedLegacy)
        return false;
    if (!valid_config(config_)) {
        classify(Failure::InvalidConfiguration);
        state_ = State::DegradedLegacy;
        return false;
    }
    // Structured supervision depends on an exact task handle.  Probe both
    // pidfd_open and pidfd_send_signal before creating a child; an unsupported
    // kernel/platform remains legacy-usable but never launches a process that
    // would later require unsafe numeric-PID/PGID fallback teardown.
    if (!exact_child_handles_supported()) {
        classify(Failure::InvalidConfiguration);
        state_ = State::DegradedLegacy;
        return false;
    }
    if (child_pid_ >= 0 || has_private_fds()) {
        shutdown();
        if (state_ == State::DegradedLegacy || child_pid_ >= 0)
            return false;
    }
    state_ = State::Starting;
    bool restart = false;
    for (uint32_t attempt = 0; attempt != config_.max_attempts_per_recovery; ++attempt) {
        if (launch_and_wait(restart))
            return true;
        if (state_ == State::DegradedLegacy)
            return false;
        restart = true;
    }
    last_failure_ = Failure::RestartExhausted;
    state_ = State::DegradedLegacy;
    return false;
}

bool Supervisor::restart_after_failure() noexcept {
    for (uint32_t attempt = 0; attempt != config_.max_attempts_per_recovery; ++attempt) {
        if (launch_and_wait(true))
            return true;
        if (state_ == State::DegradedLegacy)
            return false;
    }
    last_failure_ = Failure::RestartExhausted;
    state_ = State::DegradedLegacy;
    return false;
}

bool Supervisor::poll() noexcept {
    if (state_ != State::Ready)
        return false;
    if (child_pid_ < 0) {
        const bool proven_dead = terminate_group();
        if (proven_dead)
            cleanup_lease(current_lease_);
        else {
            current_lease_.reset();
            state_ = State::DegradedLegacy;
            last_failure_ = Failure::RestartExhausted;
            return false;
        }
        close_pipes();
        classify(Failure::PostReadyExit);
        return restart_after_failure();
    }
    if (!child_has_exited_exact(child_pid_))
        return true;
    const bool proven_dead = terminate_group();
    if (proven_dead)
        cleanup_lease(current_lease_);
    else {
        current_lease_.reset();
        state_ = State::DegradedLegacy;
        last_failure_ = Failure::RestartExhausted;
        return false;
    }
    close_pipes();
    classify(Failure::PostReadyExit);
    return restart_after_failure();
}

} // namespace icecc::p50::sidecar
