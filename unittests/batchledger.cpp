/*
   G4 count>1 batch-ledger gate (local-oracle 13:43 acceptance spec).  Tests-only
   child of 61e2b73; product source is unchanged.  Each named case runs a FRESH
   daemon/session/client so one failure cannot contaminate another:

     batchledger <iceccd> <case>

   Cases (each must exit nonzero for its stated RED observable at 61e2b73, not a
   setup failure; and pass under the P_batch correction):

     self-control          the exact frame collector + absence helper self-test
     client-done-filter    Gate 1 -- unmatched/duplicate client JobDone not forwarded
     late-usecs            Gate 2 -- completed IDs remain duplicate memory
     local-capacity        Gate 3 -- batch local decisions use one serialized lane
     batch-nocs            Gate 4 -- batch NoCS shares the local FIFO
     teardown-clean        Gate 5 -- clean End emits no redundant terminals
     ... (teardown matrix rows added incrementally)
*/

#include "comm.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static int failures = 0;

#define REQUIRE(cond, what)                                             \
    do {                                                                \
        if (cond) { fprintf(stderr, "ok       - %s\n", what); }         \
        else { fprintf(stderr, "FAILED   - %s\n", what); ++failures; }  \
    } while (0)

#define REQUIRE_OR_ABORT(cond, what)                                    \
    do {                                                                \
        if (cond) { fprintf(stderr, "ok       - %s\n", what); }         \
        else {                                                          \
            fprintf(stderr, "FAILED   - %s\n", what); ++failures;       \
            fprintf(stderr, "ABORT    - precondition failed\n");        \
            goto done;                                                  \
        }                                                               \
    } while (0)

/* ---------------------------------------------------------------- harness --- */
static int listen_on_port(int requested_port, int *actual_port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(requested_port));
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0
            || listen(fd, 8) != 0) { close(fd); return -1; }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&addr), &len) != 0) {
        close(fd); return -1;
    }
    *actual_port = ntohs(addr.sin_port);
    return fd;
}

static MsgChannel *accept_channel(int listener, int timeout_msec)
{
    struct pollfd pfd = { listener, POLLIN, 0 };
    if (poll(&pfd, 1, timeout_msec) <= 0) return nullptr;
    const int fd = accept(listener, nullptr, nullptr);
    if (fd < 0) return nullptr;
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof(peer));
    peer.sin_family = AF_INET;
    return Service::createChannel(fd, reinterpret_cast<struct sockaddr *>(&peer), sizeof(peer));
}

static MsgChannel *connect_unix_bounded(const std::string &path, int timeout_msec)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        if (access(path.c_str(), F_OK) == 0) {
            MsgChannel *channel = Service::createChannel(path);
            if (channel) return channel;
        }
        usleep(20 * 1000);
    }
    return nullptr;
}

static Msg *wait_for_type(MsgChannel *channel, Msg::Value wanted, int timeout_msec)
{
    if (!channel) return nullptr;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        if (msg) { if (*msg == wanted) return msg; delete msg; }
        if (channel->at_eof()) return nullptr;
    }
    return nullptr;
}

static MsgChannel *accept_login_channel(int listener, int timeout_msec, Msg **login_out)
{
    *login_out = nullptr;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
        MsgChannel *channel = accept_channel(listener, remaining);
        if (!channel) continue;
        Msg *login = wait_for_type(channel, Msg::LOGIN, remaining < 5000 ? remaining : 5000);
        if (login) { *login_out = login; return channel; }
        delete channel;
    }
    return nullptr;
}

static bool wait_child(pid_t pid, int timeout_msec, int *status)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        const pid_t got = waitpid(pid, status, WNOHANG);
        if (got == pid) return true;
        if (got < 0) return false;
        usleep(20 * 1000);
    }
    return false;
}

/* ------------------------------------------------ exact frame collector --- */
struct DoneFrame {
    uint32_t job_id;
    uint32_t exitcode;
    bool from_server;   /* origin: JobDone from a compile server vs a submitter */
};

