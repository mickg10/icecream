#pragma once

// Incremental, outer-loop-owned supervision for a Protocol-50 cache sidecar.
//
// SidecarSupervisor predates the daemon event loop and deliberately remains a
// synchronous compatibility component.  This reducer is the production seam
// for the event-loop implementation: it never forks, polls, sleeps, waits,
// or signals.  The daemon supplies observations and performs at most the one
// action returned by each call.

#include "p50_sidecar_identity.h"

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
        return owner.valid() && pid > 1 && pidfd >= 0 &&
               registry_generation != 0 && incarnation.valid() &&
               (kernel_code == CLD_EXITED || kernel_code == CLD_KILLED ||
                kernel_code == CLD_DUMPED) && reaper_owner_kind_valid(kind) &&
               (kind != ReaperOwnerKind::DirectWorker || starttime_ticks != 0);
    }
};

// Complete control identity supplied by the compiler-attempt owner.  The
// lifecycle does not derive this value from a PID, a path, or a Boolean
// success flag.  Every field participates in the grant binding and is
// allocator-owned by the caller which created the attempt.
struct AttemptControlBinding {
    local::Identity identity{};
    uint64_t control_binding_id = 0;
    uint64_t logical_job = 0;
    uint64_t assignment_epoch = 0;
    uint64_t assignment_nonce = 0;
    uint64_t request_id = 0;
    uint64_t f_store_generation = 0;
    StoreIdentityRoot store_root{};
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};

    [[nodiscard]] bool valid() const noexcept {
        return identity.generation != 0 && identity.attempt != 0 &&
               control_binding_id != 0 && logical_job != 0 &&
               assignment_epoch != 0 && assignment_nonce != 0 && request_id != 0 &&
               f_store_generation != 0 && store_root.valid() &&
               c_store_guid == c_store_guid_for_root(store_root) &&
               f_store_guid == f_store_guid_for_root(store_root) &&
               c_store_guid != f_store_guid;
    }
    [[nodiscard]] bool matches(const LaunchIncarnation& incarnation) const noexcept {
        return valid() && incarnation.valid() &&
               identity == incarnation.identity &&
               f_store_generation == incarnation.store_generation &&
               store_root == incarnation.store_root &&
               c_store_guid == incarnation.c_store_guid &&
               f_store_guid == incarnation.f_store_guid;
    }
    friend bool operator==(const AttemptControlBinding&,
                           const AttemptControlBinding&) = default;
};

enum class SourceTerminalReason : uint8_t {
    Running = 0,
    ExactEnd,
    CancelledOrMessageAfterEnd,
    UnexpectedMessage,
    UnexpectedEof,
    ReadFailure,
    CompilerStdinFailureBeforeExactEnd,
    TimedOut,
    IntegrityFailure,
};

// This is deliberately richer than CompilerInputSource::complete().  The
// latter Boolean conflates exact END, timeout, unexpected EOF, and malformed
// messages; none of those values may be substituted for this observation.
struct SourceTerminalObservation {
    local::Identity attempt{};
    uint64_t source_observation_id = 0;
    SourceTerminalReason reason = SourceTerminalReason::Running;
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    uint64_t pending_bytes = 0;
    uint64_t discarded_bytes = 0;
    bool ordinary_client_alive = false;
    bool exact_wire_terminal = false;
    bool integrity_valid = false;

    [[nodiscard]] bool valid() const noexcept {
        if (attempt.generation == 0 || attempt.attempt == 0 ||
            source_observation_id == 0 || reason == SourceTerminalReason::Running ||
            raw_digest == Digest128{})
            return false;
        if (reason == SourceTerminalReason::ExactEnd &&
            (!exact_wire_terminal || !integrity_valid || discarded_bytes != 0 ||
             pending_bytes != 0))
            return false;
        return true;
    }

