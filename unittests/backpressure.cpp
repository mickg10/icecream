/*
    Red/green tests for MsgChannel behaviour under send-side backpressure
    (https://github.com/mickgvirtu/icecream issue #1), plus a regression
    test for the monitor message wire tags.

    The core contract exercised by "contract":
      A dispatch-style send against a peer whose receive buffer is full must
      not destroy the channel, and once the peer drains, the follow-up
      dispatch message (USE_CS) must arrive intact.

    The test adapts to the implementation variant it is compiled against:
      - ICECC_MSGCHANNEL_HAS_DEFERRED_SEND (defined by comm.h on the fixed
        branch): dispatch sends use SendNonBlocking|SendDeferrable and the
        pending buffer is drained with flush_pending().
      - ICECC_TEST_FIXBRANCH: only meaningful when this source is compiled
        against the fix/submitter-requeue-on-transient-timeout branch's
        libicecc (whose MsgChannel has clear_writebuf()); it selects that
        branch's prescribed recovery -- clear_writebuf() + re-send of a fresh
        dispatch message after the blocking-send timeout.  Defining it while
        building against any other branch is a compile error by design.
        Cross-branch red run:
          g++ -std=c++17 -DICECC_TEST_FIXBRANCH -I<thatbranch>/services \
              backpressure.cpp <thatbranch>/services/.libs/libicecc.a ...
      - otherwise (base): plain blocking send, no recovery API.

    Every variant is checked against the same assertions.  Exit code 0 on
    pass, 1 on failure.
*/

#include "comm.h"
#include "logging.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;

#define REQUIRE(cond, what)                                             \
    do {                                                                \
        if (cond) {                                                     \
            fprintf(stderr, "ok       - %s\n", what);                   \
        } else {                                                        \
            fprintf(stderr, "FAILED   - %s (at %s:%d)\n", what,         \
                    __FILE__, __LINE__);                                \
            ++failures;                                                 \
        }                                                               \
    } while (0)

struct ChannelPair
{
    MsgChannel *snd = nullptr;
    MsgChannel *rcv = nullptr;
};

static void test_message_type_names()
{
    const Msg unknown(Msg::UNKNOWN);
    REQUIRE(unknown.to_string() == "UNKNOWN",
            "explicit UNKNOWN message type has a stable name");

    const Msg invalid(static_cast<Msg::Value>(0xffffffffu));
    REQUIRE(invalid.to_string() == "UNKNOWN",
            "out-of-range message type falls back to UNKNOWN");
}

// socketpair-backed MsgChannel pair; sndbuf_bytes > 0 shrinks the sender-side
// socket buffer so a large message reliably jams mid-transmission.
static ChannelPair make_channel_pair(int sndbuf_bytes)
{
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0) {
        perror("socketpair");
        exit(2);
    }
    if (sndbuf_bytes > 0) {
        setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf_bytes, sizeof(sndbuf_bytes));
        setsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &sndbuf_bytes, sizeof(sndbuf_bytes));
        // The clog scenarios need the kernel to actually honour small
        // buffers; if it will not (non-Linux AF_UNIX accounting, clamping),
        // a 64KiB send would complete and every jam-dependent assertion
        // would be a false red.  Skip (automake exit 77) instead.
        int eff_snd = 0, eff_rcv = 0;
        socklen_t l = sizeof(eff_snd);
        getsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &eff_snd, &l);
        l = sizeof(eff_rcv);
        getsockopt(fds[1], SOL_SOCKET, SO_RCVBUF, &eff_rcv, &l);
        // 64KiB = the largest jam message the suite sends; if the kernel
        // kept more combined buffering than that, the send would complete
        // and every jam assertion would be a false red.
        if (eff_snd + eff_rcv >= 64 * 1024) {
            fprintf(stderr, "SKIP     - cannot shrink socket buffers "
                    "(effective %d+%d bytes); backpressure untestable here\n",
                    eff_snd, eff_rcv);
            exit(77);
        }
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;

    ChannelPair p;
    // createChannel() completes the protocol handshake synchronously, so the
    // two ends have to be brought up concurrently.
    std::thread ta([&] {
        p.snd = Service::createChannel(fds[0], (struct sockaddr *)&sa, sizeof(sa));
    });
    std::thread tb([&] {
        p.rcv = Service::createChannel(fds[1], (struct sockaddr *)&sa, sizeof(sa));
    });
    ta.join();
    tb.join();
    if (!p.snd || !p.rcv) {
        fprintf(stderr, "FAILED   - could not establish channel pair\n");
        exit(2);
    }
    return p;
}

