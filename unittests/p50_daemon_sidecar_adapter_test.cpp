#include "cache/p50_daemon_sidecar_adapter.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using icecc::p50::sidecar::CentralChildReaperRegistry;
using icecc::p50::daemon::DaemonSidecarAdapter;

namespace {

bool exact_node(const std::string& path, mode_t mode, bool directory,
                uid_t uid, gid_t gid)
{
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0 &&
           (directory ? S_ISDIR(info.st_mode) : S_ISSOCK(info.st_mode)) &&
           (info.st_mode & 07777) == mode && info.st_uid == uid &&
           info.st_gid == gid;
}

bool remove_p29_fingerprint_cache(const std::string& root)
{
    for (const char* name : {"p29-system-source-fingerprint-v1.cache",
                             "p29-system-source-fingerprint-v1.lock"}) {
        const std::string path = root + "/" + name;
        struct stat info{};
        if (::lstat(path.c_str(), &info) != 0) {
            if (errno == ENOENT)
                continue;
            return false;
        }
        if (!S_ISREG(info.st_mode) || ::unlink(path.c_str()) != 0)
            return false;
    }
    return true;
}

int poll_timeout(DaemonSidecarAdapter& adapter,
                 std::chrono::steady_clock::time_point limit)
{
    const auto now = std::chrono::steady_clock::now();
    auto remaining = limit > now
        ? std::chrono::duration_cast<std::chrono::milliseconds>(limit - now)
        : std::chrono::milliseconds(0);
    if (adapter.outer_immediate_turn_required())
        return 0;
    const auto lifecycle_deadline = adapter.outer_next_deadline();
    if (lifecycle_deadline != std::chrono::steady_clock::time_point{}) {
        const auto lifecycle_remaining = lifecycle_deadline > now
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                  lifecycle_deadline - now)
            : std::chrono::milliseconds(0);
        remaining = std::min(remaining, lifecycle_remaining);
    }
    return static_cast<int>(std::max(remaining.count(), int64_t{0}));
}

// This is the test's outer-loop owner.  It has one poll inventory and routes
// one exact central-reaper attempt after the lifecycle turn.  It never calls
// the historical start/poll/shutdown ABI or waitpid.
bool outer_turn(DaemonSidecarAdapter& adapter,
                CentralChildReaperRegistry& reaper,
                icecc::p50::advertisement::Update& update,
                std::chrono::steady_clock::time_point limit)
{
    update = icecc::p50::advertisement::Update{};
    const auto now = std::chrono::steady_clock::now();
    (void)adapter.outer_begin_turn(now, &update);

    std::vector<pollfd> pollfds;
    adapter.outer_append_pollfds(pollfds);
    const int timeout = poll_timeout(adapter, limit);
    if (!pollfds.empty())
        (void)::poll(pollfds.data(), pollfds.size(), timeout);
    else
        (void)::poll(nullptr, 0, timeout);

    if (!adapter.outer_action_taken())
        (void)adapter.outer_advance_turn(std::chrono::steady_clock::now(),
                                         pollfds, &update);

    const pid_t pid = adapter.outer_child_pid();
    const int pidfd = adapter.outer_pidfd();
    if (pid > 1 && pidfd >= 0) {
        bool ready = false;
        for (const pollfd& descriptor : pollfds) {
            if (descriptor.fd == pidfd &&
                (descriptor.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
                ready = true;
                break;
            }
        }
        if (ready) {
            const auto event = reaper.reap_one(pid, pidfd);
            if (event.has_value())
                (void)adapter.outer_observe_child_reaped(*event);
        }
        // This is an in-memory publication retry only; it never repeats the
        // exact pidfd waitid operation after a consumed event.
        const auto pending = reaper.publish_pending();
        if (pending.has_value())
            (void)adapter.outer_observe_child_reaped(*pending);
    }
    return std::chrono::steady_clock::now() < limit;
}

template <typename Predicate>
bool drive_until(DaemonSidecarAdapter& adapter,
                 CentralChildReaperRegistry& reaper,
                 icecc::p50::advertisement::Update& update,
                 std::chrono::milliseconds budget,
                 Predicate predicate)
{
    const auto limit = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < limit) {
        if (predicate())
            return true;
        if (!outer_turn(adapter, reaper, update, limit))
            break;
    }
    return predicate();
}

