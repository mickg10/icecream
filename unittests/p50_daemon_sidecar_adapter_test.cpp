#include "cache/p50_daemon_sidecar_adapter.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using icecc::p50::sidecar::CentralChildReaperRegistry;
using icecc::p50::daemon::DaemonSidecarAdapter;

namespace {

// Slice-1 hardening requirement #2: DaemonSidecarAdapter::
// outer_observe_child_reaped(const sidecar::ReapEvent&) must reject on
// !event.exact_identity() and not merely on the four explicit field
// comparisons it separately re-checks (owner/pid/pidfd/registry_generation).
// Set false the first time a real, already-scheduled reap is tampered with
// (incarnation zeroed -- a field exact_identity() requires valid() but which
// the function never compares against anything else) and still accepted, or
// still mutates an observable counter. Checked once at the end of main().
bool g_exact_identity_hardening_ok = true;

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
            if (event.has_value()) {
                // See the g_exact_identity_hardening_ok comment above: this
                // piggybacks on an already-scheduled real reap, so it adds
                // no new process, wait, or timing window.
                icecc::p50::sidecar::ReapEvent tampered = *event;
                tampered.incarnation = icecc::p50::sidecar::LaunchIncarnation{};
                const uint64_t exits_before = adapter.cumulative_post_ready_exits();
                const bool authenticated_before = adapter.authenticated();
                if (tampered.exact_identity() ||
                    adapter.outer_observe_child_reaped(tampered) ||
                    adapter.cumulative_post_ready_exits() != exits_before ||
                    adapter.authenticated() != authenticated_before)
                    g_exact_identity_hardening_ok = false;
                (void)adapter.outer_observe_child_reaped(*event);
            }
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

#if defined(ICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS)
// Task 4 (k310) requirement: unit-test the proc-fallback CLOEXEC sweep in
// isolation, with no fork and no execve, so the sweep's own correctness
// (every non-exempt fd gains FD_CLOEXEC, the internal /proc dir fd is not
// leaked, a forced open() failure returns false and touches nothing) is
// proven directly, independent of the Fork-case dispatch logic exercised
// separately below by run_close_range_dispatch_tests().
bool run_sweep_unit_tests()
{
    int pipe_a[2];
    int pipe_b[2];
    if (::pipe(pipe_a) != 0)
        return false;
    if (::pipe(pipe_b) != 0) {
        ::close(pipe_a[0]);
        ::close(pipe_a[1]);
        return false;
    }
    const std::vector<int> synthetic{pipe_a[0], pipe_a[1], pipe_b[0], pipe_b[1]};
    bool failed = false;
    for (int fd : synthetic) {
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags < 0 || ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) != 0)
            failed = true;
    }
    // One fd starts with CLOEXEC already set, to confirm the sweep leaves an
    // already-correct bit alone rather than, say, toggling it off.
    {
        const int flags = ::fcntl(pipe_b[0], F_GETFD);
        if (flags < 0 || ::fcntl(pipe_b[0], F_SETFD, flags | FD_CLOEXEC) != 0)
            failed = true;
    }

    auto open_fd_snapshot = [] {
        std::vector<int> open_fds;
        for (int fd = 0; fd < 1024; ++fd) {
            if (::fcntl(fd, F_GETFD) >= 0)
                open_fds.push_back(fd);
        }
        return open_fds;
    };

    const std::vector<int> before = open_fd_snapshot();
    if (!DaemonSidecarAdapter::test_sweep_cloexec_proc_fallback(false))
        failed = true;
    const std::vector<int> after = open_fd_snapshot();
    // No fd gained or lost: the sweep's own /proc/self/fd dir fd must not
    // leak, and it must not have closed anything it was not supposed to.
    if (before != after)
        failed = true;
    for (int fd : synthetic) {
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) == 0)
            failed = true;
    }

    // force_open_failure: must return false and must not touch any fd.
    // Clear CLOEXEC on one synthetic fd again so a spurious mutation would
    // be observable.
    {
        const int flags = ::fcntl(pipe_a[0], F_GETFD);
        if (flags < 0 || ::fcntl(pipe_a[0], F_SETFD, flags & ~FD_CLOEXEC) != 0)
            failed = true;
    }
    if (DaemonSidecarAdapter::test_sweep_cloexec_proc_fallback(true))
        failed = true;
    {
        const int flags = ::fcntl(pipe_a[0], F_GETFD);
        if (flags < 0 || (flags & FD_CLOEXEC) != 0)
            failed = true;
    }

    for (int fd : synthetic)
        ::close(fd);
    return !failed;
}

