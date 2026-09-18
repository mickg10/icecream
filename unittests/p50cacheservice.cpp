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

#include <chrono>
#include <atomic>
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
    const auto second = launch_attempt();
    expect_structured_ready(second.second);
    CHECK(::close(second.second) == 0);
    CHECK(::kill(second.first, SIGTERM) == 0);
    int second_status = 0;
    CHECK(::waitpid(second.first, &second_status, 0) == second.first);
    CHECK(WIFEXITED(second_status) && WEXITSTATUS(second_status) == 0);
    struct stat cache_info{};
    CHECK(::stat((root + "/p29-system-source-fingerprint-v1.cache").c_str(),
                 &cache_info) == 0 && cache_info.st_size > 0);
    CHECK(::unlink(socket.c_str()) == 0);
    CHECK(::unlink((root + "/p29-system-source-fingerprint-v1.cache").c_str()) == 0);
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

int loopback_listener(uint16_t& port);

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

int loopback_listener(uint16_t& port) {
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0);
    int reuse = 1;
    CHECK(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(::listen(listener, 1) == 0);
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
};

void serve_stalled_source_arm(int listener,
                              std::atomic<bool>& arm_received) noexcept {
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
        pollfd descriptor{channel->fd, POLLIN | POLLHUP | POLLERR, 0};
        int ready = -1;
        do {
            ready = ::poll(&descriptor, 1, 4000);
        } while (ready < 0 && errno == EINTR);
        if (ready > 0) {
            uint8_t byte = 0;
            (void)::recv(channel->fd, &byte, 1, 0);
        }
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
    SourceArmServerObservation& observation) noexcept {
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

        namespace asio = boost::asio;
        asio::io_context context;
        boost::system::error_code error;
        auto socket = P50ServerEndpoint::adopt_connected_fd(
            context.get_executor(), raw_fd, error);
        if (!socket.has_value())
            return;
        P50ServerEndpointConfig server_config;
        server_config.input_job_state = [](
            CStoreGuid, const TxBegin&, const TxCommit&,
            std::span<const uint8_t>) { return InputJobState::Open; };
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
                               SourceArmServerObservation& observation) noexcept {
    sockaddr_in peer{};
    socklen_t peer_size = sizeof(peer);
    const int accepted = ::accept(
        listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
    (void)::close(listener);
    serve_accepted_source_transfer(accepted, peer, peer_size, f_store_guid,
                                   f_store_generation, observation);
}

struct SourceConnectRetryObservation {
    unsigned int accepted_connections = 0;
    std::vector<uint8_t> first_connection_bytes;
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
    const int length = std::snprintf(
        path.data(), path.size(), "/tmp/%.*s-XXXXXX",
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

void test_stalled_f_arm_is_bounded_before_healthy_transfer() {
    StoreIdentityRoot local_root{};
    local_root.bytes[15] = 0x61;
    const SidecarLaunchIdentity launch = test_sidecar_launch(local_root);
    service::RuntimeConfig config = test_runtime_config();
    config.c_store_guid = launch.c_store_guid;
    config.f_store_guid = launch.f_store_guid;
    config.f_store_generation = launch.store_generation;
    config.sidecar_launch = launch;
    config.source_open_arm_timeout = std::chrono::milliseconds(1500);
    service::SidecarRuntime runtime(std::move(config));

    uint16_t stalled_port = 0;
    const int stalled_listener = loopback_listener(stalled_port);
    std::atomic<bool> stalled_arm_received{false};
    std::thread stalled_server([&] {
        serve_stalled_source_arm(stalled_listener, stalled_arm_received);
    });
    const std::array<uint8_t, 8> stalled_source{
        's', 't', 'a', 'l', 'l', 'e', 'd', '\n'};
    local::P50SourceTransferResult stalled_result;
    const auto clock = sidecar::process_monotonic_clock_identity();
    const auto stalled_deadline =
        sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            clock.clock_domain_id, clock.time_namespace_id);
    std::thread stalled_transfer([&] {
        stalled_result = runtime.transfer_source_on_owner(
            source_transfer_request(stalled_port, 121, CACHE_PROFILE_ZSTD_TU),
            stalled_deadline,
            local::HandoffFd(source_file("p50-stalled-arm", stalled_source)));
    });
    const auto arm_wait_limit =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!stalled_arm_received.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < arm_wait_limit)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(stalled_arm_received.load(std::memory_order_acquire));

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
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            clock.clock_domain_id, clock.time_namespace_id);
    const auto healthy_started = std::chrono::steady_clock::now();
    const local::P50SourceTransferResult healthy_result =
        runtime.transfer_source_on_owner(
            source_transfer_request(healthy_port, 131, CACHE_PROFILE_ZSTD_TU),
            healthy_deadline,
            local::HandoffFd(source_file("p50-healthy-arm", healthy_source)));
    const auto healthy_elapsed =
        std::chrono::steady_clock::now() - healthy_started;

    stalled_transfer.join();
    stalled_server.join();
    healthy_server.join();
    CHECK(stalled_result.code == local::SourceTransferResultCode::Error);
    CHECK(stalled_result.error_code == 4 && stalled_result.attempts == 0);
    CHECK(healthy_result.code == local::SourceTransferResultCode::Committed);
    CHECK(healthy_result.attempts == 1);
    CHECK(healthy_result.raw_bytes == healthy_source.size());
    CHECK(healthy_result.raw_digest == icecc::digest128(healthy_source));
    CHECK(healthy_observation.accepted && healthy_observation.protocol_50 &&
          healthy_observation.arm_received && healthy_observation.armed_sent &&
          healthy_observation.cache_session_received &&
          healthy_observation.ready_sent &&
          healthy_observation.transfer_completed);
    CHECK(healthy_elapsed < std::chrono::seconds(3));
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

int main() {
    try {
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
        test_route_endpoint_cap_refuses_before_f_open();
        test_known_endpoint_relationship_cap_refuses_before_f_open();
        test_source_connect_protocol_slice_retries_before_arm();
        test_source_connect_protocol_slices_share_one_outer_budget();
        test_stalled_f_arm_is_bounded_before_healthy_transfer();
        test_route_poison_latches_before_successor_f_open();
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
