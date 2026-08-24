#define private public
#include "cache/p50_local_transport.h"
#undef private

#if !defined(ICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS)
#error "p50_local_transport_test must compile with its test-only transport seam"
#endif

#include <array>
#include <cassert>
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
    Connection client = connect_unix(path, &status);
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
    ::close(listener);
    ::unlink(path.c_str());
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
        handshake_identity();
        truncated_read();
        credentials();
        unix_setup();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "p50localtransport: %s\n", error.what());
        return EXIT_FAILURE;
    }
    std::puts("p50localtransport: ok");
    return EXIT_SUCCESS;
}
