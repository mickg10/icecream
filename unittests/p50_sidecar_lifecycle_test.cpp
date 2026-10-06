#include "cache/p50_sidecar_lifecycle.h"

#include "cache/p50_compat_child_handle.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace icecc::p50;
using namespace icecc::p50::sidecar;

namespace {

int failures = 0;

#define CHECK(condition, message)                                                   \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::cerr << "FAIL: " << message << "\n";                             \
            ++failures;                                                             \
        }                                                                           \
    } while (false)

ssize_t deterministic_entropy(void* destination, size_t size,
                              unsigned) noexcept {
    static uint64_t next = 1;
    if (size != 16) return -1;
    auto* bytes = static_cast<uint8_t*>(destination);
    const uint64_t value = next++;
    for (size_t i = 0; i != 8; ++i)
        bytes[i] = static_cast<uint8_t>(value >> (i * 8));
    for (size_t i = 8; i != size; ++i)
        bytes[i] = static_cast<uint8_t>(0xa0 + i);
    return static_cast<ssize_t>(size);
}

std::shared_ptr<LaunchIdentityAllocator> allocator(uint64_t generation) {
    return std::make_shared<LaunchIdentityAllocator>(generation, 1,
                                                     deterministic_entropy);
}

// Reads the process-state character (e.g. 'Z' for zombie, 'R' for running,
// 'S' for sleeping) from /proc/<pid>/stat. Returns 0 if the entry cannot be
// read at all, mirroring the same "unreadable means gone" signal already
// used by read_starttime_ticks(). The second field (comm) is itself
// parenthesized and may contain ')' characters, so the state character is
// taken as the first non-space character after the *last* ')' in the line,
// exactly as the kernel's own proc(5) parsing advice recommends.
char read_proc_state_char(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", static_cast<int>(pid));
    FILE* file = std::fopen(path, "r");
    if (file == nullptr)
        return 0;
    char buffer[512];
    const size_t read_count = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[read_count] = '\0';
    const char* close_paren = std::strrchr(buffer, ')');
    if (close_paren == nullptr || *(close_paren + 1) == '\0')
        return 0;
    const char* cursor = close_paren + 1;
    while (*cursor == ' ')
        ++cursor;
    return *cursor;
}

// After a consuming reap, /proc/<pid> may already be fully gone (state
// character 0), or -- far less likely, but possible under aggressive pid
// reuse -- the kernel may have already recycled the pid for an unrelated
// process by the time this is checked. Both outcomes mean "this exact
// child is gone": the second is distinguished from "still our zombie" the
// same way CompatInvariantViolation itself already does, by starttime --
// a recycled pid cannot report the original child's starttime.
bool proc_pid_gone(pid_t pid, unsigned long long original_starttime) {
    if (read_proc_state_char(pid) == 0)
        return true;
    unsigned long long now_starttime = 0;
    if (!icecc::p50::platform::read_starttime_ticks(pid, &now_starttime))
        return true;
    return now_starttime != original_starttime;
}

class TestKillDomainVerifier final : public KillDomainVerifier {
public:
    std::optional<KillDomainLease> capture(pid_t pid, pid_t pgid) noexcept override {
        const auto lease = issue(pid, pgid, ++serial_);
        if (lease.valid() && captured_count_ < captured_.size()) {
            captured_pids_[captured_count_] = pid;
            captured_[captured_count_] = lease;
            ++captured_count_;
        }
        return lease;
    }
    bool proves_absent(const KillDomainLease&, pid_t, pid_t pgid,
                       const LifecycleObservation& observation) const noexcept override {
        return observation.group == GroupObservation::Gone &&
               observation.observed_pgid == pgid;
    }

    std::optional<KillDomainLease> lease_for(pid_t pid) const noexcept {
        for (size_t index = 0; index != captured_count_; ++index) {
            if (captured_pids_[index] == pid)
                return captured_[index];
        }
        return std::nullopt;
    }

private:
    uint64_t serial_ = 0;
    std::array<pid_t, 32> captured_pids_{};
    std::array<std::optional<KillDomainLease>, 32> captured_{};
    size_t captured_count_ = 0;
};

class PermissiveKillDomainVerifier final : public KillDomainVerifier {
public:
    std::optional<KillDomainLease> capture(pid_t pid, pid_t pgid) noexcept override {
        const auto lease = issue(pid, pgid, ++serial_);
        if (lease.valid() && captured_count_ < captured_.size()) {
            captured_pids_[captured_count_] = pid;
            captured_[captured_count_] = lease;
            ++captured_count_;
        }
        return lease;
    }

    bool proves_absent(const KillDomainLease&, pid_t, pid_t,
                       const LifecycleObservation&) const noexcept override {
        return true;
    }

    std::optional<KillDomainLease> lease_for(pid_t pid) const noexcept {
        for (size_t index = 0; index != captured_count_; ++index) {
            if (captured_pids_[index] == pid)
                return captured_[index];
        }
        return std::nullopt;
    }

private:
    uint64_t serial_ = 0;
    std::array<pid_t, 8> captured_pids_{};
    std::array<std::optional<KillDomainLease>, 8> captured_{};
    size_t captured_count_ = 0;
};

void create_socket_node(const std::string& directory, const std::string& path) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    CHECK(!error, "create UNIX listener directory witness");
    (void)::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0, "create UNIX listener witness");
    struct sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", path.c_str());
    CHECK(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
          "bind UNIX listener witness");
    (void)::close(fd);
}

void remove_socket_node(const LifecycleIdentity& identity) {
    (void)::unlink((identity.private_directory + "/cache.sock").c_str());
}

ReadyLease ready_for(const LifecycleIdentity& identity, pid_t pid) {
    create_socket_node(identity.private_directory,
                       identity.private_directory + "/cache.sock");
    struct stat node {};
    CHECK(::lstat((identity.private_directory + "/cache.sock").c_str(), &node) == 0,
          "stat UNIX listener witness");
    ReadyLease lease;
    lease.identity = identity.control;
    lease.store_generation = identity.store_generation;
    lease.pid = pid;
    lease.store_root = identity.store_root;
    lease.store_derivation_version = kStoreIdentityDerivationVersion;
    lease.c_store_guid = identity.c_store_guid;
    lease.f_store_guid = identity.f_store_guid;
    lease.private_directory = identity.private_directory;
    lease.socket_path = identity.private_directory + "/cache.sock";
    lease.socket_path_digest = icecc::digest128(lease.socket_path);
    lease.listener_device = node.st_dev;
    lease.listener_inode = node.st_ino;
    lease.directory_device = node.st_dev;
    lease.directory_inode = node.st_ino;
    return lease;
}

std::string hex_guid(const Id128& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.bytes.size() * 2);
    for (const uint8_t byte : value.bytes) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

LifecycleObservation forked(
    pid_t pid,
    const std::shared_ptr<KillDomainVerifier>& verifier = {}) {
    LifecycleObservation observation;
    observation.exec = ExecObservation::Succeeded;
    observation.pid = pid;
    observation.observed_pgid = pid;
    if (verifier) {
        const auto lease = verifier->capture(pid, pid);
        if (lease.has_value())
            observation.group_domain = *lease;
    }
    return observation;
}

LaunchIncarnation incarnation_for(const LifecycleIdentity& identity) {
    LaunchIncarnation incarnation;
    incarnation.identity = identity.control;
    incarnation.store_generation = identity.store_generation;
    incarnation.store_root = identity.store_root;
    incarnation.c_store_guid = identity.c_store_guid;
    incarnation.f_store_guid = identity.f_store_guid;
    return incarnation;
}

ReapEvent exact_reap_for(const SidecarLifecycle& lifecycle, pid_t pid,
                         int pidfd, uint64_t registry_generation) {
    ReapEvent event;
    if (!lifecycle.identity().has_value())
        return event;
    event.owner = lifecycle.owner_key();
    event.pid = pid;
    event.status = 0;
    event.pidfd = pidfd;
    event.registry_generation = registry_generation;
    event.incarnation = incarnation_for(*lifecycle.identity());
    event.kernel_code = CLD_EXITED;
    event.kind = ReaperOwnerKind::Sidecar;
    return event;
}

bool queue_exact_reap(SidecarLifecycle& lifecycle, pid_t pid) {
    const int pidfd = 1000 + pid;
    const uint64_t registry_generation = static_cast<uint64_t>(pid) + 1;
    const auto mailbox = lifecycle.reap_mailbox().lock();
    if (!mailbox || !lifecycle.identity().has_value())
        return false;
    lifecycle.bind_reaper_identity(pidfd, registry_generation);
    return mailbox->enqueue(
        exact_reap_for(lifecycle, pid, pidfd, registry_generation));
}

void move_to_forked(SidecarLifecycle& lifecycle, pid_t pid,
                    std::chrono::steady_clock::time_point now,
                    const std::shared_ptr<KillDomainVerifier>& verifier = {}) {
    LifecycleObservation fork = forked(pid, verifier);
    fork.exec = ExecObservation::None;
    (void)lifecycle.advance(now, fork);
    (void)lifecycle.advance(now, forked(pid));
}

void move_to_ready(SidecarLifecycle& lifecycle, pid_t pid,
                   std::chrono::steady_clock::time_point now,
                   const std::shared_ptr<KillDomainVerifier>& verifier = {}) {
    move_to_forked(lifecycle, pid, now, verifier);
    const auto identity = *lifecycle.identity();
    LifecycleObservation ready;
    ready.ready = ReadyObservation::Complete;
    ready.store_generation = identity.store_generation;
    ready.ready_lease = ready_for(identity, pid);
    ready.observed_device = ready.ready_lease->listener_device;
    ready.observed_inode = ready.ready_lease->listener_inode;
    (void)lifecycle.advance(now, ready);
}