/* Collect ALL JobDone frames the scheduler receives over `window_msec`, in exact
   arrival order, recording id/exit/origin.  Ordered multiset: three JobDone of
   one id are three entries, never satisfying a three-distinct-id expectation. */
static std::vector<DoneFrame> collect_job_done(MsgChannel *sched, int window_msec)
{
    std::vector<DoneFrame> frames;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(window_msec);
    while (sched && Clock::now() < deadline) {
        Msg *msg = sched->get_msg(1, true);
        if (msg) {
            if (*msg == Msg::JOB_DONE) {
                JobDoneMsg *d = dynamic_cast<JobDoneMsg *>(msg);
                if (d) {
                    DoneFrame f;
                    f.job_id = d->job_id;
                    f.exitcode = d->exitcode;
                    f.from_server = d->is_from_server();
                    frames.push_back(f);
                }
            }
            delete msg;
        }
        if (sched->at_eof()) break;
    }
    return frames;
}

static int count_id(const std::vector<DoneFrame> &f, uint32_t id)
{
    int n = 0;
    for (const DoneFrame &d : f) if (d.job_id == id) ++n;
    return n;
}

/* Capture up to `want` USE_CS frames delivered to a client, exact fields. */
struct SeenUseCS { uint32_t job_id; bool got_env; std::string host; };
static std::vector<SeenUseCS> capture_use_cs(MsgChannel *client, int want, int timeout_msec)
{
    std::vector<SeenUseCS> seen;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (client && static_cast<int>(seen.size()) < want && Clock::now() < deadline) {
        Msg *msg = client->get_msg(1, true);
        if (msg) {
            if (*msg == Msg::USE_CS) {
                UseCSMsg *u = dynamic_cast<UseCSMsg *>(msg);
                if (u) { SeenUseCS s; s.job_id = u->job_id; s.got_env = u->got_env != 0; s.host = u->hostname; seen.push_back(s); }
            }
            delete msg;
        }
        if (client->at_eof()) break;
    }
    return seen;
}

/* has_msg()-before-poll absence helper: no complete frame may appear.  A frame
   already buffered in userspace is reported without waiting for a new edge;
   POLLERR/POLLNVAL and read errors fail closed. */
static bool expect_no_frame(MsgChannel *ch, int timeout_msec, std::string *seen)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (ch && Clock::now() < deadline) {
        if (ch->has_msg()) {
            Msg *m = ch->get_msg(0, true);
            if (m) { if (seen) *seen = m->to_string(); delete m; return false; }
            if (ch->at_eof()) { if (seen) *seen = "<eof>"; return false; }
        }
        if (ch->at_eof()) { if (seen) *seen = "<eof>"; return false; }
        const int remaining = std::max<int>(1, static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
        struct pollfd pfd = { ch->fd, POLLIN | POLLHUP | POLLERR, 0 };
        const int rc = poll(&pfd, 1, remaining);
        if (rc == 0) return true;
        if (rc < 0) { if (errno == EINTR) continue; return false; }
        if (pfd.revents & (POLLERR | POLLNVAL)) { if (seen) *seen = "<poll error>"; return false; }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            if (!ch->read_a_bit()) { if (seen) *seen = "<read error>"; return false; }
            if (ch->has_msg()) {
                Msg *m = ch->get_msg(0, true);
                if (m) { if (seen) *seen = m->to_string(); delete m; return false; }
                if (ch->at_eof()) { if (seen) *seen = "<eof>"; return false; }
            }
            if (ch->at_eof()) { if (seen) *seen = "<eof>"; return false; }
        }
    }
    return true;
}

/* ----------------------------------------------------------- fresh farm --- */
struct Farm {
    pid_t pid = -1;
    int listener = -1;
    MsgChannel *sched = nullptr;
    std::string work;
    std::string socket_path;
};

/* Start a fresh iceccd with `slots` local slots against a fake scheduler and
   activate the session with ConfCS.  Returns false on any setup failure. */
