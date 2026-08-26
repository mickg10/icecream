#include "../cache/p50_sidecar_supervisor.h"

#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/socket.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace icecc::p50::sidecar;

namespace {

void check(bool condition, const char* expression) {
    if (!condition)
        throw std::runtime_error(expression);
}

#define CHECK(expression) check((expression), #expression)

bool write_all(int fd, const char* bytes, size_t size) {
    size_t written = 0;
    while (written != size) {
        const ssize_t result = ::write(fd, bytes + written, size - written);
        if (result > 0) {
            written += static_cast<size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

int ready_fd() {
    const char* value = std::getenv(kReadyFdEnvironment.data());
    return value == nullptr ? -1 : std::atoi(value);
}

const char* required_environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

int structured_listener(const char* path) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const size_t size = std::strlen(path);
    if (size == 0 || size >= sizeof(address.sun_path))
        return -1;
    std::memcpy(address.sun_path, path, size + 1);
    const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0)
        return -1;
    const socklen_t length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + size + 1);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), length) != 0 ||
        ::chmod(path, 0600) != 0 || ::listen(listener, 4) != 0) {
        (void)::close(listener);
        return -1;
    }
    return listener;
}

int structured_child(const std::string& mode, int fd) {
    const char* format = required_environment("ICECC_CACHE_SERVICE_READY_FORMAT");
    const char* generation =
        required_environment("ICECC_CACHE_SERVICE_EXPECTED_GENERATION");
    const char* attempt = required_environment("ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT");
    const char* guid =
        required_environment("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID");
    const char* c_guid =
        required_environment("ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID");
    const char* path = required_environment("ICECC_CACHE_SERVICE_EXPECTED_SOCKET");
    const char* digest =
        required_environment("ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST");
    if (format == nullptr || std::string(format) != "2" || generation == nullptr ||
        attempt == nullptr || guid == nullptr || c_guid == nullptr || path == nullptr ||
        digest == nullptr)
        return 108;
    int listener = -1;
    bool inherited = false;
    if (const char* raw_listener = ::getenv("ICECC_CACHE_SERVICE_LISTENER_FD");
        raw_listener != nullptr) {
        listener = std::atoi(raw_listener);
        inherited = listener >= 0;
    }
    if (listener < 0)
        listener = structured_listener(path);
    if (listener < 0)
        return 109;
    struct stat pathname{};
    if ((!inherited && ::lstat(path, &pathname) != 0) || ::fstat(listener, &pathname) != 0) {
        (void)::close(listener);
        return 110;
    }
    const long long published_pid = static_cast<long long>(::getpid()) +
                                    (mode == "structured-wrong-pid" ? 1 : 0);
    const std::string ready =
        "READY v2 generation=" + std::string(generation) + " attempt=" + attempt +
        " pid=" + std::to_string(published_pid) + " C_STORE_GUID=" + c_guid +
        " F_STORE_GUID=" + guid +
        " PATH=" + path + " DIGEST=" + digest +
        " DEV=" + std::to_string(static_cast<unsigned long long>(pathname.st_dev)) +
        " INO=" + std::to_string(static_cast<unsigned long long>(pathname.st_ino)) +
        "\n";
    std::string wire = ready;
    if (mode == "structured-trailing-space")
        wire.insert(wire.size() - 1, 1, ' ');
    else if (mode == "structured-double-space")
        wire.insert(5, 1, ' ');
    if (!write_all(fd, wire.data(), wire.size())) {
        (void)::close(listener);
        return 111;
    }
    (void)::close(fd);
    if (mode == "structured-exit") {
        (void)::close(listener);
        return 0;
    }
    (void)::signal(SIGTERM, SIG_IGN);
    const int result = ::pause();
    (void)::close(listener);
    return result;
}

