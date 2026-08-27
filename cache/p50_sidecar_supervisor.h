#pragma once

// Lifecycle-only supervisor for the future Protocol-50 cache sidecar.
//
// This component deliberately has no daemon, socket, scheduler, or
// advertisement dependency.  It owns only the child process and its private
// readiness/exec-status pipes.  A later daemon adapter can consume the
// deterministic state and counters without changing this lifecycle policy.

#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
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

struct MonotonicClockIdentity {
    // CLOCK_MONOTONIC is the only clock accepted by the cross-process
    // deadline contract.  The time namespace inode distinguishes a daemon
    // from a process which has been moved into another time namespace.
    uint64_t clock_domain_id = 0;
    uint64_t time_namespace_id = 0;

    [[nodiscard]] bool valid() const noexcept {
        return clock_domain_id != 0 && time_namespace_id != 0;
    }
    friend bool operator==(const MonotonicClockIdentity&,
                           const MonotonicClockIdentity&) = default;
};

// Called during process/configuration setup, never from a lifecycle turn. A
// forked/execed sidecar inherits the daemon's time namespace and therefore
// shares this identity unless an external namespace change invalidates it.
[[nodiscard]] inline MonotonicClockIdentity
process_monotonic_clock_identity() noexcept {
    struct stat info{};
    if (::stat("/proc/self/ns/time", &info) != 0)
        return {};
    const uint64_t namespace_id =
        (static_cast<uint64_t>(info.st_dev) << 32) ^
        static_cast<uint64_t>(info.st_ino);
    if (namespace_id == 0)
        return {};
    // Linux's CLOCK_MONOTONIC clock domain is represented by id 1 here.  The
    // numeric id is part of the signed wire binding, not a duration unit.
    return MonotonicClockIdentity{1, namespace_id};
}

// A deadline crossing the daemon/sidecar boundary is an absolute value in
// the CLOCK_MONOTONIC domain.  The two identity fields are supplied by the
// process owner which established the relationship; zero is intentionally
// invalid so a receiver cannot silently adopt its own clock or time
// namespace.  The representation is never reconstructed from a receive-time
// plus remaining duration.
struct AbsoluteMonotonicDeadline {
    int64_t expires_at_ns = 0;
    uint64_t clock_domain_id = 0;
    uint64_t time_namespace_id = 0;

    [[nodiscard]] bool valid() const noexcept {
        return expires_at_ns > 0 && clock_domain_id != 0 &&
               time_namespace_id != 0;
    }
    [[nodiscard]] bool matches_clock(uint64_t expected_clock_domain_id,
                                     uint64_t expected_time_namespace_id) const noexcept {
        return valid() && expected_clock_domain_id != 0 &&
               expected_time_namespace_id != 0 &&
               clock_domain_id == expected_clock_domain_id &&
               time_namespace_id == expected_time_namespace_id;
    }
    [[nodiscard]] bool matches_clock(
        const MonotonicClockIdentity& expected) const noexcept {
        return expected.valid() &&
               matches_clock(expected.clock_domain_id,
                             expected.time_namespace_id);
    }
    [[nodiscard]] bool expired(int64_t now_ns, uint64_t observed_clock_domain_id,
                               uint64_t observed_time_namespace_id) const noexcept {
        return !matches_clock(observed_clock_domain_id, observed_time_namespace_id) ||
               now_ns >= expires_at_ns;
    }
    [[nodiscard]] std::chrono::steady_clock::time_point as_steady_time_point() const noexcept {
        return std::chrono::steady_clock::time_point{
            std::chrono::nanoseconds{expires_at_ns}};
    }
    [[nodiscard]] static AbsoluteMonotonicDeadline from_steady_time_point(
        std::chrono::steady_clock::time_point deadline,
        uint64_t clock_domain_id, uint64_t time_namespace_id) noexcept {
        const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline.time_since_epoch()).count();
        return AbsoluteMonotonicDeadline{count, clock_domain_id,
                                         time_namespace_id};
    }
    friend bool operator==(const AbsoluteMonotonicDeadline&,
                           const AbsoluteMonotonicDeadline&) = default;
};

