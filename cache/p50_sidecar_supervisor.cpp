#include "p50_sidecar_supervisor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
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
constexpr int kMaxFallbackFd = 1 << 20;
constexpr int64_t kMaxConfiguredMilliseconds = 7LL * 24 * 60 * 60 * 1000;

bool write_errno_record(int fd, int error) noexcept {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&error);
    size_t written = 0;
    while (written != sizeof(error)) {
        const ssize_t result = ::write(fd, bytes + written, sizeof(error) - written);
        if (result > 0) {
            written += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

bool set_cloexec(int fd, bool enabled) noexcept {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0)
        return false;
    const int wanted = enabled ? flags | FD_CLOEXEC : flags & ~FD_CLOEXEC;
    return wanted == flags || ::fcntl(fd, F_SETFD, wanted) == 0;
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
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ((flags & O_NONBLOCK) != 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

int fallback_fd_limit() noexcept {
    const long value = ::sysconf(_SC_OPEN_MAX);
    if (value < 3 || value > kMaxFallbackFd)
        return -1;
    return static_cast<int>(value);
}

bool mark_child_fds_cloexec(int fallback_limit) noexcept {
#if defined(__linux__) && defined(SYS_close_range)
    constexpr unsigned int kCloseRangeCloexec = 1u << 2;
    const long result = ::syscall(SYS_close_range, 3u, UINT_MAX, kCloseRangeCloexec);
    if (result == 0)
        return true;
    if (errno != ENOSYS && errno != EINVAL)
        return false;
#endif
    if (fallback_limit < 3)
        return false;
    for (int fd = 3; fd < fallback_limit; ++fd) {
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags < 0) {
            if (errno == EBADF)
                continue;
            return false;
        }
        if ((flags & FD_CLOEXEC) == 0 && ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
            return false;
    }
    return true;
}

bool command_is_valid(const Config& config) noexcept {
    if (config.executable.empty() || config.executable.front() != '/' ||
        config.executable.find('\0') != std::string::npos ||
        config.readiness_timeout.count() < 0 || config.shutdown_timeout.count() < 0 ||
        config.restart_window.count() <= 0 ||
        config.readiness_timeout.count() > kMaxConfiguredMilliseconds ||
        config.shutdown_timeout.count() > kMaxConfiguredMilliseconds ||
        config.restart_window.count() > kMaxConfiguredMilliseconds ||
        config.max_attempts_per_recovery == 0)
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
    if (fd >= 0)
        ::close(fd);
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
    case Failure::Exec: ++counters_.exec_failures; break;
    case Failure::ReadinessTimeout: ++counters_.readiness_timeouts; break;
    case Failure::PreReadyExit: ++counters_.pre_ready_exits; break;
    case Failure::InvalidReady: ++counters_.invalid_ready_messages; break;
    case Failure::PostReadyExit: ++counters_.post_ready_exits; break;
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

bool signal_group(pid_t process_group, int signal) noexcept {
    if (process_group <= 1)
        return false;
    for (int attempt = 0; attempt != 4; ++attempt) {
        if (::kill(-process_group, signal) == 0)
            return true;
        if (errno == EINTR)
            continue;
        return false;
    }
    return false;
}

bool group_exists(pid_t process_group) noexcept {
    if (process_group <= 1)
        return false;
    if (::kill(-process_group, 0) == 0)
        return true;
    return errno == EPERM;
}

bool Supervisor::wait_for_exit(std::chrono::milliseconds timeout) noexcept {
    if (child_pid_ < 0)
        return true;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int status = 0;
        const pid_t result = ::waitpid(child_pid_, &status, WNOHANG);
        if (result == child_pid_) {
            child_pid_ = -1;
            return true;
        }
        if (result < 0) {
            if (errno == EINTR)
                continue;
            if (errno == ECHILD) {
                child_pid_ = -1;
                return true;
            }
            return false;
        }
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
    if (child_pid_ < 0 && process_group_ < 0)
        return;
    const bool grouped = process_group_ > 1;
    bool term_sent = grouped ? signal_group(process_group_, SIGTERM) : false;
    if ((!grouped || !term_sent) && child_pid_ >= 0) {
        for (int attempt = 0; attempt != 4; ++attempt) {
            if (::kill(child_pid_, SIGTERM) == 0 || errno == ESRCH) {
                term_sent = true;
                break;
            }
            if (errno != EINTR)
                break;
        }
    }
    const bool direct_exited = child_pid_ < 0 || wait_for_exit(config_.shutdown_timeout);
    const bool group_alive = grouped && group_exists(process_group_);
    if (!direct_exited || group_alive) {
        bool killed = grouped ? signal_group(process_group_, SIGKILL) : false;
        if (!killed && child_pid_ >= 0) {
            for (int attempt = 0; attempt != 4; ++attempt) {
                if (::kill(child_pid_, SIGKILL) == 0 || errno == ESRCH) {
                    killed = true;
                    break;
                }
                if (errno != EINTR)
                    break;
            }
        }
        if (killed || !group_alive)
            ++counters_.forced_kills;
        if (child_pid_ >= 0)
            reap_blocking();
    } else if (child_pid_ >= 0) {
        reap_blocking();
    }
    if (!term_sent && child_pid_ >= 0) {
        // A transient signal failure must not leave an owned child behind.
        (void)::kill(child_pid_, SIGKILL);
        reap_blocking();
    }
    process_group_ = -1;
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
    if (child_pid_ < 0 && process_group_ < 0 && !has_private_fds()) {
        if (state_ != State::DegradedLegacy)
            state_ = State::Stopped;
        return;
    }
    ++counters_.shutdowns;
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
    ++counters_.restarts;
    return true;
}

bool Supervisor::launch_and_wait(bool restart) noexcept {
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
        ::close(ready_pipe[0]);
        ::close(exec_pipe[0]);
        if (::setpgid(0, 0) != 0) {
            const int error = errno;
            (void)write_errno_record(exec_pipe[1], error);
            _exit(127);
        }
        if (!mark_child_fds_cloexec(ambient_fd_limit)) {
            const int error = errno == 0 ? EMFILE : errno;
            (void)write_errno_record(exec_pipe[1], error);
            _exit(127);
        }
        (void)set_cloexec(ready_pipe[1], false);
        ::execve(config_.executable.c_str(), argv.data(), environment.data());
        const int error = errno;
        (void)write_errno_record(exec_pipe[1], error);
        _exit(127);
    }
    ::close(ready_pipe[1]);
    ::close(exec_pipe[1]);
    if (pid < 0) {
        close_if_open(ready_pipe[0]);
        close_if_open(exec_pipe[0]);
        classify(Failure::Exec);
        return false;
    }

    // The child sets its own group before exec; this parent-side call closes
    // the fork/exec race on platforms where the child reaches exec quickly.
    for (int attempt = 0; attempt != 4; ++attempt) {
        if (::setpgid(pid, pid) == 0)
            break;
        if (errno != EINTR)
            break; // EACCES means the child already performed setpgid.
    }

    child_pid_ = pid;
    process_group_ = pid;
    ready_read_ = ready_pipe[0];
    exec_read_ = exec_pipe[0];
    ++counters_.launches;
    state_ = State::Starting;
    if (wait_for_ready())
        return true;
    close_pipes();
    if (child_pid_ >= 0)
        terminate_child();
    return false;
}

bool Supervisor::wait_for_ready() noexcept {
    std::string ready;
    ready.reserve(kMaxReadyBytes);
    std::array<uint8_t, sizeof(int)> exec_error{};
    size_t exec_error_bytes = 0;
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
                        exec_read_, exec_error.data() + exec_error_bytes,
                        exec_error.size() - exec_error_bytes);
                    if (bytes > 0) {
                        exec_error_bytes += static_cast<size_t>(bytes);
                        if (exec_error_bytes == exec_error.size()) {
                            classify(Failure::Exec);
                            reap_blocking();
                            return false;
                        }
                        continue;
                    }
                    if (bytes == 0) {
                        if (exec_error_bytes != 0) {
                            classify(Failure::Exec);
                            reap_blocking();
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
                char bytes[32]{};
                const ssize_t count = ::read(ready_read_, bytes, sizeof(bytes));
                if (count > 0) {
                    ready.append(bytes, static_cast<size_t>(count));
                    if (ready.size() > kMaxReadyBytes ||
                        ready.compare(0, ready.size(), kReadyMessage, ready.size()) != 0) {
                        classify(Failure::InvalidReady);
                        return false;
                    }
                } else if (count == 0) {
                    // READY-FD EOF is not proof that the child exited.  Only
                    // waitpid may establish that; otherwise return failure so
                    // launch cleanup performs bounded TERM/KILL/reap.
                    int status = 0;
                    const pid_t child_result =
                        child_pid_ < 0 ? -1 : ::waitpid(child_pid_, &status, WNOHANG);
                    if (ready.size() == kReadyMessageSize) {
                        if (child_result == child_pid_ ||
                            (child_result < 0 && errno == ECHILD))
                            child_pid_ = -1;
                        close_if_open(ready_read_);
                        close_if_open(exec_read_);
                        state_ = State::Ready;
                        last_failure_ = Failure::None;
                        return true;
                    }
                    if (child_result == child_pid_ ||
                        (child_result < 0 && errno == ECHILD)) {
                        child_pid_ = -1;
                        classify(Failure::PreReadyExit);
                    } else {
                        classify(Failure::InvalidReady);
                    }
                    return false;
                }
            }
        }
        int status = 0;
        const pid_t result = ::waitpid(child_pid_, &status, WNOHANG);
        if (result == child_pid_) {
            child_pid_ = -1;
            // The child may have closed READY immediately after writing the
            // complete message.  Keep reading for the required EOF instead
            // of misclassifying that valid READY-then-close sequence as a
            // pre-ready crash.
            if (ready.size() == kReadyMessageSize)
                continue;
            classify(Failure::PreReadyExit);
            return false;
        }
        if (result < 0 && errno != EINTR && errno != ECHILD) {
            classify(Failure::PreReadyExit);
            return false;
        }
    }
    classify(Failure::ReadinessTimeout);
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
    int status = 0;
    const pid_t result = ::waitpid(child_pid_, &status, WNOHANG);
    if (result == 0)
        return true;
    if (result < 0 && errno == EINTR)
        return true;
    if (result < 0 && errno != ECHILD)
        return false;
    child_pid_ = -1;
    terminate_group();
    close_pipes();
    classify(Failure::PostReadyExit);
    return restart_after_failure();
}

} // namespace icecc::p50::sidecar
