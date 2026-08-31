#pragma once

// Main-loop-owned daemon integration for the Protocol-50 cache sidecar.
//
// The adapter is intentionally the only owner of an incarnation's outer
// lifecycle, control relationship, and READY advertisement Controller.  The
// daemon remains the owner of its public listener and reports that observation
// through observe_public_listener(); this class never opens a public socket.

#include "p50_daemon_cache_dispatch.h"
#include "p50_daemon_control.h"
#include "p50_input_fd_attachment.h"
#include "p50_local_transport.h"
#include "p50_ready_advertisement.h"
#include "p50_sidecar_lifecycle.h"
#include "p50_sidecar_supervisor.h"

#include <chrono>
#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <poll.h>
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
    // Zero denotes a local-only adapter (for a submitter daemon with no
    // public worker listener), which can authenticate control handoffs but
    // never projects a scheduler advertisement.
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
    // Established once by the daemon process and inherited by the sidecar;
    // typed cross-process deadlines must carry this exact pair.
    sidecar::MonotonicClockIdentity clock_identity{};

    // Production iceccd supplies its one daemon-owned central reaper.  A null
    // value is retained for standalone adapter tests; the outer-loop path
    // still uses exact pid/PGID identity and never performs a local reap.
    sidecar::CentralChildReaperRegistry* central_reaper = nullptr;
    // The compiler-attempt leaf owner is an independent authority.  The
    // adapter only routes its typed capabilities; it never creates a local
    // InputAttachmentCore or treats caller booleans as cgroup proof.
    std::shared_ptr<sidecar::AttemptLeafAuthority> attempt_leaf_authority;
};

struct PublicListenerObservation {
    bool bound = false;
    uint32_t port = 0;
};

// Proof supplied by the future compiler-attempt reducer before it may ask the
// sidecar owner to install B.  The adapter does not infer any of these facts
// from a timeout or a PID alone; every field is an independent exact-A
// retirement observation.
struct InputRetirementProof {
    RemoteInputLeaseBinding binding{};
    // Final proof from the authoritative attempt-leaf/reaper lane.  A
    // shared immutable handle preserves replay identity while the contained
    // OUTPUT_QUIESCENCE_GRANTED and consumed exact-pid status remain
    // non-forgeable move-only capabilities.
    std::shared_ptr<const sidecar::AttemptLeafRetirementJoin> leaf_join;
    bool direct_worker_status_consumed = false;
    bool process_group_absent = false;
    bool control_closed = false;
    bool input_fd_closed = false;
    bool path_absent = false;
    bool store_generation_current = false;

    [[nodiscard]] bool complete() const noexcept {
        return binding.valid() && leaf_join != nullptr && leaf_join->valid() &&
               leaf_join->matches(binding.identity) && direct_worker_status_consumed &&
               process_group_absent && control_closed && input_fd_closed &&
               path_absent && store_generation_current;
    }
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
    // Historical source compatibility only.  The synchronous Supervisor is
    // no longer instantiated by this adapter; callers must use outer_*().
    [[nodiscard]] sidecar::Supervisor* supervisor() noexcept { return nullptr; }
    [[nodiscard]] const sidecar::Supervisor* supervisor() const noexcept {
        return nullptr;
    }

