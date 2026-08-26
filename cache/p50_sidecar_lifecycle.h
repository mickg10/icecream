#pragma once

// Incremental, outer-loop-owned supervision for a Protocol-50 cache sidecar.
//
// SidecarSupervisor predates the daemon event loop and deliberately remains a
// synchronous compatibility component.  This reducer is the production seam
// for the event-loop implementation: it never forks, polls, sleeps, waits,
// or signals.  The daemon supplies observations and performs at most the one
// action returned by each call.

#include "p50_sidecar_supervisor.h"

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

struct ReapEvent {
    ReaperOwnerKey owner{};
    pid_t pid = -1;
    int status = 0;
    bool echild = false;
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
    [[nodiscard]] ReaperOwnerKey owner_key() const noexcept {
        return ReaperOwnerKey{owner_id_, owner_generation_};
    }
    [[nodiscard]] std::weak_ptr<ReapMailbox> reap_mailbox() const noexcept {
        return reap_mailbox_;
    }

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

    private:
        Registration(std::shared_ptr<SharedState> state, pid_t pid,
                     ReaperOwnerKey owner) noexcept
            : state_(std::move(state)), pid_(pid), owner_(owner) {}
        friend class CentralChildReaperRegistry;
        std::shared_ptr<SharedState> state_;
        pid_t pid_ = -1;
        ReaperOwnerKey owner_{};
    };

    // Preferred value-event API.  The returned token unregisters exactly this
    // PID/generation when destroyed, even if the registry itself is gone.
    Registration register_owner(pid_t pid, pid_t pgid, ReaperOwnerKey owner,
                                std::weak_ptr<ReapMailbox> mailbox,
                                dev_t listener_device = 0,
                                ino_t listener_inode = 0) noexcept;

    bool unregister_owner(pid_t pid, ReaperOwnerKey owner) noexcept;
    bool observe_child_reaped(pid_t pid, ReaperOwnerKey owner, int status,
                              bool echild = false) noexcept;
    bool observe_child_reaped(pid_t pid, int status, bool echild = false) noexcept;
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
        bool observed = false;
        size_t slot = 0;
    };
    struct SharedState {
        static constexpr size_t kMaximumOwners = 256;
        std::mutex mutex;
        std::unordered_map<pid_t, Entry> owners;
        std::vector<pid_t> slots;
        size_t cursor = 0;

        SharedState() {
            owners.reserve(kMaximumOwners);
            slots.reserve(kMaximumOwners);
        }
    };
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
