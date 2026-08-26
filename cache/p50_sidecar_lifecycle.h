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
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

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
};

enum class ExecObservation : uint8_t { None = 0, Succeeded, Failed };
enum class ReadyObservation : uint8_t { None = 0, Partial, Complete, Invalid };
// `Gone` represents the exact ESRCH proof without colliding with errno's
// platform macro named ESRCH.
enum class GroupObservation : uint8_t { Unknown = 0, Present, Gone };

// All facts in this structure are supplied by the daemon's already-running
// outer loop.  In particular, group=ESRCH is the exact getpgid/kill proof for
// the expected PGID; leader reaping or ECHILD is not substituted for it.
struct LifecycleObservation {
    ExecObservation exec = ExecObservation::None;
    ReadyObservation ready = ReadyObservation::None;
    std::string_view ready_bytes{};
    std::optional<ReadyLease> ready_lease;
    uint64_t store_generation = 0;
    GroupObservation group = GroupObservation::Unknown;
    pid_t pid = -1;
    pid_t observed_pgid = -1;
    bool path_absent = false;
    std::string_view observed_path{};
    bool request_replacement = false;
    bool request_legacy = false;
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
    uint64_t store_generation = 1;
    std::string private_root;
    std::chrono::milliseconds launch_timeout{1000};
    std::chrono::milliseconds exec_timeout{1000};
    std::chrono::milliseconds ready_timeout{1000};
    std::chrono::milliseconds grace_timeout{1000};
    std::chrono::milliseconds kill_timeout{1000};
    uint32_t max_attempts = 3;
    std::shared_ptr<LaunchIdentityAllocator> identities;
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

private:
    LifecycleActionResult result(LifecycleAction action) const noexcept;
    bool allocate_identity() noexcept;
    bool accept_ready(const LifecycleObservation& observation) noexcept;
    bool exact_group_absent(const LifecycleObservation& observation) const noexcept;
    bool exact_path_absent(const LifecycleObservation& observation) const noexcept;
    void withdraw() noexcept;
    void enter_termination(std::chrono::steady_clock::time_point now) noexcept;
    void clear_incarnation() noexcept;

    SidecarLifecycleConfig config_;
    LifecycleState state_ = LifecycleState::Stopped;
    std::optional<LifecycleIdentity> identity_;
    std::optional<ReadyLease> current_ready_lease_;
    pid_t child_pid_ = -1;
    pid_t process_group_ = -1;
    uint32_t attempts_ = 0;
    bool leader_reaped_ = false;
    bool echild_observed_ = false;
    bool exec_succeeded_ = false;
    bool term_sent_ = false;
    bool kill_sent_ = false;
    std::string ready_buffer_;
    std::chrono::steady_clock::time_point deadline_{};
};

// Central-reaper ownership seam.  The reaper calls observe_child_reaped once
// for each PID; it never broadcasts a wait result to multiple supervisors.
class CentralChildReaperRegistry {
public:
    bool register_owner(pid_t pid, pid_t pgid, SidecarLifecycle& owner) noexcept;
    bool observe_child_reaped(pid_t pid, int status, bool echild = false) noexcept;
    // Fair central-reaper scheduling: one owner is selected per outer-loop
    // turn, with a rotating cursor rather than an unordered-map sentinel.
    [[nodiscard]] std::optional<pid_t> next_unobserved_pid() noexcept;
    [[nodiscard]] size_t size() const noexcept { return owners_.size(); }

private:
    struct Entry {
        pid_t pgid = -1;
        SidecarLifecycle* owner = nullptr;
        bool observed = false;
    };
    std::unordered_map<pid_t, Entry> owners_;
    std::deque<pid_t> order_;
    size_t cursor_ = 0;
};

const char* lifecycle_state_name(LifecycleState state) noexcept;
const char* lifecycle_action_name(LifecycleAction action) noexcept;

// Parser seams are pure and bounded.  Parsing is intentionally outside the
// reducer so an event loop can feed trickled bytes without blocking.
bool parse_exec_status(std::string_view bytes, ExecObservation& result) noexcept;
bool parse_ready_frame(std::string_view bytes, const LifecycleIdentity& expected,
                       pid_t expected_pid, ReadyLease& lease) noexcept;

} // namespace icecc::p50::sidecar
