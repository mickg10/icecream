#include "../cache/p50_cache_service.h"
#include "../cache/p50_control_operation.h"
#include "comm.h"
#include "../services/digest128.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
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
#include <boost/asio/use_future.hpp>
#include <future>
#include <memory>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <thread>

using namespace icecc::p50;

namespace {

constexpr int kStartupReadyTimeoutMilliseconds = 5000;

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
    char path[] = "/tmp/icecc-p51-async-source-XXXXXX";
    const int fd = ::mkstemp(path);
    CHECK(fd >= 0);
    CHECK(::unlink(path) == 0);
    const uint8_t bytes[2] = {0x51, 0x52};
    CHECK(write_all(fd, bytes));
    return local::HandoffFd(fd);
}

void receive_p51_transfer_error(local::Connection& peer,
                                local::Identity identity,
                                uint64_t request_id,
                                std::chrono::steady_clock::time_point deadline,
                                bool acknowledge) {
    local::Frame response;
    CHECK(peer.receive_until(response, deadline) == local::Status::Ok);
    CHECK(response.type == local::MessageType::Data);
    CHECK(local::validate_identity(response, identity) == local::Status::Ok);
    local::ControlOperation decoded;
    CHECK(local::decode_control_operation(response.payload, decoded));
    CHECK(decoded.kind == local::ControlOperationKind::P51SourceTransfer);
    CHECK(decoded.request_id == request_id);
    CHECK(decoded.p51_source_transfer_result.has_value());
    CHECK(decoded.p51_source_transfer_result->code ==
          local::SourceTransferResultCode::Error);
    if (acknowledge) {
        const local::Frame goodbye{local::kProtocolVersion,
                                   local::MessageType::Goodbye,
                                   identity, {}};
        CHECK(peer.send_until(goodbye, deadline) == local::Status::Ok);
    }
}