    [[nodiscard]] bool preparation_eligible() const noexcept {
        return valid() && reason == SourceTerminalReason::ExactEnd &&
               exact_wire_terminal && integrity_valid && pending_bytes == 0 &&
               discarded_bytes == 0;
    }
    friend bool operator==(const SourceTerminalObservation&,
                           const SourceTerminalObservation&) = default;
};

enum class OutputTerminalClass : uint8_t {
    Open = 0,
    Eof,
    ReadFailure,
    TimedOut,
};

struct OutputStreamObservation {
    uint64_t raw_bytes = 0;
    Digest128 raw_digest{};
    OutputTerminalClass terminal = OutputTerminalClass::Open;

    [[nodiscard]] bool valid() const noexcept {
        return terminal != OutputTerminalClass::Open &&
               raw_digest != Digest128{};
    }
    [[nodiscard]] bool drained_to_eof() const noexcept {
        return valid() && terminal == OutputTerminalClass::Eof;
    }
    friend bool operator==(const OutputStreamObservation&,
                           const OutputStreamObservation&) = default;
};

// Direct-worker and real-compiler identities are separate on purpose.  A
// worker PID is not the compiler PID, and either numeric PID without its
// start-time/allocator incarnation can be reused by the kernel.
struct DirectWorkerIdentity {
    ReaperOwnerKey owner{};
    uint64_t registry_generation = 0;
    int pidfd = -1;
    pid_t pid = -1;
    uint64_t starttime_ticks = 0;
    LaunchIncarnation incarnation{};

    [[nodiscard]] bool valid() const noexcept {
        return owner.valid() && registry_generation != 0 && pidfd >= 0 &&
               pid > 1 && starttime_ticks != 0 && incarnation.valid();
    }
    friend bool operator==(const DirectWorkerIdentity&,
                           const DirectWorkerIdentity&) = default;
};

struct RealCompilerIdentity {
    pid_t pid = -1;
    uint64_t starttime_ticks = 0;
    uint64_t identity_nonce = 0;

    [[nodiscard]] bool valid() const noexcept {
        return pid > 1 && starttime_ticks != 0 && identity_nonce != 0;
    }
    friend bool operator==(const RealCompilerIdentity&,
                           const RealCompilerIdentity&) = default;
};

// Exact wait-status identity copied from a consumed central-reaper event.  A
// caller cannot create DirectWorkerStatusConsumed from this value; it is only
// the canonical digestable identity carried by the local-drained observation.
struct DirectWorkerStatusIdentity {
    ReaperOwnerKey owner{};
    uint64_t registry_generation = 0;
    int pidfd = -1;
    pid_t pid = -1;
    int status = 0;
    int kernel_code = 0;
    uint64_t starttime_ticks = 0;
    LaunchIncarnation incarnation{};

    [[nodiscard]] bool valid() const noexcept {
        return owner.valid() && registry_generation != 0 && pidfd >= 0 &&
               pid > 1 && starttime_ticks != 0 && incarnation.valid() &&
               (kernel_code == CLD_EXITED || kernel_code == CLD_KILLED ||
                kernel_code == CLD_DUMPED);
    }
    friend bool operator==(const DirectWorkerStatusIdentity&,
                           const DirectWorkerStatusIdentity&) = default;
};

// Complete local conjunction before a worker may prepare ordinary output.
// This value is an observation only; positive authority remains the opaque
// OutputQuiescenceGranted minted by AttemptLeafAuthority.
struct LocalDrainedObservation {
    AttemptControlBinding control{};
    DirectWorkerIdentity direct_worker{};
    RealCompilerIdentity real_compiler{};
    uint64_t observation_id = 0;
    DirectWorkerStatusIdentity wait_status{};
    OutputStreamObservation stdout_stream{};
    OutputStreamObservation stderr_stream{};
    SourceTerminalObservation source{};
    Digest128 observation_digest{};
    bool cancellation_requested = false;

