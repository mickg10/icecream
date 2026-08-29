#define private public
#include "cache/p50_local_transport.h"
#undef private

#if !defined(ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS)
#error "p50_local_transport_test must compile with its test-only transport seam"
#endif

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/socket.h>

using namespace icecc::p50::local;

namespace {

void check(bool value, const char* expression) {
    if (!value)
        throw std::runtime_error(expression);
}

#define CHECK(value) check((value), #value)

void interrupt_handler(int) {}

std::string test_replacement_path;

bool replace_listener_path_after_bind(const char* path) noexcept {
    if (::rename(path, test_replacement_path.c_str()) != 0)
        return false;
    const int replacement_fd =
        ::open(path, O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
    if (replacement_fd < 0)
        return false;
    ::close(replacement_fd);
    return true;
}

Frame data_frame() {
    return Frame{kProtocolVersion, MessageType::Data, Identity{17, 42},
                 {0, 1, 2, 3, 4, 5}};
}

bool saturate_socket(int fd, size_t* bytes_filled = nullptr) {
    const int original_flags = ::fcntl(fd, F_GETFL);
    if (original_flags < 0 || ::fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0)
        return false;

    std::array<unsigned char, 64 * 1024> bytes{};
    size_t total = 0;
    bool saturated = false;
    for (;;) {
        int send_flags = 0;
#if defined(MSG_NOSIGNAL)
        send_flags |= MSG_NOSIGNAL;
#endif
        const ssize_t count = ::send(fd, bytes.data(), bytes.size(), send_flags);
        if (count > 0) {
            total += static_cast<size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        saturated = count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        break;
    }
    const bool restored = ::fcntl(fd, F_SETFL, original_flags) == 0;
    if (bytes_filled != nullptr)
        *bytes_filled = total;
    return saturated && restored;
}

int raw_nonblocking_unix_client(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    address.sun_path[path.size()] = '\0';
    const socklen_t address_length =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), address_length) == 0 ||
        errno == EINPROGRESS)
        return fd;
    ::close(fd);
    return -1;
}

struct EintrConnectProbe {
    int calls = 0;
    int first_fd = -1;
    struct stat first_stat{};
    bool first_stat_valid = false;
    bool first_closed = false;
    bool second_distinct = false;
    bool second_cloexec = false;
};

EintrConnectProbe* active_eintr_probe = nullptr;

int force_first_connect_eintr(int fd, const void* address, size_t address_length) noexcept {
    if (active_eintr_probe == nullptr)
        return ::connect(fd, static_cast<const sockaddr*>(address),
                         static_cast<socklen_t>(address_length));

    ++active_eintr_probe->calls;
    if (active_eintr_probe->calls == 1) {
        active_eintr_probe->first_fd = fd;
        active_eintr_probe->first_stat_valid =
            ::fstat(fd, &active_eintr_probe->first_stat) == 0;
        errno = EINTR;
        return -1;
    }

    const int flags = ::fcntl(fd, F_GETFD);
    active_eintr_probe->second_cloexec =
        flags >= 0 && (flags & FD_CLOEXEC) != 0;
    struct stat second_stat{};
    if (active_eintr_probe->first_stat_valid && ::fstat(fd, &second_stat) == 0) {
        active_eintr_probe->second_distinct =
            fd != active_eintr_probe->first_fd ||
            second_stat.st_dev != active_eintr_probe->first_stat.st_dev ||
            second_stat.st_ino != active_eintr_probe->first_stat.st_ino;
    }
    if (fd == active_eintr_probe->first_fd) {
        active_eintr_probe->first_closed = active_eintr_probe->second_distinct;
    } else {
        errno = 0;
        active_eintr_probe->first_closed =
            ::fcntl(active_eintr_probe->first_fd, F_GETFD) < 0 && errno == EBADF;
    }
    return ::connect(fd, static_cast<const sockaddr*>(address),
                     static_cast<socklen_t>(address_length));
}