    // Outer-loop lifecycle seam.  These methods are the only production
    // entry points used by iceccd.  Each call performs at most one reducer
    // turn and at most one launch/TERM/KILL/cleanup/connect/send/receive
    // action; waiting and deadline selection remain in the daemon poll owner.
    bool outer_begin_turn(std::chrono::steady_clock::time_point now,
                          advertisement::Update* update = nullptr) noexcept;
    bool outer_advance_turn(std::chrono::steady_clock::time_point now,
                            const std::vector<pollfd>& pollfds,
                            advertisement::Update* update = nullptr) noexcept;
    // Withdraw the current relationship and route scheduler/runtime loss
    // through the lifecycle reducer.  This is distinct from final daemon
    // shutdown: after exact teardown reaches RetryEligible, a later scheduler
    // turn may ask the same allocator to mint the next incarnation.
    void outer_request_replacement() noexcept;
    // Grants the active scheduler owner permission to mint the next sidecar
    // incarnation after replacement teardown.  Without this grant a parked
    // RetryEligible state cannot launch or force zero-timeout turns.
    void outer_set_scheduler_owner(bool active) noexcept;
    void outer_request_shutdown(advertisement::Update* update = nullptr) noexcept;
    void outer_append_pollfds(std::vector<pollfd>& pollfds) const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point outer_next_deadline() const noexcept;
    [[nodiscard]] sidecar::MonotonicClockIdentity
    monotonic_clock_identity() const noexcept { return config_.clock_identity; }
    [[nodiscard]] bool outer_observe_child_reaped(pid_t pid, int status,
                                                   bool echild = false) noexcept;
    [[nodiscard]] bool outer_observe_child_reaped(
        const sidecar::ReapEvent& event) noexcept;
    [[nodiscard]] const std::optional<sidecar::ReadyLease>&
    outer_current_ready_lease() const noexcept;
    [[nodiscard]] pid_t outer_child_pid() const noexcept;
    [[nodiscard]] int outer_pidfd() const noexcept { return outer_pidfd_; }
    [[nodiscard]] sidecar::LifecycleState outer_lifecycle_state() const noexcept;
    // True only after the shutdown request has been driven through the same
    // one-poll/one-reducer-turn path and every owned descriptor, registration,
    // and exact cleanup phase has reached its terminal state.  Destruction
    // never substitutes for this proof.
    [[nodiscard]] bool outer_shutdown_complete() const noexcept;
    [[nodiscard]] bool outer_action_taken() const noexcept { return outer_action_taken_; }
    // True while already-admitted in-memory work can make progress without
    // waiting for another fd event.  The daemon grants zero-timeout turns to
    // these finite plans; each turn still performs at most one fallible
    // action.  This covers teardown cleanup as well as launch: otherwise a
    // quiet farm can sleep until the absolute teardown deadline between two
    // identity-checked cleanup steps and fail closed before retrying.
    [[nodiscard]] bool outer_immediate_turn_required() const noexcept;
    [[nodiscard]] bool outer_launch_plan_active() const noexcept {
        return outer_launch_phase_ != 0;
    }
    // Diagnostic accessors (trace-only).
    [[nodiscard]] int outer_launch_phase_diag() const noexcept {
        return int(outer_launch_phase_);
    }
    [[nodiscard]] bool outer_launch_failed_diag() const noexcept {
        return outer_launch_failed_;
    }
    [[nodiscard]] const std::optional<InputLifecycleResult>&
    outer_last_input_lifecycle_result() const noexcept {
        return outer_last_input_lifecycle_result_;
    }
    std::optional<InputLifecycleResult> take_outer_input_lifecycle_result() noexcept;
    [[nodiscard]] bool outer_input_operation_active() const noexcept {
        return outer_input_operation_ != nullptr;
    }
    // The lease is the operation-scoped owner/cancellation identity carried
    // into a future adopted endpoint.  It is returned only for the current
    // queued/active operation; a timer or completion holding an older lease
    // cannot affect a reused attempt role slot.
    [[nodiscard]] std::optional<InputLifecycleOperationLease>
    outer_input_operation_lease() const noexcept;
    [[nodiscard]] std::optional<sidecar::AbsoluteMonotonicDeadline>
    outer_input_absolute_deadline() const noexcept;
    // Posts cancellation to this adapter's owner.  The call performs no I/O;
    // the next outer turn closes the exact dialogue and routes its failure
    // through the common A-retirement reducer.  A stale lease is rejected.
    [[nodiscard]] bool outer_cancel_input_operation(
        const InputLifecycleOperationLease& lease) noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point
    outer_input_next_deadline() const noexcept;