static bool setup_farm(const char *iceccd, int slots, Farm &f)
{
    char tmpl[] = "/tmp/icecream-g4-batch.XXXXXX";
    char *t = mkdtemp(tmpl);
    if (!t) { perror("mkdtemp"); return false; }
    f.work = t;
    const std::string envdir = f.work + "/envs";
    f.socket_path = f.work + "/iceccd.sock";
    const std::string log = f.work + "/iceccd.log";
    mkdir(envdir.c_str(), 0700);
    fprintf(stderr, "retained work directory: %s\n", f.work.c_str());

    int port = 0;
    { int probe = listen_on_port(0, &port); if (probe >= 0) close(probe); }
    if (port <= 0) return false;
    int bound = 0;
    f.listener = listen_on_port(port, &bound);
    if (f.listener < 0 || bound != port) return false;

    f.pid = fork();
    if (f.pid == 0) {
        char spec[64];
        snprintf(spec, sizeof(spec), "127.0.0.1:%d", port);
        char sl[16]; snprintf(sl, sizeof(sl), "%d", slots);
        setenv("ICECC_TESTS", "1", 1);
        setenv("ICECC_TEST_SOCKET", f.socket_path.c_str(), 1);
        execl(iceccd, iceccd, "--no-remote", "-m", sl, "-p", "10245",
              "-s", spec, "-n", "g4-batch-gate", "-N", "g4-daemon",
              "-b", envdir.c_str(), "-l", log.c_str(),
              "-v", "-v", "-v", static_cast<char *>(nullptr));
        perror("execl iceccd");
        _exit(127);
    }
    if (f.pid < 0) return false;

    Msg *login = nullptr;
    f.sched = accept_login_channel(f.listener, 20000, &login);
    if (!f.sched || !login) { delete login; return false; }
    delete login;
    if (!f.sched->send_msg(ConfCSMsg())) return false;
    usleep(150 * 1000);
    return true;
}

static void teardown_farm(Farm &f, bool *clean_exit)
{
    int status = 0;
    bool reaped = false;
    if (f.pid > 0) {
        kill(f.pid, SIGTERM);
        reaped = wait_child(f.pid, 10000, &status);
        if (!reaped) { kill(f.pid, SIGKILL); waitpid(f.pid, &status, 0); }
    }
    if (clean_exit) *clean_exit = reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    delete f.sched; f.sched = nullptr;
    if (f.listener >= 0) { close(f.listener); f.listener = -1; }
}

static GetCSMsg make_getcs(const char *file, unsigned int count)
{
    return GetCSMsg(Environments(), file, CompileJob::Lang_CXX, count,
                    "x86_64", 0, std::string(), 0, 0, 0);
}

/* Publish GetCS(count=n) from `client`, read the forwarded GetCS at S, return
   the daemon's client_id (0 on failure). */
static uint32_t publish_batch(MsgChannel *client, MsgChannel *sched,
                              const char *file, unsigned int n, unsigned int *fwd_count)
{
    if (!client->send_msg(make_getcs(file, n))) return 0;
    Msg *fwd = wait_for_type(sched, Msg::GET_CS, 5000);
    if (!fwd) return 0;
    GetCSMsg *g = dynamic_cast<GetCSMsg *>(fwd);
    uint32_t cid = g ? g->client_id : 0;
    if (fwd_count) *fwd_count = g ? g->count : 0;
    delete fwd;
    return cid;
}

/* ================================================================ CASES === */

/* Connected MsgChannel pair; createChannel() handshakes synchronously so both
   ends must come up concurrently (backpressure.cpp pattern). */
struct ChannelPair { MsgChannel *a = nullptr; MsgChannel *b = nullptr; };
static ChannelPair make_channel_pair()
{
    int fds[2];
    ChannelPair p;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return p;
    struct sockaddr_un sa; memset(&sa, 0, sizeof(sa)); sa.sun_family = AF_UNIX;
    std::thread ta([&]{ p.a = Service::createChannel(fds[0], (struct sockaddr *)&sa, sizeof(sa)); });
    std::thread tb([&]{ p.b = Service::createChannel(fds[1], (struct sockaddr *)&sa, sizeof(sa)); });
    ta.join(); tb.join();
    return p;
}

