// Regression test for the phase-3 authentication EOF bug in
// DaemonSidecarAdapter::outer_advance_authentication() (phase 3, the HelloAck
// read): a clean peer EOF (recv() returning 0) matches neither the
// `count > 0` nor the `count < 0 && errno != EINTR/EAGAIN/EWOULDBLOCK`
// branch, and because a clean EOF on a Unix stream socket sets POLLIN
// alongside POLLHUP, the universal early-exit gate in
// outer_advance_authentication() (which only fires on POLLHUP/POLLERR
// *without* concurrent POLLIN/POLLOUT) does not fire either. The adapter
// spins, re-attempting recv() every outer turn, until the bounded
// Config::connect_timeout deadline force-closes it.
//
// This test reuses the public outer-loop API exactely as
// unittests/p50_daemon_sidecar_adapter_test.cpp does (outer_begin_turn /
// outer_append_pollfds / outer_advance_turn), driving a real
// DaemonSidecarAdapter against a small fixture executable
// (p50_fake_sidecar_eof, see p50_fake_sidecar_eof.cpp) that answers READY and
// then closes its accepted connection without ever sending a HelloAck. No
// new private test hooks were added to DaemonSidecarAdapter for this: the
// pass/fail signal below is derived entirely from already-public state
// (outer_append_pollfds()'s returned pollfd count, alongside the always-
// public outer_pidfd()).
//
// Detection strategy: once the fixture's EOF marker file appears (meaning
// the fixture has accepted the connection, drained the Hello, and closed),
// phase 3's auth fd is the *only* pollfd entry outer_append_pollfds() adds
// besides the child's pidfd (the exec/ready self-pipes are already closed by
// this point in the lifecycle, same as in the existing adapter test). So:
//   - before the fix: the auth fd lingers in the pollfd set until
//     Config::connect_timeout elapses, because phase 3 never closes it on
//     EOF;
//   - after the fix: the auth fd disappears from the pollfd set on the very
//     next outer turn, because the new `count == 0` branch closes it
//     immediately.
// This test asserts the elapsed wall-clock time between "marker observed"
// and "pollfd count drops back to pidfd-only" is small (well under
// Config::connect_timeout). It must fail on pristine c8bb615f (the spin runs
// close to the full connect_timeout) and pass once the count==0 fix is
// applied.

#include "cache/p50_daemon_sidecar_adapter.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using icecc::p50::sidecar::CentralChildReaperRegistry;
using icecc::p50::daemon::DaemonSidecarAdapter;

namespace {

// Keeps the assertion well clear of normal local-loopback handshake jitter
// (connect/getsockopt/SO_PEERCRED/send on a fresh AF_UNIX pair are all
// sub-millisecond) while staying far below Config::connect_timeout (1000ms,
// cache/p50_daemon_sidecar_adapter.h and daemon/main.cpp), which is the
// value the pre-fix spin is bounded by and converges toward.
constexpr std::chrono::milliseconds kMaxAcceptableEofLatency{200};

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

// Same single poll-inventory-per-turn shape as the existing adapter test's
// outer_turn(), but returns the pollfds it polled so the caller can inspect
// how many entries survived this turn's outer_advance_turn().
std::vector<pollfd> one_turn(DaemonSidecarAdapter& adapter,
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
        const auto pending = reaper.publish_pending();
        if (pending.has_value())
            (void)adapter.outer_observe_child_reaped(*pending);
    }
    return pollfds;
}

double rusage_cpu_ms(const struct rusage& usage)
{
    const double user_ms = usage.ru_utime.tv_sec * 1000.0 +
                           usage.ru_utime.tv_usec / 1000.0;
    const double sys_ms = usage.ru_stime.tv_sec * 1000.0 +
                          usage.ru_stime.tv_usec / 1000.0;
    return user_ms + sys_ms;
}

} // namespace

