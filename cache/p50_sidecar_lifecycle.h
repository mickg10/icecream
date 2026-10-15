#pragma once

// Incremental, outer-loop-owned supervision for a Protocol-50 cache sidecar.
//
// SidecarSupervisor predates the daemon event loop and deliberately remains a
// synchronous compatibility component.  This reducer is the production seam
// for the event-loop implementation: it never forks, polls, sleeps, waits,
// or signals.  The daemon supplies observations and performs at most the one
// action returned by each call.

#include "p50_sidecar_identity.h"
#include "p50_compat_child_handle.h"

#include <chrono>
#include <cstdint>
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace icecc::p50::sidecar {

enum class LifecycleState : uint8_t {
    Stopped = 0,
    LaunchPrepared,
    ForkedAwaitExecAndReady,
    Ready,
    TerminatingGrace,
    TerminatingKill,
    ReapAndGroupCheck,
    RetryEligible,
    DegradedLegacy,
    FailedClosed,
};

enum class LifecycleAction : uint8_t {
    None = 0,
    LaunchPrepared,
    Withdraw,
    SendTerm,
    SendKill,
    PublishReady,
    // The exact group/reap proof is complete but the old listener node is
    // still present.  The outer adapter performs one identity-checked cleanup
    // action, then feeds path absence back on a later reducer turn.
    CleanupPath,
    EnterDegradedLegacy,
    RetryEligible,
    FailedClosed,
};

enum class ExecObservation : uint8_t { None = 0, Succeeded, Failed };
enum class ReadyObservation : uint8_t { None = 0, Partial, Complete, Invalid };
// `Gone` represents the exact ESRCH proof without colliding with errno's
// platform macro named ESRCH.
enum class GroupObservation : uint8_t { Unknown = 0, Present, Gone };

// A numeric PGID and an ESRCH probe are not a non-reusable kill authority:
// zombies can keep the number present and a later process group can reuse it.
// Only a KillDomainVerifier can issue this opaque capability after another
// authority has proved that the group identity cannot be reused. The reducer
// never creates or infers one; without it teardown fails closed.
class KillDomainLease {
public:
    KillDomainLease() = default;
    [[nodiscard]] bool valid() const noexcept { return capability_ != nullptr; }
    // Keep the kernel-backed identity opaque while allowing the reducer to
    // reject a verifier that returns a lease for a different fork.
    [[nodiscard]] bool matches(pid_t pid, pid_t pgid) const noexcept {
        return capability_ != nullptr && capability_->pid == pid &&
               capability_->pgid == pgid;
    }

private:
    struct Capability {
        uint64_t serial = 0;
        pid_t pid = -1;
        pid_t pgid = -1;
    };
    explicit KillDomainLease(std::shared_ptr<const Capability> capability) noexcept
        : capability_(std::move(capability)) {}
    std::shared_ptr<const Capability> capability_;
    friend class KillDomainVerifier;
    friend bool operator==(const KillDomainLease&, const KillDomainLease&) = default;
};

struct LifecycleObservation;

// The production default is deliberately rejecting.  A platform adapter may
// derive from this interface and issue a lease only after creating a private,
// per-attempt kernel-backed cgroup-v2 leaf and proving its lifetime.  A caller
// cannot manufacture a valid lease from PID/PGID/boolean observations: the
// capability and serial are private to this authority.
class KillDomainVerifier {
public:
    virtual ~KillDomainVerifier() = default;
    [[nodiscard]] virtual std::optional<KillDomainLease> capture(
        pid_t pid, pid_t pgid) noexcept = 0;
    [[nodiscard]] virtual bool proves_absent(
        const KillDomainLease& lease, pid_t pid, pid_t pgid,
        const LifecycleObservation& observation) const noexcept = 0;

protected:
    static KillDomainLease issue(pid_t pid, pid_t pgid,
                                 uint64_t serial) noexcept;
};

class RejectingKillDomainVerifier final : public KillDomainVerifier {
public:
    std::optional<KillDomainLease> capture(pid_t, pid_t) noexcept override {
        return std::nullopt;
    }
    bool proves_absent(const KillDomainLease&, pid_t, pid_t,
                      const LifecycleObservation&) const noexcept override {
        return false;
    }
};

// All facts in this structure are supplied by the daemon's already-running
// outer loop.  A numeric group=Gone/ESRCH observation is only one fact; leader
// reaping, ECHILD, or that numeric probe alone never substitutes for the
// non-reusable KillDomainLease.
struct LifecycleObservation {
    ExecObservation exec = ExecObservation::None;
    ReadyObservation ready = ReadyObservation::None;
    std::string_view ready_bytes{};
    std::optional<ReadyLease> ready_lease;
    uint64_t store_generation = 0;
    GroupObservation group = GroupObservation::Unknown;
    // The reducer never treats this caller-provided value as authority.  It is
    // accepted only when it is the exact opaque token captured for this fork;
    // the verifier still has to prove the independently observed absence.
    KillDomainLease group_domain{};
    pid_t pid = -1;
    pid_t observed_pgid = -1;
    bool path_absent = false;
    std::string_view observed_path{};
    dev_t observed_device = 0;
    ino_t observed_inode = 0;
    bool child_waitable = false;
    bool identity_lost = false;
    bool request_replacement = false;
    bool request_legacy = false;
};

struct ReaperOwnerKey {
    uint64_t owner_id = 0;
    uint64_t generation = 0;

    [[nodiscard]] bool valid() const noexcept {
        return owner_id != 0 && generation != 0;
    }
    friend bool operator==(const ReaperOwnerKey&, const ReaperOwnerKey&) = default;
};

// A registry slot has one consumer role.  Keeping the role in the immutable
// event prevents a future compiler-attempt reducer from asking the sidecar
// delivery route for a direct-worker status (or vice versa), even when a PID
// happens to be reused after the original registration is retired.
enum class ReaperOwnerKind : uint8_t {
    Sidecar = 0,
    DirectWorker = 1,
};

[[nodiscard]] inline bool reaper_owner_kind_valid(
    ReaperOwnerKind kind) noexcept {
    return kind == ReaperOwnerKind::Sidecar ||
           kind == ReaperOwnerKind::DirectWorker;
}

struct ReapEvent {
    ReaperOwnerKey owner{};
    pid_t pid = -1;
    int status = 0;
    bool echild = false;
    // The central reaper snapshots the exact kernel/registry identity at the
    // delivery boundary.  These fields are metadata, not caller-provided
    // authority: a lifecycle accepts an event only when the snapshot still
    // names its bound pidfd, registry slot, and LaunchIncarnation.
    int pidfd = -1;
    // Which child-identity primitive this event's `pidfd`/`starttime_ticks`
    // pair was observed through.  Native events carry an exact pidfd and
    // leave starttime_ticks as supplementary (direct-worker only); Compat
    // events carry no pidfd (ENOSYS on kernel 3.10-class hosts) and instead
    // are exact only when `starttime_ticks` still matches the registered
    // (pid, starttime) pair at delivery -- see
    // icecc::p50::platform::CompatChildHandle in p50_compat_child_handle.h.
    ChildHandleBackend backend = ChildHandleBackend::Native;
    uint64_t registry_generation = 0;
    LaunchIncarnation incarnation{};
    // Raw kernel wait classification captured by the exact pidfd probe.  The
    // converted wait `status` above is retained for existing consumers, but
    // the event is not exact unless its CLD_* origin is carried too.
    int kernel_code = 0;
    ReaperOwnerKind kind = ReaperOwnerKind::Sidecar;
    // Direct-worker consumers additionally require the creator's kernel
    // start-time fence.  Sidecar registrations may leave this zero because
    // their LaunchIncarnation/path lease is the lifecycle identity; a direct
    // worker registration is rejected unless this value is supplied.
    uint64_t starttime_ticks = 0;

    [[nodiscard]] bool exact_identity() const noexcept {
        const bool handle_exact = (backend == ChildHandleBackend::Native)
                                       ? (pidfd >= 0)
                                       : (pidfd < 0 && starttime_ticks != 0);
        return owner.valid() && pid > 1 && handle_exact &&
               registry_generation != 0 && incarnation.valid() &&
               (kernel_code == CLD_EXITED || kernel_code == CLD_KILLED ||
                kernel_code == CLD_DUMPED) && reaper_owner_kind_valid(kind) &&
               (kind != ReaperOwnerKind::DirectWorker || starttime_ticks != 0);
    }
};

// A Compat-backend starttime mismatch observed by the central registry is an
// invariant violation, not a benign "no change yet" result: the kernel-3.10
// (pid, starttime_ticks) fence that identifies this exact child no longer
// matches, which means a blind reap happened somewhere outside the central
// registry's two-phase protocol (the child exited and its /proc entry aged
// out, or the pid was reused by an unrelated process). This can never be
// retried or treated as a transient absence, and it carries no kernel wait-
// status classification -- there is no real exit code to report -- so it
// cannot be shaped as a ReapEvent (see ReapEvent::exact_identity() above,
// which requires a genuine CLD_* origin). It is delivered through its own
// single-slot channel on the same per-owner mailbox the lifecycle already
// holds from registration.
struct CompatInvariantViolation {
    ReaperOwnerKind kind = ReaperOwnerKind::Sidecar;
    pid_t pid = -1;
    ReaperOwnerKey owner{};
    uint64_t registry_generation = 0;
    LaunchIncarnation incarnation{};
    uint64_t expected_starttime_ticks = 0;
    // 0 means /proc's entry for this pid was already gone (exited past
    // /proc's reach); nonzero-but-mismatched means the numeric pid was
    // reused by an unrelated process.
    uint64_t observed_starttime_ticks = 0;
    // Whether the /proc/<pid>/stat read-and-parse itself succeeded. This,
    // not observed_errno below, is the authoritative signal for
    // distinguishing "unreadable/unparseable" from "mismatched":
    // observed_starttime_ticks alone cannot tell them apart (a readable stat
    // line can itself report starttime 0), and observed_errno alone cannot
    // either, because a parse-only failure (no ')' found, or field 22
    // missing from an otherwise-readable line) leaves errno at 0 -- the same
    // value a successful, merely-mismatched read reports.
    bool observed_readable = false;
    // errno captured from the failed read of /proc/<pid>/stat. Only
    // meaningful when observed_readable is false: 0 then means the
    // underlying read syscalls succeeded but the content was unparseable
    // (no ')' found, or field 22 missing), while nonzero names the real
    // failure (e.g. ENOENT once /proc/<pid> itself is gone). When
    // observed_readable is true this is always 0 and carries no
    // information -- observed_starttime_ticks is the genuine, readable,
    // mismatched value instead.
    int observed_errno = 0;

    [[nodiscard]] bool exact_identity() const noexcept {
        return owner.valid() && pid > 1 && registry_generation != 0 &&
               incarnation.valid() && reaper_owner_kind_valid(kind);
    }
};

// A small value mailbox is the lifetime boundary between the central reaper
// and a lifecycle.  It contains no raw owner pointer and is bounded so a
// broken outer loop cannot turn reap delivery into unbounded allocation.
class ReapMailbox {
public:
    bool enqueue(ReapEvent event) noexcept;
    bool dequeue(ReapEvent& event) noexcept;

    // A Compat invariant violation is a single terminal fact: the registry
    // removes the owner immediately after recording one (see
    // CentralChildReaperRegistry::publish_compat_invariant_violation()), so
    // at most one can ever matter for a given owner incarnation and a
    // one-slot channel is sufficient. It is intentionally delivered through
    // the same mailbox the owner already holds a weak_ptr to from
    // registration -- no new wiring at register_owner() is needed.
    bool enqueue_violation(CompatInvariantViolation violation) noexcept;
    bool dequeue_violation(CompatInvariantViolation& violation) noexcept;

private:
    static constexpr size_t kMaximumEvents = 16;
    std::array<ReapEvent, kMaximumEvents> events_{};
    std::mutex mutex_;
    size_t head_ = 0;
    size_t count_ = 0;
    std::optional<CompatInvariantViolation> violation_;
};

struct LifecycleIdentity {
    local::Identity control{};
    // This namespace generation is intentionally separate from control.generation.
    // A store may restart at generation 1 when its fresh root changes.
    uint64_t store_generation = 0;
    StoreIdentityRoot store_root{};
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    std::string private_directory;
    dev_t listener_device = 0;
    ino_t listener_inode = 0;

    [[nodiscard]] bool valid() const noexcept;
    friend bool operator==(const LifecycleIdentity&, const LifecycleIdentity&) = default;
};

struct LifecycleActionResult {
    LifecycleAction action = LifecycleAction::None;
    LifecycleState state = LifecycleState::Stopped;
    LifecycleIdentity identity{};
    pid_t pid = -1;
    pid_t pgid = -1;
    std::string path;
};

struct SidecarLifecycleConfig {
    uint64_t control_generation = 0;
    std::string private_root;
    std::chrono::milliseconds launch_timeout{1000};
    std::chrono::milliseconds exec_timeout{1000};
    std::chrono::milliseconds ready_timeout{1000};
    std::chrono::milliseconds grace_timeout{1000};
    std::chrono::milliseconds kill_timeout{1000};
    uint32_t max_attempts = 3;
    // This allocator is the sole mint for the complete launch incarnation:
    // control attempt, F-store generation, StoreIdentity root, and role GUIDs.
    // A second caller-selected store-generation authority is intentionally
    // absent so replacement cannot retain or forge an old F-store fence.
    std::shared_ptr<LaunchIdentityAllocator> identities;
    std::shared_ptr<KillDomainVerifier> kill_domain_verifier;
};

class SidecarLifecycle {
public:
    explicit SidecarLifecycle(SidecarLifecycleConfig config) noexcept;

    SidecarLifecycle(const SidecarLifecycle&) = delete;
    SidecarLifecycle& operator=(const SidecarLifecycle&) = delete;

    static bool valid_config(const SidecarLifecycleConfig& config) noexcept;

    // The only operation which burns an attempt/root.  It performs no fork;
    // the returned LaunchPrepared action is handed to the outer loop.
    LifecycleActionResult begin(std::chrono::steady_clock::time_point now) noexcept;

    // Applies a bounded set of observations and returns at most one action.
    // It never calls poll/select/sleep/waitpid and never launches twice per turn.
    LifecycleActionResult advance(
        std::chrono::steady_clock::time_point now,
        const LifecycleObservation& observation = {}) noexcept;

    // Called only by the central reaper.  A duplicate, stale, or foreign PID
    // is rejected.  ECHILD records leader knowledge but does not prove PGID
    // disappearance.
    bool observe_child_reaped(pid_t pid, int status, bool echild = false) noexcept;
    // Exact value event emitted by CentralChildReaperRegistry.  Unlike the
    // legacy scalar overload this carries the immutable pidfd, registration
    // generation, and allocator-issued incarnation fence.
    bool observe_child_reaped(const ReapEvent& event) noexcept;
    [[nodiscard]] LifecycleState state() const noexcept { return state_; }
    [[nodiscard]] const std::optional<LifecycleIdentity>& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const std::optional<ReadyLease>& current_ready_lease() const noexcept {
        return current_ready_lease_;
    }
    [[nodiscard]] uint32_t attempts() const noexcept { return attempts_; }
    [[nodiscard]] bool leader_reaped() const noexcept { return leader_reaped_; }
    [[nodiscard]] bool term_sent() const noexcept { return term_sent_; }
    [[nodiscard]] bool kill_sent() const noexcept { return kill_sent_; }
    // The outer daemon folds this absolute deadline into its one poll
    // timeout.  The reducer itself never sleeps or polls.
    [[nodiscard]] std::chrono::steady_clock::time_point next_deadline() const noexcept {
        return deadline_;
    }
    [[nodiscard]] ReaperOwnerKey owner_key() const noexcept {
        return ReaperOwnerKey{owner_id_, owner_generation_};
    }
    // The outer loop binds the registry generation minted for this exact
    // registration, and -- for the Native backend only -- the descriptor
    // returned by pidfd_open.  A lifecycle event without this exact binding
    // is rejected; the old scalar observer remains only as a fail-closed
    // source-compatibility seam.
    //
    // register_owner()'s own contract requires pidfd >= 0 for
    // ChildHandleBackend::Native and pidfd < 0 for ::Compat (the
    // pidfd-less, starttime-fenced handle); pidfd_ already defaults to -1,
    // so a Compat caller correctly leaves it untouched here while still
    // needing registry_generation_ bound -- consume_reap_events()'s
    // violation cross-check (the only acceptance path a Compat owner has)
    // requires identity_.has_value() && registry_generation_ != 0 and does
    // not consult pidfd_ at all. Gating this whole bind on pidfd >= 0 would
    // silently leave registry_generation_ at 0 for every Compat owner and
    // make every one of its invariant violations look stale forever.
    void bind_reaper_identity(int pidfd, uint64_t registry_generation) noexcept {
        if (registry_generation == 0)
            return;
        if (pidfd >= 0)
            pidfd_ = pidfd;
        registry_generation_ = registry_generation;
    }
    // The outer launcher captures the private listener node before forking.
    // Bind that exact device/inode to the allocator-issued identity even when
    // the child fails before READY; teardown then has a path fence for the
    // pre-READY failure case and cannot fall back to name-only removal.
    void bind_listener_identity(dev_t device, ino_t inode) noexcept {
        if (identity_.has_value() && device != 0 && inode != 0 &&
            (identity_->listener_device == 0 ||
             (identity_->listener_device == device &&
              identity_->listener_inode == inode))) {
            identity_->listener_device = device;
            identity_->listener_inode = inode;
        }
    }
    [[nodiscard]] int bound_pidfd() const noexcept { return pidfd_; }
    [[nodiscard]] uint64_t bound_registry_generation() const noexcept {
        return registry_generation_;
    }
    [[nodiscard]] std::weak_ptr<ReapMailbox> reap_mailbox() const noexcept {
        return reap_mailbox_;
    }
    // Sticky once set: a Compat invariant violation forces FailClosed on the
    // very turn it is observed (see consume_reap_events()/advance()) and is
    // never cleared afterward, so a caller that only wants to log the fact
    // once can safely treat a transition from empty to non-empty as the one
    // and only occurrence for this lifecycle instance.
    [[nodiscard]] const std::optional<CompatInvariantViolation>&
    compat_invariant_violation() const noexcept {
        return compat_invariant_violation_;
    }

#if defined(ICECC_P50_SIDECAR_LIFECYCLE_TEST_HOOKS)
    // Compile-time-only seam exposing the private exact-identity gate on a
    // single reap event, so unit tests can prove rejection/acceptance of a
    // non-exact event directly without forking a real child to drive
    // consume_reap_events(). The installed library is built without this
    // macro and never exposes consume_reap() outside the class.
    bool test_consume_reap(const ReapEvent& event) noexcept {
        return consume_reap(event);
    }
#endif

private:
    LifecycleActionResult result(LifecycleAction action) const noexcept;
    bool allocate_identity() noexcept;
    bool accept_ready(const LifecycleObservation& observation) noexcept;
    bool exact_group_absent(const LifecycleObservation& observation) const noexcept;
    bool exact_path_absent(const LifecycleObservation& observation) const noexcept;
    void withdraw() noexcept;
    void enter_termination(std::chrono::steady_clock::time_point now) noexcept;
    void clear_incarnation() noexcept;
    void consume_reap_events() noexcept;
    bool consume_reap(const ReapEvent& event) noexcept;
    LifecycleActionResult fail_closed() noexcept;
    bool teardown_expired(std::chrono::steady_clock::time_point now) const noexcept;

    SidecarLifecycleConfig config_;
    LifecycleState state_ = LifecycleState::Stopped;
    std::optional<LifecycleIdentity> identity_;
    std::optional<ReadyLease> current_ready_lease_;
    pid_t child_pid_ = -1;
    pid_t process_group_ = -1;
    uint32_t attempts_ = 0;
    bool leader_reaped_ = false;
    bool leader_waitable_ = false;
    bool echild_observed_ = false;
    bool exec_succeeded_ = false;
    bool term_sent_ = false;
    bool kill_sent_ = false;
    bool identity_lost_ = false;
    bool teardown_started_ = false;
    bool legacy_requested_ = false;
    bool group_proof_required_ = false;
    std::optional<KillDomainLease> group_domain_;
    std::shared_ptr<KillDomainVerifier> kill_domain_verifier_;
    std::string ready_buffer_;
    std::chrono::steady_clock::time_point deadline_{};
    std::chrono::steady_clock::time_point teardown_deadline_{};
    uint64_t owner_id_ = 0;
    uint64_t owner_generation_ = 1;
    std::shared_ptr<ReapMailbox> reap_mailbox_;
    int pidfd_ = -1;
    uint64_t registry_generation_ = 0;
    std::optional<CompatInvariantViolation> compat_invariant_violation_;
};

// Central-reaper ownership seam.  The reaper calls observe_child_reaped once
// for each PID; it never broadcasts a wait result to multiple supervisors.
class CentralChildReaperRegistry {
    struct SharedState;

public:
    CentralChildReaperRegistry() noexcept;
    class Registration {
    public:
        Registration() = default;
        ~Registration();
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&& other) noexcept;
        Registration& operator=(Registration&& other) noexcept;

        [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
        void reset() noexcept;
        [[nodiscard]] ReaperOwnerKey owner_key() const noexcept { return owner_; }
        [[nodiscard]] uint64_t registry_generation() const noexcept {
            return registry_generation_;
        }

    private:
        Registration(std::shared_ptr<SharedState> state, pid_t pid,
                     ReaperOwnerKey owner, uint64_t registry_generation) noexcept
            : state_(std::move(state)), pid_(pid), owner_(owner),
              registry_generation_(registry_generation) {}
        friend class CentralChildReaperRegistry;
        std::shared_ptr<SharedState> state_;
        pid_t pid_ = -1;
        ReaperOwnerKey owner_{};
        uint64_t registry_generation_ = 0;
    };

    // Preferred value-event API.  The returned token unregisters exactly this
    // PID/generation when destroyed, even if the registry itself is gone.
    // `backend` is mandatory (not defaulted) and placed ahead of every
    // defaulted parameter below so a caller can never silently fall back to
    // Native: Native requires a real `pidfd`; Compat requires `pidfd < 0`
    // and a non-zero `starttime_ticks` (see
    // icecc::p50::platform::CompatChildHandle, p50_compat_child_handle.h).
    Registration register_owner(pid_t pid, pid_t pgid, ReaperOwnerKey owner,
                                std::weak_ptr<ReapMailbox> mailbox,
                                ChildHandleBackend backend,
                                dev_t listener_device = 0,
                                ino_t listener_inode = 0, int pidfd = -1,
                                LaunchIncarnation incarnation = {},
                                ReaperOwnerKind kind = ReaperOwnerKind::Sidecar,
                                uint64_t starttime_ticks = 0) noexcept;

    bool unregister_owner(pid_t pid, ReaperOwnerKey owner) noexcept;
    bool observe_child_reaped(pid_t pid, ReaperOwnerKey owner, int status,
                              bool echild = false) noexcept;
    bool observe_child_reaped(pid_t pid, int status, bool echild = false) noexcept;
    // The central registry is the sole wait-status consumer for registered
    // children.  It performs one exact-pidfd WNOWAIT observation and one
    // bounded later exact-pidfd consumption, publishing an immutable value
    // event; it never uses an anonymous child sweep or treats ECHILD as a
    // reap.  `pidfd` is checked against the registration before probing.
    [[nodiscard]] std::optional<ReapEvent> reap_one(
        pid_t pid, int pidfd) noexcept;
    [[nodiscard]] std::optional<ReapEvent> reap_one_unobserved() noexcept;
    // Retry only the bounded in-memory publication step for a status that was
    // already consumed through its exact pidfd.  This is needed when a target
    // lifecycle mailbox was temporarily full: pidfd readiness is cleared by
    // the consume operation, so publication cannot depend on another poll
    // edge and must never repeat waitid().
    [[nodiscard]] std::optional<ReapEvent> publish_pending() noexcept;
    [[nodiscard]] std::optional<std::pair<dev_t, ino_t>> listener_node(
        pid_t pid, ReaperOwnerKey owner) const noexcept;
    // Fair central-reaper scheduling: one owner is selected per outer-loop
    // turn, with a rotating cursor rather than an unordered-map sentinel.
    [[nodiscard]] std::optional<pid_t> next_unobserved_pid() noexcept;
    [[nodiscard]] size_t size() const noexcept;

private:
    struct Entry {
        pid_t pgid = -1;
        ReaperOwnerKey owner{};
        std::weak_ptr<ReapMailbox> mailbox;
        dev_t listener_device = 0;
        ino_t listener_inode = 0;
        int pidfd = -1;
        ChildHandleBackend backend = ChildHandleBackend::Native;
        LaunchIncarnation incarnation{};
        uint64_t registry_generation = 0;
        bool wait_observed = false;
        bool wait_consumed = false;
        int observed_code = 0;
        int observed_status = 0;
        bool observed = false;
        size_t slot = 0;
        ReaperOwnerKind kind = ReaperOwnerKind::Sidecar;
        uint64_t starttime_ticks = 0;
    };
    struct SharedState {
        static constexpr size_t kMaximumOwners = 256;
        std::mutex mutex;
        std::unordered_map<pid_t, Entry> owners;
        std::vector<pid_t> slots;
        size_t cursor = 0;
        uint64_t next_registry_generation = 1;

        SharedState() {
            owners.reserve(kMaximumOwners);
            slots.reserve(kMaximumOwners);
        }
    };
    [[nodiscard]] std::optional<ReapEvent> publish_observed(
        pid_t pid, ReaperOwnerKey owner) noexcept;

    // A kernel-3.10-class Compat-backend observation found the owner's
    // (pid, starttime_ticks) fence broken -- either the /proc entry is gone
    // or the pid has already been recycled by the kernel. This is an
    // invariant violation, never a benign "no change yet" result: it is
    // delivered through the owner's mailbox (if one is live) as a
    // CompatInvariantViolation, and the owner slot is unconditionally
    // removed here (inlining unregister_owner()'s erase logic rather than
    // calling it, since the caller already holds state_->mutex and the lock
    // is non-reentrant). DirectWorker owners never have a live mailbox
    // (register_direct_worker() always passes an expired one), so for them
    // this call degrades to pid-slot hygiene only -- consistent with
    // DirectWorker's existing "never retried" contract.
    void publish_compat_invariant_violation(
        pid_t pid, ReaperOwnerKey owner, uint64_t observed_starttime_ticks,
        bool observed_readable, int observed_errno) noexcept;

    std::shared_ptr<SharedState> state_;
};

const char* lifecycle_state_name(LifecycleState state) noexcept;
const char* lifecycle_action_name(LifecycleAction action) noexcept;

// Parser seams are pure and bounded.  Parsing is intentionally outside the
// reducer so an event loop can feed trickled bytes without blocking.
bool parse_exec_status(std::string_view bytes, ExecObservation& result) noexcept;
bool parse_ready_frame(std::string_view bytes, const LifecycleIdentity& expected,
                       pid_t expected_pid, ReadyLease& lease) noexcept;

} // namespace icecc::p50::sidecar
