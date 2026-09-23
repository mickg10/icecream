#pragma once

// Synchronous compatibility supervisor for the Protocol-50 cache sidecar.
// Production lifecycle and shared identity contracts live in separate units.

#include "cache/p50_sidecar_identity.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace icecc::p50::sidecar {

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
    std::string executable;
    std::vector<std::string> arguments;
    std::chrono::milliseconds readiness_timeout{1000};
    std::chrono::milliseconds shutdown_timeout{1000};
    std::chrono::milliseconds restart_window{10000};
    uint32_t max_restarts = 3;
    uint32_t max_attempts_per_recovery = 16;
    std::string lease_root;
    std::shared_ptr<LaunchIdentityAllocator> launch_identities;
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

    static bool valid_config(const Config& config) noexcept;
    bool start() noexcept;
    bool poll() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] Failure last_failure() const noexcept { return last_failure_; }
    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }
    [[nodiscard]] pid_t child_pid() const noexcept { return child_pid_; }
    [[nodiscard]] pid_t process_group_id() const noexcept {
        return process_group_owned_ ? process_group_ : -1;
    }
    [[nodiscard]] bool has_private_fds() const noexcept;
    [[nodiscard]] const std::optional<ReadyLease>& current_lease() const noexcept {
        return current_lease_;
    }

private:
    bool launch_and_wait(bool restart) noexcept;
    bool wait_for_ready() noexcept;
    bool restart_after_failure() noexcept;
    bool reserve_restart() noexcept;
    void classify(Failure failure) noexcept;
    void close_pipes() noexcept;
    void reap_blocking() noexcept;
    bool child_has_exited_exact(pid_t expected_child) const noexcept;
    bool terminate_child() noexcept;
    bool terminate_group() noexcept;
    bool prepare_lease() noexcept;
    void cleanup_lease(std::optional<ReadyLease>& lease) noexcept;

    Config config_;
    State state_ = State::Stopped;
    Failure last_failure_ = Failure::None;
    Counters counters_{};
    pid_t child_pid_ = -1;
    int child_pidfd_ = -1;
    pid_t process_group_ = -1;
    bool process_group_owned_ = false;
    int ready_read_ = -1;
    int exec_read_ = -1;
    std::vector<std::chrono::steady_clock::time_point> restart_times_;
    std::optional<ReadyLease> pending_lease_;
    std::optional<ReadyLease> current_lease_;
    int pending_listener_fd_ = -1;
};

} // namespace icecc::p50::sidecar