bool fill_unix_listener_queue(const std::string& path, std::vector<int>& clients) {
    for (int attempt = 0; attempt != 64; ++attempt) {
        const int fd = raw_nonblocking_unix_client(path);
        if (fd < 0)
            return !clients.empty();
        int socket_error = 0;
        socklen_t socket_error_length = sizeof(socket_error);
        CHECK(::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error,
                           &socket_error_length) == 0);
        if (socket_error == 0) {
            clients.push_back(fd);
            continue;
        }
        ::close(fd);
        return !clients.empty();
    }
    return !clients.empty();
}

std::array<uint8_t, kFrameHeaderSize> oversize_header() {
    std::array<uint8_t, kFrameHeaderSize> oversize{};
    oversize[0] = 'P';
    oversize[1] = '5';
    oversize[2] = '0';
    oversize[3] = 'L';
    oversize[5] = static_cast<uint8_t>(kProtocolVersion);
    oversize[7] = static_cast<uint8_t>(MessageType::Data);
    oversize[9] = 1;
    oversize[11] = 1;
    return oversize;
}

void framing_and_limits() {
    const Frame source = data_frame();
    Status status = Status::InvalidArgument;
    const auto bytes = encode_frame(source, &status);
    CHECK(status == Status::Ok);
    Frame decoded;
    CHECK(decode_frame(bytes, decoded) == Status::Ok);
    CHECK(decoded == source);

    CHECK(decode_frame(std::span<const uint8_t>(bytes).first(kFrameHeaderSize - 1), decoded) ==
          Status::Truncated);
    auto malformed = bytes;
    malformed[0] = 'X';
    CHECK(decode_frame(malformed, decoded) == Status::Malformed);
    malformed = bytes;
    malformed[4] = 0;
    malformed[5] = 2;
    CHECK(decode_frame(malformed, decoded) == Status::UnsupportedVersion);
    malformed = bytes;
    malformed.push_back(0);
    CHECK(decode_frame(malformed, decoded) == Status::Malformed);

    const auto oversize = oversize_header();
    CHECK(decode_frame(oversize, decoded) == Status::Oversize);

    CHECK(validate_identity(source, Identity{16, 42}) == Status::StaleGeneration);
    CHECK(validate_identity(source, Identity{17, 43}) == Status::IdentityMismatch);

    Frame too_large = source;
    too_large.payload.resize(kMaxFramePayload + 1);
    CHECK(encode_frame(too_large, &status).empty());
    CHECK(status == Status::Oversize);

    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    Connection writer(sockets[0]);
    CHECK(writer.valid());
    CHECK(writer.send(source) == Status::Ok);
    CHECK(read_frame(sockets[1], decoded) == Status::Ok);
    CHECK(decoded == source);
    ::close(sockets[1]);
}

void single_writer_busy_is_load_bearing() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    Connection writer(sockets[0]);
    CHECK(writer.valid());
    // Claiming the private gate models a writer already in progress and makes
    // the Busy regression deterministic rather than scheduler-dependent.
    CHECK(!writer.writing_.test_and_set(std::memory_order_acquire));
    CHECK(writer.send(data_frame()) == Status::Busy);
    writer.writing_.clear(std::memory_order_release);
    CHECK(writer.send(data_frame()) == Status::Ok);
    Frame received;
    CHECK(read_frame(sockets[1], received) == Status::Ok);
    CHECK(received == data_frame());
    ::close(sockets[1]);
}

