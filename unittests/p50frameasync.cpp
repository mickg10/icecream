#include "../cache/p50_local_transport.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>

using namespace icecc::p50::local;
using Clock = std::chrono::steady_clock;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main() {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 2;
    Connection reader(pair[0]), writer(pair[1]);
    const Frame expected = make_hello(PeerRole::Daemon, {17, 29});
    const auto bytes = encode_frame(expected);
    FrameOperation read(reader, Clock::now() + std::chrono::seconds(5));
    read.advance();
    CHECK(!read.done());
    CHECK(read.poll_events() == POLLIN);
    for (uint8_t byte : bytes) {
        CHECK(::send(writer.native_handle(), &byte, 1, 0) == 1);
        read.advance();
    }
    CHECK(read.done() && read.status() == Status::Ok);
    CHECK(read.frame() == expected);
    {
        FrameOperation write(writer, expected, Clock::now() + std::chrono::seconds(5));
        FrameOperation competing(writer, expected, Clock::now() + std::chrono::seconds(5));
        CHECK(competing.done() && competing.status() == Status::Busy);
        CHECK(writer.valid());
        write.advance();
        CHECK(write.done() && write.status() == Status::Ok);
    }
    FrameOperation second(reader, Clock::now() + std::chrono::seconds(5));
    second.advance();
    second.advance();
    CHECK(second.done() && second.frame() == expected);
    {
        FrameOperation expired(reader, Clock::now());
        expired.advance();
        CHECK(expired.done() && expired.status() == Status::Timeout);
        CHECK(!reader.valid());
    }
    // A malformed header fails before allocating its claimed payload.
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 2;
    {
        Connection input(pair[0]), output(pair[1]);
        auto malformed = bytes;
        malformed[0] ^= 1;
        CHECK(::send(output.native_handle(), malformed.data(), malformed.size(), 0)
              == static_cast<ssize_t>(malformed.size()));
        FrameOperation operation(input, Clock::now() + std::chrono::seconds(5));
        operation.advance();
        CHECK(operation.done() && operation.status() == Status::Malformed);
        CHECK(!input.valid());
    }
    // Cancellation closes a partially consumed relationship, never resetting
    // its cursor and permitting the remaining bytes to become another frame.
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 2;
    {
        Connection input(pair[0]), output(pair[1]);
        FrameOperation operation(input, Clock::now() + std::chrono::seconds(5));
        CHECK(::send(output.native_handle(), bytes.data(), 3, 0) == 3);
        operation.advance();
        CHECK(!operation.done());
        operation.cancel();
        CHECK(operation.done() && !input.valid());
    }
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 2;
    {
        Connection input(pair[0]), output(pair[1]);
        int small = 1024;
        CHECK(::setsockopt(output.native_handle(), SOL_SOCKET, SO_SNDBUF,
                          &small, sizeof(small)) == 0);
        Frame large{kProtocolVersion, MessageType::Data, {3, 4},
                    std::vector<uint8_t>(kMaxFramePayload, 0x5a)};
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        FrameOperation send(output, large, deadline);
        send.advance();
        CHECK(!send.done());
        // A full socket yields immediately, retaining both cursor and gate.
        for (int i = 0; i < 10; ++i) send.advance();
        CHECK(!send.done());
        CHECK(output.send_until(expected, deadline) == Status::Busy);
        FrameOperation receive(input, deadline);
        for (int i = 0; i < 10000 && (!send.done() || !receive.done()); ++i) {
            receive.advance();
            send.advance();
        }
        CHECK(send.done() && send.status() == Status::Ok);
        CHECK(receive.done() && receive.status() == Status::Ok);
        CHECK(receive.frame() == large);
    }
    for (size_t prefix : {size_t{0}, size_t{3}, kFrameHeaderSize}) {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) return 2;
        Connection input(pair[0]);
        if (prefix) CHECK(::send(pair[1], bytes.data(), prefix, 0)
                          == static_cast<ssize_t>(prefix));
        ::close(pair[1]);
        FrameOperation receive(input, Clock::now() + std::chrono::seconds(5));
        for (int i = 0; i < 3 && !receive.done(); ++i) receive.advance();
        CHECK(receive.done());
        CHECK(receive.status() == (prefix ? Status::Truncated : Status::CleanEof));
    }
    std::string temporary = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
        + "/p50async.XXXXXX";
    CHECK(::mkdtemp(temporary.data()) != nullptr);
    const std::string socket_path = temporary + "/control.sock";
    const int listener = listen_unix(socket_path, 2);
    CHECK(listener >= 0);
    {
        UnixConnectOperation connect(socket_path, Clock::now() + std::chrono::seconds(5));
        connect.advance();
        if (!connect.done()) {
            pollfd pfd{connect.poll_fd(), connect.poll_events(), 0};
            CHECK(::poll(&pfd, 1, 1000) == 1);
            connect.advance(pfd.revents);
        }
        CHECK(connect.done() && connect.status() == Status::Ok);
        Connection peer = connect.take_connection();
        CHECK(peer.valid() && peer.cloexec());
        CHECK((::fcntl(peer.native_handle(), F_GETFL) & O_NONBLOCK) == 0);
        Connection accepted = accept_unix(listener);
        CHECK(accepted.valid());
        CHECK(!connect.take_connection().valid());
    }
    {
        UnixConnectOperation expired(socket_path, Clock::now());
        expired.advance();
        CHECK(expired.done() && expired.status() == Status::Timeout);
        CHECK(!expired.take_connection().valid());
        UnixConnectOperation missing(temporary + "/absent", Clock::now() + std::chrono::seconds(5));
        missing.advance();
        CHECK(missing.done() && missing.status() == Status::InvalidPath);
    }
    ::close(listener);
    CHECK(::unlink(socket_path.c_str()) == 0);
    CHECK(::rmdir(temporary.c_str()) == 0);
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