namespace detail {

// Lease paths are generated below one private, absolute root.  Keep the
// representation lexical and unambiguous: cleanup operates with dirfds, but
// this check is also the fence against a path alias being accepted as lease
// metadata.
inline bool canonical_absolute_lease_path(std::string_view path) noexcept {
    if (path.empty() || path.front() != '/' || path.size() > local::kMaxUnixPath ||
        path.back() == '/' || path.find('\0') != std::string_view::npos)
        return false;
    size_t begin = 1;
    while (begin < path.size()) {
        const size_t end = path.find('/', begin);
        const size_t length = end == std::string_view::npos ? path.size() - begin
                                                              : end - begin;
        if (length == 0 || (length == 1 && path[begin] == '.') ||
            (length == 2 && path[begin] == '.' && path[begin + 1] == '.'))
            return false;
        for (size_t index = begin; index != begin + length; ++index) {
            const char character = path[index];
            if (character == ' ' || character == '\t' || character == '\n' ||
                character == '\r')
                return false;
        }
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return true;
}

} // namespace detail

// The supervisor gives the child this inherited descriptor through the
// environment.  The descriptor itself is private, CLOEXEC in the parent, and
// the only descriptor deliberately cleared in the pre-exec child.
inline constexpr std::string_view kReadyFdEnvironment =
    "ICECC_CACHE_SERVICE_READY_FD";
inline constexpr std::string_view kListenerFdEnvironment =
    "ICECC_CACHE_SERVICE_LISTENER_FD";

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
    // F-store generation is an incarnation authority distinct from the
    // daemon control generation.  It advances for every sidecar allocation,
    // including replacement under one daemon generation.
    uint64_t store_generation = 0;
    StoreIdentityRoot store_root{};
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};

    [[nodiscard]] bool valid() const noexcept {
        return identity.generation != 0 && identity.attempt != 0 &&
        store_generation != 0 && store_generation != std::numeric_limits<uint64_t>::max() &&
        store_root.valid() &&
        store_identity_guid_valid_for_role(c_store_guid.bytes,
                                           kStoreIdentityClientRole) &&
        store_identity_guid_valid_for_role(f_store_guid.bytes,
                                           kStoreIdentityFileRole) &&
        c_store_guid != f_store_guid &&
        c_store_guid == c_store_guid_for_root(store_root) &&
        f_store_guid == f_store_guid_for_root(store_root);
    }
    friend bool operator==(const LaunchIncarnation&,
                           const LaunchIncarnation&) = default;
};

// This allocator is deliberately owned outside Supervisor and may be shared
// by replacement Supervisor/controller objects inside one iceccd.  That is
// the fence which prevents a controller recreation from reusing either a
// launch attempt or F_STORE_GUID.
class LaunchIdentityAllocator {
public:
    LaunchIdentityAllocator(uint64_t generation, uint64_t first_attempt = 1,
                            StoreIdentityEntropyProvider entropy_provider =
                                system_store_identity_entropy) noexcept;
    std::optional<LaunchIncarnation> allocate() noexcept;

private:
    std::mutex mutex_;
    uint64_t generation_ = 0;
    uint64_t next_attempt_ = 0;
    uint64_t next_store_generation_ = 0;
    StoreIdentityEntropyProvider entropy_provider_ = nullptr;
    std::deque<StoreIdentityRoot> recent_roots_;
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

    // When set, each launch receives a fresh StoreIdentity-rooted private
    // directory and must publish a structured READY lease.  The directory is
    // never reused between launches or Supervisor recreations.
    std::string lease_root;
    std::shared_ptr<LaunchIdentityAllocator> launch_identities;
};

struct ReadyLease {
    local::Identity identity{};
    uint64_t store_generation = 0;
    pid_t pid = -1;
    StoreIdentityRoot store_root{};
    uint64_t store_derivation_version = 0;
    CStoreGuid c_store_guid{};
    FStoreGuid f_store_guid{};
    std::string private_directory;
    std::string socket_path;
    Digest128 socket_path_digest{};
    dev_t listener_device = 0;
    ino_t listener_inode = 0;
    dev_t directory_device = 0;
    ino_t directory_inode = 0;

    [[nodiscard]] bool valid() const noexcept {
        if (identity.generation == 0 || identity.attempt == 0 ||
            store_generation == 0 || store_generation == std::numeric_limits<uint64_t>::max() ||
            pid <= 1 ||
            !store_root.valid() ||
            store_derivation_version != kStoreIdentityDerivationVersion ||
            !store_identity_guid_valid_for_role(c_store_guid.bytes,
                                                kStoreIdentityClientRole) ||
            !store_identity_guid_valid_for_role(f_store_guid.bytes,
                                                kStoreIdentityFileRole) ||
            c_store_guid != c_store_guid_for_root(store_root) ||
            f_store_guid != f_store_guid_for_root(store_root) ||
            c_store_guid == f_store_guid ||
            !detail::canonical_absolute_lease_path(private_directory) ||
            private_directory.size() + sizeof("/cache.sock") - 1 > local::kMaxUnixPath ||
            socket_path.size() != private_directory.size() + sizeof("/cache.sock") - 1 ||
            socket_path.compare(0, private_directory.size(), private_directory) != 0 ||
            socket_path[private_directory.size()] != '/' ||
            socket_path.compare(private_directory.size() + 1, sizeof("cache.sock") - 1,
                                "cache.sock") != 0 ||
            socket_path_digest != digest128(socket_path) || listener_device == 0 ||
            listener_inode == 0 || directory_device == 0 || directory_inode == 0)
            return false;
        return true;
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

    // Exact-handle STOP anchor, bounded group TERM, group/direct KILL, and
    // mandatory reap.  It is safe to call repeatedly and closes every
    // supervisor-owned descriptor.
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
    // every direct signal and to STOP/observe the live group leader before a
    // numeric PGID may be signalled.  A platform without the exact pidfd APIs
    // fails before launch; there is no numeric PID or unanchored-PGID fallback.
    int child_pidfd_ = -1;
    pid_t process_group_ = -1;
    bool process_group_owned_ = false;
    int ready_read_ = -1;
    int exec_read_ = -1;
    std::vector<std::chrono::steady_clock::time_point> restart_times_;
    std::optional<ReadyLease> pending_lease_;
    std::optional<ReadyLease> current_lease_;
    // The daemon owns this pre-bound listener until fork; the child receives
    // the sole CLOEXEC-cleared copy and adopts it after dropping privileges.
    int pending_listener_fd_ = -1;
};

} // namespace icecc::p50::sidecar