int fake_child(const char* mode) {
    const int fd = ready_fd();
    if (fd < 0)
        return 90;
    if (std::string(mode).rfind("structured-", 0) == 0)
        return structured_child(mode, fd);
    if (std::string(mode) == "timeout" || std::string(mode) == "pre-exit")
        return std::string(mode) == "pre-exit" ? 23 : pause();
    if (std::string(mode) == "partial") {
        (void)write_all(fd, "READY", 5);
        ::close(fd);
        return 0;
    }
    if (std::string(mode) == "extra") {
        (void)write_all(fd, "READY\nX", 7);
        ::close(fd);
        return 0;
    }
    if (std::string(mode) == "delayed-extra") {
        (void)write_all(fd, "READY\n", 6);
        ::usleep(30000);
        (void)write_all(fd, "X", 1);
        ::close(fd);
        return 0;
    }
    if (std::string(mode) == "invalid") {
        (void)write_all(fd, "NOPE", 4);
        ::close(fd);
        return 0;
    }
    if (std::string(mode) == "no-close") {
        (void)write_all(fd, "READY\n", 6);
        return pause();
    }
    if (std::string(mode) == "close-empty-live") {
        // Regression for the READY-EOF path: EOF does not imply that the
        // service exited, so the supervisor must never perform a blocking
        // reap here.  Ignore TERM to make teardown exercise its bounded
        // TERM-to-KILL path after rejecting the empty message.
        (void)::signal(SIGTERM, SIG_IGN);
        ::close(fd);
        return pause();
    }
    if (std::string(mode) == "pre-ready-grandchild") {
        // Exit before READY after forking a TERM-ignoring helper.  Once the
        // leader is exited it cannot be STOP-anchored, so the supervisor must
        // fail closed without signalling its now-unanchored numeric PGID.
        const pid_t helper = ::fork();
        if (helper < 0)
            return 96;
        if (helper == 0) {
            // Keep the READY writer open briefly so the direct leader becomes
            // an exited, unreaped child before the parent observes EOF.  This
            // deterministically exercises the no-live-anchor branch.
            ::usleep(50000);
            ::close(fd);
            (void)::signal(SIGTERM, SIG_IGN);
            (void)::pause();
            _exit(0);
        }
        const char* pid_path = std::getenv("ICECC_GRANDCHILD_PID_FILE");
        if (pid_path == nullptr)
            return 97;
        const int pid_file = ::open(pid_path, O_WRONLY | O_APPEND);
        if (pid_file < 0)
            return 98;
        char pid_text[32]{ };
        const int pid_size = std::snprintf(pid_text, sizeof(pid_text), "%ld\n",
                                           static_cast<long>(helper));
        const bool written = pid_size > 0 &&
                             write_all(pid_file, pid_text, static_cast<size_t>(pid_size));
        ::close(pid_file);
        return written ? 0 : 99;
    }
    if (std::string(mode) == "move-group") {
        const char* helper_path = std::getenv("ICECC_MOVED_HELPER_PID_FILE");
        if (helper_path == nullptr)
            return 102;
        const pid_t helper = ::fork();
        if (helper < 0)
            return 103;
        if (helper == 0) {
            ::close(fd);
            (void)::signal(SIGTERM, SIG_IGN);
            (void)::pause();
            _exit(0);
        }
        const int helper_file = ::open(helper_path, O_WRONLY | O_TRUNC);
        if (helper_file < 0)
            return 104;
        char helper_text[32]{ };
        const int helper_size = std::snprintf(helper_text, sizeof(helper_text), "%ld\n",
                                              static_cast<long>(helper));
        const bool helper_written = helper_size > 0 &&
                                     write_all(helper_file, helper_text,
                                               static_cast<size_t>(helper_size));
        ::close(helper_file);
        if (!helper_written)
            return 105;
        const char* group_text = std::getenv("ICECC_MOVE_TO_PGID");
        if (group_text == nullptr)
            return 106;
        const long group = std::strtol(group_text, nullptr, 10);
        // The supervisor launched us as a session leader.  Joining a sibling
        // group from the daemon's original session must be kernel-refused.
        errno = 0;
        if (group <= 1 || ::setpgid(0, static_cast<pid_t>(group)) == 0 ||
            errno != EPERM)
            return 107;
    }
    if (std::string(mode) == "sentinel") {
        const char* sentinel = std::getenv("ICECC_SENTINEL_FD");
        if (sentinel != nullptr) {
            char proc_path[64]{};
            std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%s", sentinel);
            if (::access(proc_path, F_OK) == 0)
                return 91;
        }
    }
    if (std::string(mode) == "grandchild") {
        const pid_t helper = ::fork();
        if (helper < 0)
            return 93;
        if (helper == 0) {
            ::close(fd);
            (void)::signal(SIGTERM, SIG_IGN);
            (void)::pause();
            _exit(0);
        }
        const char* pid_path = std::getenv("ICECC_GRANDCHILD_PID_FILE");
        if (pid_path == nullptr)
            return 94;
        const int pid_file = ::open(pid_path, O_WRONLY | O_TRUNC);
        if (pid_file < 0)
            return 95;
        char pid_text[32]{};
        const int pid_size = std::snprintf(pid_text, sizeof(pid_text), "%ld\n",
                                           static_cast<long>(helper));
        (void)write_all(pid_file, pid_text, static_cast<size_t>(pid_size));
        ::close(pid_file);
    }
    const bool ignores_term = std::string(mode) == "ready-live" ||
                              std::string(mode) == "grandchild" ||
                              std::string(mode) == "move-group";
    if (ignores_term)
        (void)::signal(SIGTERM, SIG_IGN);
    if (!write_all(fd, "READY\n", 6))
        return 92;
    ::close(fd);
    if (std::string(mode) == "ready-exit" || std::string(mode) == "sentinel")
        return std::string(mode) == "ready-exit" ? 0 : pause();
    // Keep the process and its process group alive so shutdown tests exercise
    // TERM->KILL and descendant ownership without a shell child.
    (void)::signal(SIGTERM, SIG_IGN);
    return pause();
}