// Task 4 (k310) requirement: prove the Fork-case dispatch itself, through a
// real fork+exec of the cache-service binary (the dispatch lives inline in
// OuterLaunchPhase::Fork's child branch, not in a separately callable
// function, so only a real launch exercises it).  ENOSYS/EINVAL/EPERM must
// each take the proc fallback and still reach a fully authenticated, READY
// sidecar; any other errno (EBADF here) must fail closed; and
// force_proc_open_failure stacked on a fallback-triggering errno must also
// fail closed (the fallback-of-the-fallback failing is equivalent to the
// fallback itself being broken).
bool run_close_range_dispatch_tests(
    const std::string& temp_root, CentralChildReaperRegistry& reaper,
    const DaemonSidecarAdapter::Config& base_config)
{
    auto make_root = [&](const char* suffix) -> std::string {
        const std::string pattern = temp_root + "/p50cr-" + suffix + "-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (::mkdtemp(buffer.data()) == nullptr || ::chmod(buffer.data(), 0700) != 0)
            return std::string();
        return std::string(buffer.data());
    };

    for (int injected_errno : {ENOSYS, EINVAL, EPERM}) {
        const std::string root = make_root("fb");
        if (root.empty())
            return false;
        auto config = base_config;
        config.runtime_directory = root;
        DaemonSidecarAdapter adapter(config);
        adapter.observe_public_listener(true, config.public_listener_port);
        adapter.outer_set_scheduler_owner(true);
        adapter.test_force_close_range_errno(injected_errno);
        icecc::p50::advertisement::Update update;
        const bool reached_ready =
            drive_until(adapter, reaper, update, std::chrono::seconds(5), [&] {
                return adapter.authenticated() &&
                       adapter.advertisement_snapshot().present();
            });
        const bool shut_down = reached_ready &&
                               drive_shutdown(adapter, reaper, update);
        const bool cleaned = remove_p29_fingerprint_cache(root) &&
                             ::rmdir(root.c_str()) == 0;
        if (!reached_ready || !shut_down || !cleaned)
            return false;
    }

    {
        const std::string root = make_root("fc");
        if (root.empty())
            return false;
        auto config = base_config;
        config.runtime_directory = root;
        DaemonSidecarAdapter adapter(config);
        adapter.observe_public_listener(true, config.public_listener_port);
        adapter.outer_set_scheduler_owner(true);
        adapter.test_force_close_range_errno(EBADF);
        icecc::p50::advertisement::Update update;
        const bool reached_ready =
            drive_until(adapter, reaper, update, std::chrono::seconds(3), [&] {
                return adapter.authenticated() &&
                       adapter.advertisement_snapshot().present();
            });
        // attempt() only becomes non-zero once a ready lease is bound; this
        // scenario is specifically designed so READY is never reached (every
        // forked child self-exits on the untolerated EBADF before execve),
        // so attempt() stays 0 by construction. Use the lifecycle's own
        // relaunch counter instead to confirm a relaunch really happened.
        const bool actually_attempted = adapter.outer_lifecycle_attempts() > 0;
        adapter.outer_set_scheduler_owner(false);
        const bool shut_down = drive_shutdown(adapter, reaper, update);
        ::rmdir(root.c_str());
        if (reached_ready || !actually_attempted || !shut_down)
            return false;
    }

    {
        const std::string root = make_root("fo");
        if (root.empty())
            return false;
        auto config = base_config;
        config.runtime_directory = root;
        DaemonSidecarAdapter adapter(config);
        adapter.observe_public_listener(true, config.public_listener_port);
        adapter.outer_set_scheduler_owner(true);
        adapter.test_force_close_range_errno(ENOSYS);
        adapter.test_force_proc_open_failure(true);
        icecc::p50::advertisement::Update update;
        const bool reached_ready =
            drive_until(adapter, reaper, update, std::chrono::seconds(3), [&] {
                return adapter.authenticated() &&
                       adapter.advertisement_snapshot().present();
            });
        // Same rationale as the EBADF block above: the fallback-of-the-
        // fallback is forced to fail too, so READY (and attempt()) is never
        // reached by design. Use the lifecycle's relaunch counter instead.
        const bool actually_attempted = adapter.outer_lifecycle_attempts() > 0;
        adapter.outer_set_scheduler_owner(false);
        const bool shut_down = drive_shutdown(adapter, reaper, update);
        ::rmdir(root.c_str());
        if (reached_ready || !actually_attempted || !shut_down)
            return false;
    }

    return true;
}
#endif // defined(ICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS)

