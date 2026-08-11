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
/* Every field that distinguishes a correct terminal from a resurrected/reordered/
   field-altered one (local-oracle item 1): exact id, SIGNED exit code, origin,
   flags, unknown-client id, and the completion sentinels. */
struct DoneFrame {
    uint32_t job_id;
    int      exitcode;      /* signed */
    bool     from_server;
    uint32_t flags;
    uint32_t unknown_client;
    uint32_t real_msec;
    uint32_t user_msec;
};

/* Collect ALL JobDone frames over `window_msec` in EXACT ARRIVAL ORDER.  An
   ordered multiset -- three JobDone of one id are three ordered entries and never
   satisfy a three-distinct-id expectation.  Returns whether the channel errored
   (EOF/read) so absence/quiet-window checks can fail closed. */
static std::vector<DoneFrame> collect_job_done(MsgChannel *sched, int window_msec, bool *errored = nullptr)
{
    std::vector<DoneFrame> frames;
    if (errored) *errored = false;
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
                    f.flags = d->flags;
                    f.unknown_client = d->unknown_job_client_id();
                    f.real_msec = d->real_msec;
                    f.user_msec = d->user_msec;
                    frames.push_back(f);
                }
            }
            delete msg;
        }
        if (sched->at_eof()) { if (errored) *errored = true; break; }
    }
    return frames;
}

static int count_id(const std::vector<DoneFrame> &f, uint32_t id)
{
    int n = 0;
    for (const DoneFrame &d : f) if (d.job_id == id) ++n;
    return n;
}

/* A client-id cancellation is a JobDone whose unknown_job_client_id() is non-zero
   (set_unknown_job_client_id stores the client id in job_id, comm.cpp). */
static int count_cancellations(const std::vector<DoneFrame> &f, uint32_t client_id)
{
    int n = 0;
    for (const DoneFrame &d : f) if (d.unknown_client != 0 && d.unknown_client == client_id) ++n;
    return n;
}

/* The exact ordered sequence of SETTLEMENT job-ids (real terminals, excluding
   client-id cancellations), for order asserts. */
static std::vector<uint32_t> done_order(const std::vector<DoneFrame> &f)
{
    std::vector<uint32_t> ids;
    for (const DoneFrame &d : f) if (d.unknown_client == 0 && d.job_id != 0) ids.push_back(d.job_id);
    return ids;
}

static std::string ids_to_string(const std::vector<uint32_t> &v)
{
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += std::to_string(v[i]); }
    return s + "]";
}

/* Capture up to `want` USE_CS frames, EVERY serialized field (local-oracle item 1). */
struct SeenUseCS {
    uint32_t job_id;
    std::string host;
    uint32_t port;
    std::string platform;
    bool got_env;
    uint32_t client_id;
    uint32_t matched_job_id;
};
static std::vector<SeenUseCS> capture_use_cs(MsgChannel *client, int want, int timeout_msec, bool *errored = nullptr)
{
    std::vector<SeenUseCS> seen;
    if (errored) *errored = false;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (client && static_cast<int>(seen.size()) < want && Clock::now() < deadline) {
        Msg *msg = client->get_msg(1, true);
        if (msg) {
            if (*msg == Msg::USE_CS) {
                UseCSMsg *u = dynamic_cast<UseCSMsg *>(msg);
                if (u) {
                    SeenUseCS s;
                    s.job_id = u->job_id;
                    s.host = u->hostname;
                    s.port = u->port;
                    s.platform = u->host_platform;
                    s.got_env = u->got_env != 0;
                    s.client_id = u->client_id;
                    s.matched_job_id = u->matched_job_id;
                    seen.push_back(s);
                }
            }
            delete msg;
        }
        if (client->at_eof()) { if (errored) *errored = true; break; }
    }
    return seen;
}

/* A UseCS-absence claim that FAILS CLOSED (local-oracle item 2): true only for
   bounded silence -- no USE_CS delivered AND no EOF/read error.  A closed or
   errored channel is never treated as "no decision". */