static const unsigned int kJobId = 4711;
static const char *kUseCsHost = "fake-cs.example";
static const unsigned int kUseCsPort = 10245;

// Drain the receiver for up to ~deadline seconds; remember every message.
struct DrainResult
{
    bool got_big_intact = false;
    bool got_use_cs = false;
    bool use_cs_intact = false;
    std::vector<std::string> seen;
};

static DrainResult drain_receiver(MsgChannel *rcv, const std::string &big_payload, int deadline_s)
{
    DrainResult r;
    time_t start = time(nullptr);
    while (time(nullptr) - start <= deadline_s) {
        Msg *m = rcv->get_msg(2, true);
        if (!m) {
            if (rcv->at_eof()) {
                r.seen.push_back("<eof>");
                break;
            }
            continue;     // maybe just a timeout, keep waiting until deadline
        }
        r.seen.push_back(m->to_string());
        if (*m == Msg::STATUS_TEXT) {
            StatusTextMsg *st = dynamic_cast<StatusTextMsg *>(m);
            if (st && st->text == big_payload) {
                r.got_big_intact = true;
            }
        } else if (*m == Msg::USE_CS) {
            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
            r.got_use_cs = true;
            r.use_cs_intact = u && u->job_id == kJobId && u->hostname == kUseCsHost
                              && u->port == kUseCsPort;
            delete m;
            break;
        }
        delete m;
    }
    return r;
}

