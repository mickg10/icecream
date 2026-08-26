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
        CHECK(::poll(&descriptor, 1, 2000) > 0);
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

void expect_exact_ready_then_eof(int fd) {
    std::array<char, 6> message{};
    size_t received = 0;
    while (received != message.size()) {
        struct pollfd descriptor{fd, POLLIN | POLLHUP, 0};
        CHECK(::poll(&descriptor, 1, 1000) > 0);
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

RuntimeCase authenticated_runtime_pair() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    RuntimeCase pair{local::Connection(sockets[0]), local::Connection(sockets[1])};
    const local::CredentialExpectation expected = current_credentials();
    CHECK(pair.sender.verify_peer_credentials(expected) == local::Status::Ok);
    CHECK(pair.receiver.verify_peer_credentials(expected) == local::Status::Ok);
    return pair;
}

service::RuntimeConfig test_runtime_config() {
    service::RuntimeConfig config;
    StoreIdentityRoot root{};
    root.bytes[15] = 9;
    config.c_store_guid = c_store_guid_for_root(root);
    config.f_store_guid = Id128::from_u64(9001);
    return config;
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
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION", "1", 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_C_STORE_GUID", c_guid.c_str(), 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_F_STORE_GUID", guid.c_str(), 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_SOCKET", expected_socket.c_str(), 1);
        (void)::setenv("ICECC_CACHE_SERVICE_EXPECTED_SOCKET_DIGEST", digest.c_str(), 1);
        const std::string listener_fd = std::to_string(prebound_listener);
        (void)::setenv("ICECC_CACHE_SERVICE_LISTENER_FD", listener_fd.c_str(), 1);
        ::execl(executable.c_str(), executable.c_str(), "--socket", expected_socket.c_str(),
                "--peer-uid", uid.c_str(), "--peer-gid", gid.c_str(), "--generation",
                generation.c_str(), "--attempt", attempt.c_str(),
                "--store-derivation-version", "1", "--c-store-guid", c_guid.c_str(),
                "--f-store-guid", guid.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    (void)::close(ready[1]);
    CHECK(::close(prebound_listener) == 0);
    const std::string ready_message = read_bounded_to_eof(ready[0]);
    (void)::close(ready[0]);
    CHECK(ready_message.rfind("READY v2 generation=91 attempt=7 DERIVATION_VERSION=1 pid=", 0) == 0);
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
    EndpointIoControl first_endpoint_control;
    first_endpoint_control.before_completion_check = [&](CompletionStamp&) {
        first_owner_id = std::this_thread::get_id();
    };
    auto authority = std::make_shared<P50PreparationAuthority>(Id128::from_u64(9002));
    P50ClientEndpoint client(authority);

    uint16_t port = 0;
    const int listener = loopback_listener(port);
    asio::io_context client_context;
    std::future<ClientRunResult> client_result;
    std::atomic<bool> first_ready_seen{false};
    std::thread client_thread([&] {
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
    P50ClientEndpoint second_client(second_authority);
    uint16_t second_port = 0;
    const int second_listener = loopback_listener(second_port);
    asio::io_context second_client_context;
    std::future<ClientRunResult> second_client_result;
    std::atomic<bool> second_ready_seen{false};
    std::thread second_client_thread([&] {
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

    // The preload gate holds the child immediately before write(READY).  The
    // visible socket therefore proves bind/listen completed, while the close
    // below deterministically makes the subsequent write fail with EPIPE.
    CHECK(wait_for_socket_node(socket, 2000));
    struct pollfd gate_seen{acknowledgement[0], POLLIN | POLLHUP, 0};
    CHECK(::poll(&gate_seen, 1, 2000) > 0);
    char gate_ack = 0;
    CHECK(::read(acknowledgement[0], &gate_ack, 1) == 1 && gate_ack == 1);
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
        legacy_store_identity_launches();
        signal_interrupts_control_wait(SIGTERM);
        signal_interrupts_control_wait(SIGINT);
        authenticated_idle_dispatcher_persists();
        replacement_node_is_not_removed();
        peer_credentials_are_required();
        rejects_identity_role_and_malformed();
        slowloris_deadline_is_total_and_listener_recovers();
        frame_header_and_payload_share_one_deadline();
        test_runtime_store_identity_is_explicit_and_role_tagged();
        structured_launch_is_complete_and_fail_closed();
        structured_c_guid_is_strict();
        test_runtime_identity_disconnect_and_endpoint_failure();
        test_runtime_stop_interrupts_control_wait();
        test_runtime_zstd_tu_af_unix_loopback();
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