Config fake_config(const char* mode, uint32_t max_restarts = 0) {
    Config config;
    config.executable = "/proc/self/exe";
    config.arguments = {"--fake-child", mode};
    config.readiness_timeout = std::chrono::milliseconds(150);
    config.shutdown_timeout = std::chrono::milliseconds(60);
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
    // Sanitizer startup and fork/exec instrumentation can exceed the normal
    // lifecycle budget; the assertions below still exercise bounded cleanup.
    config.readiness_timeout = std::chrono::milliseconds(1000);
    config.shutdown_timeout = std::chrono::milliseconds(150);
#endif
    config.restart_window = std::chrono::milliseconds(500);
    config.max_restarts = max_restarts;
    return config;
}

std::string make_private_root() {
    char pattern[] = "/tmp/icecc-sidecar-lease-XXXXXX";
    char* root = ::mkdtemp(pattern);
    CHECK(root != nullptr);
    CHECK(::chmod(root, 0700) == 0);
    return root;
}

void remove_test_lease_root(const std::string& root) {
    DIR* directory = ::opendir(root.c_str());
    CHECK(directory != nullptr);
    for (;;) {
        errno = 0;
        dirent* entry = ::readdir(directory);
        if (entry == nullptr) {
            CHECK(errno == 0);
            break;
        }
        if (std::string(entry->d_name) == "." || std::string(entry->d_name) == "..")
            continue;
        const std::string lease_directory = root + "/" + entry->d_name;
        const std::string socket = lease_directory + "/cache.sock";
        struct stat info{};
        if (::lstat(socket.c_str(), &info) == 0) {
            CHECK(S_ISSOCK(info.st_mode));
            CHECK(::unlink(socket.c_str()) == 0);
        }
        CHECK(::rmdir(lease_directory.c_str()) == 0);
    }
    CHECK(::closedir(directory) == 0);
    CHECK(::rmdir(root.c_str()) == 0);
}

Config structured_config(const char* mode, const std::string& root,
                         const std::shared_ptr<LaunchIdentityAllocator>& allocator,
                         uint32_t max_restarts = 3) {
    Config config = fake_config(mode, max_restarts);
    config.lease_root = root;
    config.launch_identities = allocator;
    return config;
}

void validation_and_exec_failure() {
    Config invalid;
    CHECK(!Supervisor::valid_config(invalid));
    invalid.executable = "relative-service";
    CHECK(!Supervisor::valid_config(invalid));
    invalid.executable = "/bin/sh";
    invalid.restart_window = std::chrono::milliseconds(0);
    CHECK(!Supervisor::valid_config(invalid));
    invalid.restart_window = std::chrono::milliseconds(1);
    invalid.max_attempts_per_recovery = 0;
    CHECK(!Supervisor::valid_config(invalid));
    invalid.max_attempts_per_recovery = 16;
    invalid.max_restarts = std::numeric_limits<uint32_t>::max();
    CHECK(!Supervisor::valid_config(invalid));

    const std::string lease_root = make_private_root();
    Config missing_allocator = fake_config("structured-live");
    missing_allocator.lease_root = lease_root;
    CHECK(!Supervisor::valid_config(missing_allocator));
    CHECK(::rmdir(lease_root.c_str()) == 0);

    // This is a regular executable, so configuration validation succeeds;
    // execve then reports ENOENT for its deliberately missing interpreter.
    char path[] = "/tmp/icecc-sidecar-exec-XXXXXX";
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0);
    const char script[] = "#!/icecc-interpreter-that-does-not-exist\nexit 0\n";
    CHECK(::write(fd, script, sizeof(script) - 1) == static_cast<ssize_t>(sizeof(script) - 1));
    CHECK(::fchmod(fd, 0700) == 0);
    CHECK(::close(fd) == 0);

    Config config = fake_config("pre-exit");
    config.executable = path;
    config.readiness_timeout = std::chrono::milliseconds(100);
    Supervisor supervisor(config);
    CHECK(Supervisor::valid_config(config));
    CHECK(!supervisor.start());
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().exec_failures >= 1);
    CHECK(supervisor.counters().launches == 1);
    CHECK(!supervisor.has_private_fds());
    CHECK(::unlink(path) == 0);
}