// Robustness-fix regression for OuterKillDomainVerifier::capture(): before
// release() existed, every crash/relaunch cycle appended one Record to
// records_ (and, on the Native pidfd backend, opened one real pidfd) and
// nothing ever removed it until the whole adapter was destroyed. This test
// does several real kill-and-relaunch cycles through the public adapter API
// -- the exact same crash/relaunch path already exercised once in main()
// below -- and confirms this process's own open pidfd count settles into a
// steady state instead of growing by one every cycle.
int open_pidfd_count()
{
    int count = 0;
    char target[256];
    for (int fd = 0; fd < 4096; ++fd) {
        if (::fcntl(fd, F_GETFD) < 0)
            continue;
        char path[32];
        std::snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        const ssize_t n = ::readlink(path, target, sizeof(target) - 1);
        if (n > 0) {
            target[static_cast<size_t>(n)] = '\0';
            if (std::strncmp(target, "anon_inode:[pidfd]", 18) == 0)
                ++count;
        }
    }
    return count;
}

bool run_kill_domain_release_bound_test(
    const std::string& temp_root, CentralChildReaperRegistry& reaper,
    const DaemonSidecarAdapter::Config& base_config)
{
    const std::string pattern = temp_root + "/p50kd-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr || ::chmod(buffer.data(), 0700) != 0)
        return false;
    const std::string root(buffer.data());

    auto config = base_config;
    config.runtime_directory = root;
    // This test's whole point is to drive several real kill/relaunch cycles
    // back-to-back with no pacing, so it can observe the kill-domain record
    // (and pidfd) count settle into a steady state. The production default
    // of max_restarts=3 within a 10s restart_window is a legitimate
    // crash-loop throttle, but it is tuned for organic failures, not a
    // deliberate same-process test driving kCycles relaunches in a few
    // hundred milliseconds; left at the default, the adapter degrades to
    // legacy mid-test and the test fails on an unrelated timeout rather than
    // exercising the release() bound it is meant to check. Raise the budget
    // for this local config copy only (not base_config, not any other
    // test's adapter) so every cycle gets a real relaunch.
    config.max_restarts = 64;
    DaemonSidecarAdapter adapter(config);
    adapter.observe_public_listener(true, config.public_listener_port);
    adapter.outer_set_scheduler_owner(true);
    icecc::p50::advertisement::Update update;

    bool ok = drive_until(adapter, reaper, update, std::chrono::seconds(5), [&] {
        return adapter.authenticated() && adapter.advertisement_snapshot().present();
    });

    constexpr int kCycles = 6;
    int steady_state_count = -1;
    for (int cycle = 0; ok && cycle < kCycles; ++cycle) {
        const pid_t dying_pid = adapter.outer_child_pid();
        ok = dying_pid > 1 && ::kill(dying_pid, SIGKILL) == 0;
        ok = ok && drive_until(adapter, reaper, update, std::chrono::seconds(5), [&] {
            return adapter.authenticated() && adapter.outer_child_pid() > 1 &&
                   adapter.outer_child_pid() != dying_pid;
        });
        if (!ok)
            break;
        const int count = open_pidfd_count();
        if (cycle == 1) {
            steady_state_count = count;
        } else if (cycle > 1 && count > steady_state_count) {
            std::fprintf(stderr,
                         "kill-domain release regression: open pidfd count"
                         " grew from %d to %d after relaunch cycle %d\n",
                         steady_state_count, count, cycle);
            ok = false;
        }
    }

    // All kCycles relaunches must actually have happened (not merely the
    // first one or two before some unrelated timeout cut the loop short);
    // otherwise a plateau of "whatever we saw before giving up" would pass
    // trivially without exercising release() at all.
    if (ok && steady_state_count < 0) {
        std::fprintf(stderr,
                     "kill-domain release regression: steady state was never"
                     " established (loop exited before cycle 1)\n");
        ok = false;
    }
    if (ok) {
        std::fprintf(stderr,
                     "kill-domain release bound: open pidfd count held at %d"
                     " through %d relaunch cycles\n",
                     steady_state_count, kCycles - 1);
    }

    ok = ok && drive_shutdown(adapter, reaper, update);
    ok = ok && remove_p29_fingerprint_cache(root);
    return ok && ::rmdir(root.c_str()) == 0;
}