void bounded_send_is_wall_time_limited() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int send_buffer = 1024;
    CHECK(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                       sizeof(send_buffer)) == 0);
    CHECK(saturate_socket(sockets[0]));
    Connection writer(sockets[0]);
    CHECK(writer.valid());

    Frame large = data_frame();
    large.payload.assign(kMaxFramePayload, 0xa5);
    const auto start = std::chrono::steady_clock::now();
    const Status timeout = writer.send_until(
        large, start + std::chrono::milliseconds(120));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    CHECK(timeout == Status::Timeout);
    CHECK(elapsed >= 80 && elapsed <= 700);
    CHECK(writer.valid());
    CHECK((::fcntl(writer.fd_, F_GETFL) & O_NONBLOCK) == 0);
    ::close(sockets[1]);

    int concurrent_sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, concurrent_sockets) == 0);
    CHECK(::setsockopt(concurrent_sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                       sizeof(send_buffer)) == 0);
    CHECK(saturate_socket(concurrent_sockets[0]));
    Connection concurrent_writer(concurrent_sockets[0]);
    CHECK(concurrent_writer.valid());
    std::atomic<bool> started{false};
    Status first_status = Status::InvalidArgument;
    std::thread first([&] {
        started.store(true, std::memory_order_release);
        first_status = concurrent_writer.send_until(
            large, std::chrono::steady_clock::now() + std::chrono::milliseconds(220));
    });
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    for (int attempt = 0; attempt != 100000 &&
         !concurrent_writer.writing_.test(std::memory_order_acquire); ++attempt)
        std::this_thread::yield();
    const Status concurrent_status = concurrent_writer.send_until(
        data_frame(), std::chrono::steady_clock::now() + std::chrono::seconds(1));
    first.join();
    CHECK(concurrent_status == Status::Busy);
    CHECK(first_status == Status::Timeout);
    ::close(concurrent_sockets[1]);

    int successful_sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, successful_sockets) == 0);
    Connection successful_writer(successful_sockets[0]);
    CHECK(successful_writer.send_until(
              data_frame(), std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
          Status::Ok);
    Frame received;
    CHECK(read_frame(successful_sockets[1], received) == Status::Ok);
    CHECK(received == data_frame());
    ::close(successful_sockets[1]);
}

void absolute_deadline_edges() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int send_buffer = 1024;
    CHECK(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                       sizeof(send_buffer)) == 0);
    CHECK(saturate_socket(sockets[0]));
    Connection writer(sockets[0]);
    Frame large = data_frame();
    large.payload.assign(kMaxFramePayload, 0xa5);

    const auto short_start = std::chrono::steady_clock::now();
    CHECK(writer.send_until(large, short_start + std::chrono::microseconds(100)) ==
          Status::Timeout);
    const auto short_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - short_start).count();
    // This is deliberately below the old ceil-to-one-millisecond behavior;
    // leave a little scheduler slack while still detecting a millisecond
    // floor on an otherwise idle test host.
    CHECK(short_elapsed < 1000);
    ::close(sockets[1]);

    int max_sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, max_sockets) == 0);
    CHECK(::setsockopt(max_sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                       sizeof(send_buffer)) == 0);
    CHECK(saturate_socket(max_sockets[0]));
    Connection max_writer(max_sockets[0]);
    std::thread close_peer([peer = max_sockets[1]] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        ::close(peer);
    });
    const auto max_start = std::chrono::steady_clock::now();
    CHECK(max_writer.send_until(large, std::chrono::steady_clock::time_point::max()) ==
          Status::IoError);
    close_peer.join();
    const auto max_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - max_start).count();
    CHECK(max_elapsed < 500);
}

void concurrent_reader_during_bounded_send() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int send_buffer = 1024;
    CHECK(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &send_buffer,
                       sizeof(send_buffer)) == 0);
    CHECK(saturate_socket(sockets[0]));
    Connection writer(sockets[0]);
    Frame large = data_frame();
    large.payload.assign(kMaxFramePayload, 0x5a);
    Status send_status = Status::InvalidArgument;
    std::thread sender([&] {
        send_status = writer.send_until(
            large, std::chrono::steady_clock::now() + std::chrono::milliseconds(140));
    });
    while (!writer.writing_.test(std::memory_order_acquire))
        std::this_thread::yield();
    // Ensure the bounded writer is in its wait before starting a blocking
    // receive on the same Connection/open file description.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    Frame received;
    Status receive_status = Status::InvalidArgument;
    std::thread reader([&] { receive_status = writer.receive(received); });
    const auto bytes = encode_frame(data_frame());
    std::thread peer_sender([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        size_t done = 0;
        while (done != bytes.size()) {
            int send_flags = 0;
#if defined(MSG_NOSIGNAL)
            send_flags |= MSG_NOSIGNAL;
#endif
            const ssize_t count = ::send(sockets[1], bytes.data() + done,
                                         bytes.size() - done, send_flags);
            if (count > 0) {
                done += static_cast<size_t>(count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
    });
    reader.join();
    peer_sender.join();
    sender.join();
    CHECK(receive_status == Status::Ok);
    CHECK(received == data_frame());
    CHECK(send_status == Status::Timeout);
    CHECK((::fcntl(writer.fd_, F_GETFL) & O_NONBLOCK) == 0);
    ::close(sockets[1]);
}

void absolute_receive_budget_and_terminal_teardown() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    Connection reader(sockets[1]);
    const auto bytes = encode_frame(data_frame());
    CHECK(::send(sockets[0], bytes.data(), kFrameHeaderSize, MSG_NOSIGNAL) ==
          static_cast<ssize_t>(kFrameHeaderSize));
    std::thread delayed_payload([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        ::send(sockets[0], bytes.data() + kFrameHeaderSize,
               bytes.size() - kFrameHeaderSize, MSG_NOSIGNAL);
    });
    Frame ignored;
    const auto deadline_start = std::chrono::steady_clock::now();
    CHECK(reader.receive_until(ignored, deadline_start + std::chrono::milliseconds(70)) ==
          Status::Timeout);
    delayed_payload.join();
    const auto deadline_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - deadline_start).count();
    CHECK(deadline_elapsed < 400);
    ::close(sockets[0]);

    int partial_sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, partial_sockets) == 0);
    Connection partial_reader(partial_sockets[1]);
    CHECK(::send(partial_sockets[0], bytes.data(), kFrameHeaderSize + 1,
                 MSG_NOSIGNAL) == static_cast<ssize_t>(kFrameHeaderSize + 1));
    ::close(partial_sockets[0]);
    CHECK(partial_reader.receive_until(
              ignored, std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
          Status::Truncated);
}

