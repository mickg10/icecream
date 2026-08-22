/*
   Discriminators for the daemon's legacy FileChunk/End input source.  The
   source is driven through real MsgChannel buffering; only compiler-stdin
   writes are scripted so short writes and EINTR are deterministic.
*/

#include "comm.h"
#include "compiler_input.h"
#include "workit.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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

struct WriteAction
{
    ssize_t result;
    int error;
};

class ScriptedLegacyChunkSource : public LegacyChunkSource
{
public:
    ScriptedLegacyChunkSource(MsgChannel *channel, int client_fd)
        : LegacyChunkSource(channel, client_fd)
    {
    }

    std::deque<WriteAction> actions;
    std::vector<unsigned char> written;

protected:
    ssize_t write_bytes(int, const void *buffer, std::size_t size) override
    {
        if (actions.empty()) {
            std::fprintf(stderr, "scripted writer exhausted\n");
            std::exit(2);
        }
        const WriteAction action = actions.front();
        actions.pop_front();
        if (action.result < 0) {
            errno = action.error;
            return -1;
        }
        const std::size_t count = std::min(size, static_cast<std::size_t>(action.result));
        const unsigned char *bytes = static_cast<const unsigned char *>(buffer);
        written.insert(written.end(), bytes, bytes + count);
        return static_cast<ssize_t>(count);
    }
};

bool wait_for_read(LegacyChunkSource &source, unsigned int stats[],
                   CompilerInputReadResult wanted)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        const CompilerInputReadResult result = source.read_next(stats);
        if (result == wanted) {
            return true;
        }
        if (result != CompilerInputReadResult::NoMessage) {
            return false;
        }
        usleep(1000);
    }
    return false;
}

void send_chunk(MsgChannel *channel, const std::string &bytes)
{
    FileChunkMsg chunk(reinterpret_cast<unsigned char *>(const_cast<char *>(bytes.data())),
                       bytes.size());
    if (!channel->send_msg(chunk)) {
        std::fprintf(stderr, "could not send test FileChunkMsg\n");
        std::exit(2);
    }
}

void test_buffering_short_writes_end_and_accounting()
{
    ChannelPair pair = make_channel_pair();
    const std::string payload = "abcdef";
    send_chunk(pair.sender, payload);
    REQUIRE(pair.sender->send_msg(EndMsg()), "test queues EndMsg behind FileChunkMsg");

    unsigned int stats[8] = {};
    ScriptedLegacyChunkSource source(pair.receiver, pair.receiver->fd);
    REQUIRE(wait_for_read(source, stats, CompilerInputReadResult::Chunk),
            "legacy source reads the buffered FileChunkMsg first");
    REQUIRE(source.has_pending() && source.pending_size() == payload.size(),
            "legacy source retains one pending chunk");
    REQUIRE(stats[JobStatistics::in_uncompressed] == source.pending_size(),
            "uncompressed accounting uses the received FileChunkMsg length exactly");
    REQUIRE(stats[JobStatistics::in_compressed] == source.pending_compressed_size(),
            "compressed accounting uses the received FileChunkMsg value exactly");
    REQUIRE(source.read_next(stats) == CompilerInputReadResult::NoMessage,
            "buffered EndMsg is not read ahead of pending compiler input");
    REQUIRE(!source.complete(), "input stays incomplete while a chunk is pending");

    source.actions.push_back({ 2, 0 });
    source.actions.push_back({ 4, 0 });
    REQUIRE(source.write_pending(-1) == CompilerInputWriteResult::Pending,
            "short compiler-stdin write retains the unwritten suffix");
    REQUIRE(source.pending_offset() == 2 && source.has_pending(),
            "short write advances only the legacy partial-write offset");
    REQUIRE(source.write_pending(-1) == CompilerInputWriteResult::ChunkComplete,
            "second compiler-stdin write completes the chunk");
    REQUIRE(!source.has_pending() && source.written
            == std::vector<unsigned char>(payload.begin(), payload.end()),
            "partial writes deliver each input byte once and in order");

    REQUIRE(wait_for_read(source, stats, CompilerInputReadResult::End),
            "legacy source observes EndMsg only after chunk completion");
    REQUIRE(source.complete() && !source.has_pending(),
            "EndMsg marks complete input with no pending data");

    delete pair.sender;
    delete pair.receiver;
}

void test_empty_and_post_end_message()
{
    ChannelPair pair = make_channel_pair();
    REQUIRE(pair.sender->send_msg(EndMsg()), "empty input queues only EndMsg");
    REQUIRE(pair.sender->send_msg(PingMsg()), "test queues a message after EndMsg");

    unsigned int stats[8] = {};
    LegacyChunkSource source(pair.receiver, pair.receiver->fd);
    REQUIRE(wait_for_read(source, stats, CompilerInputReadResult::End),
            "empty input completes without a FileChunkMsg");
    REQUIRE(stats[JobStatistics::in_uncompressed] == 0
                && stats[JobStatistics::in_compressed] == 0,
            "empty input retains zero legacy byte accounting");
    REQUIRE(wait_for_read(source, stats, CompilerInputReadResult::MessageAfterEnd),
            "a buffered post-End message retains the cancellation discriminator");

    delete pair.sender;
    delete pair.receiver;
}