// Diagnostic coverage for the daemon-side input-lifecycle RPC trace
// (append_daemon_lifecycle_trace(), gated on
// ICECC_P50_DAEMON_LIFECYCLE_TRACE). Production bug being diagnosed: on farm
// workers, CloseAcceptedJob input-lifecycle RPCs are sometimes retired
// (reason=protocol action=2 status=Timeout|Disconnected) before the request
// is ever delivered to the sidecar; this trace is the only direct evidence
// of where the configured input_lifecycle_timeout budget is actually spent
// on the daemon side. Two invariants must hold:
//   (a) with the env var unset, the trace file must never be created at
//       all (zero behavior change by default);
//   (b) with the env var set to a path, every input-lifecycle RPC that
//       reaches a terminal result must append one compact-JSON record
//       there, with exactly the field names the trace format documents.
// This is also the first test in this file to drive the input-lifecycle
// RPC path (apply_input_lifecycle() / CloseAcceptedJob) at all. The
// specific terminal InputLifecycleStatus returned is deliberately not
// asserted: no InputRecord is ever published for the probe key, so the
// realistic outcome is a rejection such as UnknownRecord, which is exactly
// as valid for exercising the trace as a success would be -- only the
// trace's presence/absence and schema are under test here.
bool run_daemon_lifecycle_trace_test(
    const std::string& temp_root, CentralChildReaperRegistry& reaper,
    const DaemonSidecarAdapter::Config& base_config)
{
    const std::string pattern = temp_root + "/p50lt-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr || ::chmod(buffer.data(), 0700) != 0)
        return false;
    const std::string root(buffer.data());

    auto config = base_config;
    config.runtime_directory = root;
    DaemonSidecarAdapter adapter(config);
    adapter.observe_public_listener(true, config.public_listener_port);
    adapter.outer_set_scheduler_owner(true);
    icecc::p50::advertisement::Update update;

    bool ok = drive_until(adapter, reaper, update, std::chrono::seconds(5), [&] {
        return adapter.authenticated() && adapter.advertisement_snapshot().present();
    });

    // One request_id counter shared by both sub-tests below; the actual
    // replay key is operation_id (minted internally by
    // next_input_lifecycle_operation()), not request_id, so reusing the
    // counter across sub-tests cannot collide with anything.
    uint64_t next_request_id = 1;
    const auto issue_close_accepted_job = [&]() {
        const icecc::p50::local::Identity identity{config.generation, adapter.attempt()};
        const icecc::p50::InputRecordKey key{
            icecc::p50::Id128::from_u64(0x7261636554ULL), icecc::p50::TuSeq{1}};
        const icecc::p50::InputLeaseOwner owner{101, 1, 1};
        const icecc::p50::InputFdRequest lease{identity, key, owner, next_request_id++};
        const auto immediate = adapter.apply_input_lifecycle(
            lease, icecc::p50::InputLifecycleAction::CloseAcceptedJob);
        if (immediate.status == icecc::p50::InputLifecycleStatus::InvalidArgument ||
            immediate.status == icecc::p50::InputLifecycleStatus::StoreReplaced) {
            std::fprintf(stderr,
                         "daemon lifecycle trace test: apply_input_lifecycle"
                         " rejected immediately (status=%u), scenario moot\n",
                         static_cast<unsigned>(immediate.status));
            return false;
        }
        if (!drive_until(adapter, reaper, update, std::chrono::seconds(5), [&] {
                return adapter.pending_input_lifecycle_count() == 0;
            })) {
            std::fprintf(stderr,
                         "daemon lifecycle trace test: CloseAcceptedJob never"
                         " reached a terminal result within 5s\n");
            return false;
        }
        if (!adapter.take_outer_input_lifecycle_result().has_value()) {
            std::fprintf(stderr,
                         "daemon lifecycle trace test: no terminal result"
                         " available even though pending count reached 0\n");
            return false;
        }
        return true;
    };

    // One shared candidate path for both sub-tests below, deliberately
    // reused: sub-test A proves nothing appears at this exact path while
    // the env var is unset, then sub-test B points the env var at this
    // same path and proves content now appears there. Reusing one path
    // (rather than two unrelated ones) directly ties the "absent" and
    // "present" claims to the same env-var transition instead of merely
    // probing two unconnected locations.
    std::string trace_path;
    {
        const std::string trace_pattern = root + "/trace-XXXXXX";
        std::vector<char> trace_buffer(trace_pattern.begin(), trace_pattern.end());
        trace_buffer.push_back('\0');
        const int fd = ::mkstemp(trace_buffer.data());
        if (fd < 0) {
            ok = false;
        } else {
            ::close(fd);
            trace_path = trace_buffer.data();
            ::unlink(trace_path.c_str());
        }
    }

    // Sub-test A: env unset -> the trace file must never be created.
    ::unsetenv("ICECC_P50_DAEMON_LIFECYCLE_TRACE");
    ok = ok && issue_close_accepted_job();
    if (ok) {
        struct stat info{};
        if (::stat(trace_path.c_str(), &info) == 0 || errno != ENOENT) {
            std::fprintf(stderr,
                         "daemon lifecycle trace regression: a file exists at"
                         " the probe path (or stat failed with errno=%d, not"
                         " ENOENT) even though ICECC_P50_DAEMON_LIFECYCLE_TRACE"
                         " was unset\n", errno);
            ok = false;
        }
    }

    // Sub-test B: env set to that same path -> exactly the documented
    // compact-JSON schema must now appear there.
    ok = ok && ::setenv("ICECC_P50_DAEMON_LIFECYCLE_TRACE", trace_path.c_str(), 1) == 0;
    ok = ok && issue_close_accepted_job();
    ::unsetenv("ICECC_P50_DAEMON_LIFECYCLE_TRACE");

    if (ok) {
        const int fd = ::open(trace_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            std::fprintf(stderr,
                         "daemon lifecycle trace regression: trace file was"
                         " never created even though"
                         " ICECC_P50_DAEMON_LIFECYCLE_TRACE was set (errno=%d)\n",
                         errno);
            ok = false;
        } else {
            char contents[4096] = {0};
            const ssize_t n = ::read(fd, contents, sizeof(contents) - 1);
            ::close(fd);
            if (n <= 0) {
                std::fprintf(stderr,
                             "daemon lifecycle trace regression: trace file"
                             " was created but is empty\n");
                ok = false;
            } else {
                const std::string text(contents, static_cast<size_t>(n));
                static const char* const kNeedles[] = {
                    "\"schema\":\"icecream-p50-daemon-lifecycle-v1\"",
                    "\"wall_clock_ns\":",
                    "\"operation_id\":",
                    "\"action\":2,",
                    "\"status\":",
                    "\"job_identity_hash\":\"",
                    "\"pending_depth_at_enqueue\":",
                    "\"turns_before_first_attempt\":",
                    "\"enqueue_to_first_attempt_ns\":",
                    "\"enqueue_to_connect_done_ns\":",
                    "\"enqueue_to_request_sent_ns\":",
                    "\"enqueue_to_response_received_ns\":",
                    "\"enqueue_to_ack_sent_ns\":",
                    "\"enqueue_to_terminal_ns\":",
                };
                for (const char* needle : kNeedles) {
                    if (text.find(needle) == std::string::npos) {
                        std::fprintf(stderr,
                                     "daemon lifecycle trace regression:"
                                     " missing expected field '%s' in"
                                     " recorded trace: %s\n",
                                     needle, text.c_str());
                        ok = false;
                    }
                }
            }
        }
        ::unlink(trace_path.c_str());
    }

    ok = ok && drive_shutdown(adapter, reaper, update);
    ok = ok && remove_p29_fingerprint_cache(root);
    return ok && ::rmdir(root.c_str()) == 0;
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
    parked_adapter.outer_request_replacement();
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

    if (!remove_p29_fingerprint_cache(root) || ::rmdir(directory) != 0)
        return 25;
    if (!g_exact_identity_hardening_ok)
        return 28;

    // Task 4 (k310): fork-safe CLOEXEC-sweep hardening.  See the function
    // comments above for exactly what each proves.
    if (!run_sweep_unit_tests())
        return 29;
    if (!run_close_range_dispatch_tests(temp_root, reaper, config))
        return 30;

    // Robustness fix: OuterKillDomainVerifier::release() must keep the
    // per-incarnation kill-domain record (and its pidfd, on the Native
    // backend) bounded across repeated crash/relaunch cycles instead of
    // leaking one per incarnation forever.
    if (!run_kill_domain_release_bound_test(std::string(temp_root), reaper,
                                            config))
        return 31;

    // Diagnostic trace coverage for the farm input-lifecycle retirement
    // investigation: see run_daemon_lifecycle_trace_test()'s comment for the
    // production bug this is diagnosing and exactly what it proves.
    if (!run_daemon_lifecycle_trace_test(std::string(temp_root), reaper,
                                         config))
        return 32;

    std::puts("p50 daemon sidecar adapter outer lifecycle: ok");
    return 0;
}