// The core Issue-1 contract, at MsgChannel level.
static void test_contract()
{
    ChannelPair p = make_channel_pair(8 * 1024);

    // settle the handshake with one round-trip
    REQUIRE(p.snd->send_msg(PingMsg()), "handshake ping sent");
    Msg *m = p.rcv->get_msg(10);
    REQUIRE(m && *m == Msg::PING, "handshake ping received");
    delete m;

    // A message much larger than the socket buffer: guarantees the send jams
    // with part of the message already on the wire (the partial-send state a
    // clogged submitter produces at the moment its buffers fill up).
    const std::string big(64 * 1024, 'x');

    fprintf(stderr, "# clogging: sending 64KiB against an unread ~16KiB socket buffer\n");
    time_t t0 = time(nullptr);

#if defined(ICECC_MSGCHANNEL_HAS_DEFERRED_SEND)
    // Fixed variant: the dispatch path queues without blocking.
    bool sent = p.snd->send_msg(StatusTextMsg(big),
                                MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable);
    REQUIRE(sent, "deferrable send accepted while peer is clogged");
    REQUIRE(p.snd->has_pending_write(), "unsent remainder is queued");
    REQUIRE(time(nullptr) - t0 <= 2, "deferrable send did not block");
#else
    // Base / fix-branch variant: the dispatch path is a blocking send that
    // runs into the flush_writebuf() poll timeout (~30s).
    bool sent = p.snd->send_msg(StatusTextMsg(big));
    REQUIRE(!sent, "blocking send reports failure after send timeout");
#endif

    // The heart of Issue 1: transient backpressure must not kill the channel.
    REQUIRE(!p.snd->at_eof(), "channel still alive after backpressure");

    if (p.snd->at_eof()) {
        fprintf(stderr, "# channel poisoned; skipping delivery phase (it cannot pass)\n");
        delete p.snd;
        delete p.rcv;
        return;
    }

#if defined(ICECC_TEST_FIXBRANCH)
    // The fix branch's prescribed recovery: discard the stale unsent bytes,
    // then re-dispatch a fresh message once the scheduler retries the job.
    p.snd->clear_writebuf();
#endif

    // Peer starts draining now.  All flush loops are deadline-bounded so a
    // wedged receiver turns into a failed assertion, not a hung make check.
    std::thread sender([&] {
#if defined(ICECC_MSGCHANNEL_HAS_DEFERRED_SEND)
        time_t deadline = time(nullptr) + 30;
        // finish the queued big message, then dispatch USE_CS the same way
        while (p.snd->has_pending_write() && p.snd->flush_pending()
               && time(nullptr) < deadline) {
            usleep(2000);
        }
        bool ok = p.snd->send_msg(UseCSMsg("x86_64", kUseCsHost, kUseCsPort, kJobId, true, 1, 0),
                                  MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable);
        while (ok && p.snd->has_pending_write() && p.snd->flush_pending()
               && time(nullptr) < deadline) {
            usleep(2000);
        }
#else
        // blocking re-dispatch; completes as the peer drains
        p.snd->send_msg(UseCSMsg("x86_64", kUseCsHost, kUseCsPort, kJobId, true, 1, 0));
#endif
    });

    DrainResult r = drain_receiver(p.rcv, big, 20);
    sender.join();

    fprintf(stderr, "# receiver saw:");
    for (const std::string &s : r.seen) {
        fprintf(stderr, " %s", s.c_str());
    }
    fprintf(stderr, "\n");

    REQUIRE(r.got_use_cs && r.use_cs_intact,
            "re-dispatched USE_CS reached the peer intact after drain");
    REQUIRE(!p.snd->at_eof(), "sender channel alive at the end");
    REQUIRE(!p.rcv->at_eof(), "receiver channel alive at the end");

#if defined(ICECC_MSGCHANNEL_HAS_DEFERRED_SEND)
    // The fixed variant additionally must deliver the original message: the
    // scheduler never abandons a partially transmitted dispatch.
    REQUIRE(r.got_big_intact, "original jammed message delivered intact");
    REQUIRE(!p.snd->has_pending_write(), "write queue fully drained");
#endif

    delete p.snd;
    delete p.rcv;
}

#if defined(ICECC_MSGCHANNEL_HAS_DEFERRED_SEND)
// The production pattern empty_queue() relies on: many messages appended to a
// write buffer that already holds deferred bytes, then drained.  Ordering and
// intactness after the drain are the observable proxy for the msgofs == 0
// compaction invariant in flush_writebuf() and for send_msg()/flush_pending()
// interleaving.
static void test_multiqueue(int bufsize)
{
    fprintf(stderr, "# multiqueue with %d-byte socket buffers\n", bufsize);
    ChannelPair p = make_channel_pair(bufsize);

    REQUIRE(p.snd->send_msg(PingMsg()), "handshake ping sent");
    Msg *m = p.rcv->get_msg(10);
    REQUIRE(m && *m == Msg::PING, "handshake ping received");
    delete m;

    // Jam the channel with one message bigger than the socket buffers...
    const std::string big(32 * 1024, 'a');
    REQUIRE(p.snd->send_msg(StatusTextMsg(big),
                            MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable),
            "initial big send accepted");
    REQUIRE(p.snd->has_pending_write(), "big message left pending bytes");

    // ...then append a stream of distinct messages behind it, the way the
    // scheduler keeps dispatching UseCS replies to a clogged submitter.
    const int kFollowers = 20;
    bool all_queued = true;
    for (int i = 0; i < kFollowers; ++i) {
        std::string payload = "follower-" + std::to_string(i)
                              + "-" + std::string(64 + i, 'b');
        if (!p.snd->send_msg(StatusTextMsg(payload),
                             MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable)) {
            all_queued = false;
        }
    }
    REQUIRE(all_queued, "all follower sends accepted while clogged");
    REQUIRE(!p.snd->at_eof(), "channel alive with a deep pending queue");
    REQUIRE(p.snd->deferred_output_armed(),
            "deferred-output deadline is armed while backed up");
    REQUIRE(p.snd->deferred_output_deadline_msec() > icecream_monotonic_msec(),
            "armed deadline lies in the future");

    // Drain: flush pending from one side, read everything on the other.
    std::atomic<bool> flush_ok{true};
    std::thread sender([&] {
        time_t deadline = time(nullptr) + 30;
        while (p.snd->has_pending_write() && time(nullptr) < deadline) {
            if (!p.snd->flush_pending()) {
                flush_ok = false;
                return;
            }
            usleep(2000);
        }
    });

    int followers_in_order = 0;
    bool big_intact = false;
    int expected = 0;
    time_t start = time(nullptr);
    while (expected < kFollowers && time(nullptr) - start <= 20) {
        Msg *got = p.rcv->get_msg(2, true);
        if (!got) {
            if (p.rcv->at_eof()) {
                break;
            }
            continue;
        }
        StatusTextMsg *st = dynamic_cast<StatusTextMsg *>(got);
        if (st) {
            if (st->text == big) {
                big_intact = true;
            } else if (st->text.compare(0, 9, "follower-") == 0) {
                std::string prefix = "follower-" + std::to_string(expected) + "-";
                if (st->text.compare(0, prefix.size(), prefix) == 0
                        && st->text.size() == prefix.size() + 64 + expected) {
                    ++followers_in_order;
                }
                ++expected;
            }
        }
        delete got;
    }
    sender.join();

    REQUIRE(flush_ok.load(), "flush_pending never reported a dead connection");
    REQUIRE(big_intact, "jammed first message delivered intact");
    REQUIRE(followers_in_order == kFollowers,
            "all queued messages arrived intact and in send order");
    REQUIRE(!p.snd->has_pending_write(), "pending queue fully drained");
    REQUIRE(!p.snd->deferred_output_armed(),
            "deferred-output deadline disarmed after full drain");

    delete p.snd;
    delete p.rcv;
}

