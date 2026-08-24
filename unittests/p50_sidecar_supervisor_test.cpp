#include "../cache/p50_sidecar_supervisor.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace icecc::p50::sidecar;

namespace {

void check(bool condition, const char* expression) {
    if (!condition)
        throw std::runtime_error(expression);
}

#define CHECK(expression) check((expression), #expression)

bool write_all(int fd, const char* bytes, size_t size) {
    size_t written = 0;
    while (written != size) {
        const ssize_t result = ::write(fd, bytes + written, size - written);
        if (result > 0) {
            written += static_cast<size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

int ready_fd() {
    const char* value = std::getenv(kReadyFdEnvironment.data());
    return value == nullptr ? -1 : std::atoi(value);
}

int fake_child(const char* mode) {
    const int fd = ready_fd();
    if (fd < 0)
        return 90;
    if (std::string(mode) == "timeout" || std::string(mode) == "pre-exit")
        return std::string(mode) == "pre-exit" ? 23 : pause();
    if (std::string(mode) == "partial") {
        (void)write_all(fd, "READY", 5);
        ::close(fd);
        return 0;
    }
    if (std::string(mode) == "extra") {
        (void)write_all(fd, "READY\nX", 7);
        ::close(fd);
        return 0;
    }
    if (std::string(mode) == "delayed-extra") {
        (void)write_all(fd, "READY\n", 6);
        ::usleep(30000);
        (void)write_all(fd, "X", 1);
        ::close(fd);
        return 0;
    }
    if (std::string(mode) == "invalid") {
        (void)write_all(fd, "NOPE", 4);
        ::close(fd);
        return 0;
    }
    if (std::string(mode) == "no-close") {
        (void)write_all(fd, "READY\n", 6);
        return pause();
    }
    if (std::string(mode) == "close-empty-live") {
        // Regression for the READY-EOF path: EOF does not imply that the
        // service exited, so the supervisor must never perform a blocking
        // reap here.  Ignore TERM to make teardown exercise its bounded
        // TERM-to-KILL path after rejecting the empty message.
        (void)::signal(SIGTERM, SIG_IGN);
        ::close(fd);
        return pause();
    }
    if (std::string(mode) == "pre-ready-grandchild") {
        // Exit before READY after forking a TERM-ignoring helper.  The parent
        // supervisor must retain and tear down the owned PGID even though the
        // direct child is reaped by the readiness state machine.
        const pid_t helper = ::fork();
        if (helper < 0)
            return 96;
        if (helper == 0) {
            ::close(fd);
            (void)::signal(SIGTERM, SIG_IGN);
            (void)::pause();
            _exit(0);
        }
        const char* pid_path = std::getenv("ICECC_GRANDCHILD_PID_FILE");
        if (pid_path == nullptr)
            return 97;
        const int pid_file = ::open(pid_path, O_WRONLY | O_APPEND);
        if (pid_file < 0)
            return 98;
        char pid_text[32]{ };
        const int pid_size = std::snprintf(pid_text, sizeof(pid_text), "%ld\n",
                                           static_cast<long>(helper));
        const bool written = pid_size > 0 &&
                             write_all(pid_file, pid_text, static_cast<size_t>(pid_size));
        ::close(pid_file);
        return written ? 0 : 99;
    }
    if (std::string(mode) == "move-group") {
        const char* helper_path = std::getenv("ICECC_MOVED_HELPER_PID_FILE");
        if (helper_path == nullptr)
            return 102;
        const pid_t helper = ::fork();
        if (helper < 0)
            return 103;
        if (helper == 0) {
            ::close(fd);
            (void)::signal(SIGTERM, SIG_IGN);
            (void)::pause();
            _exit(0);
        }
        const int helper_file = ::open(helper_path, O_WRONLY | O_TRUNC);
        if (helper_file < 0)
            return 104;
        char helper_text[32]{ };
        const int helper_size = std::snprintf(helper_text, sizeof(helper_text), "%ld\n",
                                              static_cast<long>(helper));
        const bool helper_written = helper_size > 0 &&
                                     write_all(helper_file, helper_text,
                                               static_cast<size_t>(helper_size));
        ::close(helper_file);
        if (!helper_written)
            return 105;
        const char* group_text = std::getenv("ICECC_MOVE_TO_PGID");
        if (group_text == nullptr)
            return 106;
        const long group = std::strtol(group_text, nullptr, 10);
        if (group <= 1 || ::setpgid(0, static_cast<pid_t>(group)) != 0)
            return 107;
    }
    if (std::string(mode) == "sentinel") {
        const char* sentinel = std::getenv("ICECC_SENTINEL_FD");
        if (sentinel != nullptr) {
            char proc_path[64]{};
            std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%s", sentinel);
            if (::access(proc_path, F_OK) == 0)
                return 91;
        }
    }
    if (std::string(mode) == "grandchild") {
        const pid_t helper = ::fork();
        if (helper < 0)
            return 93;
        if (helper == 0) {
            ::close(fd);
            (void)::signal(SIGTERM, SIG_IGN);
            (void)::pause();
            _exit(0);
        }
        const char* pid_path = std::getenv("ICECC_GRANDCHILD_PID_FILE");
        if (pid_path == nullptr)
            return 94;
        const int pid_file = ::open(pid_path, O_WRONLY | O_TRUNC);
        if (pid_file < 0)
            return 95;
        char pid_text[32]{};
        const int pid_size = std::snprintf(pid_text, sizeof(pid_text), "%ld\n",
                                           static_cast<long>(helper));
        (void)write_all(pid_file, pid_text, static_cast<size_t>(pid_size));
        ::close(pid_file);
    }
    const bool ignores_term = std::string(mode) == "ready-live" ||
                              std::string(mode) == "grandchild" ||
                              std::string(mode) == "move-group";
    if (ignores_term)
        (void)::signal(SIGTERM, SIG_IGN);
    if (!write_all(fd, "READY\n", 6))
        return 92;
    ::close(fd);
    if (std::string(mode) == "ready-exit" || std::string(mode) == "sentinel")
        return std::string(mode) == "ready-exit" ? 0 : pause();
    // Keep the process and its process group alive so shutdown tests exercise
    // TERM->KILL and descendant ownership without a shell child.
    (void)::signal(SIGTERM, SIG_IGN);
    return pause();
}

Config fake_config(const char* mode, uint32_t max_restarts = 0) {
    Config config;
    config.executable = "/proc/self/exe";
    config.arguments = {"--fake-child", mode};
    config.readiness_timeout = std::chrono::milliseconds(150);
    config.shutdown_timeout = std::chrono::milliseconds(60);
    config.restart_window = std::chrono::milliseconds(500);
    config.max_restarts = max_restarts;
    return config;
}

void validation_and_exec_failure() {
    Config invalid;
    CHECK(!Supervisor::valid_config(invalid));
    invalid.executable = "relative-service";
    CHECK(!Supervisor::valid_config(invalid));
    invalid.executable = "/bin/sh";
    invalid.restart_window = std::chrono::milliseconds(0);
    CHECK(!Supervisor::valid_config(invalid));
    invalid.restart_window = std::chrono::milliseconds(1);
    invalid.max_attempts_per_recovery = 0;
    CHECK(!Supervisor::valid_config(invalid));
    invalid.max_attempts_per_recovery = 16;
    invalid.max_restarts = std::numeric_limits<uint32_t>::max();
    CHECK(!Supervisor::valid_config(invalid));

    // This is a regular executable, so configuration validation succeeds;
    // execve then reports ENOENT for its deliberately missing interpreter.
    char path[] = "/tmp/icecc-sidecar-exec-XXXXXX";
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0);
    const char script[] = "#!/icecc-interpreter-that-does-not-exist\nexit 0\n";
    CHECK(::write(fd, script, sizeof(script) - 1) == static_cast<ssize_t>(sizeof(script) - 1));
    CHECK(::fchmod(fd, 0700) == 0);
    CHECK(::close(fd) == 0);

    Config config = fake_config("pre-exit");
    config.executable = path;
    config.readiness_timeout = std::chrono::milliseconds(100);
    Supervisor supervisor(config);
    CHECK(Supervisor::valid_config(config));
    CHECK(!supervisor.start());
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().exec_failures >= 1);
    CHECK(supervisor.counters().launches == 1);
    CHECK(!supervisor.has_private_fds());
    CHECK(::unlink(path) == 0);
}

void ready_and_shutdown() {
    Supervisor supervisor(fake_config("ready-live"));
    CHECK(supervisor.start());
    CHECK(supervisor.state() == State::Ready);
    CHECK(supervisor.last_failure() == Failure::None);
    CHECK(supervisor.counters().launches == 1);
    CHECK(supervisor.child_pid() > 0);
    CHECK(supervisor.process_group_id() == supervisor.child_pid());
    CHECK(!supervisor.has_private_fds());
    const pid_t pid = supervisor.child_pid();
    supervisor.shutdown();
    CHECK(supervisor.state() == State::Stopped);
    CHECK(supervisor.child_pid() < 0);
    CHECK(supervisor.process_group_id() < 0);
    CHECK(supervisor.counters().shutdowns == 1);
    CHECK(supervisor.counters().forced_kills == 1);
    int status = 0;
    CHECK(::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
    supervisor.shutdown();
    CHECK(supervisor.counters().shutdowns == 1);
}

void timeout_and_pre_ready_exit_are_distinct() {
    Supervisor timeout(fake_config("timeout"));
    CHECK(!timeout.start());
    CHECK(timeout.state() == State::DegradedLegacy);
    CHECK(timeout.last_failure() == Failure::RestartExhausted);
    CHECK(timeout.counters().readiness_timeouts >= 1);
    CHECK(timeout.counters().pre_ready_exits == 0);
    CHECK(!timeout.has_private_fds());

    Supervisor crash(fake_config("pre-exit"));
    CHECK(!crash.start());
    CHECK(crash.state() == State::DegradedLegacy);
    CHECK(crash.counters().pre_ready_exits + crash.counters().invalid_ready_messages >= 1);
    CHECK(crash.counters().readiness_timeouts == 0);
}

void repeated_post_ready_crashes_exhaust_budget() {
    Supervisor supervisor(fake_config("ready-exit", 2));
    CHECK(supervisor.start());
    CHECK(supervisor.state() == State::Ready);
    bool exhausted = false;
    for (int attempt = 0; attempt != 20 && !exhausted; ++attempt) {
        exhausted = !supervisor.poll();
        if (!exhausted)
            ::usleep(10000);
    }
    CHECK(exhausted);
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.last_failure() == Failure::RestartExhausted);
    CHECK(supervisor.counters().post_ready_exits >= 1);
    CHECK(supervisor.counters().restarts >= 2);
    CHECK(supervisor.counters().launches == 3);
    CHECK(!supervisor.has_private_fds());
}

void invalid_ready_is_bounded() {
    Supervisor supervisor(fake_config("invalid"));
    CHECK(!supervisor.start());
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().invalid_ready_messages + supervisor.counters().pre_ready_exits >= 1);
    CHECK(!supervisor.has_private_fds());
}

void exact_ready_close_protocol() {
    for (const char* mode : {"partial", "extra", "delayed-extra", "invalid"}) {
        Supervisor supervisor(fake_config(mode));
        CHECK(!supervisor.start());
        CHECK(supervisor.state() == State::DegradedLegacy);
        CHECK(supervisor.counters().invalid_ready_messages + supervisor.counters().pre_ready_exits >= 1);
        CHECK(!supervisor.has_private_fds());
        CHECK(supervisor.child_pid() < 0);
    }

    Supervisor no_close(fake_config("no-close"));
    CHECK(!no_close.start());
    CHECK(no_close.state() == State::DegradedLegacy);
    CHECK(no_close.counters().readiness_timeouts >= 1);
    CHECK(!no_close.has_private_fds());
    CHECK(no_close.child_pid() < 0);
}

void empty_ready_eof_from_live_child_is_bounded() {
    Supervisor supervisor(fake_config("close-empty-live"));
    const auto started = std::chrono::steady_clock::now();
    // A regression to blocking waitpid() would otherwise hang the complete
    // test executable rather than produce a useful failing row.
    (void)::alarm(5);
    CHECK(!supervisor.start());
    (void)::alarm(0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(2));
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().invalid_ready_messages >= 1);
    CHECK(supervisor.counters().forced_kills == 1);
    CHECK(!supervisor.has_private_fds());
    CHECK(supervisor.child_pid() < 0);
    CHECK(supervisor.process_group_id() < 0);
}

