/*
   Discriminators for the legacy remote-input attachment seam.  These tests use
   real MsgChannel framing so the refactor cannot silently change chunk sizes,
   message order, or empty-stream behavior.
*/

#include "client.h"
#include "comm.h"
#include "input_pump.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{

int failures = 0;

#define REQUIRE(condition, description)                                      \
    do {                                                                     \
        if (condition) {                                                     \
            std::fprintf(stderr, "ok       - %s\n", description);          \
        } else {                                                             \
            std::fprintf(stderr, "FAILED   - %s (at %s:%d)\n", description, \
                         __FILE__, __LINE__);                                \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

struct ChannelPair
{
    MsgChannel *sender = nullptr;
    MsgChannel *receiver = nullptr;
};

ChannelPair make_channel_pair()
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::perror("socketpair");
        std::exit(2);
    }

    struct sockaddr_un address;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;

    ChannelPair pair;
    std::thread first([&] {
        pair.sender = Service::createChannel(fds[0],
                                              reinterpret_cast<struct sockaddr *>(&address),
                                              sizeof(address));
    });
    std::thread second([&] {
        pair.receiver = Service::createChannel(fds[1],
                                                reinterpret_cast<struct sockaddr *>(&address),
                                                sizeof(address));
    });
    first.join();
    second.join();
    if (!pair.sender || !pair.receiver) {
        std::fprintf(stderr, "could not establish MsgChannel pair\n");
        std::exit(2);
    }
    return pair;
}

int input_fd(const std::vector<unsigned char> &bytes)
{
    char path[] = "/tmp/icecc-input-pump-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        std::perror("mkstemp");
        std::exit(2);
    }
    unlink(path);

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            std::perror("write temporary input");
            std::exit(2);
        }
        offset += static_cast<std::size_t>(written);
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        std::perror("lseek temporary input");
        std::exit(2);
    }
    return fd;
}

std::vector<unsigned char> input_bytes(std::size_t size)
{
    std::vector<unsigned char> result(size);
    for (std::size_t i = 0; i < size; ++i) {
        result[i] = static_cast<unsigned char>((i * 131U + i / 251U) & 0xffU);
    }
    return result;
}

struct Capture
{
    std::vector<Msg::Value> types;
    std::vector<std::size_t> chunk_sizes;
    std::vector<unsigned char> bytes;
    bool timed_out = false;
};

Capture capture_to_end(MsgChannel *channel)
{
    Capture capture;
    for (;;) {
        Msg *message = channel->get_msg(10, true);
        if (!message) {
            capture.timed_out = true;
            return capture;
        }
        capture.types.push_back(static_cast<Msg::Value>(*message));
        if (*message == Msg::FILE_CHUNK) {
            FileChunkMsg *chunk = static_cast<FileChunkMsg *>(message);
            capture.chunk_sizes.push_back(chunk->len);
            capture.bytes.insert(capture.bytes.end(), chunk->buffer, chunk->buffer + chunk->len);
        }
        const bool ended = *message == Msg::END;
        delete message;
        if (ended) {
            return capture;
        }
    }
}

void test_size(std::size_t size, const std::vector<std::size_t> &expected_chunks)
{
    ChannelPair pair = make_channel_pair();
    LegacyRemoteSink sink(pair.sender);
    const std::vector<unsigned char> expected = input_bytes(size);
    const int fd = input_fd(expected);
    Capture capture;
    std::thread receiver([&] { capture = capture_to_end(pair.receiver); });

    sink.send_fd(fd);
    REQUIRE(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
            "legacy sink closes the input descriptor after EOF");
    REQUIRE(sink.send_end(), "legacy sink queues EndMsg after input chunks");
    receiver.join();

    REQUIRE(!capture.timed_out, "receiver reaches EndMsg");
    REQUIRE(capture.chunk_sizes == expected_chunks,
            "legacy sink retains exact 100000-byte chunk boundaries");
    REQUIRE(capture.bytes == expected, "legacy sink retains exact input bytes");
    REQUIRE(!capture.types.empty() && capture.types.back() == Msg::END,
            "EndMsg is last on the legacy input stream");
    for (std::size_t i = 0; i + 1 < capture.types.size(); ++i) {
        REQUIRE(capture.types[i] == Msg::FILE_CHUNK,
                "every message before EndMsg is FileChunkMsg");
    }

    delete pair.sender;
    delete pair.receiver;
}

void test_boundaries_and_empty_input()
{
    test_size(0, {});
    test_size(99999, { 99999 });
    test_size(100000, { 100000 });
    test_size(100001, { 100000, 1 });
    test_size(200000, { 100000, 100000 });
}

void test_read_error_closes_fd()
{
    ChannelPair pair = make_channel_pair();
    LegacyRemoteSink sink(pair.sender);
    const int fd = open(".", O_RDONLY | O_DIRECTORY);
    REQUIRE(fd >= 0, "directory descriptor available for read-error discriminator");
    try {
        sink.send_fd(fd);
        REQUIRE(false, "read error raises client_error");
    } catch (const client_error &error) {
        REQUIRE(error.errorCode == 16, "read error retains legacy error code 16");
    }
    REQUIRE(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
            "read error retains legacy descriptor closure");
    delete pair.sender;
    delete pair.receiver;
}

void test_send_error_closes_fd()
{
    ChannelPair pair = make_channel_pair();
    delete pair.receiver;
    pair.receiver = nullptr;
    LegacyRemoteSink sink(pair.sender);
    const int fd = input_fd(input_bytes(1));
    try {
        sink.send_fd(fd);
        REQUIRE(false, "closed peer raises client_error");
    } catch (const client_error &error) {
        REQUIRE(error.errorCode == 15, "send error retains legacy error code 15");
    }
    REQUIRE(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
            "send error retains legacy descriptor closure");
    delete pair.sender;
}

void test_status_response_during_send_failure_closes_fd()
{
    ChannelPair pair = make_channel_pair();
    REQUIRE(pair.receiver->send_msg(StatusTextMsg("remote rejected source")),
            "peer queues a terminal STATUS_TEXT before refusing source bytes");
    REQUIRE(shutdown(pair.receiver->fd, SHUT_RD) == 0,
            "peer refuses subsequent source bytes while retaining its response stream");

    LegacyRemoteSink sink(pair.sender);
    const int fd = input_fd(input_bytes(1));
    try {
        sink.send_fd(fd);
        REQUIRE(false, "terminal STATUS_TEXT raises client_error");
    } catch (const client_error &error) {
        REQUIRE(error.errorCode == 23,
                "terminal STATUS_TEXT retains legacy remote-status error code 23");
    }
    REQUIRE(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
            "terminal STATUS_TEXT cannot bypass owned descriptor closure");

    delete pair.sender;
    delete pair.receiver;
}

}

int main()
{
    test_boundaries_and_empty_input();
    test_read_error_closes_fd();
    test_send_error_closes_fd();
    test_status_response_during_send_failure_closes_fd();
    return failures == 0 ? 0 : 1;
}