bool drive_shutdown(DaemonSidecarAdapter& adapter,
                    CentralChildReaperRegistry& reaper,
                    icecc::p50::advertisement::Update& update)
{
    adapter.outer_request_shutdown(&update);
    return drive_until(adapter, reaper, update, std::chrono::seconds(5), [&] {
        return adapter.outer_shutdown_complete();
    });
}

// The source gate can hold the first authenticated sidecar at this exact
// READY edge while it records the independently-owned process.  This is a
// bounded test-only rendezvous; production code has no corresponding wait.
bool hold_after_ready()
{
    const char* path = std::getenv("ICECC_P50_TEST_READY_HOLD");
    if (path == nullptr || *path == '\0')
        return true;
    FILE* marker = std::fopen(path, "w");
    if (marker == nullptr)
        return false;
    std::fprintf(marker, "pid=%lld\n", static_cast<long long>(::getpid()));
    std::fclose(marker);
    for (int i = 0; i < 5000 && ::access(path, F_OK) == 0; ++i)
        ::usleep(1000);
    return ::access(path, F_OK) != 0;
}

bool token_is(const char* value, const char* expected)
{
    return value != nullptr && std::strcmp(value, expected) == 0;
}

} // namespace

int main()
{
    const char* temp_root = std::getenv("TMPDIR");
    const char* service = std::getenv("ICECC_TEST_CACHE_SERVICE");
    if (temp_root == nullptr || temp_root[0] != '/' || service == nullptr ||
        *service == '\0' || ::access(service, X_OK) != 0)
        return 2;

    const std::string directory_pattern =
        std::string(temp_root) + "/p50ad-XXXXXX";
    std::vector<char> directory_buffer(directory_pattern.begin(),
                                        directory_pattern.end());
    directory_buffer.push_back('\0');
    char* const directory = directory_buffer.data();
    if (::mkdtemp(directory) == nullptr || ::chmod(directory, 0700) != 0)
        return 2;
    const std::string root(directory);

    DaemonSidecarAdapter::Config config;
    config.executable = service;
    config.runtime_directory = root;
    config.generation = 1;
    config.expected_daemon_uid = static_cast<uint64_t>(::geteuid());
    config.expected_daemon_gid = static_cast<uint64_t>(::getegid());
    config.expected_service_uid = config.expected_daemon_uid;
    config.expected_service_gid = config.expected_daemon_gid;
    config.public_listener_port = 10245;
    // Match daemon/main.cpp: the real service may spend up to two seconds
    // fingerprinting before READY, longer than the adapter's generic default.
    config.readiness_timeout = std::chrono::milliseconds(5000);
    config.clock_identity =
        icecc::p50::sidecar::process_monotonic_clock_identity();

    if (!DaemonSidecarAdapter::valid_config(config))
        return 3;
    auto local_only = config;
    local_only.public_listener_port = 0;
    if (!DaemonSidecarAdapter::valid_config(local_only))
        return 4;
    auto invalid = config;
    invalid.generation = 0;
    if (DaemonSidecarAdapter::valid_config(invalid))
        return 5;
    invalid = config;
    invalid.expected_service_uid += 1;
    if (DaemonSidecarAdapter::valid_config(invalid))
        return 6;
    // The high special-bit mode has no low permission bits.  It must still
    // be rejected by the exact 0700 owner-directory contract; this keeps the
    // executable source mutant which checks only 0077 from surviving.
    if (::chmod(directory, 01700) != 0 ||
        DaemonSidecarAdapter::valid_config(config) ||
        ::chmod(directory, 0700) != 0)
        return 7;

    CentralChildReaperRegistry reaper;
    config.central_reaper = &reaper;
    DaemonSidecarAdapter adapter(config);
    adapter.observe_public_listener(true, config.public_listener_port);
    adapter.outer_set_scheduler_owner(true);
    icecc::p50::advertisement::Update update;
    if (!drive_until(adapter, reaper, update, std::chrono::seconds(5), [&] {
            return adapter.authenticated() &&
                   adapter.advertisement_snapshot().present();
        }))
    {
        return 8;
    }
    if (adapter.outer_next_deadline() !=
            std::chrono::steady_clock::time_point{} ||
        adapter.outer_immediate_turn_required())
        return 26;
    if (!hold_after_ready())
        return 27;

    // CancelAttempt for an input that was never committed is an authenticated
    // exact no-op.  The sidecar records the operation replay, but the adapter
    // must not tear down a healthy READY relationship for this expected
    // UnknownRecord response.
    const auto ready_for_cancel = adapter.outer_current_ready_lease();
    if (!ready_for_cancel || !ready_for_cancel->valid())
        return 28;
    const auto make_absent_cancel = [&](uint64_t operation_id,
                                        uint64_t logical_job,
                                        uint64_t tu_seq) {
        icecc::p50::InputLifecycleRequest request;
        request.identity = ready_for_cancel->identity;
        request.key = icecc::p50::InputRecordKey{
            ready_for_cancel->c_store_guid, icecc::p50::TuSeq{tu_seq}};
        request.owner = icecc::p50::InputLeaseOwner{
            logical_job, UINT64_C(0x901), UINT64_C(0x902) + logical_job};
        request.operation_id = operation_id;
        request.action = icecc::p50::InputLifecycleAction::CancelAttempt;
        request.deadline = std::chrono::steady_clock::now() +
                           std::chrono::seconds(2);
        return request;
    };
    const auto settle_next_input_lifecycle = [&](std::chrono::milliseconds budget) {
        std::optional<icecc::p50::InputLifecycleResult> result;
        const bool settled = drive_until(adapter, reaper, update, budget, [&] {
            result = adapter.take_outer_input_lifecycle_result();
            return result.has_value();
        });
        return settled ? result
                       : std::optional<icecc::p50::InputLifecycleResult>{};
    };
    auto absent_cancel = make_absent_cancel(0x901, 0x911, 0x921);
    const auto cancel_queued = adapter.test_queue_input_lifecycle(absent_cancel);
    if (cancel_queued.status != icecc::p50::InputLifecycleStatus::Disconnected)
        return 29;
    const auto cancel_result = settle_next_input_lifecycle(
        std::chrono::seconds(3));
    std::fprintf(stderr,
        "cancel result present=%d status=%u action=%u request_match=%d authenticated=%d advertised=%d pending=%zu error=%u\n",
        int(cancel_result.has_value()),
        cancel_result ? unsigned(cancel_result->status) : 255u,
        cancel_result ? unsigned(cancel_result->request.action) : 255u,
        int(cancel_result && cancel_result->request == absent_cancel),
        int(adapter.authenticated()),
        int(adapter.advertisement_snapshot().present()),
        adapter.pending_input_lifecycle_count(),
        unsigned(adapter.last_error()));
    if (!cancel_result ||
        cancel_result->status != icecc::p50::InputLifecycleStatus::UnknownRecord ||
        cancel_result->request != absent_cancel || !adapter.authenticated() ||
        !adapter.advertisement_snapshot().present() ||
        adapter.pending_input_lifecycle_count() != 0)
        return 30;
    const auto exact_replay = adapter.test_queue_input_lifecycle(absent_cancel);
    if (exact_replay.status != icecc::p50::InputLifecycleStatus::AlreadyApplied ||
        exact_replay.request != absent_cancel || !adapter.authenticated() ||
        !adapter.advertisement_snapshot().present())
        return 31;
    auto next_absent_cancel = make_absent_cancel(0x902, 0x912, 0x922);
    const auto next_queued = adapter.test_queue_input_lifecycle(next_absent_cancel);
    if (next_queued.status != icecc::p50::InputLifecycleStatus::Disconnected)
        return 32;
    const auto next_result = settle_next_input_lifecycle(
        std::chrono::seconds(3));
    if (!next_result ||
        next_result->status != icecc::p50::InputLifecycleStatus::UnknownRecord ||
        next_result->request != next_absent_cancel || !adapter.authenticated() ||
        !adapter.advertisement_snapshot().present() ||
        adapter.pending_input_lifecycle_count() != 0)
        return 33;
    std::puts("absent CancelAttempt is replayed as a no-op without READY withdrawal");

    const pid_t first_pid = adapter.outer_child_pid();
    const std::string first_path = adapter.socket_path();
    const std::string first_directory =
        first_path.substr(0, first_path.find_last_of('/'));
    if (first_pid <= 1 || first_path.empty() ||
        !exact_node(root, 0700, true, ::geteuid(), ::getegid()) ||
        !exact_node(first_directory, 0700, true, ::geteuid(), ::getegid()) ||
        !exact_node(first_path, 0600, false, ::geteuid(), ::getegid()))
        return 9;

    // A real child crash is delivered through the one central exact-pidfd
    // registry.  No test-side wait status or anonymous wait is accepted.
    const uint64_t first_attempt = adapter.attempt();
    if (::kill(first_pid, SIGKILL) != 0)
        return 10;
    if (!drive_until(adapter, reaper, update, std::chrono::seconds(5), [&] {
            return adapter.cumulative_post_ready_exits() >= 1 &&
                   adapter.authenticated() &&
                   adapter.outer_child_pid() > 1 &&
                   adapter.outer_child_pid() != first_pid;
        }))
    {
        return 11;
    }
    if (::access(first_path.c_str(), F_OK) == 0 ||
        ::access(first_directory.c_str(), F_OK) == 0 || adapter.attempt() <= 1)
        return 12;
    // The exit was observed before any cleanup TERM/KILL, so the child's own
    // signal, not the control hang-up it caused, is the logged cause.
    const auto& killed = adapter.outer_last_retirement_diag();
    if (!token_is(killed.cause, "signal") || killed.code != SIGKILL ||
        killed.attempt != first_attempt || !killed.ready)
        return 41;

    // Runtime identity loss must withdraw the relationship on an outer turn;
    // a permanently silent/invalid private root may not remain advertised.
    if (::chmod(directory, 0750) != 0)
        return 13;
    if (!drive_until(adapter, reaper, update, std::chrono::seconds(2), [&] {
            return !adapter.authenticated() &&
                   adapter.advertisement_snapshot().absent();
        }))
        return 14;
    if (::chmod(directory, 0700) != 0)
        return 15;
    if (!drive_shutdown(adapter, reaper, update))
        return 16;
    if (::access(adapter.socket_path().c_str(), F_OK) == 0)
        return 17;

    // A submitter-only daemon still needs an authenticated local adapter for
    // the C cache-control handoff, but has no public listener to advertise.
    auto local_config = config;
    local_config.public_listener_port = 0;
    DaemonSidecarAdapter local_adapter(local_config);
    local_adapter.observe_public_listener(false, 0);
    local_adapter.outer_set_scheduler_owner(true);
    icecc::p50::advertisement::Update local_update;
    if (!drive_until(local_adapter, reaper, local_update,
                     std::chrono::seconds(5), [&] {
                         return local_adapter.authenticated() &&
                                local_adapter.advertisement_snapshot().absent();
                     }))
        return 18;
    if (!drive_shutdown(local_adapter, reaper, local_update))
        return 19;

    // Replacement teardown is allowed to finish without an active scheduler,
    // but RetryEligible must remain parked: it must neither mint B nor keep
    // the daemon in a zero-timeout loop.  Restoring the owner releases exactly
    // one successor launch on a later outer turn.
    DaemonSidecarAdapter parked_adapter(config);
    parked_adapter.observe_public_listener(true, config.public_listener_port);
    parked_adapter.outer_set_scheduler_owner(true);
    icecc::p50::advertisement::Update parked_update;
    if (!drive_until(parked_adapter, reaper, parked_update,
                     std::chrono::seconds(5), [&] {
                         return parked_adapter.authenticated() &&
                                parked_adapter.advertisement_snapshot().present();
                     }))
        return 20;
    const uint64_t parked_attempt = parked_adapter.attempt();
    parked_adapter.outer_set_scheduler_owner(false);
    parked_adapter.outer_request_replacement("LocalSidecarReplacementRequired");
    if (!drive_until(parked_adapter, reaper, parked_update,
                     std::chrono::seconds(5), [&] {
                         return parked_adapter.outer_lifecycle_state() ==
                                    icecc::p50::sidecar::LifecycleState::RetryEligible &&
                                !parked_adapter.outer_immediate_turn_required();
                     }))
        return 21;
    if (parked_adapter.attempt() != parked_attempt ||
        parked_adapter.outer_child_pid() > 1 ||
        parked_adapter.outer_immediate_turn_required())
        return 22;
    // The explicit request stays the cause although our own TERM/KILL then
    // produced the exit that completed this teardown.
    const auto& requested = parked_adapter.outer_last_retirement_diag();
    if (!token_is(requested.cause, "replacement-request") ||
        !token_is(requested.detail, "LocalSidecarReplacementRequired") ||
        requested.attempt != parked_attempt || !requested.signalled)
        return 42;
    parked_adapter.outer_set_scheduler_owner(true);
    if (!drive_until(parked_adapter, reaper, parked_update,
                     std::chrono::seconds(5), [&] {
                         return parked_adapter.attempt() > parked_attempt &&
                                parked_adapter.authenticated() &&
                                parked_adapter.advertisement_snapshot().present();
                     }))
        return 23;
    if (!drive_shutdown(parked_adapter, reaper, parked_update))
        return 24;

    // UnknownRecord is benign only for an idempotent attempt cancellation.
    // A typed terminal CLOSE against a missing record still retires this
    // relationship, proving the exception does not bless other operations.
    DaemonSidecarAdapter other_lifecycle_adapter(config);
    other_lifecycle_adapter.observe_public_listener(
        true, config.public_listener_port);
    other_lifecycle_adapter.outer_set_scheduler_owner(true);
    icecc::p50::advertisement::Update other_update;
    if (!drive_until(other_lifecycle_adapter, reaper, other_update,
                     std::chrono::seconds(5), [&] {
                         return other_lifecycle_adapter.authenticated() &&
                                other_lifecycle_adapter.advertisement_snapshot().present();
                     }))
        return 34;
    const auto other_ready = other_lifecycle_adapter.outer_current_ready_lease();
    if (!other_ready || !other_ready->valid())
        return 35;
    icecc::p50::RemoteInputLeaseBinding absent_close;
    absent_close.identity = other_ready->identity;
    absent_close.key = icecc::p50::InputRecordKey{
        other_ready->c_store_guid, icecc::p50::TuSeq{0x933}};
    absent_close.owner = icecc::p50::InputLeaseOwner{
        UINT64_C(0x934), UINT64_C(0x935), UINT64_C(0x936)};
    absent_close.operation_id = UINT64_C(0x937);
    absent_close.f_store_generation = other_ready->store_generation;
    absent_close.f_store_guid = other_ready->f_store_guid;
    absent_close.immutable_digest.bytes[0] = 1;
    absent_close.retirement_id = UINT64_C(0x938);
    const auto close_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
    const auto clock_identity = other_lifecycle_adapter.monotonic_clock_identity();
    const auto absent_close_deadline =
        icecc::p50::sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            close_deadline, clock_identity.clock_domain_id,
            clock_identity.time_namespace_id);
    if (!absent_close.valid())
        return 36;
    const auto close_queued = other_lifecycle_adapter.outer_close_logical_input_lease(
        absent_close, absent_close_deadline);
    if (close_queued.status != icecc::p50::InputLifecycleStatus::Disconnected)
        return 37;
    other_lifecycle_adapter.outer_set_scheduler_owner(false);
    std::optional<icecc::p50::InputLifecycleResult> close_result;
    if (!drive_until(other_lifecycle_adapter, reaper, other_update,
                     std::chrono::seconds(3), [&] {
                         close_result =
                             other_lifecycle_adapter.take_outer_input_lifecycle_result();
                         return close_result.has_value();
                     }) ||
        close_result->status != icecc::p50::InputLifecycleStatus::UnknownRecord ||
        close_result->request.action !=
            icecc::p50::InputLifecycleAction::CloseLogicalInputLease)
        return 38;
    if (!drive_until(other_lifecycle_adapter, reaper, other_update,
                     std::chrono::seconds(5), [&] {
                         return !other_lifecycle_adapter.authenticated() &&
                                other_lifecycle_adapter.advertisement_snapshot().absent() &&
                                other_lifecycle_adapter.outer_lifecycle_state() ==
                                    icecc::p50::sidecar::LifecycleState::RetryEligible;
                     }))
        return 39;
    const auto& input_retired = other_lifecycle_adapter.outer_last_retirement_diag();
    if (!token_is(input_retired.cause, "input-lifecycle") ||
        !token_is(input_retired.detail, "InputLifecycleProtocol"))
        return 43;
    std::puts("absent CloseLogicalInputLease remains fail-closed");
    if (!drive_shutdown(other_lifecycle_adapter, reaper, other_update))
        return 40;

    if (!remove_p29_fingerprint_cache(root) || ::rmdir(directory) != 0)
        return 25;
    std::puts("p50 daemon sidecar adapter outer lifecycle: ok");
    return 0;
}
