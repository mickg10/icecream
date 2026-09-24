#include "cache/p50_sidecar_lifecycle.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
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
        321, 321, lifecycle.owner_key(), lifecycle.reap_mailbox(), 0, 0,
        lifecycle_pidfd, incarnation_for(first));
    CHECK(lifecycle_registration.valid(), "central registry owns one pid");
    CHECK(!reaper.register_owner(321, 321, lifecycle.owner_key(),
                                 lifecycle.reap_mailbox(), 0, 0,
                                 lifecycle_pidfd, incarnation_for(first)).valid(),
          "one PID has one owner");
    SidecarLifecycle sentinel(config);
    auto sentinel_registration = reaper.register_owner(
        322, 322, sentinel.owner_key(), sentinel.reap_mailbox(), 0, 0,
        1322, incarnation_for(first));
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
            lifecycle.reap_mailbox(), 0, 0, 20000 + static_cast<int>(index),
            incarnation_for(first));
        CHECK(token.valid(), "registry accepts an owner below its fixed cap");
        registrations.push_back(std::move(token));
    }
    CHECK(capacity_registry.size() == 256 &&
              !capacity_registry
                   .register_owner(20000, 20000, ReaperOwnerKey{20000, 1},
                                    lifecycle.reap_mailbox(), 0, 0, 20256,
                                    incarnation_for(first))
                   .valid(),
          "registry rejects the owner above its fixed total cap");

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
        700, 700, old_key, owner->reap_mailbox(), first_device, first_inode,
        old_pidfd, incarnation_for(*owner->identity()));
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
        700, 700, new_key, reused->reap_mailbox(), first_device, first_inode,
        reused_pidfd, incarnation_for(*reused->identity()));
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
        701, 701, dying_key, dying->reap_mailbox(), first_device, first_inode,
        1702, incarnation_for(*dying->identity()));
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