void terminal_poll_errors_and_sigpipe_safety() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    Connection writer(sockets[0]);
    ::close(sockets[1]);
    const auto start = std::chrono::steady_clock::now();
    CHECK(writer.send_until(data_frame(), start + std::chrono::seconds(1)) == Status::IoError);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count() < 200);

    int invalid_sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, invalid_sockets) == 0);
    Connection invalid(invalid_sockets[0]);
    ::close(invalid.fd_);
    CHECK(invalid.send_until(data_frame(),
                             std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
          Status::IoError);
    invalid.fd_ = -1;
    ::close(invalid_sockets[1]);
}

void handshake_identity() {
    const Identity identity{99, 7};
    const Frame hello = make_hello(PeerRole::Daemon, identity);
    CHECK(validate_handshake(hello, MessageType::Hello, PeerRole::Daemon, identity) == Status::Ok);
    CHECK(validate_handshake(hello, MessageType::Hello, PeerRole::Daemon,
                             Identity{98, 7}) == Status::StaleGeneration);
    CHECK(validate_handshake(hello, MessageType::Hello, PeerRole::Daemon,
                             Identity{99, 8}) == Status::IdentityMismatch);
    CHECK(validate_handshake(hello, MessageType::Hello, PeerRole::Sidecar, identity) ==
          Status::WrongRole);
    CHECK(validate_handshake(make_hello_ack(PeerRole::Daemon, identity), MessageType::Hello,
                             PeerRole::Daemon, identity) == Status::Malformed);
}

void truncated_read() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const auto bytes = encode_frame(data_frame());
    CHECK(::write(sockets[0], bytes.data(), kFrameHeaderSize + 1) ==
          static_cast<ssize_t>(kFrameHeaderSize + 1));
    ::close(sockets[0]);
    Frame ignored;
    CHECK(read_frame(sockets[1], ignored) == Status::Truncated);
    ::close(sockets[1]);

    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const auto oversize = oversize_header();
    CHECK(::write(sockets[0], oversize.data(), oversize.size()) ==
          static_cast<ssize_t>(oversize.size()));
    CHECK(read_frame(sockets[1], ignored) == Status::Oversize);
    ::close(sockets[0]);
    ::close(sockets[1]);

    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    ::close(sockets[0]);
    CHECK(read_frame(sockets[1], ignored) == Status::CleanEof);
    ::close(sockets[1]);

    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    const auto partial_bytes = encode_frame(data_frame());
    CHECK(::write(sockets[0], partial_bytes.data(), kFrameHeaderSize - 1) ==
          static_cast<ssize_t>(kFrameHeaderSize - 1));
    ::close(sockets[0]);
    CHECK(read_frame(sockets[1], ignored) == Status::Truncated);
    ::close(sockets[1]);

    // Header validation must fail before waiting for the declared payload.
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    auto invalid_header = bytes;
    invalid_header.resize(kFrameHeaderSize);
    invalid_header[0] = 'X';
    invalid_header[11] = 0xff;
    CHECK(::write(sockets[0], invalid_header.data(), invalid_header.size()) ==
          static_cast<ssize_t>(invalid_header.size()));
    CHECK(read_frame(sockets[1], ignored) == Status::Malformed);
    ::close(sockets[0]);
    ::close(sockets[1]);
}