void structured_lease_rotates_across_restart_and_controller_recreation() {
    const std::string root = make_private_root();
    auto allocator = std::make_shared<LaunchIdentityAllocator>(71, 1);
    Config config = structured_config("structured-live", root, allocator, 4);
    Supervisor first(config);
    CHECK(first.start());
    CHECK(first.current_lease().has_value());
    const ReadyLease lease1 = *first.current_lease();
    CHECK(lease1.valid());
    ReadyLease noncanonical = lease1;
    noncanonical.f_store_guid.bytes[0] ^= 1;
    CHECK(!noncanonical.valid());
    noncanonical = lease1;
    noncanonical.socket_path_digest.bytes[0] ^= 1;
    CHECK(!noncanonical.valid());
    noncanonical = lease1;
    noncanonical.private_directory += "/../alias";
    CHECK(!noncanonical.valid());
    CHECK((lease1.identity == icecc::p50::local::Identity{71, 1}));
    CHECK(lease1.f_store_guid ==
          icecc::p50::f_store_guid_for_incarnation(lease1.identity));
    CHECK(::kill(first.child_pid(), SIGKILL) == 0);
    bool restarted = false;
    for (int attempt = 0; attempt != 100 && !restarted; ++attempt) {
        restarted = first.poll() && first.current_lease().has_value() &&
                    first.current_lease()->identity.attempt == 2;
        if (!restarted)
            ::usleep(5000);
    }
    CHECK(restarted);
    const ReadyLease lease2 = *first.current_lease();
    CHECK(lease2.valid());
    CHECK((lease2.identity == icecc::p50::local::Identity{71, 2}));
    CHECK(lease2.f_store_guid ==
          icecc::p50::f_store_guid_for_incarnation(lease2.identity));
    CHECK(lease2.f_store_guid != lease1.f_store_guid);
    CHECK(lease2.private_directory != lease1.private_directory);
    CHECK(lease2.socket_path != lease1.socket_path);
    first.shutdown();

    Supervisor replacement(structured_config("structured-live", root, allocator));
    CHECK(replacement.start());
    CHECK(replacement.current_lease().has_value());
    CHECK((replacement.current_lease()->identity ==
           icecc::p50::local::Identity{71, 3}));
    CHECK(replacement.current_lease()->f_store_guid ==
          icecc::p50::f_store_guid_for_incarnation(
              replacement.current_lease()->identity));
    replacement.shutdown();
    CHECK(::rmdir(root.c_str()) == 0);
}

void structured_dead_or_malformed_ready_is_never_current() {
    for (const char* mode : {"structured-exit", "structured-wrong-pid",
                             "structured-trailing-space", "structured-double-space"}) {
        const std::string root = make_private_root();
        auto allocator = std::make_shared<LaunchIdentityAllocator>(81, 1);
        Config config = structured_config(mode, root, allocator, 0);
        config.max_attempts_per_recovery = 1;
        Supervisor supervisor(config);
        CHECK(!supervisor.start());
        CHECK(!supervisor.current_lease().has_value());
        CHECK(supervisor.state() == State::DegradedLegacy);
        CHECK(supervisor.counters().pre_ready_exits +
                  supervisor.counters().invalid_ready_messages >=
              1);
        remove_test_lease_root(root);
    }
}

void cleanup_never_deletes_replaced_socket() {
    const std::string root = make_private_root();
    auto allocator = std::make_shared<LaunchIdentityAllocator>(91, 1);
    Supervisor supervisor(structured_config("structured-live", root, allocator));
    CHECK(supervisor.start());
    const ReadyLease lease = *supervisor.current_lease();
    CHECK(::unlink(lease.socket_path.c_str()) == 0);
    const int replacement = structured_listener(lease.socket_path.c_str());
    CHECK(replacement >= 0);
    supervisor.shutdown();

    struct stat replacement_info{};
    CHECK(::lstat(lease.socket_path.c_str(), &replacement_info) == 0);
    CHECK(S_ISSOCK(replacement_info.st_mode));
    CHECK(replacement_info.st_ino != lease.listener_inode);
    struct stat directory_info{};
    CHECK(::lstat(lease.private_directory.c_str(), &directory_info) == 0);
    CHECK(directory_info.st_ino == lease.directory_inode);
    CHECK(::close(replacement) == 0);
    CHECK(::unlink(lease.socket_path.c_str()) == 0);
    CHECK(::rmdir(lease.private_directory.c_str()) == 0);
    CHECK(::rmdir(root.c_str()) == 0);
}