void pre_ready_group_is_reaped_across_retries() {
    char pid_path[] = "/tmp/icecc-sidecar-pre-ready-helper-XXXXXX";
    const int path_fd = ::mkstemp(pid_path);
    CHECK(path_fd >= 0);
    CHECK(::close(path_fd) == 0);
    CHECK(::setenv("ICECC_GRANDCHILD_PID_FILE", pid_path, 1) == 0);

    Config config = fake_config("pre-ready-grandchild", 10);
    config.readiness_timeout = std::chrono::milliseconds(100);
    config.shutdown_timeout = std::chrono::milliseconds(40);
    config.max_attempts_per_recovery = 2;
    Supervisor supervisor(config);
    (void)::alarm(5);
    CHECK(!supervisor.start());
    (void)::alarm(0);
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().launches == 2);
    CHECK(supervisor.counters().forced_kills >= 2);
    CHECK(supervisor.process_group_id() < 0);
    CHECK(supervisor.child_pid() < 0);
    CHECK(!supervisor.has_private_fds());
    CHECK(::unsetenv("ICECC_GRANDCHILD_PID_FILE") == 0);

    std::vector<pid_t> helpers;
    for (int attempt = 0; attempt != 50 && helpers.size() < 2; ++attempt) {
        const int fd = ::open(pid_path, O_RDONLY);
        if (fd >= 0) {
            char text[128]{};
            const ssize_t bytes = ::read(fd, text, sizeof(text) - 1);
            ::close(fd);
            if (bytes > 0) {
                char* cursor = text;
                char* end = text + bytes;
                while (cursor < end) {
                    char* next = nullptr;
                    const long value = std::strtol(cursor, &next, 10);
                    if (next == cursor)
                        break;
                    if (value > 1 && value <= std::numeric_limits<pid_t>::max()) {
                        const pid_t helper = static_cast<pid_t>(value);
                        if (std::find(helpers.begin(), helpers.end(), helper) == helpers.end())
                            helpers.push_back(helper);
                    }
                    cursor = next;
                }
            }
        }
        if (helpers.size() < 2)
            ::usleep(5000);
    }
    CHECK(helpers.size() == 2);
    for (const pid_t helper : helpers) {
        bool gone = false;
        for (int attempt = 0; attempt != 100 && !gone; ++attempt) {
            errno = 0;
            gone = ::kill(helper, 0) < 0 && errno == ESRCH;
            if (!gone)
                ::usleep(5000);
        }
        CHECK(gone);
    }
    CHECK(::unlink(pid_path) == 0);
}

