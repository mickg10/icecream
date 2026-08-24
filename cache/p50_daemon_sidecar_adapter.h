#pragma once

// Main-loop-owned daemon integration for the Protocol-50 cache sidecar.
//
// The adapter is intentionally the only owner of an incarnation's
// Supervisor, control Dispatcher, and READY advertisement Controller.  The
// daemon remains the owner of its public listener and reports that observation
// through observe_public_listener(); this class never opens a public socket.

#include "p50_daemon_cache_dispatch.h"
#include "p50_local_transport.h"
#include "p50_ready_advertisement.h"
#include "p50_sidecar_supervisor.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace icecc::p50::daemon {

enum class AdapterState : uint8_t {
    Stopped = 0,
    Starting,
    Ready,
    Absent,
    Failed,
    ShuttingDown,
};

enum class AdapterError : uint8_t {
    None = 0,
    InvalidConfiguration,
    AttemptExhausted,
    CounterRegression,
    CounterSaturated,
    AttemptOverflow,
    StalePath,
    RuntimeNodeFailure,
    StartupFailure,
    AuthenticationFailure,
    ListenerFailure,
    ShutdownFailure,
};

struct Config {
    std::string executable;
    std::string runtime_directory;
    uint64_t generation = 0;

    // The daemon identity is passed to the service as --peer-uid/gid and is
    // also the required owner/group of runtime_directory.
    uint64_t expected_daemon_uid = 0;
    uint64_t expected_daemon_gid = 0;
    // The credentials observed on the authenticated AF_UNIX connection must
    // match these values and the supervised child PID exactly.
    uint64_t expected_service_uid = 0;
    uint64_t expected_service_gid = 0;

    std::optional<uint64_t> drop_uid;
    std::optional<uint64_t> drop_gid;

    // The adapter does not own this listener.  A present advertisement is
    // emitted only when observe_public_listener() reports this exact port.
    uint32_t public_listener_port = 0;

    std::chrono::milliseconds readiness_timeout{1000};
    std::chrono::milliseconds connect_timeout{1000};
    std::chrono::milliseconds handoff_timeout{250};
    std::chrono::milliseconds shutdown_timeout{1000};
    std::chrono::milliseconds restart_window{10000};
    uint32_t max_restarts = 3;
    uint32_t max_attempts_per_recovery = 16;
};

struct PublicListenerObservation {
    bool bound = false;
    uint32_t port = 0;
};

class DaemonSidecarAdapter {
public:
    using Config = daemon::Config;

    explicit DaemonSidecarAdapter(Config config) noexcept;
    ~DaemonSidecarAdapter();

    DaemonSidecarAdapter(const DaemonSidecarAdapter&) = delete;
    DaemonSidecarAdapter& operator=(const DaemonSidecarAdapter&) = delete;

    static bool valid_config(const Config& config) noexcept;

    // Starts/recoveries are bounded by max_attempts_per_recovery and the
    // adapter-owned rolling restart budget.  `update`, when supplied, is
    // filled with ordered advertisement transitions from this call.
    bool start(advertisement::Update* update = nullptr) noexcept;
    bool poll(advertisement::Update* update = nullptr) noexcept;
    void shutdown() noexcept;

    // The daemon supplies an observation of its already-bound public socket.
    // No descriptor ownership crosses this boundary.
    void observe_public_listener(bool bound, uint32_t port) noexcept;
    void observe_public_listener(PublicListenerObservation observation) noexcept;

    [[nodiscard]] AdapterState state() const noexcept { return state_; }
    [[nodiscard]] AdapterError last_error() const noexcept { return last_error_; }
    [[nodiscard]] const std::string& socket_path() const noexcept { return socket_path_; }
    [[nodiscard]] uint64_t cumulative_post_ready_exits() const noexcept {
        return cumulative_post_ready_exits_;
    }
    [[nodiscard]] uint64_t attempt() const noexcept { return attempt_; }
    [[nodiscard]] bool authenticated() const noexcept;
    [[nodiscard]] advertisement::Snapshot advertisement_snapshot() const noexcept {
        return controller_.snapshot();
    }
    [[nodiscard]] CacheSessionDispatcher* dispatcher() noexcept { return dispatcher_.get(); }
    [[nodiscard]] const CacheSessionDispatcher* dispatcher() const noexcept {
        return dispatcher_.get();
    }
    [[nodiscard]] sidecar::Supervisor* supervisor() noexcept { return supervisor_.get(); }
    [[nodiscard]] const sidecar::Supervisor* supervisor() const noexcept {
        return supervisor_.get();
    }

private:
    bool begin_attempt(bool recovery) noexcept;
    bool attach_current() noexcept;
    bool recover(advertisement::Update& update) noexcept;
    bool collect_counter_delta() noexcept;
    bool reserve_outer_restart() noexcept;
    bool next_attempt() noexcept;
    bool make_attempt_node() noexcept;
    void cleanup_attempt_node() noexcept;
    void disable_relationship() noexcept;
    void apply_observation(advertisement::Update& update) noexcept;
    static void append_update(advertisement::Update& destination,
                              const advertisement::Update& source) noexcept;
    void fail(AdapterError error) noexcept;

    Config config_;
    AdapterState state_ = AdapterState::Stopped;
    AdapterError last_error_ = AdapterError::None;
    PublicListenerObservation public_listener_{};
    std::unique_ptr<sidecar::Supervisor> supervisor_;
    std::unique_ptr<CacheSessionDispatcher> dispatcher_;
    advertisement::Controller controller_;
    std::string socket_path_;
    std::string attempt_directory_;
    uint64_t attempt_ = 0;
    uint64_t cumulative_post_ready_exits_ = 0;
    uint64_t prior_supervisor_post_ready_exits_ = 0;
    bool prior_counter_observed_ = false;
    bool counter_failed_ = false;
    std::vector<std::chrono::steady_clock::time_point> restart_times_;
};

} // namespace icecc::p50::daemon
