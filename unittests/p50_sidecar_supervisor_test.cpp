#include "../cache/p50_sidecar_supervisor.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace icecc::p50::sidecar;

namespace {

void check(bool condition, const char* expression) {
    if (!condition)
        throw std::runtime_error(expression);
}

#define CHECK(expression) check((expression), #expression)

Config shell_config(const char* body, uint32_t max_restarts = 0) {
    Config config;
    config.executable = "/bin/sh";
    config.arguments = {"-c", body, "icecc-cache-service"};
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

    // This is a regular executable, so configuration validation succeeds;
    // execve then reports ENOENT for its deliberately missing interpreter.
    char path[] = "/tmp/icecc-sidecar-exec-XXXXXX";
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0);
    const char script[] = "#!/icecc-interpreter-that-does-not-exist\nexit 0\n";
    CHECK(::write(fd, script, sizeof(script) - 1) == static_cast<ssize_t>(sizeof(script) - 1));
    CHECK(::fchmod(fd, 0700) == 0);
    CHECK(::close(fd) == 0);

    Config config = shell_config(":");
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
    Supervisor supervisor(shell_config(
        "trap '' TERM; eval \"printf 'READY\\n' >&$ICECC_CACHE_SERVICE_READY_FD\"; while :; do :; done"));
    CHECK(supervisor.start());
    CHECK(supervisor.state() == State::Ready);
    CHECK(supervisor.last_failure() == Failure::None);
    CHECK(supervisor.counters().launches == 1);
    CHECK(supervisor.child_pid() > 0);
    CHECK(!supervisor.has_private_fds());
    const pid_t pid = supervisor.child_pid();
    supervisor.shutdown();
    CHECK(supervisor.state() == State::Stopped);
    CHECK(supervisor.child_pid() < 0);
    CHECK(supervisor.counters().shutdowns == 1);
    CHECK(supervisor.counters().forced_kills == 1);
    int status = 0;
    CHECK(::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
    supervisor.shutdown();
    CHECK(supervisor.counters().shutdowns == 1);
}

void timeout_and_pre_ready_exit_are_distinct() {
    Supervisor timeout(shell_config("while :; do :; done"));
    CHECK(!timeout.start());
    CHECK(timeout.state() == State::DegradedLegacy);
    CHECK(timeout.last_failure() == Failure::RestartExhausted);
    CHECK(timeout.counters().readiness_timeouts >= 1);
    CHECK(timeout.counters().pre_ready_exits == 0);
    CHECK(!timeout.has_private_fds());

    Supervisor crash(shell_config("exit 23"));
    CHECK(!crash.start());
    CHECK(crash.state() == State::DegradedLegacy);
    CHECK(crash.counters().pre_ready_exits >= 1);
    CHECK(crash.counters().readiness_timeouts == 0);
}

void repeated_post_ready_crashes_exhaust_budget() {
    Supervisor supervisor(shell_config(
        "eval \"printf 'READY\\n' >&$ICECC_CACHE_SERVICE_READY_FD\"; exit 0", 2));
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
    CHECK(supervisor.counters().restarts == 2);
    CHECK(supervisor.counters().launches == 3);
    CHECK(!supervisor.has_private_fds());
}

void invalid_ready_is_bounded() {
    Supervisor supervisor(shell_config(
        "eval \"printf 'NOPE' >&$ICECC_CACHE_SERVICE_READY_FD\"; while :; do :; done"));
    CHECK(!supervisor.start());
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().invalid_ready_messages >= 1);
    CHECK(!supervisor.has_private_fds());
}

} // namespace

int main() {
    try {
        validation_and_exec_failure();
        ready_and_shutdown();
        timeout_and_pre_ready_exit_are_distinct();
        repeated_post_ready_crashes_exhaust_budget();
        invalid_ready_is_bounded();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "p50sidecarsupervisor: %s\n", error.what());
        return EXIT_FAILURE;
    }
    std::puts("p50sidecarsupervisor: ok");
    return EXIT_SUCCESS;
}