void ambient_fd_is_not_inherited() {
    char path[] = "/tmp/icecc-sidecar-sentinel-XXXXXX";
    const int sentinel_fd = ::mkstemp(path);
    CHECK(sentinel_fd >= 0);
    const int flags = ::fcntl(sentinel_fd, F_GETFD);
    CHECK(flags >= 0);
    CHECK(::fcntl(sentinel_fd, F_SETFD, flags & ~FD_CLOEXEC) == 0);
    const std::string number = std::to_string(sentinel_fd);
    CHECK(::setenv("ICECC_SENTINEL_FD", number.c_str(), 1) == 0);
    Supervisor supervisor(fake_config("sentinel"));
    CHECK(supervisor.start());
    CHECK(supervisor.state() == State::Ready);
    CHECK(::unsetenv("ICECC_SENTINEL_FD") == 0);
    CHECK(::fcntl(sentinel_fd, F_GETFD) >= 0);
    supervisor.shutdown();
    CHECK(!supervisor.has_private_fds());
    CHECK(supervisor.child_pid() < 0);
    CHECK(::close(sentinel_fd) == 0);
    CHECK(::unlink(path) == 0);
}

#if defined(ICECC_P50_FORCE_FD_FALLBACK)
void forced_fd_fallback_is_bounded_and_excludes_ambient_fd() {
    const auto started = std::chrono::steady_clock::now();
    (void)::alarm(3);
    ambient_fd_is_not_inherited();
    (void)::alarm(0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

void forced_fd_fallback_handles_high_ambient_fd() {
    char path[] = "/tmp/icecc-sidecar-high-sentinel-XXXXXX";
    const int base_fd = ::mkstemp(path);
    CHECK(base_fd >= 0);
    const int high_fd = ::fcntl(base_fd, F_DUPFD, 9000);
    CHECK(high_fd >= 9000);
    CHECK(::close(base_fd) == 0);
    const int flags = ::fcntl(high_fd, F_GETFD);
    CHECK(flags >= 0);
    CHECK(::fcntl(high_fd, F_SETFD, flags & ~FD_CLOEXEC) == 0);
    const std::string number = std::to_string(high_fd);
    CHECK(::setenv("ICECC_SENTINEL_FD", number.c_str(), 1) == 0);
    Supervisor supervisor(fake_config("sentinel"));
    CHECK(supervisor.start());
    CHECK(::unsetenv("ICECC_SENTINEL_FD") == 0);
    supervisor.shutdown();
    CHECK(::close(high_fd) == 0);
    CHECK(::unlink(path) == 0);
}
#endif

void moved_child_refuses_stale_group_signal() {
    char helper_path[] = "/tmp/icecc-sidecar-moved-helper-XXXXXX";
    const int helper_file = ::mkstemp(helper_path);
    CHECK(helper_file >= 0);
    CHECK(::close(helper_file) == 0);
    const pid_t sentinel = ::fork();
    CHECK(sentinel >= 0);
    if (sentinel == 0) {
        (void)::setpgid(0, 0);
        (void)::signal(SIGTERM, SIG_IGN);
        (void)::pause();
        _exit(0);
    }
    CHECK(::setpgid(sentinel, sentinel) == 0 || errno == EACCES);
    const std::string group_text = std::to_string(sentinel);
    CHECK(::setenv("ICECC_MOVE_TO_PGID", group_text.c_str(), 1) == 0);
    CHECK(::setenv("ICECC_MOVED_HELPER_PID_FILE", helper_path, 1) == 0);
    Supervisor supervisor(fake_config("move-group"));
    (void)::alarm(5);
    CHECK(supervisor.start());
    CHECK(supervisor.process_group_id() > 1);
    CHECK(supervisor.process_group_id() != sentinel);
    supervisor.shutdown();
    (void)::alarm(0);
    CHECK(::unsetenv("ICECC_MOVE_TO_PGID") == 0);
    CHECK(::unsetenv("ICECC_MOVED_HELPER_PID_FILE") == 0);

    pid_t helper = -1;
    const int read_file = ::open(helper_path, O_RDONLY);
    CHECK(read_file >= 0);
    char helper_text[32]{};
    const ssize_t helper_bytes = ::read(read_file, helper_text, sizeof(helper_text) - 1);
    CHECK(::close(read_file) == 0);
    CHECK(helper_bytes > 0);
    helper = static_cast<pid_t>(std::strtol(helper_text, nullptr, 10));
    CHECK(helper > 1);
    errno = 0;
    CHECK(::kill(helper, 0) == 0);
    CHECK(::kill(helper, SIGKILL) == 0 || errno == ESRCH);
    bool helper_gone = false;
    for (int attempt = 0; attempt != 100; ++attempt) {
        errno = 0;
        if (::kill(helper, 0) < 0 && errno == ESRCH) {
            helper_gone = true;
            break;
        }
        ::usleep(5000);
    }
    CHECK(helper_gone);
    CHECK(::unlink(helper_path) == 0);

    errno = 0;
    CHECK(::kill(sentinel, 0) == 0);
    CHECK(::kill(sentinel, SIGKILL) == 0 || errno == ESRCH);
    int status = 0;
    while (::waitpid(sentinel, &status, 0) < 0 && errno == EINTR)
        ;
}

void shutdown_owns_process_group() {
    char pid_path[] = "/tmp/icecc-sidecar-helper-XXXXXX";
    const int path_fd = ::mkstemp(pid_path);
    CHECK(path_fd >= 0);
    CHECK(::close(path_fd) == 0);
    CHECK(::setenv("ICECC_GRANDCHILD_PID_FILE", pid_path, 1) == 0);
    Supervisor supervisor(fake_config("grandchild"));
    CHECK(supervisor.start());
    CHECK(supervisor.state() == State::Ready);
    CHECK(::unsetenv("ICECC_GRANDCHILD_PID_FILE") == 0);

    pid_t helper = -1;
    for (int attempt = 0; attempt != 20 && helper < 0; ++attempt) {
        const int fd = ::open(pid_path, O_RDONLY);
        if (fd >= 0) {
            char text[32]{};
            const ssize_t bytes = ::read(fd, text, sizeof(text) - 1);
            ::close(fd);
            if (bytes > 0)
                helper = static_cast<pid_t>(std::strtol(text, nullptr, 10));
        }
        if (helper < 0)
            ::usleep(5000);
    }
    CHECK(helper > 1);
    supervisor.shutdown();
    bool gone = false;
    for (int attempt = 0; attempt != 40 && !gone; ++attempt) {
        if (::kill(helper, 0) < 0 && errno == ESRCH)
            gone = true;
        if (!gone)
            ::usleep(5000);
    }
    CHECK(gone);
    CHECK(supervisor.counters().forced_kills == 1);
    CHECK(!supervisor.has_private_fds());
    CHECK(supervisor.child_pid() < 0);
    CHECK(::unlink(pid_path) == 0);
}

void restart_window_ages_without_unbounded_call() {
    Config config = fake_config("ready-exit", 1);
    config.restart_window = std::chrono::milliseconds(30);
    config.max_attempts_per_recovery = 2;
    Supervisor supervisor(config);
    CHECK(supervisor.start());
    for (int attempt = 0; attempt != 40 && supervisor.counters().restarts < 1; ++attempt) {
        (void)supervisor.poll();
        ::usleep(5000);
    }
    CHECK(supervisor.counters().restarts >= 1);
    ::usleep(60000);
    for (int attempt = 0; attempt != 40 && supervisor.counters().restarts < 2; ++attempt) {
        (void)supervisor.poll();
        ::usleep(5000);
    }
    CHECK(supervisor.counters().restarts >= 2);
    CHECK(supervisor.state() == State::Ready);
    supervisor.shutdown();
}

void bounded_recovery_attempts() {
    Config config = fake_config("timeout", 100);
    config.readiness_timeout = std::chrono::milliseconds(20);
    config.restart_window = std::chrono::milliseconds(1);
    config.max_attempts_per_recovery = 2;
    Supervisor supervisor(config);
    const auto started = std::chrono::steady_clock::now();
    CHECK(!supervisor.start());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(1));
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().launches == 2);
    CHECK(!supervisor.has_private_fds());
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--fake-child")
        return fake_child(argv[2]);
    try {
        validation_and_exec_failure();
        ready_and_shutdown();
        timeout_and_pre_ready_exit_are_distinct();
        repeated_post_ready_crashes_exhaust_budget();
        invalid_ready_is_bounded();
        exact_ready_close_protocol();
        empty_ready_eof_from_live_child_is_bounded();
        pre_ready_group_is_reaped_across_retries();
        ambient_fd_is_not_inherited();
#if defined(ICECC_P50_FORCE_FD_FALLBACK)
        forced_fd_fallback_is_bounded_and_excludes_ambient_fd();
        forced_fd_fallback_handles_high_ambient_fd();
#endif
        moved_child_refuses_stale_group_signal();
        shutdown_owns_process_group();
        restart_window_ages_without_unbounded_call();
        bounded_recovery_attempts();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "p50sidecarsupervisor: %s\n", error.what());
        return EXIT_FAILURE;
    }
    std::puts("p50sidecarsupervisor: ok");
    return EXIT_SUCCESS;
}