static bool no_use_cs(MsgChannel *client, int window_msec)
{
    bool err = false;
    std::vector<SeenUseCS> v = capture_use_cs(client, 1, window_msec, &err);
    return v.empty() && !err;
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
        REQUIRE(no_use_cs(client, 2000), "late UseCS(J1) after completion is not delivered to the client");

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
        /* LOCAL decisions: host==remote_name (127.0.0.1) && port==daemon_port.
           A --no-remote submitter has daemon_port==0, so a same-daemon UseCS uses
           port 0 (matches the scalar local-detect at scheduler_use_cs). */
        for (uint32_t j : { L1, L2, L3 })
            sent = sent && f.sched->send_msg(UseCSMsg("x86_64", "127.0.0.1", 0, j, true, cid, 0));
        REQUIRE_OR_ABORT(sent, "scheduler delivered three LOCAL batch decisions");

        REQUIRE(no_use_cs(client, 2500), "no local batch UseCS delivered while the compile slot is occupied");

        /* release the blocker -> exactly L1, then one-at-a-time on each JobDone */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        std::vector<SeenUseCS> a = capture_use_cs(client, 1, 4000);
        REQUIRE(a.size() == 1 && a[0].job_id == L1, "after release exactly L1 is delivered");
        REQUIRE(no_use_cs(client, 1200), "L2 withheld until L1 completes");
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

/* Teardown matrix -- remote rows.  A batch of 3 remote decisions; `deliver` of
   them are delivered, then the client ends by raw EOF (use_endmsg=false) or a
   clean EndMsg (use_endmsg=true).  NORMAL end (clean EndMsg with all expected
   delivered and no unfinished local) settles nothing; every other end settles
   the delivered-but-unfinished entries by exact id in order plus one client-id
   cancellation for the undelivered tail. */
static int run_teardown_remote(const char *iceccd, int deliver, bool use_endmsg)
{
    Farm f;
    MsgChannel *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated");
    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "batch client connected");
    {
        unsigned int fcount = 0;
        const uint32_t cid = publish_batch(client, f.sched, "td.cpp", 3, &fcount);
        REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
        const uint32_t J[3] = { 7000, 7001, 7002 };
        for (int i = 0; i < deliver; ++i)
            f.sched->send_msg(UseCSMsg("x86_64", "10.0.0.9", 3632u, J[i], true, cid, 0));
        if (deliver > 0) {
            std::vector<SeenUseCS> got = capture_use_cs(client, deliver, 5000);
            REQUIRE_OR_ABORT((int)got.size() == deliver, "the delivered remote decisions reached the client");
        }
        /* end the client */
        if (use_endmsg) {
            REQUIRE(client->send_msg(EndMsg()), "client sent a clean EndMsg");
        } else {
            delete client; client = nullptr;   /* raw EOF */
        }
        std::vector<DoneFrame> term = collect_job_done(f.sched, 4000);

        const bool normal = use_endmsg && deliver == 3;   /* all expected delivered, no local */
        std::vector<uint32_t> expect;
        if (!normal) for (int i = 0; i < deliver; ++i) expect.push_back(J[i]);
        const int expect_cancel = (!normal && deliver < 3) ? 1 : 0;

        std::vector<uint32_t> seq = done_order(term);
        fprintf(stderr, "         (end=%s deliver=%d -> terminals %s cancels=%d)\n",
                use_endmsg ? "EndMsg" : "EOF", deliver, ids_to_string(seq).c_str(),
                count_cancellations(term, cid));
        REQUIRE(seq == expect, "exact ordered settlement matches the plan");
        REQUIRE(count_cancellations(term, cid) == expect_cancel, "exact client-id cancellation count");
        /* no terminal for a not-yet-delivered id ever */
        for (int i = deliver; i < 3; ++i)
            REQUIRE(count_id(term, J[i]) == 0, "no terminal for an undelivered decision");

        /* trailing liveness: a fresh scalar client completes a real GetCS->S->
           UseCS->client exchange (a live scheduler forwards, so we must answer). */
        MsgChannel *live = connect_unix_bounded(f.socket_path, 3000);
        REQUIRE(live != nullptr, "daemon accepts a new client after teardown");
        if (live) {
            live->send_msg(make_getcs("post.cpp", 1));
            Msg *lfwd = wait_for_type(f.sched, Msg::GET_CS, 4000);
            REQUIRE(lfwd != nullptr, "post-teardown scalar GetCS reached the scheduler");
            uint32_t lc = 0;
            { GetCSMsg *g = dynamic_cast<GetCSMsg *>(lfwd); if (g) lc = g->client_id; }
            delete lfwd;
            f.sched->send_msg(UseCSMsg("x86_64", "10.0.0.9", 3632u, 7500, true, lc, 0));
            std::vector<SeenUseCS> pv = capture_use_cs(live, 1, 4000);
            REQUIRE(pv.size() == 1 && (pv.empty() || pv[0].job_id == 7500),
                    "post-teardown scalar exchange completed exactly once");
            delete live;
        }
    }
done:
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Teardown matrix -- local rows.  Three LOCAL decisions behind a capacity blocker
   (JobLocalBegin held), ended early in a queued/active state, or normally after
   all local entries complete.  A local unfinished entry is always settled by
   exact id; capacity returns to baseline; a normal End settles nothing. */
static int run_teardown_local(const char *iceccd, const std::string &scenario)
{
    Farm f;
    MsgChannel *blocker = nullptr, *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated (-m1)");
    blocker = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(blocker != nullptr, "blocker connected");
    {
        JobLocalBeginMsg jlb(0, "blk.o", false, "td-blocker", "g++ -c blk.cpp -o blk.o",
                             JobLocalBeginMsg::LocalFlagNone);
        REQUIRE_OR_ABORT(blocker->send_msg(jlb), "blocker sent JobLocalBegin");
        Msg *ack = wait_for_type(blocker, Msg::JOB_LOCAL_BEGIN, 5000);
        REQUIRE_OR_ABORT(ack != nullptr, "daemon granted the blocker the slot");
        delete ack;
    }
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500); delete m; }

    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "batch client connected");
    {
        unsigned int fcount = 0;
        const uint32_t cid = publish_batch(client, f.sched, "tdl.cpp", 3, &fcount);
        REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
        const uint32_t L1 = 7100, L2 = 7101, L3 = 7102;
        for (uint32_t j : { L1, L2, L3 })   /* LOCAL decisions: queued behind blocker */
            f.sched->send_msg(UseCSMsg("x86_64", "127.0.0.1", 0, j, true, cid, 0));
        usleep(300 * 1000);   /* let the daemon record + queue them */

        if (scenario == "queued") {
            /* end early while all three are queued (blocker still holds) */
            REQUIRE(client->send_msg(EndMsg()), "client sent EndMsg with local entries queued");
            std::vector<DoneFrame> term = collect_job_done(f.sched, 4000);
            std::vector<uint32_t> seq = done_order(term);
            fprintf(stderr, "         (queued -> terminals %s cancels=%d)\n",
                    ids_to_string(seq).c_str(), count_cancellations(term, cid));
            /* all three recorded local entries are unfinished -> settled once each */
            REQUIRE(seq == (std::vector<uint32_t>{L1, L2, L3}),
                    "queued local entries settled once each by exact id");
            REQUIRE(count_cancellations(term, cid) == 0,
                    "no cancellation when all expected decisions were recorded");
        } else if (scenario == "active") {
            /* release the blocker -> L1 becomes active; end while L1 is active */
            { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
            std::vector<SeenUseCS> a = capture_use_cs(client, 1, 4000);
            REQUIRE_OR_ABORT(a.size() == 1 && a[0].job_id == L1, "L1 delivered active after release");
            REQUIRE(client->send_msg(EndMsg()), "client sent EndMsg with L1 active");
            std::vector<DoneFrame> term = collect_job_done(f.sched, 4000);
            std::vector<uint32_t> seq = done_order(term);
            fprintf(stderr, "         (active -> terminals %s cancels=%d)\n",
                    ids_to_string(seq).c_str(), count_cancellations(term, cid));
            REQUIRE(seq == (std::vector<uint32_t>{L1, L2, L3}),
                    "active + queued local entries settled once each");
            REQUIRE(count_cancellations(term, cid) == 0, "no cancellation (all recorded)");
            /* capacity restored: a fresh blocker can take the slot again */
            MsgChannel *b2 = connect_unix_bounded(f.socket_path, 3000);
            REQUIRE(b2 != nullptr, "second blocker connected");
            if (b2) {
                JobLocalBeginMsg j2(0, "b2.o", false, "cap-check", "g++ -c b2.cpp -o b2.o", JobLocalBeginMsg::LocalFlagNone);
                b2->send_msg(j2);
                Msg *ack2 = wait_for_type(b2, Msg::JOB_LOCAL_BEGIN, 4000);
                REQUIRE(ack2 != nullptr, "capacity returned to baseline (slot re-granted)");
                delete ack2; delete b2;
            }
        } else { /* normal: deliver + complete all three, then End */
            { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
            uint32_t exp[3] = { L1, L2, L3 };
            for (int i = 0; i < 3; ++i) {
                std::vector<SeenUseCS> v = capture_use_cs(client, 1, 4000);
                REQUIRE_OR_ABORT(v.size() == 1 && v[0].job_id == exp[i], "next local decision delivered in order");
                JobDoneMsg d(exp[i], 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d);
            }
            std::vector<DoneFrame> after = collect_job_done(f.sched, 2500);   /* drain the 3 forwarded completions */
            REQUIRE(done_order(after) == (std::vector<uint32_t>{L1, L2, L3}), "the 3 local completions forwarded once each");
            REQUIRE(client->send_msg(EndMsg()), "client sent a normal EndMsg after all local done");
            std::vector<DoneFrame> term = collect_job_done(f.sched, 3000);
            fprintf(stderr, "         (normal -> extra terminals %s cancels=%d)\n",
                    ids_to_string(done_order(term)).c_str(), count_cancellations(term, cid));
            REQUIRE(done_order(term).empty(), "normal End emits no extra terminal");
            REQUIRE(count_cancellations(term, cid) == 0, "normal End emits no cancellation");
        }
    }
done:
    delete blocker;
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Item 7 -- scalar/reference coverage: the count==0 and count==1 SCALAR_ONE path
   is unchanged by P_batch (no ledger allocation).  count=0 -> no state, no S
   frame; a later count=1 works.  count=1 remote -> exactly one field-exact UseCS.
   count=1 local -> exactly one local decision.  Each a fresh session. */
static int case_scalar_count0(const char *iceccd)
{
    Farm f;
    MsgChannel *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated");
    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "scalar client connected");
    {
        /* count=0: no reply, and no GetCS forwarded to S (no request state) */
        REQUIRE(client->send_msg(make_getcs("c0.cpp", 0)), "sent GetCS(count=0)");
        std::string s;
        REQUIRE(expect_no_frame(client, 1500, &s), "count=0: no reply to the client");
        std::string s2;
        REQUIRE(expect_no_frame(f.sched, 1500, &s2), "count=0: no GetCS forwarded to S");
        /* same client count=1 works: one forwarded GetCS + one relayed UseCS */
        REQUIRE(client->send_msg(make_getcs("c0-then-1.cpp", 1)), "same client sent GetCS(count=1)");
        Msg *fwd = wait_for_type(f.sched, Msg::GET_CS, 4000);
        REQUIRE_OR_ABORT(fwd != nullptr, "count=1 forwarded to S after count=0");
        uint32_t cid = 0; { GetCSMsg *g = dynamic_cast<GetCSMsg *>(fwd); if (g) cid = g->client_id; }
        delete fwd;
        f.sched->send_msg(UseCSMsg("x86_64", "10.0.0.9", 3632u, 8000, true, cid, 0));
        std::vector<SeenUseCS> v = capture_use_cs(client, 1, 4000);
        REQUIRE(v.size() == 1 && v[0].job_id == 8000, "count=1 after count=0: exactly one UseCS");
    }
done:
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

static int case_scalar_count1(const char *iceccd, bool local)
{
    Farm f;
    MsgChannel *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated");
    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "scalar client connected");
    {
        REQUIRE(client->send_msg(make_getcs("c1.cpp", 1)), "sent GetCS(count=1)");
        Msg *fwd = wait_for_type(f.sched, Msg::GET_CS, 4000);
        REQUIRE_OR_ABORT(fwd != nullptr, "count=1 forwarded to S");
        uint32_t cid = 0; unsigned int fc = 1; { GetCSMsg *g = dynamic_cast<GetCSMsg *>(fwd); if (g) { cid = g->client_id; fc = g->count; } }
        delete fwd;
        REQUIRE(fc == 1, "forwarded count is exactly 1 (scalar path)");
        if (local) {
            f.sched->send_msg(UseCSMsg("x86_64", "127.0.0.1", 0, 8100, true, cid, 0));
        } else {
            f.sched->send_msg(UseCSMsg("x86_64", "10.7.7.7", 4444u, 8100, false, cid, 9));
        }
        std::vector<SeenUseCS> v = capture_use_cs(client, 1, 4000);
        REQUIRE_OR_ABORT(v.size() == 1 && v[0].job_id == 8100, "exactly one UseCS for count=1");
        if (!local) {
            /* remote scalar relay preserves every field exactly */
            REQUIRE(v[0].host == "10.7.7.7" && v[0].port == 4444u && !v[0].got_env && v[0].matched_job_id == 9,
                    "scalar remote UseCS relayed field-exact");
        }
        std::string s;
        REQUIRE(expect_no_frame(client, 1000, &s), "exactly one UseCS (no second)");
    }
done:
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Items 3/4 -- mixed batch: interleave Remote / Local / NoCS decisions behind a
   capacity blocker.  spec is 3 chars from {R,L,N}.  While the slot is held, every
   REMOTE decision is delivered immediately and field-exact; every LOCAL/NoCS
   decision is queued (not delivered).  After release the queued ones deliver one
   at a time in order on each client JobDone; capacity returns to baseline. */
static int case_mixed(const char *iceccd, const std::string &spec)
{
    Farm f;
    MsgChannel *blocker = nullptr, *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated (-m1)");
    blocker = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(blocker != nullptr, "blocker connected");
    {
        JobLocalBeginMsg jlb(0, "blk.o", false, "mixed-blocker", "g++ -c blk.cpp -o blk.o", JobLocalBeginMsg::LocalFlagNone);
        REQUIRE_OR_ABORT(blocker->send_msg(jlb), "blocker sent JobLocalBegin");
        Msg *ack = wait_for_type(blocker, Msg::JOB_LOCAL_BEGIN, 5000);
        REQUIRE_OR_ABORT(ack != nullptr, "daemon granted the blocker the slot");
        delete ack;
    }
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500); delete m; }

    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "batch client connected");
    {
        unsigned int fcount = 0;
        const uint32_t cid = publish_batch(client, f.sched, "mix.cpp", 3, &fcount);
        REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
        const uint32_t ID[3] = { 8300, 8301, 8302 };
        for (int i = 0; i < 3; ++i) {
            if (spec[i] == 'R')
                f.sched->send_msg(UseCSMsg("x86_64", "10.5.5.5", 5555u, ID[i], true, cid, 7));
            else if (spec[i] == 'L')
                f.sched->send_msg(UseCSMsg("x86_64", "127.0.0.1", 0, ID[i], true, cid, 0));
            else /* 'N' */
                f.sched->send_msg(NoCSMsg(ID[i], cid));
        }
        /* while the blocker holds: exactly the REMOTE decisions reach the client */
        std::vector<SeenUseCS> during = capture_use_cs(client, 3, 2500);
        std::vector<uint32_t> want_remote, want_queued;
        for (int i = 0; i < 3; ++i) (spec[i] == 'R' ? want_remote : want_queued).push_back(ID[i]);
        fprintf(stderr, "         (spec=%s during=%zu remote-expected=%zu)\n",
                spec.c_str(), during.size(), want_remote.size());
        REQUIRE(during.size() == want_remote.size(),
                "only the remote decisions are delivered while capacity is occupied");
        for (uint32_t rid : want_remote) {
            bool got = false;
            for (const SeenUseCS &s : during)
                if (s.job_id == rid) { got = true;
                    REQUIRE(s.host == "10.5.5.5" && s.port == 5555u && s.matched_job_id == 7,
                            "remote decision relayed field-exact"); }
            REQUIRE(got, "each remote decision delivered immediately");
        }
        for (uint32_t qid : want_queued) {
            bool leaked = false;
            for (const SeenUseCS &s : during) if (s.job_id == qid) leaked = true;
            REQUIRE(!leaked, "a queued local/NoCS decision is NOT delivered while blocked");
        }
        /* release -> the queued local/NoCS decisions deliver one at a time */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        for (uint32_t qid : want_queued) {
            std::vector<SeenUseCS> one = capture_use_cs(client, 1, 4000);
            REQUIRE(one.size() == 1 && one[0].job_id == qid, "queued decision delivered in order after release");
            JobDoneMsg d(qid, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d);
        }
        /* the remote decisions retained their field identity; complete them */
        for (uint32_t rid : want_remote) { JobDoneMsg d(rid, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
    }
done:
    delete blocker;
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Item 4 extra -- batch NoCS dedup + excess: a repeated NoCS(N1) is ignored; a
   fourth distinct NoCS beyond the requested count is terminalized exactly once as
   excess and never delivered. */
static int case_nocs_dedup_excess(const char *iceccd)
{
    Farm f;
    MsgChannel *blocker = nullptr, *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated (-m1)");
    blocker = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(blocker != nullptr, "blocker connected");
    {
        JobLocalBeginMsg jlb(0, "blk.o", false, "nde-blocker", "g++ -c blk.cpp -o blk.o", JobLocalBeginMsg::LocalFlagNone);
        REQUIRE_OR_ABORT(blocker->send_msg(jlb), "blocker sent JobLocalBegin");
        Msg *ack = wait_for_type(blocker, Msg::JOB_LOCAL_BEGIN, 5000);
        REQUIRE_OR_ABORT(ack != nullptr, "daemon granted the blocker the slot");
        delete ack;
    }
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500); delete m; }
    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "batch client connected");
    {
        unsigned int fcount = 0;
        const uint32_t cid = publish_batch(client, f.sched, "nde.cpp", 3, &fcount);
        REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
        const uint32_t N1 = 8400, N2 = 8401, N3 = 8402, N4 = 8403;
        f.sched->send_msg(NoCSMsg(N1, cid));
        f.sched->send_msg(NoCSMsg(N1, cid));   /* repeated -> ignored */
        f.sched->send_msg(NoCSMsg(N2, cid));
        f.sched->send_msg(NoCSMsg(N3, cid));
        usleep(300 * 1000);
        f.sched->send_msg(NoCSMsg(N4, cid));   /* 4th distinct -> excess */
        std::vector<DoneFrame> term = collect_job_done(f.sched, 3000);
        fprintf(stderr, "         (N1=%d N4=%d)\n", count_id(term, N1), count_id(term, N4));
        REQUIRE(count_id(term, N4) == 1, "4th distinct NoCS terminalized exactly once as excess");
        for (const DoneFrame &d : term) if (d.job_id == N4) REQUIRE(d.exitcode == 107, "excess terminal carries exit 107");
        REQUIRE(count_id(term, N1) == 0, "repeated NoCS(N1) ignored -- no excess terminal");
        /* release: exactly N1,N2,N3 deliver one at a time; N4 never delivered */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        uint32_t acc[3] = { N1, N2, N3 };
        for (int i = 0; i < 3; ++i) {
            std::vector<SeenUseCS> one = capture_use_cs(client, 1, 4000);
            REQUIRE(one.size() == 1 && one[0].job_id == acc[i], "accepted NoCS decision delivered in order");
            JobDoneMsg d(acc[i], 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d);
        }
        REQUIRE(no_use_cs(client, 1500), "the excess N4 was never delivered to the client");
    }