/* self-control: a forbidden complete frame delivered in the SAME burst as a
   consumed one must be detected by the absence helper without a new kernel edge;
   a poll-first / order-blind helper would miss it and falsely pass. */
static int case_self_control(const char *iceccd)
{
    (void)iceccd;
    ChannelPair p = make_channel_pair();
    REQUIRE_OR_ABORT(p.a && p.b, "channel pair established");
    REQUIRE_OR_ABORT(p.a->send_msg(LoginMsg()) && p.a->send_msg(EndMsg()),
                     "peer wrote Login + one forbidden End in a single burst");
    p.a->flush_pending();
    usleep(50 * 1000);
    {
        Msg *login = wait_for_type(p.b, Msg::LOGIN, 2000);
        REQUIRE_OR_ABORT(login != nullptr, "reader consumed the expected Login");
        delete login;
    }
    {
        std::string seen;
        REQUIRE(!expect_no_frame(p.b, 1500, &seen),
                "absence helper detected the buffered forbidden frame without a new edge");
        fprintf(stderr, "         (detected: %s)\n", seen.c_str());
    }
done:
    delete p.a; delete p.b;
    return failures;
}

/* Gate 1 -- client-done-filter: unmatched (U) and duplicate (second J1) client
   JobDone must NOT be forwarded to S; exactly [J1,J2,J3] once each must be. */
static int case_client_done_filter(const char *iceccd)
{
    Farm f;
    MsgChannel *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated");
    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "batch client connected");

    {  /* nested scope so an abort goto never crosses a local initialization */
    unsigned int fcount = 0;
    const uint32_t cid = publish_batch(client, f.sched, "g1.cpp", 3, &fcount);
    REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
    REQUIRE(fcount == 3, "forwarded GetCS preserved count=3");

    const uint32_t J1 = 6100, J2 = 6101, J3 = 6102, U = 6199;
    bool sent = true;
    for (uint32_t j : { J1, J2, J3 })
        sent = sent && f.sched->send_msg(UseCSMsg("x86_64", "10.0.0.9", 3632u, j, true, cid, 0));
    REQUIRE_OR_ABORT(sent, "scheduler delivered three remote decisions");
    std::vector<SeenUseCS> got = capture_use_cs(client, 3, 6000);
    REQUIRE_OR_ABORT(got.size() == 3, "client received all three remote UseCS");

    /* unmatched U, then a duplicate J1 in the same burst as valid J2/J3 */
    JobDoneMsg dU(U, 0, JobDoneMsg::FROM_SUBMITTER); dU.real_msec = 1; dU.user_msec = 1;
    JobDoneMsg d1(J1, 0, JobDoneMsg::FROM_SUBMITTER); d1.real_msec = 1; d1.user_msec = 1;
    JobDoneMsg d2(J2, 0, JobDoneMsg::FROM_SUBMITTER); d2.real_msec = 1; d2.user_msec = 1;
    JobDoneMsg d3(J3, 0, JobDoneMsg::FROM_SUBMITTER); d3.real_msec = 1; d3.user_msec = 1;
    client->send_msg(dU);
    client->send_msg(d1);
    client->send_msg(d1);   /* duplicate of J1 */
    client->send_msg(d2);
    client->send_msg(d3);

    std::vector<DoneFrame> f_done = collect_job_done(f.sched, 5000);
    fprintf(stderr, "         (scheduler saw %zu JobDone: U=%d J1=%d J2=%d J3=%d)\n",
            f_done.size(), count_id(f_done, U), count_id(f_done, J1),
            count_id(f_done, J2), count_id(f_done, J3));
    REQUIRE(count_id(f_done, U) == 0, "unmatched JobDone(U) is not forwarded to S");
    REQUIRE(count_id(f_done, J1) == 1, "duplicate JobDone(J1) forwarded exactly once");
    REQUIRE(count_id(f_done, J2) == 1, "JobDone(J2) forwarded exactly once");
    REQUIRE(count_id(f_done, J3) == 1, "JobDone(J3) forwarded exactly once");

    /* trailing liveness: a fresh scalar client still gets a local decision */
    {
        MsgChannel *live = connect_unix_bounded(f.socket_path, 3000);
        REQUIRE(live != nullptr, "daemon still accepts a new client after the filtered dones");
        delete live;
    }
    }  /* nested scope */

