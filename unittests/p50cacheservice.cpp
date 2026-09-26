#include "../cache/p50_cache_service.h"
#include "../cache/p50_control_operation.h"
#include "comm.h"
#include "../services/digest128.h"

#include <array>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <fcntl.h>
#include <grp.h>
#include <map>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <thread>
#include <tuple>

using namespace icecc::p50;

namespace {

constexpr int kStartupReadyTimeoutMilliseconds = 5000;

size_t process_open_fd_count() {
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory == nullptr)
        throw std::runtime_error("cannot enumerate /proc/self/fd");
    size_t count = 0;
    int read_error = 0;
    for (;;) {
        errno = 0;
        dirent* entry = ::readdir(directory);
        if (entry == nullptr) {
            read_error = errno;
            break;
        }
        if (entry->d_name[0] == '.')
            continue;
        char* end = nullptr;
        (void)std::strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == '\0')
            ++count;
    }
    (void)::closedir(directory);
    if (read_error != 0)
        throw std::runtime_error("cannot read /proc/self/fd");
    return count;
}

std::map<int, std::string> process_open_fd_snapshot() {
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory == nullptr)
        throw std::runtime_error("cannot enumerate /proc/self/fd");
    struct DirectoryGuard {
        DIR* value;
        ~DirectoryGuard() {
            if (value != nullptr)
                (void)::closedir(value);
        }
    } guard{directory};
    const int directory_fd = ::dirfd(directory);
    std::map<int, std::string> snapshot;
    int snapshot_error = 0;
    for (;;) {
        errno = 0;
        dirent* entry = ::readdir(directory);
        if (entry == nullptr) {
            snapshot_error = errno;
            break;
        }
        if (entry->d_name[0] == '.')
            continue;
        char* end = nullptr;
        const long descriptor = std::strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' ||
            descriptor == directory_fd)
            continue;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/self/fd/%ld", descriptor);
        char target[512];
        const ssize_t target_size = ::readlink(path, target, sizeof(target) - 1);
        if (target_size < 0) {
            snapshot_error = errno == 0 ? EIO : errno;
            break;
        }
        if (target_size == static_cast<ssize_t>(sizeof(target) - 1)) {
            snapshot_error = EOVERFLOW;
            break;
        }
        target[target_size] = '\0';
        snapshot.emplace(static_cast<int>(descriptor), target);
    }
    if (::closedir(directory) != 0 && snapshot_error == 0)
        snapshot_error = errno == 0 ? EIO : errno;
    guard.value = nullptr;
    if (snapshot_error != 0)
        throw std::runtime_error(std::string("cannot complete /proc/self/fd snapshot: ") +
                                 std::strerror(snapshot_error));
    return snapshot;
}

void check(bool condition, const char* expression) {
    if (!condition)
        throw std::runtime_error(expression);
}

#define CHECK(expression) check((expression), #expression)

std::string service_path() {
    const char* value = std::getenv("ICECC_TEST_CACHE_SERVICE");
    if (value == nullptr || value[0] != '/')
        throw std::runtime_error("ICECC_TEST_CACHE_SERVICE must be absolute");
    return value;
}

std::string ready_close_shim_path() {
    const char* value = std::getenv("ICECC_TEST_READY_CLOSE_SHIM");
    if (value == nullptr || value[0] != '/')
        throw std::runtime_error("ICECC_TEST_READY_CLOSE_SHIM must be absolute");
    return value;
}

bool write_all(int fd, std::span<const uint8_t> bytes) {
    size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t result = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (result > 0)
            offset += static_cast<size_t>(result);
        else if (result < 0 && errno == EINTR)
            continue;
        else
            return false;
    }
    return true;
}

bool send_byte_no_signal(int fd, uint8_t byte) {
    int flags = 0;
#if defined(MSG_NOSIGNAL)
    flags = MSG_NOSIGNAL;
#endif
    for (;;) {
        const ssize_t result = ::send(fd, &byte, 1, flags);
        if (result == 1)
            return true;
        if (result < 0 && errno == EINTR)
            continue;
        return false;
    }
}

std::string hex_id(const FStoreGuid& value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(value.bytes.size() * 2, '0');
    for (size_t index = 0; index != value.bytes.size(); ++index) {
        result[index * 2] = digits[value.bytes[index] >> 4];
        result[index * 2 + 1] = digits[value.bytes[index] & 0x0f];
    }
    return result;
}

void clear_structured_launch_environment() {
    for (const char* name : {
             "ICECC_CACHE_SERVICE_READY_FORMAT",
             "ICECC_CACHE_SERVICE_EXPECTED_GENERATION",
             "ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT",
             "ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GENERATION",
             "ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION",
             "ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID",
             "ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID",
             "ICECC_CACHE_SERVICE_EXPECTED_SOCKET",
             "ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST",
             "ICECC_CACHE_SERVICE_LISTENER_FD",
         })
        (void)::unsetenv(name);
}

std::string read_bounded_to_eof(int fd) {
    std::string result;
    for (;;) {
        struct pollfd descriptor{fd, POLLIN | POLLHUP | POLLERR, 0};
        CHECK(::poll(&descriptor, 1, kStartupReadyTimeoutMilliseconds) > 0);
        char bytes[256]{};
        const ssize_t count = ::read(fd, bytes, sizeof(bytes));
        if (count == 0)
            return result;
        CHECK(count > 0);
        result.append(bytes, static_cast<size_t>(count));
        CHECK(result.size() <= 2048);
    }
}

struct Child {
    pid_t pid = -1;
    int ready_read = -1;

    Child() = default;
    Child(pid_t child, int descriptor) : pid(child), ready_read(descriptor) {}
    Child(Child&& other) noexcept : pid(other.pid), ready_read(other.ready_read) {
        other.pid = -1;
        other.ready_read = -1;
    }
    Child& operator=(Child&& other) noexcept {
        if (this != &other) {
            pid = other.pid;
            ready_read = other.ready_read;
            other.pid = -1;
            other.ready_read = -1;
        }
        return *this;
    }

    ~Child() {
        if (pid > 0) {
            (void)::kill(pid, SIGTERM);
            int status = 0;
            (void)::waitpid(pid, &status, 0);
        }
        if (ready_read >= 0)
            (void)::close(ready_read);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
};

struct ReapOnFailure {
    pid_t pid = -1;
    std::array<int*, 5> descriptors{};

    ReapOnFailure(pid_t child, std::array<int*, 5> values)
        : pid(child), descriptors(values) {}

    ~ReapOnFailure() {
        if (pid > 0) {
            (void)::kill(pid, SIGKILL);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
        for (int* descriptor : descriptors) {
            if (descriptor != nullptr && *descriptor >= 0) {
                (void)::close(*descriptor);
                *descriptor = -1;
            }
        }
    }
    ReapOnFailure(const ReapOnFailure&) = delete;
    ReapOnFailure& operator=(const ReapOnFailure&) = delete;
};

bool wait_for_exit_bounded(pid_t pid, int timeout_milliseconds, int& status);

void expect_exact_ready_then_eof(int fd) {
    std::array<char, 6> message{};
    size_t received = 0;
    while (received != message.size()) {
        struct pollfd descriptor{fd, POLLIN | POLLHUP, 0};
        CHECK(::poll(&descriptor, 1, kStartupReadyTimeoutMilliseconds) > 0);
        const ssize_t result =
            ::read(fd, message.data() + received, message.size() - received);
        CHECK(result > 0);
        received += static_cast<size_t>(result);
    }
    CHECK(std::string_view(message.data(), message.size()) == "READY\n");
    struct pollfd ready_eof{fd, POLLIN | POLLHUP, 0};
    CHECK(::poll(&ready_eof, 1, 1000) > 0);
    char trailing = 0;
    CHECK(::read(fd, &trailing, 1) == 0);
}

void expect_structured_ready(int fd) {
    std::string message;
    for (;;) {
        struct pollfd descriptor{fd, POLLIN | POLLHUP, 0};
        CHECK(::poll(&descriptor, 1, kStartupReadyTimeoutMilliseconds) > 0);
        char bytes[256]{};
        const ssize_t result = ::read(fd, bytes, sizeof(bytes));
        CHECK(result > 0);
        message.append(bytes, static_cast<size_t>(result));
        const size_t newline = message.find('\n');
        if (newline != std::string::npos) {
            CHECK(message.substr(0, newline + 1).rfind(
                      "READY v2 generation=123 attempt=1 ", 0) == 0);
            return;
        }
        CHECK(message.size() <= 2048);
    }
}

Child launch(const std::string& directory, uint64_t generation = 7, uint64_t attempt = 1,
             uint64_t peer_uid = static_cast<uint64_t>(::getuid())) {
    int ready[2] = {-1, -1};
    CHECK(::pipe(ready) == 0);
    const int flags = ::fcntl(ready[1], F_GETFD);
    CHECK(flags >= 0);
    CHECK(::fcntl(ready[1], F_SETFD, flags & ~FD_CLOEXEC) == 0);
    const std::string socket = directory + "/service.sock";
    const std::string uid = std::to_string(peer_uid);
    const std::string gid = std::to_string(static_cast<uint64_t>(::getgid()));
    const std::string gen = std::to_string(generation);
    const std::string att = std::to_string(attempt);
    const std::string ready_fd = std::to_string(ready[1]);
    const std::string executable = service_path();
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        (void)::close(ready[0]);
        (void)::setenv("ICECC_CACHE_SERVICE_READY_FD", ready_fd.c_str(), 1);
        ::execl(executable.c_str(), executable.c_str(), "--socket", socket.c_str(),
                "--peer-uid", uid.c_str(), "--peer-gid", gid.c_str(), "--generation",
                gen.c_str(), "--attempt", att.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    (void)::close(ready[1]);
    Child child{pid, ready[0]};
    expect_exact_ready_then_eof(child.ready_read);
    (void)::close(child.ready_read);
    child.ready_read = -1;
    struct stat socket_info{};
    CHECK(::lstat(socket.c_str(), &socket_info) == 0 && S_ISSOCK(socket_info.st_mode));
    CHECK((socket_info.st_mode & 07777) == 0600);
    return child;
}

void fingerprint_stop_before_ready_is_bounded() {
    char template_path[] = "/tmp/icecc-p29-fingerprint-stop-XXXXXX";
    CHECK(::mkdtemp(template_path) != nullptr);
    CHECK(::chmod(template_path, 0700) == 0);
    const std::string root = template_path;
    const std::string lease = root + "/lease";
    CHECK(::mkdir(lease.c_str(), 0700) == 0);
    const std::string socket = lease + "/cache.sock";
    const std::string lock_path = root + "/p29-system-source-fingerprint-v1.lock";
    const std::string fingerprint_trace = root + "/fingerprint.trace";
    const int held_lock = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    CHECK(held_lock >= 0 && ::flock(held_lock, LOCK_EX) == 0);

    StoreIdentityRoot store_root{};
    store_root.bytes[15] = 0x71;
    const std::string f_guid = hex_id(f_store_guid_for_root(store_root));
    const std::string c_guid = hex_id(c_store_guid_for_root(store_root));
    const std::string socket_digest =
        icecc::digest128_hex(icecc::digest128(socket));
    const std::string uid = std::to_string(static_cast<uint64_t>(::getuid()));
    const std::string gid = std::to_string(static_cast<uint64_t>(::getgid()));
    const std::string executable = service_path();

    auto launch_structured = [&](int listener, int ready_write) {
        const pid_t pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            clear_structured_launch_environment();
            (void)::setenv("ICECC_P50_TEST_FINGERPRINT_TRACE", fingerprint_trace.c_str(), 1);
            const std::string ready_fd = std::to_string(ready_write);
            const std::string listener_fd = std::to_string(listener);
            (void)::setenv("ICECC_CACHE_SERVICE_READY_FD", ready_fd.c_str(), 1);
            (void)::setenv("ICECC_CACHE_SERVICE_READY_FORMAT", "2", 1);
            (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_GENERATION", "123", 1);
            (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT", "1", 1);
            (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GENERATION", "191", 1);
            (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION", "1", 1);
            (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID", c_guid.c_str(), 1);
            (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID", f_guid.c_str(), 1);
            (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_SOCKET", socket.c_str(), 1);
            (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST", socket_digest.c_str(), 1);
            (void)::setenv("ICECC_CACHE_SERVICE_LISTENER_FD", listener_fd.c_str(), 1);
            ::execl(executable.c_str(), executable.c_str(), "--socket", socket.c_str(),
                    "--peer-uid", uid.c_str(), "--peer-gid", gid.c_str(),
                    "--generation", "123", "--attempt", "1", "--f-store-generation",
                    "191", "--store-derivation-version", "1", "--c-store-guid",
                    c_guid.c_str(), "--f-store-guid", f_guid.c_str(),
                    static_cast<char*>(nullptr));
            _exit(127);
        }
        return pid;
    };

    auto launch_attempt = [&]() {
        local::Status status = local::Status::Ok;
        const int listener = local::listen_unix(socket, 1, &status);
        CHECK(listener >= 0 && status == local::Status::Ok);
        const int listener_flags = ::fcntl(listener, F_GETFD);
        CHECK(listener_flags >= 0 &&
              ::fcntl(listener, F_SETFD, listener_flags & ~FD_CLOEXEC) == 0);
        int ready[2] = {-1, -1};
        CHECK(::pipe(ready) == 0);
        const int ready_flags = ::fcntl(ready[1], F_GETFD);
        CHECK(ready_flags >= 0 &&
              ::fcntl(ready[1], F_SETFD, ready_flags & ~FD_CLOEXEC) == 0);
        const pid_t pid = launch_structured(listener, ready[1]);
        CHECK(::close(listener) == 0);
        CHECK(::close(ready[1]) == 0);
        return std::pair<pid_t, int>{pid, ready[0]};
    };

    const auto first = launch_attempt();
    const auto stop_started = std::chrono::steady_clock::now();
    CHECK(::usleep(100000) == 0);
    CHECK(::kill(first.first, SIGTERM) == 0);
    int first_status = 0;
    CHECK(::waitpid(first.first, &first_status, 0) == first.first);
    CHECK(std::chrono::steady_clock::now() - stop_started <
          std::chrono::milliseconds(3000));
    CHECK(WIFEXITED(first_status) && WEXITSTATUS(first_status) != 0);
    CHECK(read_bounded_to_eof(first.second).empty());
    CHECK(::close(first.second) == 0);
    CHECK(::unlink(socket.c_str()) == 0);

    CHECK(::flock(held_lock, LOCK_UN) == 0);
    CHECK(::close(held_lock) == 0);
    // The first child is reaped. Preserve its outcome separately so a slow
    // cancellation cannot be confused with the second startup's outcome.
    const std::string first_trace = root + "/fingerprint-first.trace";
    if (::rename(fingerprint_trace.c_str(), first_trace.c_str()) != 0)
        CHECK(errno == ENOENT);
    const auto second = launch_attempt();
    expect_structured_ready(second.second);
    CHECK(::close(second.second) == 0);
    CHECK(::kill(second.first, SIGTERM) == 0);
    int second_status = 0;
    CHECK(::waitpid(second.first, &second_status, 0) == second.first);
    CHECK(WIFEXITED(second_status) && WEXITSTATUS(second_status) == 0);
    const int trace_fd = ::open(fingerprint_trace.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(trace_fd >= 0);
    std::array<char, 4096> trace{};
    const ssize_t trace_size = ::read(trace_fd, trace.data(), trace.size() - 1);
    CHECK(trace_size > 0 && trace_size < static_cast<ssize_t>(trace.size() - 1));
    CHECK(::close(trace_fd) == 0);
    const bool completed = std::strstr(trace.data(), "fingerprint completed\n") != nullptr;
    const bool timed_out = std::strstr(trace.data(), "fingerprint timed-out\n") != nullptr;
    CHECK(completed != timed_out);
    CHECK(std::strstr(trace.data(), "fingerprint unavailable\n") == nullptr);
    struct stat cache_info{};
    const std::string cache_path = root + "/p29-system-source-fingerprint-v1.cache";
    const int cache_status = ::stat(cache_path.c_str(), &cache_info);
    // READY permits a bounded timeout with reuse disabled. Only an explicitly
    // completed fingerprint promises a cache; never infer completion from READY.
    if (completed)
        CHECK(cache_status == 0 && cache_info.st_size > 0);
    else
        CHECK(cache_status == 0 || errno == ENOENT);
    CHECK(::unlink(socket.c_str()) == 0);
    if (cache_status == 0)
        CHECK(::unlink(cache_path.c_str()) == 0);
    CHECK(::unlink(fingerprint_trace.c_str()) == 0);
    if (::unlink(first_trace.c_str()) != 0)
        CHECK(errno == ENOENT);
    CHECK(::unlink(lock_path.c_str()) == 0);
    CHECK(::rmdir(lease.c_str()) == 0);
    CHECK(::rmdir(root.c_str()) == 0);
}

local::Connection connect_to(const std::string& directory) {
    local::Status status = local::Status::Ok;
    local::Connection connection =
        local::connect_unix(directory + "/service.sock", &status);
    CHECK(status == local::Status::Ok && connection.valid());
    return connection;
}

void legacy_store_identity_launches() {
    char template_path[] = "/tmp/icecc-cache-legacy-store-XXXXXX";
    CHECK(::mkdtemp(template_path) != nullptr);
    const std::string directory = template_path;
    {
        Child child = launch(directory, 77, 9);
        local::Connection connection = connect_to(directory);
        CHECK(connection.send(local::make_hello(local::PeerRole::Daemon, {77, 9})) ==
              local::Status::Ok);
        local::Frame acknowledgement;
        CHECK(connection.receive_with_timeout(acknowledgement, 1000) == local::Status::Ok);
        CHECK(local::validate_handshake(acknowledgement, local::MessageType::HelloAck,
                                        local::PeerRole::Sidecar, {77, 9}) == local::Status::Ok);
    }
    CHECK(::rmdir(directory.c_str()) == 0);
}

int raw_connect(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    struct sockaddr_un address{};
    address.sun_family = AF_UNIX;
    CHECK(path.size() < sizeof(address.sun_path));
    std::memcpy(address.sun_path, path.data(), path.size());
    const socklen_t length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    CHECK(::connect(fd, reinterpret_cast<const sockaddr*>(&address), length) == 0);
    return fd;
}

void exercise_root_contract_then_drop_test_process() {
    if (::geteuid() != 0)
        return;

    constexpr uid_t drop_uid = 65534;
    constexpr gid_t drop_gid = 65534;
    const std::string executable = service_path();

    // A privileged invocation without an explicit, complete drop target must
    // fail before bind and emit no readiness byte.
    {
        char template_path[] = "/tmp/icecc-cache-service-root-refuse-XXXXXX";
        const int directory_fd = ::mkstemp(template_path);
        CHECK(directory_fd >= 0);
        CHECK(::close(directory_fd) == 0);
        CHECK(::unlink(template_path) == 0);
        CHECK(::mkdir(template_path, 0700) == 0);
        int ready[2] = {-1, -1};
        CHECK(::pipe(ready) == 0);
        const std::string ready_fd = std::to_string(ready[1]);
        const std::string socket = std::string(template_path) + "/service.sock";
        const pid_t pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            (void)::close(ready[0]);
            (void)::setenv("ICECC_CACHE_SERVICE_READY_FD", ready_fd.c_str(), 1);
            ::execl(executable.c_str(), executable.c_str(), "--socket", socket.c_str(),
                    "--peer-uid", "0", "--peer-gid", "0", "--generation", "7",
                    "--attempt", "1", static_cast<char*>(nullptr));
            _exit(127);
        }
        (void)::close(ready[1]);
        struct pollfd descriptor{ready[0], POLLIN | POLLHUP, 0};
        CHECK(::poll(&descriptor, 1, 1000) > 0);
        char byte = 0;
        CHECK(::read(ready[0], &byte, 1) == 0);
        (void)::close(ready[0]);
        int status = 0;
        CHECK(::waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0);
        CHECK(::access(socket.c_str(), F_OK) != 0);
        CHECK(::rmdir(template_path) == 0);
    }

    // The explicit drop path must prove the new identity, bind as that user,
    // and still authenticate the root daemon peer through SO_PEERCRED.
    {
        char template_path[] = "/tmp/icecc-cache-service-root-drop-XXXXXX";
        const int directory_fd = ::mkstemp(template_path);
        CHECK(directory_fd >= 0);
        CHECK(::close(directory_fd) == 0);
        CHECK(::unlink(template_path) == 0);
        CHECK(::mkdir(template_path, 0700) == 0);
        CHECK(::chown(template_path, drop_uid, drop_gid) == 0);
        int ready[2] = {-1, -1};
        CHECK(::pipe(ready) == 0);
        const std::string ready_fd = std::to_string(ready[1]);
        const std::string socket = std::string(template_path) + "/service.sock";
        const std::string uid = std::to_string(static_cast<uint64_t>(drop_uid));
        const std::string gid = std::to_string(static_cast<uint64_t>(drop_gid));
        const pid_t pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            (void)::close(ready[0]);
            (void)::setenv("ICECC_CACHE_SERVICE_READY_FD", ready_fd.c_str(), 1);
            ::execl(executable.c_str(), executable.c_str(), "--socket", socket.c_str(),
                    "--peer-uid", "0", "--peer-gid", "0", "--generation", "7",
                    "--attempt", "1", "--drop-uid", uid.c_str(), "--drop-gid",
                    gid.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        (void)::close(ready[1]);
        expect_exact_ready_then_eof(ready[0]);
        (void)::close(ready[0]);
        local::Connection connection(raw_connect(socket));
        CHECK(connection.valid());
        CHECK(connection.send(local::make_hello(local::PeerRole::Daemon, {7, 1})) ==
              local::Status::Ok);
        local::Frame ack;
        CHECK(connection.receive_with_timeout(ack, 1000) == local::Status::Ok);
        CHECK(local::validate_handshake(ack, local::MessageType::HelloAck,
                                        local::PeerRole::Sidecar, {7, 1}) == local::Status::Ok);
        CHECK(::kill(pid, SIGTERM) == 0);
        int status = 0;
        CHECK(::waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(::access(socket.c_str(), F_OK) != 0);
        CHECK(::rmdir(template_path) == 0);
    }

    // Continue the ordinary multiprocess suite as an unprivileged identity,
    // matching installed operation even when the test runner began as root.
    CHECK(::setgroups(0, nullptr) == 0);
    CHECK(::setgid(drop_gid) == 0);
    CHECK(::setuid(drop_uid) == 0);
#if defined(__linux__)
    // setuid resets this flag; restore it for this test process so the leak
    // checker can inspect its threads at exit. The installed service's own
    // identity transition above remains unchanged.
    CHECK(::prctl(PR_SET_DUMPABLE, 1L) == 0);
#endif
    CHECK(::getuid() == drop_uid && ::geteuid() == drop_uid &&
          ::getgid() == drop_gid && ::getegid() == drop_gid);
}

void signal_interrupts_control_wait(int signal) {
    char template_path[] = "/tmp/icecc-cache-service-test-XXXXXX";
    const int directory_fd = ::mkstemp(template_path);
    CHECK(directory_fd >= 0);
    CHECK(::close(directory_fd) == 0);
    CHECK(::unlink(template_path) == 0);
    CHECK(::mkdir(template_path, 0700) == 0);
    Child child = launch(template_path);
    auto connection = connect_to(template_path);
    CHECK(connection.send(local::make_hello(local::PeerRole::Daemon, {7, 1})) == local::Status::Ok);
    local::Frame ack;
    CHECK(connection.receive_with_timeout(ack, 1000) == local::Status::Ok);
    CHECK(local::validate_handshake(ack, local::MessageType::HelloAck,
                                    local::PeerRole::Sidecar, {7, 1}) == local::Status::Ok);
    const auto stop_started = std::chrono::steady_clock::now();
    CHECK(::kill(child.pid, signal) == 0);
    int status = 0;
    CHECK(::waitpid(child.pid, &status, 0) == child.pid);
    CHECK(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(1));
    child.pid = -1;
    CHECK(::access((template_path + std::string("/service.sock")).c_str(), F_OK) != 0);
    CHECK(::rmdir(template_path) == 0);
}

void authenticated_idle_dispatcher_persists() {
    char template_path[] = "/tmp/icecc-cache-service-idle-XXXXXX";
    const int directory_fd = ::mkstemp(template_path);
    CHECK(directory_fd >= 0);
    CHECK(::close(directory_fd) == 0);
    CHECK(::unlink(template_path) == 0);
    CHECK(::mkdir(template_path, 0700) == 0);
    Child child = launch(template_path);
    auto connection = connect_to(template_path);
    CHECK(connection.send(local::make_hello(local::PeerRole::Daemon, {7, 1})) ==
          local::Status::Ok);
    local::Frame ack;
    CHECK(connection.receive_with_timeout(ack, 1000) == local::Status::Ok);
    CHECK(local::validate_handshake(ack, local::MessageType::HelloAck,
                                    local::PeerRole::Sidecar, {7, 1}) == local::Status::Ok);

    // This deliberately exceeds the former per-operation 500 ms timeout.
    // A persistent authenticated dispatcher must remain clean and open.
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    unsigned char byte = 0;
    const ssize_t idle = ::recv(connection.native_handle(), &byte, sizeof(byte),
                                MSG_PEEK | MSG_DONTWAIT);
    CHECK(idle < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));

    local::Frame operation{local::kProtocolVersion, local::MessageType::Data,
                           {7, 1}, local::encode_control_operation(
                                       local::make_cache_session_operation({7, 1}, 1))};
    CHECK(connection.send(operation) == local::Status::Ok);
    connection = local::Connection(-1);

    CHECK(::kill(child.pid, SIGTERM) == 0);
    int status = 0;
    CHECK(::waitpid(child.pid, &status, 0) == child.pid);
    child.pid = -1;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(::access((std::string(template_path) + "/service.sock").c_str(), F_OK) != 0);
    CHECK(::rmdir(template_path) == 0);
}

void authenticated_control_farm_accepts_twenty_and_stops() {
    char template_path[] = "/tmp/icecc-cache-service-farm-XXXXXX";
    const int directory_fd = ::mkstemp(template_path);
    CHECK(directory_fd >= 0);
    CHECK(::close(directory_fd) == 0);
    CHECK(::unlink(template_path) == 0);
    CHECK(::mkdir(template_path, 0700) == 0);

    Child child = launch(template_path);
    constexpr size_t kConnectionCount = 20;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(1000);
    std::vector<local::Connection> connections;
    connections.reserve(kConnectionCount);
    for (size_t index = 0; index != kConnectionCount; ++index) {
        local::Status status = local::Status::Ok;
        connections.push_back(local::connect_unix_until(
            std::string(template_path) + "/service.sock", deadline, &status));
        CHECK(status == local::Status::Ok && connections.back().valid());
    }

    const local::Frame hello = local::make_hello(local::PeerRole::Daemon, {7, 1});
    for (auto& connection : connections)
        CHECK(connection.send(hello) == local::Status::Ok);
    for (auto& connection : connections) {
        local::Frame acknowledgement;
        CHECK(connection.receive_until(acknowledgement, deadline) == local::Status::Ok);
        CHECK(local::validate_handshake(acknowledgement, local::MessageType::HelloAck,
                                        local::PeerRole::Sidecar, {7, 1}) == local::Status::Ok);
    }
    CHECK(std::chrono::steady_clock::now() < deadline);

    // Keep every authenticated relationship open while the service performs
    // its normal signal-driven worker shutdown.
    for (const auto& connection : connections) {
        unsigned char byte = 0;
        const ssize_t result = ::recv(connection.native_handle(), &byte, sizeof(byte),
                                      MSG_PEEK | MSG_DONTWAIT);
        CHECK(result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    }

    const auto stop_started = std::chrono::steady_clock::now();
    CHECK(::kill(child.pid, SIGTERM) == 0);
    int status = 0;
    const bool stopped = wait_for_exit_bounded(child.pid, 1000, status);
    child.pid = -1;
    CHECK(stopped);
    CHECK(std::chrono::steady_clock::now() - stop_started <
          std::chrono::milliseconds(1000));
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    for (const auto& connection : connections) {
        struct pollfd descriptor{connection.native_handle(), POLLIN | POLLHUP | POLLERR, 0};
        CHECK(::poll(&descriptor, 1, 1000) > 0);
        unsigned char byte = 0;
        CHECK(::recv(connection.native_handle(), &byte, sizeof(byte), 0) == 0);
    }
    CHECK(::access((std::string(template_path) + "/service.sock").c_str(), F_OK) != 0);
    CHECK(::rmdir(template_path) == 0);
}

void replacement_node_is_not_removed() {
    char template_path[] = "/tmp/icecc-cache-service-test-XXXXXX";
    const int directory_fd = ::mkstemp(template_path);
    CHECK(directory_fd >= 0);
    CHECK(::close(directory_fd) == 0);
    CHECK(::unlink(template_path) == 0);
    CHECK(::mkdir(template_path, 0700) == 0);
    Child child = launch(template_path);
    const std::string socket = std::string(template_path) + "/service.sock";
    CHECK(::unlink(socket.c_str()) == 0);
    local::Status listen_status = local::Status::Ok;
    const int replacement = local::listen_unix(socket, 1, &listen_status);
    CHECK(replacement >= 0 && listen_status == local::Status::Ok);
    (void)::kill(child.pid, SIGTERM);
    int status = 0;
    CHECK(::waitpid(child.pid, &status, 0) == child.pid);
    child.pid = -1;
    struct stat replacement_info{};
    CHECK(::lstat(socket.c_str(), &replacement_info) == 0 && S_ISSOCK(replacement_info.st_mode));
    CHECK(::close(replacement) == 0);
    CHECK(::unlink(socket.c_str()) == 0);
    CHECK(::rmdir(template_path) == 0);
}

void peer_credentials_are_required() {
    if (::geteuid() == 0)
        return;
    char template_path[] = "/tmp/icecc-cache-service-test-XXXXXX";
    const int directory_fd = ::mkstemp(template_path);
    CHECK(directory_fd >= 0);
    CHECK(::close(directory_fd) == 0);
    CHECK(::unlink(template_path) == 0);
    CHECK(::mkdir(template_path, 0700) == 0);
    Child child = launch(template_path, 7, 1, static_cast<uint64_t>(::getuid()) ^ 1u);
    auto connection = connect_to(template_path);
    (void)connection.send(local::make_hello(local::PeerRole::Daemon, {7, 1}));
    local::Frame response;
    CHECK(connection.receive_with_timeout(response, 100) != local::Status::Ok);
    (void)::kill(child.pid, SIGTERM);
    int status = 0;
    CHECK(::waitpid(child.pid, &status, 0) == child.pid);
    child.pid = -1;
    CHECK(::rmdir(template_path) == 0);
}

void rejects_identity_role_and_malformed() {
    char template_path[] = "/tmp/icecc-cache-service-test-XXXXXX";
    const int directory_fd = ::mkstemp(template_path);
    CHECK(directory_fd >= 0);
    CHECK(::close(directory_fd) == 0);
    CHECK(::unlink(template_path) == 0);
    CHECK(::mkdir(template_path, 0700) == 0);
    Child child = launch(template_path);
    for (const local::Frame& frame : {
             local::make_hello(local::PeerRole::Sidecar, {7, 1}),
             local::make_hello(local::PeerRole::Daemon, {8, 1}),
             local::make_hello(local::PeerRole::Daemon, {7, 2})}) {
        auto connection = connect_to(template_path);
        CHECK(connection.send(frame) == local::Status::Ok);
        local::Frame response;
        CHECK(connection.receive_with_timeout(response, 100) != local::Status::Ok);
    }
    const int malformed = raw_connect(template_path + std::string("/service.sock"));
    const std::array<uint8_t, 4> bad{'B', 'A', 'D', '!'};
    CHECK(write_all(malformed, bad));
    (void)::close(malformed);
    const int oversize = raw_connect(template_path + std::string("/service.sock"));
    std::array<uint8_t, local::kFrameHeaderSize> huge{};
    huge[0] = 'P';
    huge[1] = '5';
    huge[2] = '0';
    huge[3] = 'L';
    huge[5] = 1;
    huge[7] = 1;
    huge[8] = 0xff;
    huge[9] = 0xff;
    huge[10] = 0xff;
    huge[11] = 0xff;
    CHECK(write_all(oversize, huge));
    (void)::close(oversize);
    (void)::kill(child.pid, SIGTERM);
    int status = 0;
    CHECK(::waitpid(child.pid, &status, 0) == child.pid);
    child.pid = -1;
    CHECK(::unlink((template_path + std::string("/service.sock")).c_str()) != 0 || errno == ENOENT);
    CHECK(::rmdir(template_path) == 0);
}

void slowloris_deadline_is_total_and_listener_recovers() {
    char template_path[] = "/tmp/icecc-cache-service-test-XXXXXX";
    const int directory_fd = ::mkstemp(template_path);
    CHECK(directory_fd >= 0);
    CHECK(::close(directory_fd) == 0);
    CHECK(::unlink(template_path) == 0);
    CHECK(::mkdir(template_path, 0700) == 0);
    Child child = launch(template_path);

    local::Status encode_status = local::Status::Ok;
    const std::vector<uint8_t> encoded =
        local::encode_frame(local::make_hello(local::PeerRole::Daemon, {7, 1}),
                            &encode_status);
    CHECK(encode_status == local::Status::Ok && encoded.size() > 2);
    const int slow = raw_connect(std::string(template_path) + "/service.sock");
    // Put this connection unambiguously at the front of the accept queue.
    CHECK(send_byte_no_signal(slow, encoded.front()));
    const pid_t trickler = ::fork();
    CHECK(trickler >= 0);
    if (trickler == 0) {
        for (size_t offset = 1; offset != encoded.size(); ++offset) {
            ::usleep(80000); // each gap is below the service's 500 ms bound
            if (!send_byte_no_signal(slow, encoded[offset]))
                break;
        }
        (void)::close(slow);
        _exit(0);
    }
    (void)::close(slow);
    Child trickle_owner{trickler, -1};

    ::usleep(30000);
    auto healthy = connect_to(template_path);
    const auto started = std::chrono::steady_clock::now();
    CHECK(healthy.send(local::make_hello(local::PeerRole::Daemon, {7, 1})) ==
          local::Status::Ok);
    local::Frame ack;
    CHECK(healthy.receive_with_timeout(ack, 1200) == local::Status::Ok);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::milliseconds(1200));
    CHECK(local::validate_handshake(ack, local::MessageType::HelloAck,
                                    local::PeerRole::Sidecar, {7, 1}) == local::Status::Ok);
    int trickle_status = 0;
    CHECK(::waitpid(trickle_owner.pid, &trickle_status, 0) == trickle_owner.pid);
    trickle_owner.pid = -1;

    (void)::kill(child.pid, SIGTERM);
    int status = 0;
    CHECK(::waitpid(child.pid, &status, 0) == child.pid);
    child.pid = -1;
    CHECK(::access((std::string(template_path) + "/service.sock").c_str(), F_OK) != 0);
    CHECK(::rmdir(template_path) == 0);
}

void frame_header_and_payload_share_one_deadline() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    local::Status encode_status = local::Status::Ok;
    const std::vector<uint8_t> encoded =
        local::encode_frame(local::make_hello(local::PeerRole::Daemon, {7, 1}),
                            &encode_status);
    CHECK(encode_status == local::Status::Ok &&
          encoded.size() == local::kFrameHeaderSize + 1);
    const pid_t writer = ::fork();
    CHECK(writer >= 0);
    if (writer == 0) {
        (void)::close(sockets[0]);
        ::usleep(300000);
        (void)write_all(sockets[1],
                        std::span<const uint8_t>(encoded).first(local::kFrameHeaderSize));
        ::usleep(300000);
        (void)write_all(sockets[1],
                        std::span<const uint8_t>(encoded).subspan(local::kFrameHeaderSize));
        (void)::close(sockets[1]);
        _exit(0);
    }
    (void)::close(sockets[1]);
    Child writer_owner{writer, -1};
    local::Connection receiver(sockets[0]);
    CHECK(receiver.valid());
    local::Frame frame;
    const auto started = std::chrono::steady_clock::now();
    CHECK(receiver.receive_with_timeout(frame, 500) != local::Status::Ok);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::milliseconds(800));
    int writer_status = 0;
    CHECK(::waitpid(writer_owner.pid, &writer_status, 0) == writer_owner.pid);
    writer_owner.pid = -1;
}

local::CredentialExpectation current_credentials() {
    return local::CredentialExpectation{static_cast<uint64_t>(::getuid()),
                                        static_cast<uint64_t>(::getgid()), std::nullopt};
}

struct RuntimeCase {
    local::Connection sender;
    local::Connection receiver;
};

int loopback_listener(uint16_t& port,
                      uint32_t bind_ipv4_host_order = INADDR_LOOPBACK);

RuntimeCase authenticated_runtime_pair() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    RuntimeCase pair{local::Connection(sockets[0]), local::Connection(sockets[1])};
    const local::CredentialExpectation expected = current_credentials();
    CHECK(pair.sender.verify_peer_credentials(expected) == local::Status::Ok);
    CHECK(pair.receiver.verify_peer_credentials(expected) == local::Status::Ok);
    return pair;
}

void send_operation_cancel(local::Connection& sender, local::Identity identity,
                           uint64_t request_id,
                           local::ControlCancelTargetRole target_role =
                               local::ControlCancelTargetRole::FSession,
                           local::ControlCancellationReason reason =
                               local::ControlCancellationReason::Requested,
                           local::ControlBindingPlaceholder binding = {},
                           local::ControlOperationRole sender_role =
                               local::ControlOperationRole::Daemon) {
    const auto payload = local::encode_control_operation(
        local::make_operation_cancel_operation(identity, target_role, request_id, reason,
                                               binding, sender_role));
    CHECK(payload.size() == local::kOperationCancelOperationBytes);
    const local::Frame frame{local::kProtocolVersion, local::MessageType::Data,
                             identity, payload};
    CHECK(sender.send_until(frame, std::chrono::steady_clock::now() +
                                      std::chrono::seconds(2)) == local::Status::Ok);
}

service::RuntimeConfig test_runtime_config() {
    service::RuntimeConfig config;
    StoreIdentityRoot root{};
    root.bytes[15] = 9;
    config.c_store_guid = c_store_guid_for_root(root);
    config.f_store_guid = Id128::from_u64(9001);
    return config;
}

void test_replacement_trigger_latches_once_and_is_opt_in() {
    struct Environment {
        std::optional<std::string> prior;
        explicit Environment(const char* value) {
            if (const char* old = std::getenv("ICECC_P50_DIAGNOSTICS"))
                prior = old;
            if (value)
                CHECK(::setenv("ICECC_P50_DIAGNOSTICS", value, 1) == 0);
            else
                CHECK(::unsetenv("ICECC_P50_DIAGNOSTICS") == 0);
        }
        ~Environment() {
            if (prior)
                (void)::setenv("ICECC_P50_DIAGNOSTICS", prior->c_str(), 1);
            else
                (void)::unsetenv("ICECC_P50_DIAGNOSTICS");
        }
    };
    struct Capture {
        FILE* file = nullptr;
        int saved_stderr = -1;
        Capture() {
            file = std::tmpfile();
            CHECK(file != nullptr);
            saved_stderr = ::dup(STDERR_FILENO);
            CHECK(saved_stderr >= 0);
            CHECK(::dup2(::fileno(file), STDERR_FILENO) == STDERR_FILENO);
        }
        std::string finish() {
            CHECK(::fflush(stderr) == 0);
            CHECK(::dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
            ::close(saved_stderr);
            saved_stderr = -1;
            CHECK(std::fseek(file, 0, SEEK_SET) == 0);
            std::string value;
            char buffer[256];
            for (;;) {
                const size_t count = std::fread(buffer, 1, sizeof(buffer), file);
                value.append(buffer, count);
                if (count != sizeof(buffer))
                    break;
            }
            std::fclose(file);
            file = nullptr;
            return value;
        }
        ~Capture() {
            if (saved_stderr >= 0) {
                (void)::dup2(saved_stderr, STDERR_FILENO);
                ::close(saved_stderr);
            }
            if (file)
                std::fclose(file);
        }
    };

    {
        Environment diagnostics("1");
        Capture capture;
        service::SidecarRuntime runtime(test_runtime_config());
        runtime.latch_route_replacement_for_test(
            ReplacementTrigger::Unattributed);
        CHECK(runtime.route_replacement_latched_for_test());
        runtime.latch_route_replacement_for_test(
            ReplacementTrigger::CompletedRequestCapacity);
        const std::string output = capture.finish();
        CHECK(output ==
              "P51_REPLACEMENT_TRIGGER {\"schema_version\":1,\"reason\":\"unattributed\"}\n");
        runtime.stop();
    }
    {
        Environment diagnostics("1");
        Capture capture;
        service::SidecarRuntime runtime(test_runtime_config());
        runtime.latch_route_replacement_for_test(
            ReplacementTrigger::EndpointIdentityRetirementCapacity);
        runtime.latch_route_replacement_for_test(
            ReplacementTrigger::Unattributed);
        const std::string output = capture.finish();
        CHECK(output ==
              "P51_REPLACEMENT_TRIGGER {\"schema_version\":1,\"reason\":\"endpoint_identity_retirement_capacity\"}\n");
        runtime.stop();
    }
    const std::array<const char*, 5> disabled_values{
        nullptr, "0", "01", "true", "1 "};
    for (const char* value : disabled_values) {
        Environment diagnostics(value);
        Capture capture;
        service::SidecarRuntime runtime(test_runtime_config());
        runtime.latch_route_replacement_for_test(
            ReplacementTrigger::ExpiredUnresolvedWitness);
        CHECK(runtime.route_replacement_latched_for_test());
        CHECK(capture.finish().empty());
        runtime.stop();
    }
}

local::P51SourceReservationRequest test_p51_reservation_request(
    const CStoreGuid& c_guid, uint64_t c_store_generation,
    uint64_t c_control_generation, uint64_t c_control_attempt,
    uint64_t source_request_id, uint32_t profile, uint32_t requested_window,
    std::chrono::milliseconds lifetime = std::chrono::seconds(2)) {
    P50SourceArmFields source;
    source.wire_job_id = static_cast<uint32_t>(source_request_id + 10);
    source.assignment_epoch = 3;
    source.assignment_nonce = 4;
    source.selected_f_host = "worker.example";
    source.selected_f_ordinary_port = 10245;
    source.selected_f_cache_port = 10246;
    source.cache_protocol = 2;
    source.cache_profile = profile;
    source.logical_job = 19;
    source.compiler_attempt = 20;
    source.c_store_generation = c_store_generation;
    source.c_store_derivation_version = kStoreIdentityDerivationVersion;
    source.c_store_guid = c_guid.bytes;
    source.source_request_id = source_request_id;
    source.source_mode = profile == CACHE_PROFILE_P29V1
                             ? P50_SOURCE_MODE_P29V1
                             : profile == CACHE_PROFILE_ZSTD_ROUTE
                                   ? P50_SOURCE_MODE_ZSTD_ROUTE
                                   : P50_SOURCE_MODE_ZSTD_TU;
    source.c_control_generation = c_control_generation;
    source.c_control_attempt = c_control_attempt;

    const auto clock = sidecar::process_monotonic_clock_identity();
    local::P51SourceReservationRequest request;
    request.arm = P51SourceArmFields{source, requested_window};
    request.absolute_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + lifetime,
            clock.clock_domain_id, clock.time_namespace_id);
    CHECK(request.arm.valid());
    return request;
}

void test_source_open_arm_timeout_bounds() {
    for (const auto timeout : {
             std::chrono::milliseconds::zero(),
             std::chrono::milliseconds(60001)}) {
        service::RuntimeConfig config = test_runtime_config();
        config.source_open_arm_timeout = timeout;
        bool rejected = false;
        try {
            service::SidecarRuntime runtime(std::move(config));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }
    service::RuntimeConfig maximum = test_runtime_config();
    maximum.source_open_arm_timeout = std::chrono::seconds(60);
    service::SidecarRuntime runtime(std::move(maximum));
}

void test_p29_fault_environment_is_exact() {
    P29InternerFaultInjection parsed = P29InternerFaultInjection::FailOnce;
    CHECK(service::parse_p29_interner_fault_injection(nullptr, parsed));
    CHECK(parsed == P29InternerFaultInjection::Disabled);
    CHECK(service::parse_p29_interner_fault_injection(
        "P29_INTERNER_FAIL_ONCE", parsed));
    CHECK(parsed == P29InternerFaultInjection::FailOnce);
    CHECK(!service::parse_p29_interner_fault_injection("", parsed));
    CHECK(parsed == P29InternerFaultInjection::Disabled);
    CHECK(!service::parse_p29_interner_fault_injection(
        "P29_INTERNER_FAIL_ALWAYS", parsed));
    CHECK(parsed == P29InternerFaultInjection::Disabled);
}

SidecarLaunchIdentity test_sidecar_launch(StoreIdentityRoot root) {
    SidecarLaunchIdentity launch;
    launch.identity = {7, 1};
    launch.store_generation = 9;
    launch.store_root = root;
    launch.c_store_guid = c_store_guid_for_root(root);
    launch.f_store_guid = f_store_guid_for_root(root);
    CHECK(launch.valid());
    return launch;
}

void test_p51_reservation_capacity_identity_and_window() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x29;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_pending_p51_source_reservations = 1;
    config.max_route_relationships = 1;
    service::SidecarRuntime runtime(std::move(config));

    // The simulated remote C store must be an independent incarnation, not
    // the C/F pair represented by this F sidecar's own launch identity.
    StoreIdentityRoot remote_root{};
    remote_root.bytes[15] = 0x2b;
    const CStoreGuid remote_c_guid = c_store_guid_for_root(remote_root);
    CHECK(!icecc::p50::store_identity_file_guid_matches_client(
        remote_c_guid.bytes, launch.f_store_guid.bytes));

    auto first = test_p51_reservation_request(
        remote_c_guid, 31, launch.identity.generation,
        launch.identity.attempt, 23, CACHE_PROFILE_ZSTD_TU, 30);
    const auto first_result = runtime.reserve_p51_source_on_owner(first);
    CHECK(first_result.error_code == 0 && first_result.armed.has_value());
    CHECK(first_result.armed->selected_window == 30);
    CHECK(first_result.armed->arm == first.arm);

    // An exact duplicate is idempotent even when the one-row pending table is
    // full; it must return the original reservation, not consume another slot.
    const auto duplicate = runtime.reserve_p51_source_on_owner(first);
    CHECK(duplicate.error_code == 0 && duplicate.armed == first_result.armed);

    auto conflicting_duplicate = test_p51_reservation_request(
        remote_c_guid, 31, launch.identity.generation,
        launch.identity.attempt, 23, CACHE_PROFILE_ZSTD_TU, 29);
    const auto conflict_result =
        runtime.reserve_p51_source_on_owner(conflicting_duplicate);
    CHECK(!conflict_result.armed.has_value() &&
          conflict_result.error_code == 0x5101);

    CHECK(runtime.cancel_p51_source_on_owner(
        first.arm, first_result.armed->reservation_id,
        std::chrono::steady_clock::now() + std::chrono::seconds(1)));

    // The relationship's negotiated window is fixed by its first ARM. A later
    // smaller request cannot silently shrink/reinterpret the same live link.
    auto too_small = test_p51_reservation_request(
        remote_c_guid, 31, launch.identity.generation,
        launch.identity.attempt, 24, CACHE_PROFILE_ZSTD_TU, 1);
    const auto small_result = runtime.reserve_p51_source_on_owner(too_small);
    CHECK(!small_result.armed.has_value() && small_result.error_code == 0x5103);

    auto changed_profile = test_p51_reservation_request(
        remote_c_guid, 31, launch.identity.generation,
        launch.identity.attempt, 25, CACHE_PROFILE_ZSTD_ROUTE, 30);
    const auto profile_result =
        runtime.reserve_p51_source_on_owner(changed_profile);
    CHECK(!profile_result.armed.has_value() &&
          profile_result.error_code == 0x5103);

    auto changed_incarnation = test_p51_reservation_request(
        remote_c_guid, 32, launch.identity.generation,
        launch.identity.attempt, 26, CACHE_PROFILE_ZSTD_TU, 30);
    const auto incarnation_result =
        runtime.reserve_p51_source_on_owner(changed_incarnation);
    CHECK(!incarnation_result.armed.has_value() &&
          incarnation_result.error_code == 0x5101);

    // Expiry retires the pending reservation and its idle relationship. A
    // distinct C incarnation can then use the sole relationship slot.
    auto expiring = test_p51_reservation_request(
        remote_c_guid, 31, launch.identity.generation,
        launch.identity.attempt, 27, CACHE_PROFILE_ZSTD_TU, 30,
        std::chrono::milliseconds(100));
    const auto expiring_result = runtime.reserve_p51_source_on_owner(expiring);
    CHECK(expiring_result.error_code == 0 &&
          expiring_result.armed.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    StoreIdentityRoot other_root{};
    other_root.bytes[15] = 0x2a;
    const CStoreGuid other_c_guid = c_store_guid_for_root(other_root);
    auto successor = test_p51_reservation_request(
        other_c_guid, 1, launch.identity.generation,
        launch.identity.attempt, 28, CACHE_PROFILE_ZSTD_TU, 30);
    const auto successor_result = runtime.reserve_p51_source_on_owner(successor);
    CHECK(successor_result.error_code == 0 &&
          successor_result.armed.has_value());
    CHECK(successor_result.armed->logical_relationship_id !=
          expiring_result.armed->logical_relationship_id);
    CHECK(successor_result.armed->relationship_epoch >
          expiring_result.armed->relationship_epoch);
    std::puts("P51_RESERVATION_OWNER exact-duplicate/window/identity/expiry: ok");
}

bool wait_for_source_operation_count(service::SidecarRuntime& runtime,
                                     size_t expected,
                                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (runtime.pending_p51_source_operations_for_test() == expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return runtime.pending_p51_source_operations_for_test() == expected;
}

local::HandoffFd oversized_test_source_fd() {
    const char* temporary_directory = std::getenv("TMPDIR");
    if (temporary_directory == nullptr || temporary_directory[0] == '\0')
        temporary_directory = "/tmp";
    std::string path = std::string(temporary_directory) +
                       "/icecc-p51-async-source-XXXXXX";
    std::vector<char> mutable_path(path.begin(), path.end());
    mutable_path.push_back('\0');
    const int fd = ::mkstemp(mutable_path.data());
    if (fd < 0)
        std::fprintf(stderr, "oversized_test_source_fd: mkstemp(%s) failed: %s\n",
                     mutable_path.data(), std::strerror(errno));
    CHECK(fd >= 0);
    CHECK(::unlink(mutable_path.data()) == 0);
    const uint8_t bytes[2] = {0x51, 0x52};
    CHECK(write_all(fd, bytes));
    return local::HandoffFd(fd);
}

local::HandoffFd sized_test_source_fd(size_t size, uint8_t value) {
    const char* temporary_directory = std::getenv("TMPDIR");
    if (temporary_directory == nullptr || temporary_directory[0] == '\0')
        temporary_directory = "/tmp";
    std::string path = std::string(temporary_directory) +
                       "/icecc-p51-sized-source-XXXXXX";
    std::vector<char> mutable_path(path.begin(), path.end());
    mutable_path.push_back('\0');
    const int fd = ::mkstemp(mutable_path.data());
    if (fd < 0)
        std::fprintf(stderr, "sized_test_source_fd: mkstemp(%s) failed: %s\n",
                     mutable_path.data(), std::strerror(errno));
    CHECK(fd >= 0);
    CHECK(::unlink(mutable_path.data()) == 0);
    const std::vector<uint8_t> bytes(size, value);
    CHECK(write_all(fd, bytes));
    CHECK(::lseek(fd, 0, SEEK_SET) == 0);
    return local::HandoffFd(fd);
}

void receive_p51_transfer_error(local::Connection& peer,
                                local::Identity identity,
                                uint64_t request_id,
                                std::chrono::steady_clock::time_point deadline,
                                bool acknowledge,
                                uint16_t expected_error_code = 0) {
    local::Frame response;
    const auto receive_status = peer.receive_until(response, deadline);
    CHECK(receive_status == local::Status::Ok);
    CHECK(response.type == local::MessageType::Data);
    CHECK(local::validate_identity(response, identity) == local::Status::Ok);
    local::ControlOperation decoded;
    CHECK(local::decode_control_operation(response.payload, decoded));
    CHECK(decoded.kind == local::ControlOperationKind::P51SourceTransfer);
    CHECK(decoded.request_id == request_id);
    CHECK(decoded.p51_source_transfer_result.has_value());
    CHECK(decoded.p51_source_transfer_result->code ==
          local::SourceTransferResultCode::Error);
    if (expected_error_code != 0)
        CHECK(decoded.p51_source_transfer_result->error_code ==
              expected_error_code);
    if (acknowledge) {
        const local::Frame goodbye{local::kProtocolVersion,
                                   local::MessageType::Goodbye,
                                   identity, {}};
        CHECK(peer.send_until(goodbye, deadline) == local::Status::Ok);
    }
}

local::P50SourceTransferResult receive_p51_transfer_result(
    local::Connection& peer, local::Identity identity, uint64_t request_id,
    std::chrono::steady_clock::time_point deadline, bool acknowledge) {
    local::Frame response;
    const auto receive_status = peer.receive_until(response, deadline);
    CHECK(receive_status == local::Status::Ok);
    CHECK(response.type == local::MessageType::Data);
    CHECK(local::validate_identity(response, identity) == local::Status::Ok);
    local::ControlOperation decoded;
    CHECK(local::decode_control_operation(response.payload, decoded));
    CHECK(decoded.kind == local::ControlOperationKind::P51SourceTransfer);
    CHECK(decoded.request_id == request_id);
    CHECK(decoded.p51_source_transfer_result.has_value());
    CHECK(decoded.p51_source_transfer_result->valid());
    const auto result = *decoded.p51_source_transfer_result;
    if (acknowledge) {
        const local::Frame goodbye{local::kProtocolVersion,
                                   local::MessageType::Goodbye,
                                   identity, {}};
        CHECK(peer.send_until(goodbye, deadline) == local::Status::Ok);
    }
    return result;
}

void test_p51_async_transfer_reply_deadline_close_and_slot_reuse() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x35;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    StoreIdentityRoot f_root{};
    f_root.bytes[15] = 0x36;
    const SidecarLaunchIdentity f_launch = test_sidecar_launch(f_root);

    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.endpoint_caps.zstd.max_raw_bytes = 1;
    config.max_pending_p51_source_reservations = 8;
    config.max_active_p51_source_transfers = 1;
    config.max_pending_p51_source_operations = 1;
    std::atomic<size_t> source_read_chunks{0};
    config.p51_source_read_chunk_for_test = [&] {
        source_read_chunks.fetch_add(1, std::memory_order_relaxed);
    };
    service::SidecarRuntime runtime(std::move(config));

    service::RuntimeConfig f_config = test_runtime_config();
    f_config.c_store_guid = f_launch.c_store_guid;
    f_config.f_store_guid = f_launch.f_store_guid;
    f_config.f_store_generation = f_launch.store_generation;
    f_config.sidecar_launch = f_launch;
    service::SidecarRuntime f_runtime(std::move(f_config));

    auto make_request = [&](uint64_t request_id,
                            std::chrono::milliseconds lifetime) {
        auto reservation = test_p51_reservation_request(
            launch.c_store_guid, launch.store_generation,
            launch.identity.generation,
            launch.identity.attempt,
            request_id, CACHE_PROFILE_ZSTD_TU, 30, lifetime);
        reservation.arm.source.selected_f_host = "127.0.0.1";
        reservation.arm.source.selected_f_cache_port = 1;
        const auto armed = f_runtime.reserve_p51_source_on_owner(reservation);
        CHECK(armed.error_code == 0 && armed.armed.has_value());
        local::P51SourceTransferRequest transfer{
            *armed.armed, reservation.absolute_deadline};
        CHECK(transfer.armed.valid());
        return transfer;
    };
    auto enqueue = [&](RuntimeCase& pair,
                       const local::P51SourceTransferRequest& request,
                       size_t source_bytes = 1) {
        const auto operation = local::make_p51_source_transfer_operation(
            launch.identity, request,
            request.armed.arm.source.source_request_id);
        return runtime.enqueue_p51_source_transfer(
            std::move(pair.sender), launch.identity, operation,
            sized_test_source_fd(source_bytes, 0x77)) ==
            service::P51SourceEnqueueResult::Accepted;
    };

    // A missing Goodbye must independently release the operation credit at
    // the original (short) source deadline.
    RuntimeCase expires_without_goodbye = authenticated_runtime_pair();
    const auto expiry_request = make_request(7100,
                                             std::chrono::milliseconds(350));
    CHECK(enqueue(expires_without_goodbye, expiry_request, 2));
    receive_p51_transfer_error(
        expires_without_goodbye.receiver, launch.identity, 7100,
        expiry_request.absolute_deadline.as_steady_time_point(), false, 3);
    CHECK(wait_for_source_operation_count(runtime, 1, std::chrono::seconds(1)));
    CHECK(wait_for_source_operation_count(runtime, 0, std::chrono::seconds(2)));

    // A response stays admitted until Goodbye or the original source deadline;
    // a cap+1 request receives a typed, bounded refusal without consuming its
    // source descriptor, incrementing admission, or starting source work.
    RuntimeCase stalled = authenticated_runtime_pair();
    const auto short_request = make_request(7101, std::chrono::seconds(20));
    const int stalled_sender_fd = stalled.sender.native_handle();
    // Deliberately oversize the admitted source. The normal asynchronous path
    // emits its existing typed SourceRead error without touching the F route;
    // keep that reply held at Goodbye to occupy the only operation slot.
    CHECK(enqueue(stalled, short_request, 2));
    const auto short_deadline = short_request.absolute_deadline.as_steady_time_point();
    receive_p51_transfer_error(stalled.receiver, launch.identity, 7101,
                               short_deadline, false, 3);
    CHECK(wait_for_source_operation_count(runtime, 1, std::chrono::seconds(1)));
    CHECK(source_read_chunks.load(std::memory_order_relaxed) == 0);

    const auto refused_request = make_request(7102, std::chrono::seconds(20));
    const auto refused_operation = local::make_p51_source_transfer_operation(
        launch.identity, refused_request,
        refused_request.armed.arm.source.source_request_id);
    auto refused_source = sized_test_source_fd(1, 0x88);
    const int refused_source_fd = refused_source.get();
    const size_t reads_before_refusal =
        source_read_chunks.load(std::memory_order_relaxed);
    const uint64_t raw_bytes_before_refusal =
        runtime.active_source_raw_bytes_for_test();
    const auto refused_payload =
        local::encode_control_operation(refused_operation);
    const auto retry_fd_snapshot = process_open_fd_snapshot();
    const size_t retry_fd_baseline = process_open_fd_count();
    constexpr size_t kBusyRetryCount = 64;
    const auto retry_loop_started = std::chrono::steady_clock::now();
    uint16_t preflight_error = 0;
    const auto run_busy_retry = [&] {
        RuntimeCase busy_pair = authenticated_runtime_pair();
        const int duplicate_fd =
            ::fcntl(refused_source_fd, F_DUPFD_CLOEXEC, 0);
        CHECK(duplicate_fd >= 0);
        local::HandoffFd retry_source(duplicate_fd);
        CHECK(retry_source.cloexec());
        const int retry_source_fd = retry_source.get();
        const size_t reads_before_attempt =
            source_read_chunks.load(std::memory_order_relaxed);
        CHECK(runtime.enqueue_p51_source_transfer(
                  std::move(busy_pair.sender), launch.identity,
                  refused_operation, std::move(retry_source),
                  &preflight_error) ==
              service::P51SourceEnqueueResult::CapacityBusy);
        CHECK(preflight_error == 0);
        CHECK(busy_pair.sender.valid() && retry_source.valid() &&
              retry_source.get() == retry_source_fd);
        CHECK(refused_source.valid() && refused_source.get() == refused_source_fd);
        CHECK(runtime.pending_p51_source_operations_for_test() == 1);
        CHECK(runtime.active_source_raw_bytes_for_test() ==
              raw_bytes_before_refusal);
        CHECK(source_read_chunks.load(std::memory_order_relaxed) ==
              reads_before_attempt);
        CHECK(local::encode_control_operation(refused_operation) ==
              refused_payload);

        std::promise<local::P50SourceTransferResult> busy_result_promise;
        auto busy_result_future = busy_result_promise.get_future();
        std::exception_ptr busy_peer_exception;
        std::jthread busy_peer([&] {
            try {
                busy_result_promise.set_value(receive_p51_transfer_result(
                    busy_pair.receiver, launch.identity, 7102,
                    refused_request.absolute_deadline.as_steady_time_point(),
                    true));
            } catch (...) {
                busy_peer_exception = std::current_exception();
            }
        });
        const auto busy_reply_started = std::chrono::steady_clock::now();
        CHECK(service::send_p51_source_transfer_error_reply(
            busy_pair.sender, launch.identity, refused_operation,
            static_cast<uint16_t>(local::SourceTransferErrorCode::CapacityBusy),
            refused_request.absolute_deadline.as_steady_time_point()));
        const auto busy_reply_elapsed = std::chrono::steady_clock::now() -
                                        busy_reply_started;
        busy_peer.join();
        if (busy_peer_exception)
            std::rethrow_exception(busy_peer_exception);
        const auto busy_result = busy_result_future.get();
        CHECK(busy_result.error_code == static_cast<uint16_t>(
            local::SourceTransferErrorCode::CapacityBusy));
        CHECK(busy_result.attempts == 0 &&
              busy_result.c_store_guid == CStoreGuid{});
        CHECK(busy_reply_elapsed < std::chrono::milliseconds(250));
        CHECK(runtime.pending_p51_source_operations_for_test() == 1);
        CHECK(runtime.active_source_raw_bytes_for_test() ==
              raw_bytes_before_refusal);
        CHECK(source_read_chunks.load(std::memory_order_relaxed) ==
              reads_before_refusal);
        CHECK(refused_source.get() == refused_source_fd);
        CHECK(process_open_fd_count() == retry_fd_baseline + 3);
    };
    for (size_t retry = 0; retry < kBusyRetryCount; ++retry) {
        run_busy_retry();
        CHECK(process_open_fd_count() == retry_fd_baseline);
    }
    CHECK(std::chrono::steady_clock::now() - retry_loop_started <
          std::chrono::seconds(10));
    CHECK(runtime.pending_p51_source_operations_for_test() == 1);
    CHECK(runtime.active_source_raw_bytes_for_test() ==
          raw_bytes_before_refusal);
    CHECK(process_open_fd_count() == retry_fd_baseline);
    std::printf("P51_CAPACITY_BUSY_PLATEAU retries=%zu operations=1 raw_bytes=%llu fd_peak=%zu fd_baseline=%zu PASS\n",
                kBusyRetryCount,
                static_cast<unsigned long long>(raw_bytes_before_refusal),
                retry_fd_baseline + 3, retry_fd_baseline);

    RuntimeCase silent = authenticated_runtime_pair();
    const auto silent_operation = local::make_p51_source_transfer_operation(
        launch.identity, refused_request,
        refused_request.armed.arm.source.source_request_id);
    const auto silent_started = std::chrono::steady_clock::now();
    CHECK(!service::send_p51_source_transfer_error_reply(
        silent.sender, launch.identity, silent_operation,
        static_cast<uint16_t>(local::SourceTransferErrorCode::CapacityBusy),
        refused_request.absolute_deadline.as_steady_time_point()));
    const auto silent_elapsed = std::chrono::steady_clock::now() - silent_started;
    CHECK(silent_elapsed < std::chrono::milliseconds(250));
    silent.sender = local::Connection(-1);

    // Release exactly the held reply; retry the identical F assignment/request
    // on a fresh control connection, without reserving/re-arming a new source.
    const local::Frame held_goodbye{local::kProtocolVersion,
                                    local::MessageType::Goodbye,
                                    launch.identity, {}};
    CHECK(stalled.receiver.send_until(held_goodbye, short_deadline) ==
          local::Status::Ok);
    CHECK(wait_for_source_operation_count(runtime, 0, std::chrono::seconds(1)));

    // The exact refused source descriptor and unchanged request are admitted
    // on a fresh control lease after capacity returns. Close the synthetic
    // peer afterward to make this a slot-reuse test, not a route-success test.
    RuntimeCase normal = authenticated_runtime_pair();
    CHECK(refused_source.get() == refused_source_fd);
    CHECK(runtime.enqueue_p51_source_transfer(
        std::move(normal.sender), launch.identity, refused_operation,
        std::move(refused_source), &preflight_error) ==
        service::P51SourceEnqueueResult::Accepted);
    CHECK(preflight_error == 0);
    normal.receiver = local::Connection(-1);
    CHECK(wait_for_source_operation_count(runtime, 0, std::chrono::seconds(1)));
    CHECK(runtime.active_source_raw_bytes_for_test() == 0);
    CHECK(!refused_source.valid());
    CHECK(::fcntl(refused_source_fd, F_GETFD) == -1 && errno == EBADF);
    CHECK(::fcntl(stalled_sender_fd, F_GETFD) == -1 && errno == EBADF);
    silent.receiver = local::Connection(-1);
    const auto released_fd_snapshot = process_open_fd_snapshot();
    for (const auto& [fd, target] : released_fd_snapshot) {
        const auto before = retry_fd_snapshot.find(fd);
        CHECK(before != retry_fd_snapshot.end() && before->second == target);
    }
    const size_t released_fd_count = process_open_fd_count();
    std::printf("P51_CAPACITY_BUSY_RELEASE operations=0 raw_bytes=0 fds_before=%zu fds_after=%zu no_new_fds=1 PASS\n",
                retry_fd_baseline, released_fd_count);

    // Peer EOF is also terminal for the local reply pump and releases its
    // bounded queue reservation without waiting out the source deadline.
    RuntimeCase disconnected = authenticated_runtime_pair();
    const auto disconnected_request = make_request(7104, std::chrono::seconds(2));
    CHECK(enqueue(disconnected, disconnected_request));
    disconnected.receiver = local::Connection(-1);
    CHECK(wait_for_source_operation_count(runtime, 0, std::chrono::seconds(1)));

    RuntimeCase stopped = authenticated_runtime_pair();
    const auto stopped_request = make_request(7105, std::chrono::seconds(2));
    CHECK(enqueue(stopped, stopped_request, 2));
    receive_p51_transfer_error(
        stopped.receiver, launch.identity, 7105,
        stopped_request.absolute_deadline.as_steady_time_point(), false, 3);
    CHECK(wait_for_source_operation_count(runtime, 1, std::chrono::seconds(1)));
    runtime.stop();
    f_runtime.stop();
    CHECK(wait_for_source_operation_count(runtime, 0, std::chrono::seconds(1)));
    std::puts("P51_ASYNC_TRANSFER local-reply/deadline/peer-close/slot-reuse: ok");
}

bool wait_for_source_raw_bytes(service::SidecarRuntime& runtime,
                               uint64_t expected,
                               std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (runtime.active_source_raw_bytes_for_test() == expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return runtime.active_source_raw_bytes_for_test() == expected;
}

void test_p51_admitted_transfer_stop_releases_raw_credit() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x3b;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);

    uint16_t f_port = 0;
    const int listener_fd = loopback_listener(f_port);
    CHECK(listener_fd >= 0 && f_port != 0);

    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_aggregate_source_raw_bytes = 2;
    config.max_active_p51_source_transfers = 1;
    config.max_pending_p51_source_operations = 1;
    service::SidecarRuntime runtime(std::move(config));

    auto reservation = test_p51_reservation_request(
        launch.c_store_guid, launch.store_generation,
        launch.identity.generation, launch.identity.attempt,
        7110, CACHE_PROFILE_ZSTD_TU, 30, std::chrono::seconds(5));
    reservation.arm.source.selected_f_host = "127.0.0.1";
    reservation.arm.source.selected_f_cache_port = f_port;
    const SidecarLaunchIdentity remote_f = [] {
        StoreIdentityRoot root{};
        root.bytes[15] = 0x3d;
        return test_sidecar_launch(root);
    }();
    P51SourceArmedFields armed;
    armed.arm = reservation.arm;
    armed.f_control_generation = 17;
    armed.f_control_attempt = 18;
    armed.f_store_generation = remote_f.store_generation;
    armed.f_store_guid = remote_f.f_store_guid.bytes;
    armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
    armed.arm_observation_id = 19;
    armed.source_budget_msec = 5000;
    armed.attempt_capability_1.bytes.fill(0xa1);
    armed.attempt_capability_2.bytes.fill(0xa2);
    armed.reservation_id.fill(0xb1);
    armed.logical_relationship_id.fill(0xb2);
    armed.relationship_epoch = 1;
    armed.selected_revision = CACHE_WIRE_REVISION_R2;
    armed.selected_window = 30;
    CHECK(armed.valid());
    const local::P51SourceTransferRequest request{
        armed, reservation.absolute_deadline};

    std::promise<int> accepted_promise;
    auto accepted_future = accepted_promise.get_future();
    std::thread acceptor([listener_fd,
                          accepted_promise = std::move(accepted_promise)]() mutable {
        struct pollfd descriptor{listener_fd, POLLIN, 0};
        int accepted = -1;
        int ready;
        do {
            ready = ::poll(&descriptor, 1, 3000);
        } while (ready < 0 && errno == EINTR);
        if (ready > 0) {
            do {
                accepted = ::accept(listener_fd, nullptr, nullptr);
            } while (accepted < 0 && errno == EINTR);
        }
        accepted_promise.set_value(accepted);
    });

    RuntimeCase pair = authenticated_runtime_pair();
    const local::ControlOperation operation =
        local::make_p51_source_transfer_operation(
            launch.identity, request, request.armed.arm.source.source_request_id);
    const bool enqueued = runtime.enqueue_p51_source_transfer(
        std::move(pair.sender), launch.identity, operation,
        oversized_test_source_fd()) == service::P51SourceEnqueueResult::Accepted;
    const bool operation_seen = enqueued && wait_for_source_operation_count(
        runtime, 1, std::chrono::seconds(1));
    const bool accepted_ready = operation_seen &&
        accepted_future.wait_for(std::chrono::seconds(4)) ==
            std::future_status::ready;
    const int accepted_fd = accepted_ready ? accepted_future.get() : -1;
    const bool connected = accepted_fd >= 0;
    const bool raw_credit_held = connected &&
        wait_for_source_raw_bytes(runtime, 2, std::chrono::seconds(1));

    // The accepted source is now beyond FD read and holds aggregate raw-byte
    // credit while the peer stalls during ordinary protocol admission. Stop
    // must cancel setup and return both its operation and byte slot.
    runtime.stop();
    pair.receiver = local::Connection(-1);
    const bool operation_released = wait_for_source_operation_count(
        runtime, 0, std::chrono::seconds(2));
    const bool raw_credit_released =
        wait_for_source_raw_bytes(runtime, 0, std::chrono::seconds(1));
    if (accepted_fd >= 0)
        (void)::close(accepted_fd);
    (void)::close(listener_fd);
    acceptor.join();
    CHECK(enqueued);
    CHECK(operation_seen);
    CHECK(connected);
    CHECK(raw_credit_held);
    CHECK(operation_released);
    CHECK(raw_credit_released);
    std::puts("P51_ASYNC_TRANSFER admitted-stop/raw-credit-release: ok");
}

void test_p51_peer_close_during_active_read_cancels_before_route() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x3a;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    const SidecarLaunchIdentity remote_f = [] {
        StoreIdentityRoot root{};
        root.bytes[15] = 0x39;
        return test_sidecar_launch(root);
    }();

    uint16_t f_port = 0;
    const int listener_fd = loopback_listener(f_port);
    CHECK(listener_fd >= 0 && f_port != 0);

    std::promise<void> read_started_promise;
    auto read_started = read_started_promise.get_future();
    std::promise<void> release_read_promise;
    const std::shared_future<void> release_read =
        release_read_promise.get_future().share();
    std::promise<bool> source_read_complete_promise;
    auto source_read_complete = source_read_complete_promise.get_future();
    std::promise<void> release_source_read_promise;
    const std::shared_future<void> release_source_read =
        release_source_read_promise.get_future().share();
    std::atomic<bool> barrier_entered{false};
    std::atomic<unsigned> read_peer_closed{0};

    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_aggregate_source_raw_bytes = 2;
    config.max_active_p51_source_transfers = 1;
    config.max_pending_p51_source_operations = 1;
    config.p51_source_read_chunk_for_test = [&] {
        if (!barrier_entered.exchange(true, std::memory_order_acq_rel)) {
            read_started_promise.set_value();
            (void)release_read.wait_for(std::chrono::seconds(5));
        }
    };
    config.p51_source_read_peer_closed_for_test = [&] {
        read_peer_closed.fetch_add(1, std::memory_order_release);
    };
    config.p51_source_read_complete_for_test = [&](bool success) {
        try {
            source_read_complete_promise.set_value(success);
            (void)release_source_read.wait_for(std::chrono::seconds(5));
        } catch (...) {
        }
    };
    service::SidecarRuntime runtime(std::move(config));

    auto reservation = test_p51_reservation_request(
        launch.c_store_guid, launch.store_generation,
        launch.identity.generation, launch.identity.attempt,
        7109, CACHE_PROFILE_ZSTD_TU, 30, std::chrono::seconds(5));
    reservation.arm.source.selected_f_host = "127.0.0.1";
    reservation.arm.source.selected_f_cache_port = f_port;
    P51SourceArmedFields armed;
    armed.arm = reservation.arm;
    armed.f_control_generation = 15;
    armed.f_control_attempt = 16;
    armed.f_store_generation = remote_f.store_generation;
    armed.f_store_guid = remote_f.f_store_guid.bytes;
    armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
    armed.arm_observation_id = 17;
    armed.source_budget_msec = 5000;
    armed.attempt_capability_1.bytes.fill(0xa1);
    armed.attempt_capability_2.bytes.fill(0xa2);
    armed.reservation_id.fill(0xb1);
    armed.logical_relationship_id.fill(0xb2);
    armed.relationship_epoch = 1;
    armed.selected_revision = CACHE_WIRE_REVISION_R2;
    armed.selected_window = 30;
    CHECK(armed.valid());
    const local::P51SourceTransferRequest request{
        armed, reservation.absolute_deadline};

    RuntimeCase pair = authenticated_runtime_pair();
    const local::ControlOperation operation =
        local::make_p51_source_transfer_operation(
            launch.identity, request,
            request.armed.arm.source.source_request_id);
    auto source_fd = oversized_test_source_fd();
    const int source_fd_number = source_fd.get();
    const bool enqueued = runtime.enqueue_p51_source_transfer(
        std::move(pair.sender), launch.identity, operation,
        std::move(source_fd)) == service::P51SourceEnqueueResult::Accepted;
    const bool read_entered = enqueued &&
        read_started.wait_for(std::chrono::seconds(3)) ==
            std::future_status::ready;
    const bool credit_held = read_entered &&
        wait_for_source_raw_bytes(runtime, 2, std::chrono::seconds(1));

    // Close the control peer while the admitted preparation worker is paused
    // immediately before its first pread.  Releasing the barrier must let the
    // read-loop probe observe EOF before the service can connect to F.
    pair.receiver = local::Connection(-1);
    release_read_promise.set_value();
    const bool source_read_observed = source_read_complete.wait_for(
        std::chrono::seconds(3)) == std::future_status::ready;
    bool source_read_succeeded = false;
    if (source_read_observed)
        source_read_succeeded = source_read_complete.get();
    errno = 0;
    const bool original_source_closed = source_fd_number >= 0 &&
        ::fcntl(source_fd_number, F_GETFD) == -1 && errno == EBADF;
    int replacement_source_fd = -1;
    if (original_source_closed) {
        const int opened = ::open("/dev/null", O_RDONLY);
        if (opened == source_fd_number) {
            replacement_source_fd = opened;
        } else if (opened >= 0) {
            replacement_source_fd = ::dup2(opened, source_fd_number);
            (void)::close(opened);
        }
    }
    release_source_read_promise.set_value();
    const auto cancel_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (read_peer_closed.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < cancel_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool read_cancelled =
        read_peer_closed.load(std::memory_order_acquire) != 0;
    const bool operation_released = wait_for_source_operation_count(
        runtime, 0, std::chrono::seconds(2));
    const bool raw_credit_released = wait_for_source_raw_bytes(
        runtime, 0, std::chrono::seconds(2));
    pollfd f_listener{listener_fd, POLLIN, 0};
    int connect_observed;
    do {
        connect_observed = ::poll(&f_listener, 1, 0);
    } while (connect_observed < 0 && errno == EINTR);

    runtime.stop();
    (void)::close(listener_fd);
    CHECK(enqueued);
    CHECK(read_entered);
    CHECK(credit_held);
    CHECK(read_cancelled);
    CHECK(source_read_observed && !source_read_succeeded);
    CHECK(original_source_closed);
    CHECK(replacement_source_fd == source_fd_number);
    CHECK(operation_released);
    CHECK(raw_credit_released);
    CHECK(connect_observed == 0);
    CHECK(::fcntl(replacement_source_fd, F_GETFD) >= 0);
    CHECK(::close(replacement_source_fd) == 0);
    std::puts("P51_ASYNC_TRANSFER active-read peer-close cancels before F connect: ok");
}

void test_p51_read_failure_releases_original_source_fd() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x3c;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    StoreIdentityRoot remote_root{};
    remote_root.bytes[15] = 0x3d;
    const SidecarLaunchIdentity remote_f = test_sidecar_launch(remote_root);
    uint16_t unused_f_port = 0;
    const int f_listener = loopback_listener(unused_f_port);
    CHECK(f_listener >= 0 && unused_f_port != 0);

    auto source_fd_number = std::make_shared<std::atomic<int>>(-1);
    std::atomic<bool> truncated{false};
    std::atomic<int> truncate_result{-1};
    std::promise<bool> source_read_complete_promise;
    auto source_read_complete = source_read_complete_promise.get_future();
    std::promise<void> release_source_read_promise;
    const std::shared_future<void> release_source_read =
        release_source_read_promise.get_future().share();
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_aggregate_source_raw_bytes = 16;
    config.max_active_p51_source_transfers = 1;
    config.max_pending_p51_source_operations = 1;
    config.p51_source_read_chunk_for_test = [&] {
        if (!truncated.exchange(true, std::memory_order_acq_rel)) {
            const int fd = source_fd_number->load(std::memory_order_acquire);
            if (fd >= 0)
                truncate_result.store(::ftruncate(fd, 0),
                                      std::memory_order_release);
        }
    };
    config.p51_source_read_complete_for_test = [&](bool success) {
        try {
            source_read_complete_promise.set_value(success);
            (void)release_source_read.wait_for(std::chrono::seconds(5));
        } catch (...) {
        }
    };
    service::SidecarRuntime runtime(std::move(config));

    auto reservation = test_p51_reservation_request(
        launch.c_store_guid, launch.store_generation,
        launch.identity.generation, launch.identity.attempt,
        7110, CACHE_PROFILE_ZSTD_TU, 30, std::chrono::seconds(8));
    reservation.arm.source.selected_f_host = "127.0.0.1";
    reservation.arm.source.selected_f_cache_port = unused_f_port;
    P51SourceArmedFields armed;
    armed.arm = reservation.arm;
    armed.f_control_generation = remote_f.identity.generation;
    armed.f_control_attempt = remote_f.identity.attempt;
    armed.f_store_generation = remote_f.store_generation;
    armed.f_store_guid = remote_f.f_store_guid.bytes;
    armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
    armed.arm_observation_id = 7110;
    armed.source_budget_msec = 8000;
    armed.attempt_capability_1.bytes.fill(0x81);
    armed.attempt_capability_2.bytes.fill(0x82);
    armed.reservation_id.fill(0x83);
    armed.logical_relationship_id.fill(0x84);
    armed.relationship_epoch = 1;
    armed.selected_revision = CACHE_WIRE_REVISION_R2;
    armed.selected_window = 30;
    CHECK(armed.valid());
    const local::P51SourceTransferRequest request{
        armed, reservation.absolute_deadline};

    RuntimeCase pair = authenticated_runtime_pair();
    auto source = sized_test_source_fd(12, 0x91);
    const int original_fd = source.get();
    source_fd_number->store(original_fd, std::memory_order_release);
    const auto operation = local::make_p51_source_transfer_operation(
        launch.identity, request, request.armed.arm.source.source_request_id);
    const bool enqueued = runtime.enqueue_p51_source_transfer(
        std::move(pair.sender), launch.identity, operation, std::move(source)) ==
        service::P51SourceEnqueueResult::Accepted;
    const bool completion_observed = enqueued &&
        source_read_complete.wait_for(std::chrono::seconds(3)) ==
            std::future_status::ready;
    bool read_succeeded = false;
    if (completion_observed)
        read_succeeded = source_read_complete.get();
    errno = 0;
    const bool source_closed = ::fcntl(original_fd, F_GETFD) == -1 &&
                               errno == EBADF;
    int replacement_fd = -1;
    if (source_closed) {
        const int opened = ::open("/dev/null", O_RDONLY);
        if (opened == original_fd) {
            replacement_fd = opened;
        } else if (opened >= 0) {
            replacement_fd = ::dup2(opened, original_fd);
            (void)::close(opened);
        }
    }
    release_source_read_promise.set_value();

    std::exception_ptr receive_error;
    bool reported_read_failure = false;
    try {
        if (enqueued)
            receive_p51_transfer_error(
                pair.receiver, launch.identity, 7110,
                request.absolute_deadline.as_steady_time_point(), true, 3);
        reported_read_failure = enqueued;
    } catch (...) {
        receive_error = std::current_exception();
    }
    const bool raw_credit_released = wait_for_source_raw_bytes(
        runtime, 0, std::chrono::seconds(2));
    runtime.stop();
    pair.receiver = local::Connection(-1);
    pollfd listener_ready{f_listener, POLLIN, 0};
    int unexpected_connect;
    do {
        unexpected_connect = ::poll(&listener_ready, 1, 0);
    } while (unexpected_connect < 0 && errno == EINTR);
    (void)::close(f_listener);

    if (receive_error)
        std::rethrow_exception(receive_error);
    CHECK(enqueued);
    CHECK(completion_observed && !read_succeeded);
    CHECK(truncated.load(std::memory_order_acquire));
    CHECK(truncate_result.load(std::memory_order_acquire) == 0);
    CHECK(source_closed && replacement_fd == original_fd);
    CHECK(reported_read_failure);
    CHECK(raw_credit_released);
    CHECK(unexpected_connect == 0);
    CHECK(::fcntl(replacement_fd, F_GETFD) >= 0);
    CHECK(::close(replacement_fd) == 0);
    std::puts("P51_ASYNC_TRANSFER read failure releases original source fd: ok");
}

void test_p51_d07_queued_cancel_position(size_t cancelled_index) {
    constexpr size_t kSurvivors = 30;
    constexpr size_t kCohort = kSurvivors + 1;
    constexpr uint64_t kHeldRawBytes = 4096;
    CHECK(cancelled_index < kCohort);

    StoreIdentityRoot c_root{};
    c_root.bytes[15] = static_cast<uint8_t>(0x61 + cancelled_index);
    const SidecarLaunchIdentity c_launch = test_sidecar_launch(c_root);
    StoreIdentityRoot f_root{};
    f_root.bytes[15] = static_cast<uint8_t>(0x71 + cancelled_index);
    const SidecarLaunchIdentity f_launch = test_sidecar_launch(f_root);

    uint16_t f_port = 0;
    const int listener = loopback_listener(f_port);
    CHECK(listener >= 0 && f_port != 0);

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool holder_read_entered = false;
    bool release_holder_read = false;
    std::atomic<unsigned> holder_peer_closed{0};
    std::array<std::atomic<unsigned>, kCohort + 1> queued_size_observations{};
    for (auto& count : queued_size_observations)
        count.store(0, std::memory_order_relaxed);
    std::mutex retired_mutex;
    std::vector<std::pair<Id128, bool>> retired_rows;

    service::RuntimeConfig f_config = test_runtime_config();
    f_config.c_store_guid = f_launch.c_store_guid;
    f_config.f_store_guid = f_launch.f_store_guid;
    f_config.f_store_generation = f_launch.store_generation;
    f_config.sidecar_launch = f_launch;
    f_config.endpoint_caps.profile = ProfileId::ZSTD_TU;
    f_config.endpoint_caps.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    f_config.endpoint_caps.zstd.max_raw_bytes = 8192;
    f_config.max_pending_p51_source_reservations = 64;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    f_config.p51_reservation_retired_for_test =
        [&](Id128 id, bool marker_retired) {
            std::lock_guard lock(retired_mutex);
            retired_rows.emplace_back(id, marker_retired);
        };
#endif
    service::SidecarRuntime f_runtime(std::move(f_config));

    service::RuntimeConfig c_config = test_runtime_config();
    c_config.c_store_guid = c_launch.c_store_guid;
    c_config.f_store_guid = c_launch.f_store_guid;
    c_config.f_store_generation = c_launch.store_generation;
    c_config.sidecar_launch = c_launch;
    c_config.endpoint_caps.profile = ProfileId::ZSTD_TU;
    c_config.endpoint_caps.supported_profiles = profile_bit(ProfileId::ZSTD_TU);
    c_config.endpoint_caps.zstd.max_raw_bytes = 8192;
    c_config.max_active_source_transfers = 4;
    c_config.max_active_p51_source_transfers = kCohort + 1;
    c_config.max_pending_p51_source_operations = kCohort + 1;
    c_config.max_aggregate_source_raw_bytes = kHeldRawBytes;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    c_config.p51_source_read_chunk_for_test = [&] {
        std::unique_lock lock(gate_mutex);
        if (holder_read_entered)
            return;
        holder_read_entered = true;
        gate_changed.notify_all();
        gate_changed.wait(lock, [&] { return release_holder_read; });
    };
    c_config.p51_source_read_peer_closed_for_test = [&] {
        holder_peer_closed.fetch_add(1, std::memory_order_release);
    };
    c_config.p51_source_credit_waiting_for_test = [&](uint64_t bytes) {
        if (bytes <= kCohort)
            queued_size_observations[static_cast<size_t>(bytes)].fetch_add(
                1, std::memory_order_release);
    };
#endif
    service::SidecarRuntime c_runtime(std::move(c_config));

    std::atomic<bool> stop_accepting{false};
    std::atomic<size_t> accepted_connections{0};
    std::thread acceptor([&] {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::seconds(20);
        while (!stop_accepting.load(std::memory_order_acquire) &&
               accepted_connections.load(std::memory_order_acquire) <
                   kSurvivors &&
               std::chrono::steady_clock::now() < end) {
            pollfd ready{listener, POLLIN, 0};
            int polled;
            do {
                polled = ::poll(&ready, 1, 100);
            } while (polled < 0 && errno == EINTR);
            if (polled <= 0 || !(ready.revents & POLLIN))
                continue;
            int fd;
            sockaddr_storage peer{};
            socklen_t peer_size = sizeof(peer);
            do {
                fd = ::accept(listener,
                    reinterpret_cast<sockaddr*>(&peer), &peer_size);
            } while (fd < 0 && errno == EINTR);
            if (fd < 0)
                continue;

            // The C runtime's production connector first negotiates the
            // ordinary public protocol, then exchanges CACHE_LINK_SESSION
            // before it hands the same socket to the R2 endpoint. Mirror that
            // boundary here; a raw accepted socket is not yet an R2 stream.
            std::unique_ptr<MsgChannel> channel(
                Service::createChannelAccepted(
                    fd, reinterpret_cast<sockaddr*>(&peer), peer_size));
            if (!channel)
                continue;
            const auto handshake_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            bool protocol_ready = true;
            while (channel->protocol_admission_state() ==
                       MsgChannel::ProtocolAdmissionState::Pending &&
                   std::chrono::steady_clock::now() < handshake_deadline) {
                short events = POLLIN;
                if (channel->has_pending_write())
                    events |= POLLOUT;
                pollfd socket{channel->fd, events, 0};
                int polled;
                do {
                    polled = ::poll(&socket, 1, 50);
                } while (polled < 0 && errno == EINTR);
                if (polled < 0 ||
                    (socket.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    protocol_ready = false;
                    break;
                }
                if ((socket.revents & POLLOUT) && !channel->flush_pending()) {
                    protocol_ready = false;
                    break;
                }
                if ((socket.revents & POLLIN) && !channel->read_a_bit()) {
                    protocol_ready = false;
                    break;
                }
            }
            if (!protocol_ready ||
                channel->protocol_admission_state() !=
                    MsgChannel::ProtocolAdmissionState::Ready ||
                !channel->finish_protocol_admission())
                continue;

            std::unique_ptr<Msg> link_request(
                channel->get_msg_until(handshake_deadline));
            if (dynamic_cast<P51CacheLinkSessionMsg*>(link_request.get()) ==
                nullptr)
                continue;
            const int adopted_fd =
                channel->send_p51_cache_link_session_ready_and_release(
                    handshake_deadline);
            if (adopted_fd < 0)
                continue;
            accepted_connections.fetch_add(1, std::memory_order_release);
            f_runtime.start_adopted_r2_endpoint(adopted_fd);
        }
    });
    auto cleanup = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1), [&](int*) {
            stop_accepting.store(true, std::memory_order_release);
            (void)::shutdown(listener, SHUT_RDWR);
            if (acceptor.joinable())
                acceptor.join();
            (void)::close(listener);
            {
                std::lock_guard lock(gate_mutex);
                release_holder_read = true;
            }
            gate_changed.notify_all();
            c_runtime.stop();
            f_runtime.stop();
        });

    struct RequestRow {
        local::P51SourceTransferRequest request;
        local::P51SourceReservationRequest reservation;
        RuntimeCase pair{local::Connection(-1), local::Connection(-1)};
        std::vector<uint8_t> bytes;
        uint64_t request_id = 0;
    };
    auto reserve_request = [&](uint64_t request_id, size_t raw_bytes,
                               uint8_t fill) {
        auto reservation = test_p51_reservation_request(
            c_launch.c_store_guid, c_launch.store_generation,
            c_launch.identity.generation, c_launch.identity.attempt,
            request_id, CACHE_PROFILE_ZSTD_TU, 30,
            std::chrono::seconds(30));
        reservation.arm.source.assignment_nonce = request_id;
        reservation.arm.source.logical_job = 100000 + request_id;
        reservation.arm.source.compiler_attempt = 1;
        reservation.arm.source.selected_f_host = "127.0.0.1";
        reservation.arm.source.selected_f_cache_port = f_port;
        const auto reserved = f_runtime.reserve_p51_source_on_owner(reservation);
        CHECK(reserved.error_code == 0 && reserved.armed.has_value());
        RequestRow row;
        row.request = local::P51SourceTransferRequest{
            *reserved.armed, reservation.absolute_deadline};
        row.reservation = reservation;
        row.pair = authenticated_runtime_pair();
        row.bytes.assign(raw_bytes, fill);
        row.request_id = request_id;
        return row;
    };
    auto enqueue = [&](RequestRow& row) {
        const auto operation = local::make_p51_source_transfer_operation(
            c_launch.identity, row.request, row.request_id);
        return c_runtime.enqueue_p51_source_transfer(
            std::move(row.pair.sender), c_launch.identity, operation,
            sized_test_source_fd(row.bytes.size(), row.bytes.front())) ==
            service::P51SourceEnqueueResult::Accepted;
    };

    const uint64_t base = 8200 + static_cast<uint64_t>(cancelled_index) * 100;
    RequestRow holder = reserve_request(base, kHeldRawBytes, 0xe1);
    std::vector<RequestRow> cohort;
    cohort.reserve(kCohort);
    for (size_t index = 0; index < kCohort; ++index) {
        const size_t size = index + 1;
        cohort.push_back(reserve_request(
            base + 1 + index, size, static_cast<uint8_t>(0x20 + index)));
    }

    const bool holder_enqueued = enqueue(holder);
    bool holder_gate_reached = false;
    {
        std::unique_lock lock(gate_mutex);
        holder_gate_reached = gate_changed.wait_for(
            lock, std::chrono::seconds(3), [&] { return holder_read_entered; });
    }
    const bool holder_credit_held = holder_gate_reached &&
        wait_for_source_raw_bytes(c_runtime, kHeldRawBytes,
                                  std::chrono::seconds(1));

    bool all_cohort_enqueued = holder_credit_held;
    for (RequestRow& row : cohort)
        all_cohort_enqueued = enqueue(row) && all_cohort_enqueued;
    const bool all_operations_admitted = all_cohort_enqueued &&
        wait_for_source_operation_count(
            c_runtime, kCohort + 1, std::chrono::seconds(3));

    const auto queue_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < queue_deadline) {
        bool all_observed = true;
        for (size_t index = 0; index < kCohort; ++index) {
            all_observed = all_observed &&
                queued_size_observations[index + 1].load(
                    std::memory_order_acquire) == 1;
        }
        if (all_observed)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    bool all_exact_requests_waiting = true;
    for (size_t index = 0; index < kCohort; ++index) {
        all_exact_requests_waiting = all_exact_requests_waiting &&
            queued_size_observations[index + 1].load(
                std::memory_order_acquire) == 1;
    }
    const bool selected_request_waiting =
        queued_size_observations[cancelled_index + 1].load(
            std::memory_order_acquire) == 1;

    RequestRow& cancelled = cohort[cancelled_index];
    const bool f_cancelled = selected_request_waiting &&
        f_runtime.cancel_p51_source_on_owner(
            cancelled.request.armed.arm,
            cancelled.request.armed.reservation_id,
            cancelled.request.absolute_deadline.as_steady_time_point());
    const bool duplicate_f_cancelled = f_cancelled &&
        f_runtime.cancel_p51_source_on_owner(
            cancelled.request.armed.arm,
            cancelled.request.armed.reservation_id,
            cancelled.request.absolute_deadline.as_steady_time_point());
    cancelled.pair.receiver = local::Connection(-1);
    const bool selected_c_operation_released = f_cancelled &&
        wait_for_source_operation_count(
            c_runtime, kCohort, std::chrono::seconds(3));

    const bool holder_f_cancelled =
        f_runtime.cancel_p51_source_on_owner(
            holder.request.armed.arm, holder.request.armed.reservation_id,
            holder.request.absolute_deadline.as_steady_time_point());
    holder.pair.receiver = local::Connection(-1);
    {
        std::lock_guard lock(gate_mutex);
        release_holder_read = true;
    }
    gate_changed.notify_all();

    const auto peer_closed_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(3);
    while (holder_peer_closed.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < peer_closed_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool holder_observed_peer_close =
        holder_peer_closed.load(std::memory_order_acquire) == 1;

    std::vector<bool> settled(kCohort, false);
    std::vector<uint64_t> survivor_tu_seqs;
    survivor_tu_seqs.reserve(kSurvivors);
    bool all_survivors_committed_and_attached = true;
    for (size_t index = 0; index < kCohort; ++index) {
        if (index == cancelled_index)
            continue;
        RequestRow& row = cohort[index];
        const auto deadline = row.request.absolute_deadline.as_steady_time_point();
        const auto result = receive_p51_transfer_result(
            row.pair.receiver, c_launch.identity, row.request_id, deadline, true);
        bool attached = result.code == local::SourceTransferResultCode::Committed &&
            result.valid() &&
            result.c_store_guid == c_launch.c_store_guid &&
            result.raw_bytes == row.bytes.size() &&
            result.raw_digest == icecc::digest128(row.bytes);
        if (attached) {
            const InputLeaseOwner owner{
                row.request.armed.arm.source.logical_job,
                row.request.armed.arm.source.assignment_epoch,
                row.request.armed.arm.source.assignment_nonce};
            const InputFdRequest attach_request{
                f_launch.identity,
                InputRecordKey{result.c_store_guid, TuSeq{result.tu_seq}}, owner,
                row.request_id + 50000};
            auto cursor = f_runtime.attach_input_on_owner(
                attach_request, deadline);
            attached = cursor.has_value() &&
                cursor->remaining() == row.bytes.size() &&
                cursor->raw_digest() == icecc::digest128(row.bytes);
            if (attached) {
                std::vector<uint8_t> actual(row.bytes.size());
                attached = cursor->read(actual) == actual.size() &&
                           actual == row.bytes;
            }
            f_runtime.finish_input_attachment_on_owner(
                attach_request, attached, deadline);
        }
        settled[index] = attached;
        all_survivors_committed_and_attached =
            all_survivors_committed_and_attached && attached;
        survivor_tu_seqs.push_back(result.tu_seq);
    }

    std::sort(survivor_tu_seqs.begin(), survivor_tu_seqs.end());
    bool contiguous_tu_seqs = survivor_tu_seqs.size() == kSurvivors;
    for (size_t seq = 0; seq < survivor_tu_seqs.size(); ++seq)
        contiguous_tu_seqs = contiguous_tu_seqs &&
                             survivor_tu_seqs[seq] == seq;
    const bool one_persistent_connection =
        accepted_connections.load(std::memory_order_acquire) == 1;
    const bool all_operations_released = wait_for_source_operation_count(
        c_runtime, 0, std::chrono::seconds(3));
    const bool all_raw_credits_released =
        wait_for_source_raw_bytes(c_runtime, 0, std::chrono::seconds(3));
    bool exact_target_retired_once = false;
    const Id128 cancelled_reservation_id{
        cancelled.request.armed.reservation_id};
    {
        std::lock_guard lock(retired_mutex);
        const auto target = std::find_if(
            retired_rows.begin(), retired_rows.end(), [&](const auto& retired) {
                return retired.first == cancelled_reservation_id;
            });
        exact_target_retired_once = target != retired_rows.end() &&
            !target->second &&
            std::count_if(retired_rows.begin(), retired_rows.end(),
                          [&](const auto& retired) {
                              return retired.first == cancelled_reservation_id;
                          }) == 1;
    }

    CHECK(holder_enqueued);
    CHECK(holder_credit_held);
    CHECK(all_cohort_enqueued);
    CHECK(all_operations_admitted);
    CHECK(all_exact_requests_waiting);
    CHECK(selected_request_waiting);
    CHECK(f_cancelled);
    CHECK(!duplicate_f_cancelled);
    CHECK(selected_c_operation_released);
    CHECK(holder_f_cancelled);
    CHECK(holder_observed_peer_close);
    CHECK(static_cast<size_t>(
              std::count(settled.begin(), settled.end(), true)) == kSurvivors);
    CHECK(all_survivors_committed_and_attached);
    CHECK(contiguous_tu_seqs);
    CHECK(one_persistent_connection);
    CHECK(all_operations_released);
    CHECK(all_raw_credits_released);
    CHECK(exact_target_retired_once);
    std::printf("P51_D07 queued cancel submission=%zu survivors=30 exact-inputs=30 "
                "fresh-tu-seq=0..29 C-raw-credit=0 scope=no-compiler\n",
                cancelled_index);
}

void test_p51_d07_queued_cancel_first_middle_last() {
    test_p51_d07_queued_cancel_position(0);
    test_p51_d07_queued_cancel_position(15);
    test_p51_d07_queued_cancel_position(30);
}

void test_p51_d07_staged_cancel_case(ProfileId profile,
                                    size_t cancelled_index) {
    constexpr size_t kCohort = 31;
    constexpr size_t kSurvivors = 30;
    CHECK(cancelled_index < kCohort);
    uint32_t cache_profile = 0;
    switch (profile) {
    case ProfileId::P29V1: cache_profile = CACHE_PROFILE_P29V1; break;
    case ProfileId::ZSTD_TU: cache_profile = CACHE_PROFILE_ZSTD_TU; break;
    case ProfileId::ZSTD_ROUTE: cache_profile = CACHE_PROFILE_ZSTD_ROUTE; break;
    }

    StoreIdentityRoot c_root{};
    c_root.bytes[14] = static_cast<uint8_t>(profile);
    c_root.bytes[15] = static_cast<uint8_t>(0x31 + cancelled_index);
    const SidecarLaunchIdentity c_launch = test_sidecar_launch(c_root);
    StoreIdentityRoot f_root{};
    f_root.bytes[14] = static_cast<uint8_t>(profile);
    f_root.bytes[15] = static_cast<uint8_t>(0x51 + cancelled_index);
    const SidecarLaunchIdentity f_launch = test_sidecar_launch(f_root);

    uint16_t f_port = 0;
    const int listener = loopback_listener(f_port);
    CHECK(listener >= 0 && f_port != 0);

    std::mutex event_mutex;
    std::condition_variable event_changed;
    bool target_staged = false;
    bool release_target = false;
    JobBind target_binding{};
    std::vector<std::pair<uint64_t, uint64_t>> prepared_ordinals;
    std::vector<uint64_t> sent_ordinals;
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> materialized;
    std::vector<std::pair<Id128, bool>> retired;
    bool cancellation_observed = false;
    std::atomic<bool> cancelled_hook_exact{false};
    const bool hold_predecessor_receipt =
        profile == ProfileId::P29V1 && cancelled_index == 15;
    bool predecessor_worker_entered = false;
    bool release_predecessor_worker = !hold_predecessor_receipt;
    std::vector<uint64_t> f_commit_ordinals;
    std::atomic<bool> stop_accepting{false};
    std::atomic<size_t> accepted_connections{0};

    service::RuntimeConfig f_config = test_runtime_config();
    f_config.c_store_guid = f_launch.c_store_guid;
    f_config.f_store_guid = f_launch.f_store_guid;
    f_config.f_store_generation = f_launch.store_generation;
    f_config.sidecar_launch = f_launch;
    f_config.endpoint_caps.profile = profile;
    f_config.endpoint_caps.supported_profiles = profile_bit(profile);
    f_config.endpoint_caps.zstd.max_raw_bytes = 8192;
    f_config.max_pending_p51_source_reservations = 48;
    f_config.endpoint_config.input_job_state =
        [&](CStoreGuid, const TxBegin& begin, const TxCommit&,
            std::span<const uint8_t> bytes) {
            std::lock_guard lock(event_mutex);
            materialized.emplace_back(begin.tu_seq.value,
                                      std::vector<uint8_t>(bytes.begin(),
                                                           bytes.end()));
            event_changed.notify_all();
            return InputJobState::Open;
        };
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    f_config.p51_reservation_retired_for_test =
        [&](Id128 id, bool marker_retired) {
            std::lock_guard lock(event_mutex);
            retired.emplace_back(id, marker_retired);
            event_changed.notify_all();
        };
#endif
    service::SidecarRuntime f_runtime(std::move(f_config));

    service::RuntimeConfig c_config = test_runtime_config();
    c_config.c_store_guid = c_launch.c_store_guid;
    c_config.f_store_guid = c_launch.f_store_guid;
    c_config.f_store_generation = c_launch.store_generation;
    c_config.sidecar_launch = c_launch;
    c_config.endpoint_caps.profile = profile;
    c_config.endpoint_caps.supported_profiles = profile_bit(profile);
    c_config.endpoint_caps.zstd.max_raw_bytes = 8192;
    c_config.max_active_source_transfers = 8;
    c_config.max_active_p51_source_transfers = 40;
    c_config.max_pending_p51_source_operations = 40;
    c_config.max_aggregate_source_raw_bytes = 1024 * 1024;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    const uint64_t target_request_id = 120000 +
        static_cast<uint64_t>(profile) * 100 + cancelled_index;
    c_config.before_r2_first_bundle_write_for_test =
        [&, target_request_id](PrepareRequestKey,
                               const JobBind& binding)
            -> boost::asio::awaitable<void> {
            {
                std::lock_guard lock(event_mutex);
                prepared_ordinals.emplace_back(binding.source_request_id,
                                                binding.relationship_ordinal);
                if (binding.source_request_id == target_request_id) {
                    target_binding = binding;
                    target_staged = true;
                    event_changed.notify_all();
                }
            }
            if (binding.source_request_id != target_request_id)
                co_return;
            const auto executor = co_await
                boost::asio::this_coro::executor;
            boost::asio::steady_timer pause(executor);
            for (;;) {
                {
                    std::lock_guard lock(event_mutex);
                    if (release_target)
                        break;
                }
                pause.expires_after(std::chrono::milliseconds(2));
                co_await pause.async_wait(boost::asio::use_awaitable);
            }
        };
    c_config.r2_prewrite_cancelled_for_test =
        [&](PrepareRequestKey, const JobBind& binding) {
            cancelled_hook_exact.store(
                binding.source_request_id == target_request_id &&
                    binding.relationship_ordinal ==
                        target_binding.relationship_ordinal,
                std::memory_order_release);
        };
    c_config.p51_source_cancel_observed_for_test =
        [&](uint64_t request_id) {
            std::lock_guard lock(event_mutex);
            if (request_id == target_request_id) {
                cancellation_observed = true;
                event_changed.notify_all();
            }
        };
    c_config.after_r2_bundle_sent_for_test = [&](uint64_t ordinal) {
        std::lock_guard lock(event_mutex);
        sent_ordinals.push_back(ordinal);
        event_changed.notify_all();
    };
#endif
    service::SidecarRuntime c_runtime(std::move(c_config));

    std::thread acceptor([&] {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::seconds(40);
        while (!stop_accepting.load(std::memory_order_acquire) &&
               accepted_connections.load(std::memory_order_acquire) == 0 &&
               std::chrono::steady_clock::now() < end) {
            pollfd ready{listener, POLLIN, 0};
            int polled;
            do { polled = ::poll(&ready, 1, 100); }
            while (polled < 0 && errno == EINTR);
            if (polled <= 0 || !(ready.revents & POLLIN))
                continue;
            sockaddr_storage peer{};
            socklen_t peer_size = sizeof(peer);
            int fd;
            do {
                fd = ::accept(listener,
                    reinterpret_cast<sockaddr*>(&peer), &peer_size);
            } while (fd < 0 && errno == EINTR);
            if (fd < 0)
                continue;
            std::unique_ptr<MsgChannel> channel(Service::createChannelAccepted(
                fd, reinterpret_cast<sockaddr*>(&peer), peer_size));
            if (!channel)
                continue;
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(4);
            bool protocol_ready = true;
            while (channel->protocol_admission_state() ==
                       MsgChannel::ProtocolAdmissionState::Pending &&
                   std::chrono::steady_clock::now() < deadline) {
                short events = POLLIN;
                if (channel->has_pending_write())
                    events = static_cast<short>(events | POLLOUT);
                pollfd socket{channel->fd, events, 0};
                int result;
                do { result = ::poll(&socket, 1, 50); }
                while (result < 0 && errno == EINTR);
                if (result < 0 ||
                    (socket.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    protocol_ready = false;
                    break;
                }
                if ((socket.revents & POLLOUT) && !channel->flush_pending()) {
                    protocol_ready = false;
                    break;
                }
                if ((socket.revents & POLLIN) && !channel->read_a_bit()) {
                    protocol_ready = false;
                    break;
                }
            }
            if (!protocol_ready ||
                channel->protocol_admission_state() !=
                    MsgChannel::ProtocolAdmissionState::Ready ||
                !channel->finish_protocol_admission())
                continue;
            std::unique_ptr<Msg> request(channel->get_msg_until(deadline));
            if (!dynamic_cast<P51CacheLinkSessionMsg*>(request.get()))
                continue;
            const int adopted =
                channel->send_p51_cache_link_session_ready_and_release(
                    deadline);
            if (adopted < 0)
                continue;
            accepted_connections.store(1, std::memory_order_release);
            EndpointIoControl control;
            control.before_materialize_on_worker = [&] {
                if (!hold_predecessor_receipt)
                    return;
                std::unique_lock lock(event_mutex);
                if (predecessor_worker_entered)
                    return;
                predecessor_worker_entered = true;
                event_changed.notify_all();
                event_changed.wait(lock, [&] {
                    return release_predecessor_worker;
                });
            };
            control.outbound_message_observer =
                [&](ActorSide actor, const Message& message) {
                    if (actor != ActorSide::F)
                        return;
                    const auto* commit = std::get_if<R2TxCommit>(&message);
                    if (!commit)
                        return;
                    std::lock_guard lock(event_mutex);
                    f_commit_ordinals.push_back(
                        commit->relationship_ordinal);
                    event_changed.notify_all();
                };
            f_runtime.start_adopted_r2_endpoint(adopted, std::move(control));
        }
    });
    auto cleanup = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1), [&](int*) {
            {
                std::lock_guard lock(event_mutex);
                release_target = true;
                release_predecessor_worker = true;
            }
            event_changed.notify_all();
            stop_accepting.store(true, std::memory_order_release);
            (void)::shutdown(listener, SHUT_RDWR);
            if (acceptor.joinable())
                acceptor.join();
            (void)::close(listener);
            c_runtime.stop();
            f_runtime.stop();
        });

    struct RequestRow {
        local::P51SourceTransferRequest transfer;
        RuntimeCase pair{local::Connection(-1), local::Connection(-1)};
        uint64_t id = 0;
        std::vector<uint8_t> bytes;
    };
    std::vector<RequestRow> rows;
    rows.reserve(kCohort);
    const uint64_t first_id = 120000 + static_cast<uint64_t>(profile) * 100;
    for (size_t index = 0; index < kCohort; ++index) {
        const uint64_t id = first_id + index;
        auto reservation = test_p51_reservation_request(
            c_launch.c_store_guid, c_launch.store_generation,
            c_launch.identity.generation, c_launch.identity.attempt,
            id, cache_profile, 30, std::chrono::seconds(30));
        reservation.arm.source.assignment_nonce = id;
        reservation.arm.source.logical_job = 160000 + id;
        reservation.arm.source.compiler_attempt = 1;
        reservation.arm.source.selected_f_host = "127.0.0.1";
        reservation.arm.source.selected_f_cache_port = f_port;
        const auto armed = f_runtime.reserve_p51_source_on_owner(reservation);
        CHECK(armed.error_code == 0 && armed.armed.has_value());
        std::vector<uint8_t> bytes(128 + index,
                                   static_cast<uint8_t>(index + 1));
        rows.push_back(RequestRow{
            local::P51SourceTransferRequest{
                *armed.armed, reservation.absolute_deadline},
            authenticated_runtime_pair(), id, std::move(bytes)});
    }

    bool all_submitted = true;
    auto enqueue_row = [&](RequestRow& row) {
        const auto operation = local::make_p51_source_transfer_operation(
            c_launch.identity, row.transfer, row.id);
        return c_runtime.enqueue_p51_source_transfer(
            std::move(row.pair.sender), c_launch.identity, operation,
            sized_test_source_fd(row.bytes.size(), row.bytes.front()));
    };
    size_t first_concurrent_index = 0;
    if (hold_predecessor_receipt) {
        all_submitted = enqueue_row(rows[0]);
        CHECK(all_submitted);
        first_concurrent_index = 1;
        std::unique_lock lock(event_mutex);
        const bool predecessor_is_unsettled = event_changed.wait_for(
            lock, std::chrono::seconds(10), [&] {
                const bool row_prepared = std::any_of(
                    prepared_ordinals.begin(), prepared_ordinals.end(),
                    [&](const auto& entry) { return entry.first == rows[0].id; });
                return row_prepared && predecessor_worker_entered &&
                    std::find(sent_ordinals.begin(), sent_ordinals.end(), 1) !=
                        sent_ordinals.end();
            });
        CHECK(predecessor_is_unsettled);
        CHECK(std::find(f_commit_ordinals.begin(), f_commit_ordinals.end(), 1) ==
              f_commit_ordinals.end());
    }
    for (size_t index = first_concurrent_index; index < kCohort; ++index)
        all_submitted = enqueue_row(rows[index]) && all_submitted;
    CHECK(all_submitted);
    bool staged = false;
    {
        std::unique_lock lock(event_mutex);
        staged = event_changed.wait_for(lock, std::chrono::seconds(15), [&] {
            return target_staged;
        });
    }
    if (!staged) {
        std::lock_guard lock(event_mutex);
        std::fprintf(stderr,
            "P51_D07 staged-cancel precondition failed profile=%u index=%zu "
            "submitted=%u accepted=%zu prepared=%zu sent=%zu ops=%zu raw=%llu\n",
            static_cast<unsigned>(profile), cancelled_index, all_submitted,
            accepted_connections.load(std::memory_order_acquire),
            prepared_ordinals.size(), sent_ordinals.size(),
            c_runtime.pending_p51_source_operations_for_test(),
            static_cast<unsigned long long>(
                c_runtime.active_source_raw_bytes_for_test()));
        for (const auto& [request_id, ordinal] : prepared_ordinals)
            std::fprintf(stderr, "  prepared request=%llu ordinal=%llu\n",
                static_cast<unsigned long long>(request_id),
                static_cast<unsigned long long>(ordinal));
        std::fflush(stderr);
        for (auto& row : rows) {
            pollfd response_ready{row.pair.receiver.native_handle(), POLLIN, 0};
            int result;
            do { result = ::poll(&response_ready, 1, 0); }
            while (result < 0 && errno == EINTR);
            if (result <= 0 || !(response_ready.revents & POLLIN))
                continue;
            try {
                const auto transfer = receive_p51_transfer_result(
                    row.pair.receiver, c_launch.identity, row.id,
                    row.transfer.absolute_deadline.as_steady_time_point(), false);
                std::fprintf(stderr,
                    "  early-result request=%llu code=%u error=%u attempts=%u\n",
                    static_cast<unsigned long long>(row.id),
                    static_cast<unsigned>(transfer.code), transfer.error_code,
                    static_cast<unsigned>(transfer.attempts));
            } catch (const std::exception& error) {
                std::fprintf(stderr, "  early-result request=%llu exception=%s\n",
                    static_cast<unsigned long long>(row.id), error.what());
            }
            break;
        }
    }
    CHECK(staged);
    bool exact_cancel_seen = false;
    bool exact_f_cancelled = false;
    bool duplicate_f_cancelled = false;
    if (staged) {
        CHECK(target_binding.source_request_id == rows[cancelled_index].id);
        CHECK(target_binding.relationship_ordinal != 0);
        rows[cancelled_index].pair.receiver = local::Connection(-1);
        {
            std::unique_lock lock(event_mutex);
            exact_cancel_seen = event_changed.wait_for(
                lock, std::chrono::seconds(3), [&] {
                    return cancellation_observed;
                });
        }
        if (exact_cancel_seen) {
            exact_f_cancelled = f_runtime.cancel_p51_source_on_owner(
                rows[cancelled_index].transfer.armed.arm,
                rows[cancelled_index].transfer.armed.reservation_id,
                rows[cancelled_index].transfer.absolute_deadline
                    .as_steady_time_point());
            duplicate_f_cancelled = f_runtime.cancel_p51_source_on_owner(
                rows[cancelled_index].transfer.armed.arm,
                rows[cancelled_index].transfer.armed.reservation_id,
                rows[cancelled_index].transfer.absolute_deadline
                    .as_steady_time_point());
        }
        {
            std::lock_guard lock(event_mutex);
            release_target = true;
            release_predecessor_worker = true;
        }
        event_changed.notify_all();
    }

    size_t exact_survivors = 0;
    std::vector<uint64_t> survivor_tu_seqs;
    for (size_t index = 0; index < kCohort; ++index) {
        if (index == cancelled_index)
            continue;
        auto& row = rows[index];
        const auto result = receive_p51_transfer_result(
            row.pair.receiver, c_launch.identity, row.id,
            row.transfer.absolute_deadline.as_steady_time_point(), true);
        bool exact = result.code == local::SourceTransferResultCode::Committed &&
            result.valid() && result.c_store_guid == c_launch.c_store_guid &&
            result.raw_bytes == row.bytes.size() &&
            result.raw_digest == icecc::digest128(row.bytes);
        if (exact) {
            const InputLeaseOwner owner{
                row.transfer.armed.arm.source.logical_job,
                row.transfer.armed.arm.source.assignment_epoch,
                row.transfer.armed.arm.source.assignment_nonce};
            const InputFdRequest attach{
                f_launch.identity,
                InputRecordKey{result.c_store_guid, TuSeq{result.tu_seq}},
                owner, row.id + 600000};
            const auto deadline =
                row.transfer.absolute_deadline.as_steady_time_point();
            auto cursor = f_runtime.attach_input_on_owner(attach, deadline);
            exact = cursor && cursor->remaining() == row.bytes.size() &&
                    cursor->raw_digest() == result.raw_digest;
            if (exact) {
                std::vector<uint8_t> actual(row.bytes.size());
                exact = cursor->read(actual) == actual.size() &&
                        actual == row.bytes;
            }
            f_runtime.finish_input_attachment_on_owner(attach, exact, deadline);
        }
        if (exact)
            ++exact_survivors;
        survivor_tu_seqs.push_back(result.tu_seq);
    }

    // The 31-request cohort keeps the negotiated W30 unchanged: request 31
    // can only be prepared after one window slot has settled. This separate
    // probe proves that even when cohort position 30 was canceled before its
    // first byte, the next valid source transaction uses that same REL_SEQ.
    const uint64_t probe_id = first_id + kCohort;
    auto probe_reservation = test_p51_reservation_request(
        c_launch.c_store_guid, c_launch.store_generation,
        c_launch.identity.generation, c_launch.identity.attempt,
        probe_id, cache_profile, 30, std::chrono::seconds(30));
    probe_reservation.arm.source.assignment_nonce = probe_id;
    probe_reservation.arm.source.logical_job = 160000 + probe_id;
    probe_reservation.arm.source.compiler_attempt = 1;
    probe_reservation.arm.source.selected_f_host = "127.0.0.1";
    probe_reservation.arm.source.selected_f_cache_port = f_port;
    const auto probe_armed = f_runtime.reserve_p51_source_on_owner(
        probe_reservation);
    CHECK(probe_armed.error_code == 0 && probe_armed.armed.has_value());
    RequestRow probe{
        local::P51SourceTransferRequest{
            *probe_armed.armed, probe_reservation.absolute_deadline},
        authenticated_runtime_pair(), probe_id,
        std::vector<uint8_t>(512, 0xf0)};
    const auto probe_operation = local::make_p51_source_transfer_operation(
        c_launch.identity, probe.transfer, probe.id);
    CHECK(c_runtime.enqueue_p51_source_transfer(
        std::move(probe.pair.sender), c_launch.identity, probe_operation,
        sized_test_source_fd(probe.bytes.size(), probe.bytes.front())));
    const auto probe_result = receive_p51_transfer_result(
        probe.pair.receiver, c_launch.identity, probe.id,
        probe.transfer.absolute_deadline.as_steady_time_point(), true);
    bool probe_exact =
        probe_result.code == local::SourceTransferResultCode::Committed &&
        probe_result.valid() && probe_result.c_store_guid == c_launch.c_store_guid &&
        probe_result.raw_bytes == probe.bytes.size() &&
        probe_result.raw_digest == icecc::digest128(probe.bytes);
    if (probe_exact) {
        const InputLeaseOwner owner{
            probe.transfer.armed.arm.source.logical_job,
            probe.transfer.armed.arm.source.assignment_epoch,
            probe.transfer.armed.arm.source.assignment_nonce};
        const InputFdRequest attach{
            f_launch.identity,
            InputRecordKey{probe_result.c_store_guid,
                           TuSeq{probe_result.tu_seq}},
            owner, probe.id + 600000};
        const auto deadline =
            probe.transfer.absolute_deadline.as_steady_time_point();
        auto cursor = f_runtime.attach_input_on_owner(attach, deadline);
        probe_exact = cursor && cursor->remaining() == probe.bytes.size() &&
                      cursor->raw_digest() == probe_result.raw_digest;
        if (probe_exact) {
            std::vector<uint8_t> actual(probe.bytes.size());
            probe_exact = cursor->read(actual) == actual.size() &&
                          actual == probe.bytes;
        }
        f_runtime.finish_input_attachment_on_owner(attach, probe_exact, deadline);
    }
    std::sort(survivor_tu_seqs.begin(), survivor_tu_seqs.end());
    bool unique_tu_seqs = survivor_tu_seqs.size() == kSurvivors;
    for (size_t i = 1; i < survivor_tu_seqs.size(); ++i)
        unique_tu_seqs = unique_tu_seqs &&
                         survivor_tu_seqs[i] != survivor_tu_seqs[i - 1];

    bool target_retired_once = false;
    {
        std::lock_guard lock(event_mutex);
        const Id128 target_id{rows[cancelled_index].transfer.armed.reservation_id};
        const auto found = std::find_if(retired.begin(), retired.end(),
            [&](const auto& item) { return item.first == target_id; });
        target_retired_once = found != retired.end() && !found->second &&
            std::count_if(retired.begin(), retired.end(), [&](const auto& item) {
                return item.first == target_id;
            }) == 1;
    }
    std::vector<uint64_t> expected_ordinals(kSurvivors + 1);
    for (size_t i = 0; i < kSurvivors + 1; ++i)
        expected_ordinals[i] = i + 1;
    std::sort(expected_ordinals.begin(), expected_ordinals.end());
    std::vector<uint64_t> sent_ordinals_snapshot;
    {
        std::lock_guard lock(event_mutex);
        sent_ordinals_snapshot = sent_ordinals;
    }
    std::sort(sent_ordinals_snapshot.begin(), sent_ordinals_snapshot.end());
    bool exact_ordinals = sent_ordinals_snapshot == expected_ordinals;
    bool candidate_reused = false;
    {
        std::lock_guard lock(event_mutex);
        const auto target = std::find_if(prepared_ordinals.begin(),
            prepared_ordinals.end(), [&](const auto& entry) {
                return entry.first == rows[cancelled_index].id;
            });
        candidate_reused = target != prepared_ordinals.end() &&
            target->second == target_binding.relationship_ordinal &&
            std::next(target) != prepared_ordinals.end() &&
            std::next(target)->second == target->second;
        for (uint64_t ordinal = 1; ordinal <= kSurvivors + 1; ++ordinal) {
            const size_t count = static_cast<size_t>(std::count_if(
                prepared_ordinals.begin(), prepared_ordinals.end(),
                [&](const auto& item) { return item.second == ordinal; }));
            candidate_reused = candidate_reused &&
                count == (ordinal == target_binding.relationship_ordinal ? 2 : 1);
        }
        CHECK(materialized.size() == kSurvivors + 1);
        for (size_t index = 0; index < kCohort; ++index) {
            const std::vector<uint8_t> expected(
                128 + index, static_cast<uint8_t>(index + 1));
            const auto match = std::find_if(materialized.begin(), materialized.end(),
                [&](const auto& row) { return row.second == expected; });
            CHECK((index == cancelled_index) ==
                  (match == materialized.end()));
        }
        CHECK(std::any_of(materialized.begin(), materialized.end(),
            [&](const auto& row) { return row.second == probe.bytes; }));
    }
    const bool operations_released = wait_for_source_operation_count(
        c_runtime, 0, std::chrono::seconds(4));
    const bool raw_credit_released = wait_for_source_raw_bytes(
        c_runtime, 0, std::chrono::seconds(4));
    const bool one_link = accepted_connections.load(std::memory_order_acquire) == 1;

    CHECK(all_submitted);
    CHECK(staged);
    CHECK(exact_cancel_seen);
    CHECK(cancelled_hook_exact.load(std::memory_order_acquire));
    CHECK(exact_f_cancelled);
    CHECK(!duplicate_f_cancelled);
    CHECK(exact_survivors == kSurvivors);
    CHECK(probe_exact);
    CHECK(unique_tu_seqs);
    CHECK(exact_ordinals);
    CHECK(candidate_reused);
    CHECK(target_retired_once);
    CHECK(operations_released);
    CHECK(raw_credit_released);
    CHECK(one_link);
    std::printf("P51_D07 staged-cancel profile=%u position=%zu submitted=31 "
                "survivors=30 probe=1 exact-ordinals=1..31 "
                "target-no-bind=1 same-ordinal-reused=1\n",
                static_cast<unsigned>(profile), cancelled_index);
}

void test_p51_d07_staged_cancel_all_profiles() {
    for (const ProfileId profile : {
             ProfileId::P29V1, ProfileId::ZSTD_TU, ProfileId::ZSTD_ROUTE}) {
        for (const size_t index : {size_t{0}, size_t{15}, size_t{30}})
            test_p51_d07_staged_cancel_case(profile, index);
    }
}

// Active-cancel recovery regression: a complete bundle is paused on F's
// materialization worker, and a later request's complete bundle is observed
// in the TCP receive queue before the canceled reservation is retired. The
// canceled reservation must settle non-successfully without materialization,
// while the exact successor still
// commits on the same logical relationship and within its original deadline.
enum class D07Scenario : uint8_t {
    ActiveCancel,
    InterruptedReplay,
    PositiveRecoveryOwner,
    CommittedAttemptReplacement,
};

std::vector<uint8_t> d14_input(size_t index) {
    static constexpr std::array<std::string_view, 4> history = {
        "", "alpha\nbeta\n", "alpha\nbeta\n", "alpha\ngamma\n"};
    if (index < history.size()) {
        const auto value = history[index];
        return std::vector<uint8_t>(value.begin(), value.end());
    }
    std::string value = "header\n";
    value += (index % 5 == 0) ? "repeat\n" : "line-";
    if (index % 5 != 0)
        value += std::to_string(index) + "\n";
    value += "tail-" + std::to_string(index % 3) + "\n";
    return std::vector<uint8_t>(value.begin(), value.end());
}

local::HandoffFd d14_exact_source_fd(std::span<const uint8_t> bytes) {
    const char* temporary_directory = std::getenv("TMPDIR");
    if (temporary_directory == nullptr || temporary_directory[0] == '\0')
        temporary_directory = "/tmp";
    std::string path = std::string(temporary_directory) +
                       "/icecc-p51-d14-source-XXXXXX";
    std::vector<char> mutable_path(path.begin(), path.end());
    mutable_path.push_back('\0');
    const int fd = ::mkstemp(mutable_path.data());
    CHECK(fd >= 0);
    CHECK(::unlink(mutable_path.data()) == 0);
    CHECK(write_all(fd, bytes));
    CHECK(::lseek(fd, 0, SEEK_SET) == 0);
    return local::HandoffFd(fd);
}

void test_p51_d14_reset_boundary_smoke(ProfileId profile,
                                       size_t job_count = 3,
                                       size_t cut_boundary = 1,
                                       bool w30 = false) {
    CHECK(job_count >= 3);
    CHECK(cut_boundary <= job_count);
    struct RunResult {
        std::vector<local::P50SourceTransferResult> results;
        std::vector<std::vector<uint8_t>> inputs;
        std::vector<R2TxCommit> commits;
        std::vector<ResetAck> reset_acks;
        std::vector<uint64_t> sent_ordinals;
        std::vector<std::pair<CStoreGuid, TuSeq>> materialized_identity;
        std::vector<std::vector<uint8_t>> materialized_bytes;
        std::vector<LinkHello> quiesced_links;
        std::vector<LinkState> observed_link_states;
        size_t accepted_links = 0;
        bool owned_duplicate_shutdown = false;
        Id128 relationship_id{};
        bool held_suffix_worker = false;
        bool suffix_bundles_sent = false;
        bool reset_before_release = false;
        bool exact_attachments = false;
        size_t final_source_operations = 0;
        uint64_t final_source_raw_bytes = 0;
    };
    const auto profile_bits = profile_bit(profile);
    uint32_t cache_profile = 0;
    switch (profile) {
    case ProfileId::P29V1: cache_profile = CACHE_PROFILE_P29V1; break;
    case ProfileId::ZSTD_TU: cache_profile = CACHE_PROFILE_ZSTD_TU; break;
    case ProfileId::ZSTD_ROUTE: cache_profile = CACHE_PROFILE_ZSTD_ROUTE; break;
    }
    auto run_episode = [&](bool force_reset) {
        StoreIdentityRoot c_root{};
        c_root.bytes[15] = force_reset ? 0x6c : 0x6b;
        const SidecarLaunchIdentity c_launch = test_sidecar_launch(c_root);
        StoreIdentityRoot f_root{};
        f_root.bytes[15] = force_reset ? 0x7c : 0x7b;
        const SidecarLaunchIdentity f_launch = test_sidecar_launch(f_root);
        uint16_t f_port = 0;
        const int listener = loopback_listener(f_port);
        CHECK(listener >= 0 && f_port != 0);

        RunResult output;
        std::mutex gate_mutex;
        std::condition_variable gate_changed;
        bool gate_armed = false;
        bool release_gate = true;
        bool suffix_worker_entered = false;
        size_t materialize_calls = 0;
        size_t hold_from_call = 2;
        std::mutex event_mutex;
        std::condition_variable event_changed;
        std::vector<R2TxCommit> commits;
        std::vector<ResetAck> reset_acks;
        std::vector<uint64_t> sent_ordinals;
        std::vector<std::pair<CStoreGuid, TuSeq>> materialized_identity;
        std::vector<std::vector<uint8_t>> materialized_bytes;
        std::vector<LinkHello> quiesced_links;
        std::vector<LinkState> observed_link_states;

        service::RuntimeConfig f_config = test_runtime_config();
        f_config.c_store_guid = f_launch.c_store_guid;
        f_config.f_store_guid = f_launch.f_store_guid;
        f_config.f_store_generation = f_launch.store_generation;
        f_config.sidecar_launch = f_launch;
        if (w30)
            f_config.max_active_source_transfers = 1;
        f_config.endpoint_caps.profile = profile;
        f_config.endpoint_caps.supported_profiles = profile_bits;
        f_config.endpoint_caps.zstd.max_raw_bytes = 65536;
        f_config.max_pending_p51_source_reservations = job_count + 8;
        f_config.endpoint_config.input_job_state =
            [&](CStoreGuid c_guid, const TxBegin& begin, const TxCommit&,
                std::span<const uint8_t> bytes) {
                std::lock_guard lock(event_mutex);
                materialized_identity.emplace_back(c_guid, begin.tu_seq);
                materialized_bytes.emplace_back(bytes.begin(), bytes.end());
                event_changed.notify_all();
                return InputJobState::Open;
            };
        service::SidecarRuntime f_runtime(std::move(f_config));

        service::RuntimeConfig c_config = test_runtime_config();
        c_config.c_store_guid = c_launch.c_store_guid;
        c_config.f_store_guid = c_launch.f_store_guid;
        c_config.f_store_generation = c_launch.store_generation;
        c_config.sidecar_launch = c_launch;
        c_config.endpoint_caps.profile = profile;
        c_config.endpoint_caps.supported_profiles = profile_bits;
        c_config.endpoint_caps.zstd.max_raw_bytes = 65536;
        c_config.max_active_source_transfers = w30 ? 32 : 4;
        // The P51 source admission cap is deliberately above 30 here. The
        // protocol's negotiated W30 is the wire-level bound; request 31 must
        // be accepted into the client queue without becoming wire ordinal 31.
        c_config.max_active_p51_source_transfers = w30 ? 32 : 8;
        c_config.max_pending_p51_source_operations = job_count + 8;
        c_config.max_aggregate_source_raw_bytes = 1024 * 1024;
        c_config.after_r2_bundle_sent_for_test = [&](uint64_t ordinal) {
            std::lock_guard lock(event_mutex);
            sent_ordinals.push_back(ordinal);
            event_changed.notify_all();
        };
        service::SidecarRuntime c_runtime(std::move(c_config));

        std::atomic<bool> stop_accepting{false};
        std::atomic<size_t> accepted_connections{0};
        std::atomic<int> probe_fd{-1};
        std::thread acceptor([&] {
            const auto end = std::chrono::steady_clock::now() +
                             std::chrono::seconds(45);
            while (!stop_accepting.load(std::memory_order_acquire) &&
                   accepted_connections.load(std::memory_order_acquire) < 8 &&
                   std::chrono::steady_clock::now() < end) {
                pollfd ready{listener, POLLIN, 0};
                int polled;
                do { polled = ::poll(&ready, 1, 100); }
                while (polled < 0 && errno == EINTR);
                if (polled <= 0 || (ready.revents & POLLIN) == 0)
                    continue;
                sockaddr_storage peer{};
                socklen_t peer_size = sizeof(peer);
                int fd;
                do {
                    fd = ::accept(listener,
                        reinterpret_cast<sockaddr*>(&peer), &peer_size);
                } while (fd < 0 && errno == EINTR);
                if (fd < 0)
                    continue;
                std::unique_ptr<MsgChannel> channel(
                    Service::createChannelAccepted(
                        fd, reinterpret_cast<sockaddr*>(&peer), peer_size));
                if (!channel)
                    continue;
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(4);
                bool ready_protocol = true;
                while (channel->protocol_admission_state() ==
                           MsgChannel::ProtocolAdmissionState::Pending &&
                       std::chrono::steady_clock::now() < deadline) {
                    short events = POLLIN;
                    if (channel->has_pending_write())
                        events = static_cast<short>(events | POLLOUT);
                    pollfd socket{channel->fd, events, 0};
                    int result;
                    do { result = ::poll(&socket, 1, 50); }
                    while (result < 0 && errno == EINTR);
                    if (result < 0 ||
                        (socket.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                        ready_protocol = false;
                        break;
                    }
                    if ((socket.revents & POLLOUT) &&
                        !channel->flush_pending()) {
                        ready_protocol = false;
                        break;
                    }
                    if ((socket.revents & POLLIN) && !channel->read_a_bit()) {
                        ready_protocol = false;
                        break;
                    }
                }
                if (!ready_protocol ||
                    channel->protocol_admission_state() !=
                        MsgChannel::ProtocolAdmissionState::Ready ||
                    !channel->finish_protocol_admission())
                    continue;
                std::unique_ptr<Msg> request(channel->get_msg_until(deadline));
                if (!dynamic_cast<P51CacheLinkSessionMsg*>(request.get()))
                    continue;
                const int adopted =
                    channel->send_p51_cache_link_session_ready_and_release(
                        deadline);
                if (adopted < 0)
                    continue;
                const int duplicate = ::dup(adopted);
                if (duplicate >= 0) {
                    const int previous = probe_fd.exchange(
                        duplicate, std::memory_order_acq_rel);
                    if (previous >= 0)
                        (void)::close(previous);
                }
                EndpointIoControl control;
                control.before_materialize_on_worker = [&] {
                    std::unique_lock lock(gate_mutex);
                    ++materialize_calls;
                    if (!gate_armed || materialize_calls < hold_from_call)
                        return;
                    suffix_worker_entered = true;
                    gate_changed.notify_all();
                    gate_changed.wait(lock, [&] { return release_gate; });
                };
                control.outbound_message_observer =
                    [&](ActorSide actor, const Message& message) {
                        if (actor != ActorSide::F)
                            return;
                        std::lock_guard lock(event_mutex);
                        if (const auto* commit = std::get_if<R2TxCommit>(&message))
                            commits.push_back(*commit);
                        else if (const auto* ack = std::get_if<ResetAck>(&message))
                            reset_acks.push_back(*ack);
                        else if (const auto* link_state =
                                     std::get_if<LinkState>(&message))
                            observed_link_states.push_back(*link_state);
                        event_changed.notify_all();
                    };
                control.r2_link_io_quiesced_observer =
                    [&](const LinkHello& hello, uint64_t, uint64_t) {
                        std::lock_guard lock(event_mutex);
                        quiesced_links.push_back(hello);
                        event_changed.notify_all();
                    };
                accepted_connections.fetch_add(1, std::memory_order_acq_rel);
                f_runtime.start_adopted_r2_endpoint(adopted,
                                                     std::move(control));
            }
        });
        auto cleanup = std::unique_ptr<int, std::function<void(int*)>>(
            reinterpret_cast<int*>(1), [&](int*) {
                {
                    std::lock_guard lock(gate_mutex);
                    release_gate = true;
                }
                gate_changed.notify_all();
                stop_accepting.store(true, std::memory_order_release);
                (void)::shutdown(listener, SHUT_RDWR);
                if (acceptor.joinable())
                    acceptor.join();
                (void)::close(listener);
                const int duplicate = probe_fd.exchange(
                    -1, std::memory_order_acq_rel);
                if (duplicate >= 0)
                    (void)::close(duplicate);
                c_runtime.stop();
                f_runtime.stop();
            });

        struct Request {
            local::P51SourceTransferRequest transfer;
            RuntimeCase pair{local::Connection(-1), local::Connection(-1)};
            uint64_t id = 0;
            std::vector<uint8_t> bytes;
        };
        auto make_request = [&](uint64_t id, std::vector<uint8_t> bytes) {
            auto reservation = test_p51_reservation_request(
                c_launch.c_store_guid, c_launch.store_generation,
                c_launch.identity.generation, c_launch.identity.attempt,
                id, cache_profile, 30, std::chrono::seconds(30));
            reservation.arm.source.assignment_nonce = id;
            reservation.arm.source.logical_job = 140000 + id;
            reservation.arm.source.compiler_attempt = 1;
            reservation.arm.source.selected_f_host = "127.0.0.1";
            reservation.arm.source.selected_f_cache_port = f_port;
            const auto armed = f_runtime.reserve_p51_source_on_owner(reservation);
            CHECK(armed.error_code == 0 && armed.armed.has_value());
            return Request{
                local::P51SourceTransferRequest{
                    *armed.armed, reservation.absolute_deadline},
                authenticated_runtime_pair(), id, std::move(bytes)};
        };
        auto enqueue = [&](Request& request) {
            const auto operation = local::make_p51_source_transfer_operation(
                c_launch.identity, request.transfer, request.id);
            return c_runtime.enqueue_p51_source_transfer(
                std::move(request.pair.sender), c_launch.identity, operation,
                d14_exact_source_fd(request.bytes)) ==
                service::P51SourceEnqueueResult::Accepted;
        };
        auto receive = [&](Request& request) {
            return receive_p51_transfer_result(
                request.pair.receiver, c_launch.identity, request.id,
                request.transfer.absolute_deadline.as_steady_time_point(), true);
        };
        auto attach_exact = [&](Request& request,
                                const local::P50SourceTransferResult& result) {
            if (result.code != local::SourceTransferResultCode::Committed ||
                !result.valid() || result.raw_bytes != request.bytes.size() ||
                result.raw_digest != icecc::digest128(request.bytes))
                return false;
            const InputLeaseOwner owner{
                request.transfer.armed.arm.source.logical_job,
                request.transfer.armed.arm.source.assignment_epoch,
                request.transfer.armed.arm.source.assignment_nonce};
            const InputFdRequest attach{
                f_launch.identity,
                InputRecordKey{result.c_store_guid, TuSeq{result.tu_seq}},
                owner, request.id + 900000};
            const auto deadline =
                request.transfer.absolute_deadline.as_steady_time_point();
            auto cursor = f_runtime.attach_input_on_owner(attach, deadline);
            bool exact = cursor && cursor->remaining() == request.bytes.size() &&
                         cursor->raw_digest() == result.raw_digest;
            if (exact) {
                std::vector<uint8_t> actual(request.bytes.size());
                exact = cursor->read(actual) == actual.size() &&
                        actual == request.bytes;
            }
            f_runtime.finish_input_attachment_on_owner(attach, exact, deadline);
            return exact;
        };

        std::vector<Request> requests;
        requests.reserve(job_count);
        for (size_t index = 0; index < job_count; ++index)
            requests.push_back(make_request(
                9201 + index, d14_input(index == 0 ? 1 : index + 1)));
        output.relationship_id.bytes =
            requests[0].transfer.armed.logical_relationship_id;
        output.inputs.reserve(requests.size());
        for (const auto& request : requests)
            output.inputs.push_back(request.bytes);
        if (!force_reset) {
            bool all_exact_attachments = true;
            for (auto& request : requests) {
                CHECK(enqueue(request));
                auto result = receive(request);
                if (result.code != local::SourceTransferResultCode::Committed) {
                    std::fprintf(stderr,
                        "P51_D14 transfer-failed profile=%u episode=baseline "
                        "index=%zu code=%u error=%u attempts=%u raw=%zu\n",
                        static_cast<unsigned>(profile), output.results.size(),
                        static_cast<unsigned>(result.code), result.error_code,
                        static_cast<unsigned>(result.attempts),
                        request.bytes.size());
                    std::fflush(stderr);
                }
                CHECK(result.code == local::SourceTransferResultCode::Committed);
                all_exact_attachments = attach_exact(request, result) &&
                                        all_exact_attachments;
                output.results.push_back(result);
            }
            output.exact_attachments = all_exact_attachments;
        } else if (w30) {
            bool exact_attachments = true;
            for (size_t index = 0; index < cut_boundary; ++index) {
                CHECK(enqueue(requests[index]));
                auto result = receive(requests[index]);
                CHECK(result.code ==
                      local::SourceTransferResultCode::Committed);
                exact_attachments =
                    attach_exact(requests[index], result) && exact_attachments;
                output.results.push_back(std::move(result));
            }
            if (cut_boundary == 31 && job_count == 32) {
                const int descriptor = probe_fd.load(std::memory_order_acquire);
                CHECK(descriptor >= 0);
                CHECK(::shutdown(descriptor, SHUT_RDWR) == 0);
                output.owned_duplicate_shutdown = true;
                CHECK(!suffix_worker_entered);
                CHECK(enqueue(requests[31]));
                auto probe = receive(requests[31]);
                CHECK(probe.code == local::SourceTransferResultCode::Committed);
                exact_attachments =
                    attach_exact(requests[31], probe) && exact_attachments;
                output.results.push_back(std::move(probe));
                output.exact_attachments = exact_attachments;
            } else {
                CHECK(cut_boundary < job_count);
                const size_t suffix_count = job_count - cut_boundary;
                const size_t expected_suffix_sent =
                    std::min<size_t>(suffix_count, 30);
                const uint64_t expected_p = cut_boundary + expected_suffix_sent;
                {
                    std::lock_guard lock(gate_mutex);
                    gate_armed = true;
                    release_gate = false;
                    hold_from_call = materialize_calls + 1;
                }
                bool all_enqueued = true;
                uint64_t expected_raw_bytes = 0;
                for (size_t index = cut_boundary; index < job_count; ++index) {
                    auto &request = requests[index];
                    expected_raw_bytes += request.bytes.size();
                    all_enqueued = enqueue(request) && all_enqueued;
                }
                bool worker_held = false;
                {
                    std::unique_lock lock(gate_mutex);
                    worker_held = gate_changed.wait_for(
                        lock, std::chrono::seconds(8),
                        [&] { return suffix_worker_entered; });
                }
                const bool operations_full =
                    all_enqueued &&
                    wait_for_source_operation_count(c_runtime, suffix_count,
                                                    std::chrono::seconds(8));
                const bool raw_credits_full = wait_for_source_raw_bytes(
                    c_runtime, expected_raw_bytes, std::chrono::seconds(8));
                bool initial_window_full = false;
                {
                    std::unique_lock lock(event_mutex);
                    initial_window_full = event_changed.wait_for(
                        lock, std::chrono::seconds(8),
                        [&] { return sent_ordinals.size() >= expected_p; });
                    CHECK(sent_ordinals.size() == expected_p);
                    for (uint64_t ordinal = 1; ordinal <= expected_p; ++ordinal)
                        CHECK(std::count(sent_ordinals.begin(),
                                         sent_ordinals.end(), ordinal) == 1);
                    CHECK(std::find(sent_ordinals.begin(), sent_ordinals.end(),
                                    expected_p + 1) == sent_ordinals.end());
                }
                if (!(worker_held && operations_full && raw_credits_full &&
                      initial_window_full)) {
                    std::lock_guard lock(event_mutex);
                    std::fprintf(
                        stderr,
                        "P51_D14 w30-precut profile=%u held=%u operations=%u "
                        "raw=%u window=%u op-count=%zu expected-ops=%zu "
                        "raw-now=%llu raw-expected=%llu sent-count=%zu\n",
                        static_cast<unsigned>(profile), worker_held,
                        operations_full, raw_credits_full, initial_window_full,
                        c_runtime.pending_p51_source_operations_for_test(),
                        suffix_count,
                        static_cast<unsigned long long>(
                            c_runtime.active_source_raw_bytes_for_test()),
                        static_cast<unsigned long long>(expected_raw_bytes),
                        sent_ordinals.size());
                    std::fflush(stderr);
                }
                CHECK(worker_held && operations_full && raw_credits_full &&
                      initial_window_full);
                CHECK(c_runtime.pending_p51_source_operations_for_test() ==
                      suffix_count);
                CHECK(c_runtime.active_source_raw_bytes_for_test() ==
                      expected_raw_bytes);
                output.held_suffix_worker = worker_held;
                output.suffix_bundles_sent = initial_window_full;

                const int descriptor = probe_fd.load(std::memory_order_acquire);
                CHECK(descriptor >= 0);
                CHECK(::shutdown(descriptor, SHUT_RDWR) == 0);
                output.owned_duplicate_shutdown = true;
                std::vector<std::optional<local::P50SourceTransferResult>>
                    results(suffix_count);
                std::vector<std::exception_ptr> waiter_errors(suffix_count);
                std::vector<std::thread> waiters;
                waiters.reserve(suffix_count);
                for (size_t suffix_index = 0; suffix_index < suffix_count;
                     ++suffix_index) {
                    waiters.emplace_back([&, suffix_index] {
                        try {
                            results[suffix_index] =
                                receive(requests[cut_boundary + suffix_index]);
                        } catch (...) {
                            waiter_errors[suffix_index] =
                                std::current_exception();
                        }
                    });
                }
                const uint64_t expected_k = cut_boundary;
                bool reset_seen = false;
                {
                    std::unique_lock lock(event_mutex);
                    reset_seen = event_changed.wait_for(
                        lock, std::chrono::seconds(10), [&] {
                            return std::any_of(
                                reset_acks.begin(), reset_acks.end(),
                                [&](const ResetAck &ack) {
                                    return ack.request.settled_prefix_k ==
                                               expected_k &&
                                           ack.recovery_prepared_prefix_p ==
                                               expected_p;
                                });
                        });
                    if (reset_seen) {
                        const auto ack = std::find_if(
                            reset_acks.begin(), reset_acks.end(),
                            [&](const ResetAck &item) {
                                return item.request.settled_prefix_k ==
                                           expected_k &&
                                       item.recovery_prepared_prefix_p ==
                                           expected_p;
                            });
                        output.reset_acks.push_back(*ack);
                    }
                }
                {
                    std::lock_guard lock(gate_mutex);
                    gate_armed = false;
                    release_gate = true;
                }
                gate_changed.notify_all();
                for (auto &waiter : waiters)
                    waiter.join();
                output.reset_before_release = reset_seen;
                CHECK(reset_seen);
                for (size_t index = 0; index < waiter_errors.size(); ++index) {
                    if (waiter_errors[index]) {
                        std::fprintf(
                            stderr,
                            "P51_D14 waiter-exception profile=%u index=%zu\n",
                            static_cast<unsigned>(profile), index);
                        std::rethrow_exception(waiter_errors[index]);
                    }
                }
                for (size_t suffix_index = 0; suffix_index < suffix_count;
                     ++suffix_index) {
                    CHECK(results[suffix_index].has_value());
                    if (results[suffix_index]->code !=
                        local::SourceTransferResultCode::Committed) {
                        std::fprintf(
                            stderr,
                            "P51_D14 transfer-failed profile=%u episode=W30 "
                            "index=%zu code=%u error=%u attempts=%u raw=%zu\n",
                            static_cast<unsigned>(profile),
                            cut_boundary + suffix_index,
                            static_cast<unsigned>(results[suffix_index]->code),
                            results[suffix_index]->error_code,
                            static_cast<unsigned>(
                                results[suffix_index]->attempts),
                            requests[cut_boundary + suffix_index].bytes.size());
                        std::fflush(stderr);
                    }
                    CHECK(results[suffix_index]->code ==
                          local::SourceTransferResultCode::Committed);
                    const size_t index = cut_boundary + suffix_index;
                    exact_attachments =
                        attach_exact(requests[index], *results[suffix_index]) &&
                        exact_attachments;
                    output.results.push_back(*results[suffix_index]);
                }
                output.exact_attachments = exact_attachments;
            }
        } else {
            // First TU is a settled positive prefix. Hold worker #2 while the
            // writer sends the complete suffix; an external duplicate-F-socket
            // shutdown then forces RESET with K=1 and P=3.
            CHECK(enqueue(requests[0]));
            auto first = receive(requests[0]);
            CHECK(first.code == local::SourceTransferResultCode::Committed);
            CHECK(attach_exact(requests[0], first));
            output.results.push_back(first);
            {
                std::lock_guard lock(gate_mutex);
                gate_armed = true;
                release_gate = false;
            }
            CHECK(enqueue(requests[1]));
            bool entered = false;
            {
                std::unique_lock lock(gate_mutex);
                entered = gate_changed.wait_for(lock, std::chrono::seconds(8), [&] {
                    return suffix_worker_entered;
                });
            }
            CHECK(entered);
            CHECK(enqueue(requests[2]));
            bool all_sent = false;
            {
                std::unique_lock lock(event_mutex);
                all_sent = event_changed.wait_for(lock, std::chrono::seconds(8), [&] {
                    return std::count(sent_ordinals.begin(), sent_ordinals.end(), 2) >= 1 &&
                           std::count(sent_ordinals.begin(), sent_ordinals.end(), 3) >= 1;
                });
            }
            CHECK(all_sent);
            output.held_suffix_worker = entered;
            output.suffix_bundles_sent = all_sent;
            const int descriptor = probe_fd.load(std::memory_order_acquire);
            CHECK(descriptor >= 0);
            CHECK(::shutdown(descriptor, SHUT_RDWR) == 0);
            output.owned_duplicate_shutdown = true;
            std::vector<std::optional<local::P50SourceTransferResult>> suffix_results(2);
            std::vector<bool> attached(2, false);
            std::array<std::thread, 2> waiters{
                std::thread([&] {
                    try { suffix_results[0] = receive(requests[1]); } catch (...) {}
                }),
                std::thread([&] {
                    try { suffix_results[1] = receive(requests[2]); } catch (...) {}
                })};
            bool reset_seen = false;
            {
                std::unique_lock lock(event_mutex);
                reset_seen = event_changed.wait_for(lock, std::chrono::seconds(10), [&] {
                    return std::any_of(reset_acks.begin(), reset_acks.end(),
                        [](const ResetAck& ack) {
                            return ack.request.settled_prefix_k == 1 &&
                                   ack.recovery_prepared_prefix_p == 3;
                        });
                });
                if (reset_seen) {
                    const auto ack = std::find_if(reset_acks.begin(), reset_acks.end(),
                        [](const ResetAck& item) {
                            return item.request.settled_prefix_k == 1 &&
                                   item.recovery_prepared_prefix_p == 3;
                        });
                    output.reset_acks.push_back(*ack);
                }
            }
            output.reset_before_release = reset_seen;
            {
                std::lock_guard lock(gate_mutex);
                release_gate = true;
                gate_armed = false;
            }
            gate_changed.notify_all();
            for (auto& waiter : waiters)
                waiter.join();
            CHECK(reset_seen);
            CHECK(suffix_results[0].has_value() && suffix_results[1].has_value());
            for (size_t i = 0; i < 2; ++i) {
                if (suffix_results[i]->code !=
                        local::SourceTransferResultCode::Committed) {
                    std::fprintf(stderr,
                        "P51_D14 transfer-failed profile=%u episode=recovered "
                        "index=%zu code=%u error=%u attempts=%u raw=%zu\n",
                        static_cast<unsigned>(profile), i + 1,
                        static_cast<unsigned>(suffix_results[i]->code),
                        suffix_results[i]->error_code,
                        static_cast<unsigned>(suffix_results[i]->attempts),
                        requests[i + 1].bytes.size());
                    std::fflush(stderr);
                }
            }
            CHECK(suffix_results[0]->code == local::SourceTransferResultCode::Committed);
            CHECK(suffix_results[1]->code == local::SourceTransferResultCode::Committed);
            for (size_t i = 0; i < 2; ++i) {
                attached[i] = attach_exact(requests[i + 1], *suffix_results[i]);
                output.results.push_back(*suffix_results[i]);
            }
            output.exact_attachments = attached[0] && attached[1];
        }
        // Stop requests asynchronous owner cleanup; it does not join the
        // endpoint work. Wait for accounting to drain before taking the final
        // credit snapshot rather than racing that cleanup.
        cleanup.reset();
        const auto drain_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::seconds(5);
        size_t pending_operations =
            c_runtime.pending_p51_source_operations_for_test();
        uint64_t pending_raw_bytes =
            c_runtime.active_source_raw_bytes_for_test();
        while ((pending_operations != 0 || pending_raw_bytes != 0) &&
               std::chrono::steady_clock::now() < drain_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            pending_operations =
                c_runtime.pending_p51_source_operations_for_test();
            pending_raw_bytes = c_runtime.active_source_raw_bytes_for_test();
        }
        if (pending_operations != 0 || pending_raw_bytes != 0) {
            std::fprintf(stderr,
                         "P51_D14 cleanup-credit-timeout operations=%zu raw=%llu\n",
                         pending_operations,
                         static_cast<unsigned long long>(pending_raw_bytes));
            std::fflush(stderr);
        }
        output.final_source_operations =
            pending_operations;
        output.final_source_raw_bytes =
            pending_raw_bytes;
        output.accepted_links = accepted_connections.load(
            std::memory_order_acquire);
        {
            std::lock_guard lock(event_mutex);
            output.commits = commits;
            output.sent_ordinals = sent_ordinals;
            output.reset_acks = reset_acks;
            output.materialized_identity = materialized_identity;
            output.materialized_bytes = materialized_bytes;
            output.quiesced_links = quiesced_links;
            output.observed_link_states = observed_link_states;
        }
        return output;
    };

    const RunResult baseline = run_episode(false);
    const RunResult recovered = run_episode(true);
    CHECK(baseline.results.size() == job_count);
    CHECK(recovered.results.size() == job_count);
    CHECK(baseline.exact_attachments);
    CHECK(baseline.final_source_operations == 0);
    CHECK(baseline.final_source_raw_bytes == 0);
    CHECK(recovered.exact_attachments);
    CHECK(recovered.final_source_operations == 0);
    CHECK(recovered.final_source_raw_bytes == 0);
    auto committed_records = [](const RunResult& run) {
        std::vector<R2TxCommit> unique;
        for (const auto& commit : run.commits) {
            const auto prior = std::find_if(unique.begin(), unique.end(),
                [&](const R2TxCommit& candidate) {
                    return candidate.relationship_ordinal ==
                           commit.relationship_ordinal;
                });
            if (prior == unique.end())
                unique.push_back(commit);
            else {
                CHECK(prior->relationship_ordinal ==
                      commit.relationship_ordinal);
                CHECK(prior->inner == commit.inner);
                CHECK(prior->transaction_digest == commit.transaction_digest);
            }
        }
        std::sort(unique.begin(), unique.end(),
            [](const R2TxCommit& left, const R2TxCommit& right) {
                return left.relationship_ordinal < right.relationship_ordinal;
            });
        return unique;
    };
    const auto baseline_commits = committed_records(baseline);
    const auto recovered_commits = committed_records(recovered);
    CHECK(baseline_commits.size() == job_count);
    CHECK(recovered_commits.size() == job_count);
    auto materialization_matches = [](const RunResult& run, size_t index) {
        const auto& result = run.results[index];
        for (size_t i = 0; i < run.materialized_identity.size(); ++i) {
            if (run.materialized_identity[i].first == result.c_store_guid &&
                run.materialized_identity[i].second.value == result.tu_seq &&
                run.materialized_bytes[i] == run.inputs[index])
                return true;
        }
        return false;
    };
    for (size_t i = 0; i < job_count; ++i) {
        CHECK(baseline.results[i].code == local::SourceTransferResultCode::Committed);
        CHECK(recovered.results[i].code == local::SourceTransferResultCode::Committed);
        CHECK(baseline.results[i].raw_bytes == recovered.results[i].raw_bytes);
        CHECK(baseline.results[i].raw_digest == recovered.results[i].raw_digest);
        CHECK(baseline.inputs[i] == recovered.inputs[i]);
        CHECK(baseline.results[i].raw_bytes == baseline.inputs[i].size());
        CHECK(baseline.results[i].raw_digest ==
              icecc::digest128(baseline.inputs[i]));
        CHECK(recovered.results[i].raw_bytes == recovered.inputs[i].size());
        CHECK(recovered.results[i].raw_digest ==
              icecc::digest128(recovered.inputs[i]));
        CHECK(materialization_matches(baseline, i));
        CHECK(materialization_matches(recovered, i));
        const auto has_receipt = [&](const std::vector<R2TxCommit>& records,
                                     const local::P50SourceTransferResult& result) {
            return std::any_of(records.begin(), records.end(),
                [&](const R2TxCommit& record) {
                    return record.inner.tu_seq.value == result.tu_seq &&
                           record.inner.raw_digest == result.raw_digest;
                });
        };
        CHECK(has_receipt(baseline_commits, baseline.results[i]));
        CHECK(has_receipt(recovered_commits, recovered.results[i]));
    }
    if (w30 && cut_boundary == 31 && job_count == 32) {
        CHECK(recovered.owned_duplicate_shutdown);
        CHECK(!recovered.held_suffix_worker);
        CHECK(!recovered.suffix_bundles_sent);
        CHECK(!recovered.reset_before_release);
        CHECK(recovered.accepted_links >= 2);
        CHECK(!recovered.quiesced_links.empty());
        CHECK(recovered.observed_link_states.size() >= 2);
        CHECK(recovered.commits.size() == job_count);
        for (uint64_t ordinal = 1; ordinal <= job_count; ++ordinal)
            CHECK(std::count_if(recovered.commits.begin(),
                                recovered.commits.end(),
                [&](const R2TxCommit& receipt) {
                    return receipt.relationship_ordinal == ordinal;
                }) == 1);
        CHECK(std::all_of(recovered.quiesced_links.begin(),
                          recovered.quiesced_links.end(),
            [&](const LinkHello& hello) {
                return hello.relationship_id == recovered.relationship_id;
            }));
        const auto first_link = std::min_element(
            recovered.quiesced_links.begin(), recovered.quiesced_links.end(),
            [](const LinkHello& left, const LinkHello& right) {
                return left.physical_link_generation <
                       right.physical_link_generation;
            });
        CHECK(first_link->relationship_id == recovered.relationship_id);
        const auto reconnect = std::find_if(
            recovered.observed_link_states.begin(),
            recovered.observed_link_states.end(),
            [&](const LinkState& state) {
                return state.relationship_id == recovered.relationship_id &&
                       state.physical_link_generation >
                           first_link->physical_link_generation;
            });
        CHECK(reconnect != recovered.observed_link_states.end());
        CHECK(reconnect->relationship_epoch == first_link->relationship_epoch);
        uint64_t terminal_reset_k = 0;
        uint64_t terminal_reset_p = 0;
        if (!recovered.reset_acks.empty()) {
            CHECK(recovered.reset_acks.size() == 1);
            const ResetAck& terminal_ack = recovered.reset_acks.front();
            CHECK(terminal_ack.request.relationship_id ==
                  recovered.relationship_id);
            CHECK(terminal_ack.request.settled_prefix_k == 31);
            CHECK(terminal_ack.recovery_prepared_prefix_p == 32);
            terminal_reset_k = terminal_ack.request.settled_prefix_k;
            terminal_reset_p = terminal_ack.recovery_prepared_prefix_p;
        }
        for (size_t index = 0; index < 31; ++index)
            CHECK(recovered.results[index].code ==
                      local::SourceTransferResultCode::Committed &&
                  recovered.results[index].tu_seq == index);
        CHECK(recovered.results[31].tu_seq == 31);
        std::printf("P51_D14 w30-terminal-reconnect profile=%u settled-K=31 "
                    "probe=32 owned-cut=1 adopted-links=%zu old-generation=%llu "
                    "new-generation=%llu held-worker=0 exact-results=32 "
                    "exact-attachments=1 commits=32 reset-acks=%zu "
                    "reset-K=%llu reset-P=%llu "
                    "final-ops=0 final-raw=0\n",
                    static_cast<unsigned>(profile), recovered.accepted_links,
                    static_cast<unsigned long long>(
                        first_link->physical_link_generation),
                    static_cast<unsigned long long>(
                        reconnect->physical_link_generation),
                    recovered.reset_acks.size(),
                    static_cast<unsigned long long>(terminal_reset_k),
                    static_cast<unsigned long long>(terminal_reset_p));
        return;
    }
    CHECK(recovered.held_suffix_worker);
    CHECK(recovered.suffix_bundles_sent);
    CHECK(recovered.reset_before_release);
    CHECK(recovered.owned_duplicate_shutdown);
    CHECK(recovered.reset_acks.size() == 1);
    const ResetAck& ack = recovered.reset_acks.front();
    const uint64_t expected_k = cut_boundary;
    CHECK(ack.request.settled_prefix_k == expected_k);
    const uint64_t expected_p =
        w30 && expected_k == 0 ? 30 : job_count;
    CHECK(ack.recovery_prepared_prefix_p == expected_p);
    CHECK(ack.request.relationship_id == recovered.relationship_id);
    CHECK(ack.request.new_relationship_epoch >
          ack.request.old_relationship_epoch);
    std::printf("P51_D14 reset-boundary-smoke profile=%u jobs=%zu requested-K=%zu "
                "observed-K=%llu P=%llu old-epoch=%llu new-epoch=%llu "
                "held-worker=%u suffix-sent=%u reset-before-release=%u "
                "exact-results=%zu exact-attachments=1 final-ops=%zu "
                "final-raw=%llu scope=%s boundary=%zu\n",
                static_cast<unsigned>(profile), job_count, cut_boundary,
                static_cast<unsigned long long>(ack.request.settled_prefix_k),
                static_cast<unsigned long long>(ack.recovery_prepared_prefix_p),
                static_cast<unsigned long long>(ack.request.old_relationship_epoch),
                static_cast<unsigned long long>(ack.request.new_relationship_epoch),
                recovered.held_suffix_worker, recovered.suffix_bundles_sent,
                recovered.reset_before_release, job_count,
                recovered.final_source_operations,
                static_cast<unsigned long long>(recovered.final_source_raw_bytes),
                w30 ? "w30" : "smoke", cut_boundary);
}

void test_p51_d14_reset_boundary_smoke_all_profiles() {
    for (const ProfileId profile : {ProfileId::P29V1, ProfileId::ZSTD_TU,
                                    ProfileId::ZSTD_ROUTE})
        test_p51_d14_reset_boundary_smoke(profile);
}

void test_p51_d14_w30_k0_all_profiles() {
    for (const ProfileId profile : {ProfileId::P29V1, ProfileId::ZSTD_TU,
                                    ProfileId::ZSTD_ROUTE})
        test_p51_d14_reset_boundary_smoke(profile, 31, 0, true);
}

void test_p51_d14_w30_terminal_all_profiles() {
    for (const ProfileId profile : {ProfileId::P29V1, ProfileId::ZSTD_TU,
                                    ProfileId::ZSTD_ROUTE})
        test_p51_d14_reset_boundary_smoke(profile, 32, 31, true);
}

void test_p51_d14_w30_reset_boundary_matrix() {
    for (const ProfileId profile : {ProfileId::P29V1, ProfileId::ZSTD_TU,
                                    ProfileId::ZSTD_ROUTE}) {
        for (size_t boundary = 0; boundary <= 30; ++boundary)
            test_p51_d14_reset_boundary_smoke(profile, 31, boundary, true);
    }
}

void test_p51_d07_active_cancel_recovery(ProfileId profile,
                                         D07Scenario scenario =
                                             D07Scenario::ActiveCancel) {
    const bool interrupt_replay =
        scenario == D07Scenario::InterruptedReplay ||
        scenario == D07Scenario::PositiveRecoveryOwner;
    const bool positive_recovery_owner =
        scenario == D07Scenario::PositiveRecoveryOwner;
    const bool committed_attempt_replacement =
        scenario == D07Scenario::CommittedAttemptReplacement;
    uint32_t cache_profile = 0;
    switch (profile) {
    case ProfileId::P29V1: cache_profile = CACHE_PROFILE_P29V1; break;
    case ProfileId::ZSTD_TU: cache_profile = CACHE_PROFILE_ZSTD_TU; break;
    case ProfileId::ZSTD_ROUTE: cache_profile = CACHE_PROFILE_ZSTD_ROUTE; break;
    }
    StoreIdentityRoot c_root{};
    c_root.bytes[15] = 0x6a;
    const SidecarLaunchIdentity c_launch = test_sidecar_launch(c_root);
    StoreIdentityRoot f_root{};
    f_root.bytes[15] = 0x7a;
    const SidecarLaunchIdentity f_launch = test_sidecar_launch(f_root);
    uint16_t f_port = 0;
    const int listener = loopback_listener(f_port);
    CHECK(listener >= 0 && f_port != 0);

    std::mutex materialize_mutex;
    std::condition_variable materialize_changed;
    unsigned materialize_calls = 0;
    bool materialization_waiting = false;
    bool release_second_bundle = false;
    auto before_materialize = [&] {
        std::unique_lock lock(materialize_mutex);
        ++materialize_calls;
        if (materialize_calls != (positive_recovery_owner ? 1u : 2u))
            return;
        materialization_waiting = true;
        materialize_changed.notify_all();
        materialize_changed.wait(lock, [&] { return release_second_bundle; });
    };

    std::mutex retired_mutex;
    std::condition_variable retired_changed;
    std::vector<std::pair<Id128, bool>> retired_rows;
    struct MaterializedObservation {
        CStoreGuid c_guid{};
        uint64_t tu_seq = 0;
        uint64_t raw_bytes = 0;
        Digest128 raw_digest{};
        Digest128 transaction_digest{};
        bool payload_matches = false;
    };
    std::mutex materialized_mutex;
    std::vector<MaterializedObservation> materialized_inputs;
    std::vector<R2TxCommit> wire_receipts;
    std::vector<ResetAck> wire_reset_acks;
    std::mutex wire_observation_mutex;

    service::RuntimeConfig f_config = test_runtime_config();
    f_config.c_store_guid = f_launch.c_store_guid;
    f_config.f_store_guid = f_launch.f_store_guid;
    f_config.f_store_generation = f_launch.store_generation;
    f_config.sidecar_launch = f_launch;
    f_config.endpoint_caps.profile = profile;
    f_config.endpoint_caps.supported_profiles = profile_bit(profile);
    f_config.endpoint_caps.zstd.max_raw_bytes = 65536;
    f_config.max_pending_p51_source_reservations = 8;
    f_config.endpoint_config.input_job_state =
        [&](CStoreGuid c_guid, const TxBegin& begin, const TxCommit& commit,
            std::span<const uint8_t> bytes) {
            {
                std::lock_guard lock(materialized_mutex);
                materialized_inputs.push_back(MaterializedObservation{
                    c_guid, begin.tu_seq.value, begin.raw_bytes,
                    begin.raw_digest, commit.transaction_digest,
                    begin.raw_bytes == bytes.size() &&
                        begin.raw_digest == icecc::digest128(bytes)});
            }
            return InputJobState::Open;
        };
    std::mutex reset_ack_mutex;
    std::condition_variable reset_ack_changed;
    std::vector<std::pair<size_t, ResetAck>> reset_acks;
    std::vector<std::pair<size_t, ReceiptRow>> recovery_receipts;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    f_config.p51_reservation_retired_for_test =
        [&](Id128 id, bool marker_retired) {
            {
                std::lock_guard lock(retired_mutex);
                retired_rows.emplace_back(id, marker_retired);
            }
            retired_changed.notify_all();
        };
#endif
    service::SidecarRuntime f_runtime(std::move(f_config));

    service::RuntimeConfig c_config = test_runtime_config();
    c_config.c_store_guid = c_launch.c_store_guid;
    c_config.f_store_guid = c_launch.f_store_guid;
    c_config.f_store_generation = c_launch.store_generation;
    c_config.sidecar_launch = c_launch;
    c_config.endpoint_caps.profile = profile;
    c_config.endpoint_caps.supported_profiles = profile_bit(profile);
    c_config.endpoint_caps.zstd.max_raw_bytes = 65536;
    c_config.max_active_source_transfers = 4;
    c_config.max_active_p51_source_transfers = 8;
    c_config.max_pending_p51_source_operations = 8;
    c_config.max_aggregate_source_raw_bytes = 1024 * 1024;
    std::mutex first_bundle_mutex;
    std::condition_variable first_bundle_changed;
    std::vector<uint64_t> c_bundle_ordinals;
    c_config.after_r2_bundle_sent_for_test = [&](uint64_t ordinal) {
        {
            std::lock_guard lock(first_bundle_mutex);
            c_bundle_ordinals.push_back(ordinal);
        }
        first_bundle_changed.notify_all();
    };
    auto replay_bundle_calls = std::make_shared<std::atomic<size_t>>(0);
    auto interrupted_ordinal = std::make_shared<std::atomic<uint64_t>>(0);
    std::atomic<bool> first_recovered_positive{false};
    std::atomic<bool> cut_after_positive_recovery{false};
    std::atomic<bool> pre_replay_state_valid{false};
    std::atomic<uint64_t> pre_replay_ordinal{0};
    std::atomic<bool> pre_replay_cut_requested{false};
    const PrepareRequestKey first_request_key{3, 9101};
    std::mutex recovery_owner_mutex;
    std::vector<PrepareRequestKey> recovery_callers;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    if (interrupt_replay && !positive_recovery_owner) {
        c_config.disconnect_r2_after_bundle_for_test =
            [replay_bundle_calls, interrupted_ordinal](uint64_t ordinal) {
                const size_t call = replay_bundle_calls->fetch_add(
                    1, std::memory_order_acq_rel) + 1;
                // The first four callbacks are the original jobs. Interrupt
                // after the first survivor is sent during the first replay.
                if (call != 5)
                    return false;
                interrupted_ordinal->store(ordinal,
                    std::memory_order_release);
                return true;
            };
    }
    if (positive_recovery_owner) {
        c_config.before_r2_recovery_attempt_for_test = [&](PrepareRequestKey key) {
            std::lock_guard lock(recovery_owner_mutex);
            recovery_callers.push_back(key);
        };
        c_config.after_r2_recovery_receipt_settled_for_test =
            [&](PrepareRequestKey key, uint64_t ordinal) {
                if (key == first_request_key && ordinal == 1)
                    first_recovered_positive.store(true,
                        std::memory_order_release);
            };
        c_config.disconnect_r2_before_replay_bundle_for_test =
            [&](uint64_t ordinal, size_t retained_rows, bool reader_running,
                bool ack_pump_running) {
                if (pre_replay_cut_requested.exchange(
                        true, std::memory_order_acq_rel))
                    return false;
                pre_replay_ordinal.store(ordinal, std::memory_order_release);
                const bool positive = first_recovered_positive.load(
                    std::memory_order_acquire);
                pre_replay_state_valid.store(
                    ordinal == 2 && retained_rows == 2 &&
                    !reader_running && !ack_pump_running && positive,
                    std::memory_order_release);
                cut_after_positive_recovery.store(positive,
                    std::memory_order_release);
                return true;
            };
    }
#endif
    service::SidecarRuntime c_runtime(std::move(c_config));

    std::atomic<bool> stop_accepting{false};
    std::atomic<size_t> accepted_connections{0};
    std::atomic<int> probe_fd{-1};
    std::thread acceptor([&] {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::seconds(40);
        while (!stop_accepting.load(std::memory_order_acquire) &&
               accepted_connections.load(std::memory_order_acquire) < 8 &&
               std::chrono::steady_clock::now() < end) {
            pollfd ready{listener, POLLIN, 0};
            int polled;
            do {
                polled = ::poll(&ready, 1, 100);
            } while (polled < 0 && errno == EINTR);
            if (polled <= 0 || !(ready.revents & POLLIN))
                continue;
            sockaddr_storage peer{};
            socklen_t peer_size = sizeof(peer);
            int fd;
            do {
                fd = ::accept(listener,
                    reinterpret_cast<sockaddr*>(&peer), &peer_size);
            } while (fd < 0 && errno == EINTR);
            if (fd < 0)
                continue;
            std::unique_ptr<MsgChannel> channel(
                Service::createChannelAccepted(
                    fd, reinterpret_cast<sockaddr*>(&peer), peer_size));
            if (!channel)
                continue;
            const auto handshake_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            bool protocol_ready = true;
            while (channel->protocol_admission_state() ==
                       MsgChannel::ProtocolAdmissionState::Pending &&
                   std::chrono::steady_clock::now() < handshake_deadline) {
                short events = POLLIN;
                if (channel->has_pending_write())
                    events |= POLLOUT;
                pollfd socket{channel->fd, events, 0};
                int result;
                do {
                    result = ::poll(&socket, 1, 50);
                } while (result < 0 && errno == EINTR);
                if (result < 0 ||
                    (socket.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    protocol_ready = false;
                    break;
                }
                if ((socket.revents & POLLOUT) && !channel->flush_pending()) {
                    protocol_ready = false;
                    break;
                }
                if ((socket.revents & POLLIN) && !channel->read_a_bit()) {
                    protocol_ready = false;
                    break;
                }
            }
            if (!protocol_ready ||
                channel->protocol_admission_state() !=
                    MsgChannel::ProtocolAdmissionState::Ready ||
                !channel->finish_protocol_admission())
                continue;
            std::unique_ptr<Msg> link_request(
                channel->get_msg_until(handshake_deadline));
            if (dynamic_cast<P51CacheLinkSessionMsg*>(link_request.get()) ==
                nullptr)
                continue;
            const int adopted_fd =
                channel->send_p51_cache_link_session_ready_and_release(
                    handshake_deadline);
            if (adopted_fd < 0)
                continue;
            int duplicate = ::dup(adopted_fd);
            int expected = -1;
            if (duplicate >= 0 &&
                !probe_fd.compare_exchange_strong(
                    expected, duplicate, std::memory_order_acq_rel))
                (void)::close(duplicate);
            EndpointIoControl control;
            control.before_materialize_on_worker = before_materialize;
            control.outbound_message_observer =
                [&](ActorSide actor, const Message& message) {
                    if (actor != ActorSide::F)
                        return;
                    std::lock_guard lock(wire_observation_mutex);
                    if (const auto* commit = std::get_if<R2TxCommit>(&message))
                        wire_receipts.push_back(*commit);
                    else if (const auto* ack = std::get_if<ResetAck>(&message))
                        wire_reset_acks.push_back(*ack);
                };
            const size_t connection_number =
                accepted_connections.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (positive_recovery_owner && connection_number == 1)
                control.close_before_write = MessageType::R2_TX_COMMIT;
            if (interrupt_replay && connection_number >= 3) {
                control.outbound_message_observer =
                    [&, connection_number](ActorSide, const Message& message) {
                        const auto* ack = std::get_if<ResetAck>(&message);
                        if (!ack)
                            return;
                        {
                            std::lock_guard lock(reset_ack_mutex);
                            reset_acks.emplace_back(connection_number, *ack);
                        }
                        reset_ack_changed.notify_all();
                    };
            } else if (positive_recovery_owner && connection_number >= 2) {
                control.outbound_message_observer =
                    [&, connection_number](ActorSide, const Message& message) {
                        std::lock_guard lock(reset_ack_mutex);
                        if (const auto* receipt =
                                std::get_if<ReceiptRow>(&message))
                            recovery_receipts.emplace_back(
                                connection_number, *receipt);
                        if (const auto* ack = std::get_if<ResetAck>(&message))
                            reset_acks.emplace_back(connection_number, *ack);
                        reset_ack_changed.notify_all();
                    };
            }
            f_runtime.start_adopted_r2_endpoint(adopted_fd,
                                                 std::move(control));
        }
    });
    auto cleanup = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1), [&](int*) {
            {
                std::lock_guard lock(materialize_mutex);
                release_second_bundle = true;
            }
            materialize_changed.notify_all();
            stop_accepting.store(true, std::memory_order_release);
            (void)::shutdown(listener, SHUT_RDWR);
            if (acceptor.joinable())
                acceptor.join();
            (void)::close(listener);
            const int descriptor = probe_fd.exchange(
                -1, std::memory_order_acq_rel);
            if (descriptor >= 0)
                (void)::close(descriptor);
            c_runtime.stop();
            f_runtime.stop();
        });

    struct RequestRow {
        local::P51SourceTransferRequest request;
        RuntimeCase pair{local::Connection(-1), local::Connection(-1)};
        std::vector<uint8_t> bytes;
        uint64_t request_id = 0;
    };
    auto make_request = [&](uint64_t request_id, size_t raw_bytes,
                            uint8_t fill) {
        const auto source_deadline = positive_recovery_owner
            ? std::chrono::seconds(12) : std::chrono::seconds(30);
        auto reservation = test_p51_reservation_request(
            c_launch.c_store_guid, c_launch.store_generation,
            c_launch.identity.generation, c_launch.identity.attempt,
            request_id, cache_profile, 30,
            source_deadline);
        reservation.arm.source.assignment_nonce = request_id;
        reservation.arm.source.logical_job = 130000 + request_id;
        reservation.arm.source.compiler_attempt = 1;
        reservation.arm.source.selected_f_host = "127.0.0.1";
        reservation.arm.source.selected_f_cache_port = f_port;
        const auto armed = f_runtime.reserve_p51_source_on_owner(reservation);
        CHECK(armed.error_code == 0 && armed.armed.has_value());
        RequestRow row;
        row.request = local::P51SourceTransferRequest{
            *armed.armed, reservation.absolute_deadline};
        row.pair = authenticated_runtime_pair();
        row.bytes.assign(raw_bytes, fill);
        row.request_id = request_id;
        return row;
    };
    auto enqueue = [&](RequestRow& row) {
        const auto operation = local::make_p51_source_transfer_operation(
            c_launch.identity, row.request, row.request_id);
        return c_runtime.enqueue_p51_source_transfer(
            std::move(row.pair.sender), c_launch.identity, operation,
            sized_test_source_fd(row.bytes.size(), row.bytes.front())) ==
            service::P51SourceEnqueueResult::Accepted;
    };
    auto attach_exact = [&](RequestRow& row,
                            const local::P50SourceTransferResult& result,
                            const InputLeaseOwner& owner,
                            bool authorize_attachment) {
        if (result.code != local::SourceTransferResultCode::Committed ||
            !result.valid() || result.c_store_guid != c_launch.c_store_guid ||
            result.raw_bytes != row.bytes.size() ||
            result.raw_digest != icecc::digest128(row.bytes))
            return false;
        const InputFdRequest attach_request{
            f_launch.identity,
            InputRecordKey{result.c_store_guid, TuSeq{result.tu_seq}}, owner,
            row.request_id + 70000};
        auto cursor = f_runtime.attach_input_on_owner(
            attach_request,
            row.request.absolute_deadline.as_steady_time_point());
        bool exact = cursor.has_value() &&
                     cursor->remaining() == row.bytes.size() &&
                     cursor->raw_digest() == icecc::digest128(row.bytes);
        if (exact) {
            std::vector<uint8_t> actual(row.bytes.size());
            exact = cursor->read(actual) == actual.size() && actual == row.bytes;
        }
        f_runtime.finish_input_attachment_on_owner(
            attach_request, exact && authorize_attachment,
            row.request.absolute_deadline.as_steady_time_point());
        return exact;
    };
    auto attachment_is_denied = [&](RequestRow& row,
                                    const local::P50SourceTransferResult& result,
                                    const InputLeaseOwner& owner,
                                    uint64_t request_id) {
        if (result.code != local::SourceTransferResultCode::Committed ||
            !result.valid() || result.c_store_guid != c_launch.c_store_guid)
            return false;
        const InputFdRequest request{
            f_launch.identity,
            InputRecordKey{result.c_store_guid, TuSeq{result.tu_seq}},
            owner, request_id};
        auto cursor = f_runtime.attach_input_on_owner(
            request, row.request.absolute_deadline.as_steady_time_point());
        const bool denied = !cursor.has_value();
        if (cursor) {
            f_runtime.finish_input_attachment_on_owner(
                request, false,
                row.request.absolute_deadline.as_steady_time_point());
        }
        return denied;
    };
    auto owner_for = [](const RequestRow& row) {
        return InputLeaseOwner{
            row.request.armed.arm.source.logical_job,
            row.request.armed.arm.source.assignment_epoch,
            row.request.armed.arm.source.assignment_nonce};
    };

    RequestRow first = make_request(9101, 512, 0x31);
    RequestRow cancelled = make_request(9102, 1024, 0x42);
    RequestRow successor = make_request(9103, 2048, 0x53);
    std::optional<RequestRow> fourth;
    if (interrupt_replay)
        fourth.emplace(make_request(9104, 4096, 0x64));
    CHECK(enqueue(first));
    std::optional<local::P50SourceTransferResult> first_result;
    bool first_exact = false;
    std::chrono::steady_clock::time_point first_finished{};
    std::thread first_waiter;
    // The positive-owner scenario starts this waiter before several checked
    // preconditions below. If any CHECK throws before the normal join point,
    // open the materializer gate and join before the std::thread destructor;
    // otherwise that destructor calls std::terminate and hides the assertion.
    auto first_waiter_unwind = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1), [&](int*) {
            if (!first_waiter.joinable())
                return;
            {
                std::lock_guard lock(materialize_mutex);
                release_second_bundle = true;
            }
            materialize_changed.notify_all();
            first_waiter.join();
        });
    if (positive_recovery_owner) {
        // In the positive-owner case the first request must be the exact
        // request paused by materialize-call #1. Do not enqueue any sibling
        // until that sole outstanding request has reached F's worker gate
        // and its complete ordinal-1 bundle has been written on C's socket.
        bool first_materialization_waiting = false;
        {
            std::unique_lock lock(materialize_mutex);
            first_materialization_waiting = materialize_changed.wait_for(
                lock, std::chrono::seconds(8), [&] {
                    return materialization_waiting;
                });
        }
        bool first_bundle_sent = false;
        {
            std::unique_lock lock(first_bundle_mutex);
            first_bundle_sent = first_bundle_changed.wait_for(
                lock, std::chrono::seconds(8), [&] {
                    return std::find(c_bundle_ordinals.begin(),
                                     c_bundle_ordinals.end(), 1) !=
                           c_bundle_ordinals.end();
                });
        }
        CHECK(first_materialization_waiting);
        CHECK(first_bundle_sent);
        {
            std::lock_guard lock(first_bundle_mutex);
            CHECK(c_bundle_ordinals.size() == 1);
            CHECK(c_bundle_ordinals.front() == 1);
        }
        first_waiter = std::thread([&] {
            try {
                first_result = receive_p51_transfer_result(
                    first.pair.receiver, c_launch.identity, first.request_id,
                    first.request.absolute_deadline.as_steady_time_point(), true);
                first_exact = attach_exact(
                    first, *first_result, owner_for(first), true);
            } catch (...) {
                // Assert the absence/failure after joining the result waiter.
            }
            first_finished = std::chrono::steady_clock::now();
        });
    } else {
        first_result = receive_p51_transfer_result(
            first.pair.receiver, c_launch.identity, first.request_id,
            first.request.absolute_deadline.as_steady_time_point(), true);
        first_exact = attach_exact(
            first, *first_result, owner_for(first),
            !committed_attempt_replacement);
        first_finished = std::chrono::steady_clock::now();
    }
    const InputLeaseOwner first_owner = owner_for(first);
    bool old_owner_rejected_during_prepare = true;
    bool replacement_rejected_before_commit = true;
    bool old_owner_rejected_after_commit = true;
    bool replacement_exact_after_commit = true;
    bool lifecycle_prepare_retained = true;
    bool lifecycle_commit_retained = true;
    Digest128 first_materialized_transaction_digest{};
    InputLeaseOwner replacement_owner = first_owner;
    if (committed_attempt_replacement) {
        CHECK(first_result.has_value());
        CHECK(first_result->code == local::SourceTransferResultCode::Committed);
        replacement_owner.assignment_epoch += 1;
        replacement_owner.assignment_nonce += 1;
        const auto retirement_deadline =
            first.request.absolute_deadline.as_steady_time_point();
        const InputRecordKey first_key{
            first_result->c_store_guid, TuSeq{first_result->tu_seq}};
        const uint64_t retirement_id = first.request_id + 0x200000;
        auto lifecycle_request = [&](InputLifecycleAction action,
                                     uint64_t operation_id) {
            InputLifecycleRequest request;
            request.identity = f_launch.identity;
            request.key = first_key;
            request.owner = first_owner;
            request.operation_id = operation_id;
            request.action = action;
            request.f_store_generation = f_launch.store_generation;
            request.f_store_guid = f_launch.f_store_guid;
            request.immutable_size = first_result->raw_bytes;
            request.immutable_digest = first_result->raw_digest;
            request.retirement_id = retirement_id;
            request.absolute_deadline = first.request.absolute_deadline;
            request.deadline = retirement_deadline;
            if (action == InputLifecycleAction::CommitAttemptReplacement)
                request.replacement_owner = replacement_owner;
            return request;
        };
        const auto prepare = lifecycle_request(
            InputLifecycleAction::PrepareAttemptRetirement,
            first.request_id + 0x300000);
        const auto prepared = f_runtime.apply_input_lifecycle_on_owner(
            prepare, retirement_deadline);
        lifecycle_prepare_retained = prepared ==
            InputLifecycleApplyStatus::AttemptQuiescedRecordRetained;
        old_owner_rejected_during_prepare = attachment_is_denied(
            first, *first_result, first_owner, first.request_id + 71001);
        replacement_rejected_before_commit = attachment_is_denied(
            first, *first_result, replacement_owner, first.request_id + 71002);
        const auto commit_replacement = lifecycle_request(
            InputLifecycleAction::CommitAttemptReplacement,
            first.request_id + 0x300001);
        const auto committed_replacement =
            f_runtime.apply_input_lifecycle_on_owner(
                commit_replacement, retirement_deadline);
        lifecycle_commit_retained = committed_replacement ==
            InputLifecycleApplyStatus::ReplacementInstalledRecordRetained;
        old_owner_rejected_after_commit = attachment_is_denied(
            first, *first_result, first_owner, first.request_id + 71003);
    }

    CHECK(enqueue(cancelled));
    bool cancelled_predecessor_ready = false;
    if (positive_recovery_owner) {
        // The first F materializer is still held, so materialization_waiting
        // remains true from request 1 and cannot prove request 2 has reached
        // the wire. With four C workers, the successor could otherwise race
        // request 2 for relationship ordinal 2. Wait until request 2's full
        // bundle is observed before submitting its successor.
        std::unique_lock lock(first_bundle_mutex);
        cancelled_predecessor_ready = first_bundle_changed.wait_for(
            lock, std::chrono::seconds(8), [&] {
                return std::find(c_bundle_ordinals.begin(),
                                 c_bundle_ordinals.end(), 2) !=
                       c_bundle_ordinals.end();
            });
        if (cancelled_predecessor_ready) {
            CHECK(c_bundle_ordinals.size() == 2);
            CHECK(c_bundle_ordinals[0] == 1);
            CHECK(c_bundle_ordinals[1] == 2);
            std::fprintf(stderr,
                         "P51_D07 positive-owner predecessor-ordinal=2 "
                         "observed-before-successor=1\n");
            std::fflush(stderr);
        }
    } else {
        std::unique_lock lock(materialize_mutex);
        cancelled_predecessor_ready = materialize_changed.wait_for(
            lock, std::chrono::seconds(8), [&] {
                return materialization_waiting;
            });
    }
    CHECK(cancelled_predecessor_ready);
    CHECK(enqueue(successor));
    const bool successor_operation_active = wait_for_source_operation_count(
        c_runtime, positive_recovery_owner ? 3 : 2,
        std::chrono::seconds(3));
    JobBind observed_successor_binding{};
    JobBind observed_fourth_binding{};
    const auto probe_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(5);
    auto observe_buffered_bundle = [&](Id128 reservation_id,
                                       JobBind& observed) {
        while (std::chrono::steady_clock::now() < probe_deadline) {
            const int descriptor = probe_fd.load(std::memory_order_acquire);
            if (descriptor < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            std::array<uint8_t, 65536> buffered{};
            const ssize_t received = ::recv(descriptor, buffered.data(),
                                            buffered.size(),
                                            MSG_PEEK | MSG_DONTWAIT);
            if (received < 4) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            size_t offset = 0;
            bool target_binding_seen = false;
            bool target_end_seen = false;
            while (offset + 4 <= static_cast<size_t>(received)) {
                FrameHeader header;
                try {
                    header = decode_frame_header(std::span<const uint8_t>(
                        buffered.data() + offset, 4));
                } catch (...) {
                    break;
                }
                const size_t frame_bytes = 4 + header.payload_bytes;
                if (frame_bytes > static_cast<size_t>(received) - offset)
                    break;
                const std::span<const uint8_t> payload(
                    buffered.data() + offset + 4, header.payload_bytes);
                if (header.type == MessageType::JOB_BIND) {
                    const Message decoded = decode_payload(header.type, payload);
                    const JobBind binding = std::get<JobBind>(decoded);
                    if (binding.reservation_id == reservation_id) {
                        observed = binding;
                        target_binding_seen = true;
                    }
                } else if (header.type == MessageType::TU_END &&
                           target_binding_seen) {
                    const Message decoded = decode_payload(header.type, payload);
                    const TuEnd end = std::get<TuEnd>(decoded);
                    target_end_seen = end.relationship_ordinal ==
                                      observed.relationship_ordinal;
                }
                offset += frame_bytes;
            }
            if (target_binding_seen && target_end_seen)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    const bool successor_bundle_buffered = successor_operation_active &&
        observe_buffered_bundle(
            Id128{successor.request.armed.reservation_id},
            observed_successor_binding);
    // Queue job 4 only after observing job 3's complete original bundle. This
    // gives the replay-cut negative control a later, still-retained caller.
    if (fourth && successor_bundle_buffered)
        CHECK(enqueue(*fourth));
    const bool fourth_bundle_buffered = !fourth ||
        observe_buffered_bundle(Id128{fourth->request.armed.reservation_id},
                                observed_fourth_binding);
    const bool all_survivor_operations_active = wait_for_source_operation_count(
        c_runtime, (interrupt_replay ? 3 : 2) +
                       (positive_recovery_owner ? 1 : 0),
        std::chrono::seconds(3));
    const bool required_operations_active = successor_operation_active &&
                                            all_survivor_operations_active;
    CHECK(required_operations_active);
    std::fprintf(stderr,
                 "P51_D07 active-cancel precondition worker-materializing=1 "
                 "active-operations=%zu successor-full-bundle-buffered=%d "
                 "fourth-full-bundle-buffered=%d "
                 "successor-ordinal=%llu\n",
                 (interrupt_replay ? size_t{3} : size_t{2}) +
                     (positive_recovery_owner ? size_t{1} : size_t{0}),
                 successor_bundle_buffered ? 1 : 0,
                 fourth_bundle_buffered ? 1 : 0,
                 static_cast<unsigned long long>(
                     observed_successor_binding.relationship_ordinal));
    std::fflush(stderr);
    CHECK(successor_bundle_buffered);
    CHECK(fourth_bundle_buffered);
    CHECK(observed_successor_binding.source_request_id == successor.request_id);
    CHECK(observed_successor_binding.relationship_ordinal == 3);
    if (fourth) {
        CHECK(observed_fourth_binding.source_request_id == fourth->request_id);
        CHECK(observed_fourth_binding.relationship_ordinal == 4);
    }
    // The isolated C runtime submitted these jobs in observed wire order, so
    // their contiguous TU sequence makes the middle key exact without
    // guessing a globally arbitrary sequence number.
    const TuSeq cancelled_tu_seq{
        observed_successor_binding.tu_seq.value - 1};
    if (!positive_recovery_owner) {
        CHECK(first_result.has_value());
        CHECK(first_result->tu_seq <= std::numeric_limits<uint64_t>::max() -
              (fourth ? uint64_t{3} : uint64_t{2}));
        CHECK(observed_successor_binding.tu_seq.value ==
              first_result->tu_seq + 2);
        if (fourth)
            CHECK(observed_fourth_binding.tu_seq.value ==
                  first_result->tu_seq + 3);
    }
    const auto cancellation_time = std::chrono::steady_clock::now();
    const auto second_deadline =
        cancelled.request.absolute_deadline.as_steady_time_point();
    const auto successor_deadline =
        successor.request.absolute_deadline.as_steady_time_point();
    const bool f_cancelled = f_runtime.cancel_p51_source_on_owner(
        cancelled.request.armed.arm,
        cancelled.request.armed.reservation_id, second_deadline);
    const bool duplicate_f_cancelled = f_cancelled &&
        f_runtime.cancel_p51_source_on_owner(
            cancelled.request.armed.arm,
            cancelled.request.armed.reservation_id, second_deadline);
    {
        std::lock_guard lock(materialize_mutex);
        release_second_bundle = true;
    }
    materialize_changed.notify_all();

    struct TransferObservation {
        std::optional<local::P50SourceTransferResult> result;
        std::string error;
        std::chrono::steady_clock::time_point finished{};
        bool exact_attachment = false;
        bool cancelled_input_absent = false;
    } second_observation, successor_observation, fourth_observation;
    auto observe_result = [&](RequestRow& row,
                              std::chrono::steady_clock::time_point deadline,
                              TransferObservation& observation) {
        try {
            observation.result = receive_p51_transfer_result(
                row.pair.receiver, c_launch.identity, row.request_id,
                deadline, true);
            if (row.request_id == successor.request_id &&
                observation.result->code ==
                    local::SourceTransferResultCode::Committed)
                observation.exact_attachment =
                    attach_exact(row, *observation.result, owner_for(row), true);
            if (fourth && row.request_id == fourth->request_id &&
                observation.result->code ==
                    local::SourceTransferResultCode::Committed)
                observation.exact_attachment =
                    attach_exact(row, *observation.result, owner_for(row), true);
            if (row.request_id == cancelled.request_id &&
                observation.result->code ==
                    local::SourceTransferResultCode::Error) {
                const InputLeaseOwner owner{
                    cancelled.request.armed.arm.source.logical_job,
                    cancelled.request.armed.arm.source.assignment_epoch,
                    cancelled.request.armed.arm.source.assignment_nonce};
                const InputFdRequest request{
                    f_launch.identity,
                    InputRecordKey{c_launch.c_store_guid, cancelled_tu_seq},
                    owner, cancelled.request_id + 70000};
                auto cursor = f_runtime.attach_input_on_owner(request, deadline);
                observation.cancelled_input_absent = !cursor.has_value();
                if (cursor)
                    f_runtime.finish_input_attachment_on_owner(
                        request, false, deadline);
            }
        } catch (const std::exception& error) {
            observation.error = error.what();
        } catch (...) {
            observation.error = "unknown exception";
        }
        observation.finished = std::chrono::steady_clock::now();
    };
    std::fprintf(stderr,
                 "P51_D07 active-cancel waiting-results concurrently "
                 "original-deadline-remaining-ms=%lld/%lld\n",
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                     second_deadline - std::chrono::steady_clock::now()).count()),
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                     successor_deadline - std::chrono::steady_clock::now()).count()));
    std::fflush(stderr);
    std::thread second_waiter([&] {
        observe_result(cancelled, second_deadline, second_observation);
    });
    std::thread successor_waiter([&] {
        observe_result(successor, successor_deadline, successor_observation);
    });
    std::thread fourth_waiter;
    if (fourth) {
        fourth_waiter = std::thread([&] {
            observe_result(*fourth,
                fourth->request.absolute_deadline.as_steady_time_point(),
                fourth_observation);
        });
    }
    second_waiter.join();
    successor_waiter.join();
    if (fourth_waiter.joinable())
        fourth_waiter.join();
    if (first_waiter.joinable())
        first_waiter.join();
    const auto second_result = second_observation.result;
    const auto successor_result = successor_observation.result;
    bool exact_cancel_retirement = false;
    {
        std::unique_lock lock(retired_mutex);
        retired_changed.wait_for(lock, std::chrono::seconds(3), [&] {
            return std::any_of(retired_rows.begin(), retired_rows.end(),
                [&](const auto& row) {
                    return row.first ==
                        Id128{cancelled.request.armed.reservation_id};
                });
        });
        const size_t matching = static_cast<size_t>(std::count_if(
            retired_rows.begin(), retired_rows.end(), [&](const auto& row) {
                return row.first ==
                    Id128{cancelled.request.armed.reservation_id};
            }));
        exact_cancel_retirement = matching == 1;
    }
    const bool second_committed = second_result.has_value() &&
        second_result->code == local::SourceTransferResultCode::Committed;
    const bool successor_committed = successor_result.has_value() &&
        successor_result->code == local::SourceTransferResultCode::Committed;
    const bool successor_exact = successor_committed &&
        successor_observation.exact_attachment;
    const bool first_committed = first_result.has_value() &&
        first_result->code == local::SourceTransferResultCode::Committed;
    if (committed_attempt_replacement && successor_committed)
        replacement_exact_after_commit = attach_exact(
            first, *first_result, replacement_owner, true);
    const auto fourth_result = fourth_observation.result;
    const bool fourth_committed = fourth_result.has_value() &&
        fourth_result->code == local::SourceTransferResultCode::Committed;
    const bool fourth_exact = fourth && fourth_committed &&
        fourth_observation.exact_attachment;
    const bool cancelled_input_absent =
        second_observation.cancelled_input_absent;
    const size_t expected_links = interrupt_replay ? 3 : 2;
    const bool exact_link_count =
        accepted_connections.load(std::memory_order_acquire) == expected_links;
    const bool original_deadlines_live = cancellation_time < second_deadline &&
                                         cancellation_time < successor_deadline;
    const bool all_operations_released = wait_for_source_operation_count(
        c_runtime, 0, std::chrono::seconds(3));
    const bool all_raw_credits_released = wait_for_source_raw_bytes(
        c_runtime, 0, std::chrono::seconds(3));
    size_t exact_materializations = 0;
    size_t exact_wire_receipts = 0;
    size_t matching_reset_acks = 0;
    bool reset_ack_exact = false;
    if (committed_attempt_replacement || positive_recovery_owner) {
        CHECK(first_result.has_value());
        std::lock_guard lock(materialized_mutex);
        const auto matches_first = [&](const MaterializedObservation& observed) {
            return observed.c_guid == first_result->c_store_guid &&
                   observed.tu_seq == first_result->tu_seq &&
                   observed.raw_bytes == first_result->raw_bytes &&
                   observed.raw_digest == first_result->raw_digest &&
                   observed.payload_matches;
        };
        const auto first_materialization = std::find_if(
            materialized_inputs.begin(), materialized_inputs.end(),
            matches_first);
        if (first_materialization != materialized_inputs.end())
            first_materialized_transaction_digest =
                first_materialization->transaction_digest;
        exact_materializations = static_cast<size_t>(std::count_if(
            materialized_inputs.begin(), materialized_inputs.end(),
            matches_first));
    }
    PrepareRequestKey first_recovery_owner{};
    {
        std::lock_guard lock(recovery_owner_mutex);
        if (!recovery_callers.empty())
            first_recovery_owner = recovery_callers.front();
    }
    bool recovered_first_receipt_row = false;
    size_t recovered_first_receipt_count = 0;
    bool first_reset_ack_exact = false;
    bool post_cut_reset_ack_exact = false;
    {
        std::lock_guard lock(reset_ack_mutex);
        const auto matches_first_receipt = [&](const auto& row) {
            return row.first == 2 && row.second.relationship_id.bytes ==
                       first.request.armed.logical_relationship_id &&
                   first_result.has_value() &&
                   row.second.receipt.relationship_ordinal == 1 &&
                   row.second.receipt.inner.tu_seq.value ==
                       first_result->tu_seq &&
                   row.second.receipt.inner.raw_digest ==
                       first_result->raw_digest &&
                   row.second.receipt.inner.transaction_digest ==
                       first_materialized_transaction_digest;
        };
        recovered_first_receipt_row = std::any_of(
            recovery_receipts.begin(), recovery_receipts.end(),
            matches_first_receipt);
        recovered_first_receipt_count = static_cast<size_t>(std::count_if(
            recovery_receipts.begin(), recovery_receipts.end(),
            matches_first_receipt));
        first_reset_ack_exact = std::any_of(
            reset_acks.begin(), reset_acks.end(), [&](const auto& item) {
                return item.first == 2 &&
                       item.second.request.relationship_id.bytes ==
                           first.request.armed.logical_relationship_id &&
                       item.second.recovery_verified_floor_a == 0 &&
                       item.second.request.settled_prefix_k == 1 &&
                       item.second.recovery_prepared_prefix_p == 4 &&
                       item.second.unavailable_suffix_mask == 1 &&
                       item.second.recovery_witness_digest != Digest128{};
            });
        post_cut_reset_ack_exact = std::any_of(
            reset_acks.begin(), reset_acks.end(), [&](const auto& item) {
                return item.first == 3 &&
                       item.second.request.relationship_id.bytes ==
                           first.request.armed.logical_relationship_id &&
                       item.second.recovery_verified_floor_a == 1 &&
                       item.second.request.settled_prefix_k == 1 &&
                       item.second.recovery_prepared_prefix_p == 1 &&
                       item.second.unavailable_suffix_mask == 0 &&
                       item.second.recovery_witness_digest != Digest128{};
            });
    }
    if (committed_attempt_replacement) {
        std::lock_guard lock(wire_observation_mutex);
        exact_wire_receipts = static_cast<size_t>(std::count_if(
            wire_receipts.begin(), wire_receipts.end(),
            [&](const R2TxCommit& receipt) {
                return first_result.has_value() &&
                       receipt.inner.tu_seq.value == first_result->tu_seq &&
                       receipt.inner.raw_digest == first_result->raw_digest &&
                       receipt.relationship_ordinal == 1 &&
                       receipt.inner.transaction_digest ==
                           first_materialized_transaction_digest;
            }));
        const auto matches_reset = [&](const ResetAck& candidate) {
            return candidate.request.relationship_id.bytes ==
                       first.request.armed.logical_relationship_id &&
                   candidate.request.settled_prefix_k >= 1;
        };
        matching_reset_acks = static_cast<size_t>(std::count_if(
            wire_reset_acks.begin(), wire_reset_acks.end(), matches_reset));
        const auto ack = std::find_if(
            wire_reset_acks.begin(), wire_reset_acks.end(), matches_reset);
        if (ack != wire_reset_acks.end())
            reset_ack_exact = ack->recovery_verified_floor_a >= 1 &&
                ack->recovery_prepared_prefix_p >=
                    ack->request.settled_prefix_k &&
                ack->recovery_witness_digest != Digest128{};
    }
    std::printf(
        "P51_D07 active-cancel profile=%u stage=F-materialization after-full-TU "
        "replay-interrupt=%d "
        "successor-full-bundle-buffered=1 accepted-links=%zu cancel=%d "
        "exact-link-count=%d duplicate-cancel=%d second-received=%d second-code=%u "
        "second-error=%u successor-received=%d successor-code=%u "
        "successor-error=%u successor-exact=%d fourth-received=%d fourth-code=%u "
        "fourth-exact=%d cancelled-input-absent=%d "
        "first-positive-recovered=%d positive-cut=%d pre-replay-valid=%d "
        "pre-replay-ordinal=%llu first-result=%d first-exact=%d "
        "recovery-owner=%llu/%llu recovered-first-receipt=%d "
        "recovered-first-receipt-count=%zu exact-materializations=%zu "
        "first-reset-ack-exact=%d post-cut-reset-ack-exact=%d "
        "exact-cancel-retirement=%d "
        "finished-after-cancel-ms=%lld/%lld "
        "deadline-remains-ms=%lld/%lld C-operations-released=%d "
        "C-raw-credit-released=%d committed-attempt-replacement=%d "
        "prepare-retained=%d "
        "commit-retained=%d old-denied-prep=%d successor-denied-precommit=%d "
        "old-denied-commit=%d replacement-exact-after-commit=%d "
        "exact-wire-receipts=%zu reset-ack-count=%zu "
        "reset-ack-exact=%d\n",
        static_cast<unsigned>(profile),
        interrupt_replay ? 1 : 0,
        accepted_connections.load(std::memory_order_acquire),
        f_cancelled ? 1 : 0, exact_link_count ? 1 : 0,
        duplicate_f_cancelled ? 1 : 0,
        second_result.has_value() ? 1 : 0,
        second_result.has_value()
            ? static_cast<unsigned>(second_result->code) : 0,
        second_result.has_value()
            ? static_cast<unsigned>(second_result->error_code) : 0,
        successor_result.has_value() ? 1 : 0,
        successor_result.has_value()
            ? static_cast<unsigned>(successor_result->code) : 0,
        successor_result.has_value()
            ? static_cast<unsigned>(successor_result->error_code) : 0,
        successor_exact ? 1 : 0,
        fourth_result.has_value() ? 1 : 0,
        fourth_result.has_value()
            ? static_cast<unsigned>(fourth_result->code) : 0,
        fourth_exact ? 1 : 0,
        cancelled_input_absent ? 1 : 0,
        first_recovered_positive.load(std::memory_order_acquire) ? 1 : 0,
        cut_after_positive_recovery.load(std::memory_order_acquire) ? 1 : 0,
        pre_replay_state_valid.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(
            pre_replay_ordinal.load(std::memory_order_acquire)),
        first_result.has_value() ? 1 : 0,
        first_exact ? 1 : 0,
        static_cast<unsigned long long>(first_recovery_owner.producer_session),
        static_cast<unsigned long long>(first_recovery_owner.request_token),
        recovered_first_receipt_row ? 1 : 0,
        recovered_first_receipt_count, exact_materializations,
        first_reset_ack_exact ? 1 : 0,
        post_cut_reset_ack_exact ? 1 : 0,
        exact_cancel_retirement ? 1 : 0,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            second_observation.finished - cancellation_time).count()),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            successor_observation.finished - cancellation_time).count()),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            second_deadline - cancellation_time).count()),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            successor_deadline - cancellation_time).count()),
        all_operations_released ? 1 : 0, all_raw_credits_released ? 1 : 0,
        committed_attempt_replacement ? 1 : 0,
        lifecycle_prepare_retained ? 1 : 0,
        lifecycle_commit_retained ? 1 : 0,
        old_owner_rejected_during_prepare ? 1 : 0,
        replacement_rejected_before_commit ? 1 : 0,
        old_owner_rejected_after_commit ? 1 : 0,
        replacement_exact_after_commit ? 1 : 0,
        exact_wire_receipts, matching_reset_acks,
        reset_ack_exact ? 1 : 0);
    CHECK(first_exact);
    CHECK(f_cancelled);
    CHECK(duplicate_f_cancelled == !positive_recovery_owner);
    CHECK(exact_cancel_retirement);
    CHECK(exact_link_count);
    CHECK(second_result.has_value());
    CHECK(second_result->code == local::SourceTransferResultCode::Error);
    CHECK(second_result->error_code != 0);
    CHECK(cancelled_input_absent);
    CHECK(original_deadlines_live);
    CHECK(!second_committed);
    CHECK(successor_result.has_value());
    CHECK(successor_committed);
    CHECK(successor_exact);
    CHECK(second_observation.finished <= second_deadline);
    CHECK(successor_observation.finished <= successor_deadline);
    if (positive_recovery_owner) {
        CHECK(first_committed);
        CHECK(first_finished <=
              first.request.absolute_deadline.as_steady_time_point());
        CHECK(first_recovered_positive.load(std::memory_order_acquire));
        CHECK(cut_after_positive_recovery.load(std::memory_order_acquire));
        CHECK(pre_replay_state_valid.load(std::memory_order_acquire));
        CHECK(pre_replay_ordinal.load(std::memory_order_acquire) == 2);
        CHECK(first_recovery_owner == first_request_key);
        CHECK(recovered_first_receipt_row);
        CHECK(recovered_first_receipt_count == 1);
        CHECK(exact_materializations == 1);
        CHECK(first_reset_ack_exact);
        CHECK(post_cut_reset_ack_exact);
    } else if (interrupt_replay) {
        CHECK(replay_bundle_calls->load(std::memory_order_acquire) >= 5);
        CHECK(interrupted_ordinal->load(std::memory_order_acquire) == 2);
        std::unique_lock lock(reset_ack_mutex);
        const bool observed_third_reset = reset_ack_changed.wait_for(
            lock, std::chrono::seconds(2), [&] {
                return std::any_of(reset_acks.begin(), reset_acks.end(),
                    [](const auto& item) { return item.first == 3; });
            });
        CHECK(observed_third_reset);
        const auto ack = std::find_if(reset_acks.begin(), reset_acks.end(),
            [](const auto& item) { return item.first == 3; });
        CHECK(ack != reset_acks.end());
        if (ack != reset_acks.end()) {
            std::fprintf(stderr,
                "P51_D07 replay-cut cut-callbacks=%zu cut-ordinal=%llu "
                "third-reset-ack A=%llu K=%llu P=%llu unavailable-mask=%u\n",
                replay_bundle_calls->load(std::memory_order_acquire),
                static_cast<unsigned long long>(
                    interrupted_ordinal->load(std::memory_order_acquire)),
                static_cast<unsigned long long>(
                    ack->second.recovery_verified_floor_a),
                static_cast<unsigned long long>(
                    ack->second.request.settled_prefix_k),
                static_cast<unsigned long long>(
                    ack->second.recovery_prepared_prefix_p),
                ack->second.unavailable_suffix_mask);
            std::fflush(stderr);
            CHECK(ack->second.recovery_verified_floor_a == 1);
            CHECK(ack->second.recovery_prepared_prefix_p == 2);
            CHECK(ack->second.request.settled_prefix_k == 1 ||
                  ack->second.request.settled_prefix_k == 2);
        }
    }
    CHECK(all_operations_released);
    CHECK(all_raw_credits_released);
    if (committed_attempt_replacement) {
        CHECK(first_result.has_value());
        CHECK(first_result->code == local::SourceTransferResultCode::Committed);
        CHECK(lifecycle_prepare_retained);
        CHECK(old_owner_rejected_during_prepare);
        CHECK(replacement_rejected_before_commit);
        CHECK(lifecycle_commit_retained);
        CHECK(old_owner_rejected_after_commit);
        CHECK(successor_committed);
        CHECK(replacement_exact_after_commit);
        CHECK(exact_materializations == 1);
        CHECK(exact_wire_receipts == 1);
        CHECK(matching_reset_acks >= 1);
        CHECK(reset_ack_exact);
    }
    if (fourth) {
        CHECK(fourth_result.has_value());
        CHECK(fourth_committed);
        CHECK(fourth_exact);
        CHECK(fourth_observation.finished <=
              fourth->request.absolute_deadline.as_steady_time_point());
    }
}

void test_p51_d07_active_cancel_all_profiles() {
    for (const ProfileId profile : {
             ProfileId::P29V1, ProfileId::ZSTD_TU, ProfileId::ZSTD_ROUTE})
        test_p51_d07_active_cancel_recovery(profile);
}

void test_p51_d07_active_cancel_replay_interrupt_all_profiles() {
    for (const ProfileId profile : {
             ProfileId::P29V1, ProfileId::ZSTD_TU, ProfileId::ZSTD_ROUTE})
        test_p51_d07_active_cancel_recovery(
            profile, D07Scenario::InterruptedReplay);
}

void test_p51_d07_positive_recovery_owner_all_profiles() {
    for (const ProfileId profile : {
             ProfileId::P29V1, ProfileId::ZSTD_TU, ProfileId::ZSTD_ROUTE})
        test_p51_d07_active_cancel_recovery(
            profile, D07Scenario::PositiveRecoveryOwner);
}

void test_p51_d07_committed_attempt_replacement_all_profiles() {
    for (const ProfileId profile : {
             ProfileId::P29V1, ProfileId::ZSTD_TU, ProfileId::ZSTD_ROUTE})
        test_p51_d07_active_cancel_recovery(
            profile, D07Scenario::CommittedAttemptReplacement);
}

void test_p51_d17_repeated_window_cancel(ProfileId profile,
                                         size_t cycle_count) {
    constexpr size_t kWindow = 30;
    CHECK(cycle_count > 0 && cycle_count <= 3);
    uint32_t cache_profile = 0;
    const char* profile_name = "unknown";
    switch (profile) {
    case ProfileId::P29V1:
        cache_profile = CACHE_PROFILE_P29V1;
        profile_name = "P29V1";
        break;
    case ProfileId::ZSTD_TU:
        cache_profile = CACHE_PROFILE_ZSTD_TU;
        profile_name = "ZSTD_TU";
        break;
    case ProfileId::ZSTD_ROUTE:
        cache_profile = CACHE_PROFILE_ZSTD_ROUTE;
        profile_name = "ZSTD_ROUTE";
        break;
    }

    StoreIdentityRoot c_root{};
    c_root.bytes[15] = 0x37;
    const SidecarLaunchIdentity c_launch = test_sidecar_launch(c_root);
    StoreIdentityRoot f_root{};
    f_root.bytes[15] = 0x47;
    const SidecarLaunchIdentity f_launch = test_sidecar_launch(f_root);
    uint16_t f_port = 0;
    const int listener = loopback_listener(f_port);
    CHECK(listener >= 0 && f_port != 0);

    std::mutex materialize_mutex;
    std::condition_variable materialize_changed;
    bool gate_armed = false;
    bool gate_entered = false;
    bool release_gate = true;
    size_t gate_waiters = 0;
    std::atomic<size_t> materialize_calls{0};
    std::mutex materialized_mutex;
    struct MaterializedInput {
        uint64_t raw_bytes = 0;
        Digest128 raw_digest{};
        bool bytes_match_digest = false;
    };
    std::vector<MaterializedInput> materialized_inputs;
    std::mutex ack_mutex;
    std::vector<ResetAck> reset_acks;
    std::mutex sent_mutex;
    std::condition_variable sent_changed;
    std::vector<uint64_t> sent_ordinals;
    std::mutex retired_mutex;
    std::vector<Id128> retired_reservations;

    service::RuntimeConfig f_config = test_runtime_config();
    f_config.c_store_guid = f_launch.c_store_guid;
    f_config.f_store_guid = f_launch.f_store_guid;
    f_config.f_store_generation = f_launch.store_generation;
    f_config.sidecar_launch = f_launch;
    f_config.endpoint_caps.profile = profile;
    f_config.endpoint_caps.supported_profiles = profile_bit(profile);
    f_config.endpoint_caps.zstd.max_raw_bytes = 65536;
    f_config.max_pending_p51_source_reservations = 128;
    f_config.endpoint_config.input_job_state =
        [&](CStoreGuid, const TxBegin& begin, const TxCommit&,
            std::span<const uint8_t> bytes) {
            {
                std::lock_guard lock(materialized_mutex);
                materialized_inputs.push_back(MaterializedInput{
                    begin.raw_bytes, begin.raw_digest,
                    begin.raw_bytes == bytes.size() &&
                        begin.raw_digest == icecc::digest128(bytes)});
            }
            return InputJobState::Open;
        };
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    f_config.p51_reservation_retired_for_test =
        [&](Id128 id, bool) {
            std::lock_guard lock(retired_mutex);
            retired_reservations.push_back(id);
        };
#endif
    service::SidecarRuntime f_runtime(std::move(f_config));

    service::RuntimeConfig c_config = test_runtime_config();
    c_config.c_store_guid = c_launch.c_store_guid;
    c_config.f_store_guid = c_launch.f_store_guid;
    c_config.f_store_generation = c_launch.store_generation;
    c_config.sidecar_launch = c_launch;
    c_config.endpoint_caps.profile = profile;
    c_config.endpoint_caps.supported_profiles = profile_bit(profile);
    c_config.endpoint_caps.zstd.max_raw_bytes = 65536;
    c_config.max_active_source_transfers = 4;
    c_config.max_active_p51_source_transfers = 128;
    c_config.max_pending_p51_source_operations = 128;
    c_config.max_aggregate_source_raw_bytes = 1024 * 1024;
    c_config.after_r2_bundle_sent_for_test = [&](uint64_t ordinal) {
        {
            std::lock_guard lock(sent_mutex);
            sent_ordinals.push_back(ordinal);
        }
        sent_changed.notify_all();
    };
    service::SidecarRuntime c_runtime(std::move(c_config));

    std::atomic<bool> stop_accepting{false};
    std::atomic<size_t> accepted_connections{0};
    std::atomic<int> probe_fd{-1};
    std::thread acceptor([&] {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::seconds(150);
        while (!stop_accepting.load(std::memory_order_acquire) &&
               accepted_connections.load(std::memory_order_acquire) < 8 &&
               std::chrono::steady_clock::now() < end) {
            pollfd ready{listener, POLLIN, 0};
            int polled;
            do {
                polled = ::poll(&ready, 1, 100);
            } while (polled < 0 && errno == EINTR);
            if (polled <= 0 || (ready.revents & POLLIN) == 0)
                continue;
            sockaddr_storage peer{};
            socklen_t peer_size = sizeof(peer);
            int fd;
            do {
                fd = ::accept(listener,
                    reinterpret_cast<sockaddr*>(&peer), &peer_size);
            } while (fd < 0 && errno == EINTR);
            if (fd < 0)
                continue;
            std::unique_ptr<MsgChannel> channel(
                Service::createChannelAccepted(
                    fd, reinterpret_cast<sockaddr*>(&peer), peer_size));
            if (!channel)
                continue;
            const auto handshake_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            bool protocol_ready = true;
            while (channel->protocol_admission_state() ==
                       MsgChannel::ProtocolAdmissionState::Pending &&
                   std::chrono::steady_clock::now() < handshake_deadline) {
                short events = POLLIN;
                if (channel->has_pending_write())
                    events = static_cast<short>(events | POLLOUT);
                pollfd socket{channel->fd, events, 0};
                int result;
                do {
                    result = ::poll(&socket, 1, 50);
                } while (result < 0 && errno == EINTR);
                if (result < 0 ||
                    (socket.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    protocol_ready = false;
                    break;
                }
                if ((socket.revents & POLLOUT) && !channel->flush_pending()) {
                    protocol_ready = false;
                    break;
                }
                if ((socket.revents & POLLIN) && !channel->read_a_bit()) {
                    protocol_ready = false;
                    break;
                }
            }
            if (!protocol_ready ||
                channel->protocol_admission_state() !=
                    MsgChannel::ProtocolAdmissionState::Ready ||
                !channel->finish_protocol_admission())
                continue;
            std::unique_ptr<Msg> link_request(
                channel->get_msg_until(handshake_deadline));
            if (dynamic_cast<P51CacheLinkSessionMsg*>(link_request.get()) ==
                nullptr)
                continue;
            const int adopted_fd =
                channel->send_p51_cache_link_session_ready_and_release(
                    handshake_deadline);
            if (adopted_fd < 0)
                continue;
            const int duplicate = ::dup(adopted_fd);
            if (duplicate < 0) {
                (void)::close(adopted_fd);
                continue;
            }
            const int old_probe = probe_fd.exchange(
                duplicate, std::memory_order_acq_rel);
            if (old_probe >= 0)
                (void)::close(old_probe);
            EndpointIoControl control;
            control.before_materialize_on_worker = [&] {
                materialize_calls.fetch_add(1, std::memory_order_relaxed);
                std::unique_lock lock(materialize_mutex);
                if (!gate_armed)
                    return;
                ++gate_waiters;
                gate_entered = true;
                materialize_changed.notify_all();
                materialize_changed.wait(lock, [&] { return release_gate; });
                --gate_waiters;
            };
            control.outbound_message_observer =
                [&](ActorSide actor, const Message& message) {
                    if (actor != ActorSide::F)
                        return;
                    if (const auto* ack = std::get_if<ResetAck>(&message)) {
                        std::lock_guard lock(ack_mutex);
                        reset_acks.push_back(*ack);
                    }
                };
            accepted_connections.fetch_add(1, std::memory_order_acq_rel);
            f_runtime.start_adopted_r2_endpoint(
                adopted_fd, std::move(control));
        }
    });
    auto cleanup = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1), [&](int*) {
            {
                std::lock_guard lock(materialize_mutex);
                gate_armed = false;
                release_gate = true;
            }
            materialize_changed.notify_all();
            stop_accepting.store(true, std::memory_order_release);
            (void)::shutdown(listener, SHUT_RDWR);
            if (acceptor.joinable())
                acceptor.join();
            (void)::close(listener);
            const int descriptor = probe_fd.exchange(
                -1, std::memory_order_acq_rel);
            if (descriptor >= 0)
                (void)::close(descriptor);
            c_runtime.stop();
            f_runtime.stop();
        });

    struct RequestRow {
        local::P51SourceTransferRequest request;
        RuntimeCase pair{local::Connection(-1), local::Connection(-1)};
        std::vector<uint8_t> bytes;
        uint64_t request_id = 0;
    };
    auto make_request = [&](uint64_t request_id, size_t raw_bytes,
                            uint8_t fill) {
        auto reservation = test_p51_reservation_request(
            c_launch.c_store_guid, c_launch.store_generation,
            c_launch.identity.generation, c_launch.identity.attempt,
            request_id, cache_profile, 30, std::chrono::seconds(30));
        reservation.arm.source.assignment_nonce = request_id;
        reservation.arm.source.logical_job = 500000 + request_id;
        reservation.arm.source.compiler_attempt = 1;
        reservation.arm.source.selected_f_host = "127.0.0.1";
        reservation.arm.source.selected_f_cache_port = f_port;
        const auto armed = f_runtime.reserve_p51_source_on_owner(reservation);
        CHECK(armed.error_code == 0 && armed.armed.has_value());
        RequestRow row;
        row.request = local::P51SourceTransferRequest{
            *armed.armed, reservation.absolute_deadline};
        row.pair = authenticated_runtime_pair();
        row.bytes.assign(raw_bytes, fill);
        row.request_id = request_id;
        return row;
    };
    auto enqueue = [&](RequestRow& row) {
        const auto operation = local::make_p51_source_transfer_operation(
            c_launch.identity, row.request, row.request_id);
        return c_runtime.enqueue_p51_source_transfer(
            std::move(row.pair.sender), c_launch.identity, operation,
            sized_test_source_fd(row.bytes.size(), row.bytes.front())) ==
            service::P51SourceEnqueueResult::Accepted;
    };
    auto attach_exact = [&](RequestRow& row,
                            const local::P50SourceTransferResult& result) {
        if (result.code != local::SourceTransferResultCode::Committed ||
            !result.valid() || result.c_store_guid != c_launch.c_store_guid ||
            result.raw_bytes != row.bytes.size() ||
            result.raw_digest != icecc::digest128(row.bytes))
            return false;
        const InputLeaseOwner owner{
            row.request.armed.arm.source.logical_job,
            row.request.armed.arm.source.assignment_epoch,
            row.request.armed.arm.source.assignment_nonce};
        const InputFdRequest request{
            f_launch.identity,
            InputRecordKey{result.c_store_guid, TuSeq{result.tu_seq}}, owner,
            row.request_id + 900000};
        const auto deadline =
            row.request.absolute_deadline.as_steady_time_point();
        auto cursor = f_runtime.attach_input_on_owner(request, deadline);
        bool exact = cursor.has_value() &&
                     cursor->remaining() == row.bytes.size() &&
                     cursor->raw_digest() == icecc::digest128(row.bytes);
        if (exact) {
            std::vector<uint8_t> actual(row.bytes.size());
            exact = cursor->read(actual) == actual.size() && actual == row.bytes;
        }
        f_runtime.finish_input_attachment_on_owner(request, exact, deadline);
        return exact;
    };

    auto run_fresh = [&](uint64_t request_id, uint8_t fill) {
        RequestRow fresh = make_request(request_id, 73, fill);
        CHECK(enqueue(fresh));
        const auto result = receive_p51_transfer_result(
            fresh.pair.receiver, c_launch.identity, fresh.request_id,
            fresh.request.absolute_deadline.as_steady_time_point(), true);
        if (result.code != local::SourceTransferResultCode::Committed) {
            std::fprintf(stderr,
                         "P51_D17 fresh-failed request=%llu code=%u error=%u "
                         "attempts=%u accepted-links=%zu active=%zu raw=%llu\n",
                         static_cast<unsigned long long>(fresh.request_id),
                         static_cast<unsigned>(result.code), result.error_code,
                         static_cast<unsigned>(result.attempts),
                         accepted_connections.load(std::memory_order_acquire),
                         c_runtime.pending_p51_source_operations_for_test(),
                         static_cast<unsigned long long>(
                             c_runtime.active_source_raw_bytes_for_test()));
            std::fflush(stderr);
        }
        CHECK(result.code == local::SourceTransferResultCode::Committed);
        CHECK(attach_exact(fresh, result));
    };

    // Warm the persistent relationship and establish a post-start descriptor
    // baseline; intentionally retained dictionary/history state is outside the
    // live-operation and FD leak checks below.
    run_fresh(490001, 0x21);
    CHECK(wait_for_source_operation_count(c_runtime, 0, std::chrono::seconds(2)));
    CHECK(wait_for_source_raw_bytes(c_runtime, 0, std::chrono::seconds(2)));
    {
        std::lock_guard lock(sent_mutex);
        sent_ordinals.clear();
    }
    const size_t warmed_fd_baseline = process_open_fd_count();
    const std::array<size_t, 3> cancel_positions{0, 14, 29};
    uint64_t confirmed_prefix = 1;

    for (size_t cycle = 0; cycle < cycle_count; ++cycle) {
        const size_t cancel_index = cancel_positions[cycle];
        const uint64_t request_base = 500000 + cycle * 100;
        {
            std::lock_guard lock(materialize_mutex);
            gate_entered = false;
            gate_waiters = 0;
            release_gate = false;
            gate_armed = true;
        }
        std::vector<RequestRow> cohort;
        cohort.reserve(kWindow);
        for (size_t index = 0; index < kWindow; ++index) {
            cohort.push_back(make_request(
                request_base + index,
                32 + (index % 7),
                static_cast<uint8_t>(0x20 + cycle * 31 + index)));
        }
        bool all_enqueued = true;
        for (auto& row : cohort)
            all_enqueued = enqueue(row) && all_enqueued;
        uint64_t expected_active_raw_bytes = 0;
        for (const auto& row : cohort)
            expected_active_raw_bytes += row.bytes.size();

        bool gate_waited = false;
        {
            std::unique_lock lock(materialize_mutex);
            gate_waited = materialize_changed.wait_for(
                lock, std::chrono::seconds(5), [&] { return gate_entered; });
        }
        const bool all_active = all_enqueued && gate_waited &&
            wait_for_source_operation_count(
                c_runtime, kWindow, std::chrono::seconds(5)) &&
            wait_for_source_raw_bytes(
                c_runtime, expected_active_raw_bytes,
                std::chrono::seconds(5));
        bool all_sent = false;
        {
            std::unique_lock lock(sent_mutex);
            const auto sent_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(5);
            all_sent = sent_changed.wait_until(lock, sent_deadline, [&] {
                return sent_ordinals.size() >= kWindow;
            });
        }
        std::vector<uint64_t> cycle_sent_ordinals;
        {
            std::lock_guard lock(sent_mutex);
            cycle_sent_ordinals = sent_ordinals;
        }
        const size_t cut_operations =
            c_runtime.pending_p51_source_operations_for_test();
        const uint64_t cut_raw_bytes =
            c_runtime.active_source_raw_bytes_for_test();
        size_t held_materializers_at_cut = 0;
        {
            std::lock_guard lock(materialize_mutex);
            held_materializers_at_cut = gate_waiters;
        }
        const bool occupancy_at_cut = all_active && all_sent &&
            cut_operations == kWindow &&
            cut_raw_bytes == expected_active_raw_bytes &&
            held_materializers_at_cut > 0;
        const Id128 cancelled_id{cohort[cancel_index].request.armed.reservation_id};
        const bool cancelled = occupancy_at_cut &&
            f_runtime.cancel_p51_source_on_owner(
                cohort[cancel_index].request.armed.arm,
                cohort[cancel_index].request.armed.reservation_id,
                cohort[cancel_index].request.absolute_deadline.as_steady_time_point());
        const int current_probe = probe_fd.load(std::memory_order_acquire);
        const bool disconnected = current_probe >= 0 &&
            ::shutdown(current_probe, SHUT_RDWR) == 0;
        {
            std::lock_guard lock(materialize_mutex);
            gate_armed = false;
            release_gate = true;
        }
        materialize_changed.notify_all();

        struct Observation {
            std::optional<local::P50SourceTransferResult> result;
            bool exact_attachment = false;
            std::string error;
        };
        std::vector<Observation> observations(kWindow);
        std::vector<std::thread> waiters;
        waiters.reserve(kWindow);
        for (size_t index = 0; index < kWindow; ++index) {
            waiters.emplace_back([&, index] {
                try {
                    RequestRow& row = cohort[index];
                    observations[index].result = receive_p51_transfer_result(
                        row.pair.receiver, c_launch.identity, row.request_id,
                        row.request.absolute_deadline.as_steady_time_point(),
                        true);
                    if (index != cancel_index &&
                        observations[index].result->code ==
                            local::SourceTransferResultCode::Committed)
                        observations[index].exact_attachment =
                            attach_exact(row, *observations[index].result);
                } catch (const std::exception& error) {
                    observations[index].error = error.what();
                } catch (...) {
                    observations[index].error = "unknown exception";
                }
            });
        }
        for (auto& waiter : waiters)
            waiter.join();

        size_t exact_successes = 0;
        size_t exact_errors = 0;
        size_t matching_reset_acks = 0;
        bool reset_prefix_exact = false;
        bool one_unavailable_bit_in_range = false;
        uint64_t observed_reset_p = 0;
        uint64_t observed_reset_k = 0;
        {
            std::lock_guard lock(ack_mutex);
            for (const ResetAck& ack : reset_acks) {
                if (ack.recovery_prepared_prefix_p < confirmed_prefix +
                                                        kWindow)
                    continue;
                ++matching_reset_acks;
                observed_reset_p = ack.recovery_prepared_prefix_p;
                observed_reset_k = ack.request.settled_prefix_k;
                reset_prefix_exact =
                    ack.recovery_prepared_prefix_p == confirmed_prefix +
                                                          kWindow &&
                    ack.request.settled_prefix_k >= confirmed_prefix &&
                    ack.request.settled_prefix_k <
                        ack.recovery_prepared_prefix_p;
                one_unavailable_bit_in_range =
                    ack.unavailable_suffix_mask != 0 &&
                    std::popcount(ack.unavailable_suffix_mask) == 1 &&
                    (ack.unavailable_suffix_mask >> kWindow) == 0;
                if (reset_prefix_exact && one_unavailable_bit_in_range)
                    break;
            }
        }
        bool cancelled_once = false;
        {
            std::lock_guard lock(retired_mutex);
            cancelled_once = std::count(
                retired_reservations.begin(), retired_reservations.end(),
                cancelled_id) == 1;
        }
        for (size_t index = 0; index < kWindow; ++index) {
            const auto& observation = observations[index];
            if (index == cancel_index) {
                if (observation.result &&
                    observation.result->code ==
                        local::SourceTransferResultCode::Error &&
                    observation.result->error_code != 0)
                    ++exact_errors;
            } else if (observation.result &&
                       observation.result->code ==
                           local::SourceTransferResultCode::Committed &&
                       observation.result->valid() &&
                       observation.result->c_store_guid ==
                           c_launch.c_store_guid &&
                       observation.result->raw_bytes == cohort[index].bytes.size() &&
                       observation.result->raw_digest ==
                           icecc::digest128(cohort[index].bytes) &&
                       observation.exact_attachment) {
                ++exact_successes;
            }
        }
        if (exact_successes != kWindow - 1 || exact_errors != 1) {
            std::fprintf(stderr,
                         "P51_D17 cohort-outcomes profile=%s cycle=%zu "
                         "cancel-index=%zu sent=%zu active-before=%d "
                         "cancelled=%d disconnected=%d successes=%zu "
                         "errors=%zu\n",
                         profile_name, cycle, cancel_index,
                         cycle_sent_ordinals.size(), all_active ? 1 : 0,
                         cancelled ? 1 : 0, disconnected ? 1 : 0,
                         exact_successes, exact_errors);
            for (size_t index = 0; index < kWindow; ++index) {
                if (!observations[index].error.empty())
                    std::fprintf(stderr,
                                 "P51_D17 outcome-error index=%zu %s\n",
                                 index, observations[index].error.c_str());
            }
            std::fflush(stderr);
        }
        const bool operations_zero = wait_for_source_operation_count(
            c_runtime, 0, std::chrono::seconds(5));
        const bool raw_credit_zero = wait_for_source_raw_bytes(
            c_runtime, 0, std::chrono::seconds(5));

        CHECK(all_enqueued);
        CHECK(gate_waited);
        CHECK(all_active);
        CHECK(all_sent);
        CHECK(occupancy_at_cut);
        CHECK(cancelled);
        CHECK(disconnected);
        CHECK(exact_errors == 1);
        CHECK(exact_successes == kWindow - 1);
        CHECK(cancelled_once);
        CHECK(matching_reset_acks >= 1);
        CHECK(reset_prefix_exact);
        CHECK(one_unavailable_bit_in_range);
        CHECK(operations_zero);
        CHECK(raw_credit_zero);

        {
            std::lock_guard lock(sent_mutex);
            std::set<uint64_t> cycle_ordinals;
            for (uint64_t ordinal : cycle_sent_ordinals)
                cycle_ordinals.insert(ordinal);
            CHECK(cycle_sent_ordinals.size() == kWindow);
            CHECK(cycle_ordinals.size() == kWindow);
            CHECK(*cycle_ordinals.begin() == confirmed_prefix + 1);
            CHECK(*cycle_ordinals.rbegin() == confirmed_prefix + kWindow);
        }
        cohort.clear();
        {
            std::lock_guard lock(sent_mutex);
            sent_ordinals.clear();
        }
        run_fresh(request_base + kWindow, static_cast<uint8_t>(0xe0 + cycle));
        {
            std::lock_guard lock(sent_mutex);
            CHECK(sent_ordinals.size() == 1);
            confirmed_prefix = sent_ordinals.front();
            sent_ordinals.clear();
        }
        {
            std::lock_guard lock(sent_mutex);
            sent_ordinals.clear();
        }
        CHECK(wait_for_source_operation_count(c_runtime, 0, std::chrono::seconds(2)));
        CHECK(wait_for_source_raw_bytes(c_runtime, 0, std::chrono::seconds(2)));
        const size_t quiescent_fds = process_open_fd_count();
        CHECK(quiescent_fds == warmed_fd_baseline);
        std::printf("P51_D17 cycle profile=%s index=%zu jobs=30 "
                    "cancel-submission-index=%zu "
                    "survivors=29 fresh=1 accepted-links=%zu "
                    "active-held-at-cut=%zu materializers-held=%zu "
                    "raw-held-at-cut=%llu "
                    "raw-expected=%llu "
                    "raw-bytes=0 fds=%zu baseline=%zu reset-K=%llu "
                    "reset-P=%llu fresh-ordinal=%llu sent=30\n",
                    profile_name, cycle, cancel_index,
                    accepted_connections.load(std::memory_order_acquire),
                    cut_operations,
                    held_materializers_at_cut,
                    static_cast<unsigned long long>(cut_raw_bytes),
                    static_cast<unsigned long long>(expected_active_raw_bytes),
                    quiescent_fds, warmed_fd_baseline,
                    static_cast<unsigned long long>(observed_reset_k),
                    static_cast<unsigned long long>(observed_reset_p),
                    static_cast<unsigned long long>(confirmed_prefix));
        std::fflush(stdout);
    }
    CHECK(accepted_connections.load(std::memory_order_acquire) ==
          cycle_count + 1);
}

void test_p51_aggregate_raw_budget_oversize_fit_and_stop_cleanup() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x4b;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    const SidecarLaunchIdentity remote_f = [] {
        StoreIdentityRoot root{};
        root.bytes[15] = 0x4c;
        return test_sidecar_launch(root);
    }();

    uint16_t oversize_port = 0;
    const int oversize_listener = loopback_listener(oversize_port);
    uint16_t fit_port = 0;
    const int fit_listener = loopback_listener(fit_port);
    uint16_t waiting_port = 0;
    const int waiting_listener = loopback_listener(waiting_port);
    CHECK(oversize_listener >= 0 && fit_listener >= 0 &&
          waiting_listener >= 0);

    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.endpoint_caps.zstd.max_raw_bytes = 64;
    config.max_aggregate_source_raw_bytes = 16;
    config.max_active_p51_source_transfers = 2;
    config.max_pending_p51_source_operations = 4;
    service::SidecarRuntime runtime(std::move(config));

    auto make_request = [&](uint64_t request_id, uint16_t port) {
        auto reservation = test_p51_reservation_request(
            launch.c_store_guid, launch.store_generation,
            launch.identity.generation, launch.identity.attempt,
            request_id, CACHE_PROFILE_ZSTD_TU, 30,
            std::chrono::seconds(8));
        reservation.arm.source.assignment_nonce = request_id;
        reservation.arm.source.selected_f_host = "127.0.0.1";
        reservation.arm.source.selected_f_cache_port = port;
        P51SourceArmedFields armed;
        armed.arm = reservation.arm;
        armed.f_control_generation = 47;
        armed.f_control_attempt = 48;
        armed.f_store_generation = remote_f.store_generation;
        armed.f_store_guid = remote_f.f_store_guid.bytes;
        armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
        armed.arm_observation_id = request_id;
        armed.source_budget_msec = 8000;
        armed.attempt_capability_1.bytes.fill(0xc1);
        armed.attempt_capability_2.bytes.fill(0xc2);
        armed.reservation_id.fill(static_cast<uint8_t>(request_id));
        armed.logical_relationship_id.fill(0xc3);
        armed.relationship_epoch = 1;
        armed.selected_revision = CACHE_WIRE_REVISION_R2;
        armed.selected_window = 30;
        CHECK(armed.valid());
        return local::P51SourceTransferRequest{
            armed, reservation.absolute_deadline};
    };
    auto enqueue = [&](RuntimeCase& pair,
                       const local::P51SourceTransferRequest& request,
                       local::HandoffFd source) {
        const auto operation = local::make_p51_source_transfer_operation(
            launch.identity, request,
            request.armed.arm.source.source_request_id);
        return runtime.enqueue_p51_source_transfer(
            std::move(pair.sender), launch.identity, operation,
            std::move(source)) == service::P51SourceEnqueueResult::Accepted;
    };

    // An over-cap R2 source is rejected before F connection or aggregate
    // credit allocation; the same service remains able to admit fitting work.
    RuntimeCase oversize = authenticated_runtime_pair();
    const auto oversize_request = make_request(7211, oversize_port);
    CHECK(enqueue(oversize, oversize_request,
                  sized_test_source_fd(17, 0xd1)));
    receive_p51_transfer_error(
        oversize.receiver, launch.identity, 7211,
        oversize_request.absolute_deadline.as_steady_time_point(), true,
        static_cast<uint16_t>(local::SourceTransferErrorCode::SourceTooLarge));
    CHECK(wait_for_source_raw_bytes(runtime, 0, std::chrono::seconds(1)));
    pollfd no_oversize_connect{oversize_listener, POLLIN, 0};
    int oversize_ready;
    do {
        oversize_ready = ::poll(&no_oversize_connect, 1, 100);
    } while (oversize_ready < 0 && errno == EINTR);
    CHECK(oversize_ready == 0);

    // A 12-byte R2 source holds credit and reaches its independent F socket.
    // An 8-byte sibling is then queued at the 16-byte aggregate budget and
    // must not open its socket or increase accounted bytes until credit frees.
    RuntimeCase fitting = authenticated_runtime_pair();
    const auto fitting_request = make_request(7212, fit_port);
    CHECK(enqueue(fitting, fitting_request, sized_test_source_fd(12, 0xd2)));
    const bool fitting_operation_seen = wait_for_source_operation_count(
        runtime, 1, std::chrono::seconds(1));
    pollfd fit_ready{fit_listener, POLLIN, 0};
    int fit_polled;
    do {
        fit_polled = ::poll(&fit_ready, 1, 3000);
    } while (fit_polled < 0 && errno == EINTR);
    int fit_fd = -1;
    if (fit_polled > 0)
        fit_fd = ::accept(fit_listener, nullptr, nullptr);
    const bool fitting_credit_held = fit_fd >= 0 &&
        wait_for_source_raw_bytes(runtime, 12, std::chrono::seconds(1));

    RuntimeCase waiting = authenticated_runtime_pair();
    const auto waiting_request = make_request(7213, waiting_port);
    const bool waiting_enqueued = enqueue(
        waiting, waiting_request, sized_test_source_fd(8, 0xd3));
    const bool both_operations_seen = waiting_enqueued &&
        wait_for_source_operation_count(runtime, 2, std::chrono::seconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const bool budget_still_exact =
        runtime.active_source_raw_bytes_for_test() == 12;
    pollfd no_waiting_connect{waiting_listener, POLLIN, 0};
    int waiting_ready;
    do {
        waiting_ready = ::poll(&no_waiting_connect, 1, 0);
    } while (waiting_ready < 0 && errno == EINTR);

    runtime.stop();
    fitting.receiver = local::Connection(-1);
    waiting.receiver = local::Connection(-1);
    if (fit_fd >= 0)
        (void)::close(fit_fd);
    const bool operations_released = wait_for_source_operation_count(
        runtime, 0, std::chrono::seconds(2));
    const bool credit_released =
        wait_for_source_raw_bytes(runtime, 0, std::chrono::seconds(1));

    (void)::close(oversize_listener);
    (void)::close(fit_listener);
    (void)::close(waiting_listener);
    CHECK(fitting_operation_seen);
    CHECK(fit_fd >= 0);
    CHECK(fitting_credit_held);
    CHECK(both_operations_seen);
    CHECK(budget_still_exact);
    CHECK(waiting_ready == 0);
    CHECK(operations_released);
    CHECK(credit_released);
    std::puts("P51_ASYNC_TRANSFER aggregate-raw-budget oversize/fit/stop-cleanup: ok");
}

void test_p51_aggregate_raw_budget_fitting_commit_is_exact(
    ProfileId profile = ProfileId::ZSTD_TU) {
    struct ReadGateRelease {
        std::promise<void>& holder_promise;
        std::promise<void>& fit_promise;
        bool holder_released = false;
        bool fit_released = false;
        void release_holder() noexcept {
            if (holder_released)
                return;
            holder_released = true;
            try {
                holder_promise.set_value();
            } catch (...) {
            }
        }
        void release_fit() noexcept {
            if (fit_released)
                return;
            fit_released = true;
            try {
                fit_promise.set_value();
            } catch (...) {
            }
        }
        void release() noexcept {
            release_fit();
            release_holder();
        }
        ~ReadGateRelease() { release(); }
    };
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x4d;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    const SidecarLaunchIdentity remote_f = [] {
        StoreIdentityRoot root{};
        root.bytes[15] = 0x4e;
        return test_sidecar_launch(root);
    }();
    uint16_t oversize_port = 0;
    const int oversize_listener = loopback_listener(oversize_port);
    uint16_t held_port = 0;
    const int held_listener = loopback_listener(held_port);
    uint16_t waiting_port = 0;
    const int waiting_listener = loopback_listener(waiting_port);
    uint16_t f_port = 0;
    const int f_listener = loopback_listener(f_port);
    CHECK(oversize_listener >= 0 && held_listener >= 0 &&
          waiting_listener >= 0 &&
          f_listener >= 0 && oversize_port != 0 && waiting_port != 0 &&
          held_port != 0 && f_port != 0);

    service::RuntimeConfig runtime_config = test_runtime_config();
    runtime_config.c_store_guid = launch.c_store_guid;
    runtime_config.f_store_guid = launch.f_store_guid;
    runtime_config.f_store_generation = launch.store_generation;
    runtime_config.sidecar_launch = launch;
    runtime_config.endpoint_caps.zstd.max_raw_bytes = 64;
    runtime_config.max_aggregate_source_raw_bytes = 16;
    runtime_config.max_active_p51_source_transfers = 3;
    runtime_config.max_pending_p51_source_operations = 3;
    std::promise<bool> source_read_complete_promise;
    auto source_read_complete = source_read_complete_promise.get_future();
    std::promise<void> release_source_read_promise;
    const std::shared_future<void> release_source_read =
        release_source_read_promise.get_future().share();
    std::promise<void> fit_source_read_complete_promise;
    auto fit_source_read_complete =
        fit_source_read_complete_promise.get_future();
    std::promise<void> release_fit_source_read_promise;
    const std::shared_future<void> release_fit_source_read =
        release_fit_source_read_promise.get_future().share();
    std::atomic<unsigned> source_read_completions{0};
    std::promise<uint64_t> source_credit_waiting_promise;
    auto source_credit_waiting = source_credit_waiting_promise.get_future();
    runtime_config.p51_source_read_complete_for_test = [&](bool success) {
        const unsigned ordinal = source_read_completions.fetch_add(
            1, std::memory_order_acq_rel);
        if (ordinal != 0) {
            if (ordinal == 1) {
                try {
                    fit_source_read_complete_promise.set_value();
                    (void)release_fit_source_read.wait_for(
                        std::chrono::seconds(15));
                } catch (...) {
                }
            }
            return;
        }
        try {
            source_read_complete_promise.set_value(success);
            (void)release_source_read.wait_for(std::chrono::seconds(15));
        } catch (...) {
        }
    };
    runtime_config.p51_source_credit_waiting_for_test = [&](uint64_t bytes) {
        try {
            source_credit_waiting_promise.set_value(bytes);
        } catch (...) {
        }
    };
    service::SidecarRuntime runtime(std::move(runtime_config));
    ReadGateRelease release_read_gate{release_source_read_promise,
                                      release_fit_source_read_promise};

    auto make_request = [&](uint64_t request_id, uint16_t port,
                            uint8_t reservation_byte,
                            std::chrono::milliseconds budget =
                                std::chrono::seconds(8)) {
        auto reservation = test_p51_reservation_request(
            launch.c_store_guid, launch.store_generation,
            launch.identity.generation, launch.identity.attempt,
            request_id, profile_bit(profile), 30, budget);
        reservation.arm.source.assignment_nonce = request_id;
        reservation.arm.source.selected_f_host = "127.0.0.1";
        reservation.arm.source.selected_f_cache_port = port;
        P51SourceArmedFields armed;
        armed.arm = reservation.arm;
        armed.f_control_generation = remote_f.identity.generation;
        armed.f_control_attempt = remote_f.identity.attempt;
        armed.f_store_generation = remote_f.store_generation;
        armed.f_store_guid = remote_f.f_store_guid.bytes;
        armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
        armed.arm_observation_id = request_id;
        armed.source_budget_msec = static_cast<uint32_t>(budget.count());
        armed.attempt_capability_1.bytes.fill(0xe1);
        armed.attempt_capability_2.bytes.fill(0xe2);
        armed.reservation_id.fill(reservation_byte);
        armed.logical_relationship_id.fill(0xe4);
        armed.relationship_epoch = 1;
        armed.selected_revision = CACHE_WIRE_REVISION_R2;
        armed.selected_window = 30;
        CHECK(armed.valid());
        return local::P51SourceTransferRequest{
            armed, reservation.absolute_deadline};
    };
    const auto oversize_request = make_request(7220, oversize_port, 0xe0);
    const auto request = make_request(7221, f_port, 0xe3);
    const auto waiting_request = make_request(7222, waiting_port, 0xe5);
    const auto held_request = make_request(
        7223, held_port, 0xe6, std::chrono::seconds(30));
    const auto& armed = request.armed;

    const std::vector<uint8_t> expected_input(2, 0xf1);

    std::vector<uint8_t> committed_input;
    bool commit_identity_matches = false;
    // Keep an independently owned duplicate solely for bounded teardown.
    // The endpoint owns/closes the accepted descriptor, so publishing that
    // borrowed descriptor to the main thread would risk shutdown on a reused fd.
    std::atomic<int> f_cancel_fd{-1};
    std::promise<ServerRunResult> f_done_promise;
    auto f_done = f_done_promise.get_future();
    std::optional<ServerRunResult> f_result;
    std::thread f_server([&] {
        int adopted_fd = -1;
        try {
            sockaddr_storage peer{};
            socklen_t peer_size = sizeof(peer);
            pollfd listener{f_listener, POLLIN, 0};
            int ready;
            do {
                ready = ::poll(&listener, 1, 5000);
            } while (ready < 0 && errno == EINTR);
            if (ready <= 0)
                throw std::runtime_error("R2 fit F listener was not contacted");
            int accepted;
            do {
                accepted = ::accept(f_listener,
                    reinterpret_cast<sockaddr*>(&peer), &peer_size);
            } while (accepted < 0 && errno == EINTR);
            if (accepted < 0)
                throw std::runtime_error("R2 fit F accept failed");

            std::unique_ptr<MsgChannel> channel(
                Service::createChannelAccepted(
                    accepted, reinterpret_cast<sockaddr*>(&peer), peer_size));
            if (!channel)
                throw std::runtime_error("R2 fit F channel setup failed");
            const auto handshake_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (channel->protocol_admission_state() ==
                       MsgChannel::ProtocolAdmissionState::Pending &&
                   std::chrono::steady_clock::now() < handshake_deadline) {
                short events = POLLIN;
                if (channel->has_pending_write())
                    events |= POLLOUT;
                pollfd socket{channel->fd, events, 0};
                int polled;
                do {
                    polled = ::poll(&socket, 1, 50);
                } while (polled < 0 && errno == EINTR);
                if (polled < 0 ||
                    (socket.revents & (POLLERR | POLLHUP | POLLNVAL)))
                    throw std::runtime_error("R2 fit F handshake socket failed");
                if ((socket.revents & POLLOUT) && !channel->flush_pending())
                    throw std::runtime_error("R2 fit F handshake write failed");
                if ((socket.revents & POLLIN) && !channel->read_a_bit())
                    throw std::runtime_error("R2 fit F handshake read failed");
            }
            if (channel->protocol_admission_state() !=
                    MsgChannel::ProtocolAdmissionState::Ready ||
                !channel->finish_protocol_admission())
                throw std::runtime_error("R2 fit F protocol admission failed");
            std::unique_ptr<Msg> link_request(channel->get_msg_until(
                request.absolute_deadline.as_steady_time_point()));
            if (dynamic_cast<P51CacheLinkSessionMsg*>(link_request.get()) == nullptr)
                throw std::runtime_error("R2 fit F expected CACHE_LINK_SESSION");
            adopted_fd = channel->send_p51_cache_link_session_ready_and_release(
                request.absolute_deadline.as_steady_time_point());
            if (adopted_fd < 0)
                throw std::runtime_error("R2 fit F link-session handoff failed");
            const int cancel_fd = ::dup(adopted_fd);
            if (cancel_fd < 0)
                throw std::runtime_error("R2 fit cancellation fd duplication failed");
            f_cancel_fd.store(cancel_fd, std::memory_order_release);

            namespace asio = boost::asio;
            asio::io_context context;
            boost::system::error_code adopt_error;
            auto socket = P50ServerEndpoint::adopt_connected_fd(
                context.get_executor(), adopted_fd, adopt_error);
            adopted_fd = -1;
            if (!socket.has_value() || adopt_error)
                throw std::runtime_error("R2 fit endpoint fd adoption failed");

            P50ServerEndpointConfig endpoint_config;
            endpoint_config.sidecar_launch = remote_f;
            endpoint_config.f_store_generation = remote_f.store_generation;
            endpoint_config.lookup_p51_link_reservation =
                [armed, request, profile](const LinkHello& hello)
                    -> P51SourceLinkLookupResult {
                if (hello.reservation_id != Id128{armed.reservation_id} ||
                    hello.relationship_id !=
                        Id128{armed.logical_relationship_id} ||
                    hello.c_store_guid != CStoreGuid{armed.arm.source.c_store_guid} ||
                    hello.f_store_guid != FStoreGuid{armed.f_store_guid} ||
                    hello.f_store_generation != armed.f_store_generation ||
                    hello.profile != profile ||
                    hello.relationship_epoch != armed.relationship_epoch)
                    return {P51SourceLinkLookupStatus::Invalid, std::nullopt};
                P51SourceLinkLease lease{armed, request.absolute_deadline};
                lease.relationship_epoch = hello.relationship_epoch;
                lease.history_nonce = hello.history_nonce;
                return {P51SourceLinkLookupStatus::Found, std::move(lease)};
            };
            endpoint_config.consume_p51_job_reservation =
                [armed, request, &expected_input, profile](
                    const LinkHello& hello, const JobBind& binding)
                    -> std::optional<P51SourceJobLease> {
                if (binding.reservation_id != Id128{armed.reservation_id} ||
                    binding.physical_link_generation !=
                        hello.physical_link_generation ||
                    binding.profile != profile ||
                    binding.raw_bytes != expected_input.size() ||
                    binding.raw_digest != icecc::digest128(expected_input))
                    return std::nullopt;
                P51SourceJobLease lease;
                lease.armed = armed;
                lease.absolute_deadline = request.absolute_deadline;
                lease.binding = binding;
                lease.binding_digest = compute_r2_binding_digest(binding);
                lease.input_key = InputRecordKey{hello.c_store_guid,
                                                  binding.tu_seq};
                return lease;
            };
            endpoint_config.input_job_state =
                [&](CStoreGuid, const TxBegin& begin, const TxCommit& commit,
                    std::span<const uint8_t> bytes) {
                    committed_input.assign(bytes.begin(), bytes.end());
                    commit_identity_matches =
                        begin.raw_bytes == bytes.size() &&
                        begin.raw_digest == icecc::digest128(bytes) &&
                        commit.raw_digest == begin.raw_digest;
                    return InputJobState::Open;
                };
            endpoint_config.record_p51_job_commit =
                [](const LinkHello&, const JobBind& binding,
                   const R2TxCommit& commit) {
                    return commit.relationship_ordinal ==
                               binding.relationship_ordinal &&
                           commit.binding_digest ==
                               compute_r2_binding_digest(binding);
                };
            endpoint_config.acknowledge_p51_receipt =
                [](const LinkHello& hello, const CommitAck& ack) {
                    return ack.relationship_id == hello.relationship_id &&
                           ack.relationship_epoch == hello.relationship_epoch &&
                           ack.physical_link_generation ==
                               hello.physical_link_generation;
                };
            P50ServerEndpoint endpoint(remote_f.f_store_guid, {}, nullptr,
                                       nullptr, std::move(endpoint_config));
            auto endpoint_result = asio::co_spawn(
                context,
                endpoint.run_adopted_r2(std::move(*socket)),
                asio::use_future);
            context.run();
            const int owned_cancel_fd =
                f_cancel_fd.exchange(-1, std::memory_order_acq_rel);
            if (owned_cancel_fd >= 0)
                (void)::close(owned_cancel_fd);
            f_done_promise.set_value(endpoint_result.get());
        } catch (...) {
            if (adopted_fd >= 0)
                (void)::close(adopted_fd);
            const int owned_cancel_fd =
                f_cancel_fd.exchange(-1, std::memory_order_acq_rel);
            if (owned_cancel_fd >= 0)
                (void)::close(owned_cancel_fd);
            try {
                f_done_promise.set_exception(std::current_exception());
            } catch (...) {
            }
        }
    });

    RuntimeCase held = authenticated_runtime_pair();
    const auto held_operation = local::make_p51_source_transfer_operation(
        launch.identity, held_request,
        held_request.armed.arm.source.source_request_id);
    const bool held_enqueued = runtime.enqueue_p51_source_transfer(
        std::move(held.sender), launch.identity, held_operation,
        sized_test_source_fd(14, 0xa1)) == service::P51SourceEnqueueResult::Accepted;
    const bool held_read_observed = held_enqueued &&
        source_read_complete.wait_for(std::chrono::seconds(3)) ==
            std::future_status::ready;
    bool held_read_succeeded = false;
    if (held_read_observed)
        held_read_succeeded = source_read_complete.get();
    const bool held_credit = held_read_observed && held_read_succeeded &&
        wait_for_source_raw_bytes(runtime, 14, std::chrono::seconds(1));

    RuntimeCase waiting = authenticated_runtime_pair();
    const auto waiting_operation = local::make_p51_source_transfer_operation(
        launch.identity, waiting_request,
        waiting_request.armed.arm.source.source_request_id);
    auto waiting_source_fd = sized_test_source_fd(3, 0xe5);
    const int waiting_source_fd_number = waiting_source_fd.get();
    const bool waiting_enqueued = runtime.enqueue_p51_source_transfer(
        std::move(waiting.sender), launch.identity, waiting_operation,
        std::move(waiting_source_fd)) == service::P51SourceEnqueueResult::Accepted;
    const bool credit_wait_observed = waiting_enqueued &&
        source_credit_waiting.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready;
    uint64_t waiting_credit_bytes = 0;
    if (credit_wait_observed)
        waiting_credit_bytes = source_credit_waiting.get();
    const bool waiting_operation_accounted = credit_wait_observed &&
        wait_for_source_operation_count(runtime, 2, std::chrono::seconds(1));

    // While the first source holds 14/16 raw bytes and a second exact request
    // is observed waiting for the remaining credit, an over-cap sibling must
    // fail before read/allocation without disturbing either request.
    RuntimeCase oversize = authenticated_runtime_pair();
    const auto oversize_operation = local::make_p51_source_transfer_operation(
        launch.identity, oversize_request,
        oversize_request.armed.arm.source.source_request_id);
    const bool oversize_enqueued = runtime.enqueue_p51_source_transfer(
        std::move(oversize.sender), launch.identity, oversize_operation,
        sized_test_source_fd(17, 0xef)) == service::P51SourceEnqueueResult::Accepted;
    std::exception_ptr oversize_exception;
    std::chrono::steady_clock::time_point oversize_result_at{};
    try {
        if (oversize_enqueued) {
            receive_p51_transfer_error(
                oversize.receiver, launch.identity, 7220,
                oversize_request.absolute_deadline.as_steady_time_point(),
                true, static_cast<uint16_t>(
                    local::SourceTransferErrorCode::SourceTooLarge));
            oversize_result_at = std::chrono::steady_clock::now();
        }
    } catch (...) {
        oversize_exception = std::current_exception();
    }
    const bool oversize_no_read_or_credit =
        source_read_completions.load(std::memory_order_acquire) == 1 &&
        runtime.active_source_raw_bytes_for_test() == 14;
    pollfd no_oversize_connect{oversize_listener, POLLIN, 0};
    int oversize_ready;
    do {
        oversize_ready = ::poll(&no_oversize_connect, 1, 0);
    } while (oversize_ready < 0 && errno == EINTR);

    // The 2-byte fit is submitted after the cap refusal, while the observed
    // 3-byte waiter remains queued. It may bypass that waiter only because it
    // fits the remaining credit; prove exact F commit and result before cancel.
    RuntimeCase pair = authenticated_runtime_pair();
    const auto operation = local::make_p51_source_transfer_operation(
        launch.identity, request, request.armed.arm.source.source_request_id);
    const bool enqueued = runtime.enqueue_p51_source_transfer(
        std::move(pair.sender), launch.identity, operation,
        sized_test_source_fd(expected_input.size(), 0xf1)) ==
        service::P51SourceEnqueueResult::Accepted;
    const bool fit_source_read_observed =
        fit_source_read_complete.wait_for(std::chrono::seconds(3)) ==
        std::future_status::ready;
    // Keep the fit read paused at its completion callback so the raw-credit
    // peak is sampled deterministically before it can publish and release.
    const uint64_t fit_credit_at_read_complete =
        fit_source_read_observed ? runtime.active_source_raw_bytes_for_test()
                                 : 0;
    release_read_gate.release_fit();
    const bool fit_credit_uses_remaining_bytes =
        fit_source_read_observed && fit_credit_at_read_complete == 16;
    local::P50SourceTransferResult result;
    std::exception_ptr transfer_exception;
    std::chrono::steady_clock::time_point fit_result_at{};
    try {
        if (enqueued) {
            result = receive_p51_transfer_result(
                pair.receiver, launch.identity, 7221,
                request.absolute_deadline.as_steady_time_point(), true);
            fit_result_at = std::chrono::steady_clock::now();
        }
    } catch (...) {
        transfer_exception = std::current_exception();
    }
    const bool committed = result.code == local::SourceTransferResultCode::Committed;
    const bool exact_result = committed && result.valid() &&
        result.raw_bytes == expected_input.size() &&
        result.raw_digest == icecc::digest128(expected_input);
    const bool fit_credit_released_to_holder = wait_for_source_raw_bytes(
        runtime, 14, std::chrono::seconds(1));
    const bool waiting_still_blocked_after_fit =
        wait_for_source_operation_count(runtime, 2, std::chrono::seconds(1)) &&
        runtime.active_source_raw_bytes_for_test() == 14;
    waiting.receiver = local::Connection(-1);
    const bool cancelled_waiter_released = waiting_operation_accounted &&
        wait_for_source_operation_count(runtime, 1, std::chrono::seconds(2));
    const auto cancelled_waiter_at = std::chrono::steady_clock::now();
    errno = 0;
    const bool cancelled_waiter_fd_closed = waiting_source_fd_number >= 0 &&
        ::fcntl(waiting_source_fd_number, F_GETFD) == -1 && errno == EBADF;
    pollfd no_waiting_connect{waiting_listener, POLLIN, 0};
    int waiting_ready;
    do {
        waiting_ready = ::poll(&no_waiting_connect, 1, 0);
    } while (waiting_ready < 0 && errno == EINTR);

    held.receiver = local::Connection(-1);
    release_read_gate.release();
    const bool held_operation_released = wait_for_source_operation_count(
        runtime, 0, std::chrono::seconds(2));
    const bool credit_released = wait_for_source_raw_bytes(
        runtime, 0, std::chrono::seconds(1));
    pollfd no_held_connect{held_listener, POLLIN, 0};
    int held_ready;
    do {
        held_ready = ::poll(&no_held_connect, 1, 0);
    } while (held_ready < 0 && errno == EINTR);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool counters_stayed_drained = held_operation_released &&
        credit_released &&
        runtime.pending_p51_source_operations_for_test() == 0 &&
        runtime.active_source_raw_bytes_for_test() == 0;
    pair.receiver = local::Connection(-1);
    oversize.receiver = local::Connection(-1);
    runtime.stop();
    const bool f_finished = f_done.wait_for(std::chrono::seconds(3)) ==
                            std::future_status::ready;
    if (!f_finished) {
        const int fd = f_cancel_fd.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) {
            (void)::shutdown(fd, SHUT_RDWR);
            (void)::close(fd);
        }
    }
    const bool f_finished_after_shutdown = f_finished ||
        f_done.wait_for(std::chrono::seconds(6)) == std::future_status::ready;
    std::exception_ptr f_exception;
    if (f_finished_after_shutdown) {
        try {
            f_result = f_done.get();
        } catch (...) {
            f_exception = std::current_exception();
        }
    }
    // The absolute source lease bounds the endpoint coroutine; shutdown via
    // the owned duplicate above forces the remaining socket wait to unwind.
    // Join rather than detach because the endpoint callbacks capture fixture
    // state by reference.
    if (f_server.joinable())
        f_server.join();
    (void)::close(f_listener);
    (void)::close(oversize_listener);
    (void)::close(held_listener);
    (void)::close(waiting_listener);
    if (oversize_exception)
        std::rethrow_exception(oversize_exception);
    if (transfer_exception)
        std::rethrow_exception(transfer_exception);
    if (f_exception)
        std::rethrow_exception(f_exception);
    CHECK(enqueued);
    CHECK(held_enqueued);
    CHECK(held_read_observed && held_read_succeeded && held_credit);
    CHECK(waiting_enqueued);
    CHECK(credit_wait_observed && waiting_credit_bytes == 3);
    CHECK(waiting_operation_accounted);
    CHECK(oversize_enqueued);
    CHECK(oversize_result_at != std::chrono::steady_clock::time_point{} &&
          oversize_result_at <
              oversize_request.absolute_deadline.as_steady_time_point());
    CHECK(oversize_no_read_or_credit);
    CHECK(oversize_ready == 0);
    CHECK(fit_source_read_observed);
    CHECK(fit_credit_uses_remaining_bytes);
    CHECK(fit_credit_released_to_holder);
    CHECK(waiting_still_blocked_after_fit);
    CHECK(cancelled_waiter_released);
    CHECK(cancelled_waiter_at <
          waiting_request.absolute_deadline.as_steady_time_point());
    CHECK(cancelled_waiter_fd_closed);
    CHECK(waiting_ready == 0);
    CHECK(held_operation_released);
    CHECK(held_ready == 0);
    CHECK(exact_result);
    CHECK(fit_result_at != std::chrono::steady_clock::time_point{} &&
          fit_result_at < request.absolute_deadline.as_steady_time_point());
    CHECK(credit_released);
    CHECK(counters_stayed_drained);
    CHECK(f_finished_after_shutdown);
    CHECK(f_result.has_value() && f_result->committed_input.has_value());
    CHECK(committed_input == expected_input);
    CHECK(commit_identity_matches);
    std::printf("P51_D12 oversize/credit-cancel/exact-fit profile=%u W30: ok\n",
                static_cast<unsigned>(profile));
}

void test_p51_credit_admission_bypasses_blocked_workers() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x4f;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    constexpr size_t kBlockedWaiters = 4;
    constexpr uint64_t kRawCap = 16;
    uint16_t held_port = 0;
    std::array<int, kBlockedWaiters + 2> listeners{};
    std::array<uint16_t, kBlockedWaiters + 2> ports{};
    listeners[0] = loopback_listener(held_port);
    ports[0] = held_port;
    for (size_t i = 1; i < listeners.size(); ++i)
        listeners[i] = loopback_listener(ports[i]);
    CHECK(std::all_of(listeners.begin(), listeners.end(),
                      [](int fd) { return fd >= 0; }));

    std::atomic<size_t> credit_waiters{0};
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.endpoint_caps.zstd.max_raw_bytes = 64;
    config.max_aggregate_source_raw_bytes = kRawCap;
    config.max_active_source_transfers = kBlockedWaiters;
    config.max_active_p51_source_transfers = kBlockedWaiters + 2;
    config.max_pending_p51_source_operations = kBlockedWaiters + 2;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    config.p51_source_credit_waiting_for_test =
        [&credit_waiters](uint64_t bytes) {
            if (bytes == 3)
                credit_waiters.fetch_add(1, std::memory_order_release);
        };
#endif
    service::SidecarRuntime runtime(std::move(config));

    auto make_request = [&](uint64_t request_id, uint16_t port,
                            uint8_t reservation_byte,
                            std::chrono::milliseconds budget =
                                std::chrono::seconds(8)) {
        StoreIdentityRoot remote_root{};
        remote_root.bytes[15] = reservation_byte;
        const SidecarLaunchIdentity remote_f =
            test_sidecar_launch(remote_root);
        auto reservation = test_p51_reservation_request(
            launch.c_store_guid, launch.store_generation,
            launch.identity.generation, launch.identity.attempt,
            request_id, CACHE_PROFILE_ZSTD_TU, 30,
            budget);
        reservation.arm.source.assignment_nonce = request_id;
        reservation.arm.source.selected_f_host = "127.0.0.1";
        reservation.arm.source.selected_f_cache_port = port;
        P51SourceArmedFields armed;
        armed.arm = reservation.arm;
        armed.f_control_generation = remote_f.identity.generation;
        armed.f_control_attempt = remote_f.identity.attempt;
        armed.f_store_generation = remote_f.store_generation;
        armed.f_store_guid = remote_f.f_store_guid.bytes;
        armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
        armed.arm_observation_id = request_id;
        armed.source_budget_msec = static_cast<uint32_t>(budget.count());
        armed.attempt_capability_1.bytes.fill(0xf1);
        armed.attempt_capability_2.bytes.fill(0xf2);
        armed.reservation_id.fill(reservation_byte);
        armed.logical_relationship_id.fill(
            static_cast<uint8_t>(reservation_byte ^ 0x5a));
        armed.relationship_epoch = 1;
        armed.selected_revision = CACHE_WIRE_REVISION_R2;
        armed.selected_window = 30;
        CHECK(armed.valid());
        return local::P51SourceTransferRequest{
            armed, reservation.absolute_deadline};
    };
    auto enqueue = [&](RuntimeCase& pair,
                       const local::P51SourceTransferRequest& request,
                       uint64_t request_id, size_t bytes, uint8_t fill) {
        const auto operation = local::make_p51_source_transfer_operation(
            launch.identity, request, request_id);
        return runtime.enqueue_p51_source_transfer(
            std::move(pair.sender), launch.identity, operation,
            sized_test_source_fd(bytes, fill)) ==
            service::P51SourceEnqueueResult::Accepted;
    };

    std::vector<RuntimeCase> pairs;
    pairs.reserve(kBlockedWaiters + 2);
    for (size_t i = 0; i < kBlockedWaiters + 2; ++i)
        pairs.push_back(authenticated_runtime_pair());

    // Hold 14 of 16 aggregate bytes after connection setup.  The accepted
    // peer deliberately never completes ordinary admission/READY, keeping
    // this fitting transfer's raw credit alive while its prep worker returns.
    const auto held_request = make_request(7230, ports[0], 0xf0);
    CHECK(enqueue(pairs[0], held_request, 7230, 14, 0xa0));
    const bool held_credit = wait_for_source_raw_bytes(
        runtime, 14, std::chrono::seconds(2));
    pollfd held_ready{listeners[0], POLLIN, 0};
    int held_polled;
    do {
        held_polled = ::poll(&held_ready, 1, 3000);
    } while (held_polled < 0 && errno == EINTR);
    int held_fd = -1;
    if (held_polled > 0)
        held_fd = ::accept(listeners[0], nullptr, nullptr);

    std::array<local::P51SourceTransferRequest, kBlockedWaiters> blocked_requests;
    bool blocked_enqueued = held_credit && held_fd >= 0;
    for (size_t i = 0; i < kBlockedWaiters; ++i) {
        blocked_requests[i] = make_request(
            7231 + i, ports[i + 1], static_cast<uint8_t>(0xf5 + i),
            i == 1 ? std::chrono::milliseconds(700)
                   : std::chrono::seconds(8));
        blocked_enqueued = enqueue(
            pairs[i + 1], blocked_requests[i], 7231 + i, 3,
            static_cast<uint8_t>(0xb0 + i)) && blocked_enqueued;
    }
    const bool all_waiting_operations_seen = blocked_enqueued &&
        wait_for_source_operation_count(
            runtime, kBlockedWaiters + 1, std::chrono::seconds(1));
    const auto waiter_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(2);
    while (credit_waiters.load(std::memory_order_acquire) < kBlockedWaiters &&
           std::chrono::steady_clock::now() < waiter_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool four_waiters_reached_credit_gate =
        credit_waiters.load(std::memory_order_acquire) == kBlockedWaiters;

    // Closing one queued control peer abandons only that job. A different
    // queued request expires on its original absolute deadline; neither event
    // consumes raw credit or prevents the fitting request below from running.
    pairs[1].receiver = local::Connection(-1);
    const bool closed_waiter_released = wait_for_source_operation_count(
        runtime, kBlockedWaiters, std::chrono::seconds(2));
    const bool expired_waiter_released = closed_waiter_released &&
        wait_for_source_operation_count(
            runtime, kBlockedWaiters - 1, std::chrono::seconds(2));

    const auto fitting_request = make_request(7235, ports.back(), 0xfa);
    const bool fitting_enqueued = enqueue(
        pairs.back(), fitting_request, 7235, 2, 0xc0);
    const bool operations_after_fit_seen = fitting_enqueued &&
        wait_for_source_operation_count(
            runtime, kBlockedWaiters, std::chrono::seconds(1));
    pollfd fitting_ready{listeners.back(), POLLIN, 0};
    int fitting_polled;
    do {
        fitting_polled = ::poll(&fitting_ready, 1, 1000);
    } while (fitting_polled < 0 && errno == EINTR);
    int fitting_fd = -1;
    if (fitting_polled > 0)
        fitting_fd = ::accept(listeners.back(), nullptr, nullptr);
    const uint64_t raw_at_fit_admission =
        runtime.active_source_raw_bytes_for_test();

    runtime.stop();
    for (auto& pair : pairs)
        pair.receiver = local::Connection(-1);
    if (held_fd >= 0)
        (void)::close(held_fd);
    if (fitting_fd >= 0)
        (void)::close(fitting_fd);
    const bool operations_released = wait_for_source_operation_count(
        runtime, 0, std::chrono::seconds(3));
    const bool raw_credit_released = wait_for_source_raw_bytes(
        runtime, 0, std::chrono::seconds(1));
    for (const int fd : listeners)
        if (fd >= 0)
            (void)::close(fd);

    CHECK(held_credit);
    CHECK(held_fd >= 0);
    CHECK(all_waiting_operations_seen);
    CHECK(four_waiters_reached_credit_gate);
    CHECK(fitting_enqueued);
    CHECK(operations_after_fit_seen);
    CHECK(closed_waiter_released);
    CHECK(expired_waiter_released);
    CHECK(fitting_fd >= 0);
    CHECK(raw_at_fit_admission == kRawCap);
    CHECK(operations_released);
    CHECK(raw_credit_released);
    std::puts("P51_ASYNC_TRANSFER fitting request bypassed four blocked credit workers: ok");
}

void test_p51_credit_admission_bypass_limit_serves_oldest() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0xe1;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    constexpr size_t kBypasses = 30;
    constexpr uint64_t kRawCap = 16;
    uint16_t held_port = 0;
    uint16_t oldest_port = 0;
    uint16_t blocked_port = 0;
    const int held_listener = loopback_listener(held_port);
    const int oldest_listener = loopback_listener(oldest_port);
    const int blocked_listener = loopback_listener(blocked_port);
    std::vector<int> bypass_listeners;
    std::vector<uint16_t> bypass_ports;
    bypass_listeners.reserve(kBypasses);
    bypass_ports.reserve(kBypasses);
    for (size_t i = 0; i < kBypasses; ++i) {
        uint16_t port = 0;
        bypass_listeners.push_back(loopback_listener(port));
        bypass_ports.push_back(port);
    }
    CHECK(held_listener >= 0 && oldest_listener >= 0 && blocked_listener >= 0 &&
          std::all_of(bypass_listeners.begin(), bypass_listeners.end(),
                      [](int fd) { return fd >= 0; }));

    std::atomic<size_t> oldest_wait_reported{0};
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.endpoint_caps.zstd.max_raw_bytes = 64;
    config.max_aggregate_source_raw_bytes = kRawCap;
    // This fixture intentionally exercises the byte-credit bypass policy,
    // not the separately bounded F connector setup pool.
    config.max_active_source_transfers = kBypasses + 2;
    config.max_active_p51_source_transfers = kBypasses + 2;
    config.max_pending_p51_source_operations = kBypasses + 4;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    config.p51_source_credit_waiting_for_test =
        [&oldest_wait_reported](uint64_t bytes) {
            if (bytes == 3)
                oldest_wait_reported.fetch_add(1, std::memory_order_release);
        };
#endif
    service::SidecarRuntime runtime(std::move(config));

    auto make_request = [&](uint64_t request_id, uint16_t port,
                            uint8_t identity_byte,
                            std::chrono::milliseconds budget) {
        StoreIdentityRoot remote_root{};
        remote_root.bytes[15] = identity_byte;
        const SidecarLaunchIdentity remote_f =
            test_sidecar_launch(remote_root);
        auto reservation = test_p51_reservation_request(
            launch.c_store_guid, launch.store_generation,
            launch.identity.generation, launch.identity.attempt,
            request_id, CACHE_PROFILE_ZSTD_TU, 30, budget);
        reservation.arm.source.assignment_nonce = request_id;
        reservation.arm.source.selected_f_host = "127.0.0.1";
        reservation.arm.source.selected_f_cache_port = port;
        P51SourceArmedFields armed;
        armed.arm = reservation.arm;
        armed.f_control_generation = remote_f.identity.generation;
        armed.f_control_attempt = remote_f.identity.attempt;
        armed.f_store_generation = remote_f.store_generation;
        armed.f_store_guid = remote_f.f_store_guid.bytes;
        armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
        armed.arm_observation_id = request_id;
        armed.source_budget_msec = static_cast<uint32_t>(budget.count());
        armed.attempt_capability_1.bytes.fill(identity_byte);
        armed.attempt_capability_2.bytes.fill(0x02);
        armed.reservation_id.fill(identity_byte);
        armed.logical_relationship_id.fill(
            static_cast<uint8_t>(identity_byte ^ 0xa5));
        armed.relationship_epoch = 1;
        armed.selected_revision = CACHE_WIRE_REVISION_R2;
        armed.selected_window = 30;
        CHECK(armed.valid());
        return local::P51SourceTransferRequest{
            armed, reservation.absolute_deadline};
    };
    auto enqueue = [&](RuntimeCase& pair,
                       const local::P51SourceTransferRequest& request,
                       size_t raw_bytes) {
        const uint64_t request_id =
            request.armed.arm.source.source_request_id;
        const auto operation = local::make_p51_source_transfer_operation(
            launch.identity, request, request_id);
        return runtime.enqueue_p51_source_transfer(
            std::move(pair.sender), launch.identity, operation,
            sized_test_source_fd(raw_bytes, 0x91)) ==
            service::P51SourceEnqueueResult::Accepted;
    };
    auto accept_one = [](int listener, int timeout_ms) {
        pollfd ready{listener, POLLIN, 0};
        int polled;
        do {
            polled = ::poll(&ready, 1, timeout_ms);
        } while (polled < 0 && errno == EINTR);
        if (polled <= 0)
            return -1;
        int accepted;
        do {
            accepted = ::accept(listener, nullptr, nullptr);
        } while (accepted < 0 && errno == EINTR);
        return accepted;
    };

    const auto held_request = make_request(
        7260, held_port, 0x31, std::chrono::seconds(20));
    const auto oldest_request = make_request(
        7261, oldest_port, 0x32, std::chrono::seconds(30));
    std::vector<local::P51SourceTransferRequest> bypass_requests;
    bypass_requests.reserve(kBypasses);
    for (size_t i = 0; i < kBypasses; ++i) {
        const uint64_t request_id = 7262 + i;
        bypass_requests.push_back(make_request(
            request_id, bypass_ports[i], static_cast<uint8_t>(0x40 + i),
            std::chrono::seconds(30)));
    }
    const auto blocked_request = make_request(
        7292, blocked_port, 0x7e, std::chrono::seconds(30));

    std::vector<RuntimeCase> pairs;
    pairs.reserve(kBypasses + 3);
    for (size_t i = 0; i < kBypasses + 3; ++i)
        pairs.push_back(authenticated_runtime_pair());
    std::vector<int> accepted_fds;
    accepted_fds.reserve(kBypasses + 2);

    const bool held_enqueued = enqueue(pairs[0], held_request, 14);
    const auto held_enqueued_at = std::chrono::steady_clock::now();
    local::Status held_reply_status = local::Status::IoError;
    local::Status held_goodbye_status = local::Status::IoError;
    std::optional<local::P50SourceTransferResult> held_result;
    bool held_reply_valid = false;
    std::chrono::steady_clock::time_point held_reply_finished_at{};
    const auto held_reply_deadline =
        held_request.absolute_deadline.as_steady_time_point() +
        std::chrono::seconds(3);
    std::thread held_reply_reader([&] {
        local::Frame response;
        held_reply_status = pairs[0].receiver.receive_until(
            response, held_reply_deadline);
        if (held_reply_status == local::Status::Ok &&
            response.type == local::MessageType::Data &&
            local::validate_identity(response, launch.identity) ==
                local::Status::Ok) {
            local::ControlOperation decoded;
            if (local::decode_control_operation(response.payload, decoded) &&
                decoded.kind == local::ControlOperationKind::P51SourceTransfer &&
                decoded.request_id == 7260 &&
                decoded.p51_source_transfer_result &&
                decoded.p51_source_transfer_result->valid()) {
                held_result = *decoded.p51_source_transfer_result;
                held_reply_valid = true;
                const local::Frame goodbye{local::kProtocolVersion,
                                           local::MessageType::Goodbye,
                                           launch.identity, {}};
                held_goodbye_status = pairs[0].receiver.send_until(
                    goodbye, held_reply_deadline);
            }
        }
        held_reply_finished_at = std::chrono::steady_clock::now();
    });
    int held_fd = accept_one(held_listener, 3000);
    const bool held_credit = held_enqueued && held_fd >= 0 &&
        wait_for_source_raw_bytes(runtime, 14, std::chrono::seconds(2));

    const bool oldest_enqueued = enqueue(pairs[1], oldest_request, 3);
    const auto oldest_enqueued_at = std::chrono::steady_clock::now();
    const auto oldest_wait_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (oldest_wait_reported.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < oldest_wait_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool oldest_waiting =
        oldest_wait_reported.load(std::memory_order_acquire) != 0;

    bool all_bypasses_enqueued = held_credit && oldest_enqueued && oldest_waiting;
    for (size_t i = 0; i < kBypasses; ++i) {
        all_bypasses_enqueued = enqueue(pairs[i + 2], bypass_requests[i], 0) &&
                                all_bypasses_enqueued;
        const int accepted = accept_one(bypass_listeners[i], 2000);
        all_bypasses_enqueued = accepted >= 0 && all_bypasses_enqueued;
        if (accepted >= 0)
            accepted_fds.push_back(accepted);
    }
    const auto all_bypasses_accepted_at = std::chrono::steady_clock::now();

    const bool blocked_enqueued = enqueue(
        pairs.back(), blocked_request, 0);
    const auto blocked_enqueued_at = std::chrono::steady_clock::now();
    const bool all_operations_accounted = blocked_enqueued &&
        wait_for_source_operation_count(
            runtime, kBypasses + 3, std::chrono::seconds(2));
    pollfd blocked_ready{blocked_listener, POLLIN, 0};
    int blocked_polled;
    do {
        blocked_polled = ::poll(&blocked_ready, 1, 200);
    } while (blocked_polled < 0 && errno == EINTR);
    const bool thirty_first_was_held_behind_oldest = blocked_polled == 0;

    // The oldest waiter and all later zero-byte jobs have long leases. Only
    // the held transfer may expire to free aggregate bytes. Its control reply
    // deadline is the transfer deadline; once expired, the service closes the
    // control channel instead of sending a late result/Goodbye exchange.
    const bool oldest_started_after_credit_release = held_credit &&
        wait_for_source_raw_bytes(runtime, 3, std::chrono::seconds(25)) &&
        (held_request.absolute_deadline.as_steady_time_point() <
         std::chrono::steady_clock::now());
    int oldest_fd = accept_one(oldest_listener, 3000);
    const auto oldest_started_at = std::chrono::steady_clock::now();
    if (held_reply_reader.joinable())
        held_reply_reader.join();

    runtime.stop();
    for (auto& pair : pairs)
        pair.receiver = local::Connection(-1);
    if (held_fd >= 0)
        (void)::close(held_fd);
    if (oldest_fd >= 0)
        (void)::close(oldest_fd);
    if (blocked_polled > 0) {
        int blocked_fd = ::accept(blocked_listener, nullptr, nullptr);
        if (blocked_fd >= 0)
            (void)::close(blocked_fd);
    }
    for (int fd : accepted_fds)
        (void)::close(fd);
    for (int fd : bypass_listeners)
        if (fd >= 0)
            (void)::close(fd);
    for (int fd : {held_listener, oldest_listener, blocked_listener})
        if (fd >= 0)
            (void)::close(fd);
    const bool operations_released = wait_for_source_operation_count(
        runtime, 0, std::chrono::seconds(5));
    const bool raw_credit_released = wait_for_source_raw_bytes(
        runtime, 0, std::chrono::seconds(2));

    const auto elapsed_ms = [](auto start, auto finish) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   finish - start).count();
    };
    std::fprintf(stderr,
        "credit-bypass timing: held-deadline=%lldms oldest-enqueue=%lldms 30th-accepted=%lldms "
        "31st-enqueue=%lldms oldest-start=%lldms held-reply=%lldms accepted=%zu "
        "held-status=%u held-valid=%d goodbye-status=%u\n",
        static_cast<long long>(elapsed_ms(held_enqueued_at,
            held_request.absolute_deadline.as_steady_time_point())),
        static_cast<long long>(elapsed_ms(held_enqueued_at, oldest_enqueued_at)),
        static_cast<long long>(elapsed_ms(held_enqueued_at, all_bypasses_accepted_at)),
        static_cast<long long>(elapsed_ms(held_enqueued_at, blocked_enqueued_at)),
        static_cast<long long>(elapsed_ms(held_enqueued_at, oldest_started_at)),
        static_cast<long long>(elapsed_ms(held_enqueued_at,
                                          held_reply_finished_at)),
        accepted_fds.size(), static_cast<unsigned>(held_reply_status),
        held_reply_valid ? 1 : 0,
        static_cast<unsigned>(held_goodbye_status));

    CHECK(held_credit);
    CHECK(oldest_enqueued);
    CHECK(oldest_waiting);
    CHECK(all_bypasses_enqueued);
    CHECK(accepted_fds.size() == kBypasses);
    CHECK(all_bypasses_accepted_at <
          held_request.absolute_deadline.as_steady_time_point());
    CHECK(all_operations_accounted);
    CHECK(thirty_first_was_held_behind_oldest);
    CHECK(oldest_started_after_credit_release);
    CHECK(oldest_started_at >
          held_request.absolute_deadline.as_steady_time_point());
    CHECK(held_reply_status == local::Status::CleanEof);
    CHECK(!held_reply_valid);
    CHECK(!held_result.has_value());
    CHECK(oldest_fd >= 0);
    CHECK(operations_released);
    CHECK(raw_credit_released);
    std::puts("P51_ASYNC_TRANSFER 30-bypass cap serves oldest queued job: ok");
}

void test_p51_stop_while_waiting_for_link_session_echo() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x3e;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    uint16_t f_port = 0;
    const int listener_fd = loopback_listener(f_port);
    CHECK(listener_fd >= 0 && f_port != 0);

    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_aggregate_source_raw_bytes = 2;
    config.max_active_p51_source_transfers = 1;
    config.max_pending_p51_source_operations = 1;
    service::SidecarRuntime runtime(std::move(config));

    auto reservation = test_p51_reservation_request(
        launch.c_store_guid, launch.store_generation,
        launch.identity.generation, launch.identity.attempt,
        7111, CACHE_PROFILE_ZSTD_TU, 30, std::chrono::seconds(5));
    reservation.arm.source.selected_f_host = "127.0.0.1";
    reservation.arm.source.selected_f_cache_port = f_port;
    const SidecarLaunchIdentity remote_f = [] {
        StoreIdentityRoot root{};
        root.bytes[15] = 0x3f;
        return test_sidecar_launch(root);
    }();
    P51SourceArmedFields armed;
    armed.arm = reservation.arm;
    armed.f_control_generation = 27;
    armed.f_control_attempt = 28;
    armed.f_store_generation = remote_f.store_generation;
    armed.f_store_guid = remote_f.f_store_guid.bytes;
    armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
    armed.arm_observation_id = 29;
    armed.source_budget_msec = 5000;
    armed.attempt_capability_1.bytes.fill(0xb1);
    armed.attempt_capability_2.bytes.fill(0xb2);
    armed.reservation_id.fill(0xc1);
    armed.logical_relationship_id.fill(0xc2);
    armed.relationship_epoch = 1;
    armed.selected_revision = CACHE_WIRE_REVISION_R2;
    armed.selected_window = 30;
    CHECK(armed.valid());
    const local::P51SourceTransferRequest request{
        armed, reservation.absolute_deadline};

    std::promise<bool> request_seen_promise;
    auto request_seen_future = request_seen_promise.get_future();
    std::thread peer([listener_fd,
                      request_seen_promise = std::move(request_seen_promise)]() mutable {
        struct pollfd listener{listener_fd, POLLIN, 0};
        int ready;
        do {
            ready = ::poll(&listener, 1, 3000);
        } while (ready < 0 && errno == EINTR);
        int accepted = -1;
        sockaddr_storage address{};
        socklen_t address_length = sizeof(address);
        if (ready > 0) {
            do {
                accepted = ::accept(listener_fd,
                    reinterpret_cast<sockaddr*>(&address), &address_length);
            } while (accepted < 0 && errno == EINTR);
        }
        std::unique_ptr<MsgChannel> channel(
            accepted >= 0
                ? Service::createChannelAccepted(
                    accepted, reinterpret_cast<sockaddr*>(&address),
                    address_length)
                : nullptr);
        bool saw_request = false;
        const auto handshake_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (channel && channel->protocol_admission_state() ==
                              MsgChannel::ProtocolAdmissionState::Pending &&
               std::chrono::steady_clock::now() < handshake_deadline) {
            short events = POLLIN;
            if (channel->has_pending_write())
                events |= POLLOUT;
            struct pollfd socket{channel->fd, events, 0};
            int polled;
            do {
                polled = ::poll(&socket, 1, 50);
            } while (polled < 0 && errno == EINTR);
            if (polled < 0 || (socket.revents & (POLLERR | POLLHUP | POLLNVAL)))
                break;
            if ((socket.revents & POLLOUT) && !channel->flush_pending())
                break;
            if ((socket.revents & POLLIN) && !channel->read_a_bit())
                break;
        }
        if (channel && channel->protocol_admission_state() ==
                           MsgChannel::ProtocolAdmissionState::Ready &&
            channel->finish_protocol_admission()) {
            std::unique_ptr<Msg> message(channel->get_msg_until(
                std::chrono::steady_clock::now() + std::chrono::seconds(3)));
            saw_request = dynamic_cast<P51CacheLinkSessionMsg*>(
                              message.get()) != nullptr;
        }
        try {
            request_seen_promise.set_value(saw_request);
        } catch (...) {
        }
        if (saw_request && channel) {
            const auto close_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < close_deadline) {
                struct pollfd socket{channel->fd, POLLIN, 0};
                int polled;
                do {
                    polled = ::poll(&socket, 1, 50);
                } while (polled < 0 && errno == EINTR);
                if (polled < 0 ||
                    (socket.revents & (POLLERR | POLLHUP | POLLNVAL)))
                    break;
                if ((socket.revents & POLLIN) != 0) {
                    char byte = 0;
                    const ssize_t count = ::recv(channel->fd, &byte, 1,
                                                 MSG_PEEK | MSG_DONTWAIT);
                    if (count == 0 || (count < 0 && errno != EAGAIN &&
                                       errno != EWOULDBLOCK && errno != EINTR))
                        break;
                }
            }
        }
    });

    RuntimeCase pair = authenticated_runtime_pair();
    const local::ControlOperation operation =
        local::make_p51_source_transfer_operation(
            launch.identity, request, request.armed.arm.source.source_request_id);
    const bool enqueued = runtime.enqueue_p51_source_transfer(
        std::move(pair.sender), launch.identity, operation,
        oversized_test_source_fd()) == service::P51SourceEnqueueResult::Accepted;
    const bool operation_seen = enqueued && wait_for_source_operation_count(
        runtime, 1, std::chrono::seconds(1));
    const bool got_p51_request = operation_seen &&
        request_seen_future.wait_for(std::chrono::seconds(4)) ==
            std::future_status::ready && request_seen_future.get();
    const bool raw_credit_held = got_p51_request &&
        wait_for_source_raw_bytes(runtime, 2, std::chrono::seconds(1));

    // The ordinary protocol reached READY and the peer decoded the auxiliary
    // P51_CACHE_LINK_SESSION request, but deliberately withholds its echo.
    runtime.stop();
    pair.receiver = local::Connection(-1);
    const bool operation_released = wait_for_source_operation_count(
        runtime, 0, std::chrono::seconds(2));
    const bool raw_credit_released =
        wait_for_source_raw_bytes(runtime, 0, std::chrono::seconds(1));
    (void)::close(listener_fd);
    peer.join();
    CHECK(enqueued);
    CHECK(operation_seen);
    CHECK(got_p51_request);
    CHECK(raw_credit_held);
    CHECK(operation_released);
    CHECK(raw_credit_released);
    std::puts("P51_ASYNC_TRANSFER post-protocol-stop/raw-credit-release: ok");
}

void test_p51_stop_while_waiting_for_link_state() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x40;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    uint16_t f_port = 0;
    const int listener_fd = loopback_listener(f_port);
    CHECK(listener_fd >= 0 && f_port != 0);

    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_aggregate_source_raw_bytes = 2;
    config.max_active_p51_source_transfers = 1;
    config.max_pending_p51_source_operations = 1;
    service::SidecarRuntime runtime(std::move(config));

    auto reservation = test_p51_reservation_request(
        launch.c_store_guid, launch.store_generation,
        launch.identity.generation, launch.identity.attempt,
        7112, CACHE_PROFILE_ZSTD_TU, 30, std::chrono::seconds(5));
    reservation.arm.source.selected_f_host = "127.0.0.1";
    reservation.arm.source.selected_f_cache_port = f_port;
    const SidecarLaunchIdentity remote_f = [] {
        StoreIdentityRoot root{};
        root.bytes[15] = 0x41;
        return test_sidecar_launch(root);
    }();
    P51SourceArmedFields armed;
    armed.arm = reservation.arm;
    armed.f_control_generation = 37;
    armed.f_control_attempt = 38;
    armed.f_store_generation = remote_f.store_generation;
    armed.f_store_guid = remote_f.f_store_guid.bytes;
    armed.f_store_derivation_version = kStoreIdentityDerivationVersion;
    armed.arm_observation_id = 39;
    armed.source_budget_msec = 5000;
    armed.attempt_capability_1.bytes.fill(0xd1);
    armed.attempt_capability_2.bytes.fill(0xd2);
    armed.reservation_id.fill(0xe1);
    armed.logical_relationship_id.fill(0xe2);
    armed.relationship_epoch = 1;
    armed.selected_revision = CACHE_WIRE_REVISION_R2;
    armed.selected_window = 30;
    CHECK(armed.valid());
    const local::P51SourceTransferRequest request{
        armed, reservation.absolute_deadline};

    std::promise<bool> hello_seen_promise;
    auto hello_seen_future = hello_seen_promise.get_future();
    std::thread peer([listener_fd,
                      hello_seen_promise = std::move(hello_seen_promise)]() mutable {
        struct pollfd listener{listener_fd, POLLIN, 0};
        int ready;
        do {
            ready = ::poll(&listener, 1, 3000);
        } while (ready < 0 && errno == EINTR);
        int accepted = -1;
        sockaddr_storage address{};
        socklen_t address_length = sizeof(address);
        if (ready > 0) {
            do {
                accepted = ::accept(listener_fd,
                    reinterpret_cast<sockaddr*>(&address), &address_length);
            } while (accepted < 0 && errno == EINTR);
        }
        std::unique_ptr<MsgChannel> channel(
            accepted >= 0
                ? Service::createChannelAccepted(
                    accepted, reinterpret_cast<sockaddr*>(&address),
                    address_length)
                : nullptr);
        const auto handshake_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (channel && channel->protocol_admission_state() ==
                              MsgChannel::ProtocolAdmissionState::Pending &&
               std::chrono::steady_clock::now() < handshake_deadline) {
            short events = POLLIN;
            if (channel->has_pending_write())
                events |= POLLOUT;
            struct pollfd socket{channel->fd, events, 0};
            int polled;
            do {
                polled = ::poll(&socket, 1, 50);
            } while (polled < 0 && errno == EINTR);
            if (polled < 0 || (socket.revents & (POLLERR | POLLHUP | POLLNVAL)))
                break;
            if ((socket.revents & POLLOUT) && !channel->flush_pending())
                break;
            if ((socket.revents & POLLIN) && !channel->read_a_bit())
                break;
        }
        int raw_fd = -1;
        if (channel && channel->protocol_admission_state() ==
                           MsgChannel::ProtocolAdmissionState::Ready &&
            channel->finish_protocol_admission()) {
            std::unique_ptr<Msg> message(channel->get_msg_until(
                std::chrono::steady_clock::now() + std::chrono::seconds(3)));
            if (dynamic_cast<P51CacheLinkSessionMsg*>(message.get()) != nullptr)
                raw_fd = channel->send_p51_cache_link_session_ready_and_release(
                    std::chrono::steady_clock::now() + std::chrono::seconds(3));
        }
        channel.reset();

        bool hello_seen = false;
        if (raw_fd >= 0) {
            FrameParser parser;
            const auto hello_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            std::array<uint8_t, 512> buffer{};
            while (!hello_seen &&
                   std::chrono::steady_clock::now() < hello_deadline) {
                struct pollfd socket{raw_fd, POLLIN, 0};
                int polled;
                do {
                    polled = ::poll(&socket, 1, 50);
                } while (polled < 0 && errno == EINTR);
                if (polled < 0 ||
                    (socket.revents & (POLLERR | POLLHUP | POLLNVAL)))
                    break;
                if ((socket.revents & POLLIN) == 0)
                    continue;
                const ssize_t count = ::recv(raw_fd, buffer.data(), buffer.size(), 0);
                if (count <= 0)
                    break;
                for (const auto& frame : parser.feed(
                         std::span<const uint8_t>(buffer.data(),
                                                 static_cast<size_t>(count)))) {
                    const Message message = decode_payload(frame.type, frame.payload);
                    hello_seen = std::holds_alternative<LinkHello>(message);
                    if (hello_seen)
                        break;
                }
            }
        }
        try {
            hello_seen_promise.set_value(hello_seen);
        } catch (...) {
        }
        if (hello_seen) {
            const auto close_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < close_deadline) {
                struct pollfd socket{raw_fd, POLLIN, 0};
                int polled;
                do {
                    polled = ::poll(&socket, 1, 50);
                } while (polled < 0 && errno == EINTR);
                if (polled < 0 ||
                    (socket.revents & (POLLERR | POLLHUP | POLLNVAL)))
                    break;
                if ((socket.revents & POLLIN) != 0) {
                    uint8_t byte = 0;
                    const ssize_t count = ::recv(raw_fd, &byte, 1,
                                                 MSG_PEEK | MSG_DONTWAIT);
                    if (count == 0 || (count < 0 && errno != EAGAIN &&
                                       errno != EWOULDBLOCK && errno != EINTR))
                        break;
                }
            }
        }
        if (raw_fd >= 0)
            (void)::close(raw_fd);
    });

    RuntimeCase pair = authenticated_runtime_pair();
    const local::ControlOperation operation =
        local::make_p51_source_transfer_operation(
            launch.identity, request, request.armed.arm.source.source_request_id);
    const bool enqueued = runtime.enqueue_p51_source_transfer(
        std::move(pair.sender), launch.identity, operation,
        oversized_test_source_fd()) == service::P51SourceEnqueueResult::Accepted;
    const bool operation_seen = enqueued && wait_for_source_operation_count(
        runtime, 1, std::chrono::seconds(1));
    const bool got_hello = operation_seen &&
        hello_seen_future.wait_for(std::chrono::seconds(4)) ==
            std::future_status::ready && hello_seen_future.get();
    const bool raw_credit_held = got_hello &&
        wait_for_source_raw_bytes(runtime, 2, std::chrono::seconds(1));

    // The peer has acknowledged local link adoption and decoded LINK_HELLO,
    // but withholds LINK_STATE. Stop must close that exact physical link.
    runtime.stop();
    pair.receiver = local::Connection(-1);
    const bool operation_released = wait_for_source_operation_count(
        runtime, 0, std::chrono::seconds(2));
    const bool raw_credit_released =
        wait_for_source_raw_bytes(runtime, 0, std::chrono::seconds(1));
    (void)::close(listener_fd);
    peer.join();
    CHECK(enqueued);
    CHECK(operation_seen);
    CHECK(got_hello);
    CHECK(raw_credit_held);
    CHECK(operation_released);
    CHECK(raw_credit_released);
    std::puts("P51_ASYNC_TRANSFER post-HELLO-stop/raw-credit-release: ok");
}

void test_p51_reservation_capacity_120_cancel_and_expiry() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x37;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    StoreIdentityRoot remote_root{};
    remote_root.bytes[15] = 0x38;
    const CStoreGuid remote_c = c_store_guid_for_root(remote_root);
    std::mutex retired_mutex;
    std::condition_variable retired_changed;
    std::vector<std::pair<Id128, bool>> retired_rows;
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_pending_p51_source_reservations = 120;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    config.p51_reservation_retired_for_test =
        [&](Id128 id, bool marker_retired) {
            {
                std::lock_guard lock(retired_mutex);
                retired_rows.emplace_back(id, marker_retired);
            }
            retired_changed.notify_all();
        };
#endif
    service::SidecarRuntime runtime(std::move(config));

    std::vector<local::P51SourceReservationRequest> requests;
    std::vector<P51SourceArmedFields> armed;
    requests.reserve(120);
    armed.reserve(120);
    for (uint64_t index = 0; index != 120; ++index) {
        auto request = test_p51_reservation_request(
            remote_c, 41, launch.identity.generation, launch.identity.attempt,
            7200 + index, CACHE_PROFILE_ZSTD_TU, 30,
            index == 119 ? std::chrono::seconds(6) : std::chrono::seconds(30));
        const local::P51SourceReservationResult result =
            runtime.reserve_p51_source_on_owner(request);
        CHECK(result.error_code == 0 && result.armed.has_value());
        requests.push_back(std::move(request));
        armed.push_back(*result.armed);
    }
    auto overflow = test_p51_reservation_request(
        remote_c, 41, launch.identity.generation, launch.identity.attempt,
        7320, CACHE_PROFILE_ZSTD_TU, 30);
    CHECK(!runtime.reserve_p51_source_on_owner(overflow).armed.has_value());

    const Id128 cancelled_id{armed[1].reservation_id};
    const Id128 expiring_id{armed[119].reservation_id};
    CHECK(cancelled_id != expiring_id);
    CHECK(runtime.cancel_p51_source_on_owner(
        requests[1].arm, armed[1].reservation_id,
        std::chrono::steady_clock::now() + std::chrono::seconds(1)));

#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    const auto find_retired = [&](Id128 id) {
        return std::find_if(retired_rows.begin(), retired_rows.end(),
                            [&](const auto& row) { return row.first == id; });
    };
    {
        std::unique_lock lock(retired_mutex);
        CHECK(retired_changed.wait_for(lock, std::chrono::seconds(1), [&] {
            return find_retired(cancelled_id) != retired_rows.end();
        }));
        const auto cancelled = find_retired(cancelled_id);
        CHECK(cancelled != retired_rows.end() && !cancelled->second);
    }

    // Observe autonomous timer retirement of this exact ARM before making any
    // new owner request (which would itself sweep expired reservations).
    {
        std::unique_lock lock(retired_mutex);
        CHECK(retired_changed.wait_for(lock, std::chrono::seconds(8), [&] {
            return find_retired(expiring_id) != retired_rows.end();
        }));
        CHECK(retired_rows.size() == 2);
        const auto cancelled = find_retired(cancelled_id);
        const auto expired = find_retired(expiring_id);
        CHECK(cancelled != retired_rows.end() && !cancelled->second);
        CHECK(expired != retired_rows.end() && !expired->second);
    }
#else
    CHECK(false);
#endif

    auto after_cancel = test_p51_reservation_request(
        remote_c, 41, launch.identity.generation, launch.identity.attempt,
        7321, CACHE_PROFILE_ZSTD_TU, 30);
    CHECK(runtime.reserve_p51_source_on_owner(after_cancel).armed.has_value());
    auto after_expiry = test_p51_reservation_request(
        remote_c, 41, launch.identity.generation, launch.identity.attempt,
        7322, CACHE_PROFILE_ZSTD_TU, 30);
    CHECK(runtime.reserve_p51_source_on_owner(after_expiry).armed.has_value());
    auto full_again = test_p51_reservation_request(
        remote_c, 41, launch.identity.generation, launch.identity.attempt,
        7323, CACHE_PROFILE_ZSTD_TU, 30);
    CHECK(!runtime.reserve_p51_source_on_owner(full_again).armed.has_value());
    std::puts("P51_RESERVATION_CAP global120/cancel/exact-timer-expiry: ok");
}

LinkHello test_p51_link_hello(const P51SourceArmedFields& armed,
                              uint64_t physical_generation,
                              HistoryNonce nonce,
                              LinkStartMode start_mode = LinkStartMode::Initial,
                              uint64_t verified_floor = 0) {
    LinkHello hello;
    switch (armed.arm.source.cache_profile) {
    case CACHE_PROFILE_P29V1: hello.profile = ProfileId::P29V1; break;
    case CACHE_PROFILE_ZSTD_TU: hello.profile = ProfileId::ZSTD_TU; break;
    case CACHE_PROFILE_ZSTD_ROUTE: hello.profile = ProfileId::ZSTD_ROUTE; break;
    default: CHECK(false);
    }
    hello.window = armed.selected_window;
    hello.max_frame_payload = kInitialMaxFramePayload;
    hello.max_raw_bytes = 1U << 20;
    hello.max_encoded_bytes = 1U << 20;
    hello.max_output_bytes = 1U << 20;
    hello.reservation_id = Id128{armed.reservation_id};
    hello.relationship_id = Id128{armed.logical_relationship_id};
    hello.relationship_epoch = armed.relationship_epoch;
    hello.physical_link_generation = physical_generation;
    hello.c_store_guid = CStoreGuid{armed.arm.source.c_store_guid};
    hello.c_store_generation = armed.arm.source.c_store_generation;
    hello.f_store_guid = FStoreGuid{armed.f_store_guid};
    hello.f_store_generation = armed.f_store_generation;
    hello.c_control_generation = armed.arm.source.c_control_generation;
    hello.c_control_attempt = armed.arm.source.c_control_attempt;
    hello.system_source_fingerprint = icecc::digest128("P51 owner regression");
    hello.history_nonce = nonce;
    hello.start_mode = start_mode;
    hello.verified_receipt_floor = verified_floor;
    return hello;
}

JobBind test_p51_job_binding(const P51SourceArmedFields& armed,
                             uint64_t physical_generation,
                             uint64_t ordinal, uint64_t tu_seq,
                             std::string_view raw) {
    JobBind binding;
    binding.reservation_id = Id128{armed.reservation_id};
    binding.physical_link_generation = physical_generation;
    binding.relationship_ordinal = ordinal;
    binding.wire_job_id = armed.arm.source.wire_job_id;
    binding.assignment_epoch = armed.arm.source.assignment_epoch;
    binding.assignment_nonce = armed.arm.source.assignment_nonce;
    binding.logical_job = armed.arm.source.logical_job;
    binding.compiler_attempt = armed.arm.source.compiler_attempt;
    binding.source_request_id = armed.arm.source.source_request_id;
    binding.tu_seq = TuSeq{tu_seq};
    switch (armed.arm.source.cache_profile) {
    case CACHE_PROFILE_P29V1: binding.profile = ProfileId::P29V1; break;
    case CACHE_PROFILE_ZSTD_TU: binding.profile = ProfileId::ZSTD_TU; break;
    case CACHE_PROFILE_ZSTD_ROUTE: binding.profile = ProfileId::ZSTD_ROUTE; break;
    default: CHECK(false);
    }
    binding.raw_bytes = raw.size();
    binding.raw_digest = icecc::digest128(raw);
    return binding;
}

RecoverBegin test_p51_recover_begin(const LinkHello& link, uint64_t floor,
                                    uint64_t prepared,
                                    uint32_t witness_count,
                                    Id128 operation_id) {
    RecoverBegin begin;
    begin.relationship_id = link.relationship_id;
    begin.relationship_epoch = link.relationship_epoch;
    begin.physical_link_generation = link.physical_link_generation;
    begin.operation_id = operation_id;
    begin.verified_floor_a = floor;
    begin.prepared_prefix_p = prepared;
    begin.witness_count = witness_count;
    return begin;
}

RecoverEnd test_p51_recover_end(const RecoverBegin& begin,
                                std::span<const RecoverWitness> witnesses) {
    RecoverEnd end;
    end.relationship_id = begin.relationship_id;
    end.relationship_epoch = begin.relationship_epoch;
    end.physical_link_generation = begin.physical_link_generation;
    end.operation_id = begin.operation_id;
    end.witness_count = static_cast<uint32_t>(witnesses.size());
    end.transcript_digest = compute_r2_recovery_transcript_digest(begin, witnesses);
    return end;
}

struct D11ReceiptPause {
    std::mutex mutex;
    std::condition_variable changed;
    bool at_cap = false;
    bool continue_after_cap = false;
    bool after_refill = false;
    bool finish = false;
    LinkHello link{};
    JobBind refill_binding{};
    std::exception_ptr error;
};

boost::asio::awaitable<void> d11_receipt_client(
    uint16_t f_port, ProfileId profile, uint32_t window,
    const SidecarLaunchIdentity& c_launch,
    const SidecarLaunchIdentity& f_launch,
    const std::vector<P51SourceArmedFields>& armed,
    const std::vector<std::vector<uint8_t>>& raw_inputs,
    D11ReceiptPause& pause) {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;
    const EndpointCaps caps = [&] {
        EndpointCaps value;
        value.profile = profile;
        value.supported_profiles = profile_bit(profile);
        value.zstd.max_raw_bytes = 1U << 20;
        value.zstd.max_encoded_body_bytes = 1U << 20;
        return value;
    }();
    const PreparationRouteKey route{FStoreGuid{f_launch.f_store_guid.bytes},
                                    f_launch.store_generation, profile};
    PreparationAuthorityLimits limits;
    limits.max_speculative_tus = 1;
    limits.max_speculative_raw_bytes = 1U << 20;
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_launch.c_store_guid, caps.zstd, limits, 1, profile);
    P50ClientEndpoint client(authority, caps, HistoryNonce{1}, nullptr,
                             nullptr, std::nullopt, {}, {}, route);
    const tcp::endpoint remote(asio::ip::address_v4::loopback(), f_port);
    tcp::socket socket(co_await asio::this_coro::executor);
    co_await socket.async_connect(remote, asio::use_awaitable);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(35);

    const auto prepare = [&](size_t index) {
        return authority->prepare_for_route(
            route, PrepareRequestKey{0xd110, index + 1}, raw_inputs[index]);
    };
    PreparedTuHandle prepared = prepare(0);
    LinkHello hello = test_p51_link_hello(
        armed.front(), 1, HistoryNonce{1}, LinkStartMode::Initial);
    hello.max_raw_bytes = 1U << 20;
    hello.max_encoded_bytes = 1U << 20;
    hello.max_output_bytes = 1U << 20;
    hello.system_source_fingerprint = profile == ProfileId::P29V1
        ? authority->p29v1_system_source_fingerprint(prepared)
        : icecc::digest128("D11 real F receipt ledger");
    const LinkState link_state =
        co_await client.open_r2_link(socket, hello, deadline);
    CHECK(link_state.window == window && link_state.profile == profile);

    const auto make_binding = [&](size_t index,
                                  PreparedTuHandle handle) {
        const auto& source = armed[index].arm.source;
        JobBind binding;
        binding.reservation_id = Id128{armed[index].reservation_id};
        binding.physical_link_generation = hello.physical_link_generation;
        binding.relationship_ordinal = index + 1;
        binding.wire_job_id = source.wire_job_id;
        binding.assignment_epoch = source.assignment_epoch;
        binding.assignment_nonce = source.assignment_nonce;
        binding.logical_job = source.logical_job;
        binding.compiler_attempt = source.compiler_attempt;
        binding.source_request_id = source.source_request_id;
        binding.tu_seq = authority->prepared_tu_seq(handle);
        binding.profile = profile;
        binding.raw_bytes = raw_inputs[index].size();
        binding.raw_digest = icecc::digest128(raw_inputs[index]);
        return binding;
    };

    for (size_t index = 0; index != window; ++index) {
        if (index != 0)
            prepared = prepare(index);
        const JobBind binding = make_binding(index, prepared);
        const R2SentBundle sent = co_await client.write_r2_bundle(
            socket, binding, prepared, deadline);
        const ClientRunResult receipt = co_await client.read_r2_receipt(
            socket, sent, deadline);
        CHECK(receipt.status == ClientRunStatus::Committed &&
              receipt.committed_input.has_value() &&
              receipt.committed_input->tu_seq == binding.tu_seq &&
              receipt.committed_commit.has_value() &&
              receipt.committed_commit->raw_digest == binding.raw_digest);
    }

    prepared = prepare(window);
    const JobBind refill_binding = make_binding(window, prepared);
    {
        std::unique_lock lock(pause.mutex);
        pause.link = hello;
        pause.refill_binding = refill_binding;
        pause.at_cap = true;
        pause.changed.notify_all();
        pause.changed.wait(lock, [&] { return pause.continue_after_cap; });
    }

    co_await client.write_r2_ack(socket, window, deadline);
    const R2SentBundle refill = co_await client.write_r2_bundle(
        socket, refill_binding, prepared, deadline);
    const ClientRunResult refill_receipt = co_await client.read_r2_receipt(
        socket, refill, deadline);
    CHECK(refill_receipt.status == ClientRunStatus::Committed &&
          refill_receipt.committed_input.has_value() &&
          refill_receipt.committed_input->tu_seq == refill_binding.tu_seq &&
          refill_receipt.committed_commit.has_value() &&
          refill_receipt.committed_commit->raw_digest ==
              refill_binding.raw_digest);
    co_await client.write_r2_ack(socket, window + 1, deadline);
    {
        std::unique_lock lock(pause.mutex);
        pause.after_refill = true;
        pause.changed.notify_all();
        pause.changed.wait(lock, [&] { return pause.finish; });
    }
    const std::vector<uint8_t> close_frame =
        encode_frame(Message{CloseMessage{}});
    co_await asio::async_write(socket, asio::buffer(close_frame),
                               asio::use_awaitable);
    boost::system::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
}

void test_p51_d11_real_receipt_ledger(ProfileId profile, uint32_t window) {
    CHECK(window == 1 || window == 30);
    const char* profile_name = profile == ProfileId::P29V1 ? "P29V1" :
        profile == ProfileId::ZSTD_TU ? "ZSTD_TU" : "ZSTD_ROUTE";
    const uint32_t cache_profile = profile == ProfileId::P29V1
        ? CACHE_PROFILE_P29V1
        : profile == ProfileId::ZSTD_TU
              ? CACHE_PROFILE_ZSTD_TU
              : CACHE_PROFILE_ZSTD_ROUTE;
    const size_t total_jobs = static_cast<size_t>(window) + 1;
    StoreIdentityRoot c_root{};
    c_root.bytes[15] = static_cast<uint8_t>(0x21 + static_cast<unsigned>(profile));
    const SidecarLaunchIdentity c_launch = test_sidecar_launch(c_root);
    StoreIdentityRoot f_root{};
    f_root.bytes[15] = static_cast<uint8_t>(0x41 + static_cast<unsigned>(profile));
    const SidecarLaunchIdentity f_launch = test_sidecar_launch(f_root);
    uint16_t f_port = 0;
    const int listener = loopback_listener(f_port);
    CHECK(listener >= 0 && f_port != 0);

    std::vector<local::P51SourceReservationRequest> requests;
    std::vector<P51SourceArmedFields> armed;
    std::vector<std::vector<uint8_t>> raw_inputs;
    requests.reserve(total_jobs);
    armed.reserve(total_jobs);
    raw_inputs.reserve(total_jobs);

    service::RuntimeConfig f_config = test_runtime_config();
    f_config.c_store_guid = f_launch.c_store_guid;
    f_config.f_store_guid = f_launch.f_store_guid;
    f_config.f_store_generation = f_launch.store_generation;
    f_config.sidecar_launch = f_launch;
    f_config.endpoint_caps.profile = profile;
    f_config.endpoint_caps.supported_profiles = profile_bit(profile);
    f_config.endpoint_caps.zstd.max_raw_bytes = 1U << 20;
    f_config.endpoint_caps.zstd.max_encoded_body_bytes = 1U << 20;
    f_config.max_pending_p51_source_reservations = total_jobs + 4;
    std::mutex materialized_mutex;
    std::vector<std::pair<Digest128, uint64_t>> materialized;
    f_config.endpoint_config.input_job_state =
        [&](CStoreGuid, const TxBegin& begin, const TxCommit&,
            std::span<const uint8_t> bytes) {
            std::lock_guard lock(materialized_mutex);
            materialized.emplace_back(icecc::digest128(bytes), bytes.size());
            CHECK(begin.raw_digest == icecc::digest128(bytes) &&
                  begin.raw_bytes == bytes.size());
            return InputJobState::Open;
        };
    service::SidecarRuntime f_runtime(std::move(f_config));

    for (size_t index = 0; index != total_jobs; ++index) {
        std::string text = "D11 exact F receipt ledger profile=";
        text += profile_name;
        text += " window=" + std::to_string(window);
        text += " ordinal=" + std::to_string(index + 1) + "\n";
        raw_inputs.emplace_back(text.begin(), text.end());
        auto request = test_p51_reservation_request(
            c_launch.c_store_guid, c_launch.store_generation,
            c_launch.identity.generation, c_launch.identity.attempt,
            930000 + index, cache_profile, window,
            std::chrono::seconds(60));
        request.arm.source.selected_f_host = "127.0.0.1";
        request.arm.source.selected_f_cache_port = f_port;
        request.arm.source.logical_job = 600000 + index;
        request.arm.source.compiler_attempt = 1;
        const auto result = f_runtime.reserve_p51_source_on_owner(request);
        CHECK(result.error_code == 0 && result.armed.has_value());
        requests.push_back(std::move(request));
        armed.push_back(*result.armed);
        CHECK(armed.back().selected_window == window);
        if (index != 0)
            CHECK(armed.back().logical_relationship_id ==
                  armed.front().logical_relationship_id &&
                  armed.back().relationship_epoch ==
                  armed.front().relationship_epoch);
    }

    D11ReceiptPause pause;
    std::atomic<bool> stop_accepting{false};
    std::atomic<bool> accepted{false};
    std::thread acceptor([&] {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::seconds(45);
        while (!stop_accepting.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < end) {
            pollfd ready{listener, POLLIN, 0};
            const int polled = ::poll(&ready, 1, 100);
            if (polled < 0 && errno == EINTR)
                continue;
            if (polled <= 0)
                continue;
            int fd = ::accept(listener, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            accepted.store(true, std::memory_order_release);
            f_runtime.start_adopted_r2_endpoint(fd);
            return;
        }
    });

    std::exception_ptr client_error;
    std::thread client_thread([&] {
        try {
            namespace asio = boost::asio;
            asio::io_context context;
            auto future = asio::co_spawn(
                context,
                d11_receipt_client(f_port, profile, window, c_launch,
                                   f_launch, armed, raw_inputs, pause),
                asio::use_future);
            context.run();
            future.get();
        } catch (...) {
            std::lock_guard lock(pause.mutex);
            client_error = std::current_exception();
            pause.changed.notify_all();
        }
    });

    const auto cleanup = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1), [&](int*) {
            {
                std::lock_guard lock(pause.mutex);
                pause.continue_after_cap = true;
                pause.finish = true;
            }
            pause.changed.notify_all();
            stop_accepting.store(true, std::memory_order_release);
            (void)::shutdown(listener, SHUT_RDWR);
            if (acceptor.joinable())
                acceptor.join();
            (void)::close(listener);
            if (client_thread.joinable())
                client_thread.join();
            f_runtime.stop();
        });

    {
        std::unique_lock lock(pause.mutex);
        CHECK(pause.changed.wait_for(lock, std::chrono::seconds(40), [&] {
            return pause.at_cap || client_error != nullptr;
        }));
        if (client_error)
            std::rethrow_exception(client_error);
        CHECK(accepted.load(std::memory_order_acquire));
    }

    const LinkHello link = pause.link;
    const auto full = f_runtime.p51_receipt_ledger_for_test(link);
    CHECK(full.has_value() && full->selected_window == window &&
          full->committed_prefix_k == window &&
          full->acknowledged_prefix_q == 0 &&
          full->pending_ordinal == 0 && full->receipt_count == window &&
          full->outstanding_reservations == 1 &&
          full->endpoint_usage.retained_input_records == window);
    for (uint32_t ordinal = 1; ordinal <= window; ++ordinal)
        CHECK(full->receipt_ordinals[ordinal - 1] == ordinal);
    {
        std::lock_guard lock(materialized_mutex);
        CHECK(materialized.size() == window);
        for (uint32_t ordinal = 0; ordinal != window; ++ordinal)
            CHECK(materialized[ordinal].first ==
                      icecc::digest128(raw_inputs[ordinal]) &&
                  materialized[ordinal].second == raw_inputs[ordinal].size());
    }

    // Probe the real owner admission with the exact next reservation while
    // the receipt ledger is full. Capacity rejection must be read-only: the
    // reservation remains outstanding and no pending ordinal/record appears.
    const JobBind blocked_binding = pause.refill_binding;
    CHECK(blocked_binding.reservation_id ==
              Id128{armed[window].reservation_id} &&
          blocked_binding.relationship_ordinal == window + 1 &&
          blocked_binding.raw_bytes == raw_inputs[window].size() &&
          blocked_binding.raw_digest == icecc::digest128(raw_inputs[window]));
    std::optional<P51SourceJobLease> blocked_lease;
    f_runtime.run_owner_callback_for_test([&] {
        blocked_lease = f_runtime.consume_p51_job_reservation_on_owner(
            link, blocked_binding);
    });
    CHECK(!blocked_lease.has_value());
    const auto still_full = f_runtime.p51_receipt_ledger_for_test(link);
    CHECK(still_full.has_value() && still_full->committed_prefix_k == window &&
          still_full->acknowledged_prefix_q == 0 &&
          still_full->pending_ordinal == 0 &&
          still_full->receipt_count == window &&
          still_full->outstanding_reservations == 1 &&
          still_full->endpoint_usage.retained_input_records == window);

    {
        std::lock_guard lock(pause.mutex);
        pause.continue_after_cap = true;
    }
    pause.changed.notify_all();
    {
        std::unique_lock lock(pause.mutex);
        CHECK(pause.changed.wait_for(lock, std::chrono::seconds(20), [&] {
            return pause.after_refill || client_error != nullptr;
        }));
        if (client_error)
            std::rethrow_exception(client_error);
    }
    std::optional<service::P51ReceiptLedgerSnapshot> refilled;
    const auto ack_wait_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(3);
    do {
        refilled = f_runtime.p51_receipt_ledger_for_test(link);
        if (refilled && refilled->acknowledged_prefix_q == window + 1)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < ack_wait_deadline);
    CHECK(refilled.has_value() &&
          refilled->selected_window == window &&
          refilled->committed_prefix_k == window + 1 &&
          refilled->acknowledged_prefix_q == window + 1 &&
          refilled->pending_ordinal == 0 && refilled->receipt_count == 0 &&
          refilled->outstanding_reservations == 0 &&
          refilled->endpoint_usage.retained_input_records == total_jobs);
    {
        std::lock_guard lock(materialized_mutex);
        CHECK(materialized.size() == total_jobs);
        for (size_t index = 0; index != total_jobs; ++index)
            CHECK(materialized[index].first ==
                      icecc::digest128(raw_inputs[index]) &&
                  materialized[index].second == raw_inputs[index].size());
    }
    {
        std::lock_guard lock(pause.mutex);
        pause.finish = true;
    }
    pause.changed.notify_all();
    client_thread.join();
    if (client_error)
        std::rethrow_exception(client_error);
    std::printf("P51_D11_REAL_RECEIPT_LEDGER profile=%s window=%u retained=%u refill=%u: PASS\n",
                profile_name, window, window, window + 1);
}

void test_p51_interrupted_reservation_relationship_isolation(
    bool wrong_link_identity_only = false) {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x3c;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    StoreIdentityRoot remote_roots[2]{};
    remote_roots[0].bytes[15] = 0xa1;
    remote_roots[1].bytes[15] = 0xa2;
    const auto owner_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);

    struct Reservation {
        local::P51SourceReservationRequest request;
        P51SourceArmedFields armed;
        LinkHello initial;
        LinkHello reconnect;
        JobBind binding;
    } reservations[2];
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_route_relationships = 4;
    std::vector<Id128> retired_ids;
    config.p51_reservation_retired_for_test =
        [&](Id128 id, bool) { retired_ids.push_back(id); };
    service::SidecarRuntime runtime(std::move(config));

    for (size_t index = 0; index != 2; ++index) {
        const CStoreGuid c_guid = c_store_guid_for_root(remote_roots[index]);
        reservations[index].request = test_p51_reservation_request(
            c_guid, 51, launch.identity.generation, launch.identity.attempt,
            8101 + index, CACHE_PROFILE_ZSTD_TU, 30);
        const auto result = runtime.reserve_p51_source_on_owner(
            reservations[index].request);
        CHECK(result.error_code == 0 && result.armed.has_value());
        reservations[index].armed = *result.armed;
        reservations[index].initial = test_p51_link_hello(
            reservations[index].armed, 1,
            HistoryNonce{0x810001 + index});
        reservations[index].binding = test_p51_job_binding(
            reservations[index].armed, 1, 1, 9101 + index,
            index == 0 ? "isolated-survivor" : "isolated-cancelled");
        runtime.run_owner_callback_for_test([&, index] {
            CHECK(runtime.lookup_p51_link_reservation_on_owner(
                      reservations[index].initial)
                      .has_value());
            CHECK(runtime.consume_p51_job_reservation_on_owner(
                      reservations[index].initial,
                      reservations[index].binding)
                      .has_value());
        });
    }

    const auto less_id = [](const Reservation& left,
                            const Reservation& right) {
        return left.armed.reservation_id < right.armed.reservation_id;
    };
    Reservation* survivor = &reservations[0];
    Reservation* cancelled = &reservations[1];
    if (less_id(*survivor, *cancelled))
        std::swap(survivor, cancelled);
    CHECK(cancelled->armed.reservation_id < survivor->armed.reservation_id);
    CHECK(runtime.cancel_p51_source_on_owner(
        cancelled->request.arm, cancelled->armed.reservation_id,
        owner_deadline));

    bool wrong_link_rejected = false;
    runtime.run_owner_callback_for_test([&] {
        for (Reservation* reservation : {survivor, cancelled}) {
            runtime.release_p51_link_on_owner(reservation->initial);
            reservation->reconnect = test_p51_link_hello(
                reservation->armed, 2, reservation->initial.history_nonce,
                LinkStartMode::Reconnect);
            CHECK(runtime.lookup_p51_link_reservation_on_owner(
                      reservation->reconnect)
                      .has_value());
        }

        if (wrong_link_identity_only) {
            // A link carrying the right C/generation but wrong relationship
            // identity must not settle the consumed survivor proof.
            LinkHello wrong_relationship = survivor->reconnect;
            wrong_relationship.relationship_id = Id128::from_u64(0xdeadbeef);
            wrong_link_rejected =
                !runtime.settle_p51_interrupted_job_on_owner(
                    wrong_relationship);
            return;
        }

        // The canceled ARM sorts first in the global map. Settling the
        // survivor's relationship must neither retire it early nor clear its
        // frozen proof merely because both rows have ordinal 1/generation 1.
        CHECK(runtime.settle_p51_interrupted_job_on_owner(
            survivor->reconnect));
        CHECK(retired_ids.empty());
        CHECK(runtime.settle_p51_interrupted_job_on_owner(
            cancelled->reconnect));
        CHECK(retired_ids.size() == 1 &&
              retired_ids.front() ==
                  Id128{cancelled->armed.reservation_id});
    });

    if (wrong_link_identity_only) {
        CHECK(wrong_link_rejected);
        std::puts("P51_INTERRUPTED_RESERVATION stale-link identity: PASS");
        return;
    }

    const Id128 operation_id{
        icecc::digest128("interrupted reservation isolation").bytes};
    const RecoverBegin begin = test_p51_recover_begin(
        survivor->reconnect, 0, 1, 1, operation_id);
    RecoverWitness witness;
    witness.relationship_id = begin.relationship_id;
    witness.relationship_epoch = begin.relationship_epoch;
    witness.physical_link_generation = begin.physical_link_generation;
    witness.operation_id = begin.operation_id;
    witness.relationship_ordinal = 1;
    witness.binding_digest = compute_r2_binding_digest(survivor->binding);
    witness.transaction_digest =
        icecc::digest128("pending interrupted transfer");
    witness.inner.history_nonce = survivor->initial.history_nonce;
    witness.inner.rel_seq = RelSeq{0};
    witness.inner.tu_seq = survivor->binding.tu_seq;
    witness.inner.profile = survivor->binding.profile;
    witness.inner.raw_bytes = survivor->binding.raw_bytes;
    witness.inner.raw_digest = survivor->binding.raw_digest;
    witness.inner.transaction_digest = witness.transaction_digest;
    witness.binding = survivor->binding;
    const std::array<RecoverWitness, 1> witnesses{witness};
    std::optional<P51RecoveryReceiptInterval> recovered;
    runtime.run_owner_callback_for_test([&] {
        recovered = runtime.recover_p51_receipts_on_owner(
            survivor->reconnect, begin, witnesses,
            test_p51_recover_end(begin, witnesses));
    });
    CHECK(recovered.has_value() && recovered->rows.empty() &&
          recovered->end.committed_prefix_k == 0 &&
          recovered->end.acknowledged_prefix_q == 0);

    ResetRequest reset;
    reset.relationship_id = survivor->reconnect.relationship_id;
    reset.old_relationship_epoch = survivor->reconnect.relationship_epoch;
    reset.new_relationship_epoch = reset.old_relationship_epoch + 1;
    reset.physical_link_generation =
        survivor->reconnect.physical_link_generation;
    reset.operation_id = operation_id;
    reset.settled_prefix_k = 0;
    reset.old_history_nonce = survivor->reconnect.history_nonce;
    reset.new_history_nonce =
        HistoryNonce{reset.old_history_nonce.value + 1};
    std::optional<ResetAck> ack;
    runtime.run_owner_callback_for_test([&] {
        ack = runtime.validate_p51_reset_on_owner(survivor->reconnect, reset);
        CHECK(ack.has_value());
        CHECK(runtime.commit_p51_reset_on_owner(
            survivor->reconnect, reset, *ack));
        ResetConfirm confirm;
        confirm.relationship_id = reset.relationship_id;
        confirm.new_relationship_epoch = reset.new_relationship_epoch;
        confirm.physical_link_generation = reset.physical_link_generation;
        confirm.operation_id = reset.operation_id;
        confirm.new_history_nonce = reset.new_history_nonce;
        confirm.settled_prefix_k = reset.settled_prefix_k;
        CHECK(runtime.confirm_p51_reset_on_owner(
            survivor->reconnect, confirm));
    });
    LinkHello reset_link = survivor->reconnect;
    reset_link.relationship_epoch = reset.new_relationship_epoch;
    reset_link.history_nonce = reset.new_history_nonce;
    const auto ledger = runtime.p51_receipt_ledger_for_test(reset_link);
    CHECK(ledger.has_value() && ledger->pending_ordinal == 0 &&
          ledger->committed_prefix_k == 0 &&
          ledger->outstanding_reservations == 1);
    std::puts("P51_INTERRUPTED_RESERVATION exact-C/relationship/epoch: PASS");
}

struct D11OutputCapPause {
    std::mutex mutex;
    std::condition_variable changed;
    bool abort = false;
    bool at_cap_before_probe = false;
    bool continue_cap_probe = false;
    bool cap_refusal_ready = false;
    bool cap_recovery_reset = false;
    uint32_t cap_recovery_unavailable_mask = 0;
    bool cap_cancel_accepted = false;
    bool continue_no_release = false;
    bool no_release_ready = false;
    bool no_release_refused = false;
    bool continue_refill = false;
    bool refill_ready = false;
    bool finish = false;
    LinkHello capped_link{};
    LinkHello refill_link{};
    JobBind first_binding{};
    JobBind second_binding{};
    JobBind rejected_binding{};
    JobBind blocked_binding{};
    JobBind refill_binding{};
    std::optional<ClientRunResult> first_result;
    std::optional<ClientRunResult> second_result;
    std::optional<ClientRunResult> refill_result;
    bool refused_by_terminal_close = false;
    std::exception_ptr error;
};

boost::asio::awaitable<void> d11_output_cap_client(
    uint16_t f_port, ProfileId profile, uint32_t window,
    bool expire_unpublished,
    const SidecarLaunchIdentity& c_launch,
    const SidecarLaunchIdentity& blocked_c_launch,
    const SidecarLaunchIdentity& fresh_c_launch,
    const SidecarLaunchIdentity& f_launch,
    std::array<P51SourceArmedFields, 3> capped_armed,
    const P51SourceArmedFields& blocked_armed,
    const P51SourceArmedFields& refill_armed,
    const std::array<std::vector<uint8_t>, 3>& capped_inputs,
    const std::vector<uint8_t>& refill_input,
    D11OutputCapPause& pause) {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;
    const char* profile_name = profile == ProfileId::P29V1 ? "P29V1" :
        profile == ProfileId::ZSTD_TU ? "ZSTD_TU" : "ZSTD_ROUTE";
    const auto make_authority = [&](const SidecarLaunchIdentity& client_launch) {
        EndpointCaps caps;
        caps.profile = profile;
        caps.supported_profiles = profile_bit(profile);
        caps.zstd.max_raw_bytes = 1U << 20;
        caps.zstd.max_encoded_body_bytes = 1U << 20;
        PreparationAuthorityLimits limits;
        limits.max_speculative_tus = 1;
        limits.max_speculative_raw_bytes = 1U << 20;
        auto authority = std::make_shared<P50PreparationAuthority>(
            client_launch.c_store_guid, caps.zstd, limits, 1,
            profile);
        return std::pair{std::move(caps), std::move(authority)};
    };
    const PreparationRouteKey route{
        FStoreGuid{f_launch.f_store_guid.bytes}, f_launch.store_generation,
        profile};
    const auto make_binding = [&](const P51SourceArmedFields& armed,
                                  P50PreparationAuthority& authority,
                                  PreparedTuHandle prepared,
                                  uint64_t ordinal,
                                  std::span<const uint8_t> raw) {
        const auto& source = armed.arm.source;
        JobBind binding;
        binding.reservation_id = Id128{armed.reservation_id};
        binding.physical_link_generation = 1;
        binding.relationship_ordinal = ordinal;
        binding.wire_job_id = source.wire_job_id;
        binding.assignment_epoch = source.assignment_epoch;
        binding.assignment_nonce = source.assignment_nonce;
        binding.logical_job = source.logical_job;
        binding.compiler_attempt = source.compiler_attempt;
        binding.source_request_id = source.source_request_id;
        binding.tu_seq = authority.prepared_tu_seq(prepared);
        binding.profile = profile;
        binding.raw_bytes = raw.size();
        binding.raw_digest = icecc::digest128(raw);
        return binding;
    };
    const auto prepare = [&](P50PreparationAuthority& authority,
                             size_t index, std::span<const uint8_t> raw) {
        return authority.prepare_for_route(
            route, PrepareRequestKey{0xd11c, index + 1}, raw);
    };
    try {
        auto [caps, authority] = make_authority(c_launch);
        P50ClientEndpoint client(authority, caps, HistoryNonce{1}, nullptr,
                                 nullptr, std::nullopt, {}, {}, route);
        tcp::socket socket(co_await asio::this_coro::executor);
        co_await socket.async_connect(
            tcp::endpoint(asio::ip::address_v4::loopback(), f_port),
            asio::use_awaitable);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(45);
        PreparedTuHandle prepared = prepare(*authority, 0, capped_inputs[0]);
        LinkHello link = test_p51_link_hello(
            capped_armed[0], 1, HistoryNonce{1}, LinkStartMode::Initial);
        link.max_raw_bytes = 1U << 20;
        link.max_encoded_bytes = 1U << 20;
        link.max_output_bytes = 1U << 20;
        link.system_source_fingerprint = profile == ProfileId::P29V1
            ? authority->p29v1_system_source_fingerprint(prepared)
            : icecc::digest128("D11 F output cap");
        const LinkState link_state =
            co_await client.open_r2_link(socket, link, deadline);
        CHECK(link_state.window == window && link_state.profile == profile);
        std::fprintf(stderr, "D11_STAGE initial-link-opened profile=%s window=%u\n",
                     profile_name, window);

        JobBind first_binding{};
        ClientRunResult first_result;
        for (size_t index = 0; index != 2; ++index) {
            if (index != 0)
                prepared = prepare(*authority, index, capped_inputs[index]);
            const JobBind binding = make_binding(
                capped_armed[index], *authority, prepared, index + 1,
                capped_inputs[index]);
            const R2SentBundle sent = co_await client.write_r2_bundle(
                socket, binding, prepared, deadline);
            const ClientRunResult result = co_await client.read_r2_receipt(
                socket, sent, deadline);
            std::fprintf(stderr,
                         "D11_STAGE initial-receipt index=%zu status=%u\n",
                         index, static_cast<unsigned>(result.status));
            CHECK(result.status == ClientRunStatus::Committed &&
                  result.committed_input.has_value() &&
                  result.committed_commit.has_value() &&
                  result.committed_commit->raw_digest ==
                      icecc::digest128(capped_inputs[index]));
            co_await client.write_r2_ack(socket, index + 1, deadline);
            if (index == 0) {
                first_binding = binding;
                first_result = result;
            } else {
                std::lock_guard lock(pause.mutex);
                pause.second_binding = binding;
                pause.second_result = result;
            }
        }

        {
            std::unique_lock lock(pause.mutex);
            pause.capped_link = link;
            pause.first_binding = first_binding;
            pause.first_result = first_result;
            pause.at_cap_before_probe = true;
            pause.changed.notify_all();
            pause.changed.wait(lock, [&] {
                return pause.continue_cap_probe || pause.abort;
            });
            if (pause.abort)
                co_return;
        }

        PreparedTuHandle rejected_prepared = prepare(
            *authority, 2, capped_inputs[2]);
        const JobBind rejected_binding = make_binding(
            capped_armed[2], *authority, rejected_prepared, 3,
            capped_inputs[2]);
        const R2SentBundle rejected_sent = co_await client.write_r2_bundle(
            socket, rejected_binding, rejected_prepared, deadline);
        bool refused_by_terminal_close = false;
        try {
            (void)co_await client.read_r2_receipt(
                socket, rejected_sent, deadline);
            throw std::logic_error(
                "F output-cap request unexpectedly returned a commit");
        } catch (const boost::system::system_error& error) {
            if (error.code() != asio::error::eof)
                throw;
            refused_by_terminal_close = true;
        }
        std::fprintf(stderr, "D11_STAGE same-link-cap-refusal=%u\n",
                     refused_by_terminal_close ? 1u : 0u);
        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
        socket.close(ignored);
        {
            std::unique_lock lock(pause.mutex);
            pause.capped_link = link;
            pause.first_binding = first_binding;
            pause.rejected_binding = rejected_binding;
            pause.first_result = first_result;
            pause.refused_by_terminal_close = refused_by_terminal_close;
            pause.cap_refusal_ready = true;
            pause.changed.notify_all();
            pause.changed.wait(lock, [&] {
                return pause.continue_no_release ||
                       (expire_unpublished && pause.finish) || pause.abort;
            });
            if (pause.abort || (expire_unpublished && pause.finish))
                co_return;
        }

        // The cap refusal happened after this request was consumed and
        // materialized. Cancel that exact reservation, then use the real
        // reconnect/RECOVER/RESET exchange to settle its unavailable suffix.
        // RESET clears P29 route history but deliberately preserves the two
        // committed InputRecords, so the later refill measures the retained
        // output-byte cap rather than the separate P29 segment budget.
        const std::vector<R2SentBundle> recovery_witnesses =
            client.r2_pending_witnesses(2);
        CHECK(recovery_witnesses.size() == 1 &&
              recovery_witnesses.front().binding.relationship_ordinal == 3 &&
              recovery_witnesses.front().binding == rejected_binding);
        LinkHello reconnect = link;
        reconnect.start_mode = LinkStartMode::Reconnect;
        reconnect.physical_link_generation = 2;
        reconnect.verified_receipt_floor = 2;
        const Id128 reset_operation{icecc::digest128(
            std::string("D11 cap unavailable reset/") + profile_name + "/" +
            std::to_string(window)).bytes};
        const HistoryNonce reset_nonce{
            0xd1100000ULL + static_cast<uint64_t>(profile) * 100 + window};
        tcp::socket recovery_socket(co_await asio::this_coro::executor);
        co_await recovery_socket.async_connect(
            tcp::endpoint(asio::ip::address_v4::loopback(), f_port),
            asio::use_awaitable);
        const R2RecoveryResult recovered = co_await client.recover_r2_link(
            recovery_socket, reconnect, recovery_witnesses, 2,
            reset_operation, reconnect.relationship_epoch + 1,
            reset_nonce, deadline);
        CHECK(recovered.committed_receipts.empty() &&
              recovered.reset_request.settled_prefix_k == 2 &&
              recovered.reset_ack.recovery_verified_floor_a == 2 &&
              recovered.reset_ack.recovery_prepared_prefix_p == 3 &&
              recovered.reset_ack.unavailable_suffix_mask == 1);
        {
            std::lock_guard lock(pause.mutex);
            pause.cap_recovery_reset = true;
            pause.cap_recovery_unavailable_mask =
                recovered.reset_ack.unavailable_suffix_mask;
        }
        recovery_socket.shutdown(tcp::socket::shutdown_both, ignored);
        recovery_socket.close(ignored);
        std::fprintf(stderr,
                     "D11_STAGE cap-recovery-reset K=2 P=3 unavailable=%u\n",
                     recovered.reset_ack.unavailable_suffix_mask);

        // A distinct C identity must also be refused while F remains full;
        // this separates cap enforcement from the first connection closing.
        auto [blocked_caps, blocked_authority] = make_authority(blocked_c_launch);
        P50ClientEndpoint blocked_client(
            blocked_authority, blocked_caps, HistoryNonce{1}, nullptr, nullptr,
            std::nullopt, {}, {}, route);
        tcp::socket blocked_socket(co_await asio::this_coro::executor);
        co_await blocked_socket.async_connect(
            tcp::endpoint(asio::ip::address_v4::loopback(), f_port),
            asio::use_awaitable);
        std::fprintf(stderr, "D11_STAGE blocked-c-connected profile=%s\n",
                     profile_name);
        PreparedTuHandle blocked_prepared = prepare(
            *blocked_authority, 0, refill_input);
        LinkHello blocked_link = test_p51_link_hello(
            blocked_armed, 1, HistoryNonce{1}, LinkStartMode::Initial);
        blocked_link.max_raw_bytes = 1U << 20;
        blocked_link.max_encoded_bytes = 1U << 20;
        blocked_link.max_output_bytes = 1U << 20;
        blocked_link.system_source_fingerprint = profile == ProfileId::P29V1
            ? blocked_authority->p29v1_system_source_fingerprint(blocked_prepared)
            : icecc::digest128("D11 F output cap");
        (void)co_await blocked_client.open_r2_link(
            blocked_socket, blocked_link, deadline);
        std::fprintf(stderr, "D11_STAGE blocked-c-link-opened profile=%s\n",
                     profile_name);
        const JobBind blocked_binding = make_binding(
            blocked_armed, *blocked_authority, blocked_prepared, 1,
            refill_input);
        const R2SentBundle blocked_sent = co_await blocked_client.write_r2_bundle(
            blocked_socket, blocked_binding, blocked_prepared, deadline);
        bool no_release_refused = false;
        try {
            (void)co_await blocked_client.read_r2_receipt(
                blocked_socket, blocked_sent, deadline);
            throw std::logic_error(
                "F output-cap request committed before any lease release");
        } catch (const boost::system::system_error& error) {
            if (error.code() != asio::error::eof)
                throw;
            no_release_refused = true;
        }
        std::fprintf(stderr, "D11_STAGE no-release-refusal=%u\n",
                     no_release_refused ? 1u : 0u);
        blocked_socket.shutdown(tcp::socket::shutdown_both, ignored);
        blocked_socket.close(ignored);
        {
            std::unique_lock lock(pause.mutex);
            pause.no_release_refused = no_release_refused;
            pause.blocked_binding = blocked_binding;
            pause.no_release_ready = true;
            pause.changed.notify_all();
            pause.changed.wait(lock, [&] {
                return pause.continue_refill || pause.abort;
            });
            if (pause.abort)
                co_return;
        }

        auto [fresh_caps, fresh_authority] = make_authority(fresh_c_launch);
        P50ClientEndpoint fresh_client(
            fresh_authority, fresh_caps, HistoryNonce{1}, nullptr, nullptr,
            std::nullopt, {}, {}, route);
        tcp::socket fresh_socket(co_await asio::this_coro::executor);
        co_await fresh_socket.async_connect(
            tcp::endpoint(asio::ip::address_v4::loopback(), f_port),
            asio::use_awaitable);
        PreparedTuHandle fresh_prepared = prepare(
            *fresh_authority, 0, refill_input);
        LinkHello fresh_link = test_p51_link_hello(
            refill_armed, 1, HistoryNonce{1}, LinkStartMode::Initial);
        fresh_link.max_raw_bytes = 1U << 20;
        fresh_link.max_encoded_bytes = 1U << 20;
        fresh_link.max_output_bytes = 1U << 20;
        fresh_link.system_source_fingerprint = profile == ProfileId::P29V1
            ? fresh_authority->p29v1_system_source_fingerprint(fresh_prepared)
            : icecc::digest128("D11 F output cap");
        const LinkState fresh_state = co_await fresh_client.open_r2_link(
            fresh_socket, fresh_link, deadline);
        std::fprintf(stderr, "D11_STAGE fresh-c-link-opened profile=%s\n",
                     profile_name);
        CHECK(fresh_state.window == window &&
              fresh_state.profile == profile);
        const JobBind refill_binding = make_binding(
            refill_armed, *fresh_authority, fresh_prepared, 1,
            refill_input);
        const R2SentBundle refill_sent = co_await fresh_client.write_r2_bundle(
            fresh_socket, refill_binding, fresh_prepared, deadline);
        const ClientRunResult refill_result =
            co_await fresh_client.read_r2_receipt(
                fresh_socket, refill_sent, deadline);
        CHECK(refill_result.status == ClientRunStatus::Committed &&
              refill_result.committed_input.has_value() &&
              refill_result.committed_commit.has_value() &&
              refill_result.committed_commit->raw_digest ==
                  icecc::digest128(refill_input));
        std::fprintf(stderr, "D11_STAGE post-release-refill-committed profile=%s\n",
                     profile_name);
        co_await fresh_client.write_r2_ack(fresh_socket, 1, deadline);
        {
            std::unique_lock lock(pause.mutex);
            pause.refill_link = fresh_link;
            pause.refill_binding = refill_binding;
            pause.refill_result = refill_result;
            pause.refill_ready = true;
            pause.changed.notify_all();
            pause.changed.wait(lock, [&] { return pause.finish || pause.abort; });
            if (pause.abort)
                co_return;
        }
        const std::vector<uint8_t> close_frame =
            encode_frame(Message{CloseMessage{}});
        co_await asio::async_write(fresh_socket, asio::buffer(close_frame),
                                   asio::use_awaitable);
        fresh_socket.shutdown(tcp::socket::shutdown_both, ignored);
        fresh_socket.close(ignored);
    } catch (...) {
        std::lock_guard lock(pause.mutex);
        pause.error = std::current_exception();
        pause.changed.notify_all();
    }
}

void test_p51_d11_real_f_output_byte_cap(ProfileId profile,
                                         uint32_t window,
                                         bool expire_unpublished = false) {
    CHECK(window == 1 || window == 30);
    const char* profile_name = profile == ProfileId::P29V1 ? "P29V1" :
        profile == ProfileId::ZSTD_TU ? "ZSTD_TU" : "ZSTD_ROUTE";
    const uint32_t cache_profile = profile == ProfileId::P29V1
        ? CACHE_PROFILE_P29V1
        : profile == ProfileId::ZSTD_TU
              ? CACHE_PROFILE_ZSTD_TU
              : CACHE_PROFILE_ZSTD_ROUTE;
    constexpr size_t kRawBytes = 96;
    constexpr uint64_t kOutputCap = 2 * kRawBytes;
    const auto deadline_lifetime = std::chrono::seconds(60);
    StoreIdentityRoot c_root{};
    c_root.bytes[15] = 0xa1;
    const SidecarLaunchIdentity c_launch = test_sidecar_launch(c_root);
    StoreIdentityRoot fresh_c_root{};
    fresh_c_root.bytes[15] = 0xa2;
    const SidecarLaunchIdentity fresh_c_launch =
        test_sidecar_launch(fresh_c_root);
    StoreIdentityRoot blocked_c_root{};
    blocked_c_root.bytes[15] = 0xa4;
    const SidecarLaunchIdentity blocked_c_launch =
        test_sidecar_launch(blocked_c_root);
    StoreIdentityRoot f_root{};
    f_root.bytes[15] = 0xa3;
    const SidecarLaunchIdentity f_launch = test_sidecar_launch(f_root);

    uint16_t f_port = 0;
    const int listener = loopback_listener(f_port);
    CHECK(listener >= 0 && f_port != 0);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = f_launch.c_store_guid;
    config.f_store_guid = f_launch.f_store_guid;
    config.f_store_generation = f_launch.store_generation;
    config.sidecar_launch = f_launch;
    config.endpoint_caps.profile = profile;
    config.endpoint_caps.supported_profiles = profile_bit(profile);
    config.endpoint_caps.zstd.max_raw_bytes = 1U << 20;
    config.endpoint_caps.zstd.max_encoded_body_bytes = 1U << 20;
    config.endpoint_config.owner_limits.max_retained_input_records = 8;
    config.endpoint_config.owner_limits.max_retained_input_bytes = kOutputCap;
    GlobalResourceTrace resource_trace;
    config.endpoint_config.global_resource_trace = &resource_trace;
    config.max_pending_p51_source_reservations = 8;
    std::mutex retired_mutex;
    std::vector<std::pair<Id128, bool>> retired_reservations;
    config.p51_reservation_retired_for_test =
        [&](Id128 reservation_id, bool marker_retired) {
            std::lock_guard lock(retired_mutex);
            retired_reservations.emplace_back(reservation_id,
                                              marker_retired);
        };
    std::mutex materialized_mutex;
    std::vector<std::pair<Digest128, uint64_t>> materialized;
    config.endpoint_config.input_job_state =
        [&](CStoreGuid, const TxBegin& begin, const TxCommit&,
            std::span<const uint8_t> bytes) {
            std::lock_guard lock(materialized_mutex);
            materialized.emplace_back(icecc::digest128(bytes), bytes.size());
            CHECK(begin.raw_digest == icecc::digest128(bytes) &&
                  begin.raw_bytes == bytes.size());
            return InputJobState::Open;
        };
    service::SidecarRuntime runtime(std::move(config));
    const auto trace_resident = [&] {
        uint64_t resident = 0;
        uint64_t peak = 0;
        size_t presents = 0;
        size_t releases = 0;
        runtime.run_owner_callback_for_test([&] {
            for (const auto& record : resource_trace.records()) {
                if (record.action == GlobalActionType::ARENA_PRESENT) {
                    resident += record.bytes;
                    peak = std::max(peak, resident);
                    ++presents;
                } else if (record.action == GlobalActionType::ARENA_RELEASED) {
                    CHECK(resident >= record.bytes);
                    resident -= record.bytes;
                    ++releases;
                }
            }
        });
        return std::array<uint64_t, 4>{
            resident, peak, presents, releases};
    };

    std::array<std::vector<uint8_t>, 3> capped_inputs;
    capped_inputs[0].assign(kRawBytes, 0x41);
    capped_inputs[1].assign(kRawBytes, 0x52);
    capped_inputs[2].assign(kRawBytes, 0x73);
    const std::vector<uint8_t> refill_input = capped_inputs[2];
    const std::array<SidecarLaunchIdentity, 5> client_launches{
        c_launch, c_launch, c_launch, blocked_c_launch, fresh_c_launch};
    std::array<local::P51SourceReservationRequest, 5> requests;
    std::array<P51SourceArmedFields, 5> armed;
    for (size_t index = 0; index != requests.size(); ++index) {
        const SidecarLaunchIdentity& client_launch = client_launches[index];
        requests[index] = test_p51_reservation_request(
            client_launch.c_store_guid, client_launch.store_generation,
            client_launch.identity.generation, client_launch.identity.attempt,
            940000 + index, cache_profile, window,
            expire_unpublished && index == 2
                ? std::chrono::seconds(12)
                : deadline_lifetime);
        requests[index].arm.source.selected_f_host = "127.0.0.1";
        requests[index].arm.source.selected_f_cache_port = f_port;
        requests[index].arm.source.logical_job = 710000 + index;
        requests[index].arm.source.assignment_nonce = 720000 + index;
        const auto result = runtime.reserve_p51_source_on_owner(requests[index]);
        CHECK(result.error_code == 0 && result.armed.has_value() &&
              result.armed->selected_window == window);
        armed[index] = *result.armed;
    }
    CHECK(armed[0].logical_relationship_id ==
              armed[1].logical_relationship_id &&
          armed[1].logical_relationship_id ==
              armed[2].logical_relationship_id);

    std::mutex errors_mutex;
    std::vector<ErrorMessage> endpoint_errors;
    std::atomic<size_t> accepted_connections{0};
    std::atomic<bool> stop_accepting{false};
    std::thread acceptor([&] {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::seconds(50);
        while (!stop_accepting.load(std::memory_order_acquire) &&
               accepted_connections.load(std::memory_order_acquire) < 4 &&
               std::chrono::steady_clock::now() < end) {
            pollfd ready{listener, POLLIN, 0};
            int polled;
            do {
                polled = ::poll(&ready, 1, 100);
            } while (polled < 0 && errno == EINTR);
            if (polled <= 0 || !(ready.revents & POLLIN))
                continue;
            const int fd = ::accept(listener, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            accepted_connections.fetch_add(1, std::memory_order_acq_rel);
            EndpointIoControl control;
            control.outbound_message_observer =
                [&](ActorSide actor, const Message& message) {
                    if (actor != ActorSide::F)
                        return;
                    if (const auto* error = std::get_if<ErrorMessage>(&message)) {
                        std::lock_guard lock(errors_mutex);
                        endpoint_errors.push_back(*error);
                        std::fprintf(stderr,
                                     "D11_STAGE endpoint-error code=%d\n",
                                     static_cast<int>(error->code));
                    }
                };
            runtime.start_adopted_r2_endpoint(fd, std::move(control));
        }
    });

    D11OutputCapPause pause;
    std::thread client([&] {
        try {
            namespace asio = boost::asio;
            asio::io_context context;
            auto future = asio::co_spawn(
                context,
                d11_output_cap_client(
                    f_port, profile, window, expire_unpublished, c_launch,
                    blocked_c_launch,
                    fresh_c_launch, f_launch,
                    std::array<P51SourceArmedFields, 3>{
                        armed[0], armed[1], armed[2]},
                    armed[3], armed[4], capped_inputs, refill_input, pause),
                asio::use_future);
            context.run();
            future.get();
        } catch (...) {
            try {
                throw;
            } catch (const std::exception& error) {
                std::fprintf(stderr, "D11_STAGE client-exception %s\n",
                             error.what());
            } catch (...) {
                std::fprintf(stderr, "D11_STAGE client-exception unknown\n");
            }
            std::lock_guard lock(pause.mutex);
            pause.error = std::current_exception();
            pause.changed.notify_all();
        }
    });

    const auto cleanup = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1), [&](int*) {
            {
                std::lock_guard lock(pause.mutex);
                pause.abort = true;
                pause.continue_cap_probe = true;
                pause.continue_no_release = true;
                pause.continue_refill = true;
                pause.finish = true;
            }
            pause.changed.notify_all();
            stop_accepting.store(true, std::memory_order_release);
            (void)::shutdown(listener, SHUT_RDWR);
            if (acceptor.joinable())
                acceptor.join();
            (void)::close(listener);
            if (client.joinable())
                client.join();
            runtime.stop();
        });

    {
        std::unique_lock lock(pause.mutex);
        CHECK(pause.changed.wait_for(lock, std::chrono::seconds(25), [&] {
            return pause.at_cap_before_probe || pause.error != nullptr;
        }));
        if (pause.error)
            std::rethrow_exception(pause.error);
    }
    const LinkHello capped_link = pause.capped_link;
    const JobBind first_binding = pause.first_binding;
    const ClientRunResult first_result = *pause.first_result;
    const JobBind second_binding = pause.second_binding;
    const ClientRunResult second_result = *pause.second_result;
    std::optional<service::P51ReceiptLedgerSnapshot> capped;
    const auto ack_wait_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < ack_wait_deadline) {
        capped = runtime.p51_receipt_ledger_for_test(capped_link);
        if (capped && capped->acknowledged_prefix_q == 2)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(capped.has_value() && capped->selected_window == window &&
          capped->committed_prefix_k == 2 &&
          capped->acknowledged_prefix_q == 2 &&
          capped->pending_ordinal == 0 && capped->receipt_count == 0 &&
          capped->endpoint_usage.retained_input_records == 2 &&
          capped->endpoint_usage.retained_input_bytes == kOutputCap &&
          capped->endpoint_usage.pending_encoded_bytes == 0 &&
          capped->endpoint_usage.pending_raw_bytes == 0);
    const auto lifecycle_before_cap =
        runtime.input_lifecycle_owner_count_for_test();
    CHECK(lifecycle_before_cap.has_value() && *lifecycle_before_cap == 2);
    {
        const auto totals = trace_resident();
        std::fprintf(stderr,
                     "D11_TRACE after-two resident=%llu peak=%llu presents=%llu releases=%llu\n",
                     static_cast<unsigned long long>(totals[0]),
                     static_cast<unsigned long long>(totals[1]),
                     static_cast<unsigned long long>(totals[2]),
                     static_cast<unsigned long long>(totals[3]));
    }
    {
        std::lock_guard lock(pause.mutex);
        pause.continue_cap_probe = true;
    }
    pause.changed.notify_all();
    {
        std::unique_lock lock(pause.mutex);
        CHECK(pause.changed.wait_for(lock, std::chrono::seconds(25), [&] {
            return pause.cap_refusal_ready || pause.error != nullptr;
        }));
        if (pause.error)
            std::rethrow_exception(pause.error);
    }
    const JobBind rejected_binding = pause.rejected_binding;
    CHECK(pause.refused_by_terminal_close);
    if (expire_unpublished) {
        const auto lifecycle_after_expiry =
            runtime.input_lifecycle_owner_count_for_test();
        CHECK(lifecycle_after_expiry == lifecycle_before_cap);
        const auto expiry_deadline =
            requests[2].absolute_deadline.as_steady_time_point();
        const auto retirement_deadline = expiry_deadline +
                                         std::chrono::seconds(5);
        bool exact_retirement_seen = false;
        while (std::chrono::steady_clock::now() < retirement_deadline) {
            {
                std::lock_guard lock(retired_mutex);
                exact_retirement_seen = std::any_of(
                    retired_reservations.begin(), retired_reservations.end(),
                    [&](const auto& retired) {
                        return retired.first ==
                               Id128{armed[2].reservation_id};
                    });
            }
            if (exact_retirement_seen)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(std::chrono::steady_clock::now() >= expiry_deadline &&
              exact_retirement_seen && !pause.cap_recovery_reset &&
              accepted_connections.load(std::memory_order_acquire) == 1);
        {
            std::lock_guard lock(retired_mutex);
            CHECK(retired_reservations.size() == 1 &&
                  retired_reservations.front().first ==
                      Id128{armed[2].reservation_id});
        }
        const auto lifecycle_after_cap =
            runtime.input_lifecycle_owner_count_for_test();
        CHECK(lifecycle_after_cap == lifecycle_before_cap);

        const ClientRunResult first_result = *pause.first_result;
        const ClientRunResult second_result = *pause.second_result;
        const JobBind first_binding = pause.first_binding;
        const JobBind second_binding = pause.second_binding;
        for (const auto& [key, binding, raw, request_id] : {
                 std::tuple<InputRecordKey, JobBind,
                            const std::vector<uint8_t>*, uint64_t>{
                     *first_result.committed_input, first_binding,
                     &capped_inputs[0], 950012},
                 std::tuple<InputRecordKey, JobBind,
                            const std::vector<uint8_t>*, uint64_t>{
                     *second_result.committed_input, second_binding,
                     &capped_inputs[1], 950013}}) {
            const InputFdRequest attach{
                f_launch.identity, key,
                InputLeaseOwner{binding.logical_job,
                                binding.assignment_epoch,
                                binding.assignment_nonce},
                request_id};
            auto cursor = runtime.attach_input_on_owner(
                attach, requests[0].absolute_deadline.as_steady_time_point());
            CHECK(cursor.has_value() && cursor->remaining() == raw->size() &&
                  cursor->raw_digest() == icecc::digest128(*raw));
            std::vector<uint8_t> bytes(raw->size());
            CHECK(cursor->read(bytes) == bytes.size() && bytes == *raw);
            cursor.reset();
            runtime.finish_input_attachment_on_owner(
                attach, true,
                requests[0].absolute_deadline.as_steady_time_point());
        }
        LinkHello other_c_link = test_p51_link_hello(
            armed[3], 1, HistoryNonce{0xd1101000},
            LinkStartMode::Initial);
        P51SourceLinkLookupResult other_c_lookup;
        runtime.run_owner_callback_for_test([&] {
            other_c_lookup = runtime.lookup_p51_link_reservation_on_owner(
                other_c_link);
        });
        CHECK(other_c_lookup.status == P51SourceLinkLookupStatus::Found &&
              other_c_lookup.lease.has_value());
        runtime.run_owner_callback_for_test([&] {
            runtime.release_p51_link_on_owner(other_c_link);
        });
        {
            std::lock_guard lock(pause.mutex);
            pause.finish = true;
        }
        pause.changed.notify_all();
        std::printf(
            "P51_D11_UNPUBLISHED_EXPIRY profile=%s window=%u exact-retirement/no-reconnect/prior-inputs/other-C: PASS\n",
            profile_name, window);
        return;
    }
    const bool cancel_after_materialization = runtime.cancel_p51_source_on_owner(
        requests[2].arm, armed[2].reservation_id,
        requests[2].absolute_deadline.as_steady_time_point());
    {
        std::lock_guard lock(pause.mutex);
        pause.cap_cancel_accepted = cancel_after_materialization;
    }
    std::fprintf(stderr,
                 "D11_STAGE cancel-after-materialization=%u\n",
                 cancel_after_materialization ? 1u : 0u);
    CHECK(cancel_after_materialization);
    std::optional<size_t> lifecycle_after_cap;
    const auto lifecycle_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < lifecycle_deadline) {
        lifecycle_after_cap =
            runtime.input_lifecycle_owner_count_for_test();
        if (lifecycle_after_cap == lifecycle_before_cap)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(lifecycle_after_cap == lifecycle_before_cap);
    {
        std::lock_guard lock(errors_mutex);
        CHECK(endpoint_errors.empty());
    }
    {
        std::lock_guard lock(pause.mutex);
        pause.continue_no_release = true;
    }
    pause.changed.notify_all();
    {
        std::unique_lock lock(pause.mutex);
        CHECK(pause.changed.wait_for(lock, std::chrono::seconds(25), [&] {
            return pause.no_release_ready || pause.error != nullptr;
        }));
        if (pause.error)
            std::rethrow_exception(pause.error);
        CHECK(pause.no_release_refused);
    }
    const JobBind blocked_binding = pause.blocked_binding;
    CHECK(pause.cap_recovery_reset);
    std::fprintf(stderr,
                 "D11_STAGE reset-disposition cancel-accepted=%u unavailable=%u\n",
                 pause.cap_cancel_accepted ? 1u : 0u,
                 pause.cap_recovery_unavailable_mask);
    {
        const auto totals = trace_resident();
        std::fprintf(stderr,
                     "D11_TRACE after-no-release resident=%llu peak=%llu presents=%llu releases=%llu\n",
                     static_cast<unsigned long long>(totals[0]),
                     static_cast<unsigned long long>(totals[1]),
                     static_cast<unsigned long long>(totals[2]),
                     static_cast<unsigned long long>(totals[3]));
    }
    const InputFdRequest blocked_attach{
        f_launch.identity,
        InputRecordKey{blocked_c_launch.c_store_guid, blocked_binding.tu_seq},
        InputLeaseOwner{blocked_binding.logical_job,
                        blocked_binding.assignment_epoch,
                        blocked_binding.assignment_nonce},
        950007};
    CHECK(!runtime.attach_input_on_owner(
        blocked_attach,
        requests[3].absolute_deadline.as_steady_time_point()).has_value());
    {
        std::lock_guard lock(materialized_mutex);
        CHECK(materialized.size() == 4);
        CHECK(materialized[0] == std::make_pair(
                  icecc::digest128(capped_inputs[0]),
                  static_cast<uint64_t>(capped_inputs[0].size())));
        CHECK(materialized[1] == std::make_pair(
                  icecc::digest128(capped_inputs[1]),
                  static_cast<uint64_t>(capped_inputs[1].size())));
        CHECK(materialized[2] == std::make_pair(
                  icecc::digest128(capped_inputs[2]),
                  static_cast<uint64_t>(capped_inputs[2].size())));
        CHECK(materialized[3] == std::make_pair(
                  icecc::digest128(refill_input),
                  static_cast<uint64_t>(refill_input.size())));
    }
    CHECK(first_result.committed_input.has_value() &&
          first_binding.raw_digest == icecc::digest128(capped_inputs[0]) &&
          second_result.committed_input.has_value() &&
          second_binding.raw_digest == icecc::digest128(capped_inputs[1]) &&
          rejected_binding.relationship_ordinal == 3 &&
          rejected_binding.raw_digest == icecc::digest128(capped_inputs[2]));
    const InputRecordKey rejected_key{
        c_launch.c_store_guid, rejected_binding.tu_seq};
    const InputFdRequest rejected_attach{
        f_launch.identity, rejected_key,
        InputLeaseOwner{rejected_binding.logical_job,
                        rejected_binding.assignment_epoch,
                        rejected_binding.assignment_nonce},
        950001};
    const auto nonexistent = runtime.attach_input_on_owner(
        rejected_attach,
        requests[1].absolute_deadline.as_steady_time_point());
    CHECK(!nonexistent.has_value());

    const InputRecordKey first_key = *first_result.committed_input;
    const InputLeaseOwner first_owner{
        first_binding.logical_job, first_binding.assignment_epoch,
        first_binding.assignment_nonce};
    const InputFdRequest attach_first{
        f_launch.identity, first_key, first_owner, 950002};
    auto first_cursor = runtime.attach_input_on_owner(
        attach_first, requests[0].absolute_deadline.as_steady_time_point());
    CHECK(first_cursor.has_value() &&
          first_cursor->remaining() == capped_inputs[0].size() &&
          first_cursor->raw_digest() == icecc::digest128(capped_inputs[0]));
    std::vector<uint8_t> attached_first(capped_inputs[0].size());
    CHECK(first_cursor->read(attached_first) == attached_first.size() &&
          attached_first == capped_inputs[0]);
    first_cursor.reset();
    runtime.finish_input_attachment_on_owner(
        attach_first, true,
        requests[0].absolute_deadline.as_steady_time_point());

    const InputRecordKey second_key = *second_result.committed_input;
    const InputFdRequest attach_second{
        f_launch.identity, second_key,
        InputLeaseOwner{second_binding.logical_job,
                        second_binding.assignment_epoch,
                        second_binding.assignment_nonce},
        950006};
    auto second_cursor = runtime.attach_input_on_owner(
        attach_second, requests[1].absolute_deadline.as_steady_time_point());
    CHECK(second_cursor.has_value() &&
          second_cursor->remaining() == capped_inputs[1].size() &&
          second_cursor->raw_digest() == icecc::digest128(capped_inputs[1]));
    std::vector<uint8_t> attached_second(capped_inputs[1].size());
    CHECK(second_cursor->read(attached_second) == attached_second.size() &&
          attached_second == capped_inputs[1]);
    second_cursor.reset();
    runtime.finish_input_attachment_on_owner(
        attach_second, true,
        requests[1].absolute_deadline.as_steady_time_point());

    InputLifecycleRequest close_first;
    close_first.identity = f_launch.identity;
    close_first.key = first_key;
    close_first.owner = first_owner;
    close_first.operation_id = 950003;
    close_first.action = InputLifecycleAction::CloseLogicalInputLease;
    close_first.f_store_generation = f_launch.store_generation;
    close_first.f_store_guid = f_launch.f_store_guid;
    close_first.immutable_size = capped_inputs[0].size();
    close_first.immutable_digest = icecc::digest128(capped_inputs[0]);
    close_first.retirement_id = 950004;
    close_first.absolute_deadline = requests[0].absolute_deadline;
    close_first.deadline =
        requests[0].absolute_deadline.as_steady_time_point();
    CHECK(runtime.apply_input_lifecycle_on_owner(
              close_first, close_first.deadline) ==
          InputLifecycleApplyStatus::JobClosedRecordReclaimed);
    {
        const auto totals = trace_resident();
        std::fprintf(stderr,
                     "D11_TRACE after-close-first resident=%llu peak=%llu presents=%llu releases=%llu\n",
                     static_cast<unsigned long long>(totals[0]),
                     static_cast<unsigned long long>(totals[1]),
                     static_cast<unsigned long long>(totals[2]),
                     static_cast<unsigned long long>(totals[3]));
    }
    {
        std::lock_guard lock(pause.mutex);
        pause.continue_refill = true;
    }
    pause.changed.notify_all();
    {
        std::unique_lock lock(pause.mutex);
        CHECK(pause.changed.wait_for(lock, std::chrono::seconds(20), [&] {
            return pause.refill_ready || pause.error != nullptr;
        }));
        if (pause.error) {
            const auto totals = trace_resident();
            std::fprintf(stderr,
                         "D11_TRACE refill-error resident=%llu peak=%llu presents=%llu releases=%llu\n",
                         static_cast<unsigned long long>(totals[0]),
                         static_cast<unsigned long long>(totals[1]),
                         static_cast<unsigned long long>(totals[2]),
                         static_cast<unsigned long long>(totals[3]));
            std::rethrow_exception(pause.error);
        }
    }
    const LinkHello refill_link = pause.refill_link;
    const JobBind refill_binding = pause.refill_binding;
    const ClientRunResult refill_result = *pause.refill_result;
    std::optional<service::P51ReceiptLedgerSnapshot> refilled;
    const auto refill_ack_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < refill_ack_deadline) {
        refilled = runtime.p51_receipt_ledger_for_test(refill_link);
        if (refilled && refilled->acknowledged_prefix_q == 1)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(refilled.has_value() && refilled->selected_window == window &&
          refilled->committed_prefix_k == 1 &&
          refilled->acknowledged_prefix_q == 1 &&
          refilled->receipt_count == 0 &&
          refilled->endpoint_usage.retained_input_records == 2 &&
          refilled->endpoint_usage.retained_input_bytes == kOutputCap &&
          refill_result.committed_input.has_value() &&
          refill_binding.raw_digest == icecc::digest128(refill_input));
    const InputFdRequest attach_refill{
        f_launch.identity, *refill_result.committed_input,
        InputLeaseOwner{refill_binding.logical_job,
                        refill_binding.assignment_epoch,
                        refill_binding.assignment_nonce},
        950005};
    auto refill_cursor = runtime.attach_input_on_owner(
        attach_refill,
        requests[4].absolute_deadline.as_steady_time_point());
    CHECK(refill_cursor.has_value() &&
          refill_cursor->remaining() == refill_input.size() &&
          refill_cursor->raw_digest() == icecc::digest128(refill_input));
    std::vector<uint8_t> attached_refill(refill_input.size());
    CHECK(refill_cursor->read(attached_refill) == attached_refill.size() &&
          attached_refill == refill_input);
    refill_cursor.reset();
    runtime.finish_input_attachment_on_owner(
        attach_refill, true,
        requests[4].absolute_deadline.as_steady_time_point());
    {
        std::lock_guard lock(materialized_mutex);
        CHECK(materialized.size() == 5 &&
              materialized[4] == std::make_pair(
                  icecc::digest128(refill_input),
                  static_cast<uint64_t>(refill_input.size())));
    }
    {
        std::lock_guard lock(pause.mutex);
        pause.finish = true;
    }
    pause.changed.notify_all();
    client.join();
    if (pause.error)
        std::rethrow_exception(pause.error);
    CHECK(accepted_connections.load(std::memory_order_acquire) == 4);
    std::printf("P51_D11_REAL_F_OUTPUT_BYTE_CAP profile=%s window=%u cap/refusal/close/refill: PASS\n",
                profile_name, window);
}

enum class D11PendingBudgetKind {
    Encoded,
    Raw,
    DecoderWindow,
};

const char* d11_pending_budget_name(D11PendingBudgetKind kind) noexcept {
    switch (kind) {
    case D11PendingBudgetKind::Encoded: return "encoded";
    case D11PendingBudgetKind::Raw: return "raw";
    case D11PendingBudgetKind::DecoderWindow: return "decoder-window";
    }
    return "invalid";
}

struct D11PendingBudgetGate {
    std::mutex mutex;
    std::condition_variable changed;
    size_t entered = 0;
    bool release = false;
};

struct D11PendingBudgetClientState {
    std::mutex mutex;
    std::condition_variable changed;
    bool link_opened = false;
    bool bundle_attempted = false;
    bool bundle_written = false;
    bool refused = false;
    bool committed = false;
    bool done = false;
    bool hold_after_ack = false;
    bool continue_after_ack = false;
    uint64_t encoded_bytes = 0;
    LinkHello link{};
    JobBind binding{};
    std::optional<ClientRunResult> result;
    std::exception_ptr error;
};

std::vector<uint8_t> d11_encoded_cap_input(size_t size) {
    std::vector<uint8_t> input(size);
    uint32_t state = 0x6d2b79f5U;
    for (uint8_t& byte : input) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<uint8_t>(state >> 24);
    }
    return input;
}

boost::asio::awaitable<void> d11_pending_budget_client(
    uint16_t f_port, uint32_t window,
    const SidecarLaunchIdentity& c_launch,
    const SidecarLaunchIdentity& f_launch,
    const P51SourceArmedFields& armed,
    std::span<const uint8_t> raw,
    D11PendingBudgetClientState& state) {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;
    constexpr ProfileId profile = ProfileId::ZSTD_TU;
    const PreparationRouteKey route{
        FStoreGuid{f_launch.f_store_guid.bytes}, f_launch.store_generation,
        profile};
    EndpointCaps caps;
    caps.profile = profile;
    caps.supported_profiles = profile_bit(profile);
    caps.zstd.max_raw_bytes = 1U << 20;
    caps.zstd.max_encoded_body_bytes = 1U << 20;
    PreparationAuthorityLimits authority_limits;
    authority_limits.max_speculative_tus = 1;
    authority_limits.max_speculative_raw_bytes = 1U << 20;
    auto authority = std::make_shared<P50PreparationAuthority>(
        c_launch.c_store_guid, caps.zstd, authority_limits, 1, profile);
    P50ClientEndpoint client(authority, caps, HistoryNonce{1}, nullptr,
                             nullptr, std::nullopt, {}, {}, route);
    const PreparedTuHandle prepared = authority->prepare_for_route(
        route, PrepareRequestKey{0xd11e, armed.arm.source.source_request_id},
        raw);
    JobBind binding = test_p51_job_binding(
        armed, 1, 1, authority->prepared_tu_seq(prepared).value,
        std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));
    tcp::socket socket(co_await asio::this_coro::executor);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(60);
    try {
        co_await socket.async_connect(
            tcp::endpoint(asio::ip::address_v4::loopback(), f_port),
            asio::use_awaitable);
        LinkHello link = test_p51_link_hello(
            armed, 1, HistoryNonce{1}, LinkStartMode::Initial);
        link.max_raw_bytes = 1U << 20;
        link.max_encoded_bytes = 1U << 20;
        link.max_output_bytes = 1U << 20;
        link.system_source_fingerprint =
            icecc::digest128("D11 aggregate encoded budget");
        const LinkState opened = co_await client.open_r2_link(
            socket, link, deadline);
        CHECK(opened.window == window && opened.profile == profile);
        {
            std::lock_guard lock(state.mutex);
            state.link = link;
            state.binding = binding;
            state.link_opened = true;
        }
        state.changed.notify_all();

        try {
            {
                std::lock_guard lock(state.mutex);
                state.bundle_attempted = true;
            }
            const R2SentBundle sent = co_await client.write_r2_bundle(
                socket, binding, prepared, deadline);
            {
                std::lock_guard lock(state.mutex);
                state.encoded_bytes = sent.begin.inner.body.encoded_bytes;
                state.bundle_written = true;
            }
            state.changed.notify_all();
            const ClientRunResult result = co_await client.read_r2_receipt(
                socket, sent, deadline);
            {
                std::lock_guard lock(state.mutex);
                state.result = result;
                state.committed = result.status == ClientRunStatus::Committed;
                state.refused = !state.committed;
            }
            state.changed.notify_all();
            if (result.status == ClientRunStatus::Committed)
                co_await client.write_r2_ack(
                    socket, 1, deadline);
            {
                std::unique_lock lock(state.mutex);
                if (state.hold_after_ack &&
                    !state.changed.wait_for(lock, std::chrono::seconds(10), [&] {
                        return state.continue_after_ack;
                    }))
                    throw std::runtime_error(
                        "D11 encoded-cap ACK observer was not released");
            }
        } catch (const boost::system::system_error& error) {
            if (error.code() != asio::error::eof &&
                error.code() != asio::error::connection_reset &&
                error.code() != asio::error::connection_aborted &&
                error.code() != asio::error::broken_pipe)
                throw;
            std::lock_guard lock(state.mutex);
            state.refused = true;
        }
    } catch (const boost::system::system_error& error) {
        const bool transport_close =
            error.code() == asio::error::eof ||
            error.code() == asio::error::connection_reset ||
            error.code() == asio::error::connection_aborted ||
            error.code() == asio::error::broken_pipe;
        std::lock_guard lock(state.mutex);
        if (state.link_opened && state.bundle_attempted && transport_close)
            state.refused = true;
        else
            state.error = std::current_exception();
    } catch (...) {
        std::lock_guard lock(state.mutex);
        state.error = std::current_exception();
    }
    {
        std::lock_guard lock(state.mutex);
        state.done = true;
    }
    state.changed.notify_all();
}

void test_p51_d11_real_r2_pending_budget(
    D11PendingBudgetKind budget_kind, uint32_t window) {
    constexpr ProfileId profile = ProfileId::ZSTD_TU;
    constexpr size_t kRawBytes = 1024;
    constexpr uint64_t kEncodedCap = 1536;
    constexpr uint64_t kRawCap = 1536;
    CHECK(window == 1 || window == 30);

    StoreIdentityRoot f_root{};
    f_root.bytes[15] = 0xe1;
    const SidecarLaunchIdentity f_launch = test_sidecar_launch(f_root);
    std::array<SidecarLaunchIdentity, 4> c_launches;
    for (size_t index = 0; index != c_launches.size(); ++index) {
        StoreIdentityRoot root{};
        root.bytes[15] = static_cast<uint8_t>(0xe2 + index);
        c_launches[index] = test_sidecar_launch(root);
        CHECK(c_launches[index].c_store_guid != f_launch.c_store_guid);
    }

    uint16_t f_port = 0;
    const int listener = loopback_listener(f_port);
    CHECK(listener >= 0 && f_port != 0);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = f_launch.c_store_guid;
    config.f_store_guid = f_launch.f_store_guid;
    config.f_store_generation = f_launch.store_generation;
    config.sidecar_launch = f_launch;
    config.endpoint_caps.profile = profile;
    config.endpoint_caps.supported_profiles = profile_bit(profile);
    config.endpoint_caps.zstd.max_raw_bytes = 1U << 20;
    config.endpoint_caps.zstd.max_encoded_body_bytes = 1U << 20;
    const uint64_t one_decoder_window =
        uint64_t{1} << config.endpoint_caps.zstd.max_window_log;
    const uint64_t budget_cap = [&] {
        switch (budget_kind) {
        case D11PendingBudgetKind::Encoded: return kEncodedCap;
        case D11PendingBudgetKind::Raw: return kRawCap;
        case D11PendingBudgetKind::DecoderWindow: return one_decoder_window;
        }
        return uint64_t{0};
    }();
    CHECK(budget_cap != 0);
    const uint64_t encoded_limit =
        budget_kind == D11PendingBudgetKind::Encoded ? kEncodedCap
                                                    : 4 * kRawBytes;
    const uint64_t raw_limit =
        budget_kind == D11PendingBudgetKind::Raw ? kRawCap : 4 * kRawBytes;
    const uint64_t decoder_window_limit =
        budget_kind == D11PendingBudgetKind::DecoderWindow
            ? one_decoder_window
            : uint64_t{2} << config.endpoint_caps.zstd.max_window_log;
    config.endpoint_config.owner_limits.max_pending_encoded_bytes = encoded_limit;
    config.endpoint_config.owner_limits.max_pending_raw_bytes = raw_limit;
    config.endpoint_config.owner_limits.max_decoder_window_bytes =
        decoder_window_limit;
    config.endpoint_config.owner_limits.max_retained_input_records = 8;
    config.endpoint_config.owner_limits.max_retained_input_bytes =
        8 * kRawBytes;
    config.max_pending_p51_source_reservations = 8;
    std::mutex materialized_mutex;
    std::vector<std::pair<Digest128, uint64_t>> materialized;
    config.endpoint_config.input_job_state =
        [&](CStoreGuid, const TxBegin& begin, const TxCommit&,
            std::span<const uint8_t> bytes) {
            std::lock_guard lock(materialized_mutex);
            materialized.emplace_back(icecc::digest128(bytes), bytes.size());
            CHECK(begin.raw_digest == icecc::digest128(bytes) &&
                  begin.raw_bytes == bytes.size());
            return InputJobState::Open;
        };
    service::SidecarRuntime runtime(std::move(config));

    std::vector<uint8_t> raw = d11_encoded_cap_input(kRawBytes);
    std::array<local::P51SourceReservationRequest, 4> requests;
    std::array<P51SourceArmedFields, 4> armed;
    for (size_t index = 0; index != requests.size(); ++index) {
        requests[index] = test_p51_reservation_request(
            c_launches[index].c_store_guid,
            c_launches[index].store_generation,
            c_launches[index].identity.generation,
            c_launches[index].identity.attempt,
            0xd110 + index, CACHE_PROFILE_ZSTD_TU, window,
            std::chrono::seconds(120));
        requests[index].arm.source.selected_f_host = "127.0.0.1";
        requests[index].arm.source.selected_f_cache_port = f_port;
        requests[index].arm.source.logical_job = 0xd120 + index;
        requests[index].arm.source.assignment_nonce = 0xd130 + index;
        const auto result = runtime.reserve_p51_source_on_owner(requests[index]);
        CHECK(result.error_code == 0 && result.armed.has_value() &&
              result.armed->selected_window == window);
        armed[index] = *result.armed;
    }

    D11PendingBudgetGate gate;
    D11PendingBudgetGate second_gate;
    std::atomic<bool> stop_accepting{false};
    std::atomic<size_t> accepted_connections{0};
    std::thread acceptor([&] {
        const auto end = std::chrono::steady_clock::now() +
                         std::chrono::seconds(90);
        while (!stop_accepting.load(std::memory_order_acquire) &&
               accepted_connections.load(std::memory_order_acquire) < 4 &&
               std::chrono::steady_clock::now() < end) {
            pollfd ready{listener, POLLIN, 0};
            int polled;
            do {
                polled = ::poll(&ready, 1, 100);
            } while (polled < 0 && errno == EINTR);
            if (polled <= 0 || !(ready.revents & POLLIN))
                continue;
            sockaddr_storage peer{};
            socklen_t peer_size = sizeof(peer);
            int fd;
            do {
                fd = ::accept(listener,
                    reinterpret_cast<sockaddr*>(&peer), &peer_size);
            } while (fd < 0 && errno == EINTR);
            if (fd < 0)
                continue;
            const size_t connection_index =
                accepted_connections.fetch_add(1, std::memory_order_acq_rel);
            EndpointIoControl control;
            if (connection_index == 0) {
                control.before_materialize_on_worker = [&gate] {
                    std::unique_lock lock(gate.mutex);
                    ++gate.entered;
                    gate.changed.notify_all();
                    gate.changed.wait(lock, [&] { return gate.release; });
                };
            } else if (connection_index == 1) {
                // If a cap-deletion mutant admits C2, hold it after its
                // reservation reaches the worker so the over-cap counter is
                // observable before materialization releases the charge.
                control.before_materialize_on_worker = [&second_gate] {
                    std::unique_lock lock(second_gate.mutex);
                    ++second_gate.entered;
                    second_gate.changed.notify_all();
                    second_gate.changed.wait(lock, [&] {
                        return second_gate.release;
                    });
                };
            }
            // The low-level P50ClientEndpoint used below speaks R2 directly
            // on this socket; unlike daemon-driven service fixtures, there is
            // no ordinary MsgChannel/P51-link-session preamble here.
            runtime.start_adopted_r2_endpoint(fd, std::move(control));
        }
    });

    std::array<D11PendingBudgetClientState, 4> states;
    std::array<std::thread, 4> clients;
    const auto start_client = [&](size_t index) {
        clients[index] = std::thread([&, index] {
            try {
                namespace asio = boost::asio;
                asio::io_context context;
                auto future = asio::co_spawn(
                    context,
                    d11_pending_budget_client(
                        f_port, window, c_launches[index], f_launch,
                        armed[index], raw, states[index]),
                    asio::use_future);
                context.run();
                future.get();
            } catch (...) {
                std::lock_guard lock(states[index].mutex);
                states[index].error = std::current_exception();
                states[index].done = true;
                states[index].changed.notify_all();
            }
        });
    };
    auto cleanup = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1), [&](int*) {
            {
                std::lock_guard lock(gate.mutex);
                gate.release = true;
            }
            gate.changed.notify_all();
            {
                std::lock_guard lock(second_gate.mutex);
                second_gate.release = true;
            }
            second_gate.changed.notify_all();
            stop_accepting.store(true, std::memory_order_release);
            (void)::shutdown(listener, SHUT_RDWR);
            if (acceptor.joinable())
                acceptor.join();
            (void)::close(listener);
            for (D11PendingBudgetClientState& state : states) {
                {
                    std::lock_guard lock(state.mutex);
                    state.continue_after_ack = true;
                }
                state.changed.notify_all();
            }
            for (std::thread& client : clients) {
                if (client.joinable())
                    client.join();
            }
            runtime.stop();
        });

    {
        std::lock_guard lock(states[0].mutex);
        states[0].hold_after_ack = true;
    }
    start_client(0);
    {
        std::unique_lock lock(gate.mutex);
        CHECK(gate.changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return gate.entered != 0;
        }));
    }
    {
        std::unique_lock lock(states[0].mutex);
        CHECK(states[0].changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return states[0].bundle_written || states[0].error != nullptr;
        }));
        if (states[0].error)
            std::rethrow_exception(states[0].error);
        CHECK(states[0].link_opened && states[0].bundle_written);
    }
    const LinkHello first_link = states[0].link;
    const uint64_t encoded_charge = states[0].encoded_bytes;
    const uint64_t raw_charge = raw.size();
    const uint64_t decoder_window_charge = one_decoder_window;
    const uint64_t selected_charge = [&] {
        switch (budget_kind) {
        case D11PendingBudgetKind::Encoded: return encoded_charge;
        case D11PendingBudgetKind::Raw: return raw_charge;
        case D11PendingBudgetKind::DecoderWindow:
            return decoder_window_charge;
        }
        return uint64_t{0};
    }();
    CHECK(encoded_charge != 0 && selected_charge != 0 &&
          selected_charge <= budget_cap && selected_charge * 2 > budget_cap);
    if (budget_kind != D11PendingBudgetKind::Encoded)
        CHECK(encoded_charge * 2 <= encoded_limit);
    if (budget_kind != D11PendingBudgetKind::Raw)
        CHECK(raw_charge * 2 <= raw_limit);
    if (budget_kind != D11PendingBudgetKind::DecoderWindow)
        CHECK(decoder_window_charge * 2 <= decoder_window_limit);
    auto held = runtime.p51_receipt_ledger_for_test(first_link);
    CHECK(held.has_value() &&
          held->endpoint_usage.pending_encoded_bytes == encoded_charge &&
          held->endpoint_usage.pending_raw_bytes == raw_charge &&
          held->endpoint_usage.decoder_window_bytes == decoder_window_charge &&
          held->endpoint_usage.retained_input_records == 0);

    const auto wait_client = [&](size_t index) {
        std::unique_lock lock(states[index].mutex);
        CHECK(states[index].changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return states[index].done;
        }));
        if (states[index].error)
            std::rethrow_exception(states[index].error);
    };
    const auto assert_cap_refused_without_extra_charge = [&](size_t index) {
        std::unique_lock lock(states[index].mutex);
        CHECK(states[index].changed.wait_for(lock, std::chrono::seconds(10), [&] {
            return states[index].bundle_written || states[index].done ||
                   states[index].error != nullptr;
        }));
        if (states[index].error)
            std::rethrow_exception(states[index].error);
        CHECK(states[index].link_opened && states[index].bundle_attempted);
        if (states[index].bundle_written)
            CHECK(states[index].encoded_bytes == encoded_charge);
        lock.unlock();

        const auto selected_usage = [&](
            const P50ServerOwnerUsage& usage) {
            switch (budget_kind) {
            case D11PendingBudgetKind::Encoded:
                return usage.pending_encoded_bytes;
            case D11PendingBudgetKind::Raw:
                return usage.pending_raw_bytes;
            case D11PendingBudgetKind::DecoderWindow:
                return usage.decoder_window_bytes;
            }
            return uint64_t{0};
        };
        std::optional<service::P51ReceiptLedgerSnapshot> during_refusal;
        const auto refusal_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(10);
        bool done = false;
        while (std::chrono::steady_clock::now() < refusal_deadline) {
            during_refusal = runtime.p51_receipt_ledger_for_test(first_link);
            CHECK(during_refusal.has_value());
            bool bundle_written = false;
            bool second_materialization_held = false;
            bool committed = false;
            {
                std::lock_guard state_lock(states[index].mutex);
                if (states[index].error)
                    std::rethrow_exception(states[index].error);
                bundle_written = states[index].bundle_written;
                done = states[index].done;
                committed = states[index].committed;
            }
            {
                std::lock_guard gate_lock(second_gate.mutex);
                second_materialization_held = second_gate.entered != 0;
            }
            const uint64_t observed_target =
                selected_usage(during_refusal->endpoint_usage);
            if (done || during_refusal->endpoint_usage.pending_encoded_bytes !=
                            encoded_charge ||
                during_refusal->endpoint_usage.pending_raw_bytes != raw_charge ||
                during_refusal->endpoint_usage.decoder_window_bytes !=
                    decoder_window_charge ||
                during_refusal->endpoint_usage.retained_input_records != 0 ||
                second_materialization_held || committed)
                std::fprintf(stderr,
                             "D11_STAGE %s c%zu-refusal written=%u done=%u committed=%u second-held=%u enc=%llu raw=%llu window=%llu target=%llu cap=%llu retained=%llu\n",
                             d11_pending_budget_name(budget_kind),
                             index + 1,
                             bundle_written ? 1u : 0u,
                             done ? 1u : 0u,
                             committed ? 1u : 0u,
                             second_materialization_held ? 1u : 0u,
                             static_cast<unsigned long long>(
                                 during_refusal->endpoint_usage.pending_encoded_bytes),
                             static_cast<unsigned long long>(
                                 during_refusal->endpoint_usage.pending_raw_bytes),
                             static_cast<unsigned long long>(
                                 during_refusal->endpoint_usage.decoder_window_bytes),
                             static_cast<unsigned long long>(observed_target),
                             static_cast<unsigned long long>(budget_cap),
                             static_cast<unsigned long long>(
                                 during_refusal->endpoint_usage.retained_input_records));
            CHECK(observed_target <= budget_cap);
            CHECK(during_refusal->endpoint_usage.pending_encoded_bytes ==
                      encoded_charge &&
                  during_refusal->endpoint_usage.pending_raw_bytes ==
                      raw_charge &&
                  during_refusal->endpoint_usage.decoder_window_bytes ==
                      decoder_window_charge &&
                  during_refusal->endpoint_usage.retained_input_records == 0);
            if (done)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(done);
        {
            std::lock_guard state_lock(states[index].mutex);
            CHECK(states[index].refused && !states[index].committed);
        }
    };
    start_client(1);
    assert_cap_refused_without_extra_charge(1);
    held = runtime.p51_receipt_ledger_for_test(first_link);
    CHECK(held.has_value() &&
          held->endpoint_usage.pending_encoded_bytes == encoded_charge &&
          held->endpoint_usage.pending_raw_bytes == raw_charge &&
          held->endpoint_usage.decoder_window_bytes == decoder_window_charge &&
          held->endpoint_usage.retained_input_records == 0);
    {
        std::lock_guard lock(gate.mutex);
        CHECK(gate.entered == 1);
    }
    {
        std::lock_guard lock(materialized_mutex);
        CHECK(materialized.empty());
    }

    // Negative control: another exact same-size valid TU still cannot pass
    // while C1's encoded credit remains retained.
    start_client(2);
    assert_cap_refused_without_extra_charge(2);
    held = runtime.p51_receipt_ledger_for_test(first_link);
    CHECK(held.has_value() &&
          held->endpoint_usage.pending_encoded_bytes == encoded_charge &&
          held->endpoint_usage.pending_raw_bytes == raw_charge &&
          held->endpoint_usage.decoder_window_bytes == decoder_window_charge &&
          held->endpoint_usage.retained_input_records == 0);

    {
        std::lock_guard lock(gate.mutex);
        gate.release = true;
    }
    gate.changed.notify_all();
    {
        std::unique_lock lock(states[0].mutex);
        CHECK(states[0].changed.wait_for(lock, std::chrono::seconds(15), [&] {
            return states[0].committed || states[0].error != nullptr;
        }));
        if (states[0].error)
            std::rethrow_exception(states[0].error);
    }
    std::optional<service::P51ReceiptLedgerSnapshot> released_first;
    const auto release_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < release_deadline) {
        released_first = runtime.p51_receipt_ledger_for_test(first_link);
        if (released_first &&
            released_first->acknowledged_prefix_q == 1 &&
            released_first->endpoint_usage.pending_encoded_bytes == 0 &&
            released_first->endpoint_usage.pending_raw_bytes == 0 &&
            released_first->endpoint_usage.decoder_window_bytes == 0 &&
            released_first->endpoint_usage.retained_input_records == 1)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::fprintf(stderr,
        "D11_STAGE %s-release link=%u q=%llu enc=%llu raw=%llu window=%llu retained=%llu\n",
        d11_pending_budget_name(budget_kind),
                 released_first.has_value() ? 1u : 0u,
                 static_cast<unsigned long long>(released_first ?
                     released_first->acknowledged_prefix_q : 0),
                 static_cast<unsigned long long>(released_first ?
                     released_first->endpoint_usage.pending_encoded_bytes : 0),
                 static_cast<unsigned long long>(released_first ?
                     released_first->endpoint_usage.pending_raw_bytes : 0),
                 static_cast<unsigned long long>(released_first ?
                     released_first->endpoint_usage.decoder_window_bytes : 0),
                 static_cast<unsigned long long>(released_first ?
                     released_first->endpoint_usage.retained_input_records : 0));
    CHECK(released_first.has_value() &&
          released_first->acknowledged_prefix_q == 1 &&
          released_first->endpoint_usage.pending_encoded_bytes == 0 &&
          released_first->endpoint_usage.pending_raw_bytes == 0 &&
          released_first->endpoint_usage.decoder_window_bytes == 0 &&
          released_first->endpoint_usage.retained_input_records == 1);
    {
        std::lock_guard lock(states[0].mutex);
        states[0].continue_after_ack = true;
    }
    states[0].changed.notify_all();
    wait_client(0);
    CHECK(states[0].committed && states[0].result.has_value() &&
          states[0].result->committed_input.has_value() &&
          states[0].result->committed_commit.has_value() &&
          states[0].result->committed_commit->raw_digest ==
              icecc::digest128(raw));
    CHECK(released_first->endpoint_usage.pending_encoded_bytes == 0 &&
          released_first->endpoint_usage.pending_raw_bytes == 0 &&
          released_first->endpoint_usage.decoder_window_bytes == 0 &&
          released_first->endpoint_usage.retained_input_records == 1);

    // The same-size transaction is now admissible after the first lease has
    // settled and released its encoded charge.
    {
        std::lock_guard lock(states[3].mutex);
        states[3].hold_after_ack = true;
    }
    start_client(3);
    {
        std::unique_lock lock(states[3].mutex);
        CHECK(states[3].changed.wait_for(lock, std::chrono::seconds(15), [&] {
            return states[3].committed || states[3].error != nullptr;
        }));
        if (states[3].error)
            std::rethrow_exception(states[3].error);
    }
    std::optional<service::P51ReceiptLedgerSnapshot> refilled;
    const auto refill_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < refill_deadline) {
        refilled = runtime.p51_receipt_ledger_for_test(states[3].link);
        if (refilled && refilled->acknowledged_prefix_q == 1 &&
            refilled->endpoint_usage.pending_encoded_bytes == 0 &&
            refilled->endpoint_usage.pending_raw_bytes == 0 &&
            refilled->endpoint_usage.decoder_window_bytes == 0 &&
            refilled->endpoint_usage.retained_input_records == 2)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::fprintf(stderr,
        "D11_STAGE %s-refill link=%u q=%llu enc=%llu raw=%llu window=%llu retained=%llu\n",
        d11_pending_budget_name(budget_kind),
                 refilled.has_value() ? 1u : 0u,
                 static_cast<unsigned long long>(refilled ?
                     refilled->acknowledged_prefix_q : 0),
                 static_cast<unsigned long long>(refilled ?
                     refilled->endpoint_usage.pending_encoded_bytes : 0),
                 static_cast<unsigned long long>(refilled ?
                     refilled->endpoint_usage.pending_raw_bytes : 0),
                 static_cast<unsigned long long>(refilled ?
                     refilled->endpoint_usage.decoder_window_bytes : 0),
                 static_cast<unsigned long long>(refilled ?
                     refilled->endpoint_usage.retained_input_records : 0));
    CHECK(refilled.has_value() && refilled->acknowledged_prefix_q == 1 &&
          refilled->endpoint_usage.pending_encoded_bytes == 0 &&
          refilled->endpoint_usage.pending_raw_bytes == 0 &&
          refilled->endpoint_usage.decoder_window_bytes == 0 &&
          refilled->endpoint_usage.retained_input_records == 2);
    {
        std::lock_guard lock(states[3].mutex);
        states[3].continue_after_ack = true;
    }
    states[3].changed.notify_all();
    wait_client(3);
    CHECK(states[3].link_opened && states[3].bundle_written &&
          states[3].committed && states[3].result.has_value() &&
          states[3].result->committed_input.has_value() &&
          states[3].encoded_bytes == encoded_charge);
    {
        std::lock_guard lock(materialized_mutex);
        CHECK(materialized.size() == 2 &&
              materialized[0] == std::make_pair(
                  icecc::digest128(raw), static_cast<uint64_t>(raw.size())) &&
              materialized[1] == materialized[0]);
    }
    CHECK(accepted_connections.load(std::memory_order_acquire) == 4);
    std::printf(
        "P51_D11_REAL_R2_PENDING_BUDGET profile=ZSTD_TU kind=%s window=%u encoded=%llu raw=%llu decoder-window=%llu cap=%llu: PASS\n",
        d11_pending_budget_name(budget_kind), window,
        static_cast<unsigned long long>(encoded_charge),
        static_cast<unsigned long long>(raw_charge),
        static_cast<unsigned long long>(decoder_window_charge),
        static_cast<unsigned long long>(budget_cap));
}

void test_p51_cancel_publication_and_reset_lifecycle() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x39;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    StoreIdentityRoot remote_root{};
    remote_root.bytes[15] = 0x3a;
    const CStoreGuid remote_c = c_store_guid_for_root(remote_root);
    const auto owner_deadline = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);

    // Cancellation wins before publication: authorization refuses to start
    // the commit, endpoint settlement drops the pending decode, and RESET
    // must not resurrect the cancelled ARM or an already-expired suffix row.
    {
        service::RuntimeConfig config = test_runtime_config();
        config.c_store_guid = launch.c_store_guid;
        config.f_store_guid = launch.f_store_guid;
        config.f_store_generation = launch.store_generation;
        config.sidecar_launch = launch;
        config.max_route_relationships = 1;
        service::SidecarRuntime runtime(std::move(config));
        P51SourceArmedFields armed;
        local::P51SourceReservationResult expiring_result;
        LinkHello hello;
        auto request = test_p51_reservation_request(
            remote_c, 51, launch.identity.generation, launch.identity.attempt,
            5101, CACHE_PROFILE_ZSTD_TU, 30);
        auto expiring_request = test_p51_reservation_request(
            remote_c, 51, launch.identity.generation, launch.identity.attempt,
            5102, CACHE_PROFILE_ZSTD_TU, 30,
            std::chrono::milliseconds(100));
        const auto result = runtime.reserve_p51_source_on_owner(request);
        CHECK(result.error_code == 0 && result.armed.has_value());
        armed = *result.armed;
        hello = test_p51_link_hello(armed, 1, HistoryNonce{0x510001});
        const JobBind binding = test_p51_job_binding(
            armed, hello.physical_link_generation, 1, 81,
            "cancel-before-publish");
        runtime.run_owner_callback_for_test([&] {
            CHECK(runtime.lookup_p51_link_reservation_on_owner(hello).has_value());
            CHECK(runtime.consume_p51_job_reservation_on_owner(
                hello, binding).has_value());
        });
        CHECK(runtime.cancel_p51_source_on_owner(
            request.arm, armed.reservation_id, owner_deadline));
        runtime.run_owner_callback_for_test([&] {
            CHECK(!runtime.authorize_p51_job_publication_on_owner(hello, binding));
            runtime.settle_p51_cancelled_job_on_owner(hello, binding);
        });
        expiring_result = runtime.reserve_p51_source_on_owner(expiring_request);
        CHECK(expiring_result.error_code == 0 &&
              expiring_result.armed.has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        const Id128 operation_id{icecc::digest128("cancel reset op").bytes};
        ResetRequest reset;
        ResetConfirm confirm;
        runtime.run_owner_callback_for_test([&] {
            const RecoverBegin begin = test_p51_recover_begin(
                hello, 0, 0, 0, operation_id);
            const std::array<RecoverWitness, 0> no_witnesses{};
            const auto recovered = runtime.recover_p51_receipts_on_owner(
                hello, begin, no_witnesses,
                test_p51_recover_end(begin, no_witnesses));
            CHECK(recovered.has_value());
            reset.relationship_id = hello.relationship_id;
            reset.old_relationship_epoch = hello.relationship_epoch;
            reset.new_relationship_epoch = hello.relationship_epoch + 1;
            reset.physical_link_generation = hello.physical_link_generation;
            reset.operation_id = operation_id;
            reset.settled_prefix_k = 0;
            reset.old_history_nonce = hello.history_nonce;
            reset.new_history_nonce = HistoryNonce{hello.history_nonce.value + 1};
            ResetRequest exhausted_reset = reset;
            exhausted_reset.new_relationship_epoch = UINT64_MAX;
            CHECK(!runtime.validate_p51_reset_on_owner(
                       hello, exhausted_reset)
                       .has_value());
            const auto ack = runtime.validate_p51_reset_on_owner(hello, reset);
            CHECK(ack.has_value());
            CHECK(runtime.commit_p51_reset_on_owner(hello, reset, *ack));
            confirm.relationship_id = reset.relationship_id;
            confirm.new_relationship_epoch = reset.new_relationship_epoch;
            confirm.physical_link_generation = reset.physical_link_generation;
            confirm.operation_id = reset.operation_id;
            confirm.new_history_nonce = reset.new_history_nonce;
            confirm.settled_prefix_k = reset.settled_prefix_k;
            CHECK(runtime.confirm_p51_reset_on_owner(hello, confirm));

            LinkHello reconnect = test_p51_link_hello(
                armed, 2, reset.new_history_nonce, LinkStartMode::Reconnect, 0);
            reconnect.relationship_epoch = reset.new_relationship_epoch;
            CHECK(runtime.lookup_p51_link_reservation_on_owner(
                reconnect).has_value());
            JobBind cancelled_binding = test_p51_job_binding(
                armed, reconnect.physical_link_generation, 1, 81,
                "cancel-before-publish");
            CHECK(!runtime.consume_p51_job_reservation_on_owner(
                reconnect, cancelled_binding).has_value());
            JobBind expired_binding = test_p51_job_binding(
                *expiring_result.armed, reconnect.physical_link_generation,
                1, 82, "expired-before-reset");
            CHECK(!runtime.consume_p51_job_reservation_on_owner(
                reconnect, expired_binding).has_value());
            // This fixture exercised lookup only; release its synthetic link
            // explicitly so the following capacity pressure represents a
            // genuine idle relationship eligible for route-history eviction.
            runtime.release_p51_link_on_owner(reconnect);
        });

        // A fresh relationship after idle eviction must advance beyond the
        // epoch committed by RESET, even though that epoch was not allocated
        // by the relationship-creation counter.
        StoreIdentityRoot pressure_root{};
        pressure_root.bytes[15] = 0x3b;
        const CStoreGuid pressure_c = c_store_guid_for_root(pressure_root);
        auto pressure_request = test_p51_reservation_request(
            pressure_c, 52, launch.identity.generation,
            launch.identity.attempt, 5201, CACHE_PROFILE_ZSTD_TU, 30);
        const auto pressure_result =
            runtime.reserve_p51_source_on_owner(pressure_request);
        CHECK(pressure_result.error_code == 0 && pressure_result.armed);
        CHECK(pressure_result.armed->logical_relationship_id !=
                  armed.logical_relationship_id &&
              pressure_result.armed->relationship_epoch >
                  reset.new_relationship_epoch);
    }

    // Publication wins once the owner marks the reservation as publishing.
    // The later cancel cannot retract that exact committed receipt. A repeated
    // RESET_CONFIRM after a subsequent commit stays idempotent and preserves K.
    {
        service::RuntimeConfig config = test_runtime_config();
        config.c_store_guid = launch.c_store_guid;
        config.f_store_guid = launch.f_store_guid;
        config.f_store_generation = launch.store_generation;
        config.sidecar_launch = launch;
        service::SidecarRuntime runtime(std::move(config));
        auto first_request = test_p51_reservation_request(
            remote_c, 61, launch.identity.generation, launch.identity.attempt,
            6101, CACHE_PROFILE_ZSTD_TU, 30);
        const auto first_result = runtime.reserve_p51_source_on_owner(first_request);
        CHECK(first_result.error_code == 0 && first_result.armed.has_value());
        const P51SourceArmedFields first_armed = *first_result.armed;
        LinkHello link = test_p51_link_hello(
            first_armed, 11, HistoryNonce{0x610001});
        JobBind first_binding = test_p51_job_binding(
            first_armed, link.physical_link_generation, 1, 91,
            "publication-before-cancel");
        auto make_commit = [&](const LinkHello& current_link,
                               const JobBind& binding) {
            R2TxCommit commit;
            commit.relationship_ordinal = binding.relationship_ordinal;
            commit.binding_digest = compute_r2_binding_digest(binding);
            commit.transaction_digest = icecc::digest128(
                "receipt/" + std::to_string(binding.relationship_ordinal));
            commit.inner.history_nonce = current_link.history_nonce;
            commit.inner.rel_seq = RelSeq{binding.relationship_ordinal - 1};
            commit.inner.tu_seq = binding.tu_seq;
            commit.inner.transaction_digest = commit.transaction_digest;
            commit.inner.raw_digest = binding.raw_digest;
            return commit;
        };
        ResetRequest reset;
        ResetConfirm confirm;
        runtime.run_owner_callback_for_test([&] {
            CHECK(runtime.lookup_p51_link_reservation_on_owner(link).has_value());
            CHECK(runtime.consume_p51_job_reservation_on_owner(
                link, first_binding).has_value());
            CHECK(runtime.authorize_p51_job_publication_on_owner(
                link, first_binding));
        });
        CHECK(!runtime.cancel_p51_source_on_owner(
            first_request.arm, first_armed.reservation_id, owner_deadline));

        const R2TxCommit first_commit = make_commit(link, first_binding);
        LinkHello recovery_link;
        runtime.run_owner_callback_for_test([&] {
        CHECK(runtime.record_p51_job_commit_on_owner(
            link, first_binding, first_commit));

        // Recovery witnesses name the original bind, while the RECOVER
        // exchange is carried by a newer physical link incarnation.
        runtime.release_p51_link_on_owner(link);
        recovery_link = test_p51_link_hello(
            first_armed, link.physical_link_generation + 1,
            link.history_nonce, LinkStartMode::Reconnect, 0);
        CHECK(runtime.lookup_p51_link_reservation_on_owner(
                  recovery_link).has_value());

        const Id128 operation_id{icecc::digest128("publication reset op").bytes};
        const RecoverBegin begin = test_p51_recover_begin(
            recovery_link, 0, 1, 1, operation_id);
        RecoverWitness witness;
        witness.relationship_id = begin.relationship_id;
        witness.relationship_epoch = begin.relationship_epoch;
        witness.physical_link_generation = begin.physical_link_generation;
        witness.operation_id = begin.operation_id;
        witness.relationship_ordinal = 1;
        witness.binding_digest = first_commit.binding_digest;
        witness.transaction_digest = first_commit.transaction_digest;
        witness.inner.history_nonce = first_commit.inner.history_nonce;
        witness.inner.rel_seq = first_commit.inner.rel_seq;
        witness.inner.tu_seq = first_commit.inner.tu_seq;
        witness.inner.profile = first_binding.profile;
        witness.inner.raw_bytes = first_binding.raw_bytes;
        witness.inner.raw_digest = first_binding.raw_digest;
        witness.inner.transaction_digest =
            first_commit.inner.transaction_digest;
        witness.binding = first_binding;
        const std::array<RecoverWitness, 1> witnesses{witness};
        const auto recovered = runtime.recover_p51_receipts_on_owner(
            recovery_link, begin, witnesses,
            test_p51_recover_end(begin, witnesses));
        CHECK(recovered.has_value() && recovered->rows.size() == 1);
        reset.relationship_id = recovery_link.relationship_id;
        reset.old_relationship_epoch = recovery_link.relationship_epoch;
        reset.new_relationship_epoch = recovery_link.relationship_epoch + 1;
        reset.physical_link_generation =
            recovery_link.physical_link_generation;
        reset.operation_id = operation_id;
        reset.settled_prefix_k = 1;
        reset.old_history_nonce = recovery_link.history_nonce;
        reset.new_history_nonce =
            HistoryNonce{recovery_link.history_nonce.value + 1};
        const auto ack = runtime.validate_p51_reset_on_owner(
            recovery_link, reset);
        CHECK(ack.has_value());
        CHECK(runtime.commit_p51_reset_on_owner(recovery_link, reset, *ack));
        confirm.relationship_id = reset.relationship_id;
        confirm.new_relationship_epoch = reset.new_relationship_epoch;
        confirm.physical_link_generation = reset.physical_link_generation;
        confirm.operation_id = reset.operation_id;
        confirm.new_history_nonce = reset.new_history_nonce;
        confirm.settled_prefix_k = reset.settled_prefix_k;
        CHECK(runtime.confirm_p51_reset_on_owner(recovery_link, confirm));
        });

        auto second_request = test_p51_reservation_request(
            remote_c, 61, launch.identity.generation, launch.identity.attempt,
            6102, CACHE_PROFILE_ZSTD_TU, 30);
        const auto second_result = runtime.reserve_p51_source_on_owner(second_request);
        CHECK(second_result.error_code == 0 && second_result.armed.has_value());
        runtime.run_owner_callback_for_test([&] {
        LinkHello& link = recovery_link;
        link.relationship_epoch = reset.new_relationship_epoch;
        link.history_nonce = reset.new_history_nonce;
        link.verified_receipt_floor = 1;
        JobBind second_binding = test_p51_job_binding(
            *second_result.armed, link.physical_link_generation, 2, 92,
            "later-commit");
        CHECK(runtime.consume_p51_job_reservation_on_owner(
            link, second_binding).has_value());
        CHECK(runtime.authorize_p51_job_publication_on_owner(link, second_binding));
        const R2TxCommit second_commit = make_commit(link, second_binding);
        CHECK(runtime.record_p51_job_commit_on_owner(
            link, second_binding, second_commit));
        CHECK(runtime.confirm_p51_reset_on_owner(link, confirm));

        // Inspect the retained receipt on another physical reconnect. Its
        // original JOB_BIND generation remains the prior link's generation.
        runtime.release_p51_link_on_owner(link);
        LinkHello receipt_reconnect = test_p51_link_hello(
            first_armed, link.physical_link_generation + 1,
            reset.new_history_nonce, LinkStartMode::Reconnect, 1);
        receipt_reconnect.relationship_epoch = reset.new_relationship_epoch;
        CHECK(runtime.lookup_p51_link_reservation_on_owner(
                  receipt_reconnect).has_value());

        const RecoverBegin after_duplicate_confirm = test_p51_recover_begin(
            receipt_reconnect, 1, 2, 1,
            Id128{icecc::digest128("post-confirm inspect").bytes});
        RecoverWitness second_witness;
        second_witness.relationship_id = after_duplicate_confirm.relationship_id;
        second_witness.relationship_epoch = after_duplicate_confirm.relationship_epoch;
        second_witness.physical_link_generation =
            after_duplicate_confirm.physical_link_generation;
        second_witness.operation_id = after_duplicate_confirm.operation_id;
        second_witness.relationship_ordinal = 2;
        second_witness.binding_digest = second_commit.binding_digest;
        second_witness.transaction_digest = second_commit.transaction_digest;
        second_witness.inner.history_nonce = second_commit.inner.history_nonce;
        second_witness.inner.rel_seq = second_commit.inner.rel_seq;
        second_witness.inner.tu_seq = second_commit.inner.tu_seq;
        second_witness.inner.profile = second_binding.profile;
        second_witness.inner.raw_bytes = second_binding.raw_bytes;
        second_witness.inner.raw_digest = second_binding.raw_digest;
        second_witness.inner.transaction_digest =
            second_commit.inner.transaction_digest;
        second_witness.binding = second_binding;
        const std::array<RecoverWitness, 1> second_witnesses{second_witness};
        const auto retained = runtime.recover_p51_receipts_on_owner(
            receipt_reconnect, after_duplicate_confirm, second_witnesses,
            test_p51_recover_end(after_duplicate_confirm, second_witnesses));
        CHECK(retained.has_value());
        CHECK(retained->end.committed_prefix_k == 2);
        CHECK(retained->end.acknowledged_prefix_q == 1);
        CHECK(retained->rows.size() == 1 &&
              retained->rows.front().receipt == second_commit);
        });
    }
    std::puts("P51_SOURCE_LIFECYCLE cancel/publication/reset/idempotent-confirm: ok");
}

void test_p51_reservation_expiry_before_bind_no_late_arm() {
    struct ProfileCase {
        uint32_t mask;
        const char* name;
    };
    constexpr ProfileCase profiles[] = {
        {CACHE_PROFILE_P29V1, "P29V1"},
        {CACHE_PROFILE_ZSTD_TU, "ZSTD_TU"},
        {CACHE_PROFILE_ZSTD_ROUTE, "ZSTD_ROUTE"},
    };

    // Hold the real SidecarRuntime owner executor while reserve() posts its
    // bounded owner operation. The callback is released only after the
    // reservation's unchanged absolute deadline has elapsed.
    struct OwnerQueueBlock {
        std::promise<void> entered;
        std::promise<void> release;
        std::future<void> entered_future{entered.get_future()};
        std::shared_future<void> release_future{release.get_future().share()};
        std::thread thread;
        bool released = false;

        void start(service::SidecarRuntime& runtime) {
            thread = std::thread([this, &runtime] {
                runtime.run_owner_callback_for_test([this] {
                    entered.set_value();
                    release_future.wait();
                });
            });
        }

        void unblock_and_join() noexcept {
            if (!released) {
                released = true;
                try { release.set_value(); } catch (...) {}
            }
            if (thread.joinable())
                thread.join();
        }

        ~OwnerQueueBlock() { unblock_and_join(); }
    };

    for (size_t index = 0; index != std::size(profiles); ++index) {
        StoreIdentityRoot local_root{};
        local_root.bytes[15] = static_cast<uint8_t>(0x51 + index);
        const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
        StoreIdentityRoot remote_root{};
        remote_root.bytes[15] = static_cast<uint8_t>(0x61 + index);
        const CStoreGuid remote_c = c_store_guid_for_root(remote_root);

        service::RuntimeConfig config = test_runtime_config();
        config.c_store_guid = launch.c_store_guid;
        config.f_store_guid = launch.f_store_guid;
        config.f_store_generation = launch.store_generation;
        config.sidecar_launch = launch;
        config.max_pending_p51_source_reservations = 1;
        config.max_route_relationships = 1;
        service::SidecarRuntime runtime(std::move(config));

        auto queued_request = test_p51_reservation_request(
            remote_c, 81 + index, launch.identity.generation,
            launch.identity.attempt, 8100 + index, profiles[index].mask, 30,
            std::chrono::seconds(2));
        const auto queued_deadline =
            queued_request.absolute_deadline.as_steady_time_point();
        OwnerQueueBlock owner_block;
        owner_block.start(runtime);
        const bool owner_entered = owner_block.entered_future.wait_for(
            std::chrono::seconds(2)) == std::future_status::ready;

        std::promise<void> submitter_started;
        std::future<void> started = submitter_started.get_future();
        std::promise<std::pair<local::P51SourceReservationResult,
                               std::chrono::steady_clock::time_point>>
            queued_completion;
        auto queued_result = queued_completion.get_future();
        std::thread submitter([&] {
            submitter_started.set_value();
            auto result = runtime.reserve_p51_source_on_owner(queued_request);
            queued_completion.set_value(
                {std::move(result), std::chrono::steady_clock::now()});
        });
        const bool submitter_started_in_time = started.wait_for(
            std::chrono::seconds(2)) == std::future_status::ready;
        const bool queued_call_completed = queued_result.wait_until(
            queued_deadline + std::chrono::seconds(2)) ==
            std::future_status::ready;

        // Always unblock/join before assertions so a failed check cannot
        // strand the owner or destroy a joinable thread.
        owner_block.unblock_and_join();
        if (submitter.joinable())
            submitter.join();

        CHECK(owner_entered);
        CHECK(submitter_started_in_time);
        CHECK(queued_call_completed);
        auto [queued_arm, returned_at] = queued_result.get();
        CHECK(returned_at >= queued_deadline);
        CHECK(!queued_arm.armed.has_value());
        // No reservation/relationship capacity is occupied here, so 0x5103
        // identifies the bounded owner-round-trip timeout result for this
        // valid request (rather than a full-table or validation rejection).
        CHECK(queued_arm.error_code == 0x5103);

        // Drain the expired queued callback before using the one-row limit as
        // a leak check. The callback is canceled by owner_round_trip once its
        // deadline wins; it must not install an ARM after the owner resumes.
        runtime.run_owner_callback_for_test([] {});

        auto expiring_request = test_p51_reservation_request(
            remote_c, 81 + index, launch.identity.generation,
            launch.identity.attempt, 8200 + index, profiles[index].mask, 30,
            std::chrono::milliseconds(180));
        const auto expiring_result =
            runtime.reserve_p51_source_on_owner(expiring_request);
        CHECK(expiring_result.error_code == 0 &&
              expiring_result.armed.has_value());
        const P51SourceArmedFields expiring_armed = *expiring_result.armed;
        LinkHello initial = test_p51_link_hello(
            expiring_armed, 1, HistoryNonce{0x820001 + index});
        JobBind binding = test_p51_job_binding(
            expiring_armed, initial.physical_link_generation, 1,
            82000 + index, "expired-before-initial-bind");

        const auto expiring_deadline =
            expiring_request.absolute_deadline.as_steady_time_point();
        std::this_thread::sleep_until(
            expiring_deadline + std::chrono::milliseconds(1));
        P51SourceLinkLookupResult lookup;
        std::optional<P51SourceJobLease> consumed;
        runtime.run_owner_callback_for_test([&] {
            lookup = runtime.lookup_p51_link_reservation_on_owner(initial);
            consumed = runtime.consume_p51_job_reservation_on_owner(
                initial, binding);
        });
        CHECK(lookup.status != P51SourceLinkLookupStatus::Found);
        CHECK(!lookup.lease.has_value());
        CHECK(!consumed.has_value());

        runtime.run_owner_callback_for_test([&] {
            runtime.sweep_p51_reservations_on_owner();
        });
        auto fresh_request = test_p51_reservation_request(
            remote_c, 81 + index, launch.identity.generation,
            launch.identity.attempt, 8300 + index, profiles[index].mask, 30);
        const auto fresh_result =
            runtime.reserve_p51_source_on_owner(fresh_request);
        CHECK(fresh_result.error_code == 0 && fresh_result.armed.has_value());
        CHECK(fresh_result.armed->reservation_id !=
              expiring_armed.reservation_id);
        LinkHello fresh_link = test_p51_link_hello(
            *fresh_result.armed, 2, HistoryNonce{0x830001 + index});
        JobBind fresh_binding = test_p51_job_binding(
            *fresh_result.armed, fresh_link.physical_link_generation, 1,
            83000 + index, "fresh-after-expired-bind");
        P51SourceLinkLookupResult fresh_lookup;
        std::optional<P51SourceJobLease> fresh_consumed;
        runtime.run_owner_callback_for_test([&] {
            fresh_lookup = runtime.lookup_p51_link_reservation_on_owner(
                fresh_link);
            fresh_consumed = runtime.consume_p51_job_reservation_on_owner(
                fresh_link, fresh_binding);
            if (fresh_consumed)
                runtime.settle_p51_cancelled_job_on_owner(
                    fresh_link, fresh_binding);
        });
        CHECK(fresh_lookup.status == P51SourceLinkLookupStatus::Found);
        CHECK(fresh_lookup.lease.has_value());
        CHECK(fresh_consumed.has_value());
        CHECK(fresh_consumed->binding.reservation_id ==
              fresh_binding.reservation_id);
        std::printf("P51_RESERVATION_EXPIRY profile=%s queued=no-arm/slot-free "
                    "expired-bind=denied fresh=bound\n",
                    profiles[index].name);
    }
    std::puts("P51_RESERVATION_EXPIRY before-bind/no-late-runtime-arm: ok");
}

void test_p51_reservation_profile_mask_mapping() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x49;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    StoreIdentityRoot remote_root{};
    remote_root.bytes[15] = 0x4a;
    const CStoreGuid remote_c = c_store_guid_for_root(remote_root);
    const struct ProfileCase {
        uint32_t mask;
        ProfileId id;
    } profiles[] = {
        {CACHE_PROFILE_P29V1, ProfileId::P29V1},
        {CACHE_PROFILE_ZSTD_TU, ProfileId::ZSTD_TU},
        {CACHE_PROFILE_ZSTD_ROUTE, ProfileId::ZSTD_ROUTE},
    };

    for (size_t index = 0; index != std::size(profiles); ++index) {
        service::RuntimeConfig config = test_runtime_config();
        config.c_store_guid = launch.c_store_guid;
        config.f_store_guid = launch.f_store_guid;
        config.f_store_generation = launch.store_generation;
        config.sidecar_launch = launch;
        service::SidecarRuntime runtime(std::move(config));
        const auto request = test_p51_reservation_request(
            remote_c, 70 + index, launch.identity.generation,
            launch.identity.attempt, 7400 + index, profiles[index].mask, 30);
        const auto result = runtime.reserve_p51_source_on_owner(request);
        CHECK(result.error_code == 0 && result.armed.has_value());

        LinkHello hello = test_p51_link_hello(
            *result.armed, 700 + index, HistoryNonce{0x7400 + index});
        CHECK(hello.profile == profiles[index].id);
        LinkHello wrong_profile_hello = hello;
        wrong_profile_hello.profile = profiles[index].id == ProfileId::ZSTD_ROUTE
                                          ? ProfileId::ZSTD_TU
                                          : ProfileId::ZSTD_ROUTE;
        std::optional<P51SourceLinkLease> link_lease;
        runtime.run_owner_callback_for_test([&] {
            const auto invalid = runtime.lookup_p51_link_reservation_on_owner(
                wrong_profile_hello);
            CHECK(invalid.status == P51SourceLinkLookupStatus::Invalid);
            CHECK(!invalid.has_value());
            auto absent_initial = hello;
            absent_initial.reservation_id = Id128::from_u64(0x7fff0000 + index);
            const auto missing_initial =
                runtime.lookup_p51_link_reservation_on_owner(absent_initial);
            CHECK(missing_initial.status ==
                  P51SourceLinkLookupStatus::ReservationMissing);
            auto absent_relationship = hello;
            absent_relationship.start_mode = LinkStartMode::Reconnect;
            absent_relationship.c_store_guid.bytes[15] ^= 0x80;
            absent_relationship.physical_link_generation += 1;
            const auto missing_reconnect =
                runtime.lookup_p51_link_reservation_on_owner(absent_relationship);
            CHECK(missing_reconnect.status ==
                  P51SourceLinkLookupStatus::ReservationMissing);
            const auto valid = runtime.lookup_p51_link_reservation_on_owner(hello);
            CHECK(valid.status == P51SourceLinkLookupStatus::Found);
            link_lease = valid;
        });
        CHECK(link_lease.has_value());

        JobBind binding = test_p51_job_binding(
            *result.armed, hello.physical_link_generation, 1, 84 + index,
            "profile-mask-binding");
        CHECK(binding.profile == profiles[index].id);
        JobBind wrong_profile_binding = binding;
        wrong_profile_binding.profile =
            profiles[index].id == ProfileId::ZSTD_ROUTE
                ? ProfileId::ZSTD_TU
                : ProfileId::ZSTD_ROUTE;
        std::optional<P51SourceJobLease> consumed;
        runtime.run_owner_callback_for_test([&] {
            CHECK(!runtime.consume_p51_job_reservation_on_owner(
                hello, wrong_profile_binding).has_value());
            consumed = runtime.consume_p51_job_reservation_on_owner(
                hello, binding);
        });
        CHECK(consumed.has_value());
    }
    std::puts("P51_RESERVATION_OWNER profile-mask-mapping/all-three: ok");
}

void test_p51_same_f_missing_relationship_reassignment_keeps_sibling() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x4b;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    auto remote_c = [](uint8_t tag) {
        StoreIdentityRoot root{};
        root.bytes[15] = tag;
        return c_store_guid_for_root(root);
    };
    const CStoreGuid old_c = remote_c(0x4c);
    const CStoreGuid sibling_c = remote_c(0x4d);
    const CStoreGuid pressure_c = remote_c(0x4e);

    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_route_relationships = 2;
    config.max_pending_p51_source_reservations = 8;
    service::SidecarRuntime runtime(std::move(config));

    auto reserve = [&](const CStoreGuid& c_guid, uint64_t c_generation,
                       uint64_t request_id) {
        return test_p51_reservation_request(
            c_guid, c_generation, launch.identity.generation,
            launch.identity.attempt, request_id, CACHE_PROFILE_ZSTD_TU, 30,
            std::chrono::seconds(20));
    };
    auto old_request = reserve(old_c, 81, 8101);
    const auto old_result = runtime.reserve_p51_source_on_owner(old_request);
    CHECK(old_result.error_code == 0 && old_result.armed.has_value());
    const P51SourceArmedFields old_armed = *old_result.armed;

    auto sibling_request = reserve(sibling_c, 82, 8201);
    const auto sibling_result = runtime.reserve_p51_source_on_owner(
        sibling_request);
    CHECK(sibling_result.error_code == 0 && sibling_result.armed.has_value());
    const P51SourceArmedFields sibling_armed = *sibling_result.armed;
    LinkHello sibling_link = test_p51_link_hello(
        sibling_armed, 81, HistoryNonce{0x810001});
    runtime.run_owner_callback_for_test([&] {
        const auto link = runtime.lookup_p51_link_reservation_on_owner(
            sibling_link);
        CHECK(link.status == P51SourceLinkLookupStatus::Found &&
              link.lease.has_value());
    });

    auto make_commit = [](const LinkHello& link, const JobBind& binding) {
        R2TxCommit commit;
        commit.relationship_ordinal = binding.relationship_ordinal;
        commit.binding_digest = compute_r2_binding_digest(binding);
        commit.transaction_digest = icecc::digest128(
            "same-f-retirement/" +
            std::to_string(binding.relationship_ordinal));
        commit.inner.history_nonce = link.history_nonce;
        commit.inner.rel_seq = RelSeq{binding.relationship_ordinal - 1};
        commit.inner.tu_seq = binding.tu_seq;
        commit.inner.transaction_digest = commit.transaction_digest;
        commit.inner.raw_digest = binding.raw_digest;
        return commit;
    };
    auto commit_exact = [&](const LinkHello& link,
                            const P51SourceArmedFields& armed,
                            uint64_t ordinal, uint64_t tu_seq,
                            std::string_view raw) {
        const JobBind binding = test_p51_job_binding(
            armed, link.physical_link_generation, ordinal, tu_seq, raw);
        bool committed = false;
        runtime.run_owner_callback_for_test([&] {
            const auto lease = runtime.consume_p51_job_reservation_on_owner(
                link, binding);
            CHECK(lease.has_value());
            CHECK(runtime.authorize_p51_job_publication_on_owner(link, binding));
            const auto commit = make_commit(link, binding);
            CHECK(runtime.record_p51_job_commit_on_owner(
                link, binding, commit));
            CommitAck ack{link.relationship_id, link.relationship_epoch,
                          link.physical_link_generation, ordinal};
            CHECK(runtime.acknowledge_p51_receipt_on_owner(link, ack));
            committed = true;
        });
        return committed;
    };

    // Establish a live sibling link and commit once before the missing
    // relationship is retired. Its second job below proves the exact sibling
    // relationship remains usable after old-C eviction and fresh assignment.
    CHECK(commit_exact(sibling_link, sibling_armed, 1, 0,
                       "healthy sibling before miss"));

    CHECK(runtime.cancel_p51_source_on_owner(
        old_request.arm, old_armed.reservation_id,
        old_request.absolute_deadline.as_steady_time_point()));
    auto pressure_request = reserve(pressure_c, 83, 8301);
    const auto pressure_result = runtime.reserve_p51_source_on_owner(
        pressure_request);
    CHECK(pressure_result.error_code == 0 && pressure_result.armed.has_value());
    const P51SourceArmedFields pressure_armed = *pressure_result.armed;
    CHECK(runtime.cancel_p51_source_on_owner(
        pressure_request.arm, pressure_armed.reservation_id,
        pressure_request.absolute_deadline.as_steady_time_point()));

    LinkHello old_reconnect = test_p51_link_hello(
        old_armed, 82, HistoryNonce{0x820001}, LinkStartMode::Reconnect);
    old_reconnect.physical_link_generation = 82;
    P51SourceLinkLookupResult stale_lookup;
    runtime.run_owner_callback_for_test([&] {
        stale_lookup = runtime.lookup_p51_link_reservation_on_owner(
            old_reconnect);
    });
    CHECK(stale_lookup.status ==
          P51SourceLinkLookupStatus::ReservationMissing);

    auto fresh_request = reserve(old_c, 81, 8102);
    const auto fresh_result = runtime.reserve_p51_source_on_owner(fresh_request);
    CHECK(fresh_result.error_code == 0 && fresh_result.armed.has_value());
    const P51SourceArmedFields fresh_armed = *fresh_result.armed;
    CHECK(fresh_armed.f_store_guid == old_armed.f_store_guid &&
          fresh_armed.f_store_generation == old_armed.f_store_generation &&
          fresh_armed.logical_relationship_id != old_armed.logical_relationship_id &&
          fresh_armed.relationship_epoch > old_armed.relationship_epoch);
    LinkHello fresh_link = test_p51_link_hello(
        fresh_armed, 83, HistoryNonce{0x830001});
    auto sibling_next_request = reserve(sibling_c, 82, 8202);
    const auto sibling_next_result = runtime.reserve_p51_source_on_owner(
        sibling_next_request);
    CHECK(sibling_next_result.error_code == 0 &&
          sibling_next_result.armed.has_value());
    const P51SourceArmedFields sibling_next = *sibling_next_result.armed;
    CHECK(sibling_next.logical_relationship_id ==
          sibling_armed.logical_relationship_id);
    runtime.run_owner_callback_for_test([&] {
        const auto fresh_lookup =
            runtime.lookup_p51_link_reservation_on_owner(fresh_link);
        CHECK(fresh_lookup.status == P51SourceLinkLookupStatus::Found &&
              fresh_lookup.lease.has_value());
        auto stale_logical_offer = sibling_link;
        stale_logical_offer.relationship_id = Id128::from_u64(0xdeadbeef);
        stale_logical_offer.reservation_id =
            Id128{sibling_next.reservation_id};
        const auto stale = runtime.lookup_p51_link_reservation_on_owner(
            stale_logical_offer);
        CHECK(stale.status == P51SourceLinkLookupStatus::Invalid);
    });

    CHECK(commit_exact(sibling_link, sibling_next, 2, 1,
                       "healthy sibling after miss"));
    CHECK(commit_exact(fresh_link, fresh_armed, 1, 0,
                       "fresh same-F relationship assignment"));
    std::puts("P51_RESERVATION_OWNER same-F missing/reassignment preserves sibling: ok");
}

void test_p51_same_f_missing_real_sender_transfer_keeps_sibling(
    bool established_link_reconnect = false,
    bool interleaved_trace_only = false) {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x5b;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    auto remote_c = [](uint8_t tag) {
        StoreIdentityRoot root{};
        root.bytes[15] = tag;
        return c_store_guid_for_root(root);
    };
    const CStoreGuid old_c = remote_c(0x5c);
    const CStoreGuid sibling_c = remote_c(0x5d);

    service::RuntimeConfig server_config = test_runtime_config();
    server_config.c_store_guid = launch.c_store_guid;
    server_config.f_store_guid = launch.f_store_guid;
    server_config.f_store_generation = launch.store_generation;
    server_config.sidecar_launch = launch;
    server_config.endpoint_caps.profile = ProfileId::ZSTD_TU;
    server_config.endpoint_caps.supported_profiles =
        profile_bit(ProfileId::ZSTD_TU);
    server_config.endpoint_caps.zstd.max_raw_bytes = 1U << 20;
    server_config.endpoint_caps.zstd.max_encoded_body_bytes = 1U << 20;
    server_config.max_route_relationships = 2;
    const EndpointCaps transfer_caps = server_config.endpoint_caps;
    service::SidecarRuntime server(std::move(server_config));

    uint16_t port = 0;
    const int listener = loopback_listener(port);
    std::atomic<bool> stopping{false};
    std::atomic<unsigned> accepted{0};
    std::thread accept_thread([&] {
        while (!stopping.load(std::memory_order_acquire)) {
            pollfd ready{listener, POLLIN, 0};
            const int polled = ::poll(&ready, 1, 100);
            if (polled < 0 && errno == EINTR)
                continue;
            if (polled <= 0)
                continue;
            sockaddr_storage peer{};
            socklen_t peer_size = sizeof(peer);
            const int fd = ::accept(
                listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (fd < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (!interleaved_trace_only) {
                // The original route-owner path already speaks raw R2; keep
                // its fixture transport exactly as it was before adding the
                // SidecarRuntime sender branch below.
                accepted.fetch_add(1, std::memory_order_relaxed);
                server.start_adopted_r2_endpoint(fd);
                continue;
            }
            // A production SidecarRuntime sender first negotiates the public
            // MsgChannel protocol and P51 cache-link session. Only the handed-
            // off descriptor is an R2 byte stream for this endpoint.
            std::unique_ptr<MsgChannel> channel(
                Service::createChannelAccepted(
                    fd, reinterpret_cast<sockaddr*>(&peer), peer_size));
            if (!channel)
                continue;
            const auto handshake_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            bool protocol_ready = true;
            while (channel->protocol_admission_state() ==
                       MsgChannel::ProtocolAdmissionState::Pending &&
                   std::chrono::steady_clock::now() < handshake_deadline) {
                short events = POLLIN;
                if (channel->has_pending_write())
                    events |= POLLOUT;
                pollfd socket{channel->fd, events, 0};
                int polled_handshake;
                do {
                    polled_handshake = ::poll(&socket, 1, 50);
                } while (polled_handshake < 0 && errno == EINTR);
                if (polled_handshake < 0 ||
                    (socket.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    protocol_ready = false;
                    break;
                }
                if ((socket.revents & POLLOUT) &&
                    !channel->flush_pending()) {
                    protocol_ready = false;
                    break;
                }
                if ((socket.revents & POLLIN) && !channel->read_a_bit()) {
                    protocol_ready = false;
                    break;
                }
            }
            if (!protocol_ready ||
                channel->protocol_admission_state() !=
                    MsgChannel::ProtocolAdmissionState::Ready ||
                !channel->finish_protocol_admission())
                continue;
            std::unique_ptr<Msg> link_request(
                channel->get_msg_until(handshake_deadline));
            if (dynamic_cast<P51CacheLinkSessionMsg*>(link_request.get()) ==
                nullptr)
                continue;
            const int adopted_fd =
                channel->send_p51_cache_link_session_ready_and_release(
                    handshake_deadline);
            if (adopted_fd < 0)
                continue;
            accepted.fetch_add(1, std::memory_order_relaxed);
            server.start_adopted_r2_endpoint(adopted_fd);
        }
    });

    if (interleaved_trace_only) {
        // Exercise two independent C service runtimes against this one F
        // SidecarRuntime. The bounded rendezvous runs after each complete
        // C->F bundle write, so both exact links are live simultaneously.
        // Each client runtime has its own fixed owner thread; this avoids
        // migrating either sender's thread-affine state.
        struct BundleRendezvous {
            std::mutex mutex;
            std::condition_variable changed;
            size_t arrivals = 0;
            bool timed_out = false;
            std::vector<uint64_t> ordinals;

            void arrive(uint64_t ordinal) {
                std::unique_lock lock(mutex);
                ordinals.push_back(ordinal);
                ++arrivals;
                changed.notify_all();
                if (!changed.wait_for(lock, std::chrono::seconds(5), [&] {
                        return arrivals >= 2;
                    }))
                    timed_out = true;
            }
        } rendezvous;

        std::unique_ptr<service::SidecarRuntime> first_client;
        std::unique_ptr<service::SidecarRuntime> second_client;
        bool clients_stopped = false;
        const auto stop_clients = [&] {
            if (clients_stopped)
                return;
            if (first_client)
                first_client->stop();
            if (second_client)
                second_client->stop();
            clients_stopped = true;
        };
        auto branch_cleanup = std::unique_ptr<int, std::function<void(int*)>>(
            reinterpret_cast<int*>(1), [&](int*) {
                stop_clients();
                stopping.store(true, std::memory_order_release);
                (void)::shutdown(listener, SHUT_RDWR);
                if (accept_thread.joinable())
                    accept_thread.join();
                (void)::close(listener);
                server.stop();
            });

        auto make_client = [&](uint8_t root_tag) {
            StoreIdentityRoot root{};
            root.bytes[15] = root_tag;
            const SidecarLaunchIdentity client_launch = test_sidecar_launch(root);
            service::RuntimeConfig config = test_runtime_config();
            config.c_store_guid = client_launch.c_store_guid;
            config.f_store_guid = client_launch.f_store_guid;
            config.f_store_generation = client_launch.store_generation;
            config.sidecar_launch = client_launch;
            config.endpoint_caps.profile = ProfileId::ZSTD_TU;
            config.endpoint_caps.supported_profiles =
                profile_bit(ProfileId::ZSTD_TU);
            config.endpoint_caps.zstd.max_raw_bytes = 1U << 20;
            config.endpoint_caps.zstd.max_encoded_body_bytes = 1U << 20;
            config.max_active_p51_source_transfers = 2;
            config.max_pending_p51_source_operations = 2;
            config.after_r2_bundle_sent_for_test = [&](uint64_t ordinal) {
                rendezvous.arrive(ordinal);
            };
            return std::pair{client_launch,
                             std::make_unique<service::SidecarRuntime>(
                                 std::move(config))};
        };

        auto [first_launch, first_runtime] = make_client(0x5c);
        auto [second_launch, second_runtime] = make_client(0x5d);
        first_client = std::move(first_runtime);
        second_client = std::move(second_runtime);
        CHECK(first_launch.c_store_guid != second_launch.c_store_guid);
        const char* trace_path = std::getenv("ICECC_P50_SOURCE_RESULT_TRACE");
        CHECK(trace_path != nullptr && *trace_path != '\0');
        struct stat initial_trace_stat{};
        if (::stat(trace_path, &initial_trace_stat) == 0)
            CHECK(initial_trace_stat.st_size == 0);
        else
            CHECK(errno == ENOENT);

        auto make_armed_request = [&](const SidecarLaunchIdentity& client_launch,
                                      uint64_t request_id) {
            auto reservation = test_p51_reservation_request(
                client_launch.c_store_guid, client_launch.store_generation,
                client_launch.identity.generation,
                client_launch.identity.attempt, request_id,
                CACHE_PROFILE_ZSTD_TU, 30, std::chrono::seconds(20));
            reservation.arm.source.assignment_nonce = request_id;
            reservation.arm.source.selected_f_host = "127.0.0.1";
            reservation.arm.source.selected_f_cache_port = port;
            const auto reserved = server.reserve_p51_source_on_owner(reservation);
            CHECK(reserved.error_code == 0 && reserved.armed.has_value());
            return std::pair{
                local::P51SourceTransferRequest{
                    *reserved.armed, reservation.absolute_deadline},
                request_id};
        };
        auto first_request = make_armed_request(first_launch, 9501);
        auto second_request = make_armed_request(second_launch, 9502);
        RuntimeCase first_pair = authenticated_runtime_pair();
        RuntimeCase second_pair = authenticated_runtime_pair();
        const auto enqueue = [&](service::SidecarRuntime& client,
                                 const SidecarLaunchIdentity& client_launch,
                                 RuntimeCase& pair,
                                 const local::P51SourceTransferRequest& request,
                                 uint8_t fill) {
            const auto operation = local::make_p51_source_transfer_operation(
                client_launch.identity, request,
                request.armed.arm.source.source_request_id);
            return client.enqueue_p51_source_transfer(
                std::move(pair.sender), client_launch.identity, operation,
                sized_test_source_fd(37, fill)) ==
                service::P51SourceEnqueueResult::Accepted;
        };
        CHECK(enqueue(*first_client, first_launch, first_pair,
                      first_request.first, 0xb1));
        CHECK(enqueue(*second_client, second_launch, second_pair,
                      second_request.first, 0xb2));
        const auto first_deadline =
            first_request.first.absolute_deadline.as_steady_time_point();
        const auto second_deadline =
            second_request.first.absolute_deadline.as_steady_time_point();
        const auto first_result = receive_p51_transfer_result(
            first_pair.receiver, first_launch.identity, first_request.second,
            first_deadline, true);
        const auto second_result = receive_p51_transfer_result(
            second_pair.receiver, second_launch.identity, second_request.second,
            second_deadline, true);
        CHECK(first_result.code == local::SourceTransferResultCode::Committed &&
              second_result.code == local::SourceTransferResultCode::Committed);
        CHECK(first_result.c_store_guid == first_launch.c_store_guid &&
              second_result.c_store_guid == second_launch.c_store_guid &&
              first_result.tu_seq == second_result.tu_seq);
        {
            std::lock_guard lock(rendezvous.mutex);
            CHECK(!rendezvous.timed_out && rendezvous.arrivals == 2 &&
                  rendezvous.ordinals.size() == 2 &&
                  rendezvous.ordinals[0] == rendezvous.ordinals[1]);
        }

        stop_clients();
        const auto release_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(3);
        const auto guid_hex = [](const CStoreGuid& guid) {
            constexpr char digits[] = "0123456789abcdef";
            std::string value;
            value.reserve(guid.bytes.size() * 2);
            for (const uint8_t byte : guid.bytes) {
                value.push_back(digits[byte >> 4]);
                value.push_back(digits[byte & 0xf]);
            }
            return value;
        };
        const std::string first_c_hex = guid_hex(first_launch.c_store_guid);
        const std::string second_c_hex = guid_hex(second_launch.c_store_guid);
        bool both_released = false;
        std::set<std::string> released_logical_ids;
        while (std::chrono::steady_clock::now() < release_deadline) {
            std::ifstream trace(trace_path);
            std::string line;
            bool first_release = false;
            bool second_release = false;
            released_logical_ids.clear();
            while (std::getline(trace, line)) {
                if (line.find("\"event\":\"link_released\"") ==
                    std::string::npos)
                    continue;
                if (line.find(first_c_hex) != std::string::npos)
                    first_release = true;
                if (line.find(second_c_hex) != std::string::npos)
                    second_release = true;
                const std::string key = "\"logical_link_id\":\"";
                const size_t start = line.find(key);
                if (start != std::string::npos) {
                    const size_t value_start = start + key.size();
                    const size_t end = line.find('"', value_start);
                    if (end != std::string::npos)
                        released_logical_ids.insert(
                            line.substr(value_start, end - value_start));
                }
            }
            if (first_release && second_release &&
                released_logical_ids.size() == 2) {
                both_released = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(both_released && accepted.load(std::memory_order_acquire) == 2);
        std::puts("P51_R2_TRACE two-C/one-F concurrent bundle rendezvous: PASS");
        return;
    }

    P50RouteOwnerConfig owner_config;
    owner_config.endpoint_caps = transfer_caps;
    owner_config.authority_limits.max_speculative_tus = 30;
    P50CRouteOwner old_owner(owner_config);
    P50CRouteOwner sibling_owner(owner_config);
    boost::asio::io_context c_context;
    auto c_work = boost::asio::make_work_guard(c_context);
    std::thread c_thread([&] { c_context.run(); });
    auto cleanup = std::unique_ptr<int, std::function<void(int*)>>(
        reinterpret_cast<int*>(1), [&](int*) {
            stopping.store(true, std::memory_order_release);
            (void)::shutdown(listener, SHUT_RDWR);
            if (accept_thread.joinable())
                accept_thread.join();
            (void)::close(listener);
            server.stop();
            c_context.stop();
            if (c_thread.joinable())
                c_thread.join();
        });

    auto reserve = [&](const CStoreGuid& c_guid, uint64_t c_generation,
                       uint64_t request_id) {
        auto request = test_p51_reservation_request(
            c_guid, c_generation, 71, 1, request_id,
            CACHE_PROFILE_ZSTD_TU, 30, std::chrono::seconds(20));
        request.arm.source.selected_f_host = "127.0.0.1";
        request.arm.source.selected_f_cache_port = port;
        request.arm.source.assignment_nonce = request_id;
        const auto result = server.reserve_p51_source_on_owner(request);
        CHECK(result.error_code == 0 && result.armed.has_value());
        return std::pair{request, *result.armed};
    };
    auto connector = [port](auto deadline, auto completion) {
        if (std::chrono::steady_clock::now() >= deadline) {
            completion(-1);
            return;
        }
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            completion(-1);
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) != 0) {
            (void)::close(fd);
            completion(-1);
            return;
        }
        completion(fd);
    };
    auto transfer = [&](P50CRouteOwner& route_owner,
                        const CStoreGuid& c_guid,
                        const P51SourceArmedFields& armed,
                        const std::string& bytes) {
        const auto& source = armed.arm.source;
        const P50RouteRelationship relationship{
            c_guid, FStoreGuid{armed.f_store_guid},
            armed.f_store_generation, ProfileId::ZSTD_TU};
        const PrepareRequestKey request{source.assignment_epoch,
                                        source.assignment_nonce};
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(8);
        const auto raw = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        return boost::asio::co_spawn(
            c_context,
            route_owner.transfer_p51(relationship, armed, connector,
                                     request, deadline, raw),
            boost::asio::use_future).get();
    };

    const auto [old_request, old_armed] = reserve(old_c, 91, 9101);
    const auto [sibling_request, sibling_armed] = reserve(sibling_c, 92, 9201);
    (void)sibling_request;
    const std::string sibling_first = "int sibling_before = 11;\n";
    const auto sibling_first_result =
        transfer(sibling_owner, sibling_c, sibling_armed, sibling_first);
    CHECK(sibling_first_result.status == ZstdSourceTransferStatus::Committed &&
          sibling_first_result.raw_bytes == sibling_first.size() &&
          sibling_first_result.raw_digest ==
              icecc::digest128(std::string_view(sibling_first)));

    uint64_t old_link_generation = 1;
    if (established_link_reconnect) {
        const std::string old_first = "int established_old_link = 5;\n";
        const auto old_first_result =
            transfer(old_owner, old_c, old_armed, old_first);
        CHECK(old_first_result.status == ZstdSourceTransferStatus::Committed &&
              old_first_result.raw_bytes == old_first.size() &&
              old_first_result.raw_digest ==
                  icecc::digest128(std::string_view(old_first)));
        // The returned result is the receipt; give the independent ACK pump a
        // bounded local scheduling interval before closing this connection.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Close only the old physical sender, retaining the same C route owner
        // and the independent sibling relationship. The F endpoint must then
        // evict only the exact idle relationship history before the stale
        // reconnect and higher-epoch ARM below.
        std::promise<void> old_link_closed_promise;
        auto old_link_closed = old_link_closed_promise.get_future();
        boost::asio::post(c_context, [&] {
            old_owner.cancel_active_p51_transfers();
            old_link_closed_promise.set_value();
        });
        CHECK(old_link_closed.wait_for(std::chrono::seconds(3)) ==
              std::future_status::ready);
        old_link_closed.get();

        auto pressure_request = test_p51_reservation_request(
            remote_c(0x5e), 93, launch.identity.generation,
            launch.identity.attempt, 9301, CACHE_PROFILE_ZSTD_TU, 30,
            std::chrono::seconds(20));
        pressure_request.arm.source.selected_f_host = "127.0.0.1";
        pressure_request.arm.source.selected_f_cache_port = port;
        std::optional<P51SourceArmedFields> pressure_armed;
        const auto eviction_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < eviction_deadline) {
            const auto result =
                server.reserve_p51_source_on_owner(pressure_request);
            if (result.armed) {
                pressure_armed = *result.armed;
                break;
            }
            CHECK(result.error_code == 0x5103); // bounded relationship capacity
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(pressure_armed.has_value());
        CHECK(server.cancel_p51_source_on_owner(
            pressure_request.arm, pressure_armed->reservation_id,
            pressure_request.absolute_deadline.as_steady_time_point()));

        // Profile-history retirement must not evict the already committed
        // compiler input from the same C namespace. Attach it after the old
        // relationship has been retired and compare every byte.
        CHECK(old_first_result.committed_input.has_value());
        const InputLeaseOwner old_input_owner{
            old_armed.arm.source.logical_job,
            old_armed.arm.source.assignment_epoch,
            old_armed.arm.source.assignment_nonce};
        const InputFdRequest old_input_request{
            launch.identity, *old_first_result.committed_input,
            old_input_owner, 0x5c01};
        const auto old_input_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        auto old_input_cursor = server.attach_input_on_owner(
            old_input_request, old_input_deadline);
        CHECK(old_input_cursor.has_value() &&
              old_input_cursor->remaining() == old_first.size() &&
              old_input_cursor->raw_digest() ==
                  icecc::digest128(std::string_view(old_first)));
        std::vector<uint8_t> old_input_bytes(old_first.size());
        CHECK(old_input_cursor->read(old_input_bytes) == old_input_bytes.size());
        CHECK(old_input_bytes == std::vector<uint8_t>(old_first.begin(),
                                                       old_first.end()));
        server.finish_input_attachment_on_owner(
            old_input_request, true, old_input_deadline);

        LinkHello stale_reconnect = test_p51_link_hello(
            old_armed, old_link_generation + 1, HistoryNonce{1},
            LinkStartMode::Reconnect, 1);
        const int stale_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(stale_fd >= 0);
        sockaddr_in stale_address{};
        stale_address.sin_family = AF_INET;
        stale_address.sin_port = htons(port);
        stale_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::connect(stale_fd,
                        reinterpret_cast<const sockaddr*>(&stale_address),
                        sizeof(stale_address)) == 0);
        const auto stale_frame = encode_frame(Message{stale_reconnect});
        CHECK(write_all(stale_fd, stale_frame));
        auto read_exact_before = [](int fd, std::span<uint8_t> output,
                                    std::chrono::steady_clock::time_point deadline) {
            size_t offset = 0;
            while (offset != output.size()) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                    return false;
                const auto remaining = std::chrono::duration_cast<
                    std::chrono::milliseconds>(deadline - now);
                pollfd readable{fd, POLLIN, 0};
                const int polled = ::poll(
                    &readable, 1, static_cast<int>(std::max<int64_t>(
                                      1, remaining.count())));
                if (polled < 0 && errno == EINTR)
                    continue;
                if (polled <= 0 || !(readable.revents & POLLIN) ||
                    (readable.revents & POLLNVAL))
                    return false;
                const ssize_t count = ::recv(
                    fd, output.data() + offset, output.size() - offset, 0);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    return false;
                offset += static_cast<size_t>(count);
            }
            return true;
        };
        const auto response_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::array<uint8_t, 4> response_header{};
        CHECK(read_exact_before(stale_fd, response_header, response_deadline));
        const FrameHeader decoded_header =
            decode_frame_header(response_header, kInitialMaxFramePayload);
        CHECK(decoded_header.type == MessageType::R2_LINK_REJECT);
        std::vector<uint8_t> response_payload(decoded_header.payload_bytes);
        CHECK(read_exact_before(stale_fd, response_payload, response_deadline));
        const Message response =
            decode_payload(decoded_header.type, response_payload);
        const auto* rejection = std::get_if<LinkRejectMessage>(&response);
        CHECK(rejection != nullptr && rejection->valid() &&
              rejection->reason == LinkRejectReason::ReservationMissing &&
              rejection->offered_hello_digest ==
                  compute_r2_link_offer_digest(stale_reconnect));
        CHECK(::shutdown(stale_fd, SHUT_RDWR) == 0);
        CHECK(::close(stale_fd) == 0);
    } else {
        CHECK(server.cancel_p51_source_on_owner(
            old_request.arm, old_armed.reservation_id,
            old_request.absolute_deadline.as_steady_time_point()));
        auto pressure = reserve(remote_c(0x5e), 93, 9301);
        const auto& pressure_request = pressure.first;
        const auto& pressure_armed = pressure.second;
        CHECK(server.cancel_p51_source_on_owner(
            pressure_request.arm, pressure_armed.reservation_id,
            pressure_request.absolute_deadline.as_steady_time_point()));

        const std::string stale_bytes = "int stale_old_relationship = 3;\n";
        const auto stale_result =
            transfer(old_owner, old_c, old_armed, stale_bytes);
        std::fprintf(stderr,
                     "same-F stale result status=%u reject=%u reason=%u bytes=%llu "
                     "replacement=%u local=%u attempts=%u\n",
                     static_cast<unsigned>(stale_result.status),
                     stale_result.r2_link_rejection.has_value(),
                     stale_result.r2_link_rejection
                         ? static_cast<unsigned>(stale_result.r2_link_rejection->reason)
                         : 0u,
                     static_cast<unsigned long long>(stale_result.raw_bytes),
                     stale_result.replacement_required,
                     stale_result.route_local_failure,
                     static_cast<unsigned>(stale_result.attempts));
        CHECK(stale_result.status != ZstdSourceTransferStatus::Committed &&
              stale_result.r2_link_rejection.has_value() &&
              stale_result.r2_link_rejection->reason ==
                  LinkRejectReason::ReservationMissing &&
              stale_result.raw_bytes == 0 &&
              stale_result.raw_digest == Digest128{} &&
              !stale_result.replacement_required);
    }

    const auto [fresh_request, fresh_armed] = reserve(old_c, 91, 9102);
    (void)fresh_request;
    CHECK(fresh_armed.logical_relationship_id !=
              old_armed.logical_relationship_id &&
          fresh_armed.relationship_epoch > old_armed.relationship_epoch);
    const std::string fresh_bytes = "int fresh_same_f_assignment = 7;\n";
    const auto fresh_result = transfer(old_owner, old_c, fresh_armed, fresh_bytes);
    std::fprintf(stderr,
                 "same-F fresh result status=%u reject=%u reason=%u bytes=%llu "
                 "replacement=%u local=%u attempts=%u accepted=%u\n",
                 static_cast<unsigned>(fresh_result.status),
                 fresh_result.r2_link_rejection.has_value(),
                 fresh_result.r2_link_rejection
                     ? static_cast<unsigned>(fresh_result.r2_link_rejection->reason)
                     : 0u,
                 static_cast<unsigned long long>(fresh_result.raw_bytes),
                 fresh_result.replacement_required,
                 fresh_result.route_local_failure,
                 static_cast<unsigned>(fresh_result.attempts),
                 accepted.load(std::memory_order_relaxed));
    CHECK(fresh_result.status == ZstdSourceTransferStatus::Committed &&
          fresh_result.raw_bytes == fresh_bytes.size() &&
          fresh_result.raw_digest ==
              icecc::digest128(std::string_view(fresh_bytes)));

    auto sibling_next = reserve(sibling_c, 92, 9202);
    CHECK(sibling_next.second.logical_relationship_id ==
          sibling_armed.logical_relationship_id);
    const std::string sibling_second = "int sibling_after = 13;\n";
    const auto sibling_second_result =
        transfer(sibling_owner, sibling_c, sibling_next.second,
                 sibling_second);
    CHECK(sibling_second_result.status == ZstdSourceTransferStatus::Committed &&
          sibling_second_result.raw_bytes == sibling_second.size() &&
          sibling_second_result.raw_digest ==
              icecc::digest128(std::string_view(sibling_second)));

    // Retire the actual C senders on their owner executor, then shut the F
    // endpoint down only after all exact commit results have been observed.
    std::promise<void> c_retired_promise;
    auto c_retired = c_retired_promise.get_future();
    boost::asio::post(c_context, [&] {
        old_owner.cancel_active_p51_transfers();
        sibling_owner.cancel_active_p51_transfers();
        old_owner.reset();
        sibling_owner.reset();
        c_retired_promise.set_value();
    });
    CHECK(c_retired.wait_for(std::chrono::seconds(3)) ==
          std::future_status::ready);
    c_retired.get();
    c_work.reset();
    cleanup.reset();
    CHECK(accepted.load(std::memory_order_relaxed) ==
          (established_link_reconnect ? 4u : 3u));
    std::puts(established_link_reconnect
                  ? "P51_R2_SERVICE established same-F reconnect/missing/reassignment/sibling: ok"
                  : "P51_R2_SERVICE same-F actual sender transfer/missing/reassignment/sibling: ok");
}

void test_route_endpoint_cap_refuses_before_f_open() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 9;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_route_endpoint_identities = 1;
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot retained_remote_root{};
    retained_remote_root.bytes[14] = 0x51;
    CHECK(runtime.seed_route_endpoint_identity_for_test(
        "127.0.0.2", 31001, f_store_guid_for_root(retained_remote_root), 1));

    uint16_t novel_port = 0;
    const int listener = loopback_listener(novel_port);
    const int listener_flags = ::fcntl(listener, F_GETFL);
    CHECK(listener_flags >= 0);
    CHECK(::fcntl(listener, F_SETFL, listener_flags | O_NONBLOCK) == 0);

    char source_path[] = "/tmp/p50-route-endpoint-cap-XXXXXX";
    const int source_fd = ::mkstemp(source_path);
    CHECK(source_fd >= 0);
    const std::array<uint8_t, 4> source{'c', 'a', 'p', '\n'};
    CHECK(write_all(source_fd, source));
    CHECK(::unlink(source_path) == 0);

    local::P50SourceTransferRequest request;
    request.wire_job_id = 71;
    request.assignment_epoch = 72;
    request.assignment_nonce = 73;
    request.selected_f_host = "127.0.0.1";
    request.selected_f_ordinary_port = novel_port;
    request.selected_f_cache_port = novel_port;
    request.cache_protocol = CACHE_WIRE_REVISION;
    request.cache_profile = CACHE_PROFILE_ZSTD_TU;
    request.logical_job = 74;
    request.compiler_attempt = 75;
    request.source_request_id = 76;
    request.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    CHECK(request.valid());
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(2),
        clock.clock_domain_id, clock.time_namespace_id);
    const auto started = std::chrono::steady_clock::now();
    const local::P50SourceTransferResult result = runtime.transfer_source_on_owner(
        request, deadline, local::HandoffFd(source_fd));
    CHECK(result.code == local::SourceTransferResultCode::Error);
    CHECK(result.error_code == static_cast<uint16_t>(
        local::SourceTransferErrorCode::RouteReplacementRequired));
    CHECK(result.attempts == 0);
    CHECK(std::chrono::steady_clock::now() - started <
          std::chrono::milliseconds(250));

    errno = 0;
    const int unexpected = ::accept(listener, nullptr, nullptr);
    CHECK(unexpected < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    CHECK(::close(listener) == 0);
}

void test_known_endpoint_relationship_cap_refuses_before_f_open() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 9;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);

    uint16_t port = 0;
    const int listener = loopback_listener(port);
    const int listener_flags = ::fcntl(listener, F_GETFL);
    CHECK(listener_flags >= 0);
    CHECK(::fcntl(listener, F_SETFL, listener_flags | O_NONBLOCK) == 0);

    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_route_relationships = 1;
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot retained_remote_root{};
    retained_remote_root.bytes[14] = 0x52;
    const FStoreGuid retained_remote_guid =
        f_store_guid_for_root(retained_remote_root);
    CHECK(runtime.seed_route_relationship_for_test(
        "127.0.0.1", port, retained_remote_guid, 1, ProfileId::P29V1));

    char source_path[] = "/tmp/p50-route-relationship-cap-XXXXXX";
    const int source_fd = ::mkstemp(source_path);
    CHECK(source_fd >= 0);
    const std::array<uint8_t, 4> source{'c', 'a', 'p', '\n'};
    CHECK(write_all(source_fd, source));
    CHECK(::unlink(source_path) == 0);

    local::P50SourceTransferRequest request;
    request.wire_job_id = 81;
    request.assignment_epoch = 82;
    request.assignment_nonce = 83;
    request.selected_f_host = "127.0.0.1";
    request.selected_f_ordinary_port = port;
    request.selected_f_cache_port = port;
    request.cache_protocol = CACHE_WIRE_REVISION;
    request.cache_profile = CACHE_PROFILE_ZSTD_TU;
    request.logical_job = 84;
    request.compiler_attempt = 85;
    request.source_request_id = 86;
    request.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    CHECK(request.valid());
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(2),
        clock.clock_domain_id, clock.time_namespace_id);
    const auto started = std::chrono::steady_clock::now();
    const local::P50SourceTransferResult result = runtime.transfer_source_on_owner(
        request, deadline, local::HandoffFd(source_fd));
    CHECK(result.code == local::SourceTransferResultCode::Error);
    CHECK(result.error_code == static_cast<uint16_t>(
        local::SourceTransferErrorCode::RouteReplacementRequired));
    CHECK(result.attempts == 0);
    CHECK(std::chrono::steady_clock::now() - started <
          std::chrono::milliseconds(250));

    errno = 0;
    const int unexpected = ::accept(listener, nullptr, nullptr);
    CHECK(unexpected < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    CHECK(::close(listener) == 0);
}

void structured_c_guid_is_strict() {
    service::RuntimeConfig config;
    config.f_store_guid = Id128::from_u64(9001);
    bool rejected = false;
    try {
        service::SidecarRuntime runtime(config);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
    config.c_store_guid = config.f_store_guid;
    rejected = false;
    try {
        service::SidecarRuntime runtime(config);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

void test_runtime_store_identity_is_explicit_and_role_tagged() {
    StoreIdentityRoot first_root{};
    StoreIdentityRoot restarted_root{};
    CHECK(fresh_store_identity_root(first_root));
    CHECK(fresh_store_identity_root(restarted_root));
    CHECK(first_root != restarted_root);
    const FStoreGuid first = f_store_guid_for_root(first_root);
    const FStoreGuid restarted = f_store_guid_for_root(restarted_root);
    const CStoreGuid first_c = c_store_guid_for_root(first_root);
    CHECK(first != FStoreGuid{} && restarted != FStoreGuid{});
    CHECK(first != restarted);
    CHECK(first_c != first && (first_c.bytes[0] & kStoreIdentityRoleBit) == 0);
    CHECK((first.bytes[0] & kStoreIdentityRoleBit) != 0);
}

void structured_launch_is_complete_and_fail_closed() {
    char template_path[] = "/tmp/icecc-cache-structured-XXXXXX";
    CHECK(::mkdtemp(template_path) != nullptr);
    CHECK(::chmod(template_path, 0700) == 0);
    const std::string root = template_path;
    const std::string expected_socket = root + "/incarnation.sock";
    const std::string stale_socket = root + "/static.sock";
    constexpr local::Identity expected_identity{91, 7};
    const std::string generation = std::to_string(expected_identity.generation);
    const std::string attempt = std::to_string(expected_identity.attempt);
    StoreIdentityRoot store_root{};
    store_root.bytes[15] = 0x44;
    const std::string guid = hex_id(f_store_guid_for_root(store_root));
    const std::string c_guid = hex_id(c_store_guid_for_root(store_root));
    const std::string digest =
        icecc::digest128_hex(icecc::digest128(expected_socket));
    const std::string uid = std::to_string(static_cast<uint64_t>(::getuid()));
    const std::string gid = std::to_string(static_cast<uint64_t>(::getgid()));
    const std::string executable = service_path();
    local::Status prebound_status = local::Status::Ok;
    const int prebound_listener =
        local::listen_unix(expected_socket, 1, &prebound_status);
    CHECK(prebound_listener >= 0 && prebound_status == local::Status::Ok);
    const int prebound_flags = ::fcntl(prebound_listener, F_GETFD);
    CHECK(prebound_flags >= 0);
    CHECK(::fcntl(prebound_listener, F_SETFD, prebound_flags & ~FD_CLOEXEC) == 0);

    int ready[2] = {-1, -1};
    CHECK(::pipe(ready) == 0);
    const std::string ready_fd = std::to_string(ready[1]);
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        (void)::close(ready[0]);
        clear_structured_launch_environment();
        (void)::setenv("ICECC_CACHE_SERVICE_READY_FD", ready_fd.c_str(), 1);
        (void)::setenv("ICECC_CACHE_SERVICE_READY_FORMAT", "2", 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_GENERATION", generation.c_str(), 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_ATTEMPT", attempt.c_str(), 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GENERATION", "191", 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION", "1", 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID", c_guid.c_str(), 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID", guid.c_str(), 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_SOCKET", expected_socket.c_str(), 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST", digest.c_str(), 1);
        const std::string listener_fd = std::to_string(prebound_listener);
        (void)::setenv("ICECC_CACHE_SERVICE_LISTENER_FD", listener_fd.c_str(), 1);
        ::execl(executable.c_str(), executable.c_str(), "--socket", expected_socket.c_str(),
                "--peer-uid", uid.c_str(), "--peer-gid", gid.c_str(), "--generation",
                generation.c_str(), "--attempt", attempt.c_str(), "--f-store-generation", "191",
                "--store-derivation-version", "1", "--c-store-guid", c_guid.c_str(),
                "--f-store-guid", guid.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    (void)::close(ready[1]);
    CHECK(::close(prebound_listener) == 0);
    const std::string ready_message = read_bounded_to_eof(ready[0]);
    (void)::close(ready[0]);
    CHECK(ready_message.rfind("READY v2 generation=91 attempt=7 F_STORE_GENERATION=191 DERIVATION_VERSION=1 pid=", 0) == 0);
    CHECK(ready_message.find(" C_STORE_GUID=" + c_guid) != std::string::npos);
    CHECK(ready_message.find(" F_STORE_GUID=" + guid) != std::string::npos);
    CHECK(ready_message.find(" PATH=" + expected_socket) != std::string::npos);
    CHECK(ready_message.find(" DIGEST=" + digest) != std::string::npos);
    CHECK(ready_message.ends_with("\n"));
    CHECK(::access(stale_socket.c_str(), F_OK) != 0);
    struct stat socket_info{};
    CHECK(::lstat(expected_socket.c_str(), &socket_info) == 0 &&
          S_ISSOCK(socket_info.st_mode) && (socket_info.st_mode & 07777) == 0600);
    local::Status connect_status = local::Status::Ok;
    local::Connection connection = local::connect_unix(expected_socket, &connect_status);
    CHECK(connection.valid() && connect_status == local::Status::Ok);
    CHECK(connection.send(local::make_hello(local::PeerRole::Daemon,
                                            expected_identity)) == local::Status::Ok);
    local::Frame acknowledgement;
    CHECK(connection.receive_with_timeout(acknowledgement, 1000) == local::Status::Ok);
    CHECK(local::validate_handshake(acknowledgement, local::MessageType::HelloAck,
                                    local::PeerRole::Sidecar,
                                    expected_identity) == local::Status::Ok);
    CHECK(::kill(pid, SIGTERM) == 0);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(::access(expected_socket.c_str(), F_OK) == 0);
    CHECK(::unlink(expected_socket.c_str()) == 0);

    int partial_ready[2] = {-1, -1};
    CHECK(::pipe(partial_ready) == 0);
    const std::string partial_fd = std::to_string(partial_ready[1]);
    const pid_t partial = ::fork();
    CHECK(partial >= 0);
    if (partial == 0) {
        (void)::close(partial_ready[0]);
        clear_structured_launch_environment();
        (void)::setenv("ICECC_CACHE_SERVICE_READY_FD", partial_fd.c_str(), 1);
        (void)::setenv("ICECC_CACHE_SERVICE_READY_FORMAT", "2", 1);
        ::execl(executable.c_str(), executable.c_str(), "--socket", stale_socket.c_str(),
                "--peer-uid", uid.c_str(), "--peer-gid", gid.c_str(), "--generation",
                "1", "--attempt", "1", static_cast<char*>(nullptr));
        _exit(127);
    }
    (void)::close(partial_ready[1]);
    CHECK(read_bounded_to_eof(partial_ready[0]).empty());
    (void)::close(partial_ready[0]);
    status = 0;
    CHECK(::waitpid(partial, &status, 0) == partial);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
    CHECK(::access(stale_socket.c_str(), F_OK) != 0);
    CHECK(::access(expected_socket.c_str(), F_OK) != 0);
    CHECK(::rmdir(root.c_str()) == 0);
}

void test_runtime_stop_interrupts_control_wait() {
    service::SidecarRuntime runtime(test_runtime_config());
    RuntimeCase pair = authenticated_runtime_pair();
    service::RuntimeResult runtime_result;
    std::thread worker([&] {
        runtime_result = runtime.run_one(
            pair.receiver, {{7, 1}, 1}, std::chrono::steady_clock::now() + std::chrono::seconds(30));
    });
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (runtime.live_handoff_count() == 0 && std::chrono::steady_clock::now() < wait_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(runtime.live_handoff_count() == 1);
    const auto started = std::chrono::steady_clock::now();
    runtime.stop();
    worker.join();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    CHECK(runtime_result.status == service::RuntimeStatus::Stopped);
    CHECK(runtime_result.handoff.status == local::FdHandoffStatus::Disconnected);
    CHECK(runtime.live_handoff_count() == 0 && runtime.live_session_count() == 0);
}

service::RuntimeResult run_raw_cancel_case(bool partial_dialogue,
                                           bool close_control,
                                           bool deadline_case,
                                           bool malformed_case = false) {
    service::RuntimeConfig runtime_config = test_runtime_config();
    if (deadline_case)
        runtime_config.cancellation_grace = std::chrono::milliseconds(20);
    service::SidecarRuntime runtime(std::move(runtime_config));
    RuntimeCase control = authenticated_runtime_pair();
    uint16_t port = 0;
    const int listener = loopback_listener(port);
    const int peer = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(peer >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    CHECK(::connect(peer, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    const int accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);
    CHECK(::close(listener) == 0);
    const local::HandoffRequest request{{7, 1}, deadline_case ? 4u : 1u};
    service::RuntimeResult result;
    const auto deadline = std::chrono::steady_clock::now() +
                          (deadline_case ? std::chrono::milliseconds(80)
                                         : std::chrono::seconds(5));
    std::thread worker([&] { result = runtime.run_one(control.receiver, request, deadline); });
    local::FdHandoffSender sender{local::HandoffFd(accepted)};
    CHECK(sender.send(control.sender, request,
                      std::chrono::steady_clock::now() + std::chrono::seconds(2))
              .status == local::FdHandoffStatus::Accepted);
    const auto live_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (runtime.live_session_count() == 0 && std::chrono::steady_clock::now() < live_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(runtime.live_session_count() == 1);
    if (malformed_case) {
        const local::Frame malformed{local::kProtocolVersion, local::MessageType::Data,
                                     request.identity, {0xff}};
        CHECK(control.sender.send_until(
                  malformed, std::chrono::steady_clock::now() + std::chrono::seconds(2)) ==
              local::Status::Ok);
    } else if (partial_dialogue) {
        CHECK(send_byte_no_signal(peer, 0));
    }
    if (close_control) {
        CHECK(::shutdown(control.sender.native_handle(), SHUT_RDWR) == 0);
        control.sender = local::Connection(-1);
    } else if (!deadline_case) {
        send_operation_cancel(control.sender, request.identity, request.request_id);
    }
    worker.join();
    CHECK(::close(peer) == 0);
    return result;
}

void test_operation_cancel_prebyte_mid_dialogue_eof_deadline() {
    const service::RuntimeResult prebyte = run_raw_cancel_case(false, false, false);
    CHECK(prebyte.status == service::RuntimeStatus::Cancelled &&
          prebyte.cancellation == service::RuntimeCancellationReason::Requested &&
          prebyte.endpoint.has_value() && !prebyte.endpoint->committed_input.has_value());
    const service::RuntimeResult mid_dialogue = run_raw_cancel_case(true, false, false);
    CHECK(mid_dialogue.status == service::RuntimeStatus::Cancelled &&
          mid_dialogue.cancellation == service::RuntimeCancellationReason::Requested);
    const service::RuntimeResult eof = run_raw_cancel_case(false, true, false);
    CHECK(eof.status == service::RuntimeStatus::Cancelled &&
          eof.cancellation == service::RuntimeCancellationReason::ControlEof);
    const service::RuntimeResult malformed = run_raw_cancel_case(false, false, false, true);
    CHECK(malformed.status == service::RuntimeStatus::Cancelled &&
          malformed.cancellation == service::RuntimeCancellationReason::Malformed);
    const service::RuntimeResult deadline = run_raw_cancel_case(false, false, true);
    CHECK(deadline.status == service::RuntimeStatus::Cancelled &&
          deadline.cancellation == service::RuntimeCancellationReason::Deadline);
}

void test_runtime_cancel_fail_stop_subprocess(bool owner_failure_case = false,
                                              bool live_owner_failure_case = false) {
    int marker[2] = {-1, -1};
    int live_marker[2] = {-1, -1};
    CHECK(::pipe(marker) == 0);
    CHECK(::pipe(live_marker) == 0);
    const auto child_started = std::chrono::steady_clock::now();
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(marker[0]);
        (void)::close(live_marker[0]);
        auto child_abort = [] { _exit(126); };
        service::RuntimeConfig config = test_runtime_config();
        SessionHello hello;
        hello.c_store_guid = config.c_store_guid;
        const std::vector<uint8_t> hello_frame = encode_frame(Message{hello});
        const int marker_fd = marker[1];
        config.cancellation_grace = std::chrono::milliseconds(80);
        if (owner_failure_case) {
            config.owner_failure = [] {
                throw std::runtime_error("injected endpoint owner failure");
            };
        }
        if (live_owner_failure_case) {
            const int live_marker_fd = live_marker[1];
            config.owner_failure_after_live = [live_marker_fd] {
                const uint8_t byte = 1;
                const ssize_t ignored = ::write(live_marker_fd, &byte, sizeof(byte));
                (void)ignored;
                throw std::runtime_error("injected live endpoint owner failure");
            };
        }
        config.fail_stop = [marker_fd] {
            const uint8_t byte = 1;
            const ssize_t ignored = ::write(marker_fd, &byte, sizeof(byte));
            (void)ignored;
            _exit(125);
        };
        service::SidecarRuntime runtime(std::move(config));
        RuntimeCase control = authenticated_runtime_pair();
        uint16_t port = 0;
        const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener < 0)
            child_abort();
        int reuse = 1;
        if (::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
            child_abort();
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            child_abort();
        if (::listen(listener, 1) != 0)
            child_abort();
        socklen_t address_length = sizeof(address);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) != 0)
            child_abort();
        port = ntohs(address.sin_port);
        const int peer = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (peer < 0)
            child_abort();
        address.sin_port = htons(port);
        if (::connect(peer, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
            child_abort();
        const int accepted = ::accept(listener, nullptr, nullptr);
        if (accepted < 0)
            child_abort();
        (void)::close(listener);
        EndpointIoControl endpoint_control;
        std::atomic_bool completion_stalled{false};
        if (!owner_failure_case && !live_owner_failure_case) {
            endpoint_control.before_completion_check = [&completion_stalled](CompletionStamp&) {
                completion_stalled.store(true, std::memory_order_release);
                for (;;)
                    std::this_thread::yield();
            };
        }
        const local::HandoffRequest request{{7, 1}, 88};
        service::RuntimeResult result;
        std::thread worker([&] {
            result = runtime.run_one(
                control.receiver, request,
                std::chrono::steady_clock::now() + std::chrono::milliseconds(150),
                std::move(endpoint_control));
        });
        local::FdHandoffSender sender{local::HandoffFd(accepted)};
        if (sender.send(control.sender, request,
                        std::chrono::steady_clock::now() + std::chrono::seconds(2))
                .status != local::FdHandoffStatus::Accepted)
            child_abort();
        const auto live_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        if (!owner_failure_case && !live_owner_failure_case) {
            while (runtime.live_session_count() == 0 &&
                   std::chrono::steady_clock::now() < live_deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (runtime.live_session_count() != 1)
                child_abort();
            if (!write_all(peer, hello_frame))
                child_abort();
            while (!completion_stalled.load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < live_deadline)
                std::this_thread::yield();
            if (!completion_stalled.load(std::memory_order_acquire))
                child_abort();
            send_operation_cancel(control.sender, request.identity, request.request_id);
        }
        worker.join();
        (void)result;
        (void)::close(peer);
        child_abort();
    }
    (void)::close(marker[1]);
    (void)::close(live_marker[1]);
    bool live_callback_seen = false;
    if (live_owner_failure_case) {
        struct pollfd live_marker_ready{live_marker[0], POLLIN | POLLHUP, 0};
        live_callback_seen = ::poll(&live_marker_ready, 1, 2000) > 0;
        uint8_t byte = 0;
        live_callback_seen = live_callback_seen &&
                             ::read(live_marker[0], &byte, sizeof(byte)) == 1 && byte == 1;
    }
    (void)::close(live_marker[0]);
    struct pollfd marker_ready{marker[0], POLLIN | POLLHUP, 0};
    CHECK(::poll(&marker_ready, 1, 2000) > 0);
    CHECK(std::chrono::steady_clock::now() - child_started <
          std::chrono::milliseconds(900));
    uint8_t byte = 0;
    CHECK(::read(marker[0], &byte, sizeof(byte)) == 1 && byte == 1);
    (void)::close(marker[0]);
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 125);
    if (live_owner_failure_case)
        CHECK(live_callback_seen);
}

void test_runtime_owner_failure_fail_stop_subprocess() {
    test_runtime_cancel_fail_stop_subprocess(true);
}

void test_runtime_live_owner_failure_fail_stop_subprocess() {
    test_runtime_cancel_fail_stop_subprocess(false, true);
}

void test_runtime_identity_disconnect_and_endpoint_failure() {
    const local::HandoffRequest expected{{7, 1}, 1};
    {
        service::SidecarRuntime runtime(test_runtime_config());
        RuntimeCase pair = authenticated_runtime_pair();
        local::FdHandoffSender sender{local::HandoffFd(::open("/dev/null", O_RDONLY))};
        local::FdHandoffResult sender_result;
        service::RuntimeResult runtime_result;
        std::thread worker([&] {
            runtime_result = runtime.run_one(
                pair.receiver, expected, std::chrono::steady_clock::now() + std::chrono::seconds(2));
        });
        sender_result = sender.send(pair.sender, local::HandoffRequest{{8, 1}, 1},
                                    std::chrono::steady_clock::now() + std::chrono::seconds(2));
        worker.join();
        CHECK(runtime_result.status == service::RuntimeStatus::HandoffRejected);
        CHECK(runtime_result.handoff.status == local::FdHandoffStatus::StaleGeneration);
        CHECK(sender_result.status == local::FdHandoffStatus::StaleGeneration);
        CHECK(runtime.live_handoff_count() == 0 && runtime.live_session_count() == 0);
    }
    {
        service::SidecarRuntime runtime(test_runtime_config());
        RuntimeCase pair = authenticated_runtime_pair();
        service::RuntimeResult runtime_result;
        std::thread worker([&] {
            runtime_result = runtime.run_one(
                pair.receiver, expected, std::chrono::steady_clock::now() + std::chrono::seconds(2));
        });
        pair.sender = local::Connection(-1);
        worker.join();
        CHECK(runtime_result.status == service::RuntimeStatus::HandoffRejected);
        CHECK(runtime_result.handoff.status == local::FdHandoffStatus::Disconnected);
        CHECK(runtime.live_handoff_count() == 0 && runtime.live_session_count() == 0);
    }
    {
        service::SidecarRuntime runtime(test_runtime_config());
        RuntimeCase pair = authenticated_runtime_pair();
        local::FdHandoffSender sender{local::HandoffFd(::open("/dev/null", O_RDONLY))};
        local::HandoffRequest sent = expected;
        service::RuntimeResult runtime_result;
        std::thread worker([&] {
            runtime_result = runtime.run_one(
                pair.receiver, expected, std::chrono::steady_clock::now() + std::chrono::seconds(2));
        });
        const local::FdHandoffResult sender_result = sender.send(
            pair.sender, sent, std::chrono::steady_clock::now() + std::chrono::seconds(2));
        worker.join();
        // /dev/null is deliberately not a connected TCP socket.  The helper
        // must ACK ownership first, then close it when endpoint adoption fails.
        CHECK(sender_result.status == local::FdHandoffStatus::Accepted);
        CHECK(runtime_result.handoff.status == local::FdHandoffStatus::Accepted);
        CHECK(runtime_result.status == service::RuntimeStatus::AdoptionFailed);
        CHECK(runtime.live_handoff_count() == 0 && runtime.live_session_count() == 0);
    }
    {
        service::SidecarRuntime runtime(test_runtime_config());
        RuntimeCase pair = authenticated_runtime_pair();
        runtime.stop();
        service::RuntimeResult result = runtime.run_one(
            pair.receiver, expected, std::chrono::steady_clock::now() + std::chrono::seconds(1));
        CHECK(result.status == service::RuntimeStatus::Stopped);
        CHECK(runtime.live_handoff_count() == 0 && runtime.live_session_count() == 0);
    }
}

int loopback_listener(uint16_t& port,
                      uint32_t bind_ipv4_host_order) {
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0);
    int reuse = 1;
    CHECK(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(bind_ipv4_host_order);
    address.sin_port = 0;
    CHECK(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(::listen(listener, 16) == 0);
    socklen_t length = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    port = ntohs(address.sin_port);
    return listener;
}

struct SourceArmServerObservation {
    bool accepted = false;
    bool protocol_50 = false;
    bool arm_received = false;
    bool armed_sent = false;
    bool cache_session_received = false;
    bool ready_sent = false;
    bool eof_without_cachewire = false;
    bool transfer_completed = false;
    bool arm_barrier_passed = false;
    bool commit_barrier_passed = false;
    bool commit_identity_matches_input = false;
    CStoreGuid committed_c_store_guid{};
    uint64_t committed_tu_seq = UINT64_MAX;
    uint64_t committed_rel_seq = UINT64_MAX;
    HistoryNonce committed_history_nonce{};
    ProfileId committed_profile = ProfileId::P29V1;
    std::vector<uint8_t> committed_input;
};

class SourceTransferBarrier {
public:
    explicit SourceTransferBarrier(size_t participants) : participants_(participants) {}

    bool arrive_and_wait(std::chrono::milliseconds timeout =
                             std::chrono::milliseconds(1200)) {
        std::unique_lock lock(mutex_);
        ++arrived_;
        changed_.notify_all();
        return changed_.wait_for(lock, timeout,
                                 [&] { return arrived_ >= participants_; });
    }

    [[nodiscard]] size_t arrived() const {
        std::lock_guard lock(mutex_);
        return arrived_;
    }

private:
    const size_t participants_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    size_t arrived_ = 0;
};

class SourceCommitHold {
public:
    void enter_and_wait() noexcept {
        {
            std::lock_guard lock(mutex_);
            entered_ = true;
        }
        changed_.notify_all();
        std::unique_lock lock(mutex_);
        (void)changed_.wait_for(lock, std::chrono::seconds(5),
                                [&] { return released_; });
    }

    bool wait_until_entered(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return entered_; });
    }

    void release() noexcept {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
};

class CodecWorkerGate {
public:
    void enter_and_wait() noexcept {
        std::unique_lock lock(mutex_);
        ++arrived_;
        changed_.notify_all();
        changed_.wait(lock, [&] { return released_; });
    }

    bool wait_for_arrivals(size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout,
                                 [&] { return arrived_ >= count; });
    }

    void release() noexcept {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    size_t arrived_ = 0;
    bool released_ = false;
};

void serve_stalled_source_arm(int listener,
                              std::atomic<bool>& arm_received,
                              std::shared_future<void> release) noexcept {
    try {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int accepted = ::accept(
            listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        (void)::close(listener);
        if (accepted < 0)
            return;
        std::unique_ptr<MsgChannel> channel(Service::createChannel(
            accepted, reinterpret_cast<sockaddr*>(&peer), peer_size));
        if (!channel)
            return;
        std::unique_ptr<Msg> arm_message(channel->get_msg(3, true));
        const auto* arm = arm_message != nullptr
                              ? dynamic_cast<P50SourceArmMsg*>(arm_message.get())
                              : nullptr;
        if (arm == nullptr || !arm->valid_payload())
            return;
        arm_received.store(true, std::memory_order_release);
        (void)release.wait_for(std::chrono::seconds(5));
    } catch (...) {
        (void)::close(listener);
    }
}

void serve_one_source_arm(int listener, FStoreGuid f_store_guid,
                          uint64_t f_store_generation,
                          SourceArmServerObservation& observation) noexcept {
    try {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int accepted = ::accept(
            listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        (void)::close(listener);
        if (accepted < 0)
            return;
        observation.accepted = true;
        std::unique_ptr<MsgChannel> channel(Service::createChannel(
            accepted, reinterpret_cast<sockaddr*>(&peer), peer_size));
        if (!channel)
            return;
        observation.protocol_50 = channel->protocol == PROTOCOL_VERSION;
        std::unique_ptr<Msg> arm_message(channel->get_msg(3, true));
        const auto* arm = arm_message != nullptr
                              ? dynamic_cast<P50SourceArmMsg*>(arm_message.get())
                              : nullptr;
        if (arm == nullptr || !arm->valid_payload())
            return;
        observation.arm_received = true;
        ClaimAttemptCapability128 capability_1;
        ClaimAttemptCapability128 capability_2;
        capability_1.bytes.fill(0xc1);
        capability_2.bytes.fill(0xc2);
        const P50SourceArmedMsg acknowledgement(
            arm->arm, 91, 92, f_store_generation, f_store_guid.bytes,
            kStoreIdentityDerivationVersion, 93, 2500,
            capability_1, capability_2);
        if (!channel->send_msg(acknowledgement))
            return;
        observation.armed_sent = true;
        std::unique_ptr<Msg> cache_message(channel->get_msg(3, true));
        if (cache_message == nullptr || *cache_message != Msg::CACHE_SESSION)
            return;
        observation.cache_session_received = true;
        const int raw_fd = channel->release_fd_if_input_empty();
        if (raw_fd < 0)
            return;
        observation.ready_sent = send_cache_session_ready(
            raw_fd, std::chrono::steady_clock::now() + std::chrono::seconds(2));
        if (observation.ready_sent) {
            pollfd descriptor{raw_fd, POLLIN | POLLHUP | POLLERR, 0};
            int ready = -1;
            do {
                ready = ::poll(&descriptor, 1, 2000);
            } while (ready < 0 && errno == EINTR);
            uint8_t byte = 0;
            const ssize_t count = ready > 0 ? ::recv(raw_fd, &byte, 1, 0) : -1;
            observation.eof_without_cachewire = count == 0;
        }
        (void)::close(raw_fd);
    } catch (...) {
        (void)::close(listener);
    }
}

void serve_accepted_source_transfer(
    int accepted, sockaddr_in peer, socklen_t peer_size,
    FStoreGuid f_store_guid, uint64_t f_store_generation,
    SourceArmServerObservation& observation,
    bool disconnect_after_ready = false,
    SourceTransferBarrier* arm_barrier = nullptr,
    SourceTransferBarrier* commit_barrier = nullptr,
    SourceCommitHold* commit_hold = nullptr) noexcept {
    try {
        if (accepted < 0)
            return;
        observation.accepted = true;
        std::unique_ptr<MsgChannel> channel(Service::createChannel(
            accepted, reinterpret_cast<sockaddr*>(&peer), peer_size));
        if (!channel)
            return;
        observation.protocol_50 = channel->protocol == PROTOCOL_VERSION;
        std::unique_ptr<Msg> arm_message(channel->get_msg(3, true));
        const auto* arm = arm_message != nullptr
                              ? dynamic_cast<P50SourceArmMsg*>(arm_message.get())
                              : nullptr;
        if (arm == nullptr || !arm->valid_payload())
            return;
        observation.arm_received = true;
        if (arm_barrier != nullptr) {
            observation.arm_barrier_passed = arm_barrier->arrive_and_wait();
            if (!observation.arm_barrier_passed)
                return;
        }
        ClaimAttemptCapability128 capability_1;
        ClaimAttemptCapability128 capability_2;
        capability_1.bytes.fill(0xd1);
        capability_2.bytes.fill(0xd2);
        const P50SourceArmedMsg acknowledgement(
            arm->arm, 101, 102, f_store_generation, f_store_guid.bytes,
            kStoreIdentityDerivationVersion, 103, 2500,
            capability_1, capability_2);
        if (!channel->send_msg(acknowledgement))
            return;
        observation.armed_sent = true;
        std::unique_ptr<Msg> cache_message(channel->get_msg(3, true));
        if (cache_message == nullptr || *cache_message != Msg::CACHE_SESSION)
            return;
        observation.cache_session_received = true;
        const int raw_fd = channel->release_fd_if_input_empty();
        if (raw_fd < 0)
            return;
        observation.ready_sent = send_cache_session_ready(
            raw_fd, std::chrono::steady_clock::now() + std::chrono::seconds(2));
        if (!observation.ready_sent) {
            (void)::close(raw_fd);
            return;
        }

        if (disconnect_after_ready) {
            (void)::close(raw_fd);
            return;
        }
        namespace asio = boost::asio;
        asio::io_context context;
        boost::system::error_code error;
        auto socket = P50ServerEndpoint::adopt_connected_fd(
            context.get_executor(), raw_fd, error);
        if (!socket.has_value())
            return;
        P50ServerEndpointConfig server_config;
        server_config.input_job_state = [&, commit_barrier, commit_hold](
            CStoreGuid c_store_guid, const TxBegin& begin,
            const TxCommit& commit, std::span<const uint8_t> input) {
            observation.committed_c_store_guid = c_store_guid;
            observation.committed_tu_seq = begin.tu_seq.value;
            observation.committed_rel_seq = begin.rel_seq.value;
            observation.committed_history_nonce = begin.history_nonce;
            observation.committed_profile = begin.profile;
            observation.committed_input.assign(input.begin(), input.end());
            observation.commit_identity_matches_input =
                commit.tu_seq == begin.tu_seq &&
                begin.raw_bytes == input.size() &&
                begin.raw_digest == icecc::digest128(input) &&
                commit.raw_digest == begin.raw_digest;
            if (commit_hold != nullptr)
                commit_hold->enter_and_wait();
            if (commit_barrier != nullptr) {
                observation.commit_barrier_passed =
                    commit_barrier->arrive_and_wait();
                if (!observation.commit_barrier_passed)
                    return InputJobState::Closed;
            }
            return InputJobState::Open;
        };
        P50ServerEndpoint endpoint(f_store_guid, {}, nullptr, nullptr,
                                   std::move(server_config));
        auto result = asio::co_spawn(
            context, endpoint.run_adopted(std::move(*socket)), asio::use_future);
        context.run();
        const ServerRunResult value = result.get();
        observation.transfer_completed =
            value.status == ServerRunStatus::Completed &&
            value.committed_input.has_value();
    } catch (...) {}
}

void serve_one_source_transfer(int listener, FStoreGuid f_store_guid,
                               uint64_t f_store_generation,
                               SourceArmServerObservation& observation,
                               SourceTransferBarrier* arm_barrier = nullptr,
                               SourceTransferBarrier* commit_barrier = nullptr) noexcept {
    sockaddr_in peer{};
    socklen_t peer_size = sizeof(peer);
    const int accepted = ::accept(
        listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
    (void)::close(listener);
    serve_accepted_source_transfer(accepted, peer, peer_size, f_store_guid,
                                   f_store_generation, observation, false,
                                   arm_barrier, commit_barrier);
}

template <size_t N>
void serve_source_transfers_on_persistent_f(
    int listener, FStoreGuid f_store_guid, uint64_t f_store_generation,
    CStoreGuid expected_c_guid,
    std::array<SourceArmServerObservation, N>& observations,
    SourceTransferBarrier* first_arm_barrier = nullptr,
    SourceTransferBarrier* first_commit_barrier = nullptr,
    SourceCommitHold* first_commit_hold = nullptr) noexcept {
    namespace asio = boost::asio;
    try {
        asio::io_context context;
        size_t current_index = 0;
        SourceArmServerObservation* current_observation = nullptr;
        SourceTransferBarrier* current_commit_barrier = nullptr;
        SourceCommitHold* current_commit_hold = nullptr;
        P50ServerEndpointConfig server_config;
        server_config.input_job_state = [&](CStoreGuid c_guid,
                                             const TxBegin& begin,
                                             const TxCommit& commit,
                                             std::span<const uint8_t> input) {
            if (current_observation == nullptr || c_guid != expected_c_guid)
                return InputJobState::Closed;
            auto& observation = *current_observation;
            observation.committed_c_store_guid = c_guid;
            observation.committed_tu_seq = begin.tu_seq.value;
            observation.committed_rel_seq = begin.rel_seq.value;
            observation.committed_history_nonce = begin.history_nonce;
            observation.committed_profile = begin.profile;
            observation.committed_input.assign(input.begin(), input.end());
            observation.commit_identity_matches_input =
                commit.tu_seq == begin.tu_seq &&
                begin.raw_bytes == input.size() &&
                begin.raw_digest == icecc::digest128(input) &&
                commit.raw_digest == begin.raw_digest;
            if (current_commit_hold != nullptr)
                current_commit_hold->enter_and_wait();
            if (current_commit_barrier != nullptr) {
                observation.commit_barrier_passed =
                    current_commit_barrier->arrive_and_wait();
                if (!observation.commit_barrier_passed)
                    return InputJobState::Closed;
            }
            return InputJobState::Open;
        };
        P50ServerEndpoint endpoint(f_store_guid, {}, nullptr, nullptr,
                                   std::move(server_config));
        for (current_index = 0; current_index < observations.size();
             ++current_index) {
            auto& observation = observations[current_index];
            current_observation = &observation;
            current_commit_barrier = current_index == 0
                                         ? first_commit_barrier
                                         : nullptr;
            current_commit_hold = current_index == 0
                                      ? first_commit_hold
                                      : nullptr;
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const int accepted = ::accept(
                listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (accepted < 0)
                break;
            observation.accepted = true;
            std::unique_ptr<MsgChannel> channel(Service::createChannel(
                accepted, reinterpret_cast<sockaddr*>(&peer), peer_size));
            if (!channel)
                break;
            observation.protocol_50 = channel->protocol == PROTOCOL_VERSION;
            std::unique_ptr<Msg> arm_message(channel->get_msg(3, true));
            const auto* arm = arm_message != nullptr
                                  ? dynamic_cast<P50SourceArmMsg*>(arm_message.get())
                                  : nullptr;
            if (arm == nullptr || !arm->valid_payload())
                break;
            observation.arm_received = true;
            if (current_index == 0 && first_arm_barrier != nullptr) {
                observation.arm_barrier_passed =
                    first_arm_barrier->arrive_and_wait();
                if (!observation.arm_barrier_passed)
                    break;
            }
            ClaimAttemptCapability128 capability_1;
            ClaimAttemptCapability128 capability_2;
            capability_1.bytes.fill(static_cast<uint8_t>(0xd1 + current_index));
            capability_2.bytes.fill(static_cast<uint8_t>(0xe1 + current_index));
            const P50SourceArmedMsg acknowledgement(
                arm->arm, 101, 102, f_store_generation, f_store_guid.bytes,
                kStoreIdentityDerivationVersion, 103, 2500,
                capability_1, capability_2);
            if (!channel->send_msg(acknowledgement))
                break;
            observation.armed_sent = true;
            std::unique_ptr<Msg> cache_message(channel->get_msg(3, true));
            if (cache_message == nullptr || *cache_message != Msg::CACHE_SESSION)
                break;
            observation.cache_session_received = true;
            const int raw_fd = channel->release_fd_if_input_empty();
            if (raw_fd < 0)
                break;
            observation.ready_sent = send_cache_session_ready(
                raw_fd, std::chrono::steady_clock::now() + std::chrono::seconds(2));
            if (!observation.ready_sent) {
                (void)::close(raw_fd);
                break;
            }
            boost::system::error_code error;
            auto socket = P50ServerEndpoint::adopt_connected_fd(
                context.get_executor(), raw_fd, error);
            if (!socket)
                break;
            auto result = asio::co_spawn(
                context, endpoint.run_adopted(std::move(*socket)),
                asio::use_future);
            context.run();
            context.restart();
            const ServerRunResult server_result = result.get();
            observation.transfer_completed =
                server_result.status == ServerRunStatus::Completed &&
                server_result.committed_input.has_value();
        }
        (void)::close(listener);
    } catch (...) {
        (void)::close(listener);
    }
}

void serve_two_held_source_transfers(
    int listener, FStoreGuid f_store_guid, uint64_t f_store_generation,
    std::array<SourceArmServerObservation, 2>& observations,
    SourceCommitHold& hold, uint64_t second_generation = 0) noexcept {
    for (size_t index = 0; index != observations.size(); ++index) {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int accepted = ::accept(
            listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (accepted < 0)
            break;
        serve_accepted_source_transfer(
            accepted, peer, peer_size, f_store_guid,
            index == 1 && second_generation != 0
                ? second_generation : f_store_generation,
            observations[index], false, nullptr, nullptr,
            index == 0 ? &hold : nullptr);
    }
    (void)::close(listener);
}

void serve_many_source_transfers_on_shared_f_endpoint(
    int listener, FStoreGuid f_store_guid, uint64_t f_store_generation,
    std::vector<SourceArmServerObservation>& observations,
    const std::vector<CStoreGuid>& expected_c_guids,
    CodecWorkerGate& codec_gate,
    std::atomic<size_t>& observed_live_sessions,
    std::atomic<size_t>& observed_namespaces,
    std::atomic<bool>& stop_sampling) noexcept {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;
    try {
        asio::io_context context;
        std::map<CStoreGuid, SourceArmServerObservation*> observation_by_c;
        for (size_t i = 0; i < expected_c_guids.size(); ++i)
            observation_by_c.emplace(expected_c_guids[i], &observations[i]);
        P50ServerEndpointConfig server_config;
        server_config.input_job_state = [&observation_by_c](
            CStoreGuid c_guid, const TxBegin& begin, const TxCommit& commit,
            std::span<const uint8_t> input) {
            const auto found = observation_by_c.find(c_guid);
            if (found == observation_by_c.end())
                return InputJobState::Closed;
            auto& observation = *found->second;
            observation.committed_c_store_guid = c_guid;
            observation.committed_tu_seq = begin.tu_seq.value;
            observation.committed_profile = begin.profile;
            observation.committed_input.assign(input.begin(), input.end());
            observation.commit_identity_matches_input =
                commit.tu_seq == begin.tu_seq &&
                begin.raw_bytes == input.size() &&
                begin.raw_digest == icecc::digest128(input) &&
                commit.raw_digest == begin.raw_digest;
            return InputJobState::Open;
        };
        P50ServerEndpoint endpoint(f_store_guid, {}, nullptr, nullptr,
                                   std::move(server_config));

        std::vector<tcp::socket> sockets;
        sockets.reserve(observations.size());
        for (size_t i = 0; i < observations.size(); ++i) {
            sockaddr_in peer{};
            socklen_t peer_size = sizeof(peer);
            const int accepted = ::accept(
                listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (accepted < 0) {
                (void)::close(listener);
                return;
            }
            auto& observation = observations[i];
            observation.accepted = true;
            std::unique_ptr<MsgChannel> channel(Service::createChannel(
                accepted, reinterpret_cast<sockaddr*>(&peer), peer_size));
            if (!channel)
                return;
            observation.protocol_50 = channel->protocol == PROTOCOL_VERSION;
            std::unique_ptr<Msg> arm_message(channel->get_msg(3, true));
            const auto* arm = arm_message != nullptr
                                  ? dynamic_cast<P50SourceArmMsg*>(arm_message.get())
                                  : nullptr;
            if (arm == nullptr || !arm->valid_payload())
                return;
            observation.arm_received = true;
            ClaimAttemptCapability128 capability_1;
            ClaimAttemptCapability128 capability_2;
            capability_1.bytes.fill(static_cast<uint8_t>(0xd1 + i));
            capability_2.bytes.fill(static_cast<uint8_t>(0xe1 + i));
            const P50SourceArmedMsg acknowledgement(
                arm->arm, 101, 102, f_store_generation, f_store_guid.bytes,
                kStoreIdentityDerivationVersion, 103, 2500,
                capability_1, capability_2);
            if (!channel->send_msg(acknowledgement))
                return;
            observation.armed_sent = true;
            std::unique_ptr<Msg> cache_message(channel->get_msg(3, true));
            if (cache_message == nullptr || *cache_message != Msg::CACHE_SESSION)
                return;
            observation.cache_session_received = true;
            const int raw_fd = channel->release_fd_if_input_empty();
            if (raw_fd < 0)
                return;
            observation.ready_sent = send_cache_session_ready(
                raw_fd, std::chrono::steady_clock::now() + std::chrono::seconds(2));
            if (!observation.ready_sent) {
                (void)::close(raw_fd);
                return;
            }
            boost::system::error_code error;
            auto socket = P50ServerEndpoint::adopt_connected_fd(
                context.get_executor(), raw_fd, error);
            if (!socket)
                return;
            sockets.push_back(std::move(*socket));
        }
        (void)::close(listener);

        std::vector<std::future<ServerRunResult>> results;
        results.reserve(sockets.size());
        for (auto& socket : sockets) {
            EndpointIoControl control;
            control.before_materialize_on_worker = [&codec_gate] {
                codec_gate.enter_and_wait();
            };
            results.push_back(asio::co_spawn(
                context, endpoint.run_adopted(std::move(socket),
                                               std::move(control)),
                asio::use_future));
        }
        auto sampler = std::make_shared<asio::steady_timer>(context);
        auto sample_once = std::make_shared<std::function<void()>>();
        *sample_once = [&, sampler, sample_once] {
            observed_live_sessions.store(endpoint.live_session_count(),
                                         std::memory_order_release);
            observed_namespaces.store(endpoint.namespace_count(),
                                      std::memory_order_release);
            if (stop_sampling.load(std::memory_order_acquire))
                return;
            sampler->expires_after(std::chrono::milliseconds(1));
            sampler->async_wait([sample_once](const boost::system::error_code& ec) {
                if (!ec)
                    (*sample_once)();
            });
        };
        (*sample_once)();
        context.run();
        for (size_t i = 0; i < results.size(); ++i) {
            const ServerRunResult result = results[i].get();
            if (result.completed_input.has_value()) {
                const auto found = observation_by_c.find(
                    result.completed_input->c_store_guid);
                if (found != observation_by_c.end())
                    found->second->transfer_completed =
                        result.status == ServerRunStatus::Completed &&
                        result.committed_input.has_value();
            }
        }
        *sample_once = {};
    } catch (...) {
        codec_gate.release();
        stop_sampling.store(true, std::memory_order_release);
        (void)::close(listener);
    }
}

struct SourceConnectRetryObservation {
    unsigned int accepted_connections = 0;
    std::vector<uint8_t> first_connection_bytes;
    SourceArmServerObservation first_attempt;
    SourceArmServerObservation successful;
};

std::vector<uint8_t> receive_after_protocol_slice(int fd) noexcept {
    std::vector<uint8_t> result;
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    for (;;) {
        std::array<uint8_t, 64> bytes{};
        const ssize_t count = ::recv(fd, bytes.data(), bytes.size(),
                                     MSG_DONTWAIT);
        if (count > 0) {
            result.insert(result.end(), bytes.begin(), bytes.begin() + count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        break;
    }
    return result;
}

void serve_source_transfer_after_protocol_stall(
    int listener, FStoreGuid f_store_guid, uint64_t f_store_generation,
    SourceConnectRetryObservation& observation) noexcept {
    try {
        sockaddr_in first_peer{};
        socklen_t first_peer_size = sizeof(first_peer);
        const int stalled = ::accept(
            listener, reinterpret_cast<sockaddr*>(&first_peer),
            &first_peer_size);
        if (stalled < 0) {
            (void)::close(listener);
            return;
        }
        ++observation.accepted_connections;

        // Hold protocol negotiation past the one-second connection slice.
        // The client may send only its fixed four-byte protocol proposal;
        // P50SourceArmMsg is composed only after a channel is returned.
        observation.first_connection_bytes =
            receive_after_protocol_slice(stalled);
        (void)::close(stalled);

        pollfd descriptor{listener, POLLIN, 0};
        int ready = -1;
        do {
            ready = ::poll(&descriptor, 1, 2000);
        } while (ready < 0 && errno == EINTR);
        sockaddr_in retry_peer{};
        socklen_t retry_peer_size = sizeof(retry_peer);
        const int accepted = ready > 0
                                 ? ::accept(
                                       listener,
                                       reinterpret_cast<sockaddr*>(&retry_peer),
                                       &retry_peer_size)
                                 : -1;
        (void)::close(listener);
        if (accepted >= 0)
            ++observation.accepted_connections;
        serve_accepted_source_transfer(
            accepted, retry_peer, retry_peer_size, f_store_guid,
            f_store_generation, observation.successful);
    } catch (...) {
        (void)::close(listener);
    }
}

void serve_source_transfer_with_held_retry(
    int listener, FStoreGuid f_store_guid, uint64_t f_store_generation,
    SourceConnectRetryObservation& observation,
    std::promise<void>& retry_accepted,
    std::shared_future<void> release_retry) noexcept {
    try {
        sockaddr_in first_peer{};
        socklen_t first_peer_size = sizeof(first_peer);
        const int first = ::accept(
            listener, reinterpret_cast<sockaddr*>(&first_peer),
            &first_peer_size);
        if (first < 0) {
            (void)::close(listener);
            return;
        }
        ++observation.accepted_connections;
        // Complete SOURCE_ARMED and the cache-session ready handoff, then
        // disconnect before CacheWire.  The sender's explicit retry factory
        // must therefore run on the offloaded path exercised by this test.
        serve_accepted_source_transfer(
            first, first_peer, first_peer_size, f_store_guid,
            f_store_generation, observation.first_attempt,
            /*disconnect_after_ready=*/true);

        pollfd descriptor{listener, POLLIN, 0};
        int ready = -1;
        do {
            ready = ::poll(&descriptor, 1, 2500);
        } while (ready < 0 && errno == EINTR);
        sockaddr_in retry_peer{};
        socklen_t retry_peer_size = sizeof(retry_peer);
        const int accepted = ready > 0
                                 ? ::accept(listener,
                                            reinterpret_cast<sockaddr*>(&retry_peer),
                                            &retry_peer_size)
                                 : -1;
        if (accepted < 0) {
            (void)::close(listener);
            return;
        }
        ++observation.accepted_connections;
        retry_accepted.set_value();
        (void)release_retry.wait_for(std::chrono::seconds(4));
        serve_accepted_source_transfer(
            accepted, retry_peer, retry_peer_size, f_store_guid,
            f_store_generation, observation.successful);
        (void)::close(listener);
    } catch (...) {
        (void)::close(listener);
    }
}

void serve_only_protocol_stalls(
    int listener, std::vector<std::vector<uint8_t>>& attempts) noexcept {
    try {
        // Keep both sockets stalled until the client closes them: the first
        // at its short slice, the second at the unchanged outer deadline.
        // Closing on a fixture timer would test definitive refusal instead.
        for (unsigned int index = 0; index != 2; ++index) {
            pollfd descriptor{listener, POLLIN, 0};
            int ready = -1;
            do {
                ready = ::poll(&descriptor, 1, 2000);
            } while (ready < 0 && errno == EINTR);
            const int accepted = ready > 0 ? ::accept(listener, nullptr, nullptr)
                                           : -1;
            if (accepted < 0)
                break;
            std::vector<uint8_t> bytes;
            const auto limit = std::chrono::steady_clock::now() +
                               std::chrono::seconds(4);
            while (std::chrono::steady_clock::now() < limit) {
                pollfd peer{accepted, POLLIN, 0};
                const int readable = ::poll(&peer, 1, 50);
                if (readable < 0 && errno == EINTR)
                    continue;
                if (readable < 0)
                    break;
                if (readable == 0)
                    continue;
                std::array<uint8_t, 64> chunk{};
                const ssize_t count = ::recv(accepted, chunk.data(),
                                             chunk.size(), MSG_DONTWAIT);
                if (count > 0)
                    bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
                else if (count == 0 || (errno != EINTR && errno != EAGAIN &&
                                        errno != EWOULDBLOCK))
                    break;
            }
            attempts.push_back(std::move(bytes));
            (void)::close(accepted);
        }
    } catch (...) {}
    (void)::close(listener);
}

local::P50SourceTransferRequest source_transfer_request(
    uint16_t port, uint64_t identity, uint32_t profile) {
    local::P50SourceTransferRequest request;
    request.wire_job_id = static_cast<uint32_t>(identity);
    request.assignment_epoch = identity + 1;
    request.assignment_nonce = identity + 2;
    request.selected_f_host = "127.0.0.1";
    request.selected_f_ordinary_port = port;
    request.selected_f_cache_port = port;
    request.cache_protocol = CACHE_WIRE_REVISION;
    request.cache_profile = profile;
    request.logical_job = identity + 3;
    request.compiler_attempt = identity + 4;
    request.source_request_id = identity + 5;
    if (profile == CACHE_PROFILE_P29V1)
        request.source_mode = P50_SOURCE_MODE_P29V1;
    else if (profile == CACHE_PROFILE_ZSTD_ROUTE)
        request.source_mode = P50_SOURCE_MODE_ZSTD_ROUTE;
    else
        request.source_mode = P50_SOURCE_MODE_ZSTD_TU;
    CHECK(request.valid());
    return request;
}

int source_file(std::string_view stem, std::span<const uint8_t> bytes) {
    std::array<char, 128> path{};
    const char* temporary_directory = std::getenv("TMPDIR");
    if (temporary_directory == nullptr || temporary_directory[0] == '\0')
        temporary_directory = "/tmp";
    const int length = std::snprintf(
        path.data(), path.size(), "%s/%.*s-XXXXXX", temporary_directory,
        static_cast<int>(stem.size()), stem.data());
    CHECK(length > 0 && static_cast<size_t>(length) < path.size());
    const int fd = ::mkstemp(path.data());
    CHECK(fd >= 0);
    CHECK(write_all(fd, bytes));
    CHECK(::unlink(path.data()) == 0);
    return fd;
}

void test_source_connect_protocol_slice_retries_before_arm() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x63;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.source_open_arm_timeout = std::chrono::milliseconds(3000);
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot remote_root{};
    remote_root.bytes[14] = 0x64;
    const FStoreGuid remote_guid = f_store_guid_for_root(remote_root);
    uint16_t port = 0;
    const int listener = loopback_listener(port);
    SourceConnectRetryObservation observation;
    std::thread server([&] {
        serve_source_transfer_after_protocol_stall(
            listener, remote_guid, 19, observation);
    });

    const std::array<uint8_t, 13> source{
        'r', 'e', 't', 'r', 'y', '-', 's', 'o', 'u', 'r', 'c', 'e', '\n'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            clock.clock_domain_id, clock.time_namespace_id);
    const auto started = std::chrono::steady_clock::now();
    const local::P50SourceTransferResult result =
        runtime.transfer_source_on_owner(
            source_transfer_request(port, 141, CACHE_PROFILE_ZSTD_TU),
            deadline,
            local::HandoffFd(source_file("p50-connect-retry", source)));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    server.join();

    const std::vector<uint8_t> protocol_only{
        static_cast<uint8_t>(PROTOCOL_VERSION), 0, 0, 0};
    CHECK(observation.accepted_connections == 2);
    CHECK(observation.first_connection_bytes == protocol_only);
    CHECK(result.code == local::SourceTransferResultCode::Committed);
    CHECK(result.attempts == 1);
    CHECK(result.raw_bytes == source.size());
    CHECK(result.raw_digest == icecc::digest128(source));
    CHECK(observation.successful.accepted &&
          observation.successful.protocol_50 &&
          observation.successful.arm_received &&
          observation.successful.armed_sent &&
          observation.successful.cache_session_received &&
          observation.successful.ready_sent &&
          observation.successful.transfer_completed);
    CHECK(elapsed >= std::chrono::seconds(1) &&
          elapsed < std::chrono::milliseconds(2800));
}

void test_source_connect_protocol_slices_share_one_outer_budget() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x65;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.source_open_arm_timeout = std::chrono::milliseconds(2500);
    service::SidecarRuntime runtime(std::move(config));

    uint16_t port = 0;
    const int listener = loopback_listener(port);
    std::vector<std::vector<uint8_t>> attempts;
    std::thread server([&] { serve_only_protocol_stalls(listener, attempts); });
    const std::array<uint8_t, 12> source{
        'a', 'l', 'l', '-', 's', 't', 'a', 'l', 'l', 'e', 'd', '\n'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            clock.clock_domain_id, clock.time_namespace_id);
    const auto started = std::chrono::steady_clock::now();
    const local::P50SourceTransferResult result =
        runtime.transfer_source_on_owner(
            source_transfer_request(port, 151, CACHE_PROFILE_ZSTD_TU),
            deadline,
            local::HandoffFd(source_file("p50-connect-stalls", source)));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    server.join();

    const std::vector<uint8_t> protocol_only{
        static_cast<uint8_t>(PROTOCOL_VERSION), 0, 0, 0};
    CHECK(result.code == local::SourceTransferResultCode::Error);
    CHECK(result.error_code == 4 && result.attempts == 0);
    CHECK(attempts.size() == 2);
    CHECK(std::all_of(attempts.begin(), attempts.end(),
                      [&](const auto& bytes) {
                          return bytes == protocol_only;
                      }));
    CHECK(elapsed >= std::chrono::milliseconds(2400) &&
          elapsed < std::chrono::milliseconds(3200));
}

void test_stalled_f_arm_is_bounded_before_healthy_transfer(
    uint32_t profile, size_t max_active_source_transfers = 4) {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x61;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.source_open_arm_timeout = std::chrono::seconds(4);
    config.max_active_source_transfers = max_active_source_transfers;
    service::SidecarRuntime runtime(std::move(config));

    uint16_t stalled_port = 0;
    const int stalled_listener = loopback_listener(stalled_port);
    std::atomic<bool> stalled_arm_received{false};
    std::promise<void> release_stalled_server;
    const std::shared_future<void> release_stalled_future =
        release_stalled_server.get_future().share();
    std::thread stalled_server([&] {
        serve_stalled_source_arm(stalled_listener, stalled_arm_received,
                                 release_stalled_future);
    });
    const std::array<uint8_t, 8> stalled_source{
        's', 't', 'a', 'l', 'l', 'e', 'd', '\n'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto stalled_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            clock.clock_domain_id, clock.time_namespace_id);
    std::promise<local::P50SourceTransferResult> stalled_completion;
    std::future<local::P50SourceTransferResult> stalled_future =
        stalled_completion.get_future();
    std::thread stalled_transfer([&] {
        stalled_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(stalled_port, 121, profile),
            stalled_deadline,
            local::HandoffFd(source_file("p50-stalled-arm", stalled_source))));
    });
    const auto arm_wait_limit =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!stalled_arm_received.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < arm_wait_limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool arm_received = stalled_arm_received.load(std::memory_order_acquire);

    StoreIdentityRoot healthy_root{};
    healthy_root.bytes[14] = 0x62;
    const FStoreGuid healthy_guid = f_store_guid_for_root(healthy_root);
    uint16_t healthy_port = 0;
    const int healthy_listener = loopback_listener(healthy_port);
    SourceArmServerObservation healthy_observation;
    std::thread healthy_server([&] {
        serve_one_source_transfer(healthy_listener, healthy_guid, 17,
                                  healthy_observation);
    });
    const std::array<uint8_t, 9> healthy_source{
        'h', 'e', 'a', 'l', 't', 'h', 'y', '!', '\n'};
    const auto healthy_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(2),
            clock.clock_domain_id, clock.time_namespace_id);
    std::promise<local::P50SourceTransferResult> healthy_completion;
    std::future<local::P50SourceTransferResult> healthy_future =
        healthy_completion.get_future();
    std::thread healthy_transfer([&] {
        healthy_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(healthy_port, 131, profile),
            healthy_deadline,
            local::HandoffFd(source_file("p50-healthy-arm", healthy_source))));
    });
    // F_A has received its arm but intentionally withholds its acknowledgement.
    // F_B must still reach a committed CacheWire input before A is released.
    // The deadline bounds this regression on the implementation with the old
    // process-wide transfer lock.
    const bool healthy_ready_before_release =
        healthy_future.wait_for(std::chrono::milliseconds(2200)) ==
        std::future_status::ready;
    std::optional<local::P50SourceTransferResult> early_healthy_result;
    if (healthy_ready_before_release)
        early_healthy_result = healthy_future.get();
    const bool healthy_finished_before_release =
        early_healthy_result.has_value() &&
        early_healthy_result->code ==
            local::SourceTransferResultCode::Committed;
    release_stalled_server.set_value();
    local::P50SourceTransferResult healthy_result =
        early_healthy_result.has_value()
            ? std::move(*early_healthy_result)
            : healthy_future.get();
    const local::P50SourceTransferResult stalled_result = stalled_future.get();
    {
        const int wake = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wake >= 0) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(healthy_port);
        (void)::connect(wake, reinterpret_cast<const sockaddr*>(&address),
                        sizeof(address));
        (void)::close(wake);
        }
    }
    healthy_transfer.join();
    stalled_transfer.join();
    stalled_server.join();
    healthy_server.join();
    CHECK(arm_received);
    CHECK(stalled_result.code == local::SourceTransferResultCode::Error);
    CHECK(stalled_result.error_code == 4 && stalled_result.attempts == 0);
    CHECK(healthy_finished_before_release);
    CHECK(healthy_result.code == local::SourceTransferResultCode::Committed);
    CHECK(healthy_result.attempts == 1);
    CHECK(healthy_result.raw_bytes == healthy_source.size());
    CHECK(healthy_result.raw_digest == icecc::digest128(healthy_source));
    CHECK(healthy_observation.accepted && healthy_observation.protocol_50 &&
          healthy_observation.arm_received && healthy_observation.armed_sent &&
          healthy_observation.cache_session_received &&
          healthy_observation.ready_sent &&
          healthy_observation.transfer_completed);
    CHECK(healthy_result.tu_seq == 0);
    CHECK(healthy_observation.committed_c_store_guid == launch.c_store_guid);
    CHECK(healthy_observation.committed_profile ==
          (profile == CACHE_PROFILE_P29V1
               ? ProfileId::P29V1
               : profile == CACHE_PROFILE_ZSTD_ROUTE
                     ? ProfileId::ZSTD_ROUTE
                     : ProfileId::ZSTD_TU));
    CHECK(healthy_observation.committed_input ==
          std::vector<uint8_t>(healthy_source.begin(), healthy_source.end()));
    CHECK(healthy_observation.commit_identity_matches_input);
    const char* profile_name = profile == CACHE_PROFILE_P29V1
                                   ? "P29V1"
                                   : profile == CACHE_PROFILE_ZSTD_ROUTE
                                         ? "ZSTD_ROUTE"
                                         : "ZSTD_TU";
    std::printf("A05 %s: healthy F_B committed while F_A SOURCE_ARMED "
                "was held\n", profile_name);
    std::fflush(stdout);
}

void test_held_retry_does_not_block_healthy_link(uint32_t profile) {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x69;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.source_open_arm_timeout = std::chrono::seconds(5);
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot retry_root{};
    retry_root.bytes[14] = 0x6a;
    const FStoreGuid retry_guid = f_store_guid_for_root(retry_root);
    uint16_t retry_port = 0;
    const int retry_listener = loopback_listener(retry_port);
    SourceConnectRetryObservation retry_observation;
    std::promise<void> retry_accepted_promise;
    std::future<void> retry_accepted = retry_accepted_promise.get_future();
    std::promise<void> release_retry_promise;
    const std::shared_future<void> release_retry =
        release_retry_promise.get_future().share();
    std::thread retry_server([&] {
        serve_source_transfer_with_held_retry(
            retry_listener, retry_guid, 29, retry_observation,
            retry_accepted_promise, release_retry);
    });

    const std::array<uint8_t, 11> retry_source{
        'r', 'e', 't', 'r', 'y', '-', 'h', 'e', 'l', 'd', '\n'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto retry_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(8),
            clock.clock_domain_id, clock.time_namespace_id);
    std::promise<local::P50SourceTransferResult> retry_completion;
    std::future<local::P50SourceTransferResult> retry_result =
        retry_completion.get_future();
    std::thread retry_transfer([&] {
        retry_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(retry_port, 161, profile),
            retry_deadline,
            local::HandoffFd(source_file("p50-held-retry", retry_source))));
    });
    const bool retry_is_held = retry_accepted.wait_for(std::chrono::seconds(3)) ==
                               std::future_status::ready;

    StoreIdentityRoot healthy_root{};
    healthy_root.bytes[14] = 0x6b;
    const FStoreGuid healthy_guid = f_store_guid_for_root(healthy_root);
    uint16_t healthy_port = 0;
    const int healthy_listener = loopback_listener(healthy_port);
    SourceArmServerObservation healthy_observation;
    std::thread healthy_server([&] {
        serve_one_source_transfer(healthy_listener, healthy_guid, 31,
                                  healthy_observation);
    });
    const std::array<uint8_t, 10> healthy_source{
        'h', 'e', 'a', 'l', 't', 'h', 'y', '-', 'b', '\n'};
    const auto healthy_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(3),
            clock.clock_domain_id, clock.time_namespace_id);
    std::promise<local::P50SourceTransferResult> healthy_completion;
    std::future<local::P50SourceTransferResult> healthy_result =
        healthy_completion.get_future();
    std::thread healthy_transfer([&] {
        healthy_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(healthy_port, 171, profile),
            healthy_deadline,
            local::HandoffFd(source_file("p50-healthy-during-retry", healthy_source))));
    });
    const bool healthy_finished_before_retry_release =
        healthy_result.wait_for(std::chrono::milliseconds(1800)) ==
        std::future_status::ready;

    release_retry_promise.set_value();
    const bool retry_finished = retry_result.wait_for(std::chrono::seconds(4)) ==
                                std::future_status::ready;
    const bool healthy_finished = healthy_result.wait_for(std::chrono::seconds(4)) ==
                                  std::future_status::ready;
    local::P50SourceTransferResult retry_value;
    local::P50SourceTransferResult healthy_value;
    if (retry_finished)
        retry_value = retry_result.get();
    if (healthy_finished)
        healthy_value = healthy_result.get();
    retry_transfer.join();
    healthy_transfer.join();
    if (!healthy_observation.accepted) {
        const int wake = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wake >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(healthy_port);
            (void)::connect(wake, reinterpret_cast<const sockaddr*>(&address),
                            sizeof(address));
            (void)::close(wake);
        }
    }
    retry_server.join();
    healthy_server.join();

    CHECK(retry_is_held);
    CHECK(healthy_finished_before_retry_release);
    CHECK(retry_finished && healthy_finished);
    CHECK(retry_observation.accepted_connections == 2);
    CHECK(retry_observation.first_attempt.accepted &&
          retry_observation.first_attempt.protocol_50 &&
          retry_observation.first_attempt.arm_received &&
          retry_observation.first_attempt.armed_sent &&
          retry_observation.first_attempt.cache_session_received &&
          retry_observation.first_attempt.ready_sent);
    CHECK(retry_value.code == local::SourceTransferResultCode::Committed);
    CHECK(retry_value.attempts == 2);
    CHECK(retry_value.tu_seq == 0);
    CHECK(retry_value.raw_bytes == retry_source.size() &&
          retry_value.raw_digest == icecc::digest128(retry_source));
    CHECK(retry_observation.successful.committed_input ==
          std::vector<uint8_t>(retry_source.begin(), retry_source.end()));
    const ProfileId expected_profile =
        profile == CACHE_PROFILE_P29V1
            ? ProfileId::P29V1
            : profile == CACHE_PROFILE_ZSTD_ROUTE ? ProfileId::ZSTD_ROUTE
                                                   : ProfileId::ZSTD_TU;
    CHECK(retry_observation.successful.committed_profile == expected_profile);
    CHECK(healthy_value.code == local::SourceTransferResultCode::Committed);
    CHECK(healthy_value.tu_seq == 1);
    CHECK(healthy_value.raw_bytes == healthy_source.size() &&
          healthy_value.raw_digest == icecc::digest128(healthy_source));
    CHECK(healthy_observation.committed_c_store_guid == launch.c_store_guid);
    CHECK(healthy_observation.committed_profile == expected_profile);
    CHECK(healthy_observation.committed_input ==
          std::vector<uint8_t>(healthy_source.begin(), healthy_source.end()));
    CHECK(healthy_observation.commit_identity_matches_input);
    const char* profile_name = profile == CACHE_PROFILE_P29V1
                                   ? "P29V1"
                                   : profile == CACHE_PROFILE_ZSTD_ROUTE
                                         ? "ZSTD_ROUTE"
                                         : "ZSTD_TU";
    std::printf("A06 %s: healthy F_B committed while F_A retry socket was held\n",
                profile_name);
    std::fflush(stdout);
}

void test_parallel_distinct_f_cachewire(size_t f_count, uint32_t profile) {
    CHECK(f_count >= 2 && f_count <= 4);
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = static_cast<uint8_t>(0x80 + f_count + profile);
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_active_source_transfers = 4;
    config.max_aggregate_source_raw_bytes = 4096;
    service::SidecarRuntime runtime(std::move(config));

    SourceTransferBarrier arm_barrier(f_count);
    SourceTransferBarrier commit_barrier(f_count);
    std::vector<uint16_t> ports(f_count);
    std::vector<int> listeners(f_count);
    std::vector<FStoreGuid> f_guids(f_count);
    std::vector<std::array<std::vector<uint8_t>, 3>> inputs(f_count);
    std::vector<std::array<SourceArmServerObservation, 3>> observations(f_count);
    std::vector<std::thread> servers;
    servers.reserve(f_count);
    for (size_t index = 0; index != f_count; ++index) {
        StoreIdentityRoot remote_root{};
        remote_root.bytes[14] = static_cast<uint8_t>(0x30 + index);
        remote_root.bytes[15] = static_cast<uint8_t>(0xa0 + f_count);
        f_guids[index] = f_store_guid_for_root(remote_root);
        listeners[index] = loopback_listener(ports[index]);
        inputs[index][0].resize(73 + index * 11);
        inputs[index][1] = inputs[index][0]; // warm repeat on the same F route
        inputs[index][2].resize(inputs[index][0].size() + 7);
        for (size_t byte = 0; byte != inputs[index][0].size(); ++byte)
            inputs[index][0][byte] = static_cast<uint8_t>(
                (byte * 37 + index * 53 + f_count * 7) & 0xff);
        inputs[index][1] = inputs[index][0];
        for (size_t byte = 0; byte != inputs[index][2].size(); ++byte)
            inputs[index][2][byte] = static_cast<uint8_t>(
                (byte * 41 + index * 59 + f_count * 11 + 3) & 0xff);
        servers.emplace_back([&, index] {
            serve_source_transfers_on_persistent_f(
                listeners[index], f_guids[index], 170 + index,
                launch.c_store_guid, observations[index], &arm_barrier,
                &commit_barrier);
        });
    }

    const auto clock = sidecar::process_monotonic_clock_identity();
    std::array<std::vector<local::P50SourceTransferResult>, 3> committed;
    for (size_t wave = 0; wave != 3; ++wave) {
        std::vector<std::promise<local::P50SourceTransferResult>> completions;
        std::vector<std::future<local::P50SourceTransferResult>> results;
        std::vector<std::thread> transfers;
        completions.reserve(f_count);
        results.reserve(f_count);
        transfers.reserve(f_count);
        const auto deadline =
            sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
                std::chrono::steady_clock::now() + std::chrono::seconds(7),
                clock.clock_domain_id, clock.time_namespace_id);
        for (size_t index = 0; index != f_count; ++index) {
            completions.emplace_back();
            results.push_back(completions.back().get_future());
            const uint16_t port = ports[index];
            const uint64_t request_id =
                400 + wave * 100 + index + f_count * 10 + profile;
            transfers.emplace_back([&, index, port, request_id, wave] {
                completions[index].set_value(runtime.transfer_source_on_owner(
                    source_transfer_request(port, request_id, profile), deadline,
                    local::HandoffFd(source_file("p50-distinct-f",
                                                 inputs[index][wave]))));
            });
        }
        committed[wave].reserve(f_count);
        for (auto& result : results)
            committed[wave].push_back(result.get());
        for (auto& transfer : transfers)
            transfer.join();
    }
    // A server whose client expired before connect is still blocked in accept;
    // a best-effort loopback connect wakes that fixture for bounded cleanup.
    for (uint16_t port : ports) {
        const int wake = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(wake >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        (void)::connect(wake, reinterpret_cast<const sockaddr*>(&address),
                        sizeof(address));
        (void)::close(wake);
    }
    for (auto& server : servers)
        server.join();

    std::vector<std::vector<uint64_t>> tu_sequences(3);
    for (size_t wave = 0; wave != 3; ++wave) {
        tu_sequences[wave].reserve(f_count);
        for (size_t index = 0; index != f_count; ++index) {
            const auto& result = committed[wave][index];
            const auto& observation = observations[index][wave];
            CHECK(result.code == local::SourceTransferResultCode::Committed);
            CHECK(result.valid() && result.attempts == 1);
            CHECK(result.c_store_guid == launch.c_store_guid);
            CHECK(result.raw_bytes == inputs[index][wave].size());
            CHECK(result.raw_digest == icecc::digest128(inputs[index][wave]));
            CHECK(observation.committed_c_store_guid == launch.c_store_guid);
            CHECK(observation.committed_tu_seq == result.tu_seq);
            CHECK(observation.committed_rel_seq == wave);
            CHECK(observation.committed_profile ==
                  (profile == CACHE_PROFILE_P29V1
                       ? ProfileId::P29V1
                       : profile == CACHE_PROFILE_ZSTD_ROUTE
                             ? ProfileId::ZSTD_ROUTE
                             : ProfileId::ZSTD_TU));
            CHECK(observation.committed_input == inputs[index][wave]);
            CHECK(observation.commit_identity_matches_input);
            CHECK(observation.transfer_completed);
            CHECK(observation.accepted && observation.protocol_50 &&
                  observation.arm_received && observation.armed_sent &&
                  observation.cache_session_received && observation.ready_sent);
            if (wave == 0) {
                CHECK(observation.arm_barrier_passed &&
                      observation.commit_barrier_passed);
            }
            if (wave != 0)
                CHECK(observation.committed_history_nonce ==
                      observations[index][0].committed_history_nonce);
            tu_sequences[wave].push_back(result.tu_seq);
        }
        std::sort(tu_sequences[wave].begin(), tu_sequences[wave].end());
        CHECK(std::adjacent_find(tu_sequences[wave].begin(),
                                 tu_sequences[wave].end()) ==
              tu_sequences[wave].end());
        const uint64_t expected_start = wave * f_count;
        for (size_t index = 0; index != f_count; ++index)
            CHECK(tu_sequences[wave][index] == expected_start + index);
    }
    CHECK(arm_barrier.arrived() == f_count);
    CHECK(commit_barrier.arrived() == f_count);

    const char* profile_name = profile == CACHE_PROFILE_P29V1
                                   ? "P29V1"
                                   : profile == CACHE_PROFILE_ZSTD_ROUTE
                                         ? "ZSTD_ROUTE"
                                         : "ZSTD_TU";
    std::printf("A01-%zu %s: first-wave CacheWire overlap plus same-F warm "
                "repeat/edited REL_SEQ 0/1/2; %zu exact commits\n",
                f_count, profile_name, f_count);
    std::fflush(stdout);
}

void test_parallel_distinct_f_matrix() {
    for (size_t count : {size_t{2}, size_t{3}, size_t{4}}) {
        test_parallel_distinct_f_cachewire(count, CACHE_PROFILE_ZSTD_TU);
        test_parallel_distinct_f_cachewire(count, CACHE_PROFILE_ZSTD_ROUTE);
    }
    test_parallel_distinct_f_cachewire(2, CACHE_PROFILE_P29V1);
    test_parallel_distinct_f_cachewire(3, CACHE_PROFILE_P29V1);
    test_parallel_distinct_f_cachewire(4, CACHE_PROFILE_P29V1);
}

void test_same_link_serialization(uint32_t first_profile,
                                  uint32_t second_profile,
                                  std::string_view case_id) {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = static_cast<uint8_t>(0x91 + first_profile +
                                                second_profile);
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.source_open_arm_timeout = std::chrono::seconds(5);
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot remote_root{};
    remote_root.bytes[14] = 0x93;
    const FStoreGuid f_guid = f_store_guid_for_root(remote_root);
    constexpr uint64_t f_generation = 0x559;
    uint16_t port = 0;
    const int listener = loopback_listener(port);
    std::array<SourceArmServerObservation, 2> observations;
    SourceCommitHold hold;
    std::thread server([&] {
        serve_source_transfers_on_persistent_f(
            listener, f_guid, f_generation, launch.c_store_guid, observations,
            nullptr, nullptr, &hold);
    });

    const std::array<uint8_t, 12> first_source{
        'f', 'i', 'r', 's', 't', '-', 'i', 'n', 'p', 'u', 't', '\n'};
    const std::array<uint8_t, 13> second_source{
        's', 'e', 'c', 'o', 'n', 'd', '-', 'i', 'n', 'p', 'u', 't', '\n'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(8),
        clock.clock_domain_id, clock.time_namespace_id);
    std::promise<local::P50SourceTransferResult> first_completion;
    std::future<local::P50SourceTransferResult> first_result =
        first_completion.get_future();
    std::thread first_transfer([&] {
        first_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(port, 810, first_profile), deadline,
            local::HandoffFd(source_file("p50-same-link-first", first_source))));
    });
    const bool first_held = hold.wait_until_entered(std::chrono::seconds(3));

    std::promise<local::P50SourceTransferResult> second_completion;
    std::future<local::P50SourceTransferResult> second_result =
        second_completion.get_future();
    std::thread second_transfer([&] {
        second_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(port, 811, second_profile), deadline,
            local::HandoffFd(source_file("p50-same-link-second", second_source))));
    });
    pollfd pending_connection{listener, POLLIN, 0};
    int pending = -1;
    do {
        pending = ::poll(&pending_connection, 1, 180);
    } while (pending < 0 && errno == EINTR);
    const bool second_connected_while_first_held = pending > 0;

    // Release regardless of assertion outcomes before joining owned threads.
    hold.release();
    const bool first_ready = first_result.wait_for(std::chrono::seconds(4)) ==
                             std::future_status::ready;
    const bool second_ready = second_result.wait_for(std::chrono::seconds(4)) ==
                              std::future_status::ready;
    local::P50SourceTransferResult first_value;
    local::P50SourceTransferResult second_value;
    if (first_ready)
        first_value = first_result.get();
    if (second_ready)
        second_value = second_result.get();
    first_transfer.join();
    second_transfer.join();
    // If either request expired before its socket was accepted, wake the
    // fixture's bounded accept so it can close cleanly.
    if (!observations[1].accepted) {
        const int wake = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wake >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            (void)::connect(wake, reinterpret_cast<const sockaddr*>(&address),
                            sizeof(address));
            (void)::close(wake);
        }
    }
    server.join();

    CHECK(first_held);
    CHECK(!second_connected_while_first_held);
    CHECK(first_ready && second_ready);
    CHECK(first_value.code == local::SourceTransferResultCode::Committed);
    CHECK(second_value.code == local::SourceTransferResultCode::Committed);
    CHECK(first_value.c_store_guid == launch.c_store_guid &&
          second_value.c_store_guid == launch.c_store_guid);
    CHECK(first_value.tu_seq == 0 && second_value.tu_seq == 1);
    CHECK(first_value.raw_bytes == first_source.size() &&
          first_value.raw_digest == icecc::digest128(first_source));
    CHECK(second_value.raw_bytes == second_source.size() &&
          second_value.raw_digest == icecc::digest128(second_source));
    CHECK(observations[0].committed_input ==
          std::vector<uint8_t>(first_source.begin(), first_source.end()));
    CHECK(observations[1].committed_input ==
          std::vector<uint8_t>(second_source.begin(), second_source.end()));
    CHECK(observations[0].committed_profile ==
          (first_profile == CACHE_PROFILE_P29V1
               ? ProfileId::P29V1
               : first_profile == CACHE_PROFILE_ZSTD_ROUTE
                     ? ProfileId::ZSTD_ROUTE
                     : ProfileId::ZSTD_TU));
    CHECK(observations[1].committed_profile ==
          (second_profile == CACHE_PROFILE_P29V1
               ? ProfileId::P29V1
               : second_profile == CACHE_PROFILE_ZSTD_ROUTE
                     ? ProfileId::ZSTD_ROUTE
                     : ProfileId::ZSTD_TU));
    CHECK(observations[0].committed_rel_seq == 0);
    CHECK(observations[1].committed_rel_seq ==
          (first_profile == second_profile ? 1 : 0));
    if (first_profile == second_profile)
        CHECK(observations[0].committed_history_nonce ==
              observations[1].committed_history_nonce);
    CHECK(observations[0].commit_identity_matches_input &&
          observations[1].commit_identity_matches_input);
    std::printf("%.*s: same C/F operations serialized across profile selection; "
                "both exact inputs committed in TU order\n",
                static_cast<int>(case_id.size()), case_id.data());
    std::fflush(stdout);
}

void test_same_link_serialization_matrix() {
    test_same_link_serialization(CACHE_PROFILE_P29V1, CACHE_PROFILE_P29V1,
                                 "A03 P29V1");
    test_same_link_serialization(CACHE_PROFILE_ZSTD_TU,
                                 CACHE_PROFILE_ZSTD_TU, "A03 ZSTD_TU");
    test_same_link_serialization(CACHE_PROFILE_ZSTD_ROUTE,
                                 CACHE_PROFILE_ZSTD_ROUTE,
                                 "A03 ZSTD_ROUTE");
    test_same_link_serialization(CACHE_PROFILE_P29V1,
                                 CACHE_PROFILE_ZSTD_TU, "A04 P29V1->TU");
    test_same_link_serialization(CACHE_PROFILE_ZSTD_TU,
                                 CACHE_PROFILE_ZSTD_ROUTE,
                                 "A04 TU->ROUTE");
    test_same_link_serialization(CACHE_PROFILE_ZSTD_ROUTE,
                                 CACHE_PROFILE_P29V1,
                                 "A04 ROUTE->P29V1");
}

void test_expired_alias_cannot_release_held_incarnation() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0xa1;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_active_source_transfers = 3;
    config.max_aggregate_source_raw_bytes = 1024;
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot remote_root{};
    remote_root.bytes[14] = 0xa2;
    const FStoreGuid f_guid = f_store_guid_for_root(remote_root);
    constexpr uint64_t f_generation = 0xa3;
    uint16_t port = 0;
    const int listener = loopback_listener(port, INADDR_ANY);
    CHECK(runtime.seed_route_endpoint_identity_for_test(
        "127.0.0.1", port, f_guid, f_generation));
    CHECK(runtime.seed_route_endpoint_identity_for_test(
        "127.0.0.2", port, f_guid, f_generation));
    CHECK(runtime.seed_route_endpoint_identity_for_test(
        "127.0.0.3", port, f_guid, f_generation));

    std::array<SourceArmServerObservation, 2> observations;
    SourceCommitHold hold;
    std::thread server([&] {
        serve_source_transfers_on_persistent_f(
            listener, f_guid, f_generation, launch.c_store_guid, observations,
            nullptr, nullptr, &hold);
    });
    const std::array<uint8_t, 12> first_source{
        'a', 'l', 'i', 'a', 's', '-', 'o', 'n', 'e', '\n', '!', '\n'};
    const std::array<uint8_t, 13> expired_source{
        'a', 'l', 'i', 'a', 's', '-', 't', 'w', 'o', '\n', '!', '\n', '!'};
    const std::array<uint8_t, 14> final_source{
        'a', 'l', 'i', 'a', 's', '-', 't', 'h', 'r', 'e', 'e', '\n', '!', '\n'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto first_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(7),
            clock.clock_domain_id, clock.time_namespace_id);
    auto first_request = source_transfer_request(
        port, 910, CACHE_PROFILE_ZSTD_TU);
    std::promise<local::P50SourceTransferResult> first_completion;
    auto first_result = first_completion.get_future();
    std::thread first_transfer([&] {
        first_completion.set_value(runtime.transfer_source_on_owner(
            first_request, first_deadline,
            local::HandoffFd(source_file("p50-alias-held", first_source))));
    });
    const bool first_held = hold.wait_until_entered(std::chrono::seconds(3));

    const auto expired_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500),
            clock.clock_domain_id, clock.time_namespace_id);
    auto expired_request = source_transfer_request(
        port, 911, CACHE_PROFILE_ZSTD_TU);
    expired_request.selected_f_host = "127.0.0.2";
    std::promise<local::P50SourceTransferResult> expired_completion;
    auto expired_result = expired_completion.get_future();
    std::thread expired_transfer([&] {
        expired_completion.set_value(runtime.transfer_source_on_owner(
            expired_request, expired_deadline,
            local::HandoffFd(source_file("p50-alias-expired", expired_source))));
    });

    const auto final_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            clock.clock_domain_id, clock.time_namespace_id);
    auto final_request = source_transfer_request(
        port, 912, CACHE_PROFILE_ZSTD_TU);
    final_request.selected_f_host = "127.0.0.3";
    std::promise<local::P50SourceTransferResult> final_completion;
    auto final_result = final_completion.get_future();
    std::thread final_transfer([&] {
        final_completion.set_value(runtime.transfer_source_on_owner(
            final_request, final_deadline,
            local::HandoffFd(source_file("p50-alias-final", final_source))));
    });

    const bool expired_before_release =
        expired_result.wait_for(std::chrono::seconds(2)) ==
        std::future_status::ready;
    local::P50SourceTransferResult expired_value;
    if (expired_before_release)
        expired_value = expired_result.get();
    const bool final_still_waiting_after_expiry =
        final_result.wait_for(std::chrono::milliseconds(250)) ==
        std::future_status::timeout;
    pollfd pending_connection{listener, POLLIN, 0};
    int pending = -1;
    do {
        pending = ::poll(&pending_connection, 1, 0);
    } while (pending < 0 && errno == EINTR);
    const bool alias_connected_before_predecessor_release = pending > 0;

    hold.release();
    const bool first_ready = first_result.wait_for(std::chrono::seconds(3)) ==
                             std::future_status::ready;
    const bool final_ready = final_result.wait_for(std::chrono::seconds(3)) ==
                             std::future_status::ready;
    local::P50SourceTransferResult first_value;
    local::P50SourceTransferResult final_value;
    if (first_ready)
        first_value = first_result.get();
    if (final_ready)
        final_value = final_result.get();
    first_transfer.join();
    expired_transfer.join();
    final_transfer.join();
    server.join();

    CHECK(first_held);
    CHECK(expired_before_release);
    CHECK(expired_value.code == local::SourceTransferResultCode::Error);
    CHECK(expired_value.error_code == 7 && expired_value.attempts == 0);
    CHECK(final_still_waiting_after_expiry);
    CHECK(!alias_connected_before_predecessor_release);
    CHECK(first_ready && final_ready);
    CHECK(first_value.code == local::SourceTransferResultCode::Committed);
    CHECK(first_value.tu_seq == 0 && first_value.c_store_guid == launch.c_store_guid);
    CHECK(first_value.raw_bytes == first_source.size() &&
          first_value.raw_digest == icecc::digest128(first_source));
    CHECK(final_value.code == local::SourceTransferResultCode::Committed);
    CHECK(final_value.tu_seq == 1 && final_value.c_store_guid == launch.c_store_guid);
    CHECK(final_value.raw_bytes == final_source.size() &&
          final_value.raw_digest == icecc::digest128(final_source));
    CHECK(observations[0].committed_input ==
          std::vector<uint8_t>(first_source.begin(), first_source.end()));
    CHECK(observations[1].committed_input ==
          std::vector<uint8_t>(final_source.begin(), final_source.end()));
    CHECK(observations[0].committed_profile == ProfileId::ZSTD_TU &&
          observations[1].committed_profile == ProfileId::ZSTD_TU);
    CHECK(observations[0].commit_identity_matches_input &&
          observations[1].commit_identity_matches_input);
    std::puts("A07/A12 ZSTD_TU: expired endpoint alias did not release held F incarnation");
    std::fflush(stdout);
}

void test_incarnation_change_waits_for_old_operation() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0xa8;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_route_relationships = 2;
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot remote_root{};
    remote_root.bytes[14] = 0xa9;
    const FStoreGuid f_guid = f_store_guid_for_root(remote_root);
    constexpr uint64_t old_generation = 0xaa;
    constexpr uint64_t new_generation = 0xab;
    uint16_t port = 0;
    const int listener = loopback_listener(port);
    CHECK(runtime.seed_route_endpoint_identity_for_test(
        "127.0.0.1", port, f_guid, old_generation));
    std::array<SourceArmServerObservation, 2> observations;
    SourceCommitHold hold;
    std::thread server([&] {
        serve_two_held_source_transfers(listener, f_guid, old_generation,
                                        observations, hold, new_generation);
    });

    const std::array<uint8_t, 15> old_source{
        'o', 'l', 'd', '-', 'i', 'n', 'c', 'a', 'r', 'n', 'a', 't', 'i', 'o', 'n'};
    const std::array<uint8_t, 15> new_source{
        'n', 'e', 'w', '-', 'i', 'n', 'c', 'a', 'r', 'n', 'a', 't', 'i', 'o', 'n'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(8),
        clock.clock_domain_id, clock.time_namespace_id);
    std::promise<local::P50SourceTransferResult> old_completion;
    auto old_result = old_completion.get_future();
    std::thread old_transfer([&] {
        old_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(port, 920, CACHE_PROFILE_ZSTD_TU), deadline,
            local::HandoffFd(source_file("p50-old-incarnation", old_source))));
    });
    const bool old_held = hold.wait_until_entered(std::chrono::seconds(3));

    std::promise<local::P50SourceTransferResult> new_completion;
    auto new_result = new_completion.get_future();
    std::thread new_transfer([&] {
        new_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(port, 921, CACHE_PROFILE_ZSTD_ROUTE), deadline,
            local::HandoffFd(source_file("p50-new-incarnation", new_source))));
    });
    pollfd pending_connection{listener, POLLIN, 0};
    int pending = -1;
    do {
        pending = ::poll(&pending_connection, 1, 180);
    } while (pending < 0 && errno == EINTR);
    const bool replacement_connected_while_old_held = pending > 0;
    hold.release();
    const bool old_ready = old_result.wait_for(std::chrono::seconds(4)) ==
                           std::future_status::ready;
    const bool new_ready = new_result.wait_for(std::chrono::seconds(4)) ==
                           std::future_status::ready;
    local::P50SourceTransferResult old_value;
    local::P50SourceTransferResult new_value;
    if (old_ready)
        old_value = old_result.get();
    if (new_ready)
        new_value = new_result.get();
    old_transfer.join();
    new_transfer.join();
    server.join();

    CHECK(old_held);
    CHECK(!replacement_connected_while_old_held);
    CHECK(old_ready && new_ready);
    CHECK(old_value.code == local::SourceTransferResultCode::Committed);
    CHECK(new_value.code == local::SourceTransferResultCode::Committed);
    CHECK(old_value.tu_seq == 0 && new_value.tu_seq == 1);
    CHECK(old_value.c_store_guid == launch.c_store_guid &&
          new_value.c_store_guid == launch.c_store_guid);
    CHECK(old_value.raw_bytes == old_source.size() &&
          old_value.raw_digest == icecc::digest128(old_source));
    CHECK(new_value.raw_bytes == new_source.size() &&
          new_value.raw_digest == icecc::digest128(new_source));
    CHECK(observations[0].committed_input ==
          std::vector<uint8_t>(old_source.begin(), old_source.end()));
    CHECK(observations[1].committed_input ==
          std::vector<uint8_t>(new_source.begin(), new_source.end()));
    CHECK(observations[0].committed_profile == ProfileId::ZSTD_TU &&
          observations[1].committed_profile == ProfileId::ZSTD_ROUTE);
    CHECK(observations[0].commit_identity_matches_input &&
          observations[1].commit_identity_matches_input);
    std::puts("A08 ZSTD_TU->ZSTD_ROUTE: generation change waited for old input to settle; new identity committed exactly");
    std::fflush(stdout);
}

void test_parallel_distinct_c_one_f(size_t c_count) {
    CHECK(c_count >= 2 && c_count <= 4);
    StoreIdentityRoot remote_root{};
    remote_root.bytes[14] = 0x4f;
    remote_root.bytes[15] = 0xe1;
    const FStoreGuid f_guid = f_store_guid_for_root(remote_root);
    constexpr uint64_t f_generation = 0x771;
    uint16_t port = 0;
    const int listener = loopback_listener(port);
    std::vector<SourceArmServerObservation> observations(c_count);
    CodecWorkerGate codec_gate;
    std::atomic<size_t> observed_live_sessions{0};
    std::atomic<size_t> observed_namespaces{0};
    std::atomic<bool> stop_sampling{false};

    std::vector<SidecarLaunchIdentity> launches;
    std::vector<std::unique_ptr<service::SidecarRuntime>> runtimes;
    std::vector<std::vector<uint8_t>> inputs(c_count);
    launches.reserve(c_count);
    runtimes.reserve(c_count);
    for (size_t index = 0; index != c_count; ++index) {
        StoreIdentityRoot local_root{};
        local_root.bytes[14] = 0x80;
        local_root.bytes[15] = static_cast<uint8_t>(0x20 + index);
        launches.push_back(test_sidecar_launch(local_root));
        service::RuntimeConfig config = test_runtime_config();
        config.c_store_guid = launches.back().c_store_guid;
        config.f_store_guid = launches.back().f_store_guid;
        config.f_store_generation = launches.back().store_generation;
        config.sidecar_launch = launches.back();
        config.max_active_source_transfers = 1;
        config.max_aggregate_source_raw_bytes = 1024;
        runtimes.push_back(
            std::make_unique<service::SidecarRuntime>(std::move(config)));
        inputs[index].resize(101 + index * 13);
        for (size_t byte = 0; byte != inputs[index].size(); ++byte)
            inputs[index][byte] = static_cast<uint8_t>(
                (byte * 19 + index * 71 + 5) & 0xff);
    }
    std::vector<CStoreGuid> expected_c_guids;
    expected_c_guids.reserve(c_count);
    for (const auto& launch : launches)
        expected_c_guids.push_back(launch.c_store_guid);
    std::thread server([&] {
        serve_many_source_transfers_on_shared_f_endpoint(
            listener, f_guid, f_generation, observations, expected_c_guids,
            codec_gate, observed_live_sessions, observed_namespaces,
            stop_sampling);
    });

    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(5),
        clock.clock_domain_id, clock.time_namespace_id);
    std::vector<std::promise<local::P50SourceTransferResult>> completions;
    std::vector<std::future<local::P50SourceTransferResult>> results;
    std::vector<std::thread> transfers;
    completions.reserve(c_count);
    results.reserve(c_count);
    transfers.reserve(c_count);
    for (size_t index = 0; index != c_count; ++index) {
        completions.emplace_back();
        results.push_back(completions.back().get_future());
        const uint64_t request_id = 700 + index + c_count * 10;
        transfers.emplace_back([&, index, request_id] {
            completions[index].set_value(runtimes[index]->transfer_source_on_owner(
                source_transfer_request(port, request_id,
                                        CACHE_PROFILE_ZSTD_TU),
                deadline,
                local::HandoffFd(source_file("p50-distinct-c", inputs[index]))));
        });
    }
    const size_t held_workers = std::min<size_t>(2, c_count);
    const bool codec_workers_held =
        codec_gate.wait_for_arrivals(held_workers, std::chrono::seconds(3));
    const auto overlap_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(3);
    while ((observed_live_sessions.load(std::memory_order_acquire) < c_count ||
            observed_namespaces.load(std::memory_order_acquire) < c_count) &&
           std::chrono::steady_clock::now() < overlap_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool all_namespaces_live_while_workers_held =
        observed_live_sessions.load(std::memory_order_acquire) >= c_count &&
        observed_namespaces.load(std::memory_order_acquire) >= c_count;
    stop_sampling.store(true, std::memory_order_release);
    codec_gate.release();
    std::vector<local::P50SourceTransferResult> committed;
    committed.reserve(c_count);
    for (auto& result : results)
        committed.push_back(result.get());
    for (size_t index = 0; index != c_count; ++index) {
        const int wake = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wake < 0)
            continue;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        (void)::connect(wake, reinterpret_cast<const sockaddr*>(&address),
                        sizeof(address));
        (void)::close(wake);
    }
    for (auto& transfer : transfers)
        transfer.join();
    server.join();

    std::vector<CStoreGuid> c_guids;
    for (size_t index = 0; index != c_count; ++index) {
        const auto& result = committed[index];
        CHECK(result.code == local::SourceTransferResultCode::Committed);
        CHECK(result.valid() && result.attempts == 1 && result.tu_seq == 0);
        CHECK(result.c_store_guid == launches[index].c_store_guid);
        CHECK(result.raw_bytes == inputs[index].size());
        CHECK(result.raw_digest == icecc::digest128(inputs[index]));
        const auto observation = std::find_if(
            observations.begin(), observations.end(), [&](const auto& seen) {
                return seen.committed_c_store_guid == launches[index].c_store_guid;
            });
        CHECK(observation != observations.end());
        CHECK(observation->committed_input == inputs[index]);
        CHECK(observation->committed_profile == ProfileId::ZSTD_TU);
        CHECK(observation->commit_identity_matches_input);
        CHECK(observation->accepted && observation->protocol_50 &&
              observation->arm_received && observation->armed_sent &&
              observation->cache_session_received &&
              observation->ready_sent && observation->transfer_completed);
        c_guids.push_back(result.c_store_guid);
    }
    std::sort(c_guids.begin(), c_guids.end());
    CHECK(std::adjacent_find(c_guids.begin(), c_guids.end()) == c_guids.end());
    CHECK(codec_workers_held);
    CHECK(all_namespaces_live_while_workers_held);
    std::printf("A02-%zu ZSTD_TU: one F owner held %zu codec workers while all "
                "C namespaces were live\n", c_count, held_workers);
    std::fflush(stdout);
}

void test_parallel_distinct_c_matrix() {
    for (size_t count : {size_t{2}, size_t{3}, size_t{4}})
        test_parallel_distinct_c_one_f(count);
}

void test_source_active_count_cap_waits_then_releases() {
    StoreIdentityRoot root{};
    root.bytes[15] = 0xb1;
    const auto launch = test_sidecar_launch(root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_active_source_transfers = 1;
    config.max_aggregate_source_raw_bytes = 128;
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot first_root{};
    first_root.bytes[14] = 0xb2;
    StoreIdentityRoot second_root{};
    second_root.bytes[14] = 0xb3;
    const auto first_guid = f_store_guid_for_root(first_root);
    const auto second_guid = f_store_guid_for_root(second_root);
    uint16_t first_port = 0;
    uint16_t second_port = 0;
    const int first_listener = loopback_listener(first_port);
    const int second_listener = loopback_listener(second_port);
    SourceArmServerObservation first_observation;
    SourceArmServerObservation second_observation;
    SourceCommitHold first_hold;
    std::thread first_server([&] {
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        const int accepted = ::accept(
            first_listener, reinterpret_cast<sockaddr*>(&peer), &size);
        (void)::close(first_listener);
        serve_accepted_source_transfer(
            accepted, peer, size, first_guid, 301, first_observation,
            false, nullptr, nullptr, &first_hold);
    });
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto make_deadline = [&clock](std::chrono::seconds duration) {
        return sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + duration,
            clock.clock_domain_id, clock.time_namespace_id);
    };
    const std::array<uint8_t, 12> first_source{
        'c', 'o', 'u', 'n', 't', '-', 'c', 'a', 'p', '-', '1', '!'};
    const std::array<uint8_t, 10> second_source{
        'c', 'o', 'u', 'n', 't', '-', 'c', 'a', 'p', '!'};
    std::promise<local::P50SourceTransferResult> first_completion;
    std::promise<local::P50SourceTransferResult> second_completion;
    auto first_result = first_completion.get_future();
    auto second_result = second_completion.get_future();
    std::thread first_transfer([&] {
        first_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(first_port, 1301, CACHE_PROFILE_ZSTD_TU),
            make_deadline(std::chrono::seconds(6)),
            local::HandoffFd(source_file("p50-count-cap-first", first_source))));
    });
    const bool first_held =
        first_hold.wait_until_entered(std::chrono::seconds(3));
    std::promise<void> second_started;
    auto second_started_result = second_started.get_future();
    std::thread second_transfer([&] {
        second_started.set_value();
        second_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(second_port, 1302, CACHE_PROFILE_ZSTD_TU),
            make_deadline(std::chrono::seconds(6)),
            local::HandoffFd(source_file("p50-count-cap-second", second_source))));
    });
    second_started_result.wait();
    pollfd second_ready{second_listener, POLLIN, 0};
    int accepted_before_release = -1;
    do {
        accepted_before_release = ::poll(&second_ready, 1, 180);
    } while (accepted_before_release < 0 && errno == EINTR);

    first_hold.release();
    std::thread second_server([&] {
        serve_one_source_transfer(second_listener, second_guid, 302,
                                  second_observation);
    });
    const bool first_ready = first_result.wait_for(std::chrono::seconds(7)) ==
                             std::future_status::ready;
    const bool second_ready_result =
        second_result.wait_for(std::chrono::seconds(7)) ==
        std::future_status::ready;
    local::P50SourceTransferResult first_value;
    local::P50SourceTransferResult second_value;
    if (first_ready)
        first_value = first_result.get();
    if (second_ready_result)
        second_value = second_result.get();
    first_transfer.join();
    second_transfer.join();
    for (const uint16_t port : {first_port, second_port}) {
        const int wake = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wake >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            (void)::connect(wake, reinterpret_cast<const sockaddr*>(&address),
                            sizeof(address));
            (void)::close(wake);
        }
    }
    first_server.join();
    second_server.join();
    CHECK(first_held);
    CHECK(accepted_before_release == 0);
    CHECK(first_ready && second_ready_result);
    CHECK(first_value.code == local::SourceTransferResultCode::Committed);
    CHECK(second_value.code == local::SourceTransferResultCode::Committed);
    CHECK(first_value.raw_bytes == first_source.size() &&
          first_value.raw_digest == icecc::digest128(first_source));
    CHECK(second_value.raw_bytes == second_source.size() &&
          second_value.raw_digest == icecc::digest128(second_source));
    CHECK(first_value.c_store_guid == launch.c_store_guid &&
          second_value.c_store_guid == launch.c_store_guid);
    CHECK(first_value.tu_seq == 0 && second_value.tu_seq == 1);
    CHECK(first_observation.committed_input ==
          std::vector<uint8_t>(first_source.begin(), first_source.end()));
    CHECK(second_observation.committed_input ==
          std::vector<uint8_t>(second_source.begin(), second_source.end()));
    CHECK(first_observation.committed_profile == ProfileId::ZSTD_TU &&
          second_observation.committed_profile == ProfileId::ZSTD_TU);
    CHECK(first_observation.commit_identity_matches_input &&
          second_observation.commit_identity_matches_input);
    CHECK(first_observation.transfer_completed &&
          second_observation.transfer_completed);
    CHECK(second_observation.arm_received);
    std::printf("A09: active-source count pressure waited and released after commit\n");
    std::fflush(stdout);
}

void test_source_raw_byte_cap_waits_then_releases() {
    StoreIdentityRoot root{};
    root.bytes[15] = 0xb4;
    const auto launch = test_sidecar_launch(root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_active_source_transfers = 4;
    config.max_aggregate_source_raw_bytes = 32;
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot first_root{};
    first_root.bytes[14] = 0xb5;
    StoreIdentityRoot second_root{};
    second_root.bytes[14] = 0xb6;
    const auto first_guid = f_store_guid_for_root(first_root);
    const auto second_guid = f_store_guid_for_root(second_root);
    uint16_t first_port = 0;
    uint16_t second_port = 0;
    const int first_listener = loopback_listener(first_port);
    const int second_listener = loopback_listener(second_port);
    SourceArmServerObservation first_observation;
    SourceArmServerObservation second_observation;
    SourceCommitHold first_hold;
    std::thread first_server([&] {
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        const int accepted = ::accept(
            first_listener, reinterpret_cast<sockaddr*>(&peer), &size);
        (void)::close(first_listener);
        serve_accepted_source_transfer(
            accepted, peer, size, first_guid, 311, first_observation,
            false, nullptr, nullptr, &first_hold);
    });
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = [&clock] {
        return sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(6),
            clock.clock_domain_id, clock.time_namespace_id);
    };
    const std::array<uint8_t, 24> first_source{
        'r','a','w','-','b','y','t','e','-','c','a','p','-','f','i','r','s','t','!','!','!','!','!','!'};
    const std::array<uint8_t, 9> second_source{
        'r','a','w','-','b','y','t','e','!'};
    std::promise<local::P50SourceTransferResult> first_completion;
    std::promise<local::P50SourceTransferResult> second_completion;
    auto first_result = first_completion.get_future();
    auto second_result = second_completion.get_future();
    std::thread first_transfer([&] {
        first_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(first_port, 1311, CACHE_PROFILE_ZSTD_ROUTE),
            deadline(),
            local::HandoffFd(source_file("p50-byte-cap-first", first_source))));
    });
    const bool first_held =
        first_hold.wait_until_entered(std::chrono::seconds(3));
    std::promise<void> second_started;
    auto second_started_result = second_started.get_future();
    std::thread second_transfer([&] {
        second_started.set_value();
        second_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(second_port, 1312, CACHE_PROFILE_ZSTD_ROUTE),
            deadline(),
            local::HandoffFd(source_file("p50-byte-cap-second", second_source))));
    });
    second_started_result.wait();
    pollfd second_ready{second_listener, POLLIN, 0};
    int accepted_before_release = -1;
    do {
        accepted_before_release = ::poll(&second_ready, 1, 180);
    } while (accepted_before_release < 0 && errno == EINTR);

    first_hold.release();
    std::thread second_server([&] {
        serve_one_source_transfer(second_listener, second_guid, 312,
                                  second_observation);
    });
    const bool first_ready = first_result.wait_for(std::chrono::seconds(7)) ==
                             std::future_status::ready;
    const bool second_ready_result =
        second_result.wait_for(std::chrono::seconds(7)) ==
        std::future_status::ready;
    local::P50SourceTransferResult first_value;
    local::P50SourceTransferResult second_value;
    if (first_ready)
        first_value = first_result.get();
    if (second_ready_result)
        second_value = second_result.get();
    first_transfer.join();
    second_transfer.join();
    for (const uint16_t port : {first_port, second_port}) {
        const int wake = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wake >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            (void)::connect(wake, reinterpret_cast<const sockaddr*>(&address),
                            sizeof(address));
            (void)::close(wake);
        }
    }
    first_server.join();
    second_server.join();
    CHECK(first_held);
    CHECK(accepted_before_release == 0);
    CHECK(first_ready && second_ready_result);
    CHECK(first_value.code == local::SourceTransferResultCode::Committed);
    CHECK(second_value.code == local::SourceTransferResultCode::Committed);
    CHECK(first_value.raw_bytes == first_source.size() &&
          first_value.raw_digest == icecc::digest128(first_source));
    CHECK(second_value.raw_bytes == second_source.size() &&
          second_value.raw_digest == icecc::digest128(second_source));
    CHECK(first_value.c_store_guid == launch.c_store_guid &&
          second_value.c_store_guid == launch.c_store_guid);
    CHECK(first_value.tu_seq == 0 && second_value.tu_seq == 1);
    CHECK(first_observation.committed_profile == ProfileId::ZSTD_ROUTE &&
          second_observation.committed_profile == ProfileId::ZSTD_ROUTE);
    CHECK(first_observation.commit_identity_matches_input &&
          second_observation.commit_identity_matches_input);
    CHECK(first_observation.committed_input ==
          std::vector<uint8_t>(first_source.begin(), first_source.end()));
    CHECK(second_observation.committed_input ==
          std::vector<uint8_t>(second_source.begin(), second_source.end()));
    CHECK(second_observation.transfer_completed);
    std::printf("A10: aggregate raw-byte pressure waited and released exactly\n");
    std::fflush(stdout);
}

void test_source_admission_releases_on_open_read_error_and_expiry() {
    StoreIdentityRoot root{};
    root.bytes[15] = 0xb7;
    const auto launch = test_sidecar_launch(root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_active_source_transfers = 1;
    config.max_aggregate_source_raw_bytes = 64;
    config.source_open_arm_timeout = std::chrono::milliseconds(500);
    service::SidecarRuntime runtime(std::move(config));

    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = [&clock](std::chrono::milliseconds duration) {
        return sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + duration,
            clock.clock_domain_id, clock.time_namespace_id);
    };
    const auto root_guid = [](uint8_t value) {
        StoreIdentityRoot remote{};
        remote.bytes[14] = 0xb8;
        remote.bytes[15] = value;
        return f_store_guid_for_root(remote);
    };

    // An accepted F that never answers SOURCE_ARMED forces an open failure
    // after the source count and byte reservation have already been granted.
    uint16_t open_port = 0;
    const int open_listener = loopback_listener(open_port);
    std::atomic<bool> open_arm_received{false};
    std::promise<void> release_open_server;
    auto release_open = release_open_server.get_future().share();
    std::thread open_server([&] {
        serve_stalled_source_arm(open_listener, open_arm_received, release_open);
    });
    const std::array<uint8_t, 8> open_source{'o','p','e','n','-','e','r','r'};
    std::promise<local::P50SourceTransferResult> open_completion;
    auto open_result = open_completion.get_future();
    std::thread open_transfer([&] {
        open_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(open_port, 1321, CACHE_PROFILE_ZSTD_TU),
            deadline(std::chrono::seconds(4)),
            local::HandoffFd(source_file("p50-open-error", open_source))));
    });
    const auto arm_wait_until = std::chrono::steady_clock::now() +
                                std::chrono::seconds(2);
    while (!open_arm_received.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < arm_wait_until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool open_arm_seen = open_arm_received.load(std::memory_order_acquire);
    const auto open_value = open_result.get();
    open_transfer.join();
    release_open_server.set_value();
    open_server.join();
    CHECK(open_arm_seen);
    CHECK(open_value.code == local::SourceTransferResultCode::Error);

    // Change the already-reserved file after SOURCE_ARMED is observed but
    // before the owner releases the admission barrier; read must refuse it.
    uint16_t read_port = 0;
    const int read_listener = loopback_listener(read_port);
    const FStoreGuid read_guid = root_guid(0xb9);
    SourceArmServerObservation read_observation;
    SourceTransferBarrier read_barrier(2);
    std::thread read_server([&] {
        serve_one_source_transfer(read_listener, read_guid, 321,
                                  read_observation, &read_barrier);
    });
    const std::array<uint8_t, 12> read_source{
        'r','e','a','d','-','e','r','r','o','r','!','!'};
    const int read_fd = source_file("p50-read-error", read_source);
    const int mutation_fd = ::dup(read_fd);
    CHECK(mutation_fd >= 0);
    std::promise<local::P50SourceTransferResult> read_completion;
    auto read_result = read_completion.get_future();
    std::thread read_transfer([&] {
        read_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(read_port, 1322, CACHE_PROFILE_ZSTD_ROUTE),
            deadline(std::chrono::seconds(4)), local::HandoffFd(read_fd)));
    });
    const auto barrier_wait_until = std::chrono::steady_clock::now() +
                                    std::chrono::seconds(2);
    while (read_barrier.arrived() == 0 &&
           std::chrono::steady_clock::now() < barrier_wait_until)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool read_barrier_seen = read_barrier.arrived() == 1;
    const bool read_file_truncated = ::ftruncate(mutation_fd, 0) == 0;
    (void)::close(mutation_fd);
    bool read_barrier_released = false;
    if (read_barrier_seen)
        read_barrier_released = read_barrier.arrive_and_wait();
    else {
        const int wake = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wake >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(read_port);
            (void)::connect(wake, reinterpret_cast<const sockaddr*>(&address),
                            sizeof(address));
            (void)::close(wake);
        }
    }
    const auto read_value = read_result.get();
    read_transfer.join();
    read_server.join();
    CHECK(read_barrier_seen);
    CHECK(read_file_truncated);
    CHECK(read_barrier_released);
    CHECK(read_value.code == local::SourceTransferResultCode::Error);
    CHECK(!read_observation.transfer_completed);

    // A held successful operation owns the sole count slot. The independent
    // F waiter expires before it can set up a connection; after release, a
    // fresh operation must still succeed (no leaked or stolen lease).
    uint16_t held_port = 0;
    const int held_listener = loopback_listener(held_port);
    SourceArmServerObservation held_observation;
    SourceCommitHold held_commit;
    const FStoreGuid held_guid = root_guid(0xba);
    std::thread held_server([&] {
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        const int accepted = ::accept(
            held_listener, reinterpret_cast<sockaddr*>(&peer), &size);
        (void)::close(held_listener);
        serve_accepted_source_transfer(
            accepted, peer, size, held_guid, 322, held_observation,
            false, nullptr, nullptr, &held_commit);
    });
    const std::array<uint8_t, 9> held_source{'h','e','l','d','-','o','k','!','!'};
    std::promise<local::P50SourceTransferResult> held_completion;
    auto held_result = held_completion.get_future();
    std::thread held_transfer([&] {
        held_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(held_port, 1323, CACHE_PROFILE_ZSTD_TU),
            deadline(std::chrono::seconds(5)),
            local::HandoffFd(source_file("p50-held-release", held_source))));
    });
    const bool held_entered =
        held_commit.wait_until_entered(std::chrono::seconds(3));

    uint16_t expired_port = 0;
    const int expired_listener = loopback_listener(expired_port);
    SourceArmServerObservation expired_observation;
    const std::array<uint8_t, 8> expired_source{'e','x','p','i','r','e','d','!'};
    const auto expired_value = runtime.transfer_source_on_owner(
        source_transfer_request(expired_port, 1324, CACHE_PROFILE_ZSTD_TU),
        deadline(std::chrono::milliseconds(220)),
        local::HandoffFd(source_file("p50-credit-expiry", expired_source)));
    pollfd expired_ready{expired_listener, POLLIN, 0};
    int expired_pending = -1;
    do {
        expired_pending = ::poll(&expired_ready, 1, 0);
    } while (expired_pending < 0 && errno == EINTR);
    std::thread expired_server([&] {
        serve_one_source_transfer(expired_listener, root_guid(0xbb), 323,
                                  expired_observation);
    });
    held_commit.release();
    const auto held_value = held_result.get();
    held_transfer.join();
    const int held_wake_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (held_wake_fd >= 0) {
        sockaddr_in held_address{};
        held_address.sin_family = AF_INET;
        held_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        held_address.sin_port = htons(held_port);
        (void)::connect(held_wake_fd,
                        reinterpret_cast<const sockaddr*>(&held_address),
                        sizeof(held_address));
        (void)::close(held_wake_fd);
    }
    held_server.join();
    // Wake the fixture whose request intentionally expired before setup.
    const int wake_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(wake_fd >= 0);
    sockaddr_in expired_address{};
    expired_address.sin_family = AF_INET;
    expired_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    expired_address.sin_port = htons(expired_port);
    (void)::connect(wake_fd, reinterpret_cast<const sockaddr*>(&expired_address),
                    sizeof(expired_address));
    (void)::close(wake_fd);
    expired_server.join();
    CHECK(held_entered);
    CHECK(expired_value.code == local::SourceTransferResultCode::Error);
    CHECK(expired_pending == 0);
    CHECK(held_value.code == local::SourceTransferResultCode::Committed);

    uint16_t final_port = 0;
    const int final_listener = loopback_listener(final_port);
    SourceArmServerObservation final_observation;
    const FStoreGuid final_guid = root_guid(0xbc);
    std::thread final_server([&] {
        serve_one_source_transfer(final_listener, final_guid, 324,
                                  final_observation);
    });
    const std::array<uint8_t, 7> final_source{'f','i','n','a','l','!','!'};
    const auto final_value = runtime.transfer_source_on_owner(
        source_transfer_request(final_port, 1325, CACHE_PROFILE_ZSTD_TU),
        deadline(std::chrono::seconds(4)),
        local::HandoffFd(source_file("p50-final-release", final_source)));
    const int final_wake_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (final_wake_fd >= 0) {
        sockaddr_in final_address{};
        final_address.sin_family = AF_INET;
        final_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        final_address.sin_port = htons(final_port);
        (void)::connect(final_wake_fd,
                        reinterpret_cast<const sockaddr*>(&final_address),
                        sizeof(final_address));
        (void)::close(final_wake_fd);
    }
    final_server.join();
    CHECK(final_value.code == local::SourceTransferResultCode::Committed);
    CHECK(final_value.raw_bytes == final_source.size() &&
          final_value.raw_digest == icecc::digest128(final_source));
    CHECK(final_observation.committed_input ==
          std::vector<uint8_t>(final_source.begin(), final_source.end()));
    std::printf("A11: success/open/read/expiry paths released bounded source leases\n");
    std::fflush(stdout);
}

void test_alias_waiter_releases_active_source_credit_for_independent_f() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0xbd;
    const auto launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_active_source_transfers = 2;
    config.max_aggregate_source_raw_bytes = 128;
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot shared_root{};
    shared_root.bytes[14] = 0xbe;
    StoreIdentityRoot independent_root{};
    independent_root.bytes[14] = 0xbf;
    const FStoreGuid shared_guid = f_store_guid_for_root(shared_root);
    const FStoreGuid independent_guid =
        f_store_guid_for_root(independent_root);
    constexpr uint64_t shared_generation = 401;
    uint16_t held_port = 0;
    uint16_t alias_port = 0;
    uint16_t independent_port = 0;
    const int held_listener = loopback_listener(held_port);
    const int alias_listener = loopback_listener(alias_port);
    const int independent_listener = loopback_listener(independent_port);
    SourceArmServerObservation held_observation;
    SourceArmServerObservation alias_observation;
    SourceArmServerObservation independent_observation;
    SourceCommitHold held_commit;
    SourceTransferBarrier alias_arm_barrier(2);
    std::promise<bool> alias_ready_promise;
    auto alias_ready = alias_ready_promise.get_future();
    const std::array<uint8_t, 10> held_source{
        'a','l','i','a','s','-','h','o','l','d'};
    const std::array<uint8_t, 11> alias_source{
        'a','l','i','a','s','-','w','a','i','t','!'};
    const std::array<uint8_t, 12> independent_source{
        'i','n','d','e','p','e','n','d','-','f','!','!'};
    std::promise<int> alias_fd_promise;
    auto alias_fd_future = alias_fd_promise.get_future();
    std::promise<void> held_endpoint_done_promise;
    auto held_endpoint_done_future = held_endpoint_done_promise.get_future();
    const auto accept_ready_fd = [&](int listener,
                                     SourceArmServerObservation& observation,
                                     SourceTransferBarrier* arm_barrier) {
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        const int accepted = ::accept(
            listener, reinterpret_cast<sockaddr*>(&peer), &size);
        (void)::close(listener);
        if (accepted < 0)
            return -1;
        std::unique_ptr<MsgChannel> channel(Service::createChannel(
            accepted, reinterpret_cast<sockaddr*>(&peer), size));
        if (!channel)
            return -1;
        observation.accepted = true;
        observation.protocol_50 = channel->protocol == PROTOCOL_VERSION;
        std::unique_ptr<Msg> arm_message(channel->get_msg(3, true));
        const auto* arm = arm_message != nullptr
                              ? dynamic_cast<P50SourceArmMsg*>(
                                    arm_message.get())
                              : nullptr;
        if (arm == nullptr || !arm->valid_payload())
            return -1;
        observation.arm_received = true;
        if (arm_barrier != nullptr) {
            observation.arm_barrier_passed =
                arm_barrier->arrive_and_wait();
            if (!observation.arm_barrier_passed)
                return -1;
        }
        ClaimAttemptCapability128 capability_1;
        ClaimAttemptCapability128 capability_2;
        capability_1.bytes.fill(0xd1);
        capability_2.bytes.fill(0xd2);
        const P50SourceArmedMsg acknowledgement(
            arm->arm, 101, 102, shared_generation, shared_guid.bytes,
            kStoreIdentityDerivationVersion, 103, 2500,
            capability_1, capability_2);
        if (!channel->send_msg(acknowledgement))
            return -1;
        observation.armed_sent = true;
        std::unique_ptr<Msg> cache_message(channel->get_msg(3, true));
        if (cache_message == nullptr || *cache_message != Msg::CACHE_SESSION)
            return -1;
        observation.cache_session_received = true;
        const int raw_fd = channel->release_fd_if_input_empty();
        if (raw_fd < 0)
            return -1;
        observation.ready_sent = send_cache_session_ready(
            raw_fd, std::chrono::steady_clock::now() +
                        std::chrono::seconds(2));
        if (!observation.ready_sent) {
            (void)::close(raw_fd);
            return -1;
        }
        return raw_fd;
    };

    std::thread held_server([&] {
        try {
            const int raw_fd = accept_ready_fd(
                held_listener, held_observation, nullptr);
            if (raw_fd < 0) {
                try { held_endpoint_done_promise.set_value(); } catch (...) {}
                return;
            }
            namespace asio = boost::asio;
            asio::io_context f_context;
            P50ServerEndpointConfig server_config;
            server_config.input_job_state =
                [&](CStoreGuid c_guid, const TxBegin& begin,
                    const TxCommit& commit, std::span<const uint8_t> input) {
                    SourceArmServerObservation* observation = nullptr;
                    if (std::equal(input.begin(), input.end(),
                                   held_source.begin(), held_source.end())) {
                        observation = &held_observation;
                        held_commit.enter_and_wait();
                    } else {
                        observation = &alias_observation;
                    }
                    observation->committed_c_store_guid = c_guid;
                    observation->committed_tu_seq = begin.tu_seq.value;
                    observation->committed_profile = begin.profile;
                    observation->committed_input.assign(input.begin(),
                                                        input.end());
                    observation->commit_identity_matches_input =
                        commit.tu_seq == begin.tu_seq &&
                        begin.raw_bytes == input.size() &&
                        begin.raw_digest == icecc::digest128(input) &&
                        commit.raw_digest == begin.raw_digest;
                    return InputJobState::Open;
                };
            P50ServerEndpoint f_endpoint(shared_guid, {}, nullptr, nullptr,
                                         std::move(server_config));
            const auto run_f_session = [&](int adopted_fd,
                                           SourceArmServerObservation& observation) {
                boost::system::error_code error;
                auto socket = P50ServerEndpoint::adopt_connected_fd(
                    f_context.get_executor(), adopted_fd, error);
                if (!socket.has_value()) {
                    (void)::close(adopted_fd);
                    return;
                }
                f_context.restart();
                auto result = asio::co_spawn(
                    f_context,
                    f_endpoint.run_adopted(std::move(*socket)),
                    asio::use_future);
                f_context.run();
                const ServerRunResult value = result.get();
                observation.transfer_completed =
                    value.status == ServerRunStatus::Completed &&
                    value.committed_input.has_value();
            };
            run_f_session(raw_fd, held_observation);
            try { held_endpoint_done_promise.set_value(); } catch (...) {}
            const int alias_fd = alias_fd_future.get();
            if (alias_fd >= 0)
                run_f_session(alias_fd, alias_observation);
        } catch (...) {}
        try { held_endpoint_done_promise.set_value(); } catch (...) {}
    });
    std::thread alias_server([&] {
        int raw_fd = -1;
        try {
            raw_fd = accept_ready_fd(alias_listener, alias_observation,
                                     &alias_arm_barrier);
        } catch (...) {
            if (raw_fd >= 0)
                (void)::close(raw_fd);
        }
        try { alias_fd_promise.set_value(raw_fd); } catch (...) {
            if (raw_fd >= 0)
                (void)::close(raw_fd);
        }
        try { alias_ready_promise.set_value(raw_fd >= 0); } catch (...) {}
    });
    std::thread independent_server([&] {
        serve_one_source_transfer(independent_listener, independent_guid, 402,
                                  independent_observation);
    });

    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto make_deadline = [&clock] {
        return sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(8),
            clock.clock_domain_id, clock.time_namespace_id);
    };
    std::promise<local::P50SourceTransferResult> held_completion;
    auto held_result = held_completion.get_future();
    std::thread held_transfer([&] {
        held_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(held_port, 1401, CACHE_PROFILE_ZSTD_TU),
            make_deadline(),
            local::HandoffFd(source_file("p50-alias-progress-held",
                                         held_source))));
    });
    const bool held_entered =
        held_commit.wait_until_entered(std::chrono::seconds(3));

    std::promise<local::P50SourceTransferResult> alias_completion;
    auto alias_result = alias_completion.get_future();
    std::thread alias_transfer([&] {
        alias_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(alias_port, 1402, CACHE_PROFILE_ZSTD_TU),
            make_deadline(),
            local::HandoffFd(source_file("p50-alias-progress-waiter",
                                         alias_source))));
    });
    const auto alias_arm_wait_until = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(3);
    while (alias_arm_barrier.arrived() == 0 &&
           std::chrono::steady_clock::now() < alias_arm_wait_until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool alias_arm_seen = alias_arm_barrier.arrived() == 1;
    const bool alias_arm_released = alias_arm_seen &&
                                    alias_arm_barrier.arrive_and_wait();
    const bool alias_ready_received = alias_ready.wait_for(
        std::chrono::seconds(3)) == std::future_status::ready;
    const bool alias_ready_seen = alias_ready_received && alias_ready.get();

    std::promise<local::P50SourceTransferResult> independent_completion;
    auto independent_result = independent_completion.get_future();
    std::thread independent_transfer([&] {
        independent_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(independent_port, 1403,
                                    CACHE_PROFILE_ZSTD_TU),
            make_deadline(),
            local::HandoffFd(source_file("p50-alias-progress-independent",
                                         independent_source))));
    });
    const bool independent_before_release =
        independent_result.wait_for(std::chrono::seconds(3)) ==
        std::future_status::ready;

    held_commit.release();
    const bool held_ready = held_result.wait_for(std::chrono::seconds(7)) ==
                            std::future_status::ready;
    const bool alias_done = alias_result.wait_for(std::chrono::seconds(7)) ==
                            std::future_status::ready;
    const bool independent_done = independent_before_release ||
        independent_result.wait_for(std::chrono::seconds(5)) ==
            std::future_status::ready;
    const bool held_endpoint_settled = held_endpoint_done_future.wait_for(
        std::chrono::seconds(2)) == std::future_status::ready;
    local::P50SourceTransferResult held_value;
    local::P50SourceTransferResult alias_value;
    local::P50SourceTransferResult independent_value;
    if (held_ready)
        held_value = held_result.get();
    if (alias_done)
        alias_value = alias_result.get();
    if (independent_done)
        independent_value = independent_result.get();
    held_transfer.join();
    alias_transfer.join();
    independent_transfer.join();
    for (const uint16_t port : {held_port, alias_port, independent_port}) {
        const int wake = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wake >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            (void)::connect(wake, reinterpret_cast<const sockaddr*>(&address),
                            sizeof(address));
            (void)::close(wake);
        }
    }
    alias_server.join();
    held_server.join();
    independent_server.join();

    CHECK(held_entered);
    CHECK(alias_arm_seen && alias_arm_released && alias_ready_seen);
    CHECK(held_endpoint_settled);
    CHECK(independent_before_release);
    CHECK(held_ready && alias_done && independent_done);
    CHECK(held_value.code == local::SourceTransferResultCode::Committed);
    CHECK(independent_value.code == local::SourceTransferResultCode::Committed);
    CHECK(alias_value.code == local::SourceTransferResultCode::Committed);
    CHECK(held_value.tu_seq == 0 && independent_value.tu_seq == 1 &&
          alias_value.tu_seq == 2);
    CHECK(held_value.raw_bytes == held_source.size() &&
          held_value.raw_digest == icecc::digest128(held_source));
    CHECK(independent_value.raw_bytes == independent_source.size() &&
          independent_value.raw_digest == icecc::digest128(independent_source));
    CHECK(alias_value.raw_bytes == alias_source.size() &&
          alias_value.raw_digest == icecc::digest128(alias_source));
    CHECK(held_observation.committed_input ==
          std::vector<uint8_t>(held_source.begin(), held_source.end()));
    CHECK(independent_observation.committed_input ==
          std::vector<uint8_t>(independent_source.begin(),
                               independent_source.end()));
    CHECK(alias_observation.committed_input ==
          std::vector<uint8_t>(alias_source.begin(), alias_source.end()));
    CHECK(alias_observation.committed_c_store_guid == launch.c_store_guid);
    CHECK(alias_observation.committed_tu_seq == 2);
    CHECK(alias_observation.committed_profile == ProfileId::ZSTD_TU);
    CHECK(alias_observation.commit_identity_matches_input &&
          alias_observation.transfer_completed);
    std::puts("A09 alias progress: waiting F incarnation released global credits for independent F");
    std::fflush(stdout);
}

void test_transport_loss_preserves_other_worker_service() {
    StoreIdentityRoot root{};
    root.bytes[15] = 0x73;
    const auto launch = test_sidecar_launch(root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.source_open_arm_timeout = std::chrono::milliseconds(500);
    service::SidecarRuntime runtime(std::move(config));
    const auto clock = sidecar::process_monotonic_clock_identity();
    auto deadline = [&] {
        return sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(4),
            clock.clock_domain_id, clock.time_namespace_id);
    };
    const std::array<uint8_t, 4> source{'h', '5', '!', '\n'};
    StoreIdentityRoot failed_root{};
    failed_root.bytes[14] = 0x74;
    StoreIdentityRoot healthy_root{};
    healthy_root.bytes[14] = 0x75;
    uint16_t failed_port = 0;
    const int failed_listener = loopback_listener(failed_port);
    SourceArmServerObservation failed_observation;
    std::thread failed_server([&] {
        pollfd ready{failed_listener, POLLIN, 0};
        if (::poll(&ready, 1, 5000) <= 0) {
            (void)::close(failed_listener);
            return;
        }
        sockaddr_in peer{};
        socklen_t size = sizeof(peer);
        const int fd = ::accept(failed_listener,
            reinterpret_cast<sockaddr*>(&peer), &size);
        (void)::close(failed_listener);
        serve_accepted_source_transfer(fd, peer, size, f_store_guid_for_root(failed_root),
            1, failed_observation, true);
    });
    const auto failed = runtime.transfer_source_on_owner(
        source_transfer_request(failed_port, 191, CACHE_PROFILE_P29V1),
        deadline(), local::HandoffFd(source_file("h5-failed", source)));
    failed_server.join();
    CHECK(failed_observation.ready_sent);
    CHECK(failed.code == local::SourceTransferResultCode::Error);
    CHECK(failed.attempts == 2);
    CHECK(failed.error_code != static_cast<uint16_t>(
        local::SourceTransferErrorCode::RouteReplacementRequired));

    uint16_t healthy_port = 0;
    const int healthy_listener = loopback_listener(healthy_port);
    SourceArmServerObservation healthy_observation;
    std::thread healthy_server([&] {
        pollfd ready{healthy_listener, POLLIN, 0};
        if (::poll(&ready, 1, 5000) <= 0) {
            (void)::close(healthy_listener);
            return;
        }
        serve_one_source_transfer(healthy_listener, f_store_guid_for_root(healthy_root),
                                  1, healthy_observation);
    });
    const auto healthy = runtime.transfer_source_on_owner(
        source_transfer_request(healthy_port, 201, CACHE_PROFILE_P29V1),
        deadline(), local::HandoffFd(source_file("h5-healthy", source)));
    healthy_server.join();
    CHECK(healthy.code == local::SourceTransferResultCode::Committed);
    CHECK(healthy.raw_digest == icecc::digest128(source));
    CHECK(healthy.raw_bytes == source.size());
    CHECK(healthy_observation.transfer_completed);
}

void test_route_poison_latches_before_successor_f_open() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 9;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.before_route_prepare_for_test = [] {
        throw P50RoutePoisoned("injected SidecarRuntime route poison");
    };
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot remote_root{};
    remote_root.bytes[14] = 0x53;
    const FStoreGuid remote_guid = f_store_guid_for_root(remote_root);
    uint16_t first_port = 0;
    const int first_listener = loopback_listener(first_port);
    SourceArmServerObservation first_observation;
    std::thread first_server([&] {
        serve_one_source_arm(first_listener, remote_guid, 11,
                             first_observation);
    });

    char first_source_path[] = "/tmp/p50-route-poison-first-XXXXXX";
    const int first_source_fd = ::mkstemp(first_source_path);
    CHECK(first_source_fd >= 0);
    const std::array<uint8_t, 6> first_source{'p', 'o', 'i', 's', 'o', 'n'};
    CHECK(write_all(first_source_fd, first_source));
    CHECK(::unlink(first_source_path) == 0);
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto first_deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(4),
        clock.clock_domain_id, clock.time_namespace_id);
    const local::P50SourceTransferResult first = runtime.transfer_source_on_owner(
        source_transfer_request(first_port, 91, CACHE_PROFILE_P29V1),
        first_deadline, local::HandoffFd(first_source_fd));
    first_server.join();
    CHECK(first.code == local::SourceTransferResultCode::Error);
    CHECK(first.error_code == static_cast<uint16_t>(
        local::SourceTransferErrorCode::RouteReplacementRequired));
    CHECK(first_observation.accepted && first_observation.protocol_50 &&
          first_observation.arm_received && first_observation.armed_sent &&
          first_observation.cache_session_received && first_observation.ready_sent &&
          first_observation.eof_without_cachewire);

    uint16_t successor_port = 0;
    const int successor_listener = loopback_listener(successor_port);
    const int listener_flags = ::fcntl(successor_listener, F_GETFL);
    CHECK(listener_flags >= 0);
    CHECK(::fcntl(successor_listener, F_SETFL,
                  listener_flags | O_NONBLOCK) == 0);
    char successor_source_path[] = "/tmp/p50-route-poison-successor-XXXXXX";
    const int successor_source_fd = ::mkstemp(successor_source_path);
    CHECK(successor_source_fd >= 0);
    const std::array<uint8_t, 4> successor_source{'n', 'e', 'x', 't'};
    CHECK(write_all(successor_source_fd, successor_source));
    CHECK(::unlink(successor_source_path) == 0);
    const auto successor_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(2),
            clock.clock_domain_id, clock.time_namespace_id);
    const auto started = std::chrono::steady_clock::now();
    const local::P50SourceTransferResult successor =
        runtime.transfer_source_on_owner(
            source_transfer_request(successor_port, 101,
                                    CACHE_PROFILE_ZSTD_TU),
            successor_deadline, local::HandoffFd(successor_source_fd));
    CHECK(successor.code == local::SourceTransferResultCode::Error);
    CHECK(successor.error_code == static_cast<uint16_t>(
        local::SourceTransferErrorCode::RouteReplacementRequired));
    CHECK(successor.attempts == 0);
    CHECK(std::chrono::steady_clock::now() - started <
          std::chrono::milliseconds(250));
    errno = 0;
    const int unexpected = ::accept(successor_listener, nullptr, nullptr);
    CHECK(unexpected < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    CHECK(::close(successor_listener) == 0);
}

void test_interner_fault_returns_permanent_profile_unavailable() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 9;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.p29_interner_fault_injection =
        P29InternerFaultInjection::FailOnce;
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot remote_root{};
    remote_root.bytes[14] = 0x54;
    const FStoreGuid remote_guid = f_store_guid_for_root(remote_root);
    uint16_t port = 0;
    const int listener = loopback_listener(port);
    SourceArmServerObservation observation;
    std::thread server([&] {
        serve_one_source_arm(listener, remote_guid, 12, observation);
    });

    char source_path[] = "/tmp/p50-interner-fault-XXXXXX";
    const int source_fd = ::mkstemp(source_path);
    CHECK(source_fd >= 0);
    const std::array<uint8_t, 12> source{
        'p', '2', '9', '-', 'f', 'a', 'u', 'l', 't', '\n', 'x', '\n'};
    CHECK(write_all(source_fd, source));
    CHECK(::unlink(source_path) == 0);
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(4),
        clock.clock_domain_id, clock.time_namespace_id);
    const local::P50SourceTransferResult result =
        runtime.transfer_source_on_owner(
            source_transfer_request(port, 111, CACHE_PROFILE_P29V1),
            deadline, local::HandoffFd(source_fd));
    server.join();

    CHECK(result.code == local::SourceTransferResultCode::Error);
    CHECK(result.error_code == static_cast<uint16_t>(
        local::SourceTransferErrorCode::PermanentLocalProfileUnavailable));
    CHECK(observation.accepted && observation.protocol_50 &&
          observation.arm_received && observation.armed_sent &&
          observation.cache_session_received && observation.ready_sent &&
          observation.eof_without_cachewire);
}

int connect_after_sidecar_ready(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        (void)::close(fd);
        return -1;
    }

    uint32_t wire_ready = 0;
    auto* bytes = reinterpret_cast<uint8_t*>(&wire_ready);
    size_t offset = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (offset != sizeof(wire_ready)) {
        const ssize_t received = ::recv(fd, bytes + offset,
                                        sizeof(wire_ready) - offset, MSG_DONTWAIT);
        if (received > 0) {
            offset += static_cast<size_t>(received);
            continue;
        }
        if (received == 0 ||
            (received < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            (void)::close(fd);
            return -1;
        }
        if (received < 0 && errno == EINTR)
            continue;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            (void)::close(fd);
            return -1;
        }
        const auto remaining = deadline - now;
        auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        if (milliseconds < remaining)
            ++milliseconds;
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1,
                                 static_cast<int>(milliseconds.count()));
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || (descriptor.revents & POLLNVAL) != 0 ||
            (descriptor.revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
            (void)::close(fd);
            return -1;
        }
    }
    if (ntohl(wire_ready) != CACHE_SESSION_READY_MAGIC) {
        (void)::close(fd);
        return -1;
    }
    return fd;
}

void test_runtime_zstd_tu_af_unix_loopback() {
    namespace asio = boost::asio;
    const std::vector<uint8_t> input = [] {
        std::vector<uint8_t> value;
        value.reserve(8192);
        for (size_t index = 0; index != 8192; ++index)
            value.push_back(static_cast<uint8_t>((index * 37u + index / 11u) & 0xffu));
        return value;
    }();
    std::vector<uint8_t> observed;
    service::RuntimeConfig config = test_runtime_config();
    config.endpoint_config.input_job_state = [&](CStoreGuid, const TxBegin&, const TxCommit&,
                                                   std::span<const uint8_t> exact) {
        observed.assign(exact.begin(), exact.end());
        return InputJobState::Open;
    };
    service::SidecarRuntime runtime(std::move(config));
    RuntimeCase control = authenticated_runtime_pair();
    const local::HandoffRequest request{{7, 1}, 1};
    std::thread::id first_caller_id;
    std::thread::id second_caller_id;
    std::thread::id first_owner_id;
    std::thread::id second_owner_id;
    std::atomic<bool> first_runtime_finished{false};
    std::atomic<bool> release_first_runtime{false};
    std::atomic<bool> first_commit_write_seen{false};
    EndpointIoControl first_endpoint_control;
    first_endpoint_control.before_completion_check = [&](CompletionStamp& stamp) {
        first_owner_id = std::this_thread::get_id();
        if (stamp.operation == AsyncOperationKind::WriteFragment &&
            stamp.transaction_bound && stamp.tu_seq == TuSeq{0})
            first_commit_write_seen.store(true, std::memory_order_release);
    };
    auto authority = std::make_shared<P50PreparationAuthority>(Id128::from_u64(9002));

    uint16_t port = 0;
    const int listener = loopback_listener(port);
    asio::io_context client_context;
    std::future<ClientRunResult> client_result;
    std::atomic<bool> first_ready_seen{false};
    std::thread client_thread([&] {
        P50ClientEndpoint client(authority);
        const PreparedTuHandle prepared = authority->prepare({1, 1}, input);
        const int fd = connect_after_sidecar_ready(port);
        first_ready_seen.store(fd >= 0, std::memory_order_release);
        client_result = asio::co_spawn(
            client_context, client.run_adopted_fd(fd, prepared), asio::use_future);
        client_context.run();
    });

    service::RuntimeResult runtime_result;
    std::thread runtime_thread([&] {
        first_caller_id = std::this_thread::get_id();
        runtime_result = runtime.run_one(
            control.receiver, request,
            std::chrono::steady_clock::now() + std::chrono::seconds(3),
            std::move(first_endpoint_control));
        first_runtime_finished.store(true, std::memory_order_release);
        while (!release_first_runtime.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    const int accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);
    CHECK(::close(listener) == 0);

    local::FdHandoffSender sender{local::HandoffFd(accepted)};
    const local::FdHandoffResult sender_result = sender.send(
        control.sender, request, std::chrono::steady_clock::now() + std::chrono::seconds(3));
    const auto commit_write_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(3);
    while (!first_commit_write_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < commit_write_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(first_commit_write_seen.load(std::memory_order_acquire));
    // The sidecar lifecycle observer runs in commit_materialized(), before
    // this TX_COMMIT write completion.  A real owner-queued attachment must
    // therefore succeed even while the endpoint is still finishing its
    // dialogue, rather than being surfaced as a disconnected sidecar link.
    const InputFdRequest attachment_request{
        {7, 1}, {Id128::from_u64(9002), TuSeq{0}},
        {41, 1, 2}, 901};
    const std::optional<InputCursor> attached = runtime.attach_input_on_owner(
        attachment_request, std::chrono::steady_clock::now() + std::chrono::seconds(3));
    CHECK(attached.has_value() && attached->remaining() == input.size());
    runtime.finish_input_attachment_on_owner(
        attachment_request, true,
        std::chrono::steady_clock::now() + std::chrono::seconds(3));
    // Stale, wrong-role, wrong-binding, and cross-operation cancellation
    // frames are consumed but must not affect the active dialogue.
    send_operation_cancel(control.sender, request.identity, request.request_id + 99);
    send_operation_cancel(control.sender, request.identity, request.request_id,
                          local::ControlCancelTargetRole::CSource);
    send_operation_cancel(control.sender, request.identity, request.request_id,
                          local::ControlCancelTargetRole::FSession,
                          local::ControlCancellationReason::Requested, {},
                          local::ControlOperationRole::Sidecar);
    local::ControlBindingPlaceholder wrong_binding{};
    wrong_binding[0] = 1;
    send_operation_cancel(control.sender, request.identity, request.request_id,
                          local::ControlCancelTargetRole::FSession,
                          local::ControlCancellationReason::Requested, wrong_binding);
    send_operation_cancel(control.sender, {7, 99}, request.request_id);
    const auto first_done_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!first_runtime_finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < first_done_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(first_runtime_finished.load(std::memory_order_acquire));
    client_thread.join();
    const ClientRunResult client_value = client_result.get();
    CHECK(sender_result.status == local::FdHandoffStatus::Accepted);
    CHECK(runtime_result.status == service::RuntimeStatus::Completed);
    CHECK(runtime_result.endpoint.has_value() &&
          runtime_result.endpoint->status == ServerRunStatus::Completed);
    CHECK(first_ready_seen.load(std::memory_order_acquire));
    CHECK(client_value.status == ClientRunStatus::Committed);
    CHECK(observed == input);
    CHECK(runtime.live_handoff_count() == 0 && runtime.live_session_count() == 0);

    // A single SidecarRuntime owns both dialogues.  The second authenticated
    // handoff uses the next request id and a fresh ordinary link; endpoint
    // session cleanup must leave no live registration between them.
    const std::vector<uint8_t> second_input(input.rbegin(), input.rend());
    observed.clear();
    EndpointIoControl second_endpoint_control;
    second_endpoint_control.before_completion_check = [&](CompletionStamp&) {
        second_owner_id = std::this_thread::get_id();
    };
    RuntimeCase second_control = authenticated_runtime_pair();
    auto second_authority = std::make_shared<P50PreparationAuthority>(Id128::from_u64(9012));
    uint16_t second_port = 0;
    const int second_listener = loopback_listener(second_port);
    asio::io_context second_client_context;
    std::future<ClientRunResult> second_client_result;
    std::atomic<bool> second_ready_seen{false};
    std::thread second_client_thread([&] {
        P50ClientEndpoint second_client(second_authority);
        const PreparedTuHandle prepared = second_authority->prepare({2, 2}, second_input);
        const int fd = connect_after_sidecar_ready(second_port);
        second_ready_seen.store(fd >= 0, std::memory_order_release);
        second_client_result = asio::co_spawn(second_client_context,
                                              second_client.run_adopted_fd(fd, prepared),
                                              asio::use_future);
        second_client_context.run();
    });
    service::RuntimeResult second_runtime_result;
    std::thread second_runtime_thread([&] {
        second_caller_id = std::this_thread::get_id();
        second_runtime_result = runtime.run_one(
            second_control.receiver, {{7, 1}, 2},
            std::chrono::steady_clock::now() + std::chrono::seconds(3),
            std::move(second_endpoint_control));
    });
    const int second_accepted = ::accept(second_listener, nullptr, nullptr);
    CHECK(second_accepted >= 0);
    CHECK(::close(second_listener) == 0);
    local::FdHandoffSender second_sender{local::HandoffFd(second_accepted)};
    const local::FdHandoffResult second_sender_result = second_sender.send(
        second_control.sender, {{7, 1}, 2},
        std::chrono::steady_clock::now() + std::chrono::seconds(3));
    second_runtime_thread.join();
    second_client_thread.join();
    const ClientRunResult second_client_value = second_client_result.get();
    release_first_runtime.store(true, std::memory_order_release);
    runtime_thread.join();
    CHECK(second_sender_result.status == local::FdHandoffStatus::Accepted);
    CHECK(second_runtime_result.handoff.status == local::FdHandoffStatus::Accepted);
    CHECK(second_runtime_result.status == service::RuntimeStatus::Completed);
    CHECK(second_runtime_result.endpoint.has_value() &&
          second_runtime_result.endpoint->status == ServerRunStatus::Completed);
    CHECK(second_ready_seen.load(std::memory_order_acquire));
    CHECK(second_client_value.status == ClientRunStatus::Committed);
    CHECK(observed == second_input);
    CHECK(runtime.live_handoff_count() == 0 && runtime.live_session_count() == 0);
    CHECK(first_caller_id != second_caller_id);
    CHECK(first_owner_id == second_owner_id);
    CHECK(first_owner_id != first_caller_id);
    CHECK(second_owner_id != second_caller_id);
}

void test_operation_cancel_commit_race_preserves_witness() {
    namespace asio = boost::asio;
    std::vector<uint8_t> input(8192);
    for (size_t index = 0; index != input.size(); ++index)
        input[index] = static_cast<uint8_t>((index * 17u + 3u) & 0xffu);
    service::RuntimeConfig config = test_runtime_config();
    config.endpoint_config.input_job_state = [](CStoreGuid, const TxBegin&, const TxCommit&,
                                                std::span<const uint8_t>) {
        return InputJobState::Open;
    };
    service::SidecarRuntime runtime(std::move(config));
    RuntimeCase control = authenticated_runtime_pair();
    const local::HandoffRequest request{{7, 1}, 17};
    auto authority = std::make_shared<P50PreparationAuthority>(Id128::from_u64(9123));
    uint16_t port = 0;
    const int listener = loopback_listener(port);
    asio::io_context client_context;
    std::future<ClientRunResult> client_result;
    std::atomic<bool> commit_check_seen{false};
    std::atomic<bool> release_commit_check{false};
    EndpointIoControl endpoint_control;
    endpoint_control.before_completion_check = [&](CompletionStamp& stamp) {
        if (stamp.operation != AsyncOperationKind::WriteFragment ||
            !stamp.transaction_bound)
            return;
        commit_check_seen.store(true, std::memory_order_release);
        while (!release_commit_check.load(std::memory_order_acquire))
            std::this_thread::yield();
    };
    std::thread client_thread([&] {
        P50ClientEndpoint client(authority);
        const PreparedTuHandle prepared = authority->prepare({17, 1}, input);
        const int fd = connect_after_sidecar_ready(port);
        CHECK(fd >= 0);
        client_result = asio::co_spawn(client_context,
                                       client.run_adopted_fd(fd, prepared), asio::use_future);
        client_context.run();
    });
    service::RuntimeResult runtime_result;
    std::thread runtime_thread([&] {
        runtime_result = runtime.run_one(
            control.receiver, request,
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            std::move(endpoint_control));
    });
    const int accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);
    CHECK(::close(listener) == 0);
    local::FdHandoffSender sender{local::HandoffFd(accepted)};
    CHECK(sender.send(control.sender, request,
                      std::chrono::steady_clock::now() + std::chrono::seconds(3))
              .status == local::FdHandoffStatus::Accepted);
    const auto commit_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!commit_check_seen.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < commit_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(commit_check_seen.load(std::memory_order_acquire));
    send_operation_cancel(control.sender, request.identity, request.request_id);
    release_commit_check.store(true, std::memory_order_release);
    runtime_thread.join();
    client_thread.join();
    const ClientRunResult client_value = client_result.get();
    CHECK(runtime_result.status == service::RuntimeStatus::Cancelled &&
          runtime_result.cancellation == service::RuntimeCancellationReason::Requested);
    CHECK(runtime_result.endpoint.has_value() &&
          runtime_result.endpoint->committed_input.has_value() &&
          runtime_result.endpoint->completed_input.has_value());
    CHECK(client_value.status == ClientRunStatus::Committed &&
          client_value.cancellation == ClientCancellationDisposition::None &&
          client_value.committed_commit.has_value() &&
          client_value.committed_input.has_value());
    CHECK(client_value.committed_input == runtime_result.endpoint->committed_input &&
          client_value.committed_input == runtime_result.endpoint->completed_input);
}

void test_runtime_interner_poison_preserves_active_commit() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x7a;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.p29_interner_fault_injection = P29InternerFaultInjection::FailOnce;
    config.source_open_arm_timeout = std::chrono::milliseconds(500);
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot active_root{};
    active_root.bytes[15] = 0x7b;
    const FStoreGuid active_f_guid = f_store_guid_for_root(active_root);
    StoreIdentityRoot poison_root{};
    poison_root.bytes[15] = 0x7c;
    const FStoreGuid poison_f_guid = f_store_guid_for_root(poison_root);
    uint16_t active_port = 0;
    const int active_listener = loopback_listener(active_port);
    uint16_t poison_port = 0;
    const int poison_listener = loopback_listener(poison_port);
    uint16_t refused_port = 0;
    const int refused_listener = loopback_listener(refused_port);
    SourceArmServerObservation active_observation;
    SourceArmServerObservation poison_observation;
    SourceCommitHold hold_active_commit;
    std::thread active_server([&] {
        pollfd ready{active_listener, POLLIN, 0};
        if (::poll(&ready, 1, 3000) <= 0) {
            (void)::close(active_listener);
            return;
        }
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int accepted = ::accept(
            active_listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        (void)::close(active_listener);
        serve_accepted_source_transfer(
            accepted, peer, peer_size, active_f_guid, 17,
            active_observation, false, nullptr, nullptr, &hold_active_commit);
    });
    std::thread poison_server([&] {
        pollfd ready{poison_listener, POLLIN, 0};
        if (::poll(&ready, 1, 3000) <= 0) {
            (void)::close(poison_listener);
            return;
        }
        serve_one_source_arm(poison_listener, poison_f_guid, 17,
                             poison_observation);
    });

    const std::vector<uint8_t> active_source{
        'a', 'c', 't', 'i', 'v', 'e', '-', 'c', '-', 'w', 'i', 't', 'n', 'e', 's', 's', '\n'};
    const std::vector<uint8_t> poison_source{
        '#', ' ', '1', ' ', '"', 'p', 'o', 'i', 's', 'o', 'n', '.', 'h', '"', '\n',
        'p', 'o', 'i', 's', 'o', 'n', '-', 'r', 'e', 'g', 'i', 'o', 'n', '\n'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(5),
        clock.clock_domain_id, clock.time_namespace_id);
    local::P50SourceTransferResult active_result;
    std::thread active_transfer([&] {
        active_result = runtime.transfer_source_on_owner(
            source_transfer_request(active_port, 860, CACHE_PROFILE_ZSTD_TU),
            deadline, local::HandoffFd(source_file("p50-runtime-active", active_source)));
    });
    const bool active_commit_held =
        hold_active_commit.wait_until_entered(std::chrono::seconds(2));

    // This is the actual injected C-interner FailOnce path, distinct from the
    // typed route-poison test seam: it terminalizes P29 preparation after a
    // different profile already owns a live exact input/commit operation.
    const auto poison_result = runtime.transfer_source_on_owner(
        source_transfer_request(poison_port, 861, CACHE_PROFILE_P29V1),
        deadline, local::HandoffFd(source_file("p50-runtime-poison", poison_source)));
    const bool poison_was_permanent =
        poison_result.code == local::SourceTransferResultCode::Error &&
        poison_result.error_code == static_cast<uint16_t>(
            local::SourceTransferErrorCode::PermanentLocalProfileUnavailable);

    const auto retry_result = runtime.transfer_source_on_owner(
        source_transfer_request(poison_port, 861, CACHE_PROFILE_P29V1),
        deadline, local::HandoffFd(source_file("p50-runtime-poison-retry", poison_source)));
    const bool retry_required_replacement =
        retry_result.code == local::SourceTransferResultCode::Error &&
        retry_result.error_code == static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired);
    const auto refused_result = runtime.transfer_source_on_owner(
        source_transfer_request(refused_port, 862, CACHE_PROFILE_ZSTD_ROUTE),
        deadline, local::HandoffFd(source_file("p50-runtime-refused", poison_source)));
    const bool new_route_required_replacement =
        refused_result.code == local::SourceTransferResultCode::Error &&
        refused_result.error_code == static_cast<uint16_t>(
            local::SourceTransferErrorCode::RouteReplacementRequired);
    pollfd refused_connection{};
    refused_connection.fd = refused_listener;
    refused_connection.events = POLLIN;
    const bool new_route_was_not_opened =
        ::poll(&refused_connection, 1, 0) == 0;

    // Terminal preparation state fences new work but cannot erase the active
    // route's F-side exact commit witness. Settle and join it before teardown.
    hold_active_commit.release();
    active_transfer.join();
    active_server.join();
    poison_server.join();
    const bool poison_f_saw_only_open_handshake =
        poison_observation.arm_received && poison_observation.armed_sent &&
        poison_observation.cache_session_received && poison_observation.ready_sent &&
        poison_observation.eof_without_cachewire;
    const bool active_exact_commit_witness =
        active_result.code == local::SourceTransferResultCode::Committed &&
        active_result.tu_seq == 0 &&
        active_result.raw_bytes == active_source.size() &&
        active_result.raw_digest == icecc::digest128(active_source) &&
        active_observation.transfer_completed &&
        active_observation.committed_input == active_source &&
        active_observation.commit_identity_matches_input;
    CHECK(active_commit_held);
    CHECK(poison_was_permanent);
    CHECK(retry_required_replacement);
    CHECK(new_route_required_replacement);
    CHECK(new_route_was_not_opened);
    CHECK(poison_f_saw_only_open_handshake);
    CHECK(active_exact_commit_witness);
    CHECK(::close(refused_listener) == 0);
}

[[noreturn]] void run_active_source_stop_fail_stop_child(int marker_fd) {
    auto child_abort = [] { _exit(126); };
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x7e;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    CHECK(config.cancellation_grace == std::chrono::milliseconds(100));
    CodecWorkerGate held_c_prepare;
    config.before_route_prepare_for_test = [&held_c_prepare] {
        held_c_prepare.enter_and_wait();
    };
    config.fail_stop = [marker_fd] {
        const uint8_t byte = 'S';
        if (::write(marker_fd, &byte, sizeof(byte)) !=
            static_cast<ssize_t>(sizeof(byte)))
            _exit(127);
    };
    service::SidecarRuntime runtime(std::move(config));

    StoreIdentityRoot active_root{};
    active_root.bytes[15] = 0x7f;
    const FStoreGuid active_f_guid = f_store_guid_for_root(active_root);
    uint16_t active_port = 0;
    const int active_listener = loopback_listener(active_port);
    SourceArmServerObservation active_observation;
    std::thread active_server([&] {
        serve_one_source_arm(active_listener, active_f_guid, 18,
                             active_observation);
    });
    const std::vector<uint8_t> active_source{
        's', 't', 'o', 'p', '-', 'a', 'c', 't', 'i', 'v', 'e', '\n'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(10),
        clock.clock_domain_id, clock.time_namespace_id);
    std::thread active_transfer([&] {
        (void)runtime.transfer_source_on_owner(
            source_transfer_request(active_port, 863, CACHE_PROFILE_ZSTD_TU),
            deadline, local::HandoffFd(source_file("p50-runtime-stop", active_source)));
    });
    if (!held_c_prepare.wait_for_arrivals(1, std::chrono::seconds(3)))
        child_abort();
    const uint8_t ready = 'R';
    if (::write(marker_fd, &ready, sizeof(ready)) != sizeof(ready))
        child_abort();
    // The test-only C route-owner callback remains held across stop. A fresh
    // exec child avoids inheriting a dead static codec-pool thread after fork.
    runtime.stop();
    for (;;)
        (void)::pause();
}

void test_runtime_active_source_stop_fail_stops_bounded() {
    int marker[2] = {-1, -1};
    CHECK(::pipe(marker) == 0);
    const pid_t child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)::close(marker[0]);
        const std::string marker_arg = std::to_string(marker[1]);
        ::execl("/proc/self/exe", "p50cacheservice",
                "--p50-runtime-active-source-stop-child",
                marker_arg.c_str(), static_cast<char*>(nullptr));
        _exit(126);
    }

    (void)::close(marker[1]);
    ReapOnFailure reaper(child, {&marker[0], nullptr, nullptr, nullptr, nullptr});
    struct pollfd marker_ready{marker[0], POLLIN | POLLHUP, 0};
    CHECK(::poll(&marker_ready, 1, 4000) > 0);
    uint8_t marker_byte = 0;
    CHECK(::read(marker[0], &marker_byte, sizeof(marker_byte)) == 1);
    CHECK(marker_byte == 'R');
    const auto stop_requested_at = std::chrono::steady_clock::now();
    marker_ready.revents = 0;
    CHECK(::poll(&marker_ready, 1, 1500) > 0);
    CHECK(::read(marker[0], &marker_byte, sizeof(marker_byte)) == 1);
    CHECK(marker_byte == 'S');
    int status = 0;
    CHECK(wait_for_exit_bounded(child, 1500, status));
    reaper.pid = -1;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 125);
    CHECK(std::chrono::steady_clock::now() - stop_requested_at <
          std::chrono::milliseconds(1200));
    CHECK(::close(marker[0]) == 0);
    reaper.descriptors[0] = nullptr;
}

void test_runtime_stop_bounds_opening_source_arm() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x80;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.source_open_arm_timeout = std::chrono::milliseconds(350);
    config.max_active_source_transfers = 1;
    service::SidecarRuntime runtime(std::move(config));

    uint16_t open_port = 0;
    const int open_listener = loopback_listener(open_port);
    uint16_t queued_port = 0;
    const int queued_listener = loopback_listener(queued_port);
    std::atomic<bool> arm_received{false};
    std::promise<void> release_server_promise;
    const auto release_server = release_server_promise.get_future().share();
    std::thread open_server([&] {
        serve_stalled_source_arm(open_listener, arm_received, release_server);
    });

    const std::vector<uint8_t> source{'o', 'p', 'e', 'n', '-', 's', 't', 'o', 'p', '\n'};
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto deadline = sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
        std::chrono::steady_clock::now() + std::chrono::seconds(3),
        clock.clock_domain_id, clock.time_namespace_id);
    std::promise<local::P50SourceTransferResult> completion;
    auto result = completion.get_future();
    std::thread opening_transfer([&] {
        completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(open_port, 864, CACHE_PROFILE_ZSTD_TU),
            deadline, local::HandoffFd(source_file("p50-runtime-open-stop", source))));
    });

    // The F has received SOURCE_ARM, proving C has admitted and reserved the
    // opening operation, but deliberately withholds SOURCE_ARMED. Stop must
    // bound this non-cancellable initial handshake by source_open_arm_timeout.
    const auto arm_deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
    while (!arm_received.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < arm_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const bool opening_barrier_reached =
        arm_received.load(std::memory_order_acquire);
    if (!opening_barrier_reached) {
        // Do not start the queued operation unless the opening operation has
        // demonstrably consumed the sole setup slot. Stop the transfer and
        // issue a wake connection so either accept() or the ordinary-frame
        // read in the fixture server is released before reporting failure.
        release_server_promise.set_value();
        runtime.stop();
        opening_transfer.join();
        const int wake_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (wake_fd >= 0) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(open_port);
            (void)::connect(wake_fd, reinterpret_cast<const sockaddr*>(&address),
                            sizeof(address));
            (void)::close(wake_fd);
        }
        open_server.join();
        (void)::close(queued_listener);
        CHECK(opening_barrier_reached);
    }

    const std::vector<uint8_t> queued_source{'q', 'u', 'e', 'u', 'e', 'd', '\n'};
    std::promise<void> queued_call_started_promise;
    auto queued_call_started = queued_call_started_promise.get_future();
    std::promise<local::P50SourceTransferResult> queued_completion;
    auto queued_result = queued_completion.get_future();
    std::thread queued_transfer([&] {
        queued_call_started_promise.set_value();
        queued_completion.set_value(runtime.transfer_source_on_owner(
            source_transfer_request(queued_port, 865, CACHE_PROFILE_ZSTD_ROUTE),
            deadline, local::HandoffFd(source_file("p50-runtime-queued-stop", queued_source))));
    });

    const bool queued_call_started_in_time =
        queued_call_started.wait_for(std::chrono::seconds(1)) ==
        std::future_status::ready;
    pollfd queued_connection{};
    queued_connection.fd = queued_listener;
    queued_connection.events = POLLIN;
    const bool queued_request_did_not_open_f =
        ::poll(&queued_connection, 1, 0) == 0;
    const auto stopped_at = std::chrono::steady_clock::now();
    runtime.stop();
    const bool opening_rejected_within_bound =
        result.wait_for(std::chrono::milliseconds(700)) ==
        std::future_status::ready;
    const auto opening_elapsed = std::chrono::steady_clock::now() - stopped_at;
    const bool queued_rejected_within_bound =
        queued_result.wait_for(std::chrono::milliseconds(700)) ==
        std::future_status::ready;
    const auto queued_elapsed = std::chrono::steady_clock::now() - stopped_at;
    // If a broken implementation failed to settle, close the held peer so the
    // test reports a bounded assertion rather than stranding its server thread.
    if (!opening_rejected_within_bound)
        release_server_promise.set_value();
    opening_transfer.join();
    queued_transfer.join();
    if (opening_rejected_within_bound)
        release_server_promise.set_value();
    open_server.join();
    const auto value = result.get();
    const auto queued_value = queued_result.get();
    const bool opening_rejected =
        value.code == local::SourceTransferResultCode::Error;
    const bool queued_rejected =
        queued_value.code == local::SourceTransferResultCode::Error &&
        queued_value.error_code == 7;
    CHECK(opening_barrier_reached);
    CHECK(queued_call_started_in_time);
    CHECK(queued_request_did_not_open_f);
    CHECK(opening_rejected_within_bound);
    CHECK(queued_rejected_within_bound);
    CHECK(opening_rejected);
    CHECK(queued_rejected);
    CHECK(opening_elapsed < std::chrono::milliseconds(700));
    CHECK(queued_elapsed < std::chrono::milliseconds(700));
    CHECK(::close(queued_listener) == 0);
}

void test_runtime_stop_interrupts_active_endpoint() {
    service::SidecarRuntime runtime(test_runtime_config());
    RuntimeCase control = authenticated_runtime_pair();
    uint16_t port = 0;
    const int listener = loopback_listener(port);
    const int peer = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(peer >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    CHECK(::connect(peer, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    const int accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);
    CHECK(::close(listener) == 0);
    service::RuntimeResult runtime_result;
    std::thread worker([&] {
        runtime_result = runtime.run_one(
            control.receiver, {{7, 1}, 1}, std::chrono::steady_clock::now() + std::chrono::seconds(30));
    });
    local::FdHandoffSender sender{local::HandoffFd(accepted)};
    CHECK(sender.send(control.sender, {{7, 1}, 1},
                                    std::chrono::steady_clock::now() + std::chrono::seconds(3))
              .status == local::FdHandoffStatus::Accepted);
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (runtime.live_session_count() == 0 && std::chrono::steady_clock::now() < wait_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(runtime.live_session_count() == 1);
    const auto started = std::chrono::steady_clock::now();
    runtime.stop();
    worker.join();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    CHECK(runtime_result.status == service::RuntimeStatus::Stopped);
    CHECK(runtime.live_handoff_count() == 0 && runtime.live_session_count() == 0);
    CHECK(::close(peer) == 0);
}

bool wait_for_socket_node(const std::string& path, int timeout_milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_milliseconds);
    struct stat info{};
    while (std::chrono::steady_clock::now() < deadline) {
        if (::lstat(path.c_str(), &info) == 0)
            return S_ISSOCK(info.st_mode);
        if (errno != ENOENT)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool wait_for_exit_bounded(pid_t pid, int timeout_milliseconds, int& status) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_milliseconds);
    for (;;) {
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid)
            return true;
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0 || std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    (void)::kill(pid, SIGKILL);
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return false;
}

void ready_reader_close_after_bind_is_fail_closed() {
    char template_path[] = "/tmp/icecc-cache-service-ready-close-XXXXXX";
    const int directory_fd = ::mkstemp(template_path);
    CHECK(directory_fd >= 0);
    CHECK(::close(directory_fd) == 0);
    CHECK(::unlink(template_path) == 0);
    CHECK(::mkdir(template_path, 0700) == 0);

    int ready[2] = {-1, -1};
    int gate[2] = {-1, -1};
    int acknowledgement[2] = {-1, -1};
    CHECK(::pipe(ready) == 0);
    CHECK(::pipe(gate) == 0);
    CHECK(::pipe(acknowledgement) == 0);
    const std::string socket = std::string(template_path) + "/service.sock";
    const std::string ready_fd = std::to_string(ready[1]);
    const std::string gate_fd = std::to_string(gate[0]);
    const std::string acknowledgement_fd = std::to_string(acknowledgement[1]);
    const std::string shim = ready_close_shim_path();
    const std::string executable = service_path();
    const std::string uid = std::to_string(static_cast<uint64_t>(::getuid()));
    const std::string gid = std::to_string(static_cast<uint64_t>(::getgid()));
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        (void)::close(ready[0]);
        (void)::close(gate[1]);
        (void)::close(acknowledgement[0]);
        (void)::setenv("ICECC_CACHE_SERVICE_READY_FD", ready_fd.c_str(), 1);
        (void)::setenv("ICECC_TEST_READY_WRITE_FD", ready_fd.c_str(), 1);
        (void)::setenv("ICECC_TEST_READY_GATE_FD", gate_fd.c_str(), 1);
        (void)::setenv("ICECC_TEST_READY_ACK_FD", acknowledgement_fd.c_str(), 1);
        (void)::setenv("LD_PRELOAD", shim.c_str(), 1);
        ::execl(executable.c_str(), executable.c_str(), "--socket", socket.c_str(),
                "--peer-uid", uid.c_str(), "--peer-gid", gid.c_str(), "--generation", "7",
                "--attempt", "1", static_cast<char*>(nullptr));
        _exit(127);
    }

    (void)::close(ready[1]);
    ready[1] = -1;
    (void)::close(gate[0]);
    gate[0] = -1;
    (void)::close(acknowledgement[1]);
    acknowledgement[1] = -1;
    ReapOnFailure cleanup{pid, {&ready[0], &ready[1], &gate[0], &gate[1],
                               &acknowledgement[0]}};

    // Wait for the explicit pre-READY gate using the same startup allowance
    // as the other subprocess tests, rather than timing process startup with
    // a filesystem poll. The gate holds the child after bind/listen and before
    // write(READY); closing the reader below deterministically produces EPIPE.
    struct pollfd gate_seen{acknowledgement[0], POLLIN | POLLHUP, 0};
    CHECK(::poll(&gate_seen, 1, kStartupReadyTimeoutMilliseconds) > 0);
    char gate_ack = 0;
    CHECK(::read(acknowledgement[0], &gate_ack, 1) == 1 && gate_ack == 1);
    CHECK(wait_for_socket_node(socket, 2000));
    CHECK(::close(acknowledgement[0]) == 0);
    acknowledgement[0] = -1;
    CHECK(::close(ready[0]) == 0);
    ready[0] = -1;
    const uint8_t release = 1;
    CHECK(write_all(gate[1], std::span<const uint8_t>(&release, 1)));
    CHECK(::close(gate[1]) == 0);
    gate[1] = -1;

    int status = 0;
    CHECK(wait_for_exit_bounded(pid, 2000, status));
    cleanup.pid = -1;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 2);
    CHECK(::kill(pid, 0) < 0 && errno == ESRCH);
    CHECK(::access(socket.c_str(), F_OK) != 0);
    CHECK(::rmdir(template_path) == 0);
}

void ready_requires_bind_and_replacement_is_preserved() {
    char template_path[] = "/tmp/icecc-cache-service-test-XXXXXX";
    const int directory_fd = ::mkstemp(template_path);
    CHECK(directory_fd >= 0);
    CHECK(::close(directory_fd) == 0);
    CHECK(::unlink(template_path) == 0);
    CHECK(::mkdir(template_path, 0755) == 0);
    int ready[2] = {-1, -1};
    CHECK(::pipe(ready) == 0);
    const std::string ready_fd = std::to_string(ready[1]);
    const std::string socket = std::string(template_path) + "/service.sock";
    CHECK(::setenv("ICECC_CACHE_SERVICE_READY_FD", ready_fd.c_str(), 1) == 0);
    const std::string executable = service_path();
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        (void)::close(ready[0]);
        ::execl(executable.c_str(), executable.c_str(), "--socket", socket.c_str(),
                "--peer-uid", std::to_string(static_cast<uint64_t>(::getuid())).c_str(),
                "--peer-gid", std::to_string(static_cast<uint64_t>(::getgid())).c_str(),
                "--generation", "7", "--attempt", "1", static_cast<char*>(nullptr));
        _exit(127);
    }
    (void)::close(ready[1]);
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    char byte = 0;
    CHECK(::read(ready[0], &byte, 1) == 0);
    (void)::close(ready[0]);
    CHECK(::rmdir(template_path) == 0);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 &&
            std::strcmp(argv[1],
                        "--p50-runtime-active-source-stop-child") == 0) {
            char* end = nullptr;
            errno = 0;
            const long marker_fd = std::strtol(argv[2], &end, 10);
            CHECK(errno == 0 && end != argv[2] && *end == '\0' &&
                  marker_fd >= 0 && marker_fd <= INT32_MAX);
            run_active_source_stop_fail_stop_child(
                static_cast<int>(marker_fd));
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--a05-global-gate-negative-control") == 0) {
            // A one-credit cap deliberately recreates whole-process admission
            // serialization.  The ordinary A05 progress assertion must fail
            // by name after both fixture threads have been released/joined.
            test_stalled_f_arm_is_bounded_before_healthy_transfer(
                CACHE_PROFILE_P29V1, 1);
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--transport-isolation") == 0) {
            test_transport_loss_preserves_other_worker_service();
            test_route_poison_latches_before_successor_f_open();
            test_route_endpoint_cap_refuses_before_f_open();
            test_known_endpoint_relationship_cap_refuses_before_f_open();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--replacement-trigger-latch") == 0) {
            test_replacement_trigger_latches_once_and_is_opt_in();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--same-f-real-transfer") == 0) {
            test_p51_same_f_missing_real_sender_transfer_keeps_sibling();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--same-f-established-reconnect") == 0) {
            test_p51_same_f_missing_real_sender_transfer_keeps_sibling(true);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--r2-trace-two-c-interleaved") == 0) {
            test_p51_same_f_missing_real_sender_transfer_keeps_sibling(
                false, true);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--aggregate-fit-exact") == 0) {
            test_p51_aggregate_raw_budget_fitting_commit_is_exact();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--p51-d12-oversize-fit-credit-cancel") == 0) {
            for (const ProfileId profile : {
                     ProfileId::P29V1, ProfileId::ZSTD_TU,
                     ProfileId::ZSTD_ROUTE})
                test_p51_aggregate_raw_budget_fitting_commit_is_exact(profile);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--p51-reservation-expiry-before-bind") == 0) {
            test_p51_reservation_expiry_before_bind_no_late_arm();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1],
                        "--p51-interrupted-reservation-isolation") == 0) {
            test_p51_interrupted_reservation_relationship_isolation();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1],
                        "--p51-interrupted-reservation-stale-link") == 0) {
            test_p51_interrupted_reservation_relationship_isolation(true);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--read-peer-close") == 0) {
            test_p51_peer_close_during_active_read_cancels_before_route();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--source-fd-read-failure") == 0) {
            test_p51_read_failure_releases_original_source_fd();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-active-cancel-recovery-probe") == 0) {
            test_p51_d07_active_cancel_all_profiles();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-active-cancel-replay-interrupt-p29") == 0) {
            test_p51_d07_active_cancel_recovery(
                ProfileId::P29V1, D07Scenario::InterruptedReplay);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-positive-recovery-owner-p29") == 0) {
            test_p51_d07_active_cancel_recovery(
                ProfileId::P29V1, D07Scenario::PositiveRecoveryOwner);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-positive-recovery-owner") == 0) {
            test_p51_d07_positive_recovery_owner_all_profiles();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d14-reset-boundary-smoke-zstd-tu") == 0) {
            test_p51_d14_reset_boundary_smoke(ProfileId::ZSTD_TU);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d14-reset-boundary-smoke-zstd-route") == 0) {
            test_p51_d14_reset_boundary_smoke(ProfileId::ZSTD_ROUTE);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d14-w30-k0-zstd-tu") == 0) {
            test_p51_d14_reset_boundary_smoke(ProfileId::ZSTD_TU, 31, 0, true);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d14-w30-k0-all-profiles") == 0) {
            test_p51_d14_w30_k0_all_profiles();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d14-w30-boundary-matrix") == 0) {
            test_p51_d14_w30_reset_boundary_matrix();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d14-w30-terminal-all-profiles") == 0) {
            test_p51_d14_w30_terminal_all_profiles();
            return 0;
        }
        if (argc == 4 &&
            std::strcmp(argv[1], "--d14-w30-boundary") == 0) {
            char* profile_end = nullptr;
            char* boundary_end = nullptr;
            const unsigned long profile_value =
                std::strtoul(argv[2], &profile_end, 10);
            const unsigned long boundary_value =
                std::strtoul(argv[3], &boundary_end, 10);
            CHECK(profile_end != argv[2] && *profile_end == '\0');
            CHECK(boundary_end != argv[3] && *boundary_end == '\0');
            CHECK(profile_value >= 1 && profile_value <= 3);
            CHECK(boundary_value <= 30);
            test_p51_d14_reset_boundary_smoke(
                static_cast<ProfileId>(profile_value), 31,
                static_cast<size_t>(boundary_value), true);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d14-reset-boundary-smoke") == 0) {
            test_p51_d14_reset_boundary_smoke_all_profiles();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-active-cancel-replay-interrupt") == 0) {
            test_p51_d07_active_cancel_replay_interrupt_all_profiles();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-committed-attempt-replacement") == 0) {
            test_p51_d07_committed_attempt_replacement_all_profiles();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-queued-cancel-first") == 0) {
            test_p51_d07_queued_cancel_position(0);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-queued-cancel-middle") == 0) {
            test_p51_d07_queued_cancel_position(15);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-queued-cancel-last") == 0) {
            test_p51_d07_queued_cancel_position(30);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-queued-cancel") == 0) {
            test_p51_d07_queued_cancel_first_middle_last();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d07-staged-cancel") == 0) {
            test_p51_d07_staged_cancel_all_profiles();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d17-window-cancel-zstd-tu") == 0) {
            test_p51_d17_repeated_window_cancel(ProfileId::ZSTD_TU, 1);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d17-repeated-window-cancel") == 0) {
            for (const ProfileId profile : {
                     ProfileId::P29V1, ProfileId::ZSTD_TU,
                     ProfileId::ZSTD_ROUTE})
                test_p51_d17_repeated_window_cancel(profile, 3);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--p51-reservation-capacity-120-timer-expiry") == 0) {
            test_p51_reservation_capacity_120_cancel_and_expiry();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d11-real-receipt-ledger-zstd-tu-w1") == 0) {
            test_p51_d11_real_receipt_ledger(ProfileId::ZSTD_TU, 1);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d11-real-receipt-ledger") == 0) {
            for (const ProfileId profile : {
                     ProfileId::P29V1, ProfileId::ZSTD_TU,
                     ProfileId::ZSTD_ROUTE}) {
                test_p51_d11_real_receipt_ledger(profile, 1);
                test_p51_d11_real_receipt_ledger(profile, 30);
            }
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d11-real-f-output-byte-cap") == 0) {
            for (const ProfileId profile : {
                     ProfileId::P29V1, ProfileId::ZSTD_TU,
                     ProfileId::ZSTD_ROUTE}) {
                const char* name = profile == ProfileId::P29V1 ? "P29V1" :
                    profile == ProfileId::ZSTD_TU ? "ZSTD_TU" : "ZSTD_ROUTE";
                std::fprintf(stderr,
                             "P51_D11_OUTPUT_CAP_START profile=%s window=1\n",
                             name);
                std::fflush(stderr);
                test_p51_d11_real_f_output_byte_cap(profile, 1);
                std::fprintf(stderr,
                             "P51_D11_OUTPUT_CAP_START profile=%s window=30\n",
                             name);
                std::fflush(stderr);
                test_p51_d11_real_f_output_byte_cap(profile, 30);
            }
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d11-real-f-output-byte-cap-expiry") == 0) {
            test_p51_d11_real_f_output_byte_cap(ProfileId::P29V1, 1, true);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d11-real-r2-pending-encoded-cap-zstd-tu-w1") == 0) {
            test_p51_d11_real_r2_pending_budget(
                D11PendingBudgetKind::Encoded, 1);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d11-real-r2-pending-encoded-cap") == 0) {
            for (const uint32_t window : {1U, 30U}) {
                test_p51_d11_real_r2_pending_budget(
                    D11PendingBudgetKind::Encoded, window);
            }
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d11-real-r2-pending-raw-cap-zstd-tu-w1") == 0) {
            test_p51_d11_real_r2_pending_budget(
                D11PendingBudgetKind::Raw, 1);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d11-real-r2-pending-window-cap-zstd-tu-w1") == 0) {
            test_p51_d11_real_r2_pending_budget(
                D11PendingBudgetKind::DecoderWindow, 1);
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--d11-real-r2-pending-raw-window-cap") == 0) {
            for (const uint32_t window : {1U, 30U}) {
                test_p51_d11_real_r2_pending_budget(
                    D11PendingBudgetKind::Raw, window);
                test_p51_d11_real_r2_pending_budget(
                    D11PendingBudgetKind::DecoderWindow, window);
            }
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--credit-admission-hol-witness") == 0) {
            test_p51_credit_admission_bypasses_blocked_workers();
            return 0;
        }
        if (argc == 2 &&
            std::strcmp(argv[1], "--credit-admission-bypass-limit") == 0) {
            test_p51_credit_admission_bypass_limit_serves_oldest();
            return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "--p51-capacity-overflow") == 0) {
            test_p51_async_transfer_reply_deadline_close_and_slot_reuse();
            return 0;
        }
        CHECK(argc == 1);
        exercise_root_contract_then_drop_test_process();
        fingerprint_stop_before_ready_is_bounded();
        legacy_store_identity_launches();
        signal_interrupts_control_wait(SIGTERM);
        signal_interrupts_control_wait(SIGINT);
        authenticated_idle_dispatcher_persists();
        authenticated_control_farm_accepts_twenty_and_stops();
        replacement_node_is_not_removed();
        peer_credentials_are_required();
        rejects_identity_role_and_malformed();
        slowloris_deadline_is_total_and_listener_recovers();
        frame_header_and_payload_share_one_deadline();
        test_runtime_store_identity_is_explicit_and_role_tagged();
        test_source_open_arm_timeout_bounds();
        test_p51_reservation_capacity_identity_and_window();
        test_p51_async_transfer_reply_deadline_close_and_slot_reuse();
        test_p51_admitted_transfer_stop_releases_raw_credit();
        test_p51_peer_close_during_active_read_cancels_before_route();
        test_p51_read_failure_releases_original_source_fd();
        test_p51_d07_queued_cancel_first_middle_last();
        test_p51_d07_staged_cancel_all_profiles();
        test_p51_d07_active_cancel_all_profiles();
        test_p51_d07_active_cancel_replay_interrupt_all_profiles();
        test_p51_d07_positive_recovery_owner_all_profiles();
        test_p51_d07_committed_attempt_replacement_all_profiles();
        test_p51_d14_w30_k0_all_profiles();
        test_p51_d14_w30_reset_boundary_matrix();
        test_p51_d14_w30_terminal_all_profiles();
        for (const ProfileId profile : {
                 ProfileId::P29V1, ProfileId::ZSTD_TU,
                 ProfileId::ZSTD_ROUTE})
            test_p51_d17_repeated_window_cancel(profile, 3);
        test_p51_aggregate_raw_budget_oversize_fit_and_stop_cleanup();
        for (const ProfileId profile : {
                 ProfileId::P29V1, ProfileId::ZSTD_TU,
                 ProfileId::ZSTD_ROUTE})
            test_p51_aggregate_raw_budget_fitting_commit_is_exact(profile);
        test_p51_credit_admission_bypasses_blocked_workers();
        test_p51_credit_admission_bypass_limit_serves_oldest();
        test_p51_stop_while_waiting_for_link_session_echo();
        test_p51_stop_while_waiting_for_link_state();
        test_p51_reservation_capacity_120_cancel_and_expiry();
        for (const ProfileId profile : {
                 ProfileId::P29V1, ProfileId::ZSTD_TU,
                 ProfileId::ZSTD_ROUTE}) {
            test_p51_d11_real_receipt_ledger(profile, 1);
            test_p51_d11_real_receipt_ledger(profile, 30);
        }
        for (const ProfileId profile : {
                 ProfileId::P29V1, ProfileId::ZSTD_TU,
                 ProfileId::ZSTD_ROUTE}) {
            test_p51_d11_real_f_output_byte_cap(profile, 1);
            test_p51_d11_real_f_output_byte_cap(profile, 30);
        }
        test_p51_d11_real_f_output_byte_cap(ProfileId::P29V1, 1, true);
        for (const uint32_t window : {1U, 30U}) {
            test_p51_d11_real_r2_pending_budget(
                D11PendingBudgetKind::Encoded, window);
            test_p51_d11_real_r2_pending_budget(
                D11PendingBudgetKind::Raw, window);
            test_p51_d11_real_r2_pending_budget(
                D11PendingBudgetKind::DecoderWindow, window);
        }
        test_p51_cancel_publication_and_reset_lifecycle();
        test_p51_interrupted_reservation_relationship_isolation();
        test_p51_interrupted_reservation_relationship_isolation(true);
        test_p51_reservation_expiry_before_bind_no_late_arm();
        test_p51_reservation_profile_mask_mapping();
        test_replacement_trigger_latches_once_and_is_opt_in();
        test_p51_same_f_missing_relationship_reassignment_keeps_sibling();
        test_p51_same_f_missing_real_sender_transfer_keeps_sibling();
        test_p51_same_f_missing_real_sender_transfer_keeps_sibling(true);
        test_route_endpoint_cap_refuses_before_f_open();
        test_known_endpoint_relationship_cap_refuses_before_f_open();
        test_source_connect_protocol_slice_retries_before_arm();
        test_source_connect_protocol_slices_share_one_outer_budget();
        test_stalled_f_arm_is_bounded_before_healthy_transfer(
            CACHE_PROFILE_P29V1);
        test_stalled_f_arm_is_bounded_before_healthy_transfer(
            CACHE_PROFILE_ZSTD_TU);
        test_stalled_f_arm_is_bounded_before_healthy_transfer(
            CACHE_PROFILE_ZSTD_ROUTE);
        test_held_retry_does_not_block_healthy_link(CACHE_PROFILE_P29V1);
        test_held_retry_does_not_block_healthy_link(CACHE_PROFILE_ZSTD_TU);
        test_held_retry_does_not_block_healthy_link(CACHE_PROFILE_ZSTD_ROUTE);
        test_parallel_distinct_f_matrix();
        test_parallel_distinct_c_matrix();
        test_same_link_serialization_matrix();
        test_expired_alias_cannot_release_held_incarnation();
        test_incarnation_change_waits_for_old_operation();
        test_source_active_count_cap_waits_then_releases();
        test_source_raw_byte_cap_waits_then_releases();
        test_source_admission_releases_on_open_read_error_and_expiry();
        test_alias_waiter_releases_active_source_credit_for_independent_f();
        test_route_poison_latches_before_successor_f_open();
        test_transport_loss_preserves_other_worker_service();
        test_interner_fault_returns_permanent_profile_unavailable();
        test_p29_fault_environment_is_exact();
        structured_launch_is_complete_and_fail_closed();
        structured_c_guid_is_strict();
        test_runtime_identity_disconnect_and_endpoint_failure();
        test_runtime_stop_interrupts_control_wait();
        test_operation_cancel_prebyte_mid_dialogue_eof_deadline();
        test_runtime_cancel_fail_stop_subprocess();
        test_runtime_owner_failure_fail_stop_subprocess();
        test_runtime_live_owner_failure_fail_stop_subprocess();
        test_runtime_zstd_tu_af_unix_loopback();
        test_operation_cancel_commit_race_preserves_witness();
        test_runtime_interner_poison_preserves_active_commit();
        test_runtime_stop_bounds_opening_source_arm();
        test_runtime_active_source_stop_fail_stops_bounded();
        test_runtime_stop_interrupts_active_endpoint();
        ready_reader_close_after_bind_is_fail_closed();
        ready_requires_bind_and_replacement_is_preserved();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "p50cacheservice: %s\n", error.what());
        return EXIT_FAILURE;
    }
    std::puts("p50cacheservice: ok");
    return EXIT_SUCCESS;
}