void credentials() {
    int sockets[2] = {-1, -1};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CredentialExpectation expected;
    expected.uid = static_cast<uint64_t>(::getuid());
    CHECK(verify_peer_credentials(sockets[0], expected) == Status::Ok);

    const PeerCredentialProvider provider = [](int) {
        return std::optional<PeerCredential>(PeerCredential{10, 20, 30});
    };
    expected = CredentialExpectation{};
    expected.uid = 11;
    CHECK(verify_peer_credentials(sockets[0], expected, provider) ==
          Status::PeerCredentialMismatch);
    expected.uid = 10;
    CHECK(verify_peer_credentials(sockets[0], expected, provider) == Status::Ok);
    const PeerCredentialProvider unavailable = [](int) -> std::optional<PeerCredential> {
        return std::nullopt;
    };
    CHECK(verify_peer_credentials(sockets[0], expected, unavailable) ==
          Status::PeerCredentialUnavailable);
    CHECK(verify_peer_credentials(sockets[0], CredentialExpectation{}, provider) ==
          Status::InvalidArgument);
    ::close(sockets[0]);
    ::close(sockets[1]);
}

void bounded_unix_connect() {
    char directory[] = "/tmp/icecc-p50-local-connect-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/endpoint";
    Status status = Status::InvalidArgument;
    const int listener = listen_unix(path, 1, &status);
    CHECK(listener >= 0 && status == Status::Ok);

    // A free listener completes immediately, while retaining the compatibility
    // API's blocking descriptor contract.
    Connection immediate = connect_unix_until(
        path, std::chrono::steady_clock::now() + std::chrono::seconds(1), &status);
    CHECK(immediate.valid() && status == Status::Ok);
    CHECK((::fcntl(immediate.native_handle(), F_GETFL) & O_NONBLOCK) == 0);
    CHECK((::fcntl(immediate.native_handle(), F_GETFD) & FD_CLOEXEC) != 0);
    int accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);
    ::close(accepted);

    EintrConnectProbe interrupted_probe;
    active_eintr_probe = &interrupted_probe;
    Connection interrupted = connect_unix_until_with_test_hook(
        path, std::chrono::steady_clock::now() + std::chrono::milliseconds(250), &status,
        force_first_connect_eintr);
    active_eintr_probe = nullptr;
    CHECK(interrupted.valid() && status == Status::Ok);
    CHECK(interrupted_probe.calls == 2);
    CHECK(interrupted_probe.first_closed);
    CHECK(interrupted_probe.second_distinct);
    CHECK(interrupted_probe.second_cloexec);
    accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);
    ::close(accepted);

    // The same private parent/node checks apply before a deadline is used.
    CHECK(!connect_unix_until("relative-endpoint",
                              std::chrono::steady_clock::now() + std::chrono::seconds(1),
                              &status)
               .valid());
    CHECK(status == Status::InvalidPath);
    CHECK(!connect_unix_until(std::string(kMaxUnixPath + 1, 'x'),
                              std::chrono::steady_clock::now() + std::chrono::seconds(1),
                              &status)
               .valid());
    CHECK(status == Status::InvalidPath);
    CHECK(!connect_unix_until(path + ".missing",
                              std::chrono::steady_clock::now() + std::chrono::seconds(1),
                              &status)
               .valid());
    CHECK(status == Status::InvalidPath);
    CHECK(::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP) == 0);
    CHECK(!connect_unix_until(path,
                              std::chrono::steady_clock::now() + std::chrono::seconds(1),
                              &status)
               .valid());
    CHECK(status == Status::InvalidPath);
    CHECK(::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0);
    CHECK(!connect_unix_until(path, std::chrono::steady_clock::now(), &status).valid());
    CHECK(status == Status::Timeout);

    std::vector<int> queued_clients;
    CHECK(fill_unix_listener_queue(path, queued_clients));
    const auto pending_start = std::chrono::steady_clock::now();
    std::thread release_one([listener] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const int accepted_fd = ::accept(listener, nullptr, nullptr);
        if (accepted_fd >= 0) {
            // Keep the peer alive while the pending client observes writable
            // readiness; closing it immediately would intentionally produce
            // POLLHUP and exercise the error path instead.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            ::close(accepted_fd);
        }
    });
    Connection pending = connect_unix_until(
        path, pending_start + std::chrono::milliseconds(700), &status);
    release_one.join();
    // Linux reports a saturated AF_UNIX queue as EAGAIN (rather than
    // EINPROGRESS).  The failed attempt is closed and a fresh CLOEXEC socket
    // is admitted after the listener releases capacity under the same bound.
    CHECK(pending.valid() && status == Status::Ok);
    CHECK((::fcntl(pending.native_handle(), F_GETFL) & O_NONBLOCK) == 0);
    CHECK((::fcntl(pending.native_handle(), F_GETFD) & FD_CLOEXEC) != 0);
    accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);
    ::close(accepted);

    // Once capacity is available, the same absolute API succeeds and still
    // returns a blocking, CLOEXEC descriptor.
    for (int fd : queued_clients)
        ::close(fd);
    queued_clients.clear();
    Connection after_pending = connect_unix_until(
        path, std::chrono::steady_clock::now() + std::chrono::seconds(1), &status);
    CHECK(after_pending.valid() && status == Status::Ok);
    CHECK((::fcntl(after_pending.native_handle(), F_GETFL) & O_NONBLOCK) == 0);
    CHECK((::fcntl(after_pending.native_handle(), F_GETFD) & FD_CLOEXEC) != 0);
    accepted = ::accept(listener, nullptr, nullptr);
    CHECK(accepted >= 0);
    ::close(accepted);

    // Keep the listener genuinely backlogged: the bounded operation must
    // return at its one absolute deadline rather than blocking in connect(2).
    CHECK(fill_unix_listener_queue(path, queued_clients));
    const auto saturated_start = std::chrono::steady_clock::now();
    Connection saturated = connect_unix_until(
        path, saturated_start + std::chrono::milliseconds(100), &status);
    const auto saturated_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - saturated_start).count();
    CHECK(!saturated.valid() && status == Status::Timeout);
    CHECK(saturated_elapsed >= 70 && saturated_elapsed < 700);

    // Closing a listener leaves its private node in place; a refused connect
    // is an error, never a falsely successful Connection.
    ::close(listener);
    CHECK(!connect_unix_until(path,
                              std::chrono::steady_clock::now() + std::chrono::seconds(1),
                              &status)
               .valid());
    CHECK(status == Status::IoError);
    for (int fd : queued_clients)
        ::close(fd);
    ::unlink(path.c_str());
    CHECK(::rmdir(directory) == 0);
}

