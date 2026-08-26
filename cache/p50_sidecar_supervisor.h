#pragma once

// Lifecycle-only supervisor for the future Protocol-50 cache sidecar.
//
// This component deliberately has no daemon, socket, scheduler, or
// advertisement dependency.  It owns only the child process and its private
// readiness/exec-status pipes.  A later daemon adapter can consume the
// deterministic state and counters without changing this lifecycle policy.

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

#include "p50_local_transport.h"
#include "p50_incarnation_identity.h"
#include "protocol50.h"

#include <sys/types.h>
#include <sys/stat.h>

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

struct LaunchIncarnation {
    local::Identity identity{};
    FStoreGuid f_store_guid{};

    [[nodiscard]] bool valid() const noexcept {
        return identity.generation != 0 && identity.attempt != 0 &&
               f_store_guid != FStoreGuid{} &&
               f_store_guid == f_store_guid_for_incarnation(identity);
    }
};

// This allocator is deliberately owned outside Supervisor and may be shared
// by replacement Supervisor/controller objects inside one iceccd.  That is
// the fence which prevents a controller recreation from reusing either a
// launch attempt or F_STORE_GUID.
class LaunchIdentityAllocator {
public:
    LaunchIdentityAllocator(uint64_t generation, uint64_t first_attempt = 1) noexcept;
    std::optional<LaunchIncarnation> allocate() noexcept;

private:
    std::mutex mutex_;
    uint64_t generation_ = 0;
    uint64_t next_attempt_ = 0;
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

    // When set, each launch receives a fresh identity-derived private
    // directory and must publish a structured READY lease.  The directory is
    // never reused between launches or Supervisor recreations.
    std::string lease_root;
    std::shared_ptr<LaunchIdentityAllocator> launch_identities;
};

struct ReadyLease {
    local::Identity identity{};
    pid_t pid = -1;
    FStoreGuid f_store_guid{};
    std::string private_directory;
    std::string socket_path;
    Digest128 socket_path_digest{};
    dev_t listener_device = 0;
    ino_t listener_inode = 0;
    dev_t directory_device = 0;
    ino_t directory_inode = 0;

    [[nodiscard]] bool valid() const noexcept {
        return identity.generation != 0 && identity.attempt != 0 && pid > 1 &&
               f_store_guid != FStoreGuid{} && !private_directory.empty() &&
               !socket_path.empty() && listener_device != 0 && listener_inode != 0;
    }
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
    // A nonnegative value is exposed only while this instance still owns the
    // corresponding process group.  In particular, a reaped child never
    // leaves a stale PGID observable to a later launch or destructor call.
    [[nodiscard]] pid_t process_group_id() const noexcept {
        return process_group_owned_ ? process_group_ : -1;
    }
    [[nodiscard]] bool has_private_fds() const noexcept;
    // A lease is observable only after the exact structured READY frame has
    // passed all identity, PID, path, digest, and listener inode checks.
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
    bool wait_for_exit(std::chrono::milliseconds timeout) noexcept;
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
    // On Linux this is an exact reference to the forked task.  It is used for
    // every direct signal so a reaped and reused numeric PID can never become
    // a teardown target.  Platforms without pidfd retain process-group
    // teardown, but fail closed if that group identity is invalidated.
    int child_pidfd_ = -1;
    pid_t process_group_ = -1;
    bool process_group_owned_ = false;
    int ready_read_ = -1;
    int exec_read_ = -1;
    std::vector<std::chrono::steady_clock::time_point> restart_times_;
    std::optional<ReadyLease> pending_lease_;
    std::optional<ReadyLease> current_lease_;
};

} // namespace icecc::p50::sidecar
