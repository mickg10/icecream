#pragma once

// Lifecycle-only supervisor for the future Protocol-50 cache sidecar.
//
// This component deliberately has no daemon, socket, scheduler, or
// advertisement dependency.  It owns only the child process and its private
// readiness/exec-status pipes.  A later daemon adapter can consume the
// deterministic state and counters without changing this lifecycle policy.

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <sys/types.h>

namespace icecc::p50::sidecar {

// The supervisor gives the child this inherited descriptor through the
// environment.  The descriptor itself is private, CLOEXEC in the parent, and
// the only descriptor deliberately cleared in the pre-exec child.
inline constexpr std::string_view kReadyFdEnvironment =
    "ICECC_CACHE_SERVICE_READY_FD";

enum class State : uint8_t {
    Stopped = 0,
    Starting,
    Ready,
    DegradedLegacy,
    Stopping,
};

enum class Failure : uint8_t {
    None = 0,
    InvalidConfiguration,
    Exec,
    ReadinessTimeout,
    PreReadyExit,
    InvalidReady,
    PostReadyExit,
    RestartExhausted,
};

struct Config {
    // An absolute executable path is required.  Arguments are passed directly
    // to execve; no shell is involved.  The executable itself is argv[0].
    std::string executable;
    std::vector<std::string> arguments;

    std::chrono::milliseconds readiness_timeout{1000};
    std::chrono::milliseconds shutdown_timeout{1000};
    std::chrono::milliseconds restart_window{10000};
    uint32_t max_restarts = 3;
    // Hard cap for one synchronous start/recovery call.  This complements
    // the time-window budget when each failed attempt itself spans a window.
    uint32_t max_attempts_per_recovery = 16;
};

struct Counters {
    uint64_t launches = 0;
    uint64_t restarts = 0;
    uint64_t exec_failures = 0;
    uint64_t readiness_timeouts = 0;
    uint64_t pre_ready_exits = 0;
    uint64_t invalid_ready_messages = 0;
    uint64_t post_ready_exits = 0;
    uint64_t shutdowns = 0;
    uint64_t forced_kills = 0;
};

const char* state_name(State state) noexcept;
const char* failure_name(Failure failure) noexcept;

class Supervisor {
public:
    explicit Supervisor(Config config);
    ~Supervisor();

    Supervisor(const Supervisor&) = delete;
    Supervisor& operator=(const Supervisor&) = delete;
    Supervisor(Supervisor&&) = delete;
    Supervisor& operator=(Supervisor&&) = delete;

    // Validates the command before any process or pipe is created.
    static bool valid_config(const Config& config) noexcept;

    // Starts the service and waits for one exact READY\n message.  Before
    // readiness failures consume the same bounded restart budget as later
    // crashes.  On exhaustion the supervisor enters terminal
    // DegradedLegacy and returns false.
    bool start() noexcept;

    // Reaps a ready child if it has exited.  A post-ready exit is classified
    // and restarted within the fixed monotonic window, or transitions to
    // DegradedLegacy.  Returns true only while the child is ready.
    bool poll() noexcept;

    // TERM, bounded wait, KILL if needed, and mandatory reap.  It is safe to
    // call repeatedly and closes every supervisor-owned descriptor.
    void shutdown() noexcept;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] Failure last_failure() const noexcept { return last_failure_; }
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
    [[nodiscard]] pid_t child_pid() const noexcept { return child_pid_; }
    [[nodiscard]] bool has_private_fds() const noexcept;

private:
    bool launch_and_wait(bool restart) noexcept;
    bool wait_for_ready() noexcept;
    bool restart_after_failure() noexcept;
    bool reserve_restart() noexcept;
    void classify(Failure failure) noexcept;
    void close_pipes() noexcept;
    void reap_blocking() noexcept;
    bool wait_for_exit(std::chrono::milliseconds timeout) noexcept;
    void terminate_child() noexcept;
    void terminate_group() noexcept;

    Config config_;
    State state_ = State::Stopped;
    Failure last_failure_ = Failure::None;
    Counters counters_{};
    pid_t child_pid_ = -1;
    pid_t process_group_ = -1;
    int ready_read_ = -1;
    int exec_read_ = -1;
    std::vector<std::chrono::steady_clock::time_point> restart_times_;
};

} // namespace icecc::p50::sidecar
