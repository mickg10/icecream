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
      - ICECC_TEST_FIXBRANCH (passed by the test driver when building against
        fix/submitter-requeue-on-transient-timeout): dispatch sends use the
        blocking path; on transient failure the branch's prescribed recovery
        is clear_writebuf() + re-send of a fresh dispatch message.
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
#include <cstdio>
#include <cstring>
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
            if (r.got_use_cs) {
                break;    // everything we were waiting for arrived
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
        return;
    }

#if defined(ICECC_TEST_FIXBRANCH)
    // The fix branch's prescribed recovery: discard the stale unsent bytes,
    // then re-dispatch a fresh message once the scheduler retries the job.
    p.snd->clear_writebuf();
#endif

    // Peer starts draining now.
    std::atomic<bool> sender_done{false};
    std::thread sender([&] {
#if defined(ICECC_MSGCHANNEL_HAS_DEFERRED_SEND)
        // finish the queued big message, then dispatch USE_CS the same way
        while (p.snd->has_pending_write() && p.snd->flush_pending()) {
            usleep(2000);
        }
        bool ok = p.snd->send_msg(UseCSMsg("x86_64", kUseCsHost, kUseCsPort, kJobId, true, 1, 0),
                                  MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable);
        while (ok && p.snd->has_pending_write() && p.snd->flush_pending()) {
            usleep(2000);
        }
#else
        // blocking re-dispatch; completes as the peer drains
        p.snd->send_msg(UseCSMsg("x86_64", kUseCsHost, kUseCsPort, kJobId, true, 1, 0));
#endif
        sender_done = true;
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

// Monitor messages must keep their MON_* wire tags (regression check for the
// MonGetCSMsg/MonJobDoneMsg constructors).
static void test_mon_tags()
{
    ChannelPair p = make_channel_pair(0);

    GetCSMsg gcs(Environments(), "some/file.cpp", CompileJob::Lang_CXX, 1,
                 "x86_64", 0, std::string(), false, 0, 0);
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

    if (which == "contract" || which == "all") {
        fprintf(stderr, "=== contract: dispatch send under backpressure ===\n");
        test_contract();
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