void cleanup_never_deletes_replaced_directory() {
    const std::string root = make_private_root();
    auto allocator = std::make_shared<LaunchIdentityAllocator>(92, 1);
    Supervisor supervisor(structured_config("structured-live", root, allocator));
    CHECK(supervisor.start());
    const ReadyLease lease = *supervisor.current_lease();
    const std::string moved = root + "/moved-lease";
    CHECK(::rename(lease.private_directory.c_str(), moved.c_str()) == 0);
    CHECK(::mkdir(lease.private_directory.c_str(), 0700) == 0);
    supervisor.shutdown();

    struct stat replacement_info{};
    CHECK(::lstat(lease.private_directory.c_str(), &replacement_info) == 0);
    CHECK(S_ISDIR(replacement_info.st_mode));
    CHECK(replacement_info.st_ino != lease.directory_inode);
    CHECK(::rmdir(lease.private_directory.c_str()) == 0);
    const std::string moved_socket = moved + "/cache.sock";
    CHECK(::unlink(moved_socket.c_str()) == 0);
    CHECK(::rmdir(moved.c_str()) == 0);
    CHECK(::rmdir(root.c_str()) == 0);
}

void launch_allocator_refuses_reserved_and_exhausted_identities() {
    LaunchIdentityAllocator zero_generation(0, 1);
    CHECK(!zero_generation.allocate().has_value());
    LaunchIdentityAllocator max_generation(std::numeric_limits<uint64_t>::max(), 1);
    CHECK(!max_generation.allocate().has_value());
    LaunchIdentityAllocator zero_attempt(1, 0);
    CHECK(!zero_attempt.allocate().has_value());
    LaunchIdentityAllocator max_attempt(1, std::numeric_limits<uint64_t>::max());
    CHECK(!max_attempt.allocate().has_value());
    LaunchIdentityAllocator last_usable(9,
                                        std::numeric_limits<uint64_t>::max() - 1);
    const auto allocated = last_usable.allocate();
    CHECK(allocated.has_value());
    CHECK(allocated->identity.attempt == std::numeric_limits<uint64_t>::max() - 1);
    CHECK(!last_usable.allocate().has_value());
}

void ready_and_shutdown() {
    Supervisor supervisor(fake_config("ready-live"));
    CHECK(supervisor.start());
    CHECK(supervisor.state() == State::Ready);
    CHECK(supervisor.last_failure() == Failure::None);
    CHECK(supervisor.counters().launches == 1);
    CHECK(supervisor.child_pid() > 0);
    CHECK(supervisor.process_group_id() == supervisor.child_pid());
    CHECK(::getsid(supervisor.child_pid()) == supervisor.child_pid());
    CHECK(!supervisor.has_private_fds());
    const pid_t pid = supervisor.child_pid();
    supervisor.shutdown();
    CHECK(supervisor.state() == State::Stopped);
    CHECK(supervisor.child_pid() < 0);
    CHECK(supervisor.process_group_id() < 0);
    CHECK(supervisor.counters().shutdowns == 1);
    CHECK(supervisor.counters().forced_kills == 1);
    int status = 0;
    CHECK(::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD);
    supervisor.shutdown();
    CHECK(supervisor.counters().shutdowns == 1);
}

void timeout_and_pre_ready_exit_are_distinct() {
    Supervisor timeout(fake_config("timeout"));
    CHECK(!timeout.start());
    CHECK(timeout.state() == State::DegradedLegacy);
    CHECK(timeout.last_failure() == Failure::RestartExhausted);
    CHECK(timeout.counters().readiness_timeouts >= 1);
    CHECK(timeout.counters().pre_ready_exits == 0);
    CHECK(!timeout.has_private_fds());

    Supervisor crash(fake_config("pre-exit"));
    CHECK(!crash.start());
    CHECK(crash.state() == State::DegradedLegacy);
    CHECK(crash.counters().pre_ready_exits + crash.counters().invalid_ready_messages >= 1);
    CHECK(crash.counters().readiness_timeouts == 0);
}

void repeated_post_ready_crashes_exhaust_budget() {
    Supervisor supervisor(fake_config("ready-exit", 2));
    CHECK(supervisor.start());
    CHECK(supervisor.state() == State::Ready);
    bool exhausted = false;
    for (int attempt = 0; attempt != 20 && !exhausted; ++attempt) {
        exhausted = !supervisor.poll();
        if (!exhausted)
            ::usleep(10000);
    }
    CHECK(exhausted);
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.last_failure() == Failure::RestartExhausted);
    CHECK(supervisor.counters().post_ready_exits >= 1);
    CHECK(supervisor.counters().restarts >= 2);
    CHECK(supervisor.counters().launches == 3);
    CHECK(!supervisor.has_private_fds());
}

void invalid_ready_is_bounded() {
    Supervisor supervisor(fake_config("invalid"));
    CHECK(!supervisor.start());
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().invalid_ready_messages + supervisor.counters().pre_ready_exits >= 1);
    CHECK(!supervisor.has_private_fds());
}

