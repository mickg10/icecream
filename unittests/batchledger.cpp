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

/* `what` may be a const char* OR a std::string: bind it to a std::string and
   always print via .c_str().  Passing a std::string straight into
   fprintf("%s", ...) is undefined behaviour (varargs mismatch). */
#define REQUIRE(cond, what)                                             \
    do {                                                                \
        const std::string _rq_msg = (what);                             \
        if (cond) { fprintf(stderr, "ok       - %s\n", _rq_msg.c_str()); } \
        else { fprintf(stderr, "FAILED   - %s\n", _rq_msg.c_str()); ++failures; } \
    } while (0)

#define REQUIRE_OR_ABORT(cond, what)                                    \
    do {                                                                \
        const std::string _rq_msg = (what);                             \
        if (cond) { fprintf(stderr, "ok       - %s\n", _rq_msg.c_str()); } \
        else {                                                          \
            fprintf(stderr, "FAILED   - %s\n", _rq_msg.c_str()); ++failures; \
            fprintf(stderr, "ABORT    - precondition failed\n");        \
            goto done;                                                  \
        }                                                               \
    } while (0)

/* ---------------------------------------------------------------- harness --- */
static bool sched_barrier(MsgChannel *sched, int window_msec);   /* fwd: used by setup_farm */
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
/* Stream status carried by EVERY collection result (local-oracle 20:28 #2): a
   helper never silently discards evidence.  `unexpected` lists the Msg::Value of any
   frame that was not the wanted type (and not an explicitly-permitted benign one);
   `eof`/`read_error` record a closed/broken channel.  `clean()` is true only for a
   window with no eof, no read error, and no unexpected frame. */
struct StreamStatus {
    bool eof = false;
    bool read_error = false;
    std::vector<int> unexpected;   /* Msg::Value codes of unexpected frames */
    bool clean() const { return !eof && !read_error && unexpected.empty(); }
    std::string why() const {
        std::string s;
        if (eof) s += "eof ";
        if (read_error) s += "read_error ";
        if (!unexpected.empty()) {
            s += "unexpected{";
            for (size_t i = 0; i < unexpected.size(); ++i) s += (i ? "," : "") + std::to_string(unexpected[i]);
            s += "} ";
        }
        return s.empty() ? std::string("clean") : s;
    }
};

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
    uint32_t sys_msec;
    uint32_t pfaults;
    uint32_t in_compressed;
    uint32_t in_uncompressed;
    uint32_t out_compressed;
    uint32_t out_uncompressed;
    uint32_t client_count;
};

/* Collect ALL JobDone frames over `window_msec` in EXACT ARRIVAL ORDER.  An
   ordered multiset -- three JobDone of one id are three ordered entries and never
   satisfy a three-distinct-id expectation.  Returns whether the channel errored
   (EOF/read) so absence/quiet-window checks can fail closed. */
/* Collect JOB_DONE frames in a window and ALWAYS carry stream status (local-oracle
   20:28 #2): any non-JOB_DONE frame except a benign periodic STATS/MON_STATS is
   recorded as unexpected -- STATUS_TEXT is NOT benign here (it must be consumed only
   by the explicit barrier; a stray one is a sequencing error); eof is recorded. */
struct DoneResult {
    std::vector<DoneFrame> frames;
    StreamStatus st;
    /* read convenience for done_order/count_id/done_frames_exact and range-for;
       callers still REQUIRE st.clean() to reject eof/read-error/unexpected frames. */
    operator const std::vector<DoneFrame> &() const { return frames; }
    size_t size() const { return frames.size(); }
    bool empty() const { return frames.empty(); }
    std::vector<DoneFrame>::const_iterator begin() const { return frames.begin(); }
    std::vector<DoneFrame>::const_iterator end() const { return frames.end(); }
};
static DoneResult collect_job_done(MsgChannel *sched, int window_msec)
{
    DoneResult r;
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
                    f.sys_msec = d->sys_msec;
                    f.pfaults = d->pfaults;
                    f.in_compressed = d->in_compressed;
                    f.in_uncompressed = d->in_uncompressed;
                    f.out_compressed = d->out_compressed;
                    f.out_uncompressed = d->out_uncompressed;
                    f.client_count = d->client_count;
                    r.frames.push_back(f);
                }
            } else if (*msg == Msg::STATS || *msg == Msg::MON_STATS) {
                /* benign periodic load/monitoring report on the scheduler channel --
                   background traffic, not a protocol frame; do not count it. */
            } else {
                /* any other frame -- including a stray STATUS_TEXT, JobBegin, UseCS
                   relay -- is unexpected in a completion window (sequencing error). */
                r.st.unexpected.push_back((int)(Msg::Value)(*msg));
            }
            delete msg;
        }
        if (sched->at_eof()) { r.st.eof = true; break; }
    }
    return r;
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

/* Exact ordered-frame comparator (local-oracle 5256331302 #1): the collected
   JobDone stream must equal the expected ordered list over EVERY discriminating
   field -- job id, signed exit, origin, and client-id-cancellation identity.  Any
   extra/missing/reordered/field-altered frame fails. */
/* EVERY serialized JobDone field the sender controls (local-oracle 19:02 #4): id,
   signed exit, origin (from_server) + raw flags, cancellation identity
   (unknown_client), real/user/sys time, page faults, all four byte counters, and
   the daemon-set client count. */
struct ExpectDone {
    uint32_t job_id;
    int      exitcode;
    bool     from_server;
    uint32_t unknown_client;
    uint32_t real_msec;
    uint32_t user_msec;
    uint32_t sys_msec;
    uint32_t pfaults;
    uint32_t in_compressed;
    uint32_t in_uncompressed;
    uint32_t out_compressed;
    uint32_t out_uncompressed;
    uint32_t flags;
    uint32_t client_count;
};
/* Exact ordered comparison of every field, and rejection of any unrelated frame
   seen in the same window (`unrelated`). */
/* Exact ordered comparison of the frames only; callers separately REQUIRE the
   collection's StreamStatus is clean (no eof/read-error/unexpected). */
static bool done_frames_exact(const std::vector<DoneFrame> &got,
                              const std::vector<ExpectDone> &want, std::string *why)
{
    if (got.size() != want.size()) {
        if (why) *why = "frame count " + std::to_string(got.size()) + " != expected " + std::to_string(want.size());
        return false;
    }
    for (size_t i = 0; i < got.size(); ++i) {
        const DoneFrame &g = got[i];
        const ExpectDone &w = want[i];
        if (g.job_id != w.job_id || g.exitcode != w.exitcode || g.from_server != w.from_server
                || g.unknown_client != w.unknown_client || g.real_msec != w.real_msec
                || g.user_msec != w.user_msec || g.sys_msec != w.sys_msec || g.pfaults != w.pfaults
                || g.in_compressed != w.in_compressed || g.in_uncompressed != w.in_uncompressed
                || g.out_compressed != w.out_compressed || g.out_uncompressed != w.out_uncompressed
                || g.flags != w.flags || g.client_count != w.client_count) {
            if (why) *why = "frame[" + std::to_string(i) + "] mismatch: got id=" + std::to_string(g.job_id)
                          + " exit=" + std::to_string(g.exitcode) + " fromS=" + std::to_string(g.from_server)
                          + " unk=" + std::to_string(g.unknown_client) + " real=" + std::to_string(g.real_msec)
                          + " user=" + std::to_string(g.user_msec) + " sys=" + std::to_string(g.sys_msec)
                          + " pf=" + std::to_string(g.pfaults) + " inC=" + std::to_string(g.in_compressed)
                          + " inU=" + std::to_string(g.in_uncompressed) + " outC=" + std::to_string(g.out_compressed)
                          + " outU=" + std::to_string(g.out_uncompressed) + " flags=" + std::to_string(g.flags)
                          + " cc=" + std::to_string(g.client_count);
            return false;
        }
    }
    return true;
}