void test_p51_async_transfer_reply_deadline_close_and_slot_reuse() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x35;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    StoreIdentityRoot remote_root{};
    remote_root.bytes[15] = 0x36;
    const CStoreGuid remote_c = c_store_guid_for_root(remote_root);

    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.endpoint_caps.zstd.max_raw_bytes = 1;
    config.max_pending_p51_source_reservations = 8;
    config.max_active_p51_source_transfers = 1;
    config.max_pending_p51_source_operations = 1;
    service::SidecarRuntime runtime(std::move(config));

    auto make_request = [&](uint64_t request_id,
                            std::chrono::milliseconds lifetime) {
        auto reservation = test_p51_reservation_request(
            remote_c, 31, launch.identity.generation, launch.identity.attempt,
            request_id, CACHE_PROFILE_ZSTD_TU, 30, lifetime);
        reservation.arm.source.selected_f_host = "127.0.0.1";
        reservation.arm.source.selected_f_cache_port = 1;
        const auto armed = runtime.reserve_p51_source_on_owner(reservation);
        CHECK(armed.error_code == 0 && armed.armed.has_value());
        return local::P51SourceTransferRequest{
            *armed.armed, reservation.absolute_deadline};
    };
    auto enqueue = [&](RuntimeCase& pair,
                       const local::P51SourceTransferRequest& request) {
        const auto operation = local::make_p51_source_transfer_operation(
            launch.identity, request,
            request.armed.arm.source.source_request_id);
        return runtime.enqueue_p51_source_transfer(
            std::move(pair.sender), launch.identity, operation,
            oversized_test_source_fd());
    };

    // A response stays admitted until Goodbye or the original source deadline;
    // a cap+1 request is refused without consuming the queued socket/FD.
    RuntimeCase stalled = authenticated_runtime_pair();
    const auto short_request = make_request(7101, std::chrono::milliseconds(350));
    CHECK(enqueue(stalled, short_request));
    const auto short_deadline = short_request.absolute_deadline.as_steady_time_point();
    receive_p51_transfer_error(stalled.receiver, launch.identity, 7101,
                               short_deadline, false);
    CHECK(wait_for_source_operation_count(runtime, 1, std::chrono::seconds(1)));

    RuntimeCase refused = authenticated_runtime_pair();
    const auto refused_request = make_request(7102, std::chrono::seconds(2));
    CHECK(!enqueue(refused, refused_request));
    CHECK(refused.sender.valid()); // cap refusal leaves ownership with caller
    CHECK(wait_for_source_operation_count(runtime, 0, std::chrono::seconds(2)));

    // A later request reuses the released slot; successful local receipt and
    // Goodbye are both required to retire the operation normally.
    RuntimeCase normal = authenticated_runtime_pair();
    const auto normal_request = make_request(7103, std::chrono::seconds(2));
    CHECK(enqueue(normal, normal_request));
    receive_p51_transfer_error(
        normal.receiver, launch.identity, 7103,
        normal_request.absolute_deadline.as_steady_time_point(), true);
    CHECK(wait_for_source_operation_count(runtime, 0, std::chrono::seconds(1)));

    // Peer EOF is also terminal for the local reply pump and releases its
    // bounded queue reservation without waiting out the source deadline.
    RuntimeCase disconnected = authenticated_runtime_pair();
    const auto disconnected_request = make_request(7104, std::chrono::seconds(2));
    CHECK(enqueue(disconnected, disconnected_request));
    disconnected.receiver = local::Connection(-1);
    CHECK(wait_for_source_operation_count(runtime, 0, std::chrono::seconds(1)));

    RuntimeCase stopped = authenticated_runtime_pair();
    const auto stopped_request = make_request(7105, std::chrono::seconds(2));
    CHECK(enqueue(stopped, stopped_request));
    receive_p51_transfer_error(
        stopped.receiver, launch.identity, 7105,
        stopped_request.absolute_deadline.as_steady_time_point(), false);
    CHECK(wait_for_source_operation_count(runtime, 1, std::chrono::seconds(1)));
    runtime.stop();
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
        oversized_test_source_fd());
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
        oversized_test_source_fd());
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
        oversized_test_source_fd());
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
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.max_pending_p51_source_reservations = 120;
    service::SidecarRuntime runtime(std::move(config));

    std::vector<local::P51SourceReservationRequest> requests;
    std::vector<P51SourceArmedFields> armed;
    requests.reserve(120);
    armed.reserve(120);
    for (uint64_t index = 0; index != 120; ++index) {
        auto request = test_p51_reservation_request(
            remote_c, 41, launch.identity.generation, launch.identity.attempt,
            7200 + index, CACHE_PROFILE_ZSTD_TU, 30,
            index == 119 ? std::chrono::seconds(2) : std::chrono::seconds(10));
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

    CHECK(runtime.cancel_p51_source_on_owner(
        requests[1].arm, armed[1].reservation_id,
        std::chrono::steady_clock::now() + std::chrono::seconds(1)));
    auto after_cancel = test_p51_reservation_request(
        remote_c, 41, launch.identity.generation, launch.identity.attempt,
        7321, CACHE_PROFILE_ZSTD_TU, 30);
    CHECK(runtime.reserve_p51_source_on_owner(after_cancel).armed.has_value());

    // No further owner request is issued while the first row expires; the
    // timer sweep itself must release its exact global metadata slot.
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    auto after_expiry = test_p51_reservation_request(
        remote_c, 41, launch.identity.generation, launch.identity.attempt,
        7322, CACHE_PROFILE_ZSTD_TU, 30);
    CHECK(runtime.reserve_p51_source_on_owner(after_expiry).armed.has_value());
    std::puts("P51_RESERVATION_CAP global120/cancel/idle-expiry: ok");
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
        });
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
        runtime.run_owner_callback_for_test([&] {
        CHECK(runtime.record_p51_job_commit_on_owner(
            link, first_binding, first_commit));

        const Id128 operation_id{icecc::digest128("publication reset op").bytes};
        const RecoverBegin begin = test_p51_recover_begin(
            link, 0, 1, 1, operation_id);
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
        const std::array<RecoverWitness, 1> witnesses{witness};
        const auto recovered = runtime.recover_p51_receipts_on_owner(
            link, begin, witnesses, test_p51_recover_end(begin, witnesses));
        CHECK(recovered.has_value() && recovered->rows.size() == 1);
        reset.relationship_id = link.relationship_id;
        reset.old_relationship_epoch = link.relationship_epoch;
        reset.new_relationship_epoch = link.relationship_epoch + 1;
        reset.physical_link_generation = link.physical_link_generation;
        reset.operation_id = operation_id;
        reset.settled_prefix_k = 1;
        reset.old_history_nonce = link.history_nonce;
        reset.new_history_nonce = HistoryNonce{link.history_nonce.value + 1};
        const auto ack = runtime.validate_p51_reset_on_owner(link, reset);
        CHECK(ack.has_value());
        CHECK(runtime.commit_p51_reset_on_owner(link, reset, *ack));
        confirm.relationship_id = reset.relationship_id;
        confirm.new_relationship_epoch = reset.new_relationship_epoch;
        confirm.physical_link_generation = reset.physical_link_generation;
        confirm.operation_id = reset.operation_id;
        confirm.new_history_nonce = reset.new_history_nonce;
        confirm.settled_prefix_k = reset.settled_prefix_k;
        CHECK(runtime.confirm_p51_reset_on_owner(link, confirm));
        });

        auto second_request = test_p51_reservation_request(
            remote_c, 61, launch.identity.generation, launch.identity.attempt,
            6102, CACHE_PROFILE_ZSTD_TU, 30);
        const auto second_result = runtime.reserve_p51_source_on_owner(second_request);
        CHECK(second_result.error_code == 0 && second_result.armed.has_value());
        runtime.run_owner_callback_for_test([&] {
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

        const RecoverBegin after_duplicate_confirm = test_p51_recover_begin(
            link, 1, 2, 1, Id128{icecc::digest128("post-confirm inspect").bytes});
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
        const std::array<RecoverWitness, 1> second_witnesses{second_witness};
        const auto retained = runtime.recover_p51_receipts_on_owner(
            link, after_duplicate_confirm, second_witnesses,
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
        test_p51_stop_while_waiting_for_link_session_echo();
        test_p51_stop_while_waiting_for_link_state();
        test_p51_reservation_capacity_120_cancel_and_expiry();
        test_p51_cancel_publication_and_reset_lifecycle();
        test_p51_reservation_profile_mask_mapping();
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