void exact_ready_close_protocol() {
    for (const char* mode : {"partial", "extra", "delayed-extra", "invalid"}) {
        Supervisor supervisor(fake_config(mode));
        CHECK(!supervisor.start());
        CHECK(supervisor.state() == State::DegradedLegacy);
        CHECK(supervisor.counters().invalid_ready_messages + supervisor.counters().pre_ready_exits >= 1);
        CHECK(!supervisor.has_private_fds());
        CHECK(supervisor.child_pid() < 0);
    }

    Supervisor no_close(fake_config("no-close"));
    CHECK(!no_close.start());
    CHECK(no_close.state() == State::DegradedLegacy);
    CHECK(no_close.counters().readiness_timeouts >= 1);
    CHECK(!no_close.has_private_fds());
    CHECK(no_close.child_pid() < 0);
}

void empty_ready_eof_from_live_child_is_bounded() {
    Supervisor supervisor(fake_config("close-empty-live"));
    const auto started = std::chrono::steady_clock::now();
    // A regression to blocking waitpid() would otherwise hang the complete
    // test executable rather than produce a useful failing row.
    (void)::alarm(5);
    CHECK(!supervisor.start());
    (void)::alarm(0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(2));
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().invalid_ready_messages >= 1);
    CHECK(supervisor.counters().forced_kills == 1);
    CHECK(!supervisor.has_private_fds());
    CHECK(supervisor.child_pid() < 0);
    CHECK(supervisor.process_group_id() < 0);
}

void pre_ready_exited_leader_refuses_unanchored_group_signal() {
    char pid_path[] = "/tmp/icecc-sidecar-pre-ready-helper-XXXXXX";
    const int path_fd = ::mkstemp(pid_path);
    CHECK(path_fd >= 0);
    CHECK(::close(path_fd) == 0);
    CHECK(::setenv("ICECC_GRANDCHILD_PID_FILE", pid_path, 1) == 0);

    Config config = fake_config("pre-ready-grandchild", 10);
    config.readiness_timeout = std::chrono::milliseconds(100);
    config.shutdown_timeout = std::chrono::milliseconds(40);
    config.max_attempts_per_recovery = 2;
    Supervisor supervisor(config);
    (void)::alarm(5);
    CHECK(!supervisor.start());
    (void)::alarm(0);
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().launches == 1);
    CHECK(supervisor.counters().forced_kills == 0);
    CHECK(supervisor.process_group_id() < 0);
    CHECK(supervisor.child_pid() < 0);
    CHECK(!supervisor.has_private_fds());
    CHECK(::unsetenv("ICECC_GRANDCHILD_PID_FILE") == 0);

    pid_t helper = -1;
    for (int attempt = 0; attempt != 50 && helper < 0; ++attempt) {
        const int fd = ::open(pid_path, O_RDONLY);
        if (fd >= 0) {
            char text[32]{};
            const ssize_t bytes = ::read(fd, text, sizeof(text) - 1);
            ::close(fd);
            if (bytes > 0)
                helper = static_cast<pid_t>(std::strtol(text, nullptr, 10));
        }
        if (helper < 0)
            ::usleep(5000);
    }
    CHECK(helper > 1);
    // Surviving proves teardown did not signal a numeric group after losing
    // its exact live-leader anchor.  The test owns this deliberately leaked
    // helper and performs the external/manual recovery promised by the API.
    CHECK(::kill(helper, 0) == 0);
    CHECK(::kill(helper, SIGKILL) == 0 || errno == ESRCH);
    bool gone = false;
    for (int attempt = 0; attempt != 100 && !gone; ++attempt) {
        errno = 0;
        gone = ::kill(helper, 0) < 0 && errno == ESRCH;
        if (!gone)
            ::usleep(5000);
    }
    CHECK(gone);
    CHECK(::unlink(pid_path) == 0);
}

void ambient_fd_is_not_inherited() {
    char path[] = "/tmp/icecc-sidecar-sentinel-XXXXXX";
    const int sentinel_fd = ::mkstemp(path);
    CHECK(sentinel_fd >= 0);
    const int flags = ::fcntl(sentinel_fd, F_GETFD);
    CHECK(flags >= 0);
    CHECK(::fcntl(sentinel_fd, F_SETFD, flags & ~FD_CLOEXEC) == 0);
    const std::string number = std::to_string(sentinel_fd);
    CHECK(::setenv("ICECC_SENTINEL_FD", number.c_str(), 1) == 0);
    Supervisor supervisor(fake_config("sentinel"));
    CHECK(supervisor.start());
    CHECK(supervisor.state() == State::Ready);
    CHECK(::unsetenv("ICECC_SENTINEL_FD") == 0);
    CHECK(::fcntl(sentinel_fd, F_GETFD) >= 0);
    supervisor.shutdown();
    CHECK(!supervisor.has_private_fds());
    CHECK(supervisor.child_pid() < 0);
    CHECK(::close(sentinel_fd) == 0);
    CHECK(::unlink(path) == 0);
}

