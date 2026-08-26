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
    static uint8_t next = 0x20;
    if (size != 16) return -1;
    auto* bytes = static_cast<uint8_t*>(destination);
    for (size_t i = 0; i != size; ++i) bytes[i] = next++;
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

LifecycleObservation forked(pid_t pid) {
    LifecycleObservation observation;
    observation.exec = ExecObservation::Succeeded;
    observation.pid = pid;
    observation.observed_pgid = pid;
    return observation;
}

void move_to_forked(SidecarLifecycle& lifecycle, pid_t pid,
                    std::chrono::steady_clock::time_point now) {
    LifecycleObservation fork = forked(pid);
    fork.exec = ExecObservation::None;
    (void)lifecycle.advance(now, fork);
    (void)lifecycle.advance(now, forked(pid));
}

void move_to_ready(SidecarLifecycle& lifecycle, pid_t pid,
                   std::chrono::steady_clock::time_point now) {
    move_to_forked(lifecycle, pid, now);
    const auto identity = *lifecycle.identity();
    LifecycleObservation ready;
    ready.ready = ReadyObservation::Complete;
    ready.store_generation = identity.store_generation;
    ready.ready_lease = ready_for(identity, pid);
    (void)lifecycle.advance(now, ready);
}

void test_lifecycle() {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::time_point{};
    SidecarLifecycleConfig config;
    config.control_generation = 91;
    config.store_generation = 1;
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
    CHECK(first.valid() && first.store_generation == 1 &&
              first.control.generation == config.control_generation,
          "exact independent control/store identity");
    CHECK(lifecycle.begin(t0).action == LifecycleAction::None,
          "second begin cannot launch twice in one turn");

    LifecycleObservation fork = forked(321);
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
    SidecarLifecycle rejected(config);
    const auto rejected_launch = rejected.begin(t0);
    LifecycleObservation rejected_fork = forked(323);
    rejected_fork.exec = ExecObservation::None;
    (void)rejected.advance(t0, rejected_fork);
    (void)rejected.advance(t0, forked(323));
    LifecycleObservation dropped_store = complete;
    dropped_store.ready_lease = ready_for(rejected_launch.identity, 323);
    dropped_store.store_generation = 99;
    CHECK(rejected.advance(t0, dropped_store).action == LifecycleAction::Withdraw,
          "dropped/frozen store-generation READY variant fails closed");
    CHECK(lifecycle.advance(t0, complete).action == LifecycleAction::PublishReady &&
              lifecycle.state() == LifecycleState::Ready &&
              lifecycle.current_ready_lease().has_value(),
          "complete READY publishes the current lease");
    const dev_t first_device = complete.ready_lease->listener_device;
    const ino_t first_inode = complete.ready_lease->listener_inode;

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
    ++forged_lease.listener_inode;
    forged_ready.ready_lease = forged_lease;
    CHECK(socket_substitution.advance(t0, forged_ready).action ==
              LifecycleAction::Withdraw,
          "socket substitution cannot publish from caller-supplied inode");

    CentralChildReaperRegistry reaper;
    auto lifecycle_registration = reaper.register_owner(
        321, 321, lifecycle.owner_key(), lifecycle.reap_mailbox());
    CHECK(lifecycle_registration.valid(), "central registry owns one pid");
    CHECK(!reaper.register_owner(321, 321, lifecycle.owner_key(),
                                 lifecycle.reap_mailbox()).valid(),
          "one PID has one owner");
    SidecarLifecycle sentinel(config);
    auto sentinel_registration = reaper.register_owner(
        322, 322, sentinel.owner_key(), sentinel.reap_mailbox());
    CHECK(sentinel_registration.valid(), "central registry accepts sentinel owner");
    CHECK(reaper.next_unobserved_pid().value_or(-1) == 321,
          "central reaper starts at the first owner");
    CHECK(reaper.next_unobserved_pid().value_or(-1) == 322,
          "central reaper rotates fairly to the sentinel");
    CHECK(reaper.observe_child_reaped(321, 0, true), "ECHILD/reap observed once");
    CHECK(!reaper.observe_child_reaped(321, 0, true), "duplicate reap is rejected");
    CHECK(reaper.next_unobserved_pid().value_or(-1) == 322,
          "observed owner is deleted from the fairness set");

    CentralChildReaperRegistry capacity_registry;
    std::vector<CentralChildReaperRegistry::Registration> registrations;
    registrations.reserve(256);
    for (size_t index = 0; index != 256; ++index) {
        const pid_t pid = static_cast<pid_t>(10000 + index);
        auto token = capacity_registry.register_owner(
            pid, pid, ReaperOwnerKey{static_cast<uint64_t>(pid), 1},
            lifecycle.reap_mailbox());
        CHECK(token.valid(), "registry accepts an owner below its fixed cap");
        registrations.push_back(std::move(token));
    }
    CHECK(capacity_registry.size() == 256 &&
              !capacity_registry
                   .register_owner(20000, 20000, ReaperOwnerKey{20000, 1},
                                    lifecycle.reap_mailbox())
                   .valid(),
          "registry rejects the owner above its fixed total cap");

    ReapMailbox bounded_mailbox;
    bool mailbox_full = true;
    for (size_t index = 0; index != 16; ++index)
        mailbox_full = mailbox_full &&
                       bounded_mailbox.enqueue(ReapEvent{{9, 1}, 900, 0, true});
    CHECK(mailbox_full &&
              !bounded_mailbox.enqueue(ReapEvent{{9, 1}, 900, 0, true}),
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
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(11), stale_path).action == LifecycleAction::None &&
              lifecycle.state() == LifecycleState::ReapAndGroupCheck,
          "stale path callback cannot authorize replacement");
    LifecycleObservation same_path_new_node = group_gone;
    same_path_new_node.observed_device = first_device;
    same_path_new_node.observed_inode = first_inode + 1;
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(11), same_path_new_node).action == LifecycleAction::None &&
              lifecycle.state() == LifecycleState::ReapAndGroupCheck,
          "replacement at the same pathname cannot masquerade as old-node absence");
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(11), group_gone).action == LifecycleAction::RetryEligible &&
              lifecycle.state() == LifecycleState::RetryEligible,
          "exact group and path proof authorizes retry");

    // Even an injected verifier that returns true for every callback cannot
    // authorize a fabricated lease or a mismatched observed PGID.  The
    // reducer independently binds both facts to the exact fork lease.
    SidecarLifecycleConfig permissive_config = config;
    auto permissive_verifier = std::make_shared<PermissiveKillDomainVerifier>();
    permissive_config.kill_domain_verifier = permissive_verifier;
    SidecarLifecycle permissive_group(permissive_config);
    (void)permissive_group.begin(t0);
    move_to_ready(permissive_group, 352, t0);
    LifecycleObservation permissive_request;
    permissive_request.request_replacement = true;
    CHECK(permissive_group.advance(t0, permissive_request).action ==
              LifecycleAction::Withdraw &&
              permissive_group.advance(t0).action == LifecycleAction::SendTerm,
          "permissive verifier witness starts bounded teardown");
    CHECK(permissive_group.observe_child_reaped(352, 0, true),
          "permissive verifier witness records leader reap");
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
    CHECK(ambiguous_group.observe_child_reaped(350, 0, true),
          "ambiguous PGID row records leader reap");
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
    CHECK(reused_group.observe_child_reaped(351, 0, true),
          "reused-PGID witness records leader reap");
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
              replacement.identity.store_generation == first.store_generation &&
              replacement.identity.store_root != first.store_root &&
              replacement.identity.private_directory != first.private_directory,
          "replacement burns attempt and rotates root/path without control rotation");

    // Reap-before-READY and same-turn waitable/identity-loss rows must all
    // withdraw rather than publishing a lease for a dead incarnation.
    SidecarLifecycle reap_first(config);
    (void)reap_first.begin(t0);
    move_to_forked(reap_first, 401, t0);
    const auto reap_first_identity = *reap_first.identity();
    CHECK(reap_first.observe_child_reaped(401, 0, true),
          "leader reap is accepted before READY");
    LifecycleObservation reap_first_ready;
    reap_first_ready.ready = ReadyObservation::Complete;
    reap_first_ready.store_generation = reap_first_identity.store_generation;
    reap_first_ready.ready_lease = ready_for(reap_first_identity, 401);
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
    lost.child_waitable = false;
    lost.identity_lost = true;
    CHECK(lost_ready.advance(t0, lost).action == LifecycleAction::Withdraw,
          "same-turn identity loss plus READY loses publication race");

    SidecarLifecycle reap_after_ready(config);
    (void)reap_after_ready.begin(t0);
    move_to_ready(reap_after_ready, 404, t0);
    CHECK(reap_after_ready.current_ready_lease().has_value(),
          "ordering witness starts READY");
    CHECK(reap_after_ready.observe_child_reaped(404, 0, true),
          "reap after publication is recorded by same owner");
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
    auto registration = lifetime_registry.register_owner(
        700, 700, old_key, owner->reap_mailbox(), first_device, first_inode);
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
    auto reused_registration = lifetime_registry.register_owner(
        700, 700, new_key, reused->reap_mailbox(), first_device, first_inode);
    auto reused_mailbox = reused->reap_mailbox().lock();
    CHECK(reused_registration.valid() && reused_mailbox &&
              reused_mailbox->enqueue(ReapEvent{old_key, 700, 0, true}) &&
              reused->advance(t0).action == LifecycleAction::None &&
              reused->state() == LifecycleState::ForkedAwaitExecAndReady &&
              !lifetime_registry.observe_child_reaped(700, old_key, 0, true) &&
              lifetime_registry.observe_child_reaped(700, new_key, 0, true),
          "stale generation cannot poison reused PID owner");
    (void)reused->advance(t0);
    CHECK(lifetime_registry.unregister_owner(700, new_key) &&
              !lifetime_registry.unregister_owner(700, new_key),
          "unregister is exact and non-idempotent for stale token");
    reused_registration.reset();
    auto dying = std::make_unique<SidecarLifecycle>(config);
    (void)dying->begin(t0);
    move_to_forked(*dying, 701, t0);
    const ReaperOwnerKey dying_key = dying->owner_key();
    auto dying_registration = lifetime_registry.register_owner(
        701, 701, dying_key, dying->reap_mailbox(), first_device, first_inode);
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
    move_to_ready(legacy_lifecycle, 501, t0);
    const auto legacy_ready_lease = *legacy_lifecycle.current_ready_lease();
    LifecycleObservation legacy_request;
    legacy_request.request_legacy = true;
    CHECK(legacy_lifecycle.advance(t0, legacy_request).action == LifecycleAction::Withdraw &&
              legacy_lifecycle.state() == LifecycleState::TerminatingGrace,
          "request_legacy withdraws live child instead of abandoning it");
    CHECK(legacy_lifecycle.advance(t0).action == LifecycleAction::SendTerm,
          "request_legacy drives TERM");
    CHECK(legacy_lifecycle.observe_child_reaped(501, 0, true),
          "legacy teardown records leader reap");
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
    const std::string socket = replacement.identity.private_directory + "/cache.sock";
    const std::string ready_wire =
        "READY v2 generation=" + std::to_string(replacement.identity.control.generation) +
        " attempt=" + std::to_string(replacement.identity.control.attempt) +
        " DERIVATION_VERSION=1 pid=323 C_STORE_GUID=" +
        hex_guid(replacement.identity.c_store_guid) + " F_STORE_GUID=" +
        hex_guid(replacement.identity.f_store_guid) + " PATH=" + socket +
        " DIGEST=" + icecc::digest128_hex(icecc::digest128(socket)) +
        " DEV=1 INO=2 STORE_GENERATION=" +
        std::to_string(replacement.identity.store_generation) + "\n";
    ReadyLease parsed_lease;
    CHECK(parse_ready_frame(ready_wire, replacement.identity, 323, parsed_lease) &&
              parsed_lease.valid(),
          "structured READY parser validates the complete lease tuple");
}

} // namespace

int main() {
    test_lifecycle();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "p50 sidecar lifecycle tests passed\n";
    return EXIT_SUCCESS;
}