int main()
{
    const char* temp_root = std::getenv("TMPDIR");
    const char* fixture = std::getenv("ICECC_TEST_FAKE_SIDECAR_EOF");
    if (temp_root == nullptr || temp_root[0] != '/' || fixture == nullptr ||
        *fixture == '\0' || ::access(fixture, X_OK) != 0)
        return 2;

    const std::string directory_pattern =
        std::string(temp_root) + "/p50aeof-XXXXXX";
    std::vector<char> directory_buffer(directory_pattern.begin(),
                                        directory_pattern.end());
    directory_buffer.push_back('\0');
    char* const directory = directory_buffer.data();
    if (::mkdtemp(directory) == nullptr || ::chmod(directory, 0700) != 0)
        return 2;
    const std::string root(directory);
    const std::string marker_path = root + "/eof-marker";
    if (::setenv("ICECC_P50_TEST_EOF_MARKER", marker_path.c_str(), 1) != 0)
        return 2;

    DaemonSidecarAdapter::Config config;
    config.executable = fixture;
    config.runtime_directory = root;
    config.generation = 1;
    config.expected_daemon_uid = static_cast<uint64_t>(::geteuid());
    config.expected_daemon_gid = static_cast<uint64_t>(::getegid());
    config.expected_service_uid = config.expected_daemon_uid;
    config.expected_service_gid = config.expected_daemon_gid;
    config.public_listener_port = 0;
    config.readiness_timeout = std::chrono::milliseconds(5000);

    if (!DaemonSidecarAdapter::valid_config(config))
        return 3;

    CentralChildReaperRegistry reaper;
    config.central_reaper = &reaper;
    DaemonSidecarAdapter adapter(config);
    adapter.observe_public_listener(false, 0);
    adapter.outer_set_scheduler_owner(true);
    icecc::p50::advertisement::Update update;

    // Overall test budget: launch + READY + full auth handshake up to the
    // fixture's own EOF, plus Config::connect_timeout worth of possible
    // spin, plus margin for a loaded build host.
    const auto overall_limit =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);

    bool saw_marker = false;
    std::chrono::steady_clock::time_point marker_seen{};
    struct rusage usage_at_marker{};
    bool cleared = false;
    std::chrono::steady_clock::time_point phase_cleared{};
    struct rusage usage_at_cleared{};

    while (std::chrono::steady_clock::now() < overall_limit) {
        const auto pollfds =
            one_turn(adapter, reaper, update, overall_limit);

        if (!saw_marker) {
            if (::access(marker_path.c_str(), F_OK) == 0) {
                saw_marker = true;
                marker_seen = std::chrono::steady_clock::now();
                ::getrusage(RUSAGE_SELF, &usage_at_marker);
            }
            continue;
        }

        // Once the fixture's EOF has definitely happened, phase 3's auth fd
        // is the only non-pidfd entry outer_append_pollfds() adds at this
        // point in the lifecycle (the exec/ready self-pipes already closed
        // earlier, same ordering unittests/p50_daemon_sidecar_adapter_test.cpp
        // relies on). pollfds.size() <= 1 means only the pidfd remains, i.e.
        // the auth fd was closed and outer_auth_phase_ reset to 0.
        if (pollfds.size() <= 1) {
            cleared = true;
            phase_cleared = std::chrono::steady_clock::now();
            ::getrusage(RUSAGE_SELF, &usage_at_cleared);
            break;
        }
    }

    if (!saw_marker) {
        std::fprintf(stderr,
                     "p50_adapter_auth_eof_regression: fixture EOF marker "
                     "never appeared\n");
        return 4;
    }
    if (!cleared) {
        std::fprintf(stderr,
                     "p50_adapter_auth_eof_regression: auth fd never left "
                     "the pollfd set after EOF\n");
        return 5;
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            phase_cleared - marker_seen);
    const double cpu_ms = rusage_cpu_ms(usage_at_cleared) -
                          rusage_cpu_ms(usage_at_marker);
    std::fprintf(stderr,
                 "p50_adapter_auth_eof_regression: eof-to-close elapsed=%lldms "
                 "cpu-while-spinning=%.1fms connect_timeout=%lldms\n",
                 static_cast<long long>(elapsed.count()), cpu_ms,
                 static_cast<long long>(config.connect_timeout.count()));

    adapter.outer_request_shutdown(&update);
    const auto shutdown_limit =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!adapter.outer_shutdown_complete() &&
          std::chrono::steady_clock::now() < shutdown_limit)
        (void)one_turn(adapter, reaper, update, shutdown_limit);

    ::unlink(marker_path.c_str());
    ::unlink((root + "/attempt-1/cache.sock").c_str());
    ::rmdir((root + "/attempt-1").c_str());
    ::rmdir(directory);

    if (elapsed > kMaxAcceptableEofLatency) {
        std::fprintf(stderr,
                     "p50_adapter_auth_eof_regression: FAIL elapsed %lldms "
                     "exceeds %lldms (phase-3 EOF spin bug present)\n",
                     static_cast<long long>(elapsed.count()),
                     static_cast<long long>(kMaxAcceptableEofLatency.count()));
        return 6;
    }

    std::puts("p50 adapter auth eof regression: ok");
    return 0;
}