    // Typed attempt-leaf boundary for the future compiler reducer.  The
    // central reaper remains the only source of DirectWorkerStatusConsumed;
    // these methods only route the authoritative owner and perform no wait,
    // poll, or descriptor operation.
    [[nodiscard]] std::optional<sidecar::OutputQuiescenceGranted>
    outer_grant_output_quiescence(
        const sidecar::AttemptControlBinding& control,
        const sidecar::AttemptLeafCensus& census,
        const sidecar::LocalDrainedObservation& drained,
        const sidecar::AbsoluteMonotonicDeadline& original_deadline) const noexcept;
    [[nodiscard]] std::optional<sidecar::AttemptLeafRetirementJoin>
    outer_join_attempt_leaf_retirement(
        sidecar::OutputQuiescenceGranted quiescence,
        sidecar::DirectWorkerStatusConsumed direct_worker_status,
        bool empty_after_population,
        bool leaf_cleanup_complete,
        std::chrono::steady_clock::time_point now) const noexcept;

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

    // Typed, incremental sidecar InputRecord retirement lane.  PREPARE keeps
    // the exact A InputRecord open and installs no B.  COMMIT is admitted only
    // with a complete local proof for that same A binding.  CLOSE is a
    // separate logical lease operation.  All three return Disconnected while
    // queued and expose progress through outer_*()/take_outer_*(); none opens
    // a synchronous helper or creates daemon-local InputAttachmentCore state.
    [[nodiscard]] InputLifecycleResult outer_prepare_attempt_retirement(
        RemoteInputLeaseBinding binding,
        const sidecar::AbsoluteMonotonicDeadline& original_deadline) noexcept;
    [[nodiscard]] InputLifecycleResult outer_commit_attempt_replacement(
        RemoteInputLeaseBinding binding, InputLeaseOwner replacement_owner,
        const InputRetirementProof& proof,
        const sidecar::AbsoluteMonotonicDeadline& original_deadline) noexcept;
    [[nodiscard]] InputLifecycleResult outer_close_logical_input_lease(
        RemoteInputLeaseBinding binding,
        const sidecar::AbsoluteMonotonicDeadline& original_deadline) noexcept;
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
    bool reserve_outer_restart() noexcept;
    bool next_input_lifecycle_operation(uint64_t& operation_id) noexcept;
    [[nodiscard]] bool remember_completed_input_lifecycle(
        const InputLifecycleRequest& request) noexcept;
    void retire_input_lifecycle_relationship(AdapterError error) noexcept;
    bool runtime_nodes_valid() const noexcept;
    void disable_relationship() noexcept;
    static void append_update(advertisement::Update& destination,
                              const advertisement::Update& source) noexcept;
    void append_pending_advertisement_update(
        advertisement::Update& destination) noexcept;
    void fail(AdapterError error) noexcept;

    Config config_;
    AdapterState state_ = AdapterState::Stopped;
    AdapterError last_error_ = AdapterError::None;
    PublicListenerObservation public_listener_{};
    std::shared_ptr<sidecar::LaunchIdentityAllocator> launch_identities_;
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
    std::optional<InputLifecycleRequest> outer_input_request_;
    std::unique_ptr<local::DaemonControlOperation> outer_input_operation_;
    std::optional<InputLifecycleOperationLease> outer_input_cancel_lease_;
    std::optional<InputLifecycleResult> outer_last_input_lifecycle_result_;
    bool outer_input_failure_ = false;
    advertisement::Update pending_advertisement_update_{};