/* Capture up to `want` USE_CS frames, EVERY serialized field.  On the client
   channel a batch request receives only USE_CS (and, at teardown, End); any other
   frame is unexpected and recorded. */
struct SeenUseCS {
    uint32_t job_id;
    std::string host;
    uint32_t port;
    std::string platform;
    bool got_env;
    uint32_t client_id;
    uint32_t matched_job_id;
};
struct UseCSResult {
    std::vector<SeenUseCS> frames;
    StreamStatus st;
    /* read convenience; callers still REQUIRE st.clean() at delivery assertions */
    size_t size() const { return frames.size(); }
    bool empty() const { return frames.empty(); }
    const SeenUseCS &operator[](size_t i) const { return frames[i]; }
    std::vector<SeenUseCS>::const_iterator begin() const { return frames.begin(); }
    std::vector<SeenUseCS>::const_iterator end() const { return frames.end(); }
};
static UseCSResult capture_use_cs(MsgChannel *client, int want, int timeout_msec)
{
    UseCSResult r;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (client && static_cast<int>(r.frames.size()) < want && Clock::now() < deadline) {
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
                    r.frames.push_back(s);
                }
            } else {
                r.st.unexpected.push_back((int)(Msg::Value)(*msg));   /* no benign frames on the client channel */
            }
            delete msg;
        }
        if (client->at_eof()) { r.st.eof = true; break; }
    }
    return r;
}

/* A UseCS-absence claim that FAILS CLOSED (local-oracle item 2): true only for
   bounded silence -- no USE_CS delivered AND no eof / read error / unexpected
   frame.  A closed or errored channel is never treated as "no decision". */
static bool no_use_cs(MsgChannel *client, int window_msec)
{
    UseCSResult r = capture_use_cs(client, 1, window_msec);
    return r.frames.empty() && r.st.clean();
}

/* Full-tuple UseCS expectation + exact comparator (local-oracle 20:28 #3): every
   serialized field a delivered decision carries -- job id, host, port, platform,
   got-env, CLIENT id, matched id.  Each field is compared and named on mismatch so
   a field-at-a-time negative-control table can prove every field load-bearing. */
struct ExpectUseCS {
    uint32_t job_id;
    std::string host;
    uint32_t port;
    std::string platform;
    bool got_env;
    uint32_t client_id;
    uint32_t matched_job_id;
};
static bool use_cs_exact(const SeenUseCS &g, const ExpectUseCS &w, std::string *why)
{
    const char *bad = nullptr;
    if (g.job_id != w.job_id) bad = "job_id";
    else if (g.host != w.host) bad = "host";
    else if (g.port != w.port) bad = "port";
    else if (g.platform != w.platform) bad = "platform";
    else if (g.got_env != w.got_env) bad = "got_env";
    else if (g.client_id != w.client_id) bad = "client_id";
    else if (g.matched_job_id != w.matched_job_id) bad = "matched_job_id";
    if (bad) { if (why) *why = std::string("UseCS field mismatch: ") + bad; return false; }
    return true;
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
    /* activation barrier (not a sleep): ConfCS -> GET_INTERNALS -> STATUS_TEXT proves
       the daemon processed ConfCS (session active) before any case proceeds. */
    return sched_barrier(f.sched, 4000);
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

/* Observable same-stream barrier: a GetInternalStatus on the scheduler channel is
   answered unconditionally by a StatusText (scheduler_get_internals), and the
   daemon processes scheduler messages in FIFO order, so receiving the StatusText
   reply proves every decision sent before it on the SAME channel has been
   recorded.  Replaces timing sleeps used as ordering evidence.  Only use where no
   other daemon->scheduler frame is expected between the burst and the reply
   (wait_for_type would otherwise drain it). */
static bool sched_barrier(MsgChannel *sched, int window_msec)
{
    if (!sched || !sched->send_msg(GetInternalStatus())) return false;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(window_msec);
    while (Clock::now() < deadline) {
        Msg *m = sched->get_msg(1, true);
        if (m) {
            const bool status = (*m == Msg::STATUS_TEXT);
            const bool benign = (*m == Msg::STATS || *m == Msg::MON_STATS);
            const int code = (int)(Msg::Value)(*m);
            delete m;
            if (status) return true;   /* barrier satisfied */
            if (!benign) {
                /* a lifecycle/request/terminal frame before the barrier reply is a
                   sequencing error -- do NOT silently consume it. */
                fprintf(stderr, "         (sched_barrier: unexpected frame %d before STATUS_TEXT)\n", code);
                return false;
            }
        }
        if (sched->at_eof()) return false;
    }
    return false;   /* timeout without STATUS_TEXT */
}

/* Strict wait for an exact JobBegin id on the scheduler channel: permits benign
   periodic STATS/MON_STATS, but rejects any other frame (evidence, not discarded)
   and fails closed on eof/timeout. */
static bool wait_job_begin(MsgChannel *sched, uint32_t id, int window_msec, std::string *why)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(window_msec);
    while (Clock::now() < deadline) {
        Msg *m = sched ? sched->get_msg(1, true) : nullptr;
        if (m) {
            if (*m == Msg::JOB_BEGIN) {
                JobBeginMsg *b = dynamic_cast<JobBeginMsg *>(m);
                const bool ok = (b && b->job_id == id);
                if (!ok && why) *why = "JobBegin id " + std::to_string(b ? b->job_id : 0) + " != " + std::to_string(id);
                delete m;
                return ok;
            }
            const bool benign = (*m == Msg::STATS || *m == Msg::MON_STATS);
            const int code = (int)(Msg::Value)(*m);
            delete m;
            if (!benign) { if (why) *why = "unexpected frame " + std::to_string(code) + " before JobBegin"; return false; }
        }
        if (sched && sched->at_eof()) { if (why) *why = "eof before JobBegin"; return false; }
    }
    if (why) *why = "timeout waiting for JobBegin " + std::to_string(id);
    return false;
}

