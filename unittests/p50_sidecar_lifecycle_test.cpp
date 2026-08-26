#include "cache/p50_sidecar_lifecycle.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

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

ReadyLease ready_for(const LifecycleIdentity& identity, pid_t pid) {
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
    lease.listener_device = 1;
    lease.listener_inode = 2;
    lease.directory_device = 1;
    lease.directory_inode = 3;
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

    CentralChildReaperRegistry reaper;
    CHECK(reaper.register_owner(321, 321, lifecycle), "central registry owns one pid");
    CHECK(!reaper.register_owner(321, 321, lifecycle), "one PID has one owner");
    SidecarLifecycle sentinel(config);
    CHECK(reaper.register_owner(322, 322, sentinel), "central registry accepts sentinel owner");
    CHECK(reaper.next_unobserved_pid().value_or(-1) == 321,
          "central reaper starts at the first owner");
    CHECK(reaper.next_unobserved_pid().value_or(-1) == 322,
          "central reaper rotates fairly to the sentinel");
    CHECK(reaper.observe_child_reaped(321, 0, true), "ECHILD/reap observed once");
    CHECK(!reaper.observe_child_reaped(321, 0, true), "duplicate reap is rejected");
    CHECK(reaper.next_unobserved_pid().value_or(-1) == 322,
          "observed owner is deleted from the fairness set");

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
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(20), wrong_group).action ==
              LifecycleAction::SendKill,
          "stale PGID proof cannot suppress TERM to KILL escalation");
    CHECK(lifecycle.state() == LifecycleState::TerminatingKill,
          "surviving helper keeps lifecycle in kill state");
    CHECK(lifecycle.advance(t0 + std::chrono::milliseconds(21), wrong_group).action ==
              LifecycleAction::None,
          "KILL is emitted once and never duplicated by a later turn");
    LifecycleObservation group_gone = wrong_group;
    group_gone.observed_pgid = 321;
    CHECK(lifecycle.advance(t0, group_gone).action == LifecycleAction::None &&
              lifecycle.state() == LifecycleState::ReapAndGroupCheck,
          "leader reap is insufficient until exact PGID ESRCH");
    LifecycleObservation stale_path = group_gone;
    stale_path.observed_path = "/tmp/stale-other-incarnation";
    CHECK(lifecycle.advance(t0, stale_path).action == LifecycleAction::None &&
              lifecycle.state() == LifecycleState::ReapAndGroupCheck,
          "stale path callback cannot authorize replacement");
    CHECK(lifecycle.advance(t0, group_gone).action == LifecycleAction::RetryEligible &&
              lifecycle.state() == LifecycleState::RetryEligible,
          "exact group and path proof authorizes retry");

    auto replacement = lifecycle.begin(t0);
    CHECK(replacement.action == LifecycleAction::LaunchPrepared &&
              replacement.identity.control.generation == first.control.generation &&
              replacement.identity.control.attempt != first.control.attempt &&
              replacement.identity.store_generation == first.store_generation &&
              replacement.identity.store_root != first.store_root &&
              replacement.identity.private_directory != first.private_directory,
          "replacement burns attempt and rotates root/path without control rotation");

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
