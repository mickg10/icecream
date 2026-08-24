#include "p50_sidecar_supervisor.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
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

bool command_is_valid(const Config& config) noexcept {
    if (config.executable.empty() || config.executable.front() != '/' ||
        config.executable.find('\0') != std::string::npos ||
        config.readiness_timeout.count() < 0 || config.shutdown_timeout.count() < 0 ||
        config.restart_window.count() <= 0)
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
    case Failure::Shutdown: return "shutdown";
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

void Supervisor::terminate_child() noexcept {
    if (child_pid_ < 0)
        return;
    if (::kill(child_pid_, SIGTERM) == 0 || errno == ESRCH) {
        if (wait_for_exit(config_.shutdown_timeout))
            return;
    }
    if (::kill(child_pid_, SIGKILL) == 0 || errno == ESRCH) {
        ++counters_.forced_kills;
        reap_blocking();
    } else {
        // We still own the pid.  A blocking reap is the only safe final
        // action; it prevents returning a zombie to the caller.
        reap_blocking();
    }
}

void Supervisor::shutdown() noexcept {
    if (child_pid_ < 0 && !has_private_fds()) {
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

    const pid_t pid = ::fork();
    if (pid == 0) {
        // The ready write end is intentionally the sole inherited supervisor
        // descriptor.  The exec-status write end remains CLOEXEC: EOF means
        // execve succeeded, while a short errno record means it failed.
        ::close(ready_pipe[0]);
        ::close(exec_pipe[0]);
        (void)set_cloexec(ready_pipe[1], false);
        ::execve(config_.executable.c_str(), argv.data(), environment.data());
        const int error = errno;
        (void)::write(exec_pipe[1], &error, sizeof(error));
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

    child_pid_ = pid;
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
                int error = 0;
                const ssize_t bytes = ::read(exec_read_, &error, sizeof(error));
                if (bytes == static_cast<ssize_t>(sizeof(error))) {
                    classify(Failure::Exec);
                    reap_blocking();
                    return false;
                }
                if (bytes == 0)
                    exec_succeeded = true;
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
                    if (ready == kReadyMessage) {
                        close_if_open(ready_read_);
                        close_if_open(exec_read_);
                        state_ = State::Ready;
                        last_failure_ = Failure::None;
                        return true;
                    }
                } else if (count == 0) {
                    classify(Failure::PreReadyExit);
                    reap_blocking();
                    return false;
                }
            }
        }
        int status = 0;
        const pid_t result = ::waitpid(child_pid_, &status, WNOHANG);
        if (result == child_pid_) {
            child_pid_ = -1;
            if (!exec_succeeded)
                classify(Failure::PreReadyExit);
            else
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
    for (;;) {
        if (launch_and_wait(restart))
            return true;
        if (state_ == State::DegradedLegacy)
            return false;
        restart = true;
    }
}

bool Supervisor::restart_after_failure() noexcept {
    for (;;) {
        if (launch_and_wait(true))
            return true;
        if (state_ == State::DegradedLegacy)
            return false;
    }
}

bool Supervisor::poll() noexcept {
    if (state_ != State::Ready || child_pid_ < 0)
        return false;
    int status = 0;
    const pid_t result = ::waitpid(child_pid_, &status, WNOHANG);
    if (result == 0)
        return true;
    if (result < 0 && errno == EINTR)
        return true;
    if (result < 0 && errno != ECHILD)
        return false;
    child_pid_ = -1;
    close_pipes();
    classify(Failure::PostReadyExit);
    return restart_after_failure();
}

} // namespace icecc::p50::sidecar
