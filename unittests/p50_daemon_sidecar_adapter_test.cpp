#include "cache/p50_daemon_sidecar_adapter.h"
#include "services/comm.h"

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <limits>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// The configured daemon build supplies the production comm.cpp definition;
// this weak fallback keeps the pre-service configuration gate linkable in
// reduced local builds.
__attribute__((weak)) int MsgChannel::release_fd_if_input_empty()
{
    return -1;
}

struct OrdinaryPair {
    MsgChannel* left = nullptr;
    MsgChannel* right = nullptr;
    OrdinaryPair() = default;
    OrdinaryPair(const OrdinaryPair&) = delete;
    OrdinaryPair& operator=(const OrdinaryPair&) = delete;
    OrdinaryPair(OrdinaryPair&& other) noexcept
        : left(other.left), right(other.right)
    {
        other.left = nullptr;
        other.right = nullptr;
    }
    ~OrdinaryPair()
    {
        delete left;
        delete right;
    }
};

static OrdinaryPair ordinary_pair()
{
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return {};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    OrdinaryPair pair;
    std::thread left([&] {
        pair.left = Service::createChannel(fds[0], reinterpret_cast<sockaddr*>(&address),
                                            sizeof(address));
    });
    std::thread right([&] {
        pair.right = Service::createChannel(fds[1], reinterpret_cast<sockaddr*>(&address),
                                             sizeof(address));
    });
    left.join();
    right.join();
    if (pair.left != nullptr)
        pair.left->protocol = 50;
    if (pair.right != nullptr)
        pair.right->protocol = 50;
    return pair;
}

struct AdapterGuard {
    icecc::p50::daemon::DaemonSidecarAdapter* adapter = nullptr;
    ~AdapterGuard()
    {
        if (adapter != nullptr)
            adapter->shutdown();
    }
};

struct DirectoryGuard {
    const char* path = nullptr;
    ~DirectoryGuard()
    {
        if (path != nullptr)
            (void)::rmdir(path);
    }
};

static bool exact_node(const std::string& path, mode_t mode, bool directory,
                       uid_t uid, gid_t gid)
{
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0 &&
           (directory ? S_ISDIR(info.st_mode) : S_ISSOCK(info.st_mode)) &&
           (info.st_mode & 07777) == mode && info.st_uid == uid && info.st_gid == gid;
}

static bool kill_and_reap(pid_t child)
{
    if (child <= 1 || ::kill(child, SIGKILL) != 0)
        return false;
    int status = 0;
    pid_t result = -1;
    do {
        result = ::waitpid(child, &status, 0);
    } while (result < 0 && errno == EINTR);
    return result == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
}

static bool wait_for_present(icecc::p50::daemon::DaemonSidecarAdapter& adapter,
                             icecc::p50::advertisement::Update& update)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (adapter.poll(&update) && adapter.advertisement_snapshot().present())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

