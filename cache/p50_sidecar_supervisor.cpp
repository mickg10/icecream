#include "p50_sidecar_supervisor.h"

#include <algorithm>
#include <array>
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace icecc::p50::sidecar {
namespace {

constexpr char kReadyMessage[] = "READY\n";
constexpr size_t kReadyMessageSize = sizeof(kReadyMessage) - 1;
constexpr size_t kMaxReadyBytes = kReadyMessageSize;
constexpr int kPollSliceMilliseconds = 20;
constexpr int kMaxFallbackFd = 8192;
constexpr size_t kMaxProcFdBytes = 1u << 20;
constexpr size_t kMaxProcFdReads = 256;
constexpr int kMaxEintrRetries = 8;
constexpr uint32_t kExecGroupMarker = 0x50355047u; // "P5PG"
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

bool write_group_marker(int fd) noexcept {
    return write_record(fd, &kExecGroupMarker, sizeof(kExecGroupMarker));
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
    return ::stat(config.executable.c_str(), &info) == 0 && S_ISREG(info.st_mode) &&
           ::access(config.executable.c_str(), X_OK) == 0;
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
    return ready_read_ >= 0 || exec_read_ >= 0;
}

void Supervisor::reap_blocking() noexcept {
    if (child_pid_ < 0)
        return;
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
}

enum class SignalResult : uint8_t {
    Sent,
    Gone,
    Failed,
};

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
        // ECHILD/ESRCH can mean another owner reaped the direct child before
        // teardown.  The exec marker still proves this PGID was ours; retain
        // the group signal so its helpers are not orphaned.  A reused live PID
        // is handled by the observed-PGID mismatch above.
        if (errno == ESRCH)
            return true;
        if (errno != EINTR || interrupted++ == kMaxEintrRetries)
            return false;
    }
}

bool Supervisor::wait_for_exit(std::chrono::milliseconds timeout) noexcept {
    if (child_pid_ < 0)
        return true;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (child_has_exited(child_pid_))
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int sleep_ms = static_cast<int>(std::clamp<long long>(
            remaining.count(), 1, kPollSliceMilliseconds));
        (void)::poll(nullptr, 0, sleep_ms);
    }
}

void Supervisor::terminate_group() noexcept {
    bool grouped = process_group_owned_ && process_group_ > 1;
    pid_t process_group = grouped ? process_group_ : -1;
    if (grouped && child_pid_ >= 0 && !child_is_in_group(child_pid_, process_group)) {
        // A service that moved itself out of the owned group invalidates our
        // group identity.  Refuse to signal the stale numeric PGID; direct-PID
        // teardown remains safe and the ownership bit is cleared.
        grouped = false;
        process_group = -1;
        process_group_ = -1;
        process_group_owned_ = false;
    }
    if (child_pid_ < 0 && !grouped) {
        // A PGID without the ownership bit is diagnostic residue only.  Never
        // signal it: the numeric ID may already belong to an unrelated group.
        process_group_ = -1;
        process_group_owned_ = false;
        return;
    }

    const SignalResult group_term = grouped ? signal_target(-process_group, SIGTERM)
                                             : SignalResult::Gone;
    const SignalResult direct_term = child_pid_ >= 0
                                         ? signal_target(child_pid_, SIGTERM)
                                         : SignalResult::Gone;
    (void)group_term;
    (void)direct_term;

    // A blocking reap is permitted only after the child has been observed
    // exited or a signal result established that it cannot remain running.
    bool direct_exited = child_pid_ < 0;
    if (!direct_exited)
        direct_exited = wait_for_exit(config_.shutdown_timeout);

    const bool group_alive = grouped && group_exists(process_group);
    const bool need_kill = !direct_exited || group_alive;
    SignalResult group_kill = SignalResult::Gone;
    SignalResult direct_kill = SignalResult::Gone;
    if (need_kill) {
        if (grouped)
            group_kill = signal_target(-process_group, SIGKILL);
        // Always signal the direct PID as well.  A child can voluntarily move
        // itself after launch, and a group-only success must not strand it.
        if (child_pid_ >= 0)
            direct_kill = signal_target(child_pid_, SIGKILL);
        const bool kill_established = signal_established(group_kill) ||
                                      signal_established(direct_kill);
        if (kill_established)
            increment_saturating(counters_.forced_kills);
        if (child_pid_ >= 0 && (signal_established(direct_kill) || direct_exited))
            reap_blocking();
    } else if (child_pid_ >= 0) {
        // wait_for_exit() observed an exited child without reaping it.  Keep
        // the group leader's PID alive as a zombie until group teardown has
        // completed; this closes the PGID-reuse window.
        reap_blocking();
    }

    // If all signal attempts were interrupted/failed, do not turn an unknown
    // liveness result into an unbounded wait.  The ownership bit is cleared so
    // a later launch/destructor cannot accidentally target a reused PGID.
    process_group_ = -1;
    process_group_owned_ = false;
}

void Supervisor::terminate_child() noexcept {
    terminate_group();
}

