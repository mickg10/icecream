#include "cache/p50_local_transport.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include <sys/socket.h>

using namespace icecc::p50::local;

namespace {

void check(bool value, const char* expression) {
    if (!value)
        throw std::runtime_error(expression);
}

#define CHECK(value) check((value), #value)

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
    SingleWriter writer(sockets[0]);
    CHECK(writer.send(source) == Status::Ok);
    CHECK(read_frame(sockets[1], decoded) == Status::Ok);
    CHECK(decoded == source);
    ::close(sockets[0]);
    ::close(sockets[1]);
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
    const std::string path = "/tmp/icecc-p50-local-transport-" + std::to_string(::getpid());
    ::unlink(path.c_str());
    Status status = Status::InvalidArgument;
    const int listener = listen_unix(path, 2, &status);
    CHECK(listener >= 0);
    CHECK(status == Status::Ok);

    int accepted = -1;
    Status accept_status = Status::InvalidArgument;
    std::thread accept_thread([&] {
        accepted = accept_unix(listener, &accept_status);
    });
    const int client = connect_unix(path, &status);
    CHECK(client >= 0);
    accept_thread.join();
    CHECK(accepted >= 0);
    CHECK(accept_status == Status::Ok);

    const Frame hello = make_hello(PeerRole::Sidecar, Identity{1, 1});
    CHECK(write_frame(client, hello) == Status::Ok);
    Frame received;
    CHECK(read_frame(accepted, received) == Status::Ok);
    CHECK(received == hello);
    ::close(client);
    ::close(accepted);
    ::close(listener);
    ::unlink(path.c_str());

    CHECK(listen_unix(std::string(kMaxUnixPath + 1, 'x'), 1, &status) < 0);
    CHECK(status == Status::InvalidPath);
}

} // namespace

int main() {
    try {
        framing_and_limits();
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