void test_discard_continues_buffered_input()
{
    ChannelPair pair = make_channel_pair();
    send_chunk(pair.sender, "discard me");
    REQUIRE(pair.sender->send_msg(EndMsg()), "early-compiler-exit case queues EndMsg");

    unsigned int stats[8] = {};
    LegacyChunkSource source(pair.receiver, pair.receiver->fd);
    REQUIRE(wait_for_read(source, stats, CompilerInputReadResult::Chunk),
            "early-compiler-exit case receives a chunk");
    const unsigned int accounted = stats[JobStatistics::in_uncompressed];
    source.discard_pending();
    REQUIRE(!source.has_pending(), "closed compiler stdin discards only the pending chunk");
    REQUIRE(wait_for_read(source, stats, CompilerInputReadResult::End),
            "legacy source continues draining buffered input after discard");
    REQUIRE(stats[JobStatistics::in_uncompressed] == accounted,
            "discard does not alter already-recorded input accounting");

    delete pair.sender;
    delete pair.receiver;
}

void test_interrupted_failed_and_zero_progress_writes()
{
    ChannelPair pair = make_channel_pair();
    send_chunk(pair.sender, "write failure");

    unsigned int stats[8] = {};
    ScriptedLegacyChunkSource source(pair.receiver, pair.receiver->fd);
    REQUIRE(wait_for_read(source, stats, CompilerInputReadResult::Chunk),
            "write-error case receives a chunk");
    source.actions.push_back({ -1, EINTR });
    REQUIRE(source.write_pending(-1) == CompilerInputWriteResult::Interrupted,
            "EINTR retains the pending compiler input");
    REQUIRE(source.has_pending() && source.pending_offset() == 0,
            "EINTR leaves the partial-write offset unchanged");
    source.actions.push_back({ 1, 0 });
    REQUIRE(source.write_pending(-1) == CompilerInputWriteResult::Pending,
            "write-error case first makes partial progress");
    source.actions.push_back({ -1, EPIPE });
    REQUIRE(source.write_pending(-1) == CompilerInputWriteResult::Failed,
            "non-EINTR compiler-stdin error retains the legacy failure result");
    REQUIRE(!source.has_pending() && source.pending_offset() == 0,
            "compiler-stdin error discards the pending legacy chunk");

    send_chunk(pair.sender, "zero progress");
    REQUIRE(wait_for_read(source, stats, CompilerInputReadResult::Chunk),
            "zero-progress case receives another chunk");
    source.actions.push_back({ 0, 0 });
    REQUIRE(source.write_pending(-1) == CompilerInputWriteResult::Failed,
            "zero-byte write for nonempty compiler input is terminal");
    REQUIRE(!source.has_pending() && source.pending_offset() == 0,
            "zero-progress failure cannot leave a busy-looping pending chunk");

    delete pair.sender;
    delete pair.receiver;
}

void test_unexpected_message_and_eof()
{
    {
        ChannelPair pair = make_channel_pair();
        REQUIRE(pair.sender->send_msg(PingMsg()), "unexpected-message case queues PingMsg");
        unsigned int stats[8] = {};
        LegacyChunkSource source(pair.receiver, pair.receiver->fd);
        REQUIRE(wait_for_read(source, stats, CompilerInputReadResult::UnexpectedMessage),
                "non-FileChunk/End input retains the protocol-error discriminator");
        REQUIRE(source.complete(), "unexpected input marks the source complete");
        source.disable();
        REQUIRE(source.poll_fd() == -1, "disabled legacy source leaves the fd unowned");
        delete pair.sender;
        delete pair.receiver;
    }

    {
        ChannelPair pair = make_channel_pair();
        delete pair.sender;
        pair.sender = nullptr;
        unsigned int stats[8] = {};
        LegacyChunkSource source(pair.receiver, pair.receiver->fd);
        REQUIRE(wait_for_read(source, stats, CompilerInputReadResult::UnexpectedEof),
                "peer EOF before EndMsg retains the input-error discriminator");
        REQUIRE(source.complete(), "unexpected EOF marks the source complete");
        delete pair.receiver;
    }
}

}

int main()
{
    test_buffering_short_writes_end_and_accounting();
    test_empty_and_post_end_message();
    test_discard_continues_buffered_input();
    test_interrupted_failed_and_zero_progress_writes();
    test_unexpected_message_and_eof();
    return failures == 0 ? 0 : 1;
}