/*
 * The group is killed before a post-ready restart as well as during explicit
 * shutdown.  This prevents a helper forked by a cache service from surviving
 * the direct-child reap and becoming an orphan of the daemon.
 */
void Supervisor::shutdown() noexcept {
    if (child_pid_ < 0 && !process_group_owned_ && !has_private_fds()) {
        if (state_ != State::DegradedLegacy)
            state_ = State::Stopped;
        return;
    }
    increment_saturating(counters_.shutdowns);
    state_ = State::Stopping;
    terminate_child();
    close_pipes();
    state_ = State::Stopped;
    last_failure_ = Failure::None;
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
    if (child_pid_ >= 0 || process_group_owned_)
        terminate_group();
    close_pipes();
    if (!process_group_owned_)
        process_group_ = -1;
    if (restart && !reserve_restart()) {
        last_failure_ = Failure::RestartExhausted;
        state_ = State::DegradedLegacy;
        return false;
    }
    int ready_pipe[2] = {-1, -1};
    int exec_pipe[2] = {-1, -1};
    if (!make_pipe(ready_pipe) || !make_pipe(exec_pipe)) {
        close_if_open(ready_pipe[0]);
        close_if_open(ready_pipe[1]);
        close_if_open(exec_pipe[0]);
        close_if_open(exec_pipe[1]);
        classify(Failure::Exec);
        return false;
    }
    if (!set_nonblocking(ready_pipe[0]) || !set_nonblocking(exec_pipe[0])) {
        close_if_open(ready_pipe[0]);
        close_if_open(ready_pipe[1]);
        close_if_open(exec_pipe[0]);
        close_if_open(exec_pipe[1]);
        classify(Failure::Exec);
        return false;
    }

    std::vector<std::string> environment_storage;
    std::vector<char*> environment;
    const std::string ready_env = std::string(kReadyFdEnvironment) + "=" +
                                  std::to_string(ready_pipe[1]);
    bool replaced = false;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        const std::string value(*entry);
        if (value.rfind(std::string(kReadyFdEnvironment) + "=", 0) == 0) {
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

    const pid_t pid = ::fork();
    if (pid == 0) {
        // The ready write end is intentionally the sole inherited supervisor
        // descriptor.  The exec-status write end remains CLOEXEC: EOF means
        // execve succeeded, while a short errno record means it failed.
        close_if_open(ready_pipe[0]);
        close_if_open(exec_pipe[0]);
        if (::setpgid(0, 0) != 0) {
            const int error = errno;
            (void)write_errno_record(exec_pipe[1], error);
            _exit(127);
        }
        if (!write_group_marker(exec_pipe[1]))
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
        ::execve(config_.executable.c_str(), argv.data(), environment.data());
        const int error = errno;
        (void)write_errno_record(exec_pipe[1], error);
        _exit(127);
    }
    close_if_open(ready_pipe[1]);
    close_if_open(exec_pipe[1]);
    if (pid < 0) {
        close_if_open(ready_pipe[0]);
        close_if_open(exec_pipe[0]);
        classify(Failure::Exec);
        return false;
    }

    // The child sets its own group before exec; this parent-side call closes
    // the fork/exec race on platforms where the child reaches exec quickly.
    bool parent_group_proven = false;
    for (int attempt = 0; attempt != kMaxEintrRetries; ++attempt) {
        if (::setpgid(pid, pid) == 0)
            parent_group_proven = true;
        if (parent_group_proven)
            break;
        if (errno != EINTR)
            break;
    }
    if (!parent_group_proven && errno == EACCES)
        parent_group_proven = true; // child-side setpgid completed before exec

    child_pid_ = pid;
    process_group_ = parent_group_proven ? pid : -1;
    process_group_owned_ = parent_group_proven;
    ready_read_ = ready_pipe[0];
    exec_read_ = exec_pipe[0];
    increment_saturating(counters_.launches);
    state_ = State::Starting;
    if (wait_for_ready())
        return true;
    close_pipes();
    terminate_child();
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
                            if (marker != kExecGroupMarker) {
                                classify(Failure::Exec);
                                return false;
                            }
                            if (!process_group_owned_) {
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
                        if (ready.size() > kMaxReadyBytes ||
                            ready.compare(0, ready.size(), kReadyMessage, ready.size()) != 0) {
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
                        if (ready.size() != kReadyMessageSize)
                            invalid_ready = true;
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
            close_pipes();
            state_ = State::Ready;
            last_failure_ = Failure::None;
            return true;
        }

        if (child_pid_ >= 0 && child_has_exited(expected_child)) {
            // Keep the exited child unreaped until terminate_group() has
            // signaled its owned PGID.  The zombie PID prevents a reused PGID
            // from being mistaken for this launch's group.
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
    if (child_pid_ >= 0 || has_private_fds())
        shutdown();
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
        terminate_group();
        close_pipes();
        classify(Failure::PostReadyExit);
        return restart_after_failure();
    }
    if (!child_has_exited(child_pid_))
        return true;
    terminate_group();
    close_pipes();
    classify(Failure::PostReadyExit);
    return restart_after_failure();
}

} // namespace icecc::p50::sidecar