static bool wait_for_counter(icecc::p50::daemon::DaemonSidecarAdapter& adapter,
                             uint64_t expected,
                             icecc::p50::advertisement::Update& update,
                             bool& present)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        present = adapter.poll(&update);
        if (adapter.cumulative_post_ready_exits() == expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

int main()
{
    const char* temp_root = std::getenv("TMPDIR");
    if (temp_root == nullptr || temp_root[0] != '/')
        return 2;
    const std::string directory_pattern =
        std::string(temp_root) + "/p50ad-XXXXXX";
    std::vector<char> directory_buffer(directory_pattern.begin(), directory_pattern.end());
    directory_buffer.push_back('\0');
    char* const directory = directory_buffer.data();
    if (::mkdtemp(directory) == nullptr)
        return 2;
    DirectoryGuard directory_guard{directory};
    (void)::chmod(directory, 0700);

    icecc::p50::daemon::DaemonSidecarAdapter::Config config;
    config.executable = "/bin/true";
    config.runtime_directory = directory;
    config.generation = 1;
    config.expected_daemon_uid = static_cast<uint64_t>(::geteuid());
    config.expected_daemon_gid = static_cast<uint64_t>(::getegid());
    config.expected_service_uid = config.expected_daemon_uid;
    config.expected_service_gid = config.expected_daemon_gid;
    config.public_listener_port = 10245;

    auto pre_ready = config;
    pre_ready.executable = "/bin/true";
    pre_ready.max_attempts_per_recovery = 1;
    icecc::p50::daemon::DaemonSidecarAdapter pre_ready_adapter(pre_ready);
    if (pre_ready_adapter.start() || !pre_ready_adapter.advertisement_snapshot().absent())
        return 7;

    auto invalid = config;
    invalid.public_listener_port = 0;
    if (icecc::p50::daemon::DaemonSidecarAdapter::valid_config(invalid))
        return 3;
    invalid = config;
    invalid.generation = 0;
    if (icecc::p50::daemon::DaemonSidecarAdapter::valid_config(invalid))
        return 4;
    invalid = config;
    invalid.drop_uid = 1;
    if (icecc::p50::daemon::DaemonSidecarAdapter::valid_config(invalid))
        return 5;
    invalid = config;
    invalid.expected_service_uid += 1;
    if (icecc::p50::daemon::DaemonSidecarAdapter::valid_config(invalid))
        return 6;
    if (::chmod(directory, 0750) != 0)
        return 6;
    const bool group_mode_rejected =
        !icecc::p50::daemon::DaemonSidecarAdapter::valid_config(config);
    if (::chmod(directory, 01700) != 0)
        return 6;
    const bool special_mode_rejected =
        !icecc::p50::daemon::DaemonSidecarAdapter::valid_config(config);
    if (::chmod(directory, 0700) != 0 || !group_mode_rejected ||
        !special_mode_rejected)
        return 6;

    const char* service = std::getenv("ICECC_TEST_CACHE_SERVICE");
    if (service == nullptr || *service == '\0' || ::access(service, X_OK) != 0)
        return 24;
    config.executable = service;
    config.max_restarts = 1;
    icecc::p50::daemon::DaemonSidecarAdapter adapter(config);
    AdapterGuard adapter_guard{&adapter};
    adapter.observe_public_listener(true, config.public_listener_port);
    icecc::p50::advertisement::Update update;
    if (!adapter.start(&update) || !adapter.authenticated() ||
        !adapter.advertisement_snapshot().present() || update.count != 1)
        return 8;
    const pid_t first_pid = adapter.supervisor()->child_pid();
    const std::string first_path = adapter.socket_path();
    const std::string first_directory = first_path.substr(0, first_path.find_last_of('/'));
    if (first_pid <= 1 || first_path.empty() ||
        !exact_node(directory, 0700, true, ::geteuid(), ::getegid()) ||
        !exact_node(first_directory, 0700, true, ::geteuid(), ::getegid()) ||
        !exact_node(first_path, 0600, false, ::geteuid(), ::getegid()))
        return 8;

    // Exercise the real one-shot SCM_RIGHTS path.  The consumed control
    // relationship must withdraw first; a same-poll reconnect is permitted
    // only as the second ordered transition after the adopted endpoint has
    // already completed.
    OrdinaryPair pair = ordinary_pair();
    auto* ordinary = pair.left;
    auto* ordinary_peer = pair.right;
    if (ordinary == nullptr || ordinary_peer == nullptr)
        return 9;
    if (!ordinary_peer->send_msg(CacheSessionMsg()))
        return 10;
    Msg* decoded = ordinary->get_msg(1000, false);
    if (decoded == nullptr)
        return 11;
    const auto handoff = adapter.dispatcher()->dispatch(
        *ordinary, 50, Msg::CACHE_SESSION);
    delete decoded;
    delete ordinary;
    pair.left = nullptr;
    if (handoff.result != icecc::p50::daemon::CacheDispatchResult::Accepted ||
        adapter.authenticated())
        return 12;
    bool republished = adapter.poll(&update);
    if (update.count < 1 || !update.transitions[0].absent() ||
        (republished && (update.count != 2 || !update.transitions[1].present())) ||
        (!republished && (update.count != 1 || !adapter.advertisement_snapshot().absent())))
        return 13;
    delete ordinary_peer;
    pair.right = nullptr;
    if (!republished && !wait_for_present(adapter, update))
        return 14;

    adapter.observe_public_listener(false, 0);
    if (adapter.poll(&update) || !adapter.advertisement_snapshot().absent())
        return 15;
    adapter.observe_public_listener(true, config.public_listener_port);
    if (!adapter.poll(&update) || !adapter.advertisement_snapshot().present())
        return 16;

    // Simulate the daemon's generic waitpid(-1) sweep: it reaps the killed
    // child before Supervisor::poll(), which must still classify the exit,
    // clean the exact stale socket inode, and recover without an orphan.
    if (!kill_and_reap(first_pid))
        return 17;
    bool present = false;
    if (!wait_for_counter(adapter, 1, update, present) || !present || update.count != 2 ||
        !update.transitions[0].absent() || !update.transitions[1].present() ||
        adapter.socket_path() == first_path || adapter.attempt() <= 1 ||
        ::access(first_path.c_str(), F_OK) == 0 ||
        ::access(first_directory.c_str(), F_OK) == 0)
        return 18;
    const pid_t replacement_pid = adapter.supervisor()->child_pid();
    if (replacement_pid <= 1 || replacement_pid == first_pid)
        return 19;

    const std::string replacement_path = adapter.socket_path();
    const std::string replacement_directory =
        replacement_path.substr(0, replacement_path.find_last_of('/'));
    adapter.shutdown(&update);
    if (update.count != 1 || !update.transitions[0].absent() ||
        adapter.state() != icecc::p50::daemon::AdapterState::Stopped ||
        !adapter.advertisement_snapshot().absent() ||
        ::kill(replacement_pid, 0) == 0 || errno != ESRCH ||
        ::access(replacement_path.c_str(), F_OK) == 0 ||
        ::access(replacement_directory.c_str(), F_OK) == 0)
        return 20;
    adapter.shutdown(&update);
    if (update.count != 0)
        return 21;
    adapter_guard.adapter = nullptr;

    // A second incarnation proves outer restart exhaustion after exactly one
    // successful replacement and another externally reaped post-READY exit.
    auto exhaustion_config = config;
    exhaustion_config.generation = 2;
    icecc::p50::daemon::DaemonSidecarAdapter exhaustion(exhaustion_config);
    AdapterGuard exhaustion_guard{&exhaustion};
    exhaustion.observe_public_listener(true, exhaustion_config.public_listener_port);
    if (!exhaustion.start(&update))
        return 25;
    pid_t child = exhaustion.supervisor()->child_pid();
    if (!kill_and_reap(child) ||
        !wait_for_counter(exhaustion, 1, update, present) || !present ||
        update.count != 2 || !update.transitions[0].absent() ||
        !update.transitions[1].present())
        return 26;
    child = exhaustion.supervisor()->child_pid();
    if (!kill_and_reap(child) ||
        !wait_for_counter(exhaustion, 2, update, present) || present ||
        exhaustion.supervisor() != nullptr ||
        exhaustion.last_error() != icecc::p50::daemon::AdapterError::AttemptExhausted ||
        !exhaustion.advertisement_snapshot().absent())
        return 27;
    exhaustion.shutdown();
    exhaustion_guard.adapter = nullptr;

    // Runtime privacy is continuously load-bearing, not a start-only check.
    auto privacy_config = config;
    privacy_config.generation = 3;
    icecc::p50::daemon::DaemonSidecarAdapter privacy(privacy_config);
    AdapterGuard privacy_guard{&privacy};
    privacy.observe_public_listener(true, privacy_config.public_listener_port);
    if (!privacy.start(&update) || ::chmod(directory, 0750) != 0)
        return 28;
    const bool privacy_present = privacy.poll(&update);
    const bool privacy_failed_closed = !privacy_present && update.count == 1 &&
        update.transitions[0].absent() &&
        privacy.last_error() == icecc::p50::daemon::AdapterError::RuntimeNodeFailure;
    if (::chmod(directory, 0700) != 0 || !privacy_failed_closed)
        return 28;
    privacy.shutdown();
    privacy_guard.adapter = nullptr;

    // Replacing the captured attempt directory must never make shutdown
    // remove the replacement.  Test-owned cleanup removes only the old,
    // already-dead service socket after proving that refusal.
    auto replacement_config = config;
    replacement_config.generation = 4;
    icecc::p50::daemon::DaemonSidecarAdapter replacement(replacement_config);
    AdapterGuard replacement_guard{&replacement};
    replacement.observe_public_listener(true, replacement_config.public_listener_port);
    if (!replacement.start(&update))
        return 29;
    const std::string captured_path = replacement.socket_path();
    const std::string captured_directory =
        captured_path.substr(0, captured_path.find_last_of('/'));
    const std::string moved_directory = captured_directory + ".moved";
    if (::rename(captured_directory.c_str(), moved_directory.c_str()) != 0 ||
        ::mkdir(captured_directory.c_str(), 0700) != 0 || replacement.poll(&update) ||
        !exact_node(captured_directory, 0700, true, ::geteuid(), ::getegid()))
        return 30;
    replacement.shutdown();
    replacement_guard.adapter = nullptr;
    const bool replacement_preserved =
        exact_node(captured_directory, 0700, true, ::geteuid(), ::getegid());
    const bool replacement_removed = !replacement_preserved ||
                                     ::rmdir(captured_directory.c_str()) == 0;
    const bool old_socket_removed =
        ::unlink((moved_directory + "/control.sock").c_str()) == 0;
    const bool old_directory_removed = ::rmdir(moved_directory.c_str()) == 0;
    if (!replacement_preserved || !replacement_removed ||
        !old_socket_removed || !old_directory_removed)
        return 31;

    auto overflow_config = config;
    overflow_config.generation = 5;
    overflow_config.max_attempts_per_recovery = 1;
    icecc::p50::daemon::DaemonSidecarAdapter overflow(overflow_config);
    overflow.observe_public_listener(true, overflow_config.public_listener_port);
    overflow.test_force_attempt(std::numeric_limits<uint64_t>::max());
    if (overflow.start(&update) ||
        overflow.last_error() != icecc::p50::daemon::AdapterError::AttemptOverflow)
        return 32;

    auto regression_config = config;
    regression_config.generation = 6;
    icecc::p50::daemon::DaemonSidecarAdapter regression(regression_config);
    AdapterGuard regression_guard{&regression};
    regression.observe_public_listener(true, regression_config.public_listener_port);
    if (!regression.start(&update))
        return 33;
    regression.test_force_counter_state(0, 1, true);
    if (regression.poll(&update) ||
        regression.last_error() != icecc::p50::daemon::AdapterError::CounterRegression ||
        !regression.advertisement_snapshot().absent())
        return 34;
    regression.shutdown();
    regression_guard.adapter = nullptr;

    auto saturation_config = config;
    saturation_config.generation = 7;
    icecc::p50::daemon::DaemonSidecarAdapter saturation(saturation_config);
    AdapterGuard saturation_guard{&saturation};
    saturation.observe_public_listener(true, saturation_config.public_listener_port);
    if (!saturation.start(&update))
        return 35;
    saturation.test_force_counter_state(std::numeric_limits<uint64_t>::max(), 0, true);
    if (!kill_and_reap(saturation.supervisor()->child_pid()) || saturation.poll(&update) ||
        saturation.last_error() != icecc::p50::daemon::AdapterError::CounterSaturated ||
        !saturation.advertisement_snapshot().absent())
        return 36;
    saturation.shutdown();
    saturation_guard.adapter = nullptr;

    if (::rmdir(directory) != 0)
        return 37;
    directory_guard.path = nullptr;
    std::puts("p50 daemon sidecar adapter: ok");
    return 0;
}