// The teardown trigger the scheduler relies on: flush_pending() returns false
// only when the connection actually died, and leaves the channel in the error
// state.  Also pins the edge guards (empty buffer, already-errored channel).
static void test_flush_pending_errors()
{
    // Empty buffer on a healthy channel: trivially true.
    {
        ChannelPair p = make_channel_pair(0);
        REQUIRE(!p.snd->has_pending_write(), "fresh channel has nothing pending");
        REQUIRE(p.snd->flush_pending(), "flush_pending on empty buffer succeeds");
        delete p.snd;
        delete p.rcv;
    }

    // Dead peer: jam bytes, close the receiver, flush must fail and poison.
    {
        ChannelPair p = make_channel_pair(8 * 1024);
        REQUIRE(p.snd->send_msg(StatusTextMsg(std::string(64 * 1024, 'x')),
                                MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable),
                "big deferrable send accepted");
        REQUIRE(p.snd->has_pending_write(), "bytes pending before peer death");

        delete p.rcv;    // closes the receiving end

        bool flushed_false = false;
        for (int i = 0; i < 100; ++i) {
            if (!p.snd->flush_pending()) {
                flushed_false = true;
                break;
            }
            usleep(10 * 1000);
        }
        REQUIRE(flushed_false, "flush_pending reports the dead connection");
        REQUIRE(p.snd->at_eof(), "channel is in the error state afterwards");
        // Error state is sticky: no further pretence of usability.
        REQUIRE(!p.snd->flush_pending(), "flush_pending on errored channel stays false");
        REQUIRE(!p.snd->send_msg(PingMsg(),
                                 MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable),
                "send_msg on errored channel fails");
        delete p.snd;
    }
}
#endif // ICECC_MSGCHANNEL_HAS_DEFERRED_SEND