done:
    delete blocker;
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Audit #2/#4 helper: a blocker + a batch client with N local decisions. */
static uint32_t audit_setup(const char *iceccd, Farm &f, MsgChannel *&blocker,
                            MsgChannel *&client, int nlocal, const uint32_t *ids)
{
    if (!setup_farm(iceccd, 1, f)) return 0;
    blocker = connect_unix_bounded(f.socket_path, 5000);
    if (!blocker) return 0;
    JobLocalBeginMsg jlb(0, "blk.o", false, "audit-blocker", "g++ -c blk.cpp -o blk.o", JobLocalBeginMsg::LocalFlagNone);
    if (!blocker->send_msg(jlb)) return 0;
    Msg *ack = wait_for_type(blocker, Msg::JOB_LOCAL_BEGIN, 5000);
    if (!ack) return 0;
    delete ack;
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500); delete m; }
    client = connect_unix_bounded(f.socket_path, 5000);
    if (!client) return 0;
    unsigned int fc = 0;
    const uint32_t cid = publish_batch(client, f.sched, "audit.cpp", 3, &fc);
    if (!cid) return 0;
    for (int i = 0; i < nlocal; ++i)
        f.sched->send_msg(UseCSMsg("x86_64", "127.0.0.1", 0, ids[i], true, cid, 0));
    usleep(200 * 1000);
    return cid;
}