    // The compatibility Supervisor above is retained only for the historical
    // unit API; the daemon's outer loop never calls start/poll/shutdown.  The live
    // production path below owns exactly one incremental reducer and no local
    // waitpid/reap loop.
    std::shared_ptr<sidecar::KillDomainVerifier> outer_kill_domain_;
    std::shared_ptr<sidecar::AttemptLeafAuthority> outer_attempt_leaf_authority_;
    std::unique_ptr<sidecar::SidecarLifecycle> outer_lifecycle_;
    sidecar::CentralChildReaperRegistry* outer_reaper_ = nullptr;
    // Configuration is validated once at construction.  Re-running the
    // pathname-bearing valid_config() predicate from every outer turn would
    // hide an extra lstat() in the scheduler path; live node identity is
    // observed explicitly by outer_path_observation()/runtime_nodes_valid().
    bool outer_config_valid_ = false;
    sidecar::CentralChildReaperRegistry::Registration outer_registration_;
    int outer_ready_fd_ = -1;
    int outer_exec_fd_ = -1;
    int outer_pidfd_ = -1;
    int outer_gate_fd_ = -1;
    // Pre-fork descriptors are retained explicitly so setup failure can be
    // unwound one close action at a time instead of using a hidden cleanup
    // loop.  After fork, the *_fd_ fields above are the parent-owned ends.
    int outer_launch_listener_fd_ = -1;
    int outer_launch_ready_write_fd_ = -1;
    int outer_launch_exec_write_fd_ = -1;
    int outer_launch_gate_read_fd_ = -1;
    int outer_launch_fcntl_flags_ = -1;
    pid_t outer_pid_ = -1;
    pid_t outer_pgid_ = -1;
    // Captured by the outer launch action and carried as an immutable
    // observation into the pure lifecycle reducer.  The reducer never opens
    // a pidfd or calls KillDomainVerifier::capture itself.
    std::optional<sidecar::KillDomainLease> outer_group_domain_;
    sidecar::LifecycleIdentity outer_launch_identity_{};
    bool outer_launch_identity_valid_ = false;
    std::vector<std::string> outer_launch_environment_storage_;
    std::vector<char *> outer_launch_environment_;
    std::vector<std::string> outer_launch_argv_storage_;
    std::vector<char *> outer_launch_argv_;
    std::chrono::steady_clock::time_point outer_launch_deadline_{};
    // Nonzero values are incremental launch-plan phases; zero means no plan.
    uint8_t outer_launch_phase_ = 0;
    bool outer_launch_setup_failed_ = false;
    bool outer_launch_started_ = false;
    bool outer_identity_report_pending_ = false;
    bool outer_shutdown_requested_ = false;
    bool outer_replacement_requested_ = false;
    bool outer_scheduler_owner_active_ = false;
    bool outer_replacement_input_close_pending_ = false;
    bool outer_shutdown_input_close_pending_ = false;
    // Set only when the central registry delivered an immutable exact-PID
    // event.  The reducer may consume that value in this turn, but any
    // resulting signal/cleanup action is deferred to the next outer turn so
    // the reaper delivery cannot be paired with a second fallible action.
    bool outer_reap_event_pending_ = false;
    bool outer_action_taken_ = false;
    bool outer_authenticated_ = false;
    std::chrono::steady_clock::time_point outer_auth_deadline_{};
    int outer_auth_fd_ = -1;
    size_t outer_auth_offset_ = 0;
    size_t outer_auth_read_ = 0;
    std::vector<uint8_t> outer_auth_write_;
    std::vector<uint8_t> outer_auth_read_buffer_;
    std::string outer_exec_bytes_;
    std::string outer_ready_bytes_;
    // Prefix already supplied to the reducer as an incremental READY
    // observation.  The pipe buffer is retained for exact framing/parsing;
    // replaying its whole prefix on every quiet turn would make a valid
    // trickled frame look oversized and falsely terminate the incarnation.
    size_t outer_ready_reported_ = 0;
    std::optional<sidecar::ReadyLease> outer_ready_lease_;
    std::optional<sidecar::LifecycleActionResult> outer_pending_action_;
    std::string outer_directory_path_;
    dev_t outer_directory_device_ = 0;
    ino_t outer_directory_inode_ = 0;
    dev_t outer_listener_device_ = 0;
    ino_t outer_listener_inode_ = 0;
    uint8_t outer_cleanup_phase_ = 0; // 0 none, 1 socket, 2 directory
    // Cleanup is an identity-fenced open -> rename -> stat -> unlink -> close
    // sequence.  The step is advanced by one syscall per outer turn.
    uint8_t outer_cleanup_step_ = 0;
    int outer_cleanup_parent_fd_ = -1;
    std::string outer_cleanup_capture_name_;
    bool outer_cleanup_directory_target_ = false;
    // A Gone group proof may have to survive one or more pure reducer/path
    // turns.  It is retained only with the exact opaque domain lease captured
    // for this incarnation; a bare numeric ESRCH result is never cached.
    bool outer_group_gone_observed_ = false;
    sidecar::KillDomainLease outer_group_gone_domain_{};
    // A CleanupPath action must leave the captured path identity available for
    // one later reducer observation.  Clearing it immediately after unlink
    // would make the reducer unable to prove path absence and would strand A
    // in ReapAndGroupCheck.
    bool outer_cleanup_waiting_path_observation_ = false;
    uint8_t outer_auth_phase_ = 0;    // 0 idle, 1 connect, 2 write, 3 read
    size_t outer_auth_expected_ = 0;
    bool outer_auth_start_pending_ = false;
    bool outer_launch_failed_ = false;
    // A pre-fork/setup failure still has to feed one exact pathname-absence
    // observation to the reducer after its captured cleanup phases finish.
    // Keep this separate from outer_launch_failed_: the latter is consumed as
    // the exec observation, while this identity/path obligation may survive
    // several close/unlink turns.
    bool outer_launch_failure_path_pending_ = false;
    bool outer_reap_was_ready_ = false;
    // READY may have been published before the exact pidfd event is routed.
    // Keep that fact tied to the still-owned incarnation across an earlier
    // pure withdrawal; only its matching exact reap may consume it.
    bool outer_ready_had_been_published_ = false;
    bool outer_post_ready_exit_counted_ = false;
    bool outer_exec_complete_ = false;
    bool outer_exec_failed_ = false;
    bool outer_ready_complete_ = false;
    bool outer_ready_invalid_ = false;
    bool outer_auth_failure_ = false;