void unix_setup() {
    char directory[] = "/tmp/icecc-p50-local-transport-XXXXXX";
    CHECK(::mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/endpoint";
    Status status = Status::InvalidArgument;
    const int listener = listen_unix(path, 2, &status);
    CHECK(listener >= 0);
    CHECK(status == Status::Ok);
    struct stat socket_info{};
    CHECK(::lstat(path.c_str(), &socket_info) == 0);
    CHECK((socket_info.st_mode & 07777) == 0600);

    Connection accepted(-1);
    Status accept_status = Status::InvalidArgument;
    std::thread accept_thread([&] {
        accepted = accept_unix(listener, &accept_status);
    });
    struct sigaction action{};
    struct sigaction previous_action{};
    action.sa_handler = interrupt_handler;
    sigemptyset(&action.sa_mask);
    CHECK(::sigaction(SIGUSR1, &action, &previous_action) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(::pthread_kill(accept_thread.native_handle(), SIGUSR1) == 0);
    Connection client = connect_unix_until(
        path, std::chrono::steady_clock::now() + std::chrono::seconds(1), &status);
    CHECK(client.valid());
    accept_thread.join();
    CHECK(::sigaction(SIGUSR1, &previous_action, nullptr) == 0);
    CHECK(accepted.valid());
    CHECK(accept_status == Status::Ok);
    CHECK(client.cloexec());
    CHECK(accepted.cloexec());

    CHECK((::fcntl(listener, F_GETFD) & FD_CLOEXEC) != 0);

    const Frame hello = make_hello(PeerRole::Sidecar, Identity{1, 1});
    CHECK(client.send(hello) == Status::Ok);
    Frame received;
    CHECK(accepted.receive(received) == Status::Ok);
    CHECK(received == hello);
    CHECK(client.valid());
    CHECK(accepted.valid());
    CHECK((::fcntl(listener, F_GETFD) & FD_CLOEXEC) != 0);

    // A refused endpoint is terminal after SO_ERROR and never leaves a
    // descriptor behind.  This also protects the nonblocking-connect mutant
    // that incorrectly treats POLLERR as successful readiness.
    CHECK(::close(listener) == 0);
    ::unlink(path.c_str());
    Status refused_status = Status::Ok;
    Connection refused_connection = connect_unix_until(
        path, std::chrono::steady_clock::now() + std::chrono::milliseconds(100),
        &refused_status);
    CHECK(!refused_connection.valid());
    CHECK(refused_status == Status::InvalidPath || refused_status == Status::IoError ||
          refused_status == Status::Timeout);
    CHECK(::rmdir(directory) == 0);

    CHECK(listen_unix(std::string(kMaxUnixPath + 1, 'x'), 1, &status) < 0);
    CHECK(status == Status::InvalidPath);

    // Relative, embedded-NUL, and shared-parent paths are refused.
    CHECK(listen_unix("relative-endpoint", 1, &status) < 0);
    CHECK(status == Status::InvalidPath);
    std::string embedded = directory;
    embedded.append("\0bad", 4);
    CHECK(listen_unix(embedded, 1, &status) < 0);
    CHECK(status == Status::InvalidPath);
    CHECK(listen_unix("/tmp/icecc-p50-shared-parent", 1, &status) < 0);
    CHECK(status == Status::InvalidPath);

    char second_directory[] = "/tmp/icecc-p50-local-transport-XXXXXX";
    CHECK(::mkdtemp(second_directory) != nullptr);
    const std::string second_path = std::string(second_directory) + "/endpoint";
    const int second_listener = listen_unix(second_path, 1, &status);
    CHECK(second_listener >= 0);
    CHECK(listen_unix(second_path, 1, &status) < 0);
    CHECK(status == Status::IoError);
    ::close(second_listener);
    ::unlink(second_path.c_str());
    CHECK(::rmdir(second_directory) == 0);

    char retained_directory[] = "/tmp/icecc-p50-local-transport-XXXXXX";
    CHECK(::mkdtemp(retained_directory) != nullptr);
    const std::string retained_path = std::string(retained_directory) + "/endpoint";
    test_replacement_path = retained_path + ".replacement";
    Status retained_status = Status::InvalidArgument;
    const int retained_listener = listen_unix_with_test_hook(
        retained_path, 1, &retained_status, replace_listener_path_after_bind);
    CHECK(retained_listener < 0);
    CHECK(retained_status == Status::ListenerNodeLeftForCleanup);
    CHECK(::lstat(retained_path.c_str(), &socket_info) == 0);
    CHECK(S_ISREG(socket_info.st_mode));
    ::unlink(retained_path.c_str());
    ::unlink(test_replacement_path.c_str());
    CHECK(::rmdir(retained_directory) == 0);
}

} // namespace

int main() {
    try {
        framing_and_limits();
        single_writer_busy_is_load_bearing();
        bounded_send_is_wall_time_limited();
        absolute_deadline_edges();
        concurrent_reader_during_bounded_send();
        absolute_receive_budget_and_terminal_teardown();
        terminal_poll_errors_and_sigpipe_safety();
        handshake_identity();
        truncated_read();
        credentials();
        bounded_unix_connect();
        unix_setup();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "p50localtransport: %s\n", error.what());
        return EXIT_FAILURE;
    }
    std::puts("p50localtransport: ok");
    return EXIT_SUCCESS;
}
