#include "../cache/p50_cache_service.h"

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

void valid_handshake() {
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
    (void)::kill(child.pid, SIGTERM);
    int status = 0;
    CHECK(::waitpid(child.pid, &status, 0) == child.pid);
    child.pid = -1;
    CHECK(::access((template_path + std::string("/service.sock")).c_str(), F_OK) != 0);
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
        valid_handshake();
        replacement_node_is_not_removed();
        peer_credentials_are_required();
        rejects_identity_role_and_malformed();
        slowloris_deadline_is_total_and_listener_recovers();
        frame_header_and_payload_share_one_deadline();
        ready_requires_bind_and_replacement_is_preserved();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "p50cacheservice: %s\n", error.what());
        return EXIT_FAILURE;
    }
    std::puts("p50cacheservice: ok");
    return EXIT_SUCCESS;
}
