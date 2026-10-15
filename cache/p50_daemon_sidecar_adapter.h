#pragma once

// Main-loop-owned daemon integration for the Protocol-50 cache sidecar.
//
// The adapter is intentionally the only owner of an incarnation's outer
// lifecycle, control relationship, and READY advertisement Controller.  The
// daemon remains the owner of its public listener and reports that observation
// through observe_public_listener(); this class never opens a public socket.

#include "p50_compat_child_handle.h"
#include "p50_daemon_cache_dispatch.h"
#include "p50_daemon_control.h"
#include "p50_input_fd_attachment.h"
#include "p50_local_transport.h"
#include "p50_ready_advertisement.h"
#include "p50_sidecar_lifecycle.h"
#include "p50_sidecar_identity.h"

#include <chrono>
#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <unordered_map>
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

    // Which backend the launched sidecar's InputFdAttachmentService should
    // use, passed down as --input-backend. The daemon's platform-compat
    // module (daemon/p50_platform_compat.h) resolves this exactly once at
    // daemon startup; configure_cache_adapter() never constructs this
    // adapter at all when that decision is FailClosed (see
    // doc/p50-platform-compat.md), so by the time a Config reaches this
    // adapter the value here is always Native or Compat, never a third
    // fail-closed state. Defaults to Native so any test or caller that
    // does not set this field explicitly keeps today's exact behavior.
    InputMaterializationBackend input_backend = InputMaterializationBackend::Native;

    // Entropy source for LaunchIdentityAllocator's fresh store-identity
    // roots (cache/p50_sidecar_identity.h), passed straight through to its
    // constructor. Same provenance and same fail-closed-is-handled-upstream
    // guarantee as input_backend immediately above: the daemon's
    // platform-compat module (daemon/p50_platform_compat.h) resolves this
    // exactly once at daemon startup from the M3 (random) mitigation
    // decision, and configure_cache_adapter() never constructs this adapter
    // at all when that decision is FailClosed. Defaults to the native
    // getrandom()-backed provider so any test or caller that does not set
    // this field explicitly keeps today's exact behavior.
    StoreIdentityEntropyProvider entropy_provider = system_store_identity_entropy;

    // M1 (child handle) backend for this adapter's OuterKillDomainVerifier.
    // Same provenance and same fail-closed-is-handled-upstream guarantee as
    // input_backend and entropy_provider above: the daemon's platform-compat
    // module resolves this exactly once at daemon startup from the M1
    // (child) mitigation decision, and configure_cache_adapter() never
    // constructs this adapter at all when that decision is FailClosed.
    // Defaults to the native pidfd-backed verifier so any test or caller
    // that does not set this field explicitly keeps today's exact behavior.
    ChildHandleBackend child_handle_backend = ChildHandleBackend::Native;

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

    /* R17a: the NAMED refusal -- the same predicate as valid_config, but
       the first failing clause is returned as a human-readable reason
       (field, path, expected vs actual owner/mode) so the daemon's
       "invalid cache sidecar configuration" log names the check instead
       of making the operator bisect the 25-clause chain by hand.  Empty
       string = configuration valid. */
    static std::string config_refusal_reason(const Config& config) noexcept;

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
    // Unlike attempt() (only meaningful once a ready lease has been bound),
    // this reflects the lifecycle's own relaunch counter even when every
    // attempt fails closed before reaching READY (e.g. a sustained
    // close-range dispatch failure never tolerates a child). Callers that
    // need to confirm "a relaunch was attempted" without requiring READY
    // must use this instead of attempt().
    [[nodiscard]] uint32_t outer_lifecycle_attempts() const noexcept {
        return outer_lifecycle_ ? outer_lifecycle_->attempts() : 0;
    }
    [[nodiscard]] bool authenticated() const noexcept;
    [[nodiscard]] advertisement::Snapshot advertisement_snapshot() const noexcept {
        return controller_.snapshot();
    }
    [[nodiscard]] CacheSessionDispatcher* dispatcher() noexcept { return dispatcher_.get(); }
    [[nodiscard]] const CacheSessionDispatcher* dispatcher() const noexcept {
        return dispatcher_.get();
    }
    // Outer-loop lifecycle seam.  These methods are the only production
    // entry points used by iceccd.  Each call performs at most one reducer
    // turn and at most one launch/TERM/KILL/cleanup/connect/send/receive
    // action, except that an input lifecycle dialogue chains its ready
    // phases within its per-turn syscall quota; waiting and deadline
    // selection remain in the daemon poll owner.
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
    // Sticky once set: forwards SidecarLifecycle::compat_invariant_violation().
    // A Compat-backend starttime fence mismatch is a fail-closed invariant
    // violation, never a retryable condition, so this accessor's empty-to-
    // non-empty transition happens at most once per adapter instance -- the
    // daemon's own one-shot loud-log site relies on exactly that property.
    [[nodiscard]] const std::optional<sidecar::CompatInvariantViolation>&
    outer_compat_invariant_violation() const noexcept;
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

    // Opens a fresh authenticated control relationship for one exact committed
    // InputRecord.  It neither consumes nor replaces the dispatcher relationship
    // used by CACHE_SESSION.  Every failure is descriptor-less and fail-closed.
    [[nodiscard]] InputFdAttachmentResult attach_input(
        InputRecordKey key, InputLeaseOwner owner,
        uint64_t request_id) noexcept;

    // The caller retains the observed ReadyLease and must reject completion
    // after replacement. This operation never accesses mutable adapter state.
    [[nodiscard]] std::unique_ptr<InputFdAttachmentOperation> begin_attach_input(
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
    // Fork-path CLOEXEC-sweep fault injection (Task 4).  Takes effect only
    // in the forked child, right before execve(); see
    // cache/p50_daemon_sidecar_adapter.cpp's OuterLaunchPhase::Fork.
    // force_close_range_errno: inject this errno instead of calling
    // close_range(2) at all.  ENOSYS/EINVAL/EPERM must each take the proc
    // fallback; any other value (e.g. EBADF) must fail closed.
    void test_force_close_range_errno(int errno_value) noexcept {
        test_force_close_range_errno_ = errno_value;
    }
    // force_proc_open_failure: make the proc fallback's own
    // open("/proc/self/fd") fail, so the fallback-of-the-fallback path
    // (fail closed) is reachable without root or seccomp.
    void test_force_proc_open_failure(bool force) noexcept {
        test_force_proc_open_failure_ = force;
    }
#endif
    // Direct, no-fork unit entry point for the proc-fallback CLOEXEC sweep
    // itself (Task 4).  Runs the exact same sweep the Fork-case child calls
    // (sidecar_cloexec_sweep_proc_fallback(), itself always compiled in and
    // used unconditionally by production code), but in the calling (test)
    // process against whatever fds that process already has open: no fork,
    // no execve, no seccomp/root dependency.  force_open_failure mirrors
    // test_force_proc_open_failure above: when true, the sweep's internal
    // open("/proc/self/fd") is made to fail and the function returns false
    // without touching any fd.
    //
    // Deliberately NOT gated by ICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS,
    // unlike the fault-injection setters above: it touches no hook-only
    // state, only forwards its own parameter into the always-present sweep
    // function, so it stays linkable in a production (hooks-undefined)
    // object -- which unittests/p50daemonsidecaradapter-source.sh relies on
    // when it links the hook-compiled test harness against a production
    // object to prove the two behave identically.
    static bool test_sweep_cloexec_proc_fallback(bool force_open_failure) noexcept;

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

    // Diagnostic-only lifecycle trace hooks (ICECC_P50_DAEMON_LIFECYCLE_TRACE).
    // Each is a cheap no-op (map lookup against an empty/absent-entry table)
    // when the env var is unset; none of them influence any lifecycle
    // decision or return value. See p50_daemon_sidecar_adapter.cpp for the
    // JSONL schema ("icecream-p50-daemon-lifecycle-v1") written by the last
    // of these, trace_lifecycle_emit_terminal().
    void trace_lifecycle_mark_enqueued(uint64_t operation_id) noexcept;
    void trace_lifecycle_mark_first_attempt(uint64_t operation_id) noexcept;
    void trace_lifecycle_update_milestones(uint64_t operation_id,
                                           uint8_t phase_before,
                                           uint8_t phase_after,
                                           bool control_complete) noexcept;
    void trace_lifecycle_emit_terminal(
        const InputLifecycleResult& result) noexcept;

    // Diagnostic-only per-operation progress capture for the daemon-side
    // lifecycle RPC trace (ICECC_P50_DAEMON_LIFECYCLE_TRACE). Populated only
    // when that env var names a path; otherwise this table stays empty, and
    // its upkeep is the same handful of cheap getenv()-gated branches as the
    // existing ICECC_P50_SOURCE_RESULT_TRACE convention in
    // cache/p50_cache_service.cpp. None of these fields participate in any
    // lifecycle decision -- they are read back only by the trace writer.
    struct InputLifecycleTraceState {
        std::chrono::steady_clock::time_point enqueue_mono{};
        uint64_t enqueue_turn = 0;
        size_t pending_depth_at_enqueue = 0;
        bool first_attempt_recorded = false;
        std::chrono::steady_clock::time_point first_attempt_mono{};
        uint64_t turns_before_first_attempt = 0;
        bool connect_done_recorded = false;
        std::chrono::steady_clock::time_point connect_done_mono{};
        bool request_sent_recorded = false;
        std::chrono::steady_clock::time_point request_sent_mono{};
        bool response_received_recorded = false;
        std::chrono::steady_clock::time_point response_received_mono{};
        bool ack_sent_recorded = false;
        std::chrono::steady_clock::time_point ack_sent_mono{};
    };
    // Keyed by InputLifecycleRequest::operation_id. Entries are created at
    // enqueue (queue_outer_input_request) only when the trace env var is set,
    // updated as the in-flight dialogue's Phase advances, and erased when the
    // terminal trace record for that operation_id is written.
    std::unordered_map<uint64_t, InputLifecycleTraceState> trace_lifecycle_state_;
    // Incremented unconditionally, once per daemon iteration, at the top of
    // outer_begin_turn() -- the documented once-per-iteration entry point.
    // Cheap bookkeeping with no behavioral effect; used by the lifecycle
    // trace to report how many daemon turns elapsed between a request's
    // enqueue and its first dequeue/attempt.
    uint64_t outer_turn_number_ = 0;

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
    // Always present (not macro-guarded) so the class layout is identical
    // whether or not ICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS is defined
    // for this translation unit -- only the setter methods above are
    // guarded.  Zero/false in every production object; read unconditionally
    // by the fork-path CLOEXEC sweep in the .cpp, same convention as
    // attempt_/cumulative_post_ready_exits_ above.
    int test_force_close_range_errno_ = 0;
    bool test_force_proc_open_failure_ = false;
    std::vector<std::chrono::steady_clock::time_point> restart_times_;
    std::vector<InputLifecycleRequest> pending_input_lifecycle_;
    std::vector<InputLifecycleRequest> completed_input_lifecycle_;
    std::optional<InputLifecycleRequest> outer_input_request_;
    std::unique_ptr<local::DaemonControlOperation> outer_input_operation_;
    std::optional<InputLifecycleResult> outer_last_input_lifecycle_result_;
    bool outer_input_failure_ = false;
    advertisement::Update pending_advertisement_update_{};

    // The live production path below owns exactly one incremental reducer and
    // no local waitpid/reap loop.
    std::shared_ptr<sidecar::KillDomainVerifier> outer_kill_domain_;
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