    [[nodiscard]] bool valid() const noexcept {
        return control.valid() && direct_worker.valid() && real_compiler.valid() &&
               observation_id != 0 && wait_status.valid() && stdout_stream.valid() &&
               stderr_stream.valid() && source.valid() &&
               source.attempt == control.identity &&
               control.matches(direct_worker.incarnation) &&
               control.matches(wait_status.incarnation) &&
               wait_status.owner == direct_worker.owner &&
               wait_status.registry_generation == direct_worker.registry_generation &&
               wait_status.pidfd == direct_worker.pidfd &&
               wait_status.pid == direct_worker.pid &&
               wait_status.starttime_ticks == direct_worker.starttime_ticks &&
               observation_digest != Digest128{};
    }

    [[nodiscard]] bool preparation_eligible() const noexcept {
        return valid() && !cancellation_requested && source.preparation_eligible() &&
               stdout_stream.drained_to_eof() && stderr_stream.drained_to_eof();
    }
    friend bool operator==(const LocalDrainedObservation&,
                           const LocalDrainedObservation&) = default;
};

// Exact identity for a compiler-attempt output leaf.  The registry and leaf
// authority, rather than a compiler reducer, bind these fields.  In
// particular, the pidfd/registry generation and allocator-issued incarnation
// fence are carried alongside the leaf device/inode; a path or numeric PID
// supplied without those anchors is not a usable identity.
struct AttemptLeafIdentity {
    ReaperOwnerKey owner{};
    uint64_t registry_generation = 0;
    LaunchIncarnation incarnation{};
    int pidfd = -1;
    pid_t direct_worker_pid = -1;
    uint64_t process_starttime_ticks = 0;
    dev_t leaf_device = 0;
    ino_t leaf_inode = 0;
    std::string leaf_path;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool matches(const ReapEvent& event) const noexcept;
    [[nodiscard]] bool matches(local::Identity identity) const noexcept;
    friend bool operator==(const AttemptLeafIdentity&,
                           const AttemptLeafIdentity&) = default;
};

// A census is valid only when an exact leaf identity has been joined to the
// one direct worker and both independent cgroup facts are present.  The
// booleans are observations, not authority: only an AttemptLeafAuthority can
// turn this value into the opaque OUTPUT_QUIESCENCE_GRANTED capability.
struct AttemptLeafCensus {
    AttemptLeafIdentity identity{};
    pid_t sole_direct_worker_pid = -1;
    bool no_descendants = false;
    bool singleton_cgroup_procs = false;

    [[nodiscard]] bool valid() const noexcept {
        return identity.valid() && sole_direct_worker_pid == identity.direct_worker_pid &&
               no_descendants && singleton_cgroup_procs;
    }
    friend bool operator==(const AttemptLeafCensus&,
                           const AttemptLeafCensus&) = default;
};

class OutputQuiescenceGranted;
class AttemptLeafRetirementJoin;
class AttemptLeafAuthority;

