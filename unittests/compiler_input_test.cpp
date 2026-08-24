/*
   Discriminators for the daemon's legacy FileChunk/End input source.  The
   source is driven through real MsgChannel buffering; only compiler-stdin
   writes are scripted so short writes and EINTR are deterministic.
*/

#include "comm.h"
#include "compiler_input.h"
#include "digest128.h"
#include "workit.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>
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

class ScriptedP50AttachedFileSource : public P50AttachedFileSource
{
public:
    ScriptedP50AttachedFileSource(int fd, uint64_t expected_bytes,
                                 std::array<uint8_t, 16> expected_digest)
        : P50AttachedFileSource(fd, expected_bytes, expected_digest)
    {
    }

    std::deque<WriteAction> actions;
    std::vector<unsigned char> written;

protected:
    ssize_t write_bytes(int, const void *buffer, std::size_t size) override
    {
        if (actions.empty()) {
            std::fprintf(stderr, "scripted P50 writer exhausted\n");
            std::exit(2);
        }
        const WriteAction action = actions.front();
        actions.pop_front();
        if (action.result < 0) {
            errno = action.error;
            return -1;
        }
        const std::size_t count =
            std::min(size, static_cast<std::size_t>(action.result));
        const unsigned char *bytes = static_cast<const unsigned char *>(buffer);
        written.insert(written.end(), bytes, bytes + count);
        return static_cast<ssize_t>(count);
    }
};

int immutable_input_fd(const std::vector<unsigned char> &bytes, bool add_seals = true,
                       bool reopen_read_only = true)
{
#if defined(MFD_ALLOW_SEALING) && defined(F_ADD_SEALS)
    const int writable = memfd_create("icecc-p50-compiler-input",
                                      MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (writable < 0)
        return -1;
    size_t offset = 0;
    while (offset != bytes.size()) {
        const ssize_t result = write(writable, bytes.data() + offset,
                                     bytes.size() - offset);
        if (result > 0)
            offset += static_cast<size_t>(result);
        else if (result < 0 && errno == EINTR)
            continue;
        else {
            close(writable);
            return -1;
        }
    }
    if (add_seals && fcntl(writable, F_ADD_SEALS,
                           F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK |
                               F_SEAL_SEAL) != 0) {
        close(writable);
        return -1;
    }
    if (!reopen_read_only) {
        lseek(writable, 0, SEEK_SET);
        return writable;
    }
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/fd/%d", writable);
    const int readonly = open(path, O_RDONLY | O_CLOEXEC);
    close(writable);
    return readonly;
#else
    (void)bytes;
    (void)add_seals;
    (void)reopen_read_only;
    return -1;
#endif
}

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

void test_p50_immutable_cursor_and_accounting()
{
    std::vector<unsigned char> payload(100006);
    for (size_t index = 0; index != payload.size(); ++index)
        payload[index] = static_cast<unsigned char>((index * 37u + 11u) & 0xffu);
    const icecc::Digest128 digest = icecc::digest128(payload);
    const int fd = immutable_input_fd(payload);
    REQUIRE(fd >= 0, "test creates a sealed read-only P50 InputRecord cursor");
    if (fd < 0)
        return;
    REQUIRE(lseek(fd, 17, SEEK_SET) == 17,
            "test perturbs the received cursor before compiler attachment");
    unsigned int stats[8] = {};
    ScriptedP50AttachedFileSource source(fd, payload.size(), digest.bytes);
    REQUIRE(source.poll_fd() >= 0 && !source.complete(),
            "validated P50 source owns one live independent descriptor");
    REQUIRE(source.read_next(stats) == CompilerInputReadResult::Chunk
                && source.pending_size() == 100000,
            "P50 source rewinds to byte zero and reads its first exact chunk");
    REQUIRE(source.pending_compressed_size() == 0
                && stats[JobStatistics::in_uncompressed] == 100000
                && stats[JobStatistics::in_compressed] == 0,
            "P50 compiler accounting never fabricates cache-wire encoded bytes");
    source.actions.push_back({3, 0});
    source.actions.push_back({99997, 0});
    REQUIRE(source.write_pending(-1) == CompilerInputWriteResult::Pending
                && source.pending_offset() == 3,
            "P50 compiler cursor retains a short-write suffix exactly");
    REQUIRE(source.write_pending(-1) == CompilerInputWriteResult::ChunkComplete,
            "P50 compiler cursor completes the first chunk without duplication");
    REQUIRE(source.read_next(stats) == CompilerInputReadResult::Chunk
                && source.pending_size() == 6,
            "P50 compiler cursor reads the exact final chunk");
    source.actions.push_back({6, 0});
    REQUIRE(source.write_pending(-1) == CompilerInputWriteResult::ChunkComplete,
            "P50 compiler cursor writes the exact final chunk");
    REQUIRE(source.read_next(stats) == CompilerInputReadResult::End
                && source.complete() && source.poll_fd() == -1,
            "P50 compiler cursor closes only at its validated exact EOF");
    REQUIRE(source.written == payload
                && stats[JobStatistics::in_uncompressed] == payload.size(),
            "compiler observes every committed InputRecord byte once from byte zero");
}

void test_p50_descriptor_and_identity_rejections()
{
    const std::vector<unsigned char> payload{'e', 'x', 'a', 'c', 't'};
    const icecc::Digest128 digest = icecc::digest128(payload);

    auto rejected = [&](int fd, uint64_t bytes,
                        std::array<uint8_t, 16> expected) {
        try {
            P50AttachedFileSource source(fd, bytes, expected);
        } catch (const std::exception &) {
            return true;
        }
        return false;
    };

    std::array<uint8_t, 16> wrong_digest = digest.bytes;
    wrong_digest[0] ^= 1;
    REQUIRE(rejected(immutable_input_fd(payload), payload.size(), wrong_digest),
            "P50 compiler attachment rejects a digest mismatch before compiler fork");
    REQUIRE(rejected(immutable_input_fd(payload), payload.size() + 1, digest.bytes),
            "P50 compiler attachment rejects a length mismatch before compiler fork");
    REQUIRE(rejected(immutable_input_fd(payload, false), payload.size(), digest.bytes),
            "P50 compiler attachment rejects a mutable descriptor");
    REQUIRE(rejected(immutable_input_fd(payload, true, false), payload.size(), digest.bytes),
            "P50 compiler attachment rejects a writable descriptor even when sealed");
    REQUIRE(rejected(-1, payload.size(), digest.bytes),
            "P50 compiler attachment rejects an absent descriptor without legacy fallback");
}

}

int main()
{
    test_buffering_short_writes_end_and_accounting();
    test_empty_and_post_end_message();
    test_discard_continues_buffered_input();
    test_interrupted_failed_and_zero_progress_writes();
    test_unexpected_message_and_eof();
    test_p50_immutable_cursor_and_accounting();
    test_p50_descriptor_and_identity_rejections();
    return failures == 0 ? 0 : 1;
}