done:
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Gate 2 -- late-usecs-after-completion: completing J1 must NOT erase it from
   duplicate memory.  A late UseCS(J1) after completion reaches neither the client
   nor the local lane and emits no terminal; scheduler sees one completion per id. */
static int case_late_usecs(const char *iceccd)
{
    Farm f;
    MsgChannel *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated");
    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "batch client connected");
    {
        unsigned int fcount = 0;
        const uint32_t cid = publish_batch(client, f.sched, "g2.cpp", 3, &fcount);
        REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
        const uint32_t J1 = 6200, J2 = 6201, J3 = 6202;
        bool sent = true;
        for (uint32_t j : { J1, J2, J3 })
            sent = sent && f.sched->send_msg(UseCSMsg("x86_64", "10.0.0.9", 3632u, j, true, cid, 0));
        REQUIRE_OR_ABORT(sent, "scheduler delivered three remote decisions");
        std::vector<SeenUseCS> got = capture_use_cs(client, 3, 6000);
        REQUIRE_OR_ABORT(got.size() == 3, "client received all three remote UseCS");

        /* complete J1 -> exactly one forwarded JobDone(J1) */
        { JobDoneMsg d(J1, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::vector<DoneFrame> after_j1 = collect_job_done(f.sched, 2500);
        REQUIRE(count_id(after_j1, J1) == 1, "completing J1 forwards exactly one JobDone(J1)");

        /* late duplicate decision for the completed J1, visibly different fields */
        f.sched->send_msg(UseCSMsg("x86_64", "10.9.9.9", 3632u, J1, false, cid, 0));
        std::vector<SeenUseCS> late = capture_use_cs(client, 1, 2000);
        REQUIRE(late.empty(), "late UseCS(J1) after completion is not delivered to the client");

        /* complete J2,J3 then drain: no resurrected excess terminal for J1, and
           exactly one completion each for J2,J3 */
        for (uint32_t j : { J2, J3 }) { JobDoneMsg d(j, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::vector<DoneFrame> tail = collect_job_done(f.sched, 4000);
        fprintf(stderr, "         (after-completion tail: J1=%d J2=%d J3=%d)\n",
                count_id(tail, J1), count_id(tail, J2), count_id(tail, J3));
        REQUIRE(count_id(tail, J1) == 0, "no second/excess terminal for the completed J1");
        REQUIRE(count_id(tail, J2) == 1, "JobDone(J2) forwarded exactly once");
        REQUIRE(count_id(tail, J3) == 1, "JobDone(J3) forwarded exactly once");
    }
done:
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Gate 5 (clean row) -- batch-teardown: a clean EndMsg after remote-only delivery
   must emit ZERO terminals and zero client-id cancellation.  This is the fixture
   form of the retained farm's 21 `late terminal for absent job` over-settlement. */
static int case_teardown_clean(const char *iceccd)
{
    Farm f;
    MsgChannel *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated");
    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "batch client connected");
    {
        unsigned int fcount = 0;
        const uint32_t cid = publish_batch(client, f.sched, "g5.cpp", 3, &fcount);
        REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
        const uint32_t J1 = 6500, J2 = 6501, J3 = 6502;
        bool sent = true;
        for (uint32_t j : { J1, J2, J3 })
            sent = sent && f.sched->send_msg(UseCSMsg("x86_64", "10.0.0.9", 3632u, j, true, cid, 0));
        REQUIRE_OR_ABORT(sent, "scheduler delivered three remote decisions");
        std::vector<SeenUseCS> got = capture_use_cs(client, 3, 6000);
        REQUIRE_OR_ABORT(got.size() == 3, "client received all three remote UseCS");

        /* clean End: a real EndMsg, no synthetic JobDone for the remote entries */
        REQUIRE(client->send_msg(EndMsg()), "client sent a clean EndMsg");
        std::vector<DoneFrame> after = collect_job_done(f.sched, 4000);
        fprintf(stderr, "         (terminals after clean End: J1=%d J2=%d J3=%d total=%zu)\n",
                count_id(after, J1), count_id(after, J2), count_id(after, J3), after.size());
        REQUIRE(after.empty(), "clean End emits zero terminals for delivered remote entries");

        MsgChannel *live = connect_unix_bounded(f.socket_path, 3000);
        REQUIRE(live != nullptr, "daemon responsive after clean End");
        delete live;
    }
done:
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Gate 3 -- local-capacity: batch LOCAL decisions (host=127.0.0.1:daemon_port)
   must NOT be relayed while the one compile slot is occupied; they queue one at a
   time behind a blocker and advance on the matching client JobDone.  RED@61e2b73:
   the batch branch relays same-daemon decisions immediately, bypassing capacity. */
static int case_local_capacity(const char *iceccd)
{
    Farm f;
    MsgChannel *blocker = nullptr, *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated (-m1)");

    /* occupy the single compile slot with a controllable local blocker */
    blocker = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(blocker != nullptr, "blocker client connected");
    {
        JobLocalBeginMsg jlb(0, "blk.o", false, "gate3-blocker",
                             "g++ -c blk.cpp -o blk.o", JobLocalBeginMsg::LocalFlagNone);
        REQUIRE_OR_ABORT(blocker->send_msg(jlb), "blocker sent JobLocalBegin");
        Msg *ack = wait_for_type(blocker, Msg::JOB_LOCAL_BEGIN, 5000);
        REQUIRE_OR_ABORT(ack != nullptr, "daemon granted the blocker the compile slot");
        delete ack;
    }
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500); delete m; }  /* drain S-side announce */

    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "batch client connected");
    {
        unsigned int fcount = 0;
        const uint32_t cid = publish_batch(client, f.sched, "g3.cpp", 3, &fcount);
        REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
        const uint32_t L1 = 6300, L2 = 6301, L3 = 6302;
        bool sent = true;
        for (uint32_t j : { L1, L2, L3 })   /* LOCAL decisions: host==remote_name, port==daemon_port */
            sent = sent && f.sched->send_msg(UseCSMsg("x86_64", "127.0.0.1", 10245, j, true, cid, 0));
        REQUIRE_OR_ABORT(sent, "scheduler delivered three LOCAL batch decisions");

        std::vector<SeenUseCS> during = capture_use_cs(client, 3, 2500);
        fprintf(stderr, "         (local UseCS delivered while blocker holds: %zu)\n", during.size());
        REQUIRE(during.empty(), "no local batch UseCS delivered while the compile slot is occupied");

        /* release the blocker -> exactly L1, then one-at-a-time on each JobDone */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        std::vector<SeenUseCS> a = capture_use_cs(client, 1, 4000);
        REQUIRE(a.size() == 1 && a[0].job_id == L1, "after release exactly L1 is delivered");
        std::vector<SeenUseCS> none2 = capture_use_cs(client, 1, 1200);
        REQUIRE(none2.empty(), "L2 withheld until L1 completes");
        { JobDoneMsg d(L1, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::vector<SeenUseCS> b = capture_use_cs(client, 1, 4000);
        REQUIRE(b.size() == 1 && b[0].job_id == L2, "L2 delivered after JobDone(L1)");
        { JobDoneMsg d(L2, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::vector<SeenUseCS> cc = capture_use_cs(client, 1, 4000);
        REQUIRE(cc.size() == 1 && cc[0].job_id == L3, "L3 delivered after JobDone(L2)");
        { JobDoneMsg d(L3, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
    }
done:
    delete blocker;
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Gate 4 -- batch-nocs: NoCS decisions in a batch must be accepted by ledger
   state (not require WAITFORCS) and share the one capacity-serialized local FIFO.
   RED@61e2b73: the first NoCS takes the scalar path (leaves WAITFORCS); later
   NoCS are "unmatched" and terminalized with JobDone(107). */
static int case_batch_nocs(const char *iceccd)
{
    Farm f;
    MsgChannel *blocker = nullptr, *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated (-m1)");

    blocker = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(blocker != nullptr, "blocker client connected");
    {
        JobLocalBeginMsg jlb(0, "blk.o", false, "gate4-blocker",
                             "g++ -c blk.cpp -o blk.o", JobLocalBeginMsg::LocalFlagNone);
        REQUIRE_OR_ABORT(blocker->send_msg(jlb), "blocker sent JobLocalBegin");
        Msg *ack = wait_for_type(blocker, Msg::JOB_LOCAL_BEGIN, 5000);
        REQUIRE_OR_ABORT(ack != nullptr, "daemon granted the blocker the compile slot");
        delete ack;
    }
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500); delete m; }

    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "batch client connected");
    {
        unsigned int fcount = 0;
        const uint32_t cid = publish_batch(client, f.sched, "g4.cpp", 3, &fcount);
        REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
        const uint32_t N1 = 6400, N2 = 6401, N3 = 6402;
        bool sent = true;
        for (uint32_t j : { N1, N2, N3 })
            sent = sent && f.sched->send_msg(NoCSMsg(j, cid));
        REQUIRE_OR_ABORT(sent, "scheduler delivered three NoCS decisions in one burst");

        /* RED discriminator: later NoCS must NOT be rejected as unmatched (no 107) */
        std::vector<DoneFrame> term = collect_job_done(f.sched, 3000);
        fprintf(stderr, "         (unmatched terminals: N2=%d N3=%d)\n",
                count_id(term, N2), count_id(term, N3));
        REQUIRE(count_id(term, N2) == 0 && count_id(term, N3) == 0,
                "later NoCS accepted by ledger state, not rejected with JobDone(107)");

        /* release -> one local decision at a time as capacity frees */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        std::vector<SeenUseCS> a = capture_use_cs(client, 1, 4000);
        REQUIRE(a.size() == 1 && a[0].job_id == N1, "after release exactly N1 local decision delivered");
        { JobDoneMsg d(N1, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::vector<SeenUseCS> b = capture_use_cs(client, 1, 4000);
        REQUIRE(b.size() == 1 && b[0].job_id == N2, "N2 delivered after JobDone(N1)");
        { JobDoneMsg d(N2, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::vector<SeenUseCS> cc = capture_use_cs(client, 1, 4000);
        REQUIRE(cc.size() == 1 && cc[0].job_id == N3, "N3 delivered after JobDone(N2)");
        { JobDoneMsg d(N3, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
    }
done:
    delete blocker;
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <iceccd> <case>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    const std::string c = argv[2];
    int rc = 0;
    if (c == "self-control")            rc = case_self_control(argv[1]);
    else if (c == "client-done-filter") rc = case_client_done_filter(argv[1]);
    else if (c == "late-usecs")         rc = case_late_usecs(argv[1]);
    else if (c == "teardown-clean")     rc = case_teardown_clean(argv[1]);
    else if (c == "local-capacity")     rc = case_local_capacity(argv[1]);
    else if (c == "batch-nocs")         rc = case_batch_nocs(argv[1]);
    else { fprintf(stderr, "unknown case: %s\n", c.c_str()); return 2; }

    fprintf(stderr, "%s [%s] (%d failure%s)\n",
            rc ? "RESULT: FAIL" : "RESULT: PASS", c.c_str(), rc, rc == 1 ? "" : "s");
    return rc ? 1 : 0;
}