void test_lifecycle() {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::time_point{};
    SidecarLifecycleConfig config;
    config.control_generation = 91;
    config.private_root = "/tmp/icecc-p50-lifecycle";
    config.launch_timeout = std::chrono::milliseconds(10);
    config.ready_timeout = std::chrono::milliseconds(10);
    config.grace_timeout = std::chrono::milliseconds(10);
    config.kill_timeout = std::chrono::milliseconds(10);
    config.identities = allocator(config.control_generation);
    auto verifier = std::make_shared<TestKillDomainVerifier>();
    config.kill_domain_verifier = verifier;
    SidecarLifecycle lifecycle(config);
    CHECK(SidecarLifecycle::valid_config(config), "valid lifecycle configuration");

    auto launch = lifecycle.begin(t0);
    CHECK(launch.action == LifecycleAction::LaunchPrepared &&
              lifecycle.state() == LifecycleState::LaunchPrepared,
          "begin emits one launch preparation");
    const LifecycleIdentity first = launch.identity;
    CHECK(first.valid() && first.store_generation == config.control_generation &&
              first.control.generation == config.control_generation,
          "allocator mints the exact independent control/store incarnation");
    CHECK(lifecycle.begin(t0).action == LifecycleAction::None,
          "second begin cannot launch twice in one turn");

    LifecycleObservation fork = forked(321, verifier);
    fork.exec = ExecObservation::None;
    CHECK(lifecycle.advance(t0, fork).action == LifecycleAction::None &&
              lifecycle.state() == LifecycleState::ForkedAwaitExecAndReady,
          "dedicated pid/pgid enters exec/READY state");
    LifecycleObservation exec = forked(321);
    CHECK(lifecycle.advance(t0, exec).action == LifecycleAction::None,
          "exec success starts the independent READY deadline");
    LifecycleObservation partial;
    partial.ready = ReadyObservation::Partial;
    partial.ready_bytes = "READY v2 generation=";
    CHECK(lifecycle.advance(t0, partial).action == LifecycleAction::None,
          "trickle READY is bounded and does not publish");

    LifecycleObservation complete;
    complete.ready = ReadyObservation::Complete;
    complete.store_generation = first.store_generation;
    complete.ready_lease = ready_for(first, 321);
    complete.observed_device = complete.ready_lease->listener_device;
    complete.observed_inode = complete.ready_lease->listener_inode;
    SidecarLifecycle rejected(config);
    const auto rejected_launch = rejected.begin(t0);
    LifecycleObservation rejected_fork = forked(323);
    rejected_fork.exec = ExecObservation::None;
    (void)rejected.advance(t0, rejected_fork);
    (void)rejected.advance(t0, forked(323));
    LifecycleObservation dropped_store = complete;
    dropped_store.ready_lease = ready_for(rejected_launch.identity, 323);
    dropped_store.store_generation = 99;
    dropped_store.observed_device = dropped_store.ready_lease->listener_device;
    dropped_store.observed_inode = dropped_store.ready_lease->listener_inode;
    CHECK(rejected.advance(t0, dropped_store).action == LifecycleAction::Withdraw,
          "dropped/frozen store-generation READY variant fails closed");
    CHECK(lifecycle.advance(t0, complete).action == LifecycleAction::PublishReady &&
              lifecycle.state() == LifecycleState::Ready &&
              lifecycle.current_ready_lease().has_value(),
          "complete READY publishes the current lease");
    CHECK(lifecycle.next_deadline() == std::chrono::steady_clock::time_point{},
          "complete READY disarms the startup deadline");
    const dev_t first_device = complete.ready_lease->listener_device;
    const ino_t first_inode = complete.ready_lease->listener_inode;

    // The observation and the lease are two distinct joins.  A caller cannot
    // pair a current observation with a stale lease from another F-store
    // incarnation and acquire publication authority.
    SidecarLifecycle lease_generation_mismatch(config);
    (void)lease_generation_mismatch.begin(t0);
    move_to_forked(lease_generation_mismatch, 319, t0);
    const auto lease_generation_identity = *lease_generation_mismatch.identity();
    LifecycleObservation mismatched_lease_generation;
    mismatched_lease_generation.ready = ReadyObservation::Complete;
    mismatched_lease_generation.store_generation =
        lease_generation_identity.store_generation;
    mismatched_lease_generation.ready_lease =
        ready_for(lease_generation_identity, 319);
    mismatched_lease_generation.observed_device =
        mismatched_lease_generation.ready_lease->listener_device;
    mismatched_lease_generation.observed_inode =
        mismatched_lease_generation.ready_lease->listener_inode;
    ++mismatched_lease_generation.ready_lease->store_generation;
    CHECK(lease_generation_mismatch.advance(t0, mismatched_lease_generation).action ==
              LifecycleAction::Withdraw,
          "stale lease store generation cannot publish under a current observation");

    // A fabricated DEV/INO tuple cannot turn a different current pathname
    // node into a READY lease: publication performs its own lstat/type proof.
    SidecarLifecycle socket_substitution(config);
    (void)socket_substitution.begin(t0);
    move_to_forked(socket_substitution, 320, t0);
    const auto socket_identity = *socket_substitution.identity();
    LifecycleObservation forged_ready;
    forged_ready.ready = ReadyObservation::Complete;
    forged_ready.store_generation = socket_identity.store_generation;
    auto forged_lease = ready_for(socket_identity, 320);
    forged_ready.observed_device = forged_lease.listener_device;
    forged_ready.observed_inode = forged_lease.listener_inode;
    ++forged_lease.listener_inode;
    forged_ready.ready_lease = forged_lease;
    CHECK(socket_substitution.advance(t0, forged_ready).action ==
              LifecycleAction::Withdraw,
          "socket substitution cannot publish from caller-supplied inode");

    CentralChildReaperRegistry reaper;
    constexpr int lifecycle_pidfd = 1321;
    auto lifecycle_registration = reaper.register_owner(
        321, 321, lifecycle.owner_key(), lifecycle.reap_mailbox(),
        ChildHandleBackend::Native, 0, 0, lifecycle_pidfd,
        incarnation_for(first));
    CHECK(lifecycle_registration.valid(), "central registry owns one pid");
    CHECK(!reaper.register_owner(321, 321, lifecycle.owner_key(),
                                 lifecycle.reap_mailbox(),
                                 ChildHandleBackend::Native, 0, 0,
                                 lifecycle_pidfd, incarnation_for(first)).valid(),
          "one PID has one owner");
    SidecarLifecycle sentinel(config);
    auto sentinel_registration = reaper.register_owner(
        322, 322, sentinel.owner_key(), sentinel.reap_mailbox(),
        ChildHandleBackend::Native, 0, 0, 1322, incarnation_for(first));
    CHECK(sentinel_registration.valid(), "central registry accepts sentinel owner");
    CHECK(reaper.next_unobserved_pid().value_or(-1) == 321,
          "central reaper starts at the first owner");
    CHECK(reaper.next_unobserved_pid().value_or(-1) == 322,
          "central reaper rotates fairly to the sentinel");
    CHECK(!reaper.observe_child_reaped(321, 0, true),
          "caller-supplied numeric reap/ECHILD fails closed");
    lifecycle.bind_reaper_identity(
        lifecycle_pidfd, lifecycle_registration.registry_generation());
    const auto lifecycle_mailbox = lifecycle.reap_mailbox().lock();
    CHECK(lifecycle_mailbox && lifecycle_mailbox->enqueue(exact_reap_for(
              lifecycle, 321, lifecycle_pidfd,
              lifecycle_registration.registry_generation())),
          "exact registry-bound reap event reaches the lifecycle mailbox");
    lifecycle_registration.reset();
    CHECK(reaper.next_unobserved_pid().value_or(-1) == 322,
          "unregistered owner is deleted from the fairness set");

    CentralChildReaperRegistry capacity_registry;
    std::vector<CentralChildReaperRegistry::Registration> registrations;
    registrations.reserve(256);
    for (size_t index = 0; index != 256; ++index) {
        const pid_t pid = static_cast<pid_t>(10000 + index);
        auto token = capacity_registry.register_owner(
            pid, pid, ReaperOwnerKey{static_cast<uint64_t>(pid), 1},
            lifecycle.reap_mailbox(), ChildHandleBackend::Native, 0, 0,
            20000 + static_cast<int>(index), incarnation_for(first));
        CHECK(token.valid(), "registry accepts an owner below its fixed cap");
        registrations.push_back(std::move(token));
    }
    CHECK(capacity_registry.size() == 256 &&
              !capacity_registry
                   .register_owner(20000, 20000, ReaperOwnerKey{20000, 1},
                                    lifecycle.reap_mailbox(),
                                    ChildHandleBackend::Native, 0, 0, 20256,
                                    incarnation_for(first))
                   .valid(),
          "registry rejects the owner above its fixed total cap");

    // --- Slice-1 hardening requirement #1: register_owner()/
    // register_direct_worker() accept/reject matrix. The production fix
    // landed separately; this exercises every branch of its validation
    // gate (cache/p50_sidecar_lifecycle.cpp, register_owner()). ---
    {
        CentralChildReaperRegistry hardening_registry;
        const auto hardening_mailbox = lifecycle.reap_mailbox();

        CHECK(!hardening_registry
                   .register_owner(40001, 40001, ReaperOwnerKey{40001, 1},
                                    hardening_mailbox, ChildHandleBackend::Native,
                                    0, 0, /*pidfd=*/-1, incarnation_for(first))
                   .valid(),
              "Native backend with no pidfd is rejected");

        CHECK(!hardening_registry
                   .register_owner(40002, 40002, ReaperOwnerKey{40002, 1},
                                    hardening_mailbox, ChildHandleBackend::Compat,
                                    0, 0, /*pidfd=*/-1, incarnation_for(first),
                                    ReaperOwnerKind::DirectWorker,
                                    /*starttime_ticks=*/0)
                   .valid(),
              "Compat DirectWorker via register_owner is rejected with a zero starttime");
        CHECK(!hardening_registry
                   .register_owner(40003, 40003, ReaperOwnerKey{40003, 1},
                                    hardening_mailbox, ChildHandleBackend::Compat,
                                    0, 0, /*pidfd=*/-1, incarnation_for(first),
                                    ReaperOwnerKind::DirectWorker,
                                    /*starttime_ticks=*/555)
                   .valid(),
              "Compat DirectWorker via register_owner is rejected even with a "
              "nonzero starttime -- Compat has no covering observe/kill-domain "
              "path for direct workers");

        CHECK(!hardening_registry
                   .register_direct_worker(40004, 40004, ReaperOwnerKey{40004, 1},
                                            /*pidfd=*/-1, ChildHandleBackend::Compat,
                                            incarnation_for(first),
                                            /*starttime_ticks=*/0)
                   .valid(),
              "Compat DirectWorker via register_direct_worker is rejected with a "
              "zero starttime");
        CHECK(!hardening_registry
                   .register_direct_worker(40005, 40005, ReaperOwnerKey{40005, 1},
                                            /*pidfd=*/-1, ChildHandleBackend::Compat,
                                            incarnation_for(first),
                                            /*starttime_ticks=*/777)
                   .valid(),
              "Compat DirectWorker via register_direct_worker is rejected even "
              "with a nonzero starttime");

        CHECK(!hardening_registry
                   .register_owner(40006, 40006, ReaperOwnerKey{40006, 1},
                                    hardening_mailbox, ChildHandleBackend::Compat,
                                    0, 0, /*pidfd=*/-1, incarnation_for(first),
                                    ReaperOwnerKind::Sidecar,
                                    /*starttime_ticks=*/0)
                   .valid(),
              "Compat Sidecar with a zero starttime is rejected");

        CHECK(!hardening_registry
                   .register_owner(1, 1, ReaperOwnerKey{40007, 1},
                                    hardening_mailbox, ChildHandleBackend::Compat,
                                    0, 0, /*pidfd=*/-1, incarnation_for(first),
                                    ReaperOwnerKind::Sidecar,
                                    /*starttime_ticks=*/999)
                   .valid(),
              "Compat Sidecar with pid<=1 is rejected");

        CHECK(!hardening_registry
                   .register_owner(40008, 40008, ReaperOwnerKey{40008, 1},
                                    hardening_mailbox, ChildHandleBackend::Compat,
                                    0, 0, /*pidfd=*/7, incarnation_for(first),
                                    ReaperOwnerKind::Sidecar,
                                    /*starttime_ticks=*/999)
                   .valid(),
              "Compat Sidecar with a non-negative pidfd is rejected");

        auto hardening_accept = hardening_registry.register_owner(
            40009, 40009, ReaperOwnerKey{40009, 1}, hardening_mailbox,
            ChildHandleBackend::Compat, 0, 0, /*pidfd=*/-1,
            incarnation_for(first), ReaperOwnerKind::Sidecar,
            /*starttime_ticks=*/999);
        CHECK(hardening_accept.valid(),
              "Compat Sidecar with pid>1, pidfd<0 and a nonzero starttime is "
              "the one legitimate Compat registration and is accepted");

        // `backend` has no default value in register_owner()'s signature
        // (cache/p50_sidecar_lifecycle.h): every call above, and every call
        // elsewhere in this file, must name it explicitly. A call site that
        // omitted it would fail to compile rather than silently defaulting
        // to ChildHandleBackend::Native, so this is enforced at compile
        // time rather than by a runtime CHECK.
    }

    // --- Slice-1 hardening requirement #2: ReapEvent::exact_identity() and
    // the event-acceptance gates that depend on it. Requirement #1 above
    // hardened registration; this hardens the orthogonal question of
    // whether a *delivered* event is accepted. Three independent surfaces:
    //   (a) exact_identity() itself, as a pure predicate -- every conjunct
    //       flipped one at a time from a known-good baseline;
    //   (b) AttemptLeafIdentity::matches(const ReapEvent&), which must
    //       reject on !exact_identity() even when every field it compares
    //       explicitly is already a match;
    //   (c) SidecarLifecycle::consume_reap(), same shape as (b).
    // CentralChildReaperRegistry::reap_one_direct_worker() (cache/
    // p50_sidecar_lifecycle.cpp) and DaemonSidecarAdapter::
    // outer_observe_child_reaped(const ReapEvent&) (cache/
    // p50_daemon_sidecar_adapter.cpp) also gate on exact_identity(), but
    // reaching their exact_identity()-false branch needs a live process:
    // reap_one_direct_worker()'s event is assembled entirely from fields a
    // register_direct_worker() call already validated (requirement #1), so
    // the only way left to make it non-exact is a real wait() returning a
    // non-terminal kernel_code -- that belongs with requirement #4's
    // real-fork registry integration test, not here. The adapter's gate is
    // covered directly in unittests/p50_daemon_sidecar_adapter_test.cpp
    // (g_exact_identity_hardening_ok), piggybacked on that test's existing
    // real reaps rather than introducing a new one here.
    {
        const ReaperOwnerKey exact_owner{50001, 1};
        const LaunchIncarnation exact_incarnation = incarnation_for(first);
        auto baseline_event = [&]() {
            ReapEvent event;
            event.owner = exact_owner;
            event.pid = 50001;
            event.status = 0;
            event.echild = false;
            event.pidfd = 1;
            event.backend = ChildHandleBackend::Native;
            event.registry_generation = 1;
            event.incarnation = exact_incarnation;
            event.kernel_code = CLD_EXITED;
            event.kind = ReaperOwnerKind::Sidecar;
            event.starttime_ticks = 0;
            return event;
        };
        CHECK(baseline_event().exact_identity(),
              "exact_identity baseline (Native Sidecar) is itself exact");

        auto broken = baseline_event();
        broken.owner = ReaperOwnerKey{};
        CHECK(!broken.exact_identity(), "exact_identity rejects an invalid owner");

        broken = baseline_event();
        broken.pid = 1;
        CHECK(!broken.exact_identity(), "exact_identity rejects pid <= 1");

        broken = baseline_event();
        broken.pidfd = -1;
        CHECK(!broken.exact_identity(),
              "exact_identity rejects a Native event with no pidfd");

        broken = baseline_event();
        broken.backend = ChildHandleBackend::Compat;
        CHECK(!broken.exact_identity(),
              "exact_identity rejects a Compat event that still carries a "
              "non-negative pidfd");

        broken = baseline_event();
        broken.backend = ChildHandleBackend::Compat;
        broken.pidfd = -1;
        broken.starttime_ticks = 0;
        CHECK(!broken.exact_identity(),
              "exact_identity rejects a Compat event with a zero starttime");

        broken = baseline_event();
        broken.backend = ChildHandleBackend::Compat;
        broken.pidfd = -1;
        broken.starttime_ticks = 42;
        CHECK(broken.exact_identity(),
              "exact_identity accepts the one legitimate Compat shape "
              "(pidfd<0, nonzero starttime)");

        broken = baseline_event();
        broken.registry_generation = 0;
        CHECK(!broken.exact_identity(),
              "exact_identity rejects a zero registry_generation");

        broken = baseline_event();
        broken.incarnation = LaunchIncarnation{};
        CHECK(!broken.exact_identity(),
              "exact_identity rejects an invalid incarnation");

        broken = baseline_event();
        broken.kernel_code = 0;
        CHECK(!broken.exact_identity(),
              "exact_identity rejects a kernel_code outside {EXITED, KILLED, "
              "DUMPED}");

        broken = baseline_event();
        broken.kind = static_cast<ReaperOwnerKind>(2);
        CHECK(!broken.exact_identity(),
              "exact_identity rejects an out-of-range ReaperOwnerKind");

        broken = baseline_event();
        broken.kind = ReaperOwnerKind::DirectWorker;
        broken.starttime_ticks = 0;
        CHECK(!broken.exact_identity(),
              "exact_identity rejects a DirectWorker event with a zero "
              "starttime even when the backend is Native and carries a "
              "pidfd");

        broken = baseline_event();
        broken.kind = ReaperOwnerKind::DirectWorker;
        broken.starttime_ticks = 7;
        CHECK(broken.exact_identity(),
              "exact_identity accepts a Native DirectWorker event once "
              "starttime is supplied alongside the pidfd");

        // (b) AttemptLeafIdentity::matches(const ReapEvent&): construct a
        // valid leaf identity and a ReapEvent that matches every field the
        // function compares explicitly (owner, registry_generation, pidfd,
        // pid, incarnation, starttime_ticks), but break exact_identity()
        // through a field matches() never looks at directly (kernel_code).
        // If matches() returned true here, the exact_identity() call inside
        // it would be provably redundant with its own explicit comparisons.
        AttemptLeafIdentity leaf;
        leaf.owner = exact_owner;
        leaf.registry_generation = 1;
        leaf.incarnation = exact_incarnation;
        leaf.pidfd = 1;
        leaf.direct_worker_pid = 50001;
        leaf.process_starttime_ticks = 7;
        leaf.leaf_device = 1;
        leaf.leaf_inode = 1;
        leaf.leaf_path = "/tmp/icecc-p50-lifecycle-leaf";
        CHECK(leaf.valid(), "constructed AttemptLeafIdentity is itself valid");

        ReapEvent leaf_event = baseline_event();
        leaf_event.kind = ReaperOwnerKind::DirectWorker;
        leaf_event.starttime_ticks = 7;
        CHECK(leaf.matches(leaf_event),
              "AttemptLeafIdentity::matches accepts the fully exact event");

        ReapEvent leaf_event_broken = leaf_event;
        leaf_event_broken.kernel_code = 0;
        CHECK(!leaf_event_broken.exact_identity() &&
                  !leaf.matches(leaf_event_broken),
              "AttemptLeafIdentity::matches rejects on exact_identity() "
              "alone even though owner/registry_generation/pidfd/pid/"
              "incarnation/starttime still all match");

        // (c) SidecarLifecycle::consume_reap(): same shape as (b), using a
        // fresh lifecycle driven to Forked so child_pid_/pidfd_/
        // registry_generation_/identity_ are all bound, then breaking only
        // exact_identity() (via the Compat/pidfd combination) while leaving
        // every field consume_reap() compares explicitly unchanged.
        SidecarLifecycle consume_reap_hardening(config);
        (void)consume_reap_hardening.begin(t0);
        move_to_forked(consume_reap_hardening, 50002, t0);
        constexpr int consume_reap_pidfd = 1502;
        consume_reap_hardening.bind_reaper_identity(consume_reap_pidfd, 1502);
        const ReapEvent exact_consume_event = exact_reap_for(
            consume_reap_hardening, 50002, consume_reap_pidfd, 1502);
        CHECK(exact_consume_event.exact_identity(),
              "consume_reap hardening baseline event is itself exact");

        ReapEvent non_exact_consume_event = exact_consume_event;
        non_exact_consume_event.backend = ChildHandleBackend::Compat;
        CHECK(!non_exact_consume_event.exact_identity(),
              "consume_reap hardening event is deliberately made non-exact "
              "(Compat with a non-negative pidfd)");
        CHECK(!consume_reap_hardening.test_consume_reap(non_exact_consume_event),
              "consume_reap rejects a non-exact event even though owner/"
              "pid/pidfd/registry_generation/incarnation all still match");
        CHECK(consume_reap_hardening.test_consume_reap(exact_consume_event),
              "consume_reap accepts the same event once it is exact again");
    }

    // --- Slice-1 hardening requirement #3: a starttime mismatch observed by
    // CentralChildReaperRegistry::reap_one() for a Compat-backend owner is an
    // invariant violation, not a "no change"/retryable outcome. It must
    // reach SidecarLifecycle::advance() as a sticky, idempotent
    // LifecycleAction::FailedClosed -- never retried, never silently
    // swallowed as an ordinary (non-)reap. observe_compat_status() (cache/
    // p50_sidecar_lifecycle.cpp) reads /proc/<pid>/stat's starttime and
    // compares it to the caller-registered starttime_ticks *before* ever
    // calling waitid(), so this fires on the very first observation --
    // independent of whether the child has actually exited -- and a single
    // reap_one() call against a real forked child with a deliberately wrong
    // registered starttime is sufficient; no synchronized exit or WNOWAIT/
    // consume two-step is required.
    {
        CentralChildReaperRegistry compat_registry;
        SidecarLifecycleConfig compat_config = config;
        SidecarLifecycle compat_lifecycle(compat_config);
        CHECK(compat_lifecycle.begin(t0).action == LifecycleAction::LaunchPrepared,
              "compat invariant-violation test: begin emits one launch "
              "preparation");

        const pid_t compat_child = ::fork();
        CHECK(compat_child >= 0, "compat invariant-violation test: fork succeeds");
        if (compat_child == 0)
            ::_exit(0);

        unsigned long long real_starttime = 0;
        CHECK(icecc::p50::platform::read_starttime_ticks(compat_child,
                                                          &real_starttime) &&
                  real_starttime != 0,
              "compat invariant-violation test: the real starttime is "
              "readable for the live/zombie child");
        const uint64_t wrong_starttime =
            static_cast<uint64_t>(real_starttime) + 1;

        move_to_forked(compat_lifecycle, compat_child, t0);
        CHECK(compat_lifecycle.state() == LifecycleState::ForkedAwaitExecAndReady,
              "compat invariant-violation test: lifecycle reaches the forked "
              "state");

        auto compat_registration = compat_registry.register_owner(
            compat_child, compat_child, compat_lifecycle.owner_key(),
            compat_lifecycle.reap_mailbox(), ChildHandleBackend::Compat, 0, 0,
            /*pidfd=*/-1, incarnation_for(*compat_lifecycle.identity()),
            ReaperOwnerKind::Sidecar, /*starttime_ticks=*/wrong_starttime);
        CHECK(compat_registration.valid(),
              "compat invariant-violation test: a Compat registration with "
              "pid>1, pidfd<0 and a nonzero (if wrong) starttime is itself "
              "accepted -- register_owner has no way to know the real value");

        compat_lifecycle.bind_reaper_identity(
            -1, compat_registration.registry_generation());
        CHECK(compat_lifecycle.bound_registry_generation() ==
                  compat_registration.registry_generation(),
              "compat invariant-violation test: bind_reaper_identity binds "
              "the registry generation for a Compat (pidfd<0) owner");

        const auto violation_event =
            compat_registry.reap_one(compat_child, -1);
        CHECK(!violation_event.has_value(),
              "compat invariant-violation test: reap_one on a starttime "
              "mismatch publishes no ordinary ReapEvent");
        CHECK(compat_registry.size() == 0,
              "compat invariant-violation test: the registry drops the "
              "owner the instant it publishes the violation");

        CHECK(!compat_lifecycle.compat_invariant_violation().has_value(),
              "compat invariant-violation test: not yet latched before an "
              "advance() turn drains the mailbox");
        const auto compat_result = compat_lifecycle.advance(t0);
        CHECK(compat_result.action == LifecycleAction::FailedClosed &&
                  compat_lifecycle.state() == LifecycleState::FailedClosed,
              "compat invariant-violation test: a starttime mismatch fails "
              "closed on the turn it is observed, rather than being retried "
              "or treated as no change");
        CHECK(compat_lifecycle.compat_invariant_violation().has_value() &&
                  compat_lifecycle.compat_invariant_violation()->pid ==
                      compat_child &&
                  compat_lifecycle.compat_invariant_violation()
                          ->expected_starttime_ticks == wrong_starttime &&
                  compat_lifecycle.compat_invariant_violation()
                          ->observed_starttime_ticks ==
                      static_cast<uint64_t>(real_starttime),
              "compat invariant-violation test: the latched violation "
              "carries the exact expected (wrong) and observed (real) "
              "starttime pair");

        // Sticky and idempotent: a later turn must stay FailedClosed and
        // must not re-derive, clear, or replace the latch.
        const auto compat_second = compat_lifecycle.advance(t0);
        CHECK(compat_second.action == LifecycleAction::None &&
                  compat_lifecycle.state() == LifecycleState::FailedClosed,
              "compat invariant-violation test: FailedClosed is sticky and "
              "idempotent across a later turn");

        int wait_status = 0;
        CHECK(::waitpid(compat_child, &wait_status, 0) == compat_child,
              "compat invariant-violation test: cleanup reaps the real "
              "forked child so no zombie is leaked by the test itself");
    }

    // --- fixup! requirement (b): when /proc/<pid>/stat is genuinely
    // unreadable (the child has already been fully reaped and its /proc
    // entry is gone), the violation must carry the errno from the failed
    // read.  observed_starttime_ticks alone cannot distinguish "unreadable"
    // from a readable-but-zero stat line, so observed_errno is the
    // authoritative signal and must be nonzero on this path. ---
    {
        CentralChildReaperRegistry errno_registry;
        SidecarLifecycleConfig errno_config = config;
        SidecarLifecycle errno_lifecycle(errno_config);
        CHECK(errno_lifecycle.begin(t0).action == LifecycleAction::LaunchPrepared,
              "compat errno-capture test: begin emits one launch preparation");

        const pid_t errno_child = ::fork();
        CHECK(errno_child >= 0, "compat errno-capture test: fork succeeds");
        if (errno_child == 0)
            ::_exit(0);

        unsigned long long errno_child_starttime = 0;
        CHECK(icecc::p50::platform::read_starttime_ticks(
                  errno_child, &errno_child_starttime) &&
                  errno_child_starttime != 0,
              "compat errno-capture test: the real starttime is readable "
              "before the child is fully reaped");

        // Fully reap the child up front (no WNOWAIT): its /proc/<pid> entry
        // disappears immediately, before the registry ever observes it, so
        // the registered (once-real) starttime is now unreadable.
        int errno_wait_status = 0;
        CHECK(::waitpid(errno_child, &errno_wait_status, 0) == errno_child,
              "compat errno-capture test: the test itself fully reaps the "
              "child so /proc/<pid>/stat is gone before registration");

        move_to_forked(errno_lifecycle, errno_child, t0);
        CHECK(errno_lifecycle.state() == LifecycleState::ForkedAwaitExecAndReady,
              "compat errno-capture test: lifecycle reaches the forked state");

        auto errno_registration = errno_registry.register_owner(
            errno_child, errno_child, errno_lifecycle.owner_key(),
            errno_lifecycle.reap_mailbox(), ChildHandleBackend::Compat, 0, 0,
            /*pidfd=*/-1, incarnation_for(*errno_lifecycle.identity()),
            ReaperOwnerKind::Sidecar,
            /*starttime_ticks=*/static_cast<uint64_t>(errno_child_starttime));
        CHECK(errno_registration.valid(),
              "compat errno-capture test: registration with the real "
              "(now-unreadable) starttime is itself accepted");

        errno_lifecycle.bind_reaper_identity(
            -1, errno_registration.registry_generation());
        CHECK(errno_lifecycle.bound_registry_generation() ==
                  errno_registration.registry_generation(),
              "compat errno-capture test: bind_reaper_identity binds the "
              "registry generation for a Compat (pidfd<0) owner");

        const auto errno_violation_event =
            errno_registry.reap_one(errno_child, -1);
        CHECK(!errno_violation_event.has_value(),
              "compat errno-capture test: reap_one on an unreadable /proc "
              "entry publishes no ordinary ReapEvent");
        CHECK(errno_registry.size() == 0,
              "compat errno-capture test: the registry drops the owner the "
              "instant it publishes the violation");

        const auto errno_result = errno_lifecycle.advance(t0);
        CHECK(errno_result.action == LifecycleAction::FailedClosed &&
                  errno_lifecycle.state() == LifecycleState::FailedClosed,
              "compat errno-capture test: an unreadable /proc/<pid>/stat "
              "fails closed exactly like a mismatch does");
        const auto &errno_violation =
            errno_lifecycle.compat_invariant_violation();
        CHECK(errno_violation.has_value() &&
                  errno_violation->pid == errno_child,
              "compat errno-capture test: the latched violation names the "
              "right pid");
        // observed_readable, not observed_starttime_ticks or observed_errno,
        // is the authoritative "unreadable" signal: a parse-only failure (no
        // ')' found, or field 22 missing) leaves errno at 0, the same value a
        // successful-but-mismatched read reports, and a readable stat line
        // can itself report starttime 0. In the vanishingly unlikely race
        // where the OS has already reused this pid for a new, readable
        // process by the time the registry reads /proc/<pid>/stat, the
        // read succeeds but must report a different (mismatched) starttime
        // -- it is reusing the pid, not resurrecting the exact same process
        // -- so that outcome is accepted too.
        CHECK(errno_violation.has_value() &&
                  (!errno_violation->observed_readable ||
                   (errno_violation->observed_readable &&
                    errno_violation->observed_starttime_ticks !=
                        static_cast<uint64_t>(errno_child_starttime))),
              "compat errno-capture test: the latched violation is either "
              "genuinely unreadable, or (on pid-reuse) readable but "
              "reporting a starttime that mismatches the original child's");
        // Secondary, weaker check: when the read truly failed (the expected,
        // overwhelmingly likely outcome here), errno should name a real
        // "the process is gone" failure -- ENOENT (fopen on a vanished
        // /proc/<pid>) or ESRCH -- not some unrelated errno left over from
        // an unrelated call. A parse-only failure legitimately reports 0.
        CHECK(!errno_violation.has_value() || errno_violation->observed_readable ||
                  errno_violation->observed_errno == 0 ||
                  errno_violation->observed_errno == ENOENT ||
                  errno_violation->observed_errno == ESRCH,
              "compat errno-capture test: when unreadable, observed_errno "
              "is 0 (parse-only failure) or names the process actually "
              "being gone (ENOENT/ESRCH), not an unrelated errno");
    }

    // --- Slice-1 hardening requirement #4: a real-fork registry
    // integration test. This targets CentralChildReaperRegistry's own
    // two-call reap_one() protocol directly -- no adapter
    // (SidecarLifecycle::advance()/consume_reap_events()) is in the loop;
    // SidecarLifecycle is used here only exactly as the requirement #3 test
    // above already does, as the established way to mint a real
    // owner_key()/reap_mailbox()/incarnation() triple.
    //
    // Fork a child, register it as (Compat, Sidecar), and let it exit. The
    // first reap_one() call is the WNOWAIT peek: it must observe the child
    // while its zombie is still present (/proc/<pid>/stat state 'Z'), and
    // must not reap it -- the zombie must still be visible immediately
    // afterward. The caller's own process-group kill is then issued,
    // strictly before the consuming reap. The second reap_one() call is the
    // real kernel reap (WNOWAIT cleared): it must return a populated,
    // exact_identity() ReapEvent, and /proc/<pid> must be gone immediately
    // afterward. A third reap_one() call -- a second consume attempt, with
    // the entry still registered but already observed -- must be rejected
    // (nullopt). Only then is the Registration token dropped, and the
    // registry must end empty.
    {
        CentralChildReaperRegistry fork_registry;
        SidecarLifecycleConfig fork_config = config;
        SidecarLifecycle fork_lifecycle(fork_config);
        CHECK(fork_lifecycle.begin(t0).action == LifecycleAction::LaunchPrepared,
              "real-fork registry test: begin emits one launch preparation");

        const pid_t fork_child = ::fork();
        CHECK(fork_child >= 0, "real-fork registry test: fork succeeds");
        if (fork_child == 0) {
            // Become our own process-group leader so the parent's later
            // group-kill step targets a real, meaningful kernel group
            // rather than relying on caller-asserted pid==pgid alone.
            ::setpgid(0, 0);
            ::_exit(0);
        }

        unsigned long long fork_starttime = 0;
        CHECK(icecc::p50::platform::read_starttime_ticks(fork_child,
                                                           &fork_starttime) &&
                  fork_starttime != 0,
              "real-fork registry test: the real starttime is readable for "
              "the live/zombie child");

        move_to_forked(fork_lifecycle, fork_child, t0);
        CHECK(fork_lifecycle.state() == LifecycleState::ForkedAwaitExecAndReady,
              "real-fork registry test: lifecycle reaches the forked state");

        auto fork_registration = fork_registry.register_owner(
            fork_child, fork_child, fork_lifecycle.owner_key(),
            fork_lifecycle.reap_mailbox(), ChildHandleBackend::Compat, 0, 0,
            /*pidfd=*/-1, incarnation_for(*fork_lifecycle.identity()),
            ReaperOwnerKind::Sidecar,
            /*starttime_ticks=*/static_cast<uint64_t>(fork_starttime));
        CHECK(fork_registration.valid(),
              "real-fork registry test: a Compat Sidecar registration for "
              "the real forked child, with its real starttime, is accepted");
        CHECK(fork_registry.size() == 1,
              "real-fork registry test: the registry holds exactly the one "
              "owner just registered");

        // Bounded wait for the child to actually become a zombie before the
        // WNOWAIT peek, so the peek below is proven against a real zombie,
        // not a still-running child. setpgid() in the child happens-before
        // its _exit(), so by the time it is a zombie the process-group
        // change below is guaranteed to already be in effect. The deadline
        // is wall-clock (monotonic), not an attempt count, since H1 can be
        // heavily loaded under concurrent QA runs.
        bool became_zombie = false;
        const auto zombie_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < zombie_deadline) {
            if (read_proc_state_char(fork_child) == 'Z') {
                became_zombie = true;
                break;
            }
            ::usleep(1000);
        }
        CHECK(became_zombie,
              "real-fork registry test: the child reaches zombie state "
              "within the bounded wait");

        // First reap_one() call: the WNOWAIT peek. Must observe but not
        // reap -- no ReapEvent yet, and the zombie must still be present.
        const auto peek_event = fork_registry.reap_one(fork_child, -1);
        CHECK(!peek_event.has_value(),
              "real-fork registry test: the first reap_one() call (the "
              "WNOWAIT peek) emits no ReapEvent yet");
        CHECK(read_proc_state_char(fork_child) == 'Z',
              "real-fork registry test: the zombie is still present "
              "immediately after the WNOWAIT peek -- it was observed, not "
              "reaped");
        CHECK(fork_registry.size() == 1,
              "real-fork registry test: the owner remains registered after "
              "the WNOWAIT peek");

        // The group kill: issued strictly before the consuming reap below.
        // The child is already a zombie and cannot act on any signal, so
        // this call is about ordering, not effect; it must not disturb the
        // zombie's presence or the registry's state either way.
        const int group_kill_result = ::kill(-fork_child, SIGKILL);
        CHECK(group_kill_result == 0 || errno == ESRCH,
              "real-fork registry test: the group kill targeting the "
              "child's own process group completes before the consuming "
              "reap (a zombie cannot receive it, so success or ESRCH are "
              "both acceptable)");
        CHECK(read_proc_state_char(fork_child) == 'Z',
              "real-fork registry test: the zombie survives the group kill "
              "-- only the consuming reap below may remove it");

        // Second reap_one() call: the real, consuming reap (WNOWAIT
        // cleared). Must return a populated, well-formed ReapEvent, and the
        // zombie must be gone immediately afterward.
        const auto consume_event = fork_registry.reap_one(fork_child, -1);
        CHECK(consume_event.has_value() &&
                  consume_event->exact_identity() &&
                  consume_event->pid == fork_child &&
                  consume_event->backend == ChildHandleBackend::Compat &&
                  consume_event->kind == ReaperOwnerKind::Sidecar,
              "real-fork registry test: the second reap_one() call (the "
              "consuming reap) returns a populated, well-formed ReapEvent "
              "for exactly this child");
        CHECK(proc_pid_gone(fork_child, fork_starttime),
              "real-fork registry test: /proc/<pid> is gone (or, under "
              "pid-reuse, now reports a different starttime) immediately "
              "after the consuming reap -- no zombie remains");
        CHECK(fork_registry.size() == 1,
              "real-fork registry test: the owner entry itself is still "
              "present (now fully observed) until its Registration token "
              "is dropped");

        // Third reap_one() call: a second consume attempt, with the entry
        // still registered but already observed. Must be rejected outright.
        const auto second_consume_event =
            fork_registry.reap_one(fork_child, -1);
        CHECK(!second_consume_event.has_value(),
              "real-fork registry test: a second consume attempt on an "
              "already-observed entry is rejected");
        CHECK(fork_registry.size() == 1,
              "real-fork registry test: rejecting the second consume "
              "attempt does not disturb the still-registered entry");

        // Only now drop the Registration token: observed==true alone does
        // not erase the entry, so the registry only "ends empty" once the
        // RAII token itself is released.
        fork_registration.reset();
        CHECK(fork_registry.size() == 0,
              "real-fork registry test: the registry ends empty once the "
              "Registration token is dropped");
    }

    // --- fixup! requirement (a): a Compat invariant violation whose owner,
    // pid, registry generation, or incarnation does not match this exact
    // lifecycle's identity is a stale/foreign fact.  It must be dropped
    // without being latched onto this owner (consume_reap_events() must not
    // silently substitute or corrupt this owner's own state), and dropping
    // it must not stop this owner's own, unrelated lifecycle from
    // continuing normally. ---
    {
        SidecarLifecycleConfig mismatch_config = config;
        SidecarLifecycle mismatch_lifecycle(mismatch_config);
        CHECK(mismatch_lifecycle.begin(t0).action == LifecycleAction::LaunchPrepared,
              "owner-mismatch test: begin emits one launch preparation");

        move_to_forked(mismatch_lifecycle, 50001, t0);
        CHECK(mismatch_lifecycle.state() == LifecycleState::ForkedAwaitExecAndReady,
              "owner-mismatch test: lifecycle reaches the forked state");
        mismatch_lifecycle.bind_reaper_identity(-1, 777);
        CHECK(mismatch_lifecycle.bound_registry_generation() == 777,
              "owner-mismatch test: bind_reaper_identity binds this owner's "
              "own registry generation");

        const auto mismatch_identity = *mismatch_lifecycle.identity();
        const auto mismatch_mailbox = mismatch_lifecycle.reap_mailbox().lock();
        CHECK(mismatch_mailbox != nullptr,
              "owner-mismatch test: this owner's mailbox is alive");

        // A well-formed (exact_identity()==true) but foreign violation: it
        // carries a different pid, owner key, and registry generation than
        // this exact lifecycle holds, modeling a stale fact delivered after
        // some other owner already moved on.
        CompatInvariantViolation foreign_violation;
        foreign_violation.kind = ReaperOwnerKind::Sidecar;
        foreign_violation.pid = 50002;
        foreign_violation.owner = ReaperOwnerKey{999999, 1};
        foreign_violation.registry_generation = 778;
        foreign_violation.incarnation = incarnation_for(mismatch_identity);
        foreign_violation.expected_starttime_ticks = 42;
        foreign_violation.observed_starttime_ticks = 43;
        foreign_violation.observed_readable = true;
        foreign_violation.observed_errno = 0;
        CHECK(foreign_violation.exact_identity(),
              "owner-mismatch test: the fabricated violation is itself "
              "well-formed -- the mismatch is identity, not malformation");
        CHECK(mismatch_mailbox->enqueue_violation(foreign_violation),
              "owner-mismatch test: the single-slot violation channel "
              "accepts the fabricated foreign fact");

        CHECK(!mismatch_lifecycle.compat_invariant_violation().has_value(),
              "owner-mismatch test: not yet latched before an advance() "
              "turn drains the mailbox");
        const auto mismatch_result = mismatch_lifecycle.advance(t0);
        CHECK(!mismatch_lifecycle.compat_invariant_violation().has_value(),
              "owner-mismatch test: a foreign violation (wrong pid/owner/"
              "generation) is dropped rather than latched onto this owner");
        CHECK(mismatch_result.action != LifecycleAction::FailedClosed &&
                  mismatch_lifecycle.state() != LifecycleState::FailedClosed,
              "owner-mismatch test: this owner's own lifecycle is unaffected "
              "by a foreign violation delivered into its mailbox");
    }

    ReapMailbox bounded_mailbox;
    const ReapEvent mailbox_event = exact_reap_for(
        lifecycle, 321, lifecycle_pidfd,
        lifecycle.bound_registry_generation());
    bool mailbox_full = true;
    for (size_t index = 0; index != 16; ++index)
        mailbox_full = mailbox_full &&
                       bounded_mailbox.enqueue(mailbox_event);
    CHECK(mailbox_full &&
              !bounded_mailbox.enqueue(mailbox_event),
          "reap delivery is bounded rather than an unbounded event queue");

    auto withdraw = lifecycle.advance(t0);
    CHECK(withdraw.action == LifecycleAction::Withdraw &&
              lifecycle.state() == LifecycleState::TerminatingGrace &&
              !lifecycle.current_ready_lease().has_value(),
          "reaped leader withdraws before teardown");
    auto term = lifecycle.advance(t0);
    CHECK(term.action == LifecycleAction::SendTerm, "TERM is a separate outer-loop action");
    LifecycleObservation wrong_group;
    wrong_group.group = GroupObservation::Gone;
    wrong_group.observed_pgid = 999;
    wrong_group.path_absent = true;
    wrong_group.observed_path = first.private_directory;
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(10), wrong_group).action ==
              LifecycleAction::SendKill,
          "stale PGID proof cannot suppress TERM to KILL escalation");
    CHECK(lifecycle.state() == LifecycleState::TerminatingKill,
          "surviving helper keeps lifecycle in kill state");
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(11), wrong_group).action ==
              LifecycleAction::None,
          "KILL is emitted once and never duplicated by a later turn");
    LifecycleObservation group_gone = wrong_group;
    group_gone.observed_pgid = 321;
    const auto group_lease = verifier->lease_for(321);
    CHECK(group_lease.has_value(), "verifier retains the exact fork lease");
    group_gone.group_domain = *group_lease;
    group_gone.observed_device = first_device;
    group_gone.observed_inode = first_inode;
    remove_socket_node(*lifecycle.identity());
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(11), group_gone).action == LifecycleAction::None &&
              lifecycle.state() == LifecycleState::ReapAndGroupCheck,
          "leader reap is insufficient until exact PGID ESRCH");
    LifecycleObservation stale_path = group_gone;
    stale_path.observed_path = "/tmp/stale-other-incarnation";
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(11), stale_path).action == LifecycleAction::CleanupPath &&
              lifecycle.state() == LifecycleState::ReapAndGroupCheck,
          "stale path callback requests bounded cleanup but cannot authorize replacement");
    LifecycleObservation same_path_new_node = group_gone;
    same_path_new_node.observed_device = first_device;
    same_path_new_node.observed_inode = first_inode + 1;
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(11), same_path_new_node).action == LifecycleAction::CleanupPath &&
              lifecycle.state() == LifecycleState::ReapAndGroupCheck,
          "replacement at the same pathname requests cleanup but cannot prove old-node absence");
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(11), group_gone).action == LifecycleAction::RetryEligible &&
              lifecycle.state() == LifecycleState::RetryEligible,
          "exact group and path proof authorizes retry");

    // A legacy request can arrive before the outer loop has observed a fork,
    // so there is no process or process-group lease to tear down.  The
    // reducer enters the terminal legacy state directly; a later permissive
    // GroupObservation::Gone/path-absent callback remains inert and cannot
    // manufacture a process teardown transition.
    SidecarLifecycleConfig pre_fork_config = config;
    // Keep this witness independent of the allocator's prior launch history;
    // the invariant under test is the absence of a fork/group lease, not
    // allocator exhaustion from the earlier lifecycle rows.
    pre_fork_config.identities = allocator(config.control_generation);
    SidecarLifecycle pre_fork_legacy(pre_fork_config);
    (void)pre_fork_legacy.begin(t0);
    LifecycleObservation pre_fork_request;
    pre_fork_request.request_legacy = true;
    CHECK(pre_fork_legacy.advance(t0, pre_fork_request).action ==
              LifecycleAction::EnterDegradedLegacy &&
              pre_fork_legacy.state() == LifecycleState::DegradedLegacy &&
              !pre_fork_legacy.current_ready_lease().has_value(),
          "pre-fork legacy request terminates without inventing a teardown");
    LifecycleObservation no_group_proof;
    no_group_proof.group = GroupObservation::Gone;
    no_group_proof.path_absent = true;
    no_group_proof.observed_path = pre_fork_legacy.identity()->private_directory;
    CHECK(pre_fork_legacy.advance(t0 + std::chrono::milliseconds(1), no_group_proof).action ==
              LifecycleAction::None &&
              pre_fork_legacy.state() == LifecycleState::DegradedLegacy,
          "no group proof cannot alter the terminal pre-fork legacy state");

    // Even an injected verifier that returns true for every callback cannot
    // authorize a fabricated lease or a mismatched observed PGID.  The
    // reducer independently binds both facts to the exact fork lease.
    SidecarLifecycleConfig permissive_config = config;
    auto permissive_verifier = std::make_shared<PermissiveKillDomainVerifier>();
    permissive_config.kill_domain_verifier = permissive_verifier;
    SidecarLifecycle permissive_group(permissive_config);
    (void)permissive_group.begin(t0);
    move_to_ready(permissive_group, 352, t0, permissive_verifier);
    LifecycleObservation permissive_request;
    permissive_request.request_replacement = true;
    CHECK(permissive_group.advance(t0, permissive_request).action ==
              LifecycleAction::Withdraw &&
              permissive_group.advance(t0).action == LifecycleAction::SendTerm,
          "permissive verifier witness starts bounded teardown");
    CHECK(queue_exact_reap(permissive_group, 352),
          "permissive verifier witness queues exact leader reap");
    LifecycleObservation mismatched_before_grace;
    mismatched_before_grace.group = GroupObservation::Gone;
    mismatched_before_grace.observed_pgid = 999;
    mismatched_before_grace.group_domain = *permissive_verifier->lease_for(352);
    CHECK(permissive_group.advance(t0 + std::chrono::milliseconds(1),
                                   mismatched_before_grace).action == LifecycleAction::None &&
              permissive_group.state() == LifecycleState::TerminatingGrace,
          "permissive verifier cannot authorize a mismatched PGID before grace expiry");
    LifecycleObservation fabricated_group;
    fabricated_group.group = GroupObservation::Gone;
    fabricated_group.observed_pgid = 352;
    fabricated_group.group_domain = *permissive_verifier->capture(353, 353);
    CHECK(permissive_group.advance(t0 + std::chrono::milliseconds(10),
                                   fabricated_group).action == LifecycleAction::SendKill,
          "permissive verifier cannot authorize a fabricated lease identity");

    LifecycleObservation mismatched_pgid = fabricated_group;
    mismatched_pgid.group_domain = *permissive_verifier->lease_for(352);
    mismatched_pgid.observed_pgid = 999;
    CHECK(permissive_group.advance(t0 + std::chrono::milliseconds(11),
                                   mismatched_pgid).action == LifecycleAction::None,
          "permissive verifier cannot authorize a mismatched observed PGID");

    // Numeric PGID/Gone is not a reusable kill-domain proof.  A zombie leader
    // or surviving helper can keep the number present, and a later group can
    // reuse it after reap; an absent/mismatched external lease must fail
    // closed rather than authorize a replacement.
    SidecarLifecycleConfig rejecting_config = config;
    rejecting_config.kill_domain_verifier.reset();
    SidecarLifecycle ambiguous_group(rejecting_config);
    (void)ambiguous_group.begin(t0);
    move_to_ready(ambiguous_group, 350, t0);
    LifecycleObservation ambiguous_request;
    ambiguous_request.request_replacement = true;
    CHECK(ambiguous_group.advance(t0, ambiguous_request).action ==
              LifecycleAction::Withdraw &&
              ambiguous_group.advance(t0).action == LifecycleAction::SendTerm,
          "ambiguous PGID teardown withdraws before TERM");
    CHECK(queue_exact_reap(ambiguous_group, 350),
          "ambiguous PGID row queues exact leader reap");
    LifecycleObservation numeric_gone;
    numeric_gone.group = GroupObservation::Gone;
    numeric_gone.observed_pgid = 350;
    numeric_gone.path_absent = true;
    numeric_gone.observed_path = ambiguous_group.identity()->private_directory;
    numeric_gone.observed_device = ambiguous_group.identity()->listener_device;
    numeric_gone.observed_inode = ambiguous_group.identity()->listener_inode;
    remove_socket_node(*ambiguous_group.identity());
    // Deliberately no valid group_domain: this is exactly the zombie/reuse
    // ambiguity that numeric kill(-pgid, 0) cannot resolve.
    CHECK(ambiguous_group.advance(t0 + std::chrono::milliseconds(10), numeric_gone).action ==
              LifecycleAction::SendKill,
          "numeric PGID/Gone cannot suppress KILL without a domain lease");
    CHECK(ambiguous_group.advance(t0 + std::chrono::milliseconds(20), numeric_gone).action ==
              LifecycleAction::FailedClosed &&
              ambiguous_group.state() == LifecycleState::FailedClosed &&
              !ambiguous_group.current_ready_lease().has_value(),
          "zombie/reused PGID ambiguity fails closed with capacity withheld");

    SidecarLifecycle reused_group(rejecting_config);
    (void)reused_group.begin(t0);
    move_to_ready(reused_group, 351, t0);
    CHECK(reused_group.advance(t0, ambiguous_request).action == LifecycleAction::Withdraw &&
              reused_group.advance(t0).action == LifecycleAction::SendTerm,
          "reused-PGID witness starts bounded teardown");
    CHECK(queue_exact_reap(reused_group, 351),
          "reused-PGID witness queues exact leader reap");
    LifecycleObservation mismatched_domain = numeric_gone;
    mismatched_domain.observed_pgid = 351;
    mismatched_domain.observed_path = reused_group.identity()->private_directory;
    mismatched_domain.observed_device = reused_group.identity()->listener_device;
    mismatched_domain.observed_inode = reused_group.identity()->listener_inode;
    CHECK(reused_group.advance(t0 + std::chrono::milliseconds(10), mismatched_domain).action ==
              LifecycleAction::SendKill &&
              reused_group.advance(t0 + std::chrono::milliseconds(20), mismatched_domain).action ==
                  LifecycleAction::FailedClosed,
          "reused numeric PGID with a different domain lease fails closed");

    auto replacement = lifecycle.begin(t0);
    CHECK(replacement.action == LifecycleAction::LaunchPrepared &&
              replacement.identity.control.generation == first.control.generation &&
              replacement.identity.control.attempt != first.control.attempt &&
              replacement.identity.store_generation != first.store_generation &&
              replacement.identity.store_root != first.store_root &&
              replacement.identity.private_directory != first.private_directory,
          "replacement burns attempt/store generation/root/path without control rotation");

    // Reap-before-READY and same-turn waitable/identity-loss rows must all
    // withdraw rather than publishing a lease for a dead incarnation.
    SidecarLifecycle reap_first(config);
    (void)reap_first.begin(t0);
    move_to_forked(reap_first, 401, t0);
    const auto reap_first_identity = *reap_first.identity();
    CHECK(queue_exact_reap(reap_first, 401),
          "exact leader reap is queued before READY");
    LifecycleObservation reap_first_ready;
    reap_first_ready.ready = ReadyObservation::Complete;
    reap_first_ready.store_generation = reap_first_identity.store_generation;
    reap_first_ready.ready_lease = ready_for(reap_first_identity, 401);
    reap_first_ready.observed_device =
        reap_first_ready.ready_lease->listener_device;
    reap_first_ready.observed_inode =
        reap_first_ready.ready_lease->listener_inode;
    CHECK(reap_first.advance(t0, reap_first_ready).action == LifecycleAction::Withdraw &&
              !reap_first.current_ready_lease().has_value(),
          "reap-before-READY cannot publish stale lease");

    SidecarLifecycle waitable_ready(config);
    (void)waitable_ready.begin(t0);
    move_to_forked(waitable_ready, 402, t0);
    const auto waitable_identity = *waitable_ready.identity();
    LifecycleObservation waitable = reap_first_ready;
    waitable.ready_lease = ready_for(waitable_identity, 402);
    waitable.store_generation = waitable_identity.store_generation;
    waitable.observed_device = waitable.ready_lease->listener_device;
    waitable.observed_inode = waitable.ready_lease->listener_inode;
    waitable.child_waitable = true;
    CHECK(waitable_ready.advance(t0, waitable).action == LifecycleAction::Withdraw,
          "same-turn waitable plus READY loses publication race");

    SidecarLifecycle lost_ready(config);
    (void)lost_ready.begin(t0);
    move_to_forked(lost_ready, 403, t0);
    const auto lost_identity = *lost_ready.identity();
    LifecycleObservation lost = waitable;
    lost.ready_lease = ready_for(lost_identity, 403);
    lost.store_generation = lost_identity.store_generation;
    lost.observed_device = lost.ready_lease->listener_device;
    lost.observed_inode = lost.ready_lease->listener_inode;
    lost.child_waitable = false;
    lost.identity_lost = true;
    CHECK(lost_ready.advance(t0, lost).action == LifecycleAction::Withdraw,
          "same-turn identity loss plus READY loses publication race");

    SidecarLifecycle reap_after_ready(config);
    (void)reap_after_ready.begin(t0);
    move_to_ready(reap_after_ready, 404, t0);
    CHECK(reap_after_ready.current_ready_lease().has_value(),
          "ordering witness starts READY");
    CHECK(queue_exact_reap(reap_after_ready, 404),
          "exact reap after publication is queued for the same owner");
    CHECK(reap_after_ready.current_ready_lease().has_value(),
          "lease remains until owner linearization turn");
    CHECK(reap_after_ready.advance(t0).action == LifecycleAction::Withdraw &&
              !reap_after_ready.current_ready_lease().has_value(),
          "READY-vs-reap ordering withdraws before teardown");

    // RAII/value-event registration has no raw lifecycle pointer.  It also
    // proves exact listener-node capture, unregister, PID reuse, stale-key
    // rejection, and destroyed-owner rejection under the custom sanitizer.
    CentralChildReaperRegistry lifetime_registry;
    auto owner = std::make_unique<SidecarLifecycle>(config);
    (void)owner->begin(t0);
    move_to_forked(*owner, 700, t0);
    const ReaperOwnerKey old_key = owner->owner_key();
    constexpr int old_pidfd = 1700;
    auto registration = lifetime_registry.register_owner(
        700, 700, old_key, owner->reap_mailbox(), ChildHandleBackend::Native,
        first_device, first_inode, old_pidfd,
        incarnation_for(*owner->identity()));
    CHECK(registration.valid() && lifetime_registry.size() == 1,
          "RAII registration owns exact PID/generation");
    const auto captured_node = lifetime_registry.listener_node(700, old_key);
    CHECK(captured_node.has_value() && captured_node->first == first_device &&
              captured_node->second == first_inode,
          "registry captures exact listener device/inode");
    registration.reset();
    CHECK(lifetime_registry.size() == 0 &&
              !lifetime_registry.observe_child_reaped(700, old_key, 0, true),
          "explicit unregister retires old owner exactly");
    auto reused = std::make_unique<SidecarLifecycle>(config);
    (void)reused->begin(t0);
    move_to_forked(*reused, 700, t0);
    const ReaperOwnerKey new_key = reused->owner_key();
    constexpr int reused_pidfd = 1701;
    auto reused_registration = lifetime_registry.register_owner(
        700, 700, new_key, reused->reap_mailbox(), ChildHandleBackend::Native,
        first_device, first_inode, reused_pidfd,
        incarnation_for(*reused->identity()));
    reused->bind_reaper_identity(
        reused_pidfd, reused_registration.registry_generation());
    auto reused_mailbox = reused->reap_mailbox().lock();
    ReapEvent stale_reap = exact_reap_for(
        *reused, 700, reused_pidfd,
        reused_registration.registry_generation());
    stale_reap.owner = old_key;
    CHECK(reused_registration.valid() && reused_mailbox &&
              reused_mailbox->enqueue(stale_reap) &&
              reused->advance(t0).action == LifecycleAction::None &&
              reused->state() == LifecycleState::ForkedAwaitExecAndReady &&
              !lifetime_registry.observe_child_reaped(700, old_key, 0, true) &&
              !lifetime_registry.observe_child_reaped(700, new_key, 0, true),
          "stale generation cannot poison reused PID and scalar reap stays closed");
    CHECK(lifetime_registry.unregister_owner(700, new_key) &&
              !lifetime_registry.unregister_owner(700, new_key),
          "unregister is exact and non-idempotent for stale token");
    reused_registration.reset();
    auto dying = std::make_unique<SidecarLifecycle>(config);
    (void)dying->begin(t0);
    move_to_forked(*dying, 701, t0);
    const ReaperOwnerKey dying_key = dying->owner_key();
    auto dying_registration = lifetime_registry.register_owner(
        701, 701, dying_key, dying->reap_mailbox(), ChildHandleBackend::Native,
        first_device, first_inode, 1702, incarnation_for(*dying->identity()));
    dying.reset();
    CHECK(!lifetime_registry.observe_child_reaped(701, dying_key, 0, true),
          "destroyed owner rejects reap without UAF");
    dying_registration.reset();

    // One absolute teardown deadline bounds TERM, KILL and proof.  A surviving
    // helper that ignores TERM receives one KILL, then reaches FailedClosed.
    SidecarLifecycle deadline_lifecycle(config);
    (void)deadline_lifecycle.begin(t0);
    move_to_ready(deadline_lifecycle, 500, t0);
    LifecycleObservation replace_request;
    replace_request.request_replacement = true;
    CHECK(deadline_lifecycle.advance(t0, replace_request).action ==
              LifecycleAction::Withdraw,
          "replacement withdraws before TERM");
    CHECK(deadline_lifecycle.advance(t0).action == LifecycleAction::SendTerm,
          "TERM is emitted for surviving helper");
    CHECK(deadline_lifecycle.advance(t0 + std::chrono::milliseconds(10)).action ==
              LifecycleAction::SendKill && deadline_lifecycle.kill_sent(),
          "TERM grace escalates to one KILL");
    CHECK(deadline_lifecycle.advance(t0 + std::chrono::milliseconds(20)).action ==
              LifecycleAction::FailedClosed &&
              deadline_lifecycle.state() == LifecycleState::FailedClosed &&
              !deadline_lifecycle.current_ready_lease().has_value(),
          "residue at absolute deadline reaches terminal FailedClosed");

    SidecarLifecycle legacy_lifecycle(config);
    (void)legacy_lifecycle.begin(t0);
    move_to_ready(legacy_lifecycle, 501, t0, verifier);
    const auto legacy_ready_lease = *legacy_lifecycle.current_ready_lease();
    LifecycleObservation legacy_request;
    legacy_request.request_legacy = true;
    CHECK(legacy_lifecycle.advance(t0, legacy_request).action == LifecycleAction::Withdraw &&
              legacy_lifecycle.state() == LifecycleState::TerminatingGrace,
          "request_legacy withdraws live child instead of abandoning it");
    CHECK(legacy_lifecycle.advance(t0).action == LifecycleAction::SendTerm,
          "request_legacy drives TERM");
    CHECK(queue_exact_reap(legacy_lifecycle, 501),
          "legacy teardown queues exact leader reap");
    LifecycleObservation legacy_gone;
    legacy_gone.group = GroupObservation::Gone;
    legacy_gone.observed_pgid = 501;
    legacy_gone.group_domain = *verifier->lease_for(501);
    legacy_gone.path_absent = true;
    legacy_gone.observed_path = legacy_lifecycle.identity()->private_directory;
    legacy_gone.observed_device = legacy_ready_lease.listener_device;
    legacy_gone.observed_inode = legacy_ready_lease.listener_inode;
    remove_socket_node(*legacy_lifecycle.identity());
    (void)legacy_lifecycle.advance(t0 + std::chrono::milliseconds(10), legacy_gone);
    CHECK(legacy_lifecycle.advance(t0 + std::chrono::milliseconds(10), legacy_gone).action ==
              LifecycleAction::EnterDegradedLegacy &&
              legacy_lifecycle.state() == LifecycleState::DegradedLegacy,
          "request_legacy degrades only after exact teardown proof");

    ExecObservation parsed_exec = ExecObservation::None;
    CHECK(parse_exec_status("EXEC\n", parsed_exec) &&
              parsed_exec == ExecObservation::Succeeded,
          "exec parser accepts exact success frame");
    CHECK(!parse_exec_status("EXEC\nX", parsed_exec), "exec parser rejects trailing bytes");
    const ReadyLease parsed_fixture = ready_for(replacement.identity, 323);
    const std::string socket = parsed_fixture.socket_path;
    const std::string ready_wire =
        "READY v2 generation=" + std::to_string(replacement.identity.control.generation) +
        " attempt=" + std::to_string(replacement.identity.control.attempt) +
        " F_STORE_GENERATION=" +
        std::to_string(replacement.identity.store_generation) +
        " DERIVATION_VERSION=1 pid=323 C_STORE_GUID=" +
        hex_guid(replacement.identity.c_store_guid) + " F_STORE_GUID=" +
        hex_guid(replacement.identity.f_store_guid) + " PATH=" + socket +
        " DIGEST=" + icecc::digest128_hex(icecc::digest128(socket)) +
        " DEV=" + std::to_string(parsed_fixture.listener_device) +
        " INO=" + std::to_string(parsed_fixture.listener_inode) + "\n";
    ReadyLease parsed_lease;
    CHECK(parse_ready_frame(ready_wire, replacement.identity, 323, parsed_lease) &&
              parsed_lease.valid() &&
              parsed_lease.store_generation == replacement.identity.store_generation,
          "structured READY parser validates the canonical production lease tuple");
    std::string stale_ready_key = ready_wire;
    const size_t generation_key = stale_ready_key.find("F_STORE_GENERATION");
    CHECK(generation_key != std::string::npos, "canonical READY key is present");
    stale_ready_key.replace(generation_key, std::string_view("F_STORE_GENERATION").size(),
                            "STORE_GENERATION");
    CHECK(!parse_ready_frame(stale_ready_key, replacement.identity, 323, parsed_lease),
          "obsolete READY store-generation key is rejected");
    remove_socket_node(replacement.identity);

    // Item B: a terminal DegradedLegacy state must not keep a stale deadline_.
    // The daemon's outer poll folds next_deadline() into its timeout; a stale
    // value forces poll(timeout=0) at 100% CPU forever.  Verify the deadline
    // is cleared on every path that enters DegradedLegacy.
    {
        SidecarLifecycle degraded(config);
        (void)degraded.begin(t0);
        move_to_ready(degraded, 600, t0, verifier);
        const auto degraded_lease = *degraded.current_ready_lease();
        LifecycleObservation legacy_req;
        legacy_req.request_legacy = true;
        (void)degraded.advance(t0, legacy_req);
        (void)degraded.advance(t0);  // SendTerm
        CHECK(queue_exact_reap(degraded, 600), "degraded witness queues reap");
        LifecycleObservation gone;
        gone.group = GroupObservation::Gone;
        gone.observed_pgid = 600;
        gone.group_domain = *verifier->lease_for(600);
        gone.path_absent = true;
        gone.observed_path = degraded.identity()->private_directory;
        gone.observed_device = degraded_lease.listener_device;
        gone.observed_inode = degraded_lease.listener_inode;
        remove_socket_node(*degraded.identity());
        (void)degraded.advance(t0 + std::chrono::milliseconds(10), gone);
        CHECK(degraded.advance(t0 + std::chrono::milliseconds(10), gone).action ==
                  LifecycleAction::EnterDegradedLegacy &&
                  degraded.state() == LifecycleState::DegradedLegacy,
              "degraded witness reaches DegradedLegacy");
        CHECK(degraded.next_deadline() == std::chrono::steady_clock::time_point{},
              "DegradedLegacy clears the stale deadline (item B)");
        (void)degraded.advance(t0 + std::chrono::milliseconds(100));
        CHECK(degraded.next_deadline() == std::chrono::steady_clock::time_point{},
              "DegradedLegacy deadline stays clear after advance");
    }

    // Item B: FailedClosed must also clear the deadline.
    {
        SidecarLifecycle failed(config);
        (void)failed.begin(t0);
        move_to_ready(failed, 601, t0, verifier);
        LifecycleObservation replace;
        replace.request_replacement = true;
        (void)failed.advance(t0, replace);
        (void)failed.advance(t0);       // SendTerm
        CHECK(failed.advance(t0 + std::chrono::milliseconds(100)).action ==
                  LifecycleAction::FailedClosed &&
                  failed.state() == LifecycleState::FailedClosed,
              "teardown deadline reaches FailedClosed");
        CHECK(failed.next_deadline() == std::chrono::steady_clock::time_point{},
              "FailedClosed clears the stale deadline (item B)");
    }

    // Item C: a healthy READY resets the launch-attempt budget so a
    // long-lived F survives occasional silent replacements without
    // permanently degrading after max_attempts losses.
    {
        SidecarLifecycle budget(config);
        (void)budget.begin(t0);
        CHECK(budget.attempts() == 1, "first begin consumes one attempt");
        move_to_ready(budget, 602, t0, verifier);
        CHECK(budget.attempts() == 0,
              "healthy READY resets the attempt budget (item C)");
        const auto budget_lease = *budget.current_ready_lease();
        LifecycleObservation replace;
        replace.request_replacement = true;
        (void)budget.advance(t0, replace);
        (void)budget.advance(t0);       // SendTerm
        CHECK(queue_exact_reap(budget, 602), "budget witness queues reap");
        LifecycleObservation gone;
        gone.group = GroupObservation::Gone;
        gone.observed_pgid = 602;
        gone.group_domain = *verifier->lease_for(602);
        gone.path_absent = true;
        gone.observed_path = budget.identity()->private_directory;
        gone.observed_device = budget_lease.listener_device;
        gone.observed_inode = budget_lease.listener_inode;
        remove_socket_node(*budget.identity());
        (void)budget.advance(t0 + std::chrono::milliseconds(10), gone);
        CHECK(budget.advance(t0 + std::chrono::milliseconds(10), gone).action ==
                  LifecycleAction::RetryEligible &&
                  budget.state() == LifecycleState::RetryEligible,
              "budget witness reaches RetryEligible after teardown");
        CHECK(budget.attempts() == 0,
              "RetryEligible after healthy Ready still has budget reset");
        auto relaunch = budget.begin(t0 + std::chrono::milliseconds(20));
        CHECK(relaunch.action == LifecycleAction::LaunchPrepared,
              "budget refilled after healthy Ready allows relaunch (item C)");
    }

    // Item C: a crash loop that never reaches Ready still exhausts the
    // lifecycle's own attempt budget and degrades.  The adapter's windowed
    // reserve_outer_restart is the outer bound on crash-loop rate; the
    // lifecycle's attempts_ is the inner bound when Ready never lands.
    {
        SidecarLifecycleConfig crash_config = config;
        crash_config.identities = allocator(config.control_generation);
        crash_config.max_attempts = 3;
        SidecarLifecycle crash(crash_config);
        for (uint32_t i = 0; i < crash_config.max_attempts; ++i) {
            (void)crash.begin(t0 + std::chrono::milliseconds(i * 100));
            LifecycleObservation fail;
            fail.exec = ExecObservation::Failed;
            fail.path_absent = true;
            (void)crash.advance(t0 + std::chrono::milliseconds(i * 100), fail);
            if (crash.state() == LifecycleState::DegradedLegacy)
                break;
        }
        CHECK(crash.state() == LifecycleState::DegradedLegacy,
              "crash loop without Ready exhausts attempts and degrades");
        CHECK(crash.next_deadline() == std::chrono::steady_clock::time_point{},
              "crash-loop DegradedLegacy clears deadline");
    }

    // Item B: DegradedLegacy from launch exhaustion (max_attempts reached)
    // must also clear the deadline.
    {
        SidecarLifecycleConfig exhaust_config = config;
        exhaust_config.identities = allocator(config.control_generation);
        exhaust_config.max_attempts = 1;
        SidecarLifecycle exhaust(exhaust_config);
        (void)exhaust.begin(t0);
        LifecycleObservation launch_fail;
        launch_fail.exec = ExecObservation::Failed;
        launch_fail.path_absent = true;
        (void)exhaust.advance(t0, launch_fail);
        CHECK(exhaust.state() == LifecycleState::DegradedLegacy,
              "exhausted-attempts reaches DegradedLegacy");
        CHECK(exhaust.next_deadline() ==
                  std::chrono::steady_clock::time_point{},
              "exhausted-attempts DegradedLegacy clears deadline (item B)");
    }
}

} // namespace

int main() {
    test_lifecycle();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "p50 sidecar lifecycle tests passed\n";
    return EXIT_SUCCESS;
}
