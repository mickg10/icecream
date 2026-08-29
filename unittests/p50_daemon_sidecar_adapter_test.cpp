#include "cache/p50_daemon_sidecar_adapter.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
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
    icecc::p50::advertisement::Update update;
    if (!drive_until(adapter, reaper, update, std::chrono::seconds(5), [&] {
            return adapter.authenticated() &&
                   adapter.advertisement_snapshot().present();
        }))
    {
        return 8;
    }

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
    icecc::p50::advertisement::Update local_update;
    if (!drive_until(local_adapter, reaper, local_update,
                     std::chrono::seconds(5), [&] {
                         return local_adapter.authenticated() &&
                                local_adapter.advertisement_snapshot().absent();
                     }))
        return 18;
    if (!drive_shutdown(local_adapter, reaper, local_update))
        return 19;

    if (::rmdir(directory) != 0)
        return 20;
    std::puts("p50 daemon sidecar adapter outer lifecycle: ok");
    return 0;
}
