#pragma once

// Main-loop-owned daemon integration for the Protocol-50 cache sidecar.
//
// The adapter is intentionally the only owner of an incarnation's
// Supervisor, control Dispatcher, and READY advertisement Controller.  The
// daemon remains the owner of its public listener and reports that observation
// through observe_public_listener(); this class never opens a public socket.

#include "p50_daemon_cache_dispatch.h"
#include "p50_input_fd_attachment.h"
#include "p50_local_transport.h"
#include "p50_ready_advertisement.h"
#include "p50_sidecar_supervisor.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>

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
    InputLifecycleCapacity,
    InputLifecycleOperationExhausted,
    InputLifecycleProtocol,
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
    std::chrono::milliseconds input_attachment_timeout{5000};
    std::chrono::milliseconds input_lifecycle_timeout{250};
    std::chrono::milliseconds shutdown_timeout{1000};
    std::chrono::milliseconds restart_window{10000};
    uint32_t max_restarts = 3;
    uint32_t max_attempts_per_recovery = 16;
    size_t max_pending_input_lifecycle = 4096;
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
    // filled with ordered advertisement transitions from this call.  The
    // boolean is true exactly when the resulting advertisement is present.
    bool start(advertisement::Update* update = nullptr) noexcept;
    bool poll(advertisement::Update* update = nullptr) noexcept;
    void shutdown(advertisement::Update* update = nullptr) noexcept;

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

    // Opens a fresh authenticated control relationship for one exact committed
    // InputRecord.  It neither consumes nor replaces the dispatcher relationship
    // used by CACHE_SESSION.  Every failure is descriptor-less and fail-closed.
    [[nodiscard]] InputFdAttachmentResult attach_input(
        InputRecordKey key, InputLeaseOwner owner,
        uint64_t request_id) noexcept;

    // Attempt teardown and terminal logical-job settlement are deliberately
    // distinct.  A failed operation is retained in the adapter's bounded retry
    // queue; replacement of the named sidecar incarnation reclaims it without
    // sending a stale command to the new store.
    [[nodiscard]] InputLifecycleResult apply_input_lifecycle(
        const InputFdRequest& lease,
        InputLifecycleAction action) noexcept;
    [[nodiscard]] size_t pending_input_lifecycle_count() const noexcept {
        return pending_input_lifecycle_.size();
    }

#if defined(ICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS)
    // Compile-time-only fault injection for otherwise unreachable uint64_t
    // boundaries.  Production objects are built without this macro.
    void test_force_attempt(uint64_t value) noexcept { attempt_ = value; }
    void test_force_counter_state(uint64_t cumulative, uint64_t prior,
                                  bool prior_observed) noexcept {
        cumulative_post_ready_exits_ = cumulative;
        prior_supervisor_post_ready_exits_ = prior;
        prior_counter_observed_ = prior_observed;
    }
    void test_force_input_lifecycle_operation(uint64_t value) noexcept {
        next_input_lifecycle_operation_id_ = value;
    }
#endif

private:
    bool begin_attempt(bool recovery) noexcept;
    bool attach_current() noexcept;
    bool recover(advertisement::Update& update) noexcept;
    bool collect_counter_delta() noexcept;
    bool reserve_outer_restart() noexcept;
    bool next_attempt() noexcept;
    bool next_input_lifecycle_operation(uint64_t& operation_id) noexcept;
    [[nodiscard]] bool drain_input_lifecycle() noexcept;
    [[nodiscard]] bool remember_completed_input_lifecycle(
        const InputLifecycleRequest& request) noexcept;
    void retire_input_lifecycle_relationship(AdapterError error) noexcept;
    bool make_attempt_node() noexcept;
    bool capture_socket_node() noexcept;
    bool runtime_nodes_valid() const noexcept;
    void cleanup_attempt_node() noexcept;
    void disable_relationship() noexcept;
    void apply_observation(advertisement::Update& update) noexcept;
    static void append_update(advertisement::Update& destination,
                              const advertisement::Update& source) noexcept;
    void append_pending_advertisement_update(
        advertisement::Update& destination) noexcept;
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
    dev_t attempt_directory_device_ = 0;
    ino_t attempt_directory_inode_ = 0;
    dev_t socket_device_ = 0;
    ino_t socket_inode_ = 0;
    uint64_t attempt_ = 0;
    uint64_t next_input_lifecycle_operation_id_ = 1;
    uint64_t cumulative_post_ready_exits_ = 0;
    uint64_t prior_supervisor_post_ready_exits_ = 0;
    bool prior_counter_observed_ = false;
    bool counter_failed_ = false;
    std::vector<std::chrono::steady_clock::time_point> restart_times_;
    std::vector<InputLifecycleRequest> pending_input_lifecycle_;
    std::vector<InputLifecycleRequest> completed_input_lifecycle_;
    advertisement::Update pending_advertisement_update_{};
};

} // namespace icecc::p50::daemon