/* Audit #1 (defect #2): a delivered batch-local entry is a proper scalar-lane
   CLIENTWORK job -- a production CompileFile(__client) emits JobBegin via the
   ordinary local path (RED@old draft: client left in WAITCOMPILE, no JobBegin). */
static int case_audit_jobbegin(const char *iceccd)
{
    Farm f; MsgChannel *blocker = nullptr, *client = nullptr; bool clean = false;
    const uint32_t L1 = 8500;
    const uint32_t cid = audit_setup(iceccd, f, blocker, client, 1, &L1);
    REQUIRE_OR_ABORT(cid != 0, "audit farm + one local decision set up");
    {
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        std::vector<SeenUseCS> a = capture_use_cs(client, 1, 4000);
        REQUIRE_OR_ABORT(a.size() == 1 && a[0].job_id == L1, "L1 local decision delivered");
        CompileJob job;
        job.setLanguage(CompileJob::Lang_CXX); job.setCompilerName("g++");
        job.setJobID(L1); job.setEnvironmentVersion("__client"); job.setTargetPlatform("x86_64");
        job.setInputFile(f.work + "/a.cpp"); job.setOutputFile(f.work + "/a.o"); job.setWorkingDirectory(f.work);
        CompileFileMsg compile(&job);
        REQUIRE(client->send_msg(compile), "client sent production CompileFile(__client) for L1");
        Msg *jb = wait_for_type(f.sched, Msg::JOB_BEGIN, 4000);
        REQUIRE(jb != nullptr, "CompileFile(__client) emitted JobBegin via the ordinary local path");
        { JobBeginMsg *b = dynamic_cast<JobBeginMsg *>(jb); REQUIRE(b && b->job_id == L1, "JobBegin carries the local job id L1"); }
        delete jb;
    }
done:
    delete blocker; delete client; teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Audit #2 (defect #3): completing a local entry then vanishing must not crash --
   the next local's delivery failure is handled by the scalar lane, never by a
   deleting call inside handle_job_done. */
static int case_audit_delivery_fail(const char *iceccd)
{
    Farm f; MsgChannel *blocker = nullptr, *client = nullptr; bool clean = false;
    const uint32_t ids[2] = { 8600, 8601 };
    const uint32_t cid = audit_setup(iceccd, f, blocker, client, 2, ids);
    REQUIRE_OR_ABORT(cid != 0, "audit farm + two local decisions set up");
    {
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        std::vector<SeenUseCS> a = capture_use_cs(client, 1, 4000);
        REQUIRE_OR_ABORT(a.size() == 1 && a[0].job_id == ids[0], "L1 delivered active");
        /* Deterministic next-local delivery failure (local-oracle 17:16 #1):
           freeze the daemon, put L1's completion AHEAD of the client EOF in one
           stream, close the peer, then resume.  On resume the daemon processes
           L1's completion (binds L2) and then finds the peer gone when it tries to
           deliver L2 -- exercising exactly the failed-next-local path. */
        REQUIRE_OR_ABORT(kill(f.pid, SIGSTOP) == 0, "daemon frozen (SIGSTOP)");
        usleep(80 * 1000);
        { JobDoneMsg d(ids[0], 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        client->flush_pending();
        delete client; client = nullptr;      /* EOF, ordered AFTER the completion */
        usleep(80 * 1000);
        REQUIRE(kill(f.pid, SIGCONT) == 0, "daemon resumed (SIGCONT)");

        /* exact settlement: L1's completion forwarded once; L2 (delivered-but-
           unfinished on the abnormal end) settled once; no cancellation since all
           expected decisions were recorded (delivered==expected). */
        std::vector<DoneFrame> term = collect_job_done(f.sched, 4000);
        fprintf(stderr, "         (delivery-fail settlement %s cancels=%d)\n",
                ids_to_string(done_order(term)).c_str(), count_cancellations(term, cid));
        REQUIRE(count_id(term, ids[0]) == 1, "L1 completion forwarded exactly once");
        REQUIRE(count_id(term, ids[1]) == 1, "L2 settled exactly once on the abnormal end");
        REQUIRE(count_cancellations(term, cid) == 1, "one cancellation for the undelivered 3rd decision (2 of 3 recorded)");

        /* capacity restored + real trailing exchange: a fresh scalar-local client
           gets a decision (proves the daemon is live and the slot is free). */
        MsgChannel *live = connect_unix_bounded(f.socket_path, 3000);
        REQUIRE_OR_ABORT(live != nullptr, "daemon survived the failed next-local delivery");
        live->send_msg(make_getcs("post.cpp", 1));
        Msg *lf = wait_for_type(f.sched, Msg::GET_CS, 4000);
        REQUIRE(lf != nullptr, "trailing scalar GetCS reached S");
        uint32_t lc = 0; { GetCSMsg *g = dynamic_cast<GetCSMsg *>(lf); if (g) lc = g->client_id; }
        delete lf;
        f.sched->send_msg(UseCSMsg("x86_64", "127.0.0.1", 0, 8650, true, lc, 0));
        std::vector<SeenUseCS> pv = capture_use_cs(live, 1, 4000);
        REQUIRE(pv.size() == 1 && (pv.empty() || pv[0].job_id == 8650), "trailing scalar-local exchange completed");
        delete live;
    }
done:
    delete blocker; delete client; teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Audit #3 (defect #4): scheduler loss with one active + one queued local must
   leak no pending and restore capacity -- proven by a clean exit and by the slot
   being free again for a fresh JobLocalBegin after reconnect. */
static int case_audit_sched_loss(const char *iceccd)
{
    Farm f; MsgChannel *blocker = nullptr, *client = nullptr; bool clean = false;
    const uint32_t ids[2] = { 8700, 8701 };
    const uint32_t cid = audit_setup(iceccd, f, blocker, client, 2, ids);
    REQUIRE_OR_ABORT(cid != 0, "audit farm + two local decisions set up");
    {
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        std::vector<SeenUseCS> a = capture_use_cs(client, 1, 4000);
        REQUIRE_OR_ABORT(a.size() == 1 && a[0].job_id == ids[0], "L1 active, L2 queued");
        /* drop the scheduler channel -> established scheduler loss with a queued
           and an active batch-local entry */
        delete f.sched; f.sched = nullptr;
        usleep(300 * 1000);
        /* daemon reconnects; accept its Login + ConfCS, then a fresh local blocker
           must get the slot (capacity was restored, not left charged) */
        Msg *login = nullptr;
        f.sched = accept_login_channel(f.listener, 15000, &login);
        REQUIRE_OR_ABORT(f.sched != nullptr && login != nullptr, "daemon reconnected after scheduler loss");
        delete login;
        f.sched->send_msg(ConfCSMsg());
        usleep(150 * 1000);
        MsgChannel *b2 = connect_unix_bounded(f.socket_path, 4000);
        REQUIRE(b2 != nullptr, "fresh client connected after reconnect");
        if (b2) {
            JobLocalBeginMsg j2(0, "b2.o", false, "post-loss", "g++ -c b2.cpp -o b2.o", JobLocalBeginMsg::LocalFlagNone);
            b2->send_msg(j2);
            Msg *ack2 = wait_for_type(b2, Msg::JOB_LOCAL_BEGIN, 5000);
            REQUIRE(ack2 != nullptr, "capacity restored after scheduler loss (slot re-granted)");
            delete ack2; delete b2;
        }
    }
done:
    delete blocker; delete client; teardown_farm(f, &clean);
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
    else if (c == "teardown-eof-0")     rc = run_teardown_remote(argv[1], 0, false);
    else if (c == "teardown-eof-1")     rc = run_teardown_remote(argv[1], 1, false);
    else if (c == "teardown-eof-2")     rc = run_teardown_remote(argv[1], 2, false);
    else if (c == "teardown-eof-3")     rc = run_teardown_remote(argv[1], 3, false);
    else if (c == "teardown-early-1")   rc = run_teardown_remote(argv[1], 1, true);
    else if (c == "teardown-local-queued") rc = run_teardown_local(argv[1], "queued");
    else if (c == "teardown-local-active") rc = run_teardown_local(argv[1], "active");
    else if (c == "teardown-normal-local") rc = run_teardown_local(argv[1], "normal");
    else if (c == "scalar-count0")      rc = case_scalar_count0(argv[1]);
    else if (c == "scalar-count1-remote") rc = case_scalar_count1(argv[1], false);
    else if (c == "scalar-count1-local")  rc = case_scalar_count1(argv[1], true);
    else if (c == "mixed-LRR")          rc = case_mixed(argv[1], "LRR");
    else if (c == "mixed-RLR")          rc = case_mixed(argv[1], "RLR");
    else if (c == "mixed-RNR")          rc = case_mixed(argv[1], "RNR");
    else if (c == "nocs-dedup-excess")  rc = case_nocs_dedup_excess(argv[1]);
    else if (c == "audit-jobbegin")     rc = case_audit_jobbegin(argv[1]);
    else if (c == "audit-delivery-fail") rc = case_audit_delivery_fail(argv[1]);
    else if (c == "audit-sched-loss")   rc = case_audit_sched_loss(argv[1]);
    else { fprintf(stderr, "unknown case: %s\n", c.c_str()); return 2; }

    fprintf(stderr, "%s [%s] (%d failure%s)\n",
            rc ? "RESULT: FAIL" : "RESULT: PASS", c.c_str(), rc, rc == 1 ? "" : "s");
    return rc ? 1 : 0;
}