#if defined(ICECC_P50_FORCE_FD_FALLBACK)
void forced_fd_fallback_is_bounded_and_excludes_ambient_fd() {
    const auto started = std::chrono::steady_clock::now();
    (void)::alarm(3);
    ambient_fd_is_not_inherited();
    (void)::alarm(0);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

void forced_fd_fallback_handles_high_ambient_fd() {
    char path[] = "/tmp/icecc-sidecar-high-sentinel-XXXXXX";
    const int base_fd = ::mkstemp(path);
    CHECK(base_fd >= 0);
    const int high_fd = ::fcntl(base_fd, F_DUPFD, 9000);
    CHECK(high_fd >= 9000);
    CHECK(::close(base_fd) == 0);
    const int flags = ::fcntl(high_fd, F_GETFD);
    CHECK(flags >= 0);
    CHECK(::fcntl(high_fd, F_SETFD, flags & ~FD_CLOEXEC) == 0);
    const std::string number = std::to_string(high_fd);
    CHECK(::setenv("ICECC_SENTINEL_FD", number.c_str(), 1) == 0);
    Supervisor supervisor(fake_config("sentinel"));
    CHECK(supervisor.start());
    CHECK(::unsetenv("ICECC_SENTINEL_FD") == 0);
    supervisor.shutdown();
    CHECK(::close(high_fd) == 0);
    CHECK(::unlink(path) == 0);
}
#endif

void session_leader_refuses_group_escape() {
    char helper_path[] = "/tmp/icecc-sidecar-moved-helper-XXXXXX";
    const int helper_file = ::mkstemp(helper_path);
    CHECK(helper_file >= 0);
    CHECK(::close(helper_file) == 0);
    const pid_t sentinel = ::fork();
    CHECK(sentinel >= 0);
    if (sentinel == 0) {
        (void)::setpgid(0, 0);
        (void)::signal(SIGTERM, SIG_IGN);
        (void)::pause();
        _exit(0);
    }
    CHECK(::setpgid(sentinel, sentinel) == 0 || errno == EACCES);
    const std::string group_text = std::to_string(sentinel);
    CHECK(::setenv("ICECC_MOVE_TO_PGID", group_text.c_str(), 1) == 0);
    CHECK(::setenv("ICECC_MOVED_HELPER_PID_FILE", helper_path, 1) == 0);
    Supervisor supervisor(fake_config("move-group"));
    (void)::alarm(5);
    CHECK(supervisor.start());
    const pid_t exact_child = supervisor.child_pid();
    CHECK(exact_child > 1);
    CHECK(supervisor.process_group_id() > 1);
    CHECK(supervisor.process_group_id() != sentinel);
    CHECK(::getsid(exact_child) == exact_child);
    supervisor.shutdown();
    (void)::alarm(0);
    CHECK(::unsetenv("ICECC_MOVE_TO_PGID") == 0);
    CHECK(::unsetenv("ICECC_MOVED_HELPER_PID_FILE") == 0);

    CHECK(supervisor.child_pid() < 0);
    CHECK(supervisor.state() == State::Stopped);
    CHECK(!supervisor.has_private_fds());

    pid_t helper = -1;
    const int read_file = ::open(helper_path, O_RDONLY);
    CHECK(read_file >= 0);
    char helper_text[32]{};
    const ssize_t helper_bytes = ::read(read_file, helper_text, sizeof(helper_text) - 1);
    CHECK(::close(read_file) == 0);
    CHECK(helper_bytes > 0);
    helper = static_cast<pid_t>(std::strtol(helper_text, nullptr, 10));
    CHECK(helper > 1);
    bool helper_gone = false;
    for (int attempt = 0; attempt != 100; ++attempt) {
        errno = 0;
        if (::kill(helper, 0) < 0 && errno == ESRCH) {
            helper_gone = true;
            break;
        }
        ::usleep(5000);
    }
    if (!helper_gone)
        (void)::kill(helper, SIGKILL);
    CHECK(helper_gone);
    CHECK(::unlink(helper_path) == 0);

    errno = 0;
    CHECK(::kill(sentinel, 0) == 0);
    CHECK(::kill(sentinel, SIGKILL) == 0 || errno == ESRCH);
    int status = 0;
    while (::waitpid(sentinel, &status, 0) < 0 && errno == EINTR)
        ;
}

void shutdown_owns_process_group_against_external_reaper() {
    char pid_path[] = "/tmp/icecc-sidecar-helper-XXXXXX";
    const int path_fd = ::mkstemp(pid_path);
    CHECK(path_fd >= 0);
    CHECK(::close(path_fd) == 0);
    CHECK(::setenv("ICECC_GRANDCHILD_PID_FILE", pid_path, 1) == 0);
    Supervisor supervisor(fake_config("grandchild"));
    CHECK(supervisor.start());
    CHECK(supervisor.state() == State::Ready);
    CHECK(::unsetenv("ICECC_GRANDCHILD_PID_FILE") == 0);

    pid_t helper = -1;
    for (int attempt = 0; attempt != 20 && helper < 0; ++attempt) {
        const int fd = ::open(pid_path, O_RDONLY);
        if (fd >= 0) {
            char text[32]{};
            const ssize_t bytes = ::read(fd, text, sizeof(text) - 1);
            ::close(fd);
            if (bytes > 0)
                helper = static_cast<pid_t>(std::strtol(text, nullptr, 10));
        }
        if (helper < 0)
            ::usleep(5000);
    }
    CHECK(helper > 1);
    const pid_t exact_child = supervisor.child_pid();
    CHECK(exact_child > 1);
    pid_t reaped = -2;
    int reap_error = 0;
    std::thread competing_reaper([&]() {
        int status = 0;
        errno = 0;
        reaped = ::waitpid(exact_child, &status, 0);
        reap_error = errno;
    });
    // Let the competing owner enter waitpid.  It cannot reap the live leader;
    // shutdown must STOP-anchor it before using the numeric group, so no
    // reaped/reused numeric PID can turn that group signal into a new target.
    ::usleep(5000);
    supervisor.shutdown();
    competing_reaper.join();
    CHECK(reaped == exact_child || (reaped < 0 && reap_error == ECHILD));
    bool gone = false;
    for (int attempt = 0; attempt != 40 && !gone; ++attempt) {
        if (::kill(helper, 0) < 0 && errno == ESRCH)
            gone = true;
        if (!gone)
            ::usleep(5000);
    }
    CHECK(gone);
    CHECK(supervisor.counters().forced_kills == 1);
    CHECK(!supervisor.has_private_fds());
    CHECK(supervisor.child_pid() < 0);
    CHECK(::unlink(pid_path) == 0);
}

void restart_window_ages_without_unbounded_call() {
    Config config = fake_config("ready-exit", 1);
    config.restart_window = std::chrono::milliseconds(30);
    config.max_attempts_per_recovery = 2;
    Supervisor supervisor(config);
    CHECK(supervisor.start());
    for (int attempt = 0; attempt != 40 && supervisor.counters().restarts < 1; ++attempt) {
        (void)supervisor.poll();
        ::usleep(5000);
    }
    CHECK(supervisor.counters().restarts >= 1);
    ::usleep(60000);
    for (int attempt = 0; attempt != 40 && supervisor.counters().restarts < 2; ++attempt) {
        (void)supervisor.poll();
        ::usleep(5000);
    }
    CHECK(supervisor.counters().restarts >= 2);
    CHECK(supervisor.state() == State::Ready);
    supervisor.shutdown();
}

void bounded_recovery_attempts() {
    Config config = fake_config("timeout", 100);
    config.readiness_timeout = std::chrono::milliseconds(20);
    config.restart_window = std::chrono::milliseconds(1);
    config.max_attempts_per_recovery = 2;
    Supervisor supervisor(config);
    const auto started = std::chrono::steady_clock::now();
    CHECK(!supervisor.start());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(1));
    CHECK(supervisor.state() == State::DegradedLegacy);
    CHECK(supervisor.counters().launches == 2);
    CHECK(!supervisor.has_private_fds());
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--fake-child")
        return fake_child(argv[2]);
    try {
        validation_and_exec_failure();
        launch_allocator_refuses_reserved_and_exhausted_identities();
        structured_lease_rotates_across_restart_and_controller_recreation();
        structured_dead_or_malformed_ready_is_never_current();
        cleanup_never_deletes_replaced_socket();
        cleanup_never_deletes_replaced_directory();
        ready_and_shutdown();
        timeout_and_pre_ready_exit_are_distinct();
        repeated_post_ready_crashes_exhaust_budget();
        invalid_ready_is_bounded();
        exact_ready_close_protocol();
        empty_ready_eof_from_live_child_is_bounded();
        pre_ready_exited_leader_refuses_unanchored_group_signal();
        ambient_fd_is_not_inherited();
#if defined(ICECC_P50_FORCE_FD_FALLBACK)
        forced_fd_fallback_is_bounded_and_excludes_ambient_fd();
        forced_fd_fallback_handles_high_ambient_fd();
#endif
        session_leader_refuses_group_escape();
        shutdown_owns_process_group_against_external_reaper();
        restart_window_ages_without_unbounded_call();
        bounded_recovery_attempts();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "p50sidecarsupervisor: %s\n", error.what());
        return EXIT_FAILURE;
    }
    std::puts("p50sidecarsupervisor: ok");
    return EXIT_SUCCESS;
}