/* Drive a delivered local batch entry through its REAL lifecycle and PROVE it
   completes (local-oracle 20:28 #4): CompileFile(__client, exact id) -> exact
   JobBegin(exact id) -> JobDone(exact id) -> the forwarded terminal is exactly one
   JobDone with EVERY serialized field intact and a clean stream.  A rejected
   completion (bare UseCS -> JobDone against the tightened product) yields no
   forwarded terminal and fails here rather than passing silently.
   `expect_client_count` is the daemon's clients.size() at forward time. */
static bool complete_local_entry(MsgChannel *client, MsgChannel *sched,
                                 const std::string &work, uint32_t id, int exitcode,
                                 uint32_t expect_client_count)
{
    std::string why;
    CompileJob job;
    job.setLanguage(CompileJob::Lang_CXX);
    job.setCompilerName("g++");
    job.setJobID(id);
    job.setEnvironmentVersion("__client");
    job.setTargetPlatform("x86_64");
    char in[512], out[512];
    snprintf(in, sizeof in, "%s/e%u.cpp", work.c_str(), id);
    snprintf(out, sizeof out, "%s/e%u.o", work.c_str(), id);
    job.setInputFile(in);
    job.setOutputFile(out);
    job.setWorkingDirectory(work);
    bool ok = false;
    if (!client->send_msg(CompileFileMsg(&job))) { why = "CompileFile send failed"; }
    else if (!wait_job_begin(sched, id, 4000, &why)) { /* why set */ }
    else {
        JobDoneMsg d(id, exitcode, JobDoneMsg::FROM_SUBMITTER);
        d.real_msec = 1;
        d.user_msec = 1;
        if (!client->send_msg(d)) { why = "JobDone send failed"; }
        else {
            /* the forwarded completion: exactly one JobDone, every field intact, clean stream */
            DoneResult r = collect_job_done(sched, 2500);
            if (!r.st.clean()) { why = "forward stream not clean: " + r.st.why(); }
            else {
                ExpectDone w{ id, exitcode, false, 0, 1, 1, 0, 0, 0, 0, 0, 0,
                              (uint32_t)JobDoneMsg::FROM_SUBMITTER, expect_client_count };
                ok = done_frames_exact(r.frames, std::vector<ExpectDone>{ w }, &why);
            }
        }
    }
    if (!ok) fprintf(stderr, "         (complete_local_entry %u: %s)\n", id, why.c_str());
    return ok;
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

    /* three remote decisions with visibly DISTINCT full field tuples */
    const uint32_t J1 = 6100, J2 = 6101, J3 = 6102, U = 6199;
    struct { uint32_t id; const char *host; unsigned port; unsigned matched; } R[3] = {
        { J1, "10.0.0.1", 3001, 1 }, { J2, "10.0.0.2", 3002, 2 }, { J3, "10.0.0.3", 3003, 3 } };
    bool sent = true;
    for (int i = 0; i < 3; ++i)
        sent = sent && f.sched->send_msg(UseCSMsg("x86_64", R[i].host, R[i].port, R[i].id, true, cid, R[i].matched));
    REQUIRE_OR_ABORT(sent, "scheduler delivered three remote decisions");
    UseCSResult got = capture_use_cs(client, 3, 6000);
    REQUIRE_OR_ABORT(got.size() == 3, "client received all three remote UseCS");
    bool tuples_ok = true;
    for (int i = 0; i < 3; ++i)
        tuples_ok = tuples_ok && got[i].job_id == R[i].id && got[i].host == R[i].host
                 && got[i].port == R[i].port && got[i].matched_job_id == R[i].matched
                 && got[i].got_env && got[i].platform == "x86_64";
    REQUIRE(tuples_ok, "each remote UseCS delivered in exact FIFO order with its full field tuple");

    /* unmatched U (exit 91), then a duplicate J1 in one burst with valid J2/J3;
       distinct exit sentinels so a reorder or field swap fails. */
    JobDoneMsg dU(U, 91, JobDoneMsg::FROM_SUBMITTER); dU.real_msec = 5; dU.user_msec = 5;
    JobDoneMsg d1(J1, 11, JobDoneMsg::FROM_SUBMITTER); d1.real_msec = 1; d1.user_msec = 1;
    JobDoneMsg d2(J2, 12, JobDoneMsg::FROM_SUBMITTER); d2.real_msec = 2; d2.user_msec = 2;
    JobDoneMsg d3(J3, 13, JobDoneMsg::FROM_SUBMITTER); d3.real_msec = 3; d3.user_msec = 3;
    client->send_msg(dU);
    client->send_msg(d1);
    client->send_msg(d1);   /* duplicate of J1 */
    client->send_msg(d2);
    client->send_msg(d3);

    DoneResult f_done = collect_job_done(f.sched, 5000);
    std::string why;
    /* forwarded unchanged except client_count (=1, only the batch client is connected) */
    const uint32_t FS = (uint32_t)JobDoneMsg::FROM_SUBMITTER;
    std::vector<ExpectDone> want = {
        { J1, 11, false, 0, 1, 1, 0, 0, 0, 0, 0, 0, FS, 1 },
        { J2, 12, false, 0, 2, 2, 0, 0, 0, 0, 0, 0, FS, 1 },
        { J3, 13, false, 0, 3, 3, 0, 0, 0, 0, 0, 0, FS, 1 } };
    const bool exact = done_frames_exact(f_done, want, &why);
    fprintf(stderr, "         (scheduler saw %zu JobDone; exact=%d %s)\n", f_done.size(), exact, why.c_str());
    REQUIRE(exact, "scheduler observed exactly [J1/11, J2/12, J3/13] over every field -- U + duplicate J1 filtered, no extra/unrelated frame");

    /* trailing real scalar exchange (not merely a UNIX connection) */
    MsgChannel *live = connect_unix_bounded(f.socket_path, 3000);
    REQUIRE_OR_ABORT(live != nullptr, "daemon accepts a new client after the filtered dones");
    live->send_msg(make_getcs("post.cpp", 1));
    Msg *lf = wait_for_type(f.sched, Msg::GET_CS, 4000);
    REQUIRE(lf != nullptr, "trailing scalar GetCS reached S");
    uint32_t lc = 0; { GetCSMsg *g = dynamic_cast<GetCSMsg *>(lf); if (g) lc = g->client_id; }
    delete lf;
    f.sched->send_msg(UseCSMsg("x86_64", "10.9.9.9", 5000, 6150, true, lc, 0));
    UseCSResult pv = capture_use_cs(live, 1, 4000);
    REQUIRE(pv.size() == 1 && (pv.empty() || pv[0].job_id == 6150), "trailing scalar exchange delivered exactly one decision");
    delete live;
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
        UseCSResult got = capture_use_cs(client, 3, 6000);
        REQUIRE_OR_ABORT(got.size() == 3, "client received all three remote UseCS");

        /* complete J1 -> exactly one forwarded JobDone(J1) */
        { JobDoneMsg d(J1, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        DoneResult after_j1 = collect_job_done(f.sched, 2500);
        REQUIRE(count_id(after_j1, J1) == 1, "completing J1 forwards exactly one JobDone(J1)");

        /* late duplicate decision for the completed J1, visibly different fields */
        f.sched->send_msg(UseCSMsg("x86_64", "10.9.9.9", 3632u, J1, false, cid, 0));
        REQUIRE(no_use_cs(client, 2000), "late UseCS(J1) after completion is not delivered to the client");

        /* complete J2,J3 then drain: no resurrected excess terminal for J1, and
           exactly one completion each for J2,J3 */
        for (uint32_t j : { J2, J3 }) { JobDoneMsg d(j, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        DoneResult tail = collect_job_done(f.sched, 4000);
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
        UseCSResult got = capture_use_cs(client, 3, 6000);
        REQUIRE_OR_ABORT(got.size() == 3, "client received all three remote UseCS");

        /* clean End: a real EndMsg, no synthetic JobDone for the remote entries */
        REQUIRE(client->send_msg(EndMsg()), "client sent a clean EndMsg");
        DoneResult after = collect_job_done(f.sched, 4000);
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
        UseCSResult a = capture_use_cs(client, 1, 4000);
        REQUIRE(a.size() == 1 && a[0].job_id == L1, "after release exactly L1 is delivered");
        REQUIRE(no_use_cs(client, 1200), "L2 withheld until L1 completes");
        REQUIRE(complete_local_entry(client, f.sched, f.work, L1, 0, 2),
                "L1 completed via real lifecycle (CompileFile -> JobBegin -> JobDone)");
        UseCSResult b = capture_use_cs(client, 1, 4000);
        REQUIRE(b.size() == 1 && b[0].job_id == L2, "L2 delivered after L1 completes");
        REQUIRE(no_use_cs(client, 1200), "L3 withheld until L2 completes");
        REQUIRE(complete_local_entry(client, f.sched, f.work, L2, 0, 2),
                "L2 completed via real lifecycle");
        UseCSResult cc = capture_use_cs(client, 1, 4000);
        REQUIRE(cc.size() == 1 && cc[0].job_id == L3, "L3 delivered after L2 completes");
        REQUIRE(complete_local_entry(client, f.sched, f.work, L3, 0, 2),
                "L3 completed via real lifecycle");
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
        DoneResult term = collect_job_done(f.sched, 3000);
        fprintf(stderr, "         (unmatched terminals: N2=%d N3=%d)\n",
                count_id(term, N2), count_id(term, N3));
        REQUIRE(count_id(term, N2) == 0 && count_id(term, N3) == 0,
                "later NoCS accepted by ledger state, not rejected with JobDone(107)");

        /* release -> one local decision at a time as capacity frees */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        UseCSResult a = capture_use_cs(client, 1, 4000);
        REQUIRE(a.size() == 1 && a[0].job_id == N1, "after release exactly N1 local decision delivered");
        REQUIRE(complete_local_entry(client, f.sched, f.work, N1, 0, 2), "N1 completed via real lifecycle");
        UseCSResult b = capture_use_cs(client, 1, 4000);
        REQUIRE(b.size() == 1 && b[0].job_id == N2, "N2 delivered after N1 completes");
        REQUIRE(complete_local_entry(client, f.sched, f.work, N2, 0, 2), "N2 completed via real lifecycle");
        UseCSResult cc = capture_use_cs(client, 1, 4000);
        REQUIRE(cc.size() == 1 && cc[0].job_id == N3, "N3 delivered after N2 completes");
        REQUIRE(complete_local_entry(client, f.sched, f.work, N3, 0, 2), "N3 completed via real lifecycle");
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
            UseCSResult got = capture_use_cs(client, deliver, 5000);
            REQUIRE_OR_ABORT((int)got.size() == deliver, "the delivered remote decisions reached the client");
        }
        /* end the client */
        if (use_endmsg) {
            REQUIRE(client->send_msg(EndMsg()), "client sent a clean EndMsg");
        } else {
            delete client; client = nullptr;   /* raw EOF */
        }
        DoneResult term = collect_job_done(f.sched, 4000);

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
            UseCSResult pv = capture_use_cs(live, 1, 4000);
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
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 3000),
                         "daemon recorded + queued the three local decisions (GET_INTERNALS / STATUS_TEXT barrier, not a sleep)");

        if (scenario == "queued") {
            /* end early while all three are queued (blocker still holds) */
            REQUIRE(client->send_msg(EndMsg()), "client sent EndMsg with local entries queued");
            DoneResult term = collect_job_done(f.sched, 4000);
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
            UseCSResult a = capture_use_cs(client, 1, 4000);
            REQUIRE_OR_ABORT(a.size() == 1 && a[0].job_id == L1, "L1 delivered active after release");
            REQUIRE(client->send_msg(EndMsg()), "client sent EndMsg with L1 active");
            DoneResult term = collect_job_done(f.sched, 4000);
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
        } else { /* normal: deliver + complete all three via the real lifecycle, then End */
            { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
            uint32_t exp[3] = { L1, L2, L3 };
            for (int i = 0; i < 3; ++i) {
                UseCSResult v = capture_use_cs(client, 1, 4000);
                REQUIRE_OR_ABORT(v.st.clean() && v.size() == 1 && v[0].job_id == exp[i],
                                 "next local decision delivered in order on a clean stream");
                /* complete_local_entry now collects + compares THIS entry's forwarded
                   JobDone exactly (all fields, clean stream), so no separate check. */
                REQUIRE(complete_local_entry(client, f.sched, f.work, exp[i], 0, 2),
                        "local entry completed + forwarded exactly via real lifecycle");
            }
            REQUIRE(client->send_msg(EndMsg()), "client sent a normal EndMsg after all local done");
            DoneResult term = collect_job_done(f.sched, 3000);
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
        UseCSResult v = capture_use_cs(client, 1, 4000);
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
        UseCSResult v = capture_use_cs(client, 1, 4000);
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
        UseCSResult during = capture_use_cs(client, 3, 2500);
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
        /* release -> the queued local/NoCS decisions deliver one at a time; each is
           completed through its REAL lifecycle and its forwarded terminal is proven
           exactly (a bare UseCS -> JobDone would be rejected and must not pass). */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        for (uint32_t qid : want_queued) {
            UseCSResult one = capture_use_cs(client, 1, 4000);
            REQUIRE(one.st.clean() && one.size() == 1 && one[0].job_id == qid,
                    "queued decision delivered in order after release on a clean stream");
            REQUIRE(complete_local_entry(client, f.sched, f.work, qid, 0, 2),
                    "queued local/NoCS entry completed + forwarded exactly via real lifecycle");
        }
        /* the remote decisions retained their field identity; complete each and
           compare the FORWARDED terminal exactly (a remote entry accepts JobDone
           directly). */
        for (uint32_t rid : want_remote) {
            JobDoneMsg d(rid, 33, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1;
            REQUIRE(client->send_msg(d), "remote JobDone sent");
            DoneResult r = collect_job_done(f.sched, 2500);
            REQUIRE(r.st.clean(), "remote completion stream clean (no unexpected/eof)");
            ExpectDone w{ rid, 33, false, 0, 1, 1, 0, 0, 0, 0, 0, 0, (uint32_t)JobDoneMsg::FROM_SUBMITTER, 2 };
            std::string why;
            REQUIRE(done_frames_exact(r.frames, std::vector<ExpectDone>{ w }, &why),
                    "remote completion forwarded exactly over every field");
        }
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
        /* barrier (not a sleep): prove N1(x2)/N2/N3 are recorded before N4 arrives,
           so N4 is genuinely the 4th DISTINCT decision (-> excess), not a race. */
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 3000),
                         "daemon recorded the first three distinct NoCS before N4 (GET_INTERNALS / STATUS_TEXT barrier)");
        f.sched->send_msg(NoCSMsg(N4, cid));   /* 4th distinct -> excess */
        DoneResult term = collect_job_done(f.sched, 3000);
        fprintf(stderr, "         (N1=%d N4=%d)\n", count_id(term, N1), count_id(term, N4));
        REQUIRE(count_id(term, N4) == 1, "4th distinct NoCS terminalized exactly once as excess");
        for (const DoneFrame &d : term) if (d.job_id == N4) REQUIRE(d.exitcode == 107, "excess terminal carries exit 107");
        REQUIRE(count_id(term, N1) == 0, "repeated NoCS(N1) ignored -- no excess terminal");
        /* release: exactly N1,N2,N3 deliver one at a time; N4 never delivered */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        uint32_t acc[3] = { N1, N2, N3 };
        for (int i = 0; i < 3; ++i) {
            UseCSResult one = capture_use_cs(client, 1, 4000);
            REQUIRE(one.st.clean() && one.size() == 1 && one[0].job_id == acc[i],
                    "accepted NoCS decision delivered in order on a clean stream");
            REQUIRE(complete_local_entry(client, f.sched, f.work, acc[i], 0, 2),
                    "accepted NoCS entry completed + forwarded exactly via real lifecycle");
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
    /* barrier (not a sleep): the Ping reply proves the daemon recorded all N local
       decisions before the caller applies the next stimulus. */
    if (!sched_barrier(f.sched, 3000)) return 0;
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
        UseCSResult a = capture_use_cs(client, 1, 4000);
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
        UseCSResult a = capture_use_cs(client, 1, 4000);
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
        DoneResult term = collect_job_done(f.sched, 4000);
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
        UseCSResult pv = capture_use_cs(live, 1, 4000);
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
        UseCSResult a = capture_use_cs(client, 1, 4000);
        REQUIRE_OR_ABORT(a.size() == 1 && a[0].job_id == ids[0], "L1 active, L2 queued");
        /* drop the scheduler channel -> established scheduler loss with a queued
           and an active batch-local entry.  No pre-accept sleep: the bounded accept
           below already waits for the daemon's reconnect. */
        delete f.sched; f.sched = nullptr;
        /* daemon reconnects; accept its Login + ConfCS, then a fresh local blocker
           must get the slot (capacity was restored, not left charged) */
        Msg *login = nullptr;
        f.sched = accept_login_channel(f.listener, 15000, &login);
        REQUIRE_OR_ABORT(f.sched != nullptr && login != nullptr, "daemon reconnected after scheduler loss");
        delete login;
        REQUIRE_OR_ABORT(f.sched->send_msg(ConfCSMsg()), "re-sent ConfCS after reconnect");
        /* activation barrier (not a sleep) */
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "daemon re-activated after reconnect (barrier)");
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

/* count 1..3 REMOTE integration smoke (narrowed per local-oracle 19:02: this is
   NOT the full local/NoCS/duplicate/teardown reference enumeration -- those
   transitions are covered by local-capacity, batch-nocs, nocs-dedup-excess,
   teardown-*, invalid-jobdone and compile-started).  For N in {1,2,3} a batch of N
   REMOTE decisions is delivered FIFO with exact per-entry tuples, completed in
   exact order by distinct exit, and a NORMAL End settles nothing.  N==1 is the
   allocation-free SCALAR_ONE path; N=2,3 the ledger. */
static int case_transition_enum(const char *iceccd, int N)
{
    Farm f;
    MsgChannel *client = nullptr;
    bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated");
    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "client connected");
    {
        unsigned int fc = 0;
        const uint32_t cid = publish_batch(client, f.sched, "tr.cpp", (unsigned)N, &fc);
        REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
        REQUIRE(fc == (unsigned)N, "forwarded GetCS preserved the requested count");
        const uint32_t base = 9000 + N * 10;
        /* deliver: distinct field tuple per entry */
        for (int i = 0; i < N; ++i) {
            char host[32]; snprintf(host, sizeof host, "10.1.%d.%d", N & 255, (i + 1) & 255);
            f.sched->send_msg(UseCSMsg("x86_64", host, 4000u + i, base + i, true, cid, 10 + i));
        }
        UseCSResult got = capture_use_cs(client, N, 6000);
        REQUIRE_OR_ABORT((int)got.size() == N, "all N remote decisions delivered");
        bool tup = true;
        for (int i = 0; i < N; ++i) tup = tup && got[i].job_id == base + i && got[i].port == 4000u + i && got[i].matched_job_id == (uint32_t)(10 + i);
        REQUIRE(tup, "delivered decisions in FIFO order with exact per-entry tuples");
        /* complete: distinct exit + distinct real/user per entry -> exact ordered
           terminals; the daemon forwards *m unchanged except client_count (=1, only
           the batch client is connected). */
        for (int i = 0; i < N; ++i) { JobDoneMsg d(base + i, 30 + i, JobDoneMsg::FROM_SUBMITTER); d.real_msec = i + 1; d.user_msec = i + 1; client->send_msg(d); }
        DoneResult term = collect_job_done(f.sched, 4000);
        std::vector<ExpectDone> want;
        for (int i = 0; i < N; ++i)
            want.push_back({ base + i, 30 + i, false, 0,
                             (uint32_t)(i + 1), (uint32_t)(i + 1), 0, 0, 0, 0, 0, 0,
                             (uint32_t)JobDoneMsg::FROM_SUBMITTER, 1 });
        std::string why;
        const bool exact = done_frames_exact(term, want, &why);
        fprintf(stderr, "         (transition N=%d saw %zu JobDone; exact=%d %s)\n", N, term.size(), exact, why.c_str());
        REQUIRE(exact, "exact ordered terminals over every field");
        /* NORMAL End (all delivered+completed) settles nothing */
        REQUIRE(client->send_msg(EndMsg()), "client sent a normal EndMsg");
        DoneResult after = collect_job_done(f.sched, 2500);
        REQUIRE(after.empty(), "normal End emits no extra terminal or cancellation");
    }
done:
    delete client;
    teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Product correction #2 (local-oracle 17:34): a JobDone for a decision the client
   was never actually handed -- a still-queued LOCAL_WAIT_CAPACITY entry -- must be
   REJECTED (not forwarded to S) and must NOT mark the entry completed.  The former
   boolean ledger accepted any exact unfinished id, so it forwarded the premature
   JobDone and marked L2 completed -> L2 then never delivered (RED).  Under the
   tagged state only REMOTE_DELIVERED / LOCAL_COMPILE_STARTED accept a JobDone, so
   the premature frame is dropped and L2 delivers normally after L1. */
static int case_invalid_jobdone(const char *iceccd)
{
    Farm f; MsgChannel *blocker = nullptr, *client = nullptr; bool clean = false;
    const uint32_t ids[2] = { 8800, 8801 };   /* L1 active, L2 queued */
    const uint32_t cid = audit_setup(iceccd, f, blocker, client, 2, ids);
    REQUIRE_OR_ABORT(cid != 0, "audit farm + two local decisions set up");
    {
        /* release the blocker -> L1 delivers and holds the one slot; L2 stays a
           queued LOCAL_WAIT_CAPACITY entry (the lane is occupied by L1). */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        UseCSResult a = capture_use_cs(client, 1, 4000);
        REQUIRE_OR_ABORT(a.size() == 1 && a[0].job_id == ids[0], "L1 delivered and active; L2 queued");

        /* premature JobDone for the still-queued L2: rejected, never forwarded. */
        { JobDoneMsg d(ids[1], 55, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        DoneResult premature = collect_job_done(f.sched, 1500);
        REQUIRE(count_id(premature, ids[1]) == 0, "premature JobDone for the queued L2 is NOT forwarded to S");

        /* complete L1 for real -> forwarded once; the lane frees and L2 (still
           recorded, NOT completed by the premature frame) auto-binds. */
        CompileJob j1; j1.setLanguage(CompileJob::Lang_CXX); j1.setCompilerName("g++");
        j1.setJobID(ids[0]); j1.setEnvironmentVersion("__client"); j1.setTargetPlatform("x86_64");
        j1.setInputFile(f.work + "/a.cpp"); j1.setOutputFile(f.work + "/a.o"); j1.setWorkingDirectory(f.work);
        REQUIRE(client->send_msg(CompileFileMsg(&j1)), "L1 CompileFile(__client)");
        { Msg *jb = wait_for_type(f.sched, Msg::JOB_BEGIN, 4000); REQUIRE(jb != nullptr, "L1 JobBegin via ordinary local path"); delete jb; }
        { JobDoneMsg d(ids[0], 11, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        DoneResult t1 = collect_job_done(f.sched, 3000);
        REQUIRE(count_id(t1, ids[0]) == 1, "L1 completion forwarded exactly once");

        /* L2 must now deliver -- proof the premature JobDone did not complete it. */
        UseCSResult b = capture_use_cs(client, 1, 4000);
        REQUIRE(b.size() == 1 && b[0].job_id == ids[1], "queued L2 delivered after L1 (premature JobDone did not complete it)");
    }
done:
    delete blocker; delete client; teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Product correction #1 (local-oracle 17:34): two local entries on ONE batch
   client, each performing the real sequence UseCS -> CompileFile(distinct id) ->
   exact JobBegin -> JobDone.  This is the repeated-CompileFile path that made
   handle_compile_file's `client->job = job` overwrite (and leak) the prior entry's
   CompileJob; the fix releases/nulls cl->job on the local-completion transition
   before binding the successor.  Behaviourally this asserts each entry's JobBegin
   carries its OWN distinct id and each JobDone is forwarded exactly once (the
   successor never inherits stale job state); the retained CompileJob itself is a
   pure memory leak, verified separately by an ASan/LSan build of this same case
   (a leak makes iceccd exit non-zero -> the clean-exit assertion below fails). */
static int case_local_lifetime(const char *iceccd)
{
    Farm f; MsgChannel *blocker = nullptr, *client = nullptr; bool clean = false;
    const uint32_t ids[2] = { 8810, 8811 };
    const uint32_t cid = audit_setup(iceccd, f, blocker, client, 2, ids);
    REQUIRE_OR_ABORT(cid != 0, "audit farm + two local decisions set up");
    {
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        for (int i = 0; i < 2; ++i) {
            /* deliver entry i */
            UseCSResult a = capture_use_cs(client, 1, 4000);
            REQUIRE_OR_ABORT(a.size() == 1 && a[0].job_id == ids[i], "entry delivered in order");
            /* real production CompileFile(__client) for THIS entry's distinct id */
            CompileJob job; job.setLanguage(CompileJob::Lang_CXX); job.setCompilerName("g++");
            job.setJobID(ids[i]); job.setEnvironmentVersion("__client"); job.setTargetPlatform("x86_64");
            char in[64], out[64]; snprintf(in, sizeof in, "%s/e%d.cpp", f.work.c_str(), i); snprintf(out, sizeof out, "%s/e%d.o", f.work.c_str(), i);
            job.setInputFile(in); job.setOutputFile(out); job.setWorkingDirectory(f.work);
            REQUIRE(client->send_msg(CompileFileMsg(&job)), "entry CompileFile(__client) sent");
            /* JobBegin must carry THIS entry's id -- never the prior (stale) job */
            Msg *jb = wait_for_type(f.sched, Msg::JOB_BEGIN, 4000);
            REQUIRE_OR_ABORT(jb != nullptr, "entry emitted JobBegin");
            { JobBeginMsg *b = dynamic_cast<JobBeginMsg *>(jb); REQUIRE(b && b->job_id == ids[i], "JobBegin carries THIS entry's distinct job id (no stale carry-over)"); }
            delete jb;
            /* complete this entry -> forwarded once; releases the compile job and
               binds the successor (i==0) */
            { JobDoneMsg d(ids[i], 20 + i, JobDoneMsg::FROM_SUBMITTER); d.real_msec = i + 1; d.user_msec = i + 1; client->send_msg(d); }
            DoneResult t = collect_job_done(f.sched, 3000);
            REQUIRE(count_id(t, ids[i]) == 1, "entry completion forwarded exactly once");
        }
    }
done:
    delete blocker; delete client; teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Refinement (local-oracle 17:58): LOCAL_COMPILE_STARTED.  A delivered-but-not-yet-
   compiled local entry (LOCAL_DELIVERED_SLOT_CHARGED) does NOT accept a JobDone, and
   handle_compile_file() validates the exact bound id before adopting the CompileJob.
   (a) a CompileFile with an unmatched id emits no JobBegin and adopts no job;
   (b) a JobDone for the delivered-not-started entry is rejected;
   then the real CompileFile starts it (JobBegin) and its JobDone is accepted.
   RED@boolean-55cbd1a: (a) the wrong id was adopted and announced (JobBegin), and
   (b) the delivered entry's `active` flag accepted+forwarded the early JobDone. */
static int case_compile_started(const char *iceccd)
{
    Farm f; MsgChannel *blocker = nullptr, *client = nullptr; bool clean = false;
    const uint32_t L1 = 8900;
    const uint32_t cid = audit_setup(iceccd, f, blocker, client, 1, &L1);
    REQUIRE_OR_ABORT(cid != 0, "audit farm + one local decision set up");
    {
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        UseCSResult a = capture_use_cs(client, 1, 4000);
        REQUIRE_OR_ABORT(a.size() == 1 && a[0].job_id == L1, "L1 delivered and slot-charged (not yet compiling)");

        /* (a) CompileFile with an UNMATCHED id: rejected -> no JobBegin, no adopt. */
        {
            CompileJob bad; bad.setLanguage(CompileJob::Lang_CXX); bad.setCompilerName("g++");
            bad.setJobID(9999); bad.setEnvironmentVersion("__client"); bad.setTargetPlatform("x86_64");
            bad.setInputFile(f.work + "/x.cpp"); bad.setOutputFile(f.work + "/x.o"); bad.setWorkingDirectory(f.work);
            REQUIRE(client->send_msg(CompileFileMsg(&bad)), "client sent CompileFile with an unmatched id");
            Msg *jb = wait_for_type(f.sched, Msg::JOB_BEGIN, 1500);
            REQUIRE(jb == nullptr, "unmatched CompileFile emits NO JobBegin (id not adopted)");
            delete jb;
        }

        /* (b) JobDone for the delivered-but-not-started L1: rejected, not forwarded. */
        { JobDoneMsg d(L1, 44, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        DoneResult early = collect_job_done(f.sched, 1500);
        REQUIRE(count_id(early, L1) == 0, "JobDone for a delivered-but-not-started entry is NOT forwarded");

        /* real CompileFile(L1) -> JobBegin(L1) -> LOCAL_COMPILE_STARTED */
        CompileJob job; job.setLanguage(CompileJob::Lang_CXX); job.setCompilerName("g++");
        job.setJobID(L1); job.setEnvironmentVersion("__client"); job.setTargetPlatform("x86_64");
        job.setInputFile(f.work + "/a.cpp"); job.setOutputFile(f.work + "/a.o"); job.setWorkingDirectory(f.work);
        REQUIRE(client->send_msg(CompileFileMsg(&job)), "client sent the real CompileFile(L1)");
        { Msg *jb = wait_for_type(f.sched, Msg::JOB_BEGIN, 4000);
          REQUIRE_OR_ABORT(jb != nullptr, "real CompileFile(L1) emitted JobBegin");
          JobBeginMsg *b = dynamic_cast<JobBeginMsg *>(jb); REQUIRE(b && b->job_id == L1, "JobBegin carries L1"); delete jb; }

        /* JobDone(L1) now accepted (STARTED) -> forwarded exactly once */
        { JobDoneMsg d(L1, 11, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        DoneResult t = collect_job_done(f.sched, 3000);
        REQUIRE(count_id(t, L1) == 1, "JobDone for the STARTED entry forwarded exactly once");
    }
done:
    delete blocker; delete client; teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Group 5 (local-oracle 20:28): force the scheduler send to FAIL at JobBegin for a
   delivered local entry.  Freeze the daemon, close the scheduler AHEAD of the
   CompileFile in one ordered sequence, resume.  handle_compile_file's
   send_scheduler(JobBegin) then fails; the entry is never marked LOCAL_COMPILE_
   STARTED (a34e825 sends JobBegin before STARTED), the client is torn down, and the
   charged slot is restored -- proven by the daemon reconnecting and a fresh blocker
   getting the slot.  Bounded cleanup, no leak, no JobBegin to any live scheduler. */
static int case_jobbegin_send_fail(const char *iceccd)
{
    Farm f; MsgChannel *blocker = nullptr, *client = nullptr; bool clean = false;
    const uint32_t L1 = 8950;
    const uint32_t cid = audit_setup(iceccd, f, blocker, client, 1, &L1);
    REQUIRE_OR_ABORT(cid != 0, "audit farm + one local decision set up");
    {
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        UseCSResult a = capture_use_cs(client, 1, 4000);
        REQUIRE_OR_ABORT(a.st.clean() && a.size() == 1 && a[0].job_id == L1,
                         "L1 delivered + slot charged (LOCAL_DELIVERED_SLOT_CHARGED)");
        /* freeze, close the scheduler AHEAD of the CompileFile, resume -> the
           JobBegin send fails deterministically on resume. */
        REQUIRE_OR_ABORT(kill(f.pid, SIGSTOP) == 0, "daemon frozen (SIGSTOP)");
        usleep(80 * 1000);
        delete f.sched; f.sched = nullptr;      /* scheduler gone -> JobBegin send will fail */
        {
            CompileJob j; j.setLanguage(CompileJob::Lang_CXX); j.setCompilerName("g++");
            j.setJobID(L1); j.setEnvironmentVersion("__client"); j.setTargetPlatform("x86_64");
            j.setInputFile(f.work + "/a.cpp"); j.setOutputFile(f.work + "/a.o"); j.setWorkingDirectory(f.work);
            client->send_msg(CompileFileMsg(&j)); client->flush_pending();
        }
        usleep(80 * 1000);
        REQUIRE(kill(f.pid, SIGCONT) == 0, "daemon resumed (SIGCONT)");
        /* the daemon reconnects after the scheduler loss; accept + re-activate */
        Msg *login = nullptr;
        f.sched = accept_login_channel(f.listener, 15000, &login);
        REQUIRE_OR_ABORT(f.sched != nullptr && login != nullptr, "daemon reconnected after the failed JobBegin send");
        delete login;
        REQUIRE_OR_ABORT(f.sched->send_msg(ConfCSMsg()), "re-sent ConfCS after reconnect");
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "daemon re-activated (barrier)");
        /* no JobBegin for L1 reaches the NEW scheduler (the failed send was to the old one) */
        {
            DoneResult jb = collect_job_done(f.sched, 1200);   /* also drains any stray */
            REQUIRE(jb.st.clean() && jb.frames.empty(), "no terminal/JobBegin for L1 on the reconnected scheduler");
        }
        /* capacity restored: a fresh blocker gets the slot */
        MsgChannel *b2 = connect_unix_bounded(f.socket_path, 4000);
        REQUIRE(b2 != nullptr, "fresh client connected after reconnect");
        if (b2) {
            JobLocalBeginMsg j2(0, "b2.o", false, "post-fail", "g++ -c b2.cpp -o b2.o", JobLocalBeginMsg::LocalFlagNone);
            b2->send_msg(j2);
            Msg *ack2 = wait_for_type(b2, Msg::JOB_LOCAL_BEGIN, 4000);
            REQUIRE(ack2 != nullptr, "capacity restored after the failed JobBegin send (slot re-granted)");
            delete ack2; delete b2;
        }
    }
done:
    delete blocker; delete client; teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Helper self-controls (local-oracle 20:28 #2): each stream helper must FAIL CLOSED
   on the evidence it used to discard.  Driven over a socketpair (no daemon) so the
   exact frames are controlled.  A helper that silently consumed these would pass the
   negated REQUIREs -- so each of these is itself a control on the harness. */
static int case_helper_controls(const char *iceccd)
{
    (void)iceccd;
    /* 1. unexpected-before-barrier: a JobDone ahead of STATUS_TEXT invalidates the
       barrier (a wait_for_type-style barrier would skip it and wrongly succeed). */
    { ChannelPair p = make_channel_pair();
      REQUIRE_OR_ABORT(p.a && p.b, "self-control pair 1 established");
      p.b->send_msg(JobDoneMsg(1, 0, JobDoneMsg::FROM_SUBMITTER));
      p.b->send_msg(StatusTextMsg("x")); p.b->flush_pending();
      usleep(50 * 1000);
      REQUIRE(!sched_barrier(p.a, 1500), "barrier rejects a JobDone before STATUS_TEXT (unexpected-before-barrier)");
      delete p.a; delete p.b; }
    /* 2. EOF-before-barrier: a closed peer never yields STATUS_TEXT -> fail closed. */
    { ChannelPair p = make_channel_pair();
      REQUIRE_OR_ABORT(p.a && p.b, "self-control pair 2 established");
      delete p.b; usleep(50 * 1000);
      REQUIRE(!sched_barrier(p.a, 1500), "barrier fails closed on eof (EOF-before-barrier)");
      delete p.a; }
    /* 3. unrelated-in-completion-window: a JobBegin in a JobDone window is flagged. */
    { ChannelPair p = make_channel_pair();
      REQUIRE_OR_ABORT(p.a && p.b, "self-control pair 3 established");
      p.b->send_msg(JobBeginMsg(5, 1)); p.b->flush_pending();
      usleep(50 * 1000);
      DoneResult r = collect_job_done(p.a, 1000);
      REQUIRE(!r.st.clean() && r.frames.empty(),
              "collect_job_done flags an unrelated frame (unrelated-in-completion-window)");
      delete p.a; delete p.b; }
    /* 4. EOF-in-completion-window: a closed peer sets st.eof -> not clean. */
    { ChannelPair p = make_channel_pair();
      REQUIRE_OR_ABORT(p.a && p.b, "self-control pair 4 established");
      delete p.b; usleep(50 * 1000);
      DoneResult r = collect_job_done(p.a, 1000);
      REQUIRE(!r.st.clean() && r.st.eof, "collect_job_done fails closed on eof (EOF-in-completion-window)");
      delete p.a; }
    /* 5. buffered-extra-UseCS: a second buffered UseCS is detected, not ignored. */
    { ChannelPair p = make_channel_pair();
      REQUIRE_OR_ABORT(p.a && p.b, "self-control pair 5 established");
      p.b->send_msg(UseCSMsg("x86_64", "10.0.0.1", 1u, 100, true, 1, 0));
      p.b->send_msg(UseCSMsg("x86_64", "10.0.0.2", 2u, 101, true, 1, 0));
      p.b->flush_pending(); usleep(50 * 1000);
      UseCSResult one = capture_use_cs(p.a, 1, 1500);
      REQUIRE(one.st.clean() && one.size() == 1 && one[0].job_id == 100, "captured exactly the first UseCS");
      REQUIRE(!no_use_cs(p.a, 800), "a buffered extra UseCS is detected, not silently ignored (buffered-extra-UseCS)");
      delete p.a; delete p.b; }
done:
    return failures;
}

/* Field-at-a-time negative control (local-oracle 20:28 #3): capture one remote
   UseCS with a fully-distinct tuple; use_cs_exact matches it; then mutating ANY
   single expected field makes use_cs_exact fail -- proving every field load-bearing
   (message type is fixed by the collector accepting only USE_CS). */
static int case_usecs_fields(const char *iceccd)
{
    Farm f; MsgChannel *client = nullptr; bool clean = false;
    REQUIRE_OR_ABORT(setup_farm(iceccd, 1, f), "fresh farm activated");
    client = connect_unix_bounded(f.socket_path, 5000);
    REQUIRE_OR_ABORT(client != nullptr, "client connected");
    {
        unsigned int fc = 0;
        const uint32_t cid = publish_batch(client, f.sched, "uf.cpp", 1, &fc);
        REQUIRE_OR_ABORT(cid != 0, "forwarded GetCS carried a client id");
        f.sched->send_msg(UseCSMsg("x86_64", "10.7.7.7", 6161u, 8500, true, cid, 9));
        UseCSResult got = capture_use_cs(client, 1, 4000);
        REQUIRE_OR_ABORT(got.st.clean() && got.size() == 1, "one UseCS delivered on a clean stream");
        const SeenUseCS s = got[0];
        ExpectUseCS base{ 8500, "10.7.7.7", 6161u, "x86_64", true, cid, 9 };
        std::string why;
        REQUIRE(use_cs_exact(s, base, &why), "the delivered UseCS matches the full expected tuple");
        { ExpectUseCS w = base; w.job_id ^= 1u;          std::string y; REQUIRE(!use_cs_exact(s, w, &y), "job_id is load-bearing"); }
        { ExpectUseCS w = base; w.host = "10.7.7.8";     std::string y; REQUIRE(!use_cs_exact(s, w, &y), "host is load-bearing"); }
        { ExpectUseCS w = base; w.port ^= 1u;            std::string y; REQUIRE(!use_cs_exact(s, w, &y), "port is load-bearing"); }
        { ExpectUseCS w = base; w.platform = "aarch64";  std::string y; REQUIRE(!use_cs_exact(s, w, &y), "platform is load-bearing"); }
        { ExpectUseCS w = base; w.got_env = !w.got_env;  std::string y; REQUIRE(!use_cs_exact(s, w, &y), "got_env is load-bearing"); }
        { ExpectUseCS w = base; w.client_id ^= 1u;       std::string y; REQUIRE(!use_cs_exact(s, w, &y), "client_id is load-bearing"); }
        { ExpectUseCS w = base; w.matched_job_id ^= 1u;  std::string y; REQUIRE(!use_cs_exact(s, w, &y), "matched_job_id is load-bearing"); }
    }
done:
    delete client; teardown_farm(f, &clean);
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
    else if (c == "transition-1")       rc = case_transition_enum(argv[1], 1);
    else if (c == "transition-2")       rc = case_transition_enum(argv[1], 2);
    else if (c == "transition-3")       rc = case_transition_enum(argv[1], 3);
    else if (c == "invalid-jobdone")    rc = case_invalid_jobdone(argv[1]);
    else if (c == "local-lifetime")     rc = case_local_lifetime(argv[1]);
    else if (c == "compile-started")    rc = case_compile_started(argv[1]);
    else if (c == "jobbegin-send-fail") rc = case_jobbegin_send_fail(argv[1]);
    else if (c == "usecs-fields")       rc = case_usecs_fields(argv[1]);
    else if (c == "helper-controls")    rc = case_helper_controls(argv[1]);
    else { fprintf(stderr, "unknown case: %s\n", c.c_str()); return 2; }

    fprintf(stderr, "%s [%s] (%d failure%s)\n",
            rc ? "RESULT: FAIL" : "RESULT: PASS", c.c_str(), rc, rc == 1 ? "" : "s");
    return rc ? 1 : 0;
}