// The same immutable value is the handoff contract for a future
// CompilerAttemptRecord.  It must consume this exact event (rather than
// waiting or searching by PID).  This token is deliberately move-only: one
// central registry delivery cannot be copied into two reducers.
class DirectWorkerStatusConsumed {
public:
    DirectWorkerStatusConsumed() = default;
    DirectWorkerStatusConsumed(const DirectWorkerStatusConsumed&) = delete;
    DirectWorkerStatusConsumed& operator=(const DirectWorkerStatusConsumed&) = delete;
    DirectWorkerStatusConsumed(DirectWorkerStatusConsumed&& other) noexcept;
    DirectWorkerStatusConsumed& operator=(DirectWorkerStatusConsumed&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const ReapEvent& event() const noexcept { return event_; }
    [[nodiscard]] ReaperOwnerKey owner() const noexcept { return event_.owner; }
    [[nodiscard]] pid_t pid() const noexcept { return event_.pid; }
    [[nodiscard]] int status() const noexcept { return event_.status; }
    [[nodiscard]] int pidfd() const noexcept { return event_.pidfd; }
    [[nodiscard]] uint64_t registry_generation() const noexcept {
        return event_.registry_generation;
    }
    [[nodiscard]] uint64_t starttime_ticks() const noexcept {
        return event_.starttime_ticks;
    }
    [[nodiscard]] const LaunchIncarnation& incarnation() const noexcept {
        return event_.incarnation;
    }
    [[nodiscard]] DirectWorkerStatusIdentity status_identity() const noexcept {
        return DirectWorkerStatusIdentity{event_.owner,
                                          event_.registry_generation,
                                          event_.pidfd,
                                          event_.pid,
                                          event_.status,
                                          event_.kernel_code,
                                          event_.starttime_ticks,
                                          event_.incarnation};
    }

private:
    explicit DirectWorkerStatusConsumed(ReapEvent event) noexcept
        : event_(std::move(event)),
          valid_(event_.exact_identity() && !event_.echild &&
                 event_.kind == ReaperOwnerKind::DirectWorker) {}
    ReapEvent event_{};
    bool valid_ = false;
    friend class CentralChildReaperRegistry;
};

// Move-only capability minted exactly once by the leaf authority after the
// no-descendant + singleton cgroup.procs census.  No public constructor exists
// because a caller-provided pair of booleans must never become positive
// authority.  The immutable value may be retained behind shared_ptr by a
// replay-safe outer operation, but it cannot be copied into a second token.
class OutputQuiescenceGranted {
public:
    OutputQuiescenceGranted() noexcept = default;
    OutputQuiescenceGranted(const OutputQuiescenceGranted&) = delete;
    OutputQuiescenceGranted& operator=(const OutputQuiescenceGranted&) = delete;
    OutputQuiescenceGranted(OutputQuiescenceGranted&&) noexcept;
    OutputQuiescenceGranted& operator=(OutputQuiescenceGranted&&) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const AttemptLeafCensus& census() const noexcept {
        return census_;
    }
    [[nodiscard]] const AttemptControlBinding& control_binding() const noexcept {
        return control_;
    }
    [[nodiscard]] const LocalDrainedObservation& drained_observation() const noexcept {
        return drained_;
    }
    [[nodiscard]] const AbsoluteMonotonicDeadline&
    original_deadline() const noexcept { return original_deadline_; }
    [[nodiscard]] uint64_t grant_sequence() const noexcept {
        return grant_sequence_;
    }
    // Compatibility spelling for code which treated the sequence as a
    // diagnostic serial.  It is the same allocator/authority-issued value.
    [[nodiscard]] uint64_t serial() const noexcept { return grant_sequence_; }
    [[nodiscard]] bool expired(
        std::chrono::steady_clock::time_point now) const noexcept {
        if (!original_deadline_.valid())
            return true;
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        return now_ns >= original_deadline_.expires_at_ns;
    }
    [[nodiscard]] bool preparation_eligible(
        std::chrono::steady_clock::time_point now) const noexcept {
        return valid() && !expired(now) && drained_.preparation_eligible();
    }
    [[nodiscard]] bool matches_binding(
        const AttemptControlBinding& control,
        const AttemptLeafCensus& census,
        const LocalDrainedObservation& drained,
        const AbsoluteMonotonicDeadline& original_deadline) const noexcept;

private:
    explicit OutputQuiescenceGranted(
        AttemptControlBinding control, AttemptLeafCensus census,
        LocalDrainedObservation drained,
        AbsoluteMonotonicDeadline original_deadline,
        uint64_t grant_sequence, Digest128 binding_digest) noexcept
        : control_(std::move(control)), census_(std::move(census)),
          drained_(std::move(drained)), original_deadline_(original_deadline),
          grant_sequence_(grant_sequence),
          binding_digest_(binding_digest) {}
    AttemptControlBinding control_{};
    AttemptLeafCensus census_{};
    LocalDrainedObservation drained_{};
    AbsoluteMonotonicDeadline original_deadline_{};
    uint64_t grant_sequence_ = 0;
    Digest128 binding_digest_{};
    [[nodiscard]] const Digest128& binding_digest() const noexcept {
        return binding_digest_;
    }
    friend class AttemptLeafAuthority;
    friend class SidecarLifecycle;
};

// Final exact join consumed by a future CompilerAttemptRecord.  It combines
// the one output-quiescence grant, the central registry's move-only consumed
// direct-worker status, EmptyAfterPopulation, and leaf cleanup.  Reducers do
// not wait, search by PID, or manufacture this value from ECHILD.
class AttemptLeafRetirementJoin {
public:
    AttemptLeafRetirementJoin() noexcept = default;
    AttemptLeafRetirementJoin(const AttemptLeafRetirementJoin&) = delete;
    AttemptLeafRetirementJoin& operator=(const AttemptLeafRetirementJoin&) = delete;
    AttemptLeafRetirementJoin(AttemptLeafRetirementJoin&&) noexcept;
    AttemptLeafRetirementJoin& operator=(AttemptLeafRetirementJoin&&) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const AttemptLeafIdentity& identity() const noexcept {
        return quiescence_.census().identity;
    }
    [[nodiscard]] const OutputQuiescenceGranted& quiescence() const noexcept {
        return quiescence_;
    }
    [[nodiscard]] const DirectWorkerStatusConsumed& direct_worker_status() const noexcept {
        return direct_worker_status_;
    }
    [[nodiscard]] bool empty_after_population() const noexcept {
        return empty_after_population_;
    }
    [[nodiscard]] bool leaf_cleanup_complete() const noexcept {
        return leaf_cleanup_complete_;
    }
    [[nodiscard]] const AttemptControlBinding& control_binding() const noexcept {
        return quiescence_.control_binding();
    }
    [[nodiscard]] const LocalDrainedObservation& drained_observation() const noexcept {
        return quiescence_.drained_observation();
    }
    [[nodiscard]] const AbsoluteMonotonicDeadline&
    original_deadline() const noexcept {
        return quiescence_.original_deadline();
    }
    [[nodiscard]] bool matches(local::Identity identity) const noexcept;

private:
    AttemptLeafRetirementJoin(OutputQuiescenceGranted quiescence,
                              DirectWorkerStatusConsumed direct_worker_status,
                              bool empty_after_population,
                              bool leaf_cleanup_complete) noexcept;
    OutputQuiescenceGranted quiescence_{};
    DirectWorkerStatusConsumed direct_worker_status_{};
    bool empty_after_population_ = false;
    bool leaf_cleanup_complete_ = false;
    friend class AttemptLeafAuthority;
};

// Boundary owned by the authoritative attempt-leaf/reaper integration.  The
// default daemon sidecar configuration has no implementation and therefore
// fails closed.  A future compiler-attempt lane supplies the implementation
// that performs incremental cgroup census/cleanup; it receives exact central
// DirectWorkerStatusConsumed values and never owns wait* authority.
class AttemptLeafAuthority {
public:
    virtual ~AttemptLeafAuthority() = default;
    [[nodiscard]] virtual std::optional<OutputQuiescenceGranted>
    grant_output_quiescence(
        const AttemptControlBinding& control,
        const AttemptLeafCensus& census,
        const LocalDrainedObservation& drained,
        const AbsoluteMonotonicDeadline& original_deadline) noexcept = 0;
    [[nodiscard]] virtual std::optional<AttemptLeafRetirementJoin>
    join_retirement(OutputQuiescenceGranted quiescence,
                    DirectWorkerStatusConsumed direct_worker_status,
                    bool empty_after_population,
                    bool leaf_cleanup_complete,
                    std::chrono::steady_clock::time_point now) noexcept = 0;

protected:
    static OutputQuiescenceGranted issue_output_quiescence(
        AttemptControlBinding control, AttemptLeafCensus census,
        LocalDrainedObservation drained,
        AbsoluteMonotonicDeadline original_deadline,
        uint64_t grant_sequence) noexcept;
    static AttemptLeafRetirementJoin issue_retirement_join(
        OutputQuiescenceGranted quiescence,
        DirectWorkerStatusConsumed direct_worker_status,
        bool empty_after_population, bool leaf_cleanup_complete,
        std::chrono::steady_clock::time_point now) noexcept;
};

// A small value mailbox is the lifetime boundary between the central reaper
// and a lifecycle.  It contains no raw owner pointer and is bounded so a
// broken outer loop cannot turn reap delivery into unbounded allocation.
class ReapMailbox {
public:
    bool enqueue(ReapEvent event) noexcept;
    bool dequeue(ReapEvent& event) noexcept;

private:
    static constexpr size_t kMaximumEvents = 16;
    std::array<ReapEvent, kMaximumEvents> events_{};
    std::mutex mutex_;
    size_t head_ = 0;
    size_t count_ = 0;
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
    // Optional authoritative compiler-attempt leaf owner.  The sidecar
    // reducer never substitutes a local InputAttachmentCore or caller
    // booleans when this boundary is absent; output retirement fails closed.
    std::shared_ptr<AttemptLeafAuthority> attempt_leaf_authority;
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
    // The outer loop binds the descriptor returned by pidfd_open and the
    // registry generation minted for this exact registration.  A lifecycle
    // event without this exact binding is rejected; the old scalar observer
    // remains only as a fail-closed source-compatibility seam.
    void bind_reaper_identity(int pidfd, uint64_t registry_generation) noexcept {
        if (pidfd >= 0 && registry_generation != 0) {
            pidfd_ = pidfd;
            registry_generation_ = registry_generation;
        }
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
    // The positive OUTPUT_QUIESCENCE_GRANTED and final direct-worker join
    // capabilities are minted by the configured leaf authority only.  These
    // adapters are pure routing hooks and perform no syscall or wait.
    [[nodiscard]] std::optional<OutputQuiescenceGranted>
    grant_output_quiescence(
        const AttemptControlBinding& control,
        const AttemptLeafCensus& census,
        const LocalDrainedObservation& drained,
        const AbsoluteMonotonicDeadline& original_deadline) const noexcept;
    [[nodiscard]] std::optional<AttemptLeafRetirementJoin>
    join_attempt_leaf_retirement(
        OutputQuiescenceGranted quiescence,
        DirectWorkerStatusConsumed direct_worker_status,
        bool empty_after_population,
        bool leaf_cleanup_complete,
        std::chrono::steady_clock::time_point now) const noexcept;

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
    std::shared_ptr<AttemptLeafAuthority> attempt_leaf_authority_;
    std::string ready_buffer_;
    std::chrono::steady_clock::time_point deadline_{};
    std::chrono::steady_clock::time_point teardown_deadline_{};
    uint64_t owner_id_ = 0;
    uint64_t owner_generation_ = 1;
    std::shared_ptr<ReapMailbox> reap_mailbox_;
    int pidfd_ = -1;
    uint64_t registry_generation_ = 0;
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
    Registration register_owner(pid_t pid, pid_t pgid, ReaperOwnerKey owner,
                                std::weak_ptr<ReapMailbox> mailbox,
                                dev_t listener_device = 0,
                                ino_t listener_inode = 0, int pidfd = -1,
                                LaunchIncarnation incarnation = {},
                                ReaperOwnerKind kind = ReaperOwnerKind::Sidecar,
                                uint64_t starttime_ticks = 0) noexcept;

    // Direct compiler workers have no sidecar mailbox.  This explicit route
    // makes that ownership choice visible at registration time and returns a
    // move-only status token from reap_one_direct_worker() without requiring a
    // dummy mailbox or allowing the worker to fall through to an anonymous
    // process-wide wait sweep.
    Registration register_direct_worker(
        pid_t pid, pid_t pgid, ReaperOwnerKey owner, int pidfd,
        LaunchIncarnation incarnation, uint64_t starttime_ticks = 0) noexcept;

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
    [[nodiscard]] std::optional<DirectWorkerStatusConsumed>
    reap_one_direct_worker(pid_t pid, int pidfd) noexcept;
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