    bool outer_prepare_launch(
        const sidecar::LifecycleIdentity& identity,
        std::chrono::steady_clock::time_point now) noexcept;
    // Advances one launch-plan phase.  Each call performs at most one
    // fallible kernel action and leaves all ownership in adapter fields.
    bool outer_advance_launch_step() noexcept;
    void outer_finish_launch_failure() noexcept;
    bool outer_group_action(int signal_number) noexcept;
    bool outer_read_exec() noexcept;
    bool outer_read_ready() noexcept;
    bool outer_begin_authentication(
        std::chrono::steady_clock::time_point now) noexcept;
    bool outer_advance_authentication(std::chrono::steady_clock::time_point now,
                                      short revents) noexcept;
    void outer_close_fds() noexcept;
    void outer_disarm_registration() noexcept;
    bool outer_group_observation(sidecar::LifecycleObservation& observation) noexcept;
    bool outer_path_observation(sidecar::LifecycleObservation& observation) noexcept;
    void outer_apply_action(const sidecar::LifecycleActionResult& action,
                            std::chrono::steady_clock::time_point now) noexcept;
    void outer_observe(advertisement::Update& update) noexcept;
    bool outer_cleanup_exact() noexcept;
    void outer_arm_cleanup(uint8_t target) noexcept;
    bool outer_advance_input(
        std::chrono::steady_clock::time_point now,
        const std::vector<pollfd>& pollfds,
        advertisement::Update& update) noexcept;
    InputLifecycleResult queue_outer_input_request(
        InputLifecycleRequest request) noexcept;
    static InputLifecycleStatus map_input_control_status(
        local::DaemonControlStatus status) noexcept;
};

} // namespace icecc::p50::daemon