// Non-deferrable senders must keep the old fail-fast semantics: monitors are
// dropped on backpressure (scheduler notify_monitors uses SendNonBlocking
// alone), so a clogged non-deferrable send has to fail and poison the channel
// rather than silently queue.
static void test_nondeferrable_fail_fast()
{
    ChannelPair p = make_channel_pair(8 * 1024);

    REQUIRE(p.snd->send_msg(PingMsg()), "handshake ping sent");
    Msg *m = p.rcv->get_msg(10);
    REQUIRE(m && *m == Msg::PING, "handshake ping received");
    delete m;

    const std::string big(64 * 1024, 'y');
    bool sent = p.snd->send_msg(StatusTextMsg(big), MsgChannel::SendNonBlocking);
    REQUIRE(!sent, "non-deferrable non-blocking send fails on backpressure");
    REQUIRE(p.snd->at_eof(), "channel is poisoned, as monitors rely on");

    delete p.snd;
    delete p.rcv;
}

// Monitor messages must keep their MON_* wire tags (regression check for the
// MonGetCSMsg/MonJobDoneMsg constructors).
static void test_mon_tags()
{
    ChannelPair p = make_channel_pair(0);

    GetCSMsg gcs(Environments(), "some/file.cpp", CompileJob::Lang_CXX, 1,
                 "x86_64", 0, std::string(), 0, 0, 0);
    MonGetCSMsg mon(4242, 777, &gcs);
    REQUIRE(p.snd->send_msg(mon), "MonGetCSMsg sent");

    Msg *m = p.rcv->get_msg(10);
    REQUIRE(m != nullptr, "monitor message received");
    REQUIRE(m && *m == Msg::MON_GET_CS, "MonGetCSMsg arrives tagged MON_GET_CS");
    if (m && *m == Msg::MON_GET_CS) {
        MonGetCSMsg *mg = dynamic_cast<MonGetCSMsg *>(m);
        REQUIRE(mg && mg->job_id == 4242 && mg->clientid == 777,
                "MonGetCSMsg fields survive the round trip");
    }
    delete m;

    MonJobDoneMsg mjd(JobDoneMsg(555, 0, JobDoneMsg::FROM_SUBMITTER));
    REQUIRE(p.snd->send_msg(mjd), "MonJobDoneMsg sent");
    m = p.rcv->get_msg(10);
    REQUIRE(m && *m == Msg::MON_JOB_DONE, "MonJobDoneMsg arrives tagged MON_JOB_DONE");
    if (m && *m == Msg::MON_JOB_DONE) {
        MonJobDoneMsg *mj = dynamic_cast<MonJobDoneMsg *>(m);
        REQUIRE(mj && mj->job_id == 555, "MonJobDoneMsg job id survives the round trip");
    }
    delete m;

    delete p.snd;
    delete p.rcv;
}

int main(int argc, char **argv)
{
    std::string which = argc > 1 ? argv[1] : "all";

    fprintf(stderr, "=== names: total message-type stringification ===\n");
    test_message_type_names();

    if (which == "contract" || which == "all") {
        fprintf(stderr, "=== contract: dispatch send under backpressure ===\n");
        test_contract();
    }
#if defined(ICECC_MSGCHANNEL_HAS_DEFERRED_SEND)
    if (which == "multiqueue" || which == "all") {
        fprintf(stderr, "=== multiqueue: many messages behind a jammed one ===\n");
        // 8KiB: the production-sized case.  2KiB: forces the partial write
        // into chop_output()'s no-compact window (msgofs <= 8192 with > 16
        // bytes pending), the exact case where flush_writebuf()'s
        // unconditional compaction is load-bearing -- with it reverted this
        // variant corrupts the stream (verified during review).
        test_multiqueue(8 * 1024);
        test_multiqueue(2 * 1024);
    }
    if (which == "flusherrors" || which == "all") {
        fprintf(stderr, "=== flusherrors: flush_pending dead-peer semantics ===\n");
        test_flush_pending_errors();
    }
#endif
    if (which == "nondeferrable" || which == "all") {
        fprintf(stderr, "=== nondeferrable: fail-fast semantics preserved ===\n");
        test_nondeferrable_fail_fast();
    }
    if (which == "montags" || which == "all") {
        fprintf(stderr, "=== montags: monitor message wire tags ===\n");
        test_mon_tags();
    }

    if (failures) {
        fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
