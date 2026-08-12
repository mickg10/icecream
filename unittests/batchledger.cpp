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
/* Evaluate `cond` BEFORE building `what`: many call sites pass an out-param to the
   condition (e.g. usecs_seq_exact(..., &why)) and embed that same `why` in the
   message.  If the message were built first, `why` would still be empty on failure
   and the diagnostic would read "(...)".  Ordering cond first makes the reason real. */
#define REQUIRE(cond, what)                                             \
    do {                                                                \
        const bool _rq_ok = (cond);                                     \
        const std::string _rq_msg = (what);                             \
        if (_rq_ok) { fprintf(stderr, "ok       - %s\n", _rq_msg.c_str()); } \
        else { fprintf(stderr, "FAILED   - %s\n", _rq_msg.c_str()); ++failures; } \
    } while (0)

#define REQUIRE_OR_ABORT(cond, what)                                    \
    do {                                                                \
        const bool _rq_ok = (cond);                                     \
        const std::string _rq_msg = (what);                             \
        if (_rq_ok) { fprintf(stderr, "ok       - %s\n", _rq_msg.c_str()); } \
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

/* STRICT typed wait (local-oracle 5258904367 #2 / 21:58): return the first `wanted`
   frame.  `allow_stats` is the channel-specific STATS policy -- true only on the
   scheduler channel, whose contract includes periodic STATS/MON_STATS; false
   (default) on a client channel, where a STATS occurrence is unexpected.  Any other
   non-permitted frame is evidence, not noise: stop and return nullptr (so the
   caller's REQUIRE fails closed), reporting the offending type.  Fails closed on
   eof/timeout. */
static Msg *wait_for_type(MsgChannel *channel, Msg::Value wanted, int timeout_msec, bool allow_stats = false)
{
    if (!channel) return nullptr;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (Clock::now() < deadline) {
        Msg *msg = channel->get_msg(1, true);
        if (msg) {
            if (*msg == wanted) return msg;
            if (allow_stats && (*msg == Msg::STATS || *msg == Msg::MON_STATS)) { delete msg; continue; }
            fprintf(stderr, "         (wait_for_type: unexpected frame %d before wanted %d)\n",
                    (int)(Msg::Value)(*msg), (int)wanted);
            delete msg;
            return nullptr;
        }
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
/* Explicit termination reason (local-oracle 21:58): a reader records HOW it stopped
   rather than inferring timeout from the absence of a sentinel.  STATUS_SENTINEL is
   the FIFO reader's clean stop; OK is a window collector's normal stop (count reached
   or window elapsed); TIMEOUT is the FIFO reader stopping with no sentinel; EOF_CLOSED
   / READ_ERROR are the peer closing / a read or poll error. */
enum class Term { OK, STATUS_SENTINEL, TIMEOUT, EOF_CLOSED, READ_ERROR };
static const char *term_name(Term t)
{
    switch (t) {
        case Term::OK:              return "ok";
        case Term::STATUS_SENTINEL: return "status";
        case Term::TIMEOUT:         return "timeout";
        case Term::EOF_CLOSED:      return "eof";
        case Term::READ_ERROR:      return "read_error";
    }
    return "?";
}

/* Stream status carried by EVERY window-collection result (local-oracle 20:28 #2 /
   21:58): a helper never silently discards evidence.  `unexpected` lists the
   Msg::Value of any frame that was not the wanted type (and not an explicitly-
   permitted benign one); `term` is the EXPLICIT termination reason.  `clean()` is
   true only for a normal stop with no eof/read-error/unexpected frame. */
struct StreamStatus {
    Term term = Term::OK;
    std::vector<int> unexpected;   /* Msg::Value codes of unexpected frames */
    bool clean() const { return term == Term::OK && unexpected.empty(); }
    std::string why() const {
        std::string s = std::string("term=") + term_name(term);
        if (!unexpected.empty()) {
            s += " unexpected{";
            for (size_t i = 0; i < unexpected.size(); ++i) s += (i ? "," : "") + std::to_string(unexpected[i]);
            s += "}";
        }
        return s;
    }
};

/* Poll a channel's fd after get_msg returned nothing, to classify EOF vs a genuine
   read/poll error (local-oracle item 1: read_error must actually be assigned). */
static Term classify_broken(MsgChannel *ch)
{
    struct pollfd pfd = { ch->fd, POLLIN | POLLHUP | POLLERR, 0 };
    const int rc = poll(&pfd, 1, 0);
    if (rc < 0) return Term::READ_ERROR;
    if (pfd.revents & (POLLERR | POLLNVAL)) return Term::READ_ERROR;
    return Term::EOF_CLOSED;   /* at_eof() with no error revent -> orderly close */
}

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
/* No implicit conversion or convenience accessors (local-oracle 5258904367 #3):
   every caller must reach through `.frames` AND inspect `.st`, so stream status can
   never be silently bypassed. */
struct DoneResult {
    std::vector<DoneFrame> frames;
    StreamStatus st;
};
static DoneFrame to_done_frame(JobDoneMsg *d)
{
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
    return f;
}
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
                    r.frames.push_back(to_done_frame(d));
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
        if (sched->at_eof()) { r.st.term = classify_broken(sched); break; }
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
    /* No convenience accessors (local-oracle 5258904367 #3): callers reach .frames
       AND inspect .st, so stream status is never bypassed. */
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
        if (client->at_eof()) { r.st.term = classify_broken(client); break; }
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

/* ------------------------------------------------ FIFO-sentinel reader --- */
/* local-oracle 20:28/5258904367: read the EXACT frame sequence on a channel up to a
   GET_INTERNALS -> STATUS_TEXT sentinel, with no sleeps and no buffered leftovers.
   Because the daemon processes each channel FIFO, the STATUS_TEXT proves every frame
   sent before our GET_INTERNALS has been produced; anything read before it is the
   complete, ordered set.  Benign periodic STATS/MON_STATS are permitted; every other
   frame is retained IN ORDER for exact comparison.  Termination is one of
   STATUS_TEXT (clean) / eof / read_error / timeout(no status). */
struct FrameSeq {
    std::vector<Msg *> frames;   /* exact non-benign sequence up to (excl.) STATUS_TEXT */
    Term term = Term::TIMEOUT;   /* explicit termination reason (local-oracle 21:58) */
    FrameSeq() = default;
    FrameSeq(const FrameSeq &) = delete;
    FrameSeq &operator=(const FrameSeq &) = delete;
    FrameSeq(FrameSeq &&o) noexcept : frames(std::move(o.frames)), term(o.term) { o.frames.clear(); }
    ~FrameSeq() { for (Msg *m : frames) delete m; }
    bool clean() const { return term == Term::STATUS_SENTINEL; }
    std::string why() const { return std::string("term=") + term_name(term); }
};
/* `allow_stats` is the channel-specific STATS policy (local-oracle 21:58): the
   scheduler channel's contract includes periodic STATS/MON_STATS, so pass true there;
   on a client channel pass false so a STATS occurrence lands in `frames` and fails
   the exact-sequence match rather than being silently skipped. */
static FrameSeq read_until_status(MsgChannel *ch, int window_msec, bool allow_stats)
{
    FrameSeq r;
    if (!ch || !ch->send_msg(GetInternalStatus())) { r.term = Term::READ_ERROR; return r; }
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(window_msec);
    while (Clock::now() < deadline) {
        if (ch->has_msg()) {
            Msg *m = ch->get_msg(0, true);
            if (m) {
                if (*m == Msg::STATUS_TEXT) { r.term = Term::STATUS_SENTINEL; delete m; return r; }
                if (allow_stats && (*m == Msg::STATS || *m == Msg::MON_STATS)) { delete m; continue; }
                r.frames.push_back(m);
                continue;
            }
        }
        if (ch->at_eof()) { r.term = classify_broken(ch); return r; }
        const int remaining = std::max<int>(1, static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
        struct pollfd pfd = { ch->fd, POLLIN | POLLHUP | POLLERR, 0 };
        const int rc = poll(&pfd, 1, remaining);
        if (rc == 0) continue;
        if (rc < 0) { if (errno == EINTR) continue; r.term = Term::READ_ERROR; return r; }
        if (pfd.revents & (POLLERR | POLLNVAL)) { r.term = Term::READ_ERROR; return r; }
        if (pfd.revents & (POLLIN | POLLHUP)) {
            if (!ch->read_a_bit()) {
                /* a failed read after POLLHUP is an orderly EOF, not a read error;
                   only a genuine POLLERR/POLLNVAL (classify_broken) is READ_ERROR. */
                r.term = ch->at_eof() ? Term::EOF_CLOSED : classify_broken(ch);
                return r;
            }
        }
    }
    r.term = Term::TIMEOUT;   /* window elapsed with no STATUS_TEXT -> !clean */
    return r;
}

/* The sequence must be exactly `want` UseCS in order, clean termination, nothing
   else (detects missing/extra/reordered/unrelated). */
static bool usecs_seq_exact(const FrameSeq &r, const std::vector<ExpectUseCS> &want, std::string *why)
{
    if (!r.clean()) { if (why) *why = "stream " + r.why(); return false; }
    if (r.frames.size() != want.size()) {
        if (why) *why = "UseCS count " + std::to_string(r.frames.size()) + " != " + std::to_string(want.size());
        return false;
    }
    for (size_t i = 0; i < r.frames.size(); ++i) {
        if (!(*r.frames[i] == Msg::USE_CS)) {
            if (why) *why = "frame[" + std::to_string(i) + "] not USE_CS (type "
                          + std::to_string((int)(Msg::Value)(*r.frames[i])) + ")";
            return false;
        }
        UseCSMsg *u = dynamic_cast<UseCSMsg *>(r.frames[i]);
        SeenUseCS s{ u->job_id, u->hostname, u->port, u->host_platform, u->got_env != 0, u->client_id, u->matched_job_id };
        std::string w;
        if (!use_cs_exact(s, want[i], &w)) { if (why) *why = "frame[" + std::to_string(i) + "] " + w; return false; }
    }
    return true;
}

/* The sequence must be exactly `want` JobDone in order over every field, clean
   termination, nothing else. */
static bool done_seq_exact(const FrameSeq &r, const std::vector<ExpectDone> &want, std::string *why)
{
    if (!r.clean()) { if (why) *why = "stream " + r.why(); return false; }
    std::vector<DoneFrame> got;
    for (Msg *m : r.frames) {
        if (!(*m == Msg::JOB_DONE)) {
            if (why) *why = "unexpected frame type " + std::to_string((int)(Msg::Value)(*m)) + " in JobDone window";
            return false;
        }
        JobDoneMsg *d = dynamic_cast<JobDoneMsg *>(m);
        got.push_back({ d->job_id, d->exitcode, d->is_from_server(), d->flags, d->unknown_job_client_id(),
                        d->real_msec, d->user_msec, d->sys_msec, d->pfaults, d->in_compressed, d->in_uncompressed,
                        d->out_compressed, d->out_uncompressed, d->client_count });
    }
    return done_frames_exact(got, want, why);
}

/* has_msg()-before-poll absence helper: no complete frame may appear.  A frame
   already buffered in userspace is reported without waiting for a new edge;
   POLLERR/POLLNVAL and read errors fail closed. */
static bool expect_no_frame(MsgChannel *ch, int timeout_msec, std::string *seen, bool allow_stats = false)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_msec);
    while (ch && Clock::now() < deadline) {
        if (ch->has_msg()) {
            Msg *m = ch->get_msg(0, true);
            if (m) {
                if (allow_stats && (*m == Msg::STATS || *m == Msg::MON_STATS)) { delete m; continue; }
                if (seen) *seen = m->to_string(); delete m; return false;
            }
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
                if (m) {
                    if (allow_stats && (*m == Msg::STATS || *m == Msg::MON_STATS)) { delete m; continue; }
                    if (seen) *seen = m->to_string(); delete m; return false;
                }
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

/* Start a fresh iceccd with `slots` local slots against a fake scheduler and activate
   the session with ConfCS.  Returns false on any setup failure -- with a typed failed
   stage plus the retained work dir and iceccd.log path, and NO retry: a genuine
   failure must surface, not be masked.  The retained work template honors a nonempty
   TMPDIR (default /tmp) so an integration run can direct scratch off a full root fs. */
static bool setup_farm(const char *iceccd, int slots, Farm &f)
{
    const char *tmproot = getenv("TMPDIR");
    if (!tmproot || !*tmproot) {
        tmproot = "/tmp";
    }
    std::string tmpl = std::string(tmproot) + "/icecream-g4-batch.XXXXXX";
    std::vector<char> tbuf(tmpl.c_str(), tmpl.c_str() + tmpl.size() + 1);
    char *t = mkdtemp(tbuf.data());
    if (!t) { perror("mkdtemp"); return false; }   /* work dir not yet created */
    f.work = t;
    const std::string envdir = f.work + "/envs";
    f.socket_path = f.work + "/iceccd.sock";
    const std::string log = f.work + "/iceccd.log";
    mkdir(envdir.c_str(), 0700);
    fprintf(stderr, "retained work directory: %s\n", f.work.c_str());

    /* typed failed-stage diagnostic once the work dir exists; never retries. */
    auto fail = [&](const char *stage) -> bool {
        fprintf(stderr, "setup_farm FAILED stage=%s work=%s log=%s\n",
                stage, f.work.c_str(), log.c_str());
        return false;
    };

    /* Bind ONE ephemeral scheduler listener and keep it open through Login acceptance.
       The former probe-close-rebind reopened the same port, and another process could
       grab it in the close/rebind window -- the demonstrated fresh-farm race. */
    int port = 0;
    f.listener = listen_on_port(0, &port);
    if (f.listener < 0 || port <= 0) {
        return fail("listen_on_port");
    }

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
    if (f.pid < 0) return fail("fork");

    Msg *login = nullptr;
    f.sched = accept_login_channel(f.listener, 20000, &login);
    if (!f.sched || !login) { delete login; return fail("accept_login"); }
    delete login;
    if (!f.sched->send_msg(ConfCSMsg())) return fail("send_confcs");
    /* activation barrier (not a sleep): ConfCS -> GET_INTERNALS -> STATUS_TEXT proves
       the daemon processed ConfCS (session active) before any case proceeds. */
    if (!sched_barrier(f.sched, 4000)) return fail("activation_barrier");
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
    Msg *fwd = wait_for_type(sched, Msg::GET_CS, 5000, true);
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

/* Strict wait for the forwarded JobDone with `id` on the scheduler channel (robust
   to the cross-channel race between the client's JobDone and a scheduler-side
   sentinel): permits benign STATS, rejects any other frame, fails closed on
   eof/timeout, and returns the full frame for exact field comparison. */
static bool wait_job_done(MsgChannel *sched, uint32_t id, int window_msec, DoneFrame *out, std::string *why)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(window_msec);
    while (Clock::now() < deadline) {
        Msg *m = sched ? sched->get_msg(1, true) : nullptr;
        if (m) {
            if (*m == Msg::JOB_DONE) {
                JobDoneMsg *d = dynamic_cast<JobDoneMsg *>(m);
                if (d && d->job_id == id) {
                    *out = { d->job_id, d->exitcode, d->is_from_server(), d->flags, d->unknown_job_client_id(),
                             d->real_msec, d->user_msec, d->sys_msec, d->pfaults, d->in_compressed, d->in_uncompressed,
                             d->out_compressed, d->out_uncompressed, d->client_count };
                    delete m;
                    return true;
                }
                if (why) *why = "JobDone id " + std::to_string(d ? d->job_id : 0) + " != " + std::to_string(id);
                delete m;
                return false;
            }
            const bool benign = (*m == Msg::STATS || *m == Msg::MON_STATS);
            const int code = (int)(Msg::Value)(*m);
            delete m;
            if (!benign) { if (why) *why = "unexpected frame " + std::to_string(code) + " before JobDone"; return false; }
        }
        if (sched && sched->at_eof()) { if (why) *why = "eof before JobDone"; return false; }
    }
    if (why) *why = "timeout waiting for JobDone " + std::to_string(id);
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
            /* the forwarded completion: wait strictly for the forwarded JobDone(id)
               (robust to the cross-channel race), compare EVERY field, then prove no
               extra frame follows via a zero-frame STATUS_TEXT sentinel. */
            DoneFrame got;
            if (!wait_job_done(sched, id, 3000, &got, &why)) { /* why set */ }
            else {
                ExpectDone w{ id, exitcode, false, 0, 1, 1, 0, 0, 0, 0, 0, 0,
                              (uint32_t)JobDoneMsg::FROM_SUBMITTER, expect_client_count };
                std::string cw;
                if (!done_frames_exact(std::vector<DoneFrame>{ got }, std::vector<ExpectDone>{ w }, &cw)) {
                    why = "forwarded JobDone field mismatch: " + cw;
                } else {
                    FrameSeq tail = read_until_status(sched, 1500, true);
                    if (!tail.clean()) why = "post-completion stream " + tail.why();
                    else if (!tail.frames.empty()) why = "unexpected extra frame after the forwarded JobDone";
                    else ok = true;
                }
            }
        }
    }
    if (!ok) fprintf(stderr, "         (complete_local_entry %u: %s)\n", id, why.c_str());
    return ok;
}

/* Prove a channel's pending inputs were consumed by the daemon (and any resulting
   FIFO work ran) with a client-side sentinel: nothing must be delivered back on that
   channel before its STATUS_TEXT.  Used to order a capacity-releasing event before
   reading the next delivery. */
static bool consumed_barrier(MsgChannel *ch)
{
    FrameSeq r = read_until_status(ch, 4000, false);
    return r.clean() && r.frames.empty();
}

/* Exact orderly-EOF observation of a TERMINATING client (local-oracle 00:12).  After
   the client's terminating input -- an EndMsg, or a half-closed write for a raw-EOF row
   -- the daemon settles every recorded decision to the scheduler and THEN closes the
   client (deletes the Client -> FIN).  The only success is a clean peer EOF: recv()==0
   with NOTHING delivered first.  POLLHUP alone never suffices (it can coincide with
   pending data), so we always confirm with a 0-byte read.  Outcomes are typed so the
   scenario line distinguishes them:
     - EOF_OK          orderly peer close, nothing read -> happens-before established;
     - UNEXPECTED_DATA a terminating client received a byte/frame (a sequencing bug);
     - POLL_ERROR      POLLERR/POLLNVAL or a hard read error;
     - TIMEOUT         no close within the window.
   The FIN we read is emitted only after handle_end ran, so EOF_OK proves every
   settlement terminal is already on the scheduler channel. */
enum class EndObs { EOF_OK, UNEXPECTED_DATA, POLL_ERROR, TIMEOUT };
static const char *endobs_name(EndObs e)
{
    switch (e) {
    case EndObs::EOF_OK: return "EOF_OK";
    case EndObs::UNEXPECTED_DATA: return "UNEXPECTED_DATA";
    case EndObs::POLL_ERROR: return "POLL_ERROR";
    case EndObs::TIMEOUT: return "TIMEOUT";
    }
    return "?";
}
static EndObs observe_client_eof(MsgChannel *client, int window_msec)
{
    if (!client) return EndObs::POLL_ERROR;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(window_msec);
    while (Clock::now() < deadline) {
        const int remaining = std::max<int>(1, static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
        struct pollfd pfd = { client->fd, POLLIN | POLLHUP | POLLERR, 0 };
        const int rc = poll(&pfd, 1, remaining);
        if (rc < 0) { if (errno == EINTR) continue; return EndObs::POLL_ERROR; }
        if (rc == 0) continue;
        if (pfd.revents & (POLLERR | POLLNVAL)) return EndObs::POLL_ERROR;
        /* POLLIN and/or POLLHUP: a genuine EOF requires the read itself to return 0. */
        char buf[64];
        const ssize_t n = recv(client->fd, buf, sizeof buf, MSG_DONTWAIT);
        if (n == 0) return EndObs::EOF_OK;
        if (n > 0) return EndObs::UNEXPECTED_DATA;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
        return EndObs::POLL_ERROR;
    }
    return EndObs::TIMEOUT;
}

/* Signal a RAW EOF (no EndMsg) to the daemon by half-closing the client's write side,
   keeping the read side so observe_client_eof can confirm the daemon's own close.  Used
   only where a half-close still lets the scenario proceed (the daemon does not need the
   client's read side gone); the full-EOF-during-freeze case uses a different witness. */
static bool client_send_raw_eof(MsgChannel *client)
{
    if (!client) return false;
    client->flush_pending();
    return shutdown(client->fd, SHUT_WR) == 0 || errno == ENOTCONN;
}

/* Read the EXACT scheduler terminal sequence up to a scheduler STATUS_TEXT sentinel
   (sentinel-bounded, not a fixed window).  Benign periodic STATS/MON_STATS skipped;
   every non-JOB_DONE frame recorded as unexpected; termination carried in .st so a
   caller can fail closed.  Precondition: the client input that produces these terminals
   has already been proven consumed by the caller (observe_client_eof for a terminating
   client, consumed_barrier for a mid-session one, or scheduler-FIFO order for a
   scheduler-only decision), so all terminals are present before our GET_INTERNALS and
   this read captures the complete ordered set. */
static DoneResult read_sched_terminals(MsgChannel *sched, int window_msec)
{
    DoneResult r;
    FrameSeq ds = read_until_status(sched, window_msec, true);
    if (ds.term != Term::STATUS_SENTINEL) r.st.term = ds.term;
    for (Msg *m : ds.frames) {
        if (*m == Msg::JOB_DONE) {
            JobDoneMsg *d = dynamic_cast<JobDoneMsg *>(m);
            if (d) r.frames.push_back(to_done_frame(d));
        } else {
            r.st.unexpected.push_back((int)(Msg::Value)(*m));
        }
    }
    return r;
}

/* Read an EXACT ordered terminal sequence as the causal boundary (local-oracle 00:20)
   for the ONE scenario with no client channel to observe -- a full-EOF-during-freeze,
   where the client is deleted so the daemon's WRITE to it must fail.  Instead of a
   client witness we use the KNOWN final settlement frame (the EOF-origin cancellation)
   as the causal witness: once the exact ordered sequence has arrived over EVERY
   serialized field, the old client's EOF handler has necessarily run.  Then ONE
   scheduler sentinel must show an exact zero-frame tail.  Reads frame-by-frame up to
   `window`; STATS/MON_STATS benign; every deviation -- unrelated frame, wrong
   field/order, early scheduler EOF, missing frame (timeout), or any tail frame -- is a
   typed diagnostic.  Replaces the earlier drain-to-quiescence heuristic. */
static bool exact_terminals_then_quiet(MsgChannel *sched, const std::vector<ExpectDone> &want,
                                       int window_msec, std::string *why)
{
    std::vector<DoneFrame> got;
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(window_msec);
    while (got.size() < want.size()) {
        if (Clock::now() >= deadline) {
            if (why) *why = "timeout after " + std::to_string(got.size()) + "/"
                          + std::to_string(want.size()) + " terminals";
            return false;
        }
        Msg *m = sched->get_msg(1, true);
        if (!m) {
            if (sched->at_eof()) {
                if (why) *why = "scheduler EOF after " + std::to_string(got.size()) + " terminals";
                return false;
            }
            continue;
        }
        if (*m == Msg::STATS || *m == Msg::MON_STATS) { delete m; continue; }
        if (!(*m == Msg::JOB_DONE)) {
            if (why) *why = "unrelated frame type " + std::to_string((int)(Msg::Value)(*m));
            delete m;
            return false;
        }
        DoneFrame f = to_done_frame(dynamic_cast<JobDoneMsg *>(m));
        delete m;
        std::string fw;
        if (!done_frames_exact(std::vector<DoneFrame>{ f }, std::vector<ExpectDone>{ want[got.size()] }, &fw)) {
            if (why) *why = "terminal[" + std::to_string(got.size()) + "] " + fw;
            return false;
        }
        got.push_back(f);
    }
    /* the exact ordered settlement arrived; its last frame is the causal witness.
       one scheduler sentinel must now prove an exact zero-frame tail. */
    FrameSeq tail = read_until_status(sched, window_msec, true);
    if (!tail.clean()) { if (why) *why = "tail stream " + tail.why(); return false; }
    if (!tail.frames.empty()) { if (why) *why = "unexpected tail frame after settlement"; return false; }
    return true;
}

/* Sentinel-bounded settlement read for the EndMsg teardown path (local-oracle 00:12):
   observe the client's exact orderly EOF FIRST (the daemon settles then closes), then
   read the exact scheduler terminal sequence.  On any non-EOF_OK client outcome, .st
   carries READ_ERROR and `why` names the typed outcome, so the caller fails closed. */
static DoneResult settle_terminals(MsgChannel *client, MsgChannel *sched, int window_msec, std::string *why)
{
    DoneResult r;
    const EndObs o = observe_client_eof(client, window_msec);
    if (o != EndObs::EOF_OK) {
        if (why) *why = std::string("client EOF ") + endobs_name(o);
        r.st.term = Term::READ_ERROR;
        return r;
    }
    return read_sched_terminals(sched, window_msec);
}

/* Sentinel-bounded forwarded-terminal read for a MID-SESSION completion: the client
   sent a JobDone and stays connected, so its sentinel MUST return STATUS_TEXT (an EOF
   here is itself a defect); that proves the JobDone was consumed and any forwarded
   terminal emitted, then read the exact scheduler sequence.  Replaces the fixed-window
   collect_job_done at completion sites where the client is not ending. */
static DoneResult forwarded_terminals(MsgChannel *client, MsgChannel *sched, int window_msec, std::string *why)
{
    DoneResult r;
    if (!consumed_barrier(client)) {
        if (why) *why = "client JobDone not consumed (client sentinel not clean/empty)";
        r.st.term = Term::READ_ERROR;
        return r;
    }
    return read_sched_terminals(sched, window_msec);
}

/* Read exactly the one delivered LOCAL batch UseCS via the client FIFO sentinel: a
   synthetic 127.0.0.1:daemon_port(0) decision, platform x86_64, client_id 1, the
   originating matched id (0 for a same-daemon UseCS). */
static bool read_one_local_usecs(MsgChannel *client, uint32_t id, std::string *why,
                                 const char *platform = "x86_64")
{
    FrameSeq u = read_until_status(client, 4000, false);
    /* a same-daemon UseCS keeps the decision's host_platform ("x86_64"); a NoCS
       decision synthesises an empty platform.  Both: 127.0.0.1:0, client_id 1, matched 0. */
    ExpectUseCS w{ id, "127.0.0.1", 0u, platform, true, 1u, 0u };
    return usecs_seq_exact(u, std::vector<ExpectUseCS>{ w }, why);
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
    /* two-stage FIFO (local-oracle 22:28): (1) scheduler sentinel proves the daemon
       consumed the three decisions; (2) read the EXACT client UseCS sequence up to
       the client's STATUS_TEXT -- detects missing/extra/reordered/unrelated with no
       buffered leftover. */
    REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the three decisions (sentinel)");
    FrameSeq useq = read_until_status(client, 5000, false);
    std::vector<ExpectUseCS> uwant = {
        { J1, "10.0.0.1", 3001u, "x86_64", true, cid, 1 },
        { J2, "10.0.0.2", 3002u, "x86_64", true, cid, 2 },
        { J3, "10.0.0.3", 3003u, "x86_64", true, cid, 3 } };
    std::string uy;
    REQUIRE_OR_ABORT(usecs_seq_exact(useq, uwant, &uy),
                     "exact client UseCS sequence via FIFO sentinel (" + uy + ")");

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

    std::string cw;
    DoneResult f_done = forwarded_terminals(client, f.sched, 5000, &cw);
    REQUIRE(f_done.st.clean(), "sentinel-bounded forwarded terminals (client:" + cw + " sched:" + f_done.st.why() + ")");
    std::string why;
    /* forwarded unchanged except client_count (=1, only the batch client is connected) */
    const uint32_t FS = (uint32_t)JobDoneMsg::FROM_SUBMITTER;
    std::vector<ExpectDone> want = {
        { J1, 11, false, 0, 1, 1, 0, 0, 0, 0, 0, 0, FS, 1 },
        { J2, 12, false, 0, 2, 2, 0, 0, 0, 0, 0, 0, FS, 1 },
        { J3, 13, false, 0, 3, 3, 0, 0, 0, 0, 0, 0, FS, 1 } };
    const bool exact = done_frames_exact(f_done.frames, want, &why);
    fprintf(stderr, "         (scheduler saw %zu JobDone; exact=%d %s)\n", f_done.frames.size(), exact, why.c_str());
    REQUIRE(exact, "scheduler observed exactly [J1/11, J2/12, J3/13] over every field -- U + duplicate J1 filtered, no extra/unrelated frame");

    /* trailing real scalar exchange (not merely a UNIX connection) */
    MsgChannel *live = connect_unix_bounded(f.socket_path, 3000);
    REQUIRE_OR_ABORT(live != nullptr, "daemon accepts a new client after the filtered dones");
    live->send_msg(make_getcs("post.cpp", 1));
    Msg *lf = wait_for_type(f.sched, Msg::GET_CS, 4000, true);
    REQUIRE(lf != nullptr, "trailing scalar GetCS reached S");
    uint32_t lc = 0; { GetCSMsg *g = dynamic_cast<GetCSMsg *>(lf); if (g) lc = g->client_id; }
    delete lf;
    f.sched->send_msg(UseCSMsg("x86_64", "10.9.9.9", 5000, 6150, true, lc, 0));
    REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the trailing decision (sentinel)");
    { FrameSeq useq = read_until_status(live, 4000, false); std::string uy;
      /* scalar REMOTE path relays the scheduler frame verbatim (main.cpp send_msg(*msg)),
         preserving the forwarded client_id (lc); only the scalar LOCAL path substitutes 1. */
      const bool uok = usecs_seq_exact(useq, std::vector<ExpectUseCS>{ { 6150u, "10.9.9.9", 5000u, "x86_64", true, lc, 0u } }, &uy);
      REQUIRE(uok, "trailing scalar exchange delivered exactly one decision, field-exact (" + uy + ")"); }
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
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the three decisions (sentinel)");
        { FrameSeq useq = read_until_status(client, 6000, false); std::string uy;
          REQUIRE_OR_ABORT(usecs_seq_exact(useq, std::vector<ExpectUseCS>{
                               { J1, "10.0.0.9", 3632u, "x86_64", true, cid, 0u },
                               { J2, "10.0.0.9", 3632u, "x86_64", true, cid, 0u },
                               { J3, "10.0.0.9", 3632u, "x86_64", true, cid, 0u } }, &uy),
                           "client received all three remote UseCS, field-exact via FIFO sentinel (" + uy + ")"); }

        /* complete J1 -> exactly one forwarded JobDone(J1) */
        { JobDoneMsg d(J1, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::string cw1;
        DoneResult after_j1 = forwarded_terminals(client, f.sched, 2500, &cw1);
        REQUIRE(after_j1.st.clean(), "sentinel-bounded forwarded terminals (client:" + cw1 + " sched:" + after_j1.st.why() + ")");
        REQUIRE(count_id(after_j1.frames, J1) == 1, "completing J1 forwards exactly one JobDone(J1)");

        /* late duplicate decision for the completed J1, visibly different fields */
        f.sched->send_msg(UseCSMsg("x86_64", "10.9.9.9", 3632u, J1, false, cid, 0));
        REQUIRE(no_use_cs(client, 2000), "late UseCS(J1) after completion is not delivered to the client");

        /* complete J2,J3 then drain: no resurrected excess terminal for J1, and
           exactly one completion each for J2,J3 */
        for (uint32_t j : { J2, J3 }) { JobDoneMsg d(j, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::string cwt;
        DoneResult tail = forwarded_terminals(client, f.sched, 4000, &cwt);
        REQUIRE(tail.st.clean(), "sentinel-bounded forwarded terminals (client:" + cwt + " sched:" + tail.st.why() + ")");
        fprintf(stderr, "         (after-completion tail: J1=%d J2=%d J3=%d)\n",
                count_id(tail.frames, J1), count_id(tail.frames, J2), count_id(tail.frames, J3));
        REQUIRE(count_id(tail.frames, J1) == 0, "no second/excess terminal for the completed J1");
        REQUIRE(count_id(tail.frames, J2) == 1, "JobDone(J2) forwarded exactly once");
        REQUIRE(count_id(tail.frames, J3) == 1, "JobDone(J3) forwarded exactly once");
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
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the three decisions (sentinel)");
        { FrameSeq useq = read_until_status(client, 6000, false); std::string uy;
          REQUIRE_OR_ABORT(usecs_seq_exact(useq, std::vector<ExpectUseCS>{
                               { J1, "10.0.0.9", 3632u, "x86_64", true, cid, 0u },
                               { J2, "10.0.0.9", 3632u, "x86_64", true, cid, 0u },
                               { J3, "10.0.0.9", 3632u, "x86_64", true, cid, 0u } }, &uy),
                           "client received all three remote UseCS, field-exact via FIFO sentinel (" + uy + ")"); }

        /* clean End: a real EndMsg, no synthetic JobDone for the remote entries.
           Sentinel-bounded: the client End is proven consumed (its channel settles),
           so ALL terminals it would emit are present -- the zero-terminal claim is
           happens-before, not a fixed-window guess. */
        REQUIRE(client->send_msg(EndMsg()), "client sent a clean EndMsg");
        std::string tw;
        DoneResult after = settle_terminals(client, f.sched, 4000, &tw);
        REQUIRE(after.st.clean(), "sentinel-bounded settlement (client:" + tw + " sched:" + after.st.why() + ")");
        fprintf(stderr, "         (terminals after clean End: J1=%d J2=%d J3=%d total=%zu)\n",
                count_id(after.frames, J1), count_id(after.frames, J2), count_id(after.frames, J3), after.frames.size());
        REQUIRE(after.frames.empty(), "clean End emits zero terminals for delivered remote entries");

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
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500, true); delete m; }  /* drain S-side announce */

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

        /* release the blocker -> exactly L1, then one-at-a-time on each completion.
           A blocker-channel sentinel orders the JobDone (frees the slot + advances
           the lane) before the client FIFO read of the delivered decision. */
        std::string y;
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        REQUIRE(consumed_barrier(blocker), "blocker JobDone consumed + capacity advanced");
        REQUIRE(read_one_local_usecs(client, L1, &y), "after release exactly L1 delivered (" + y + ")");
        REQUIRE(no_use_cs(client, 1200), "L2 withheld until L1 completes");
        REQUIRE(complete_local_entry(client, f.sched, f.work, L1, 0, 2),
                "L1 completed via real lifecycle (CompileFile -> JobBegin -> JobDone)");
        REQUIRE(read_one_local_usecs(client, L2, &y), "L2 delivered after L1 completes (" + y + ")");
        REQUIRE(no_use_cs(client, 1200), "L3 withheld until L2 completes");
        REQUIRE(complete_local_entry(client, f.sched, f.work, L2, 0, 2),
                "L2 completed via real lifecycle");
        REQUIRE(read_one_local_usecs(client, L3, &y), "L3 delivered after L2 completes (" + y + ")");
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
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500, true); delete m; }

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

        /* RED discriminator: later NoCS must NOT be rejected as unmatched (no 107).
           Sched-FIFO sentinel: the 3 NoCS were sent on this channel before our
           GET_INTERNALS, so any 107 the daemon emitted precedes the STATUS_TEXT --
           no client JobDone is involved, so a bare scheduler-sentinel read suffices. */
        DoneResult term = read_sched_terminals(f.sched, 3000);
        REQUIRE(term.st.clean(), "sentinel-bounded scheduler terminals (" + term.st.why() + ")");
        fprintf(stderr, "         (unmatched terminals: N2=%d N3=%d)\n",
                count_id(term.frames, N2), count_id(term.frames, N3));
        REQUIRE(count_id(term.frames, N2) == 0 && count_id(term.frames, N3) == 0,
                "later NoCS accepted by ledger state, not rejected with JobDone(107)");

        /* release -> one NoCS local decision at a time as capacity frees (two-stage
           FIFO; a NoCS decision synthesises an EMPTY platform). */
        std::string y;
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        REQUIRE(consumed_barrier(blocker), "blocker JobDone consumed + capacity advanced");
        REQUIRE(read_one_local_usecs(client, N1, &y, ""), "after release exactly N1 delivered (" + y + ")");
        REQUIRE(complete_local_entry(client, f.sched, f.work, N1, 0, 2), "N1 completed via real lifecycle");
        REQUIRE(read_one_local_usecs(client, N2, &y, ""), "N2 delivered after N1 completes (" + y + ")");
        REQUIRE(complete_local_entry(client, f.sched, f.work, N2, 0, 2), "N2 completed via real lifecycle");
        REQUIRE(read_one_local_usecs(client, N3, &y, ""), "N3 delivered after N2 completes (" + y + ")");
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
            REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the delivered decisions (sentinel)");
            std::vector<ExpectUseCS> uwant;
            for (int i = 0; i < deliver; ++i) uwant.push_back({ J[i], "10.0.0.9", 3632u, "x86_64", true, cid, 0u });
            FrameSeq useq = read_until_status(client, 5000, false); std::string uy;
            REQUIRE_OR_ABORT(usecs_seq_exact(useq, uwant, &uy),
                             "the delivered remote decisions reached the client, field-exact via FIFO sentinel (" + uy + ")");
        }
        /* end the client, then read the settlement terminals via the exact orderly-EOF
           observer (local-oracle 00:12): EndMsg for the clean path, a half-closed write
           for raw EOF; settle_terminals observes the daemon's own close (recv()==0) then
           reads the exact scheduler sequence through its sentinel. */
        if (use_endmsg) {
            REQUIRE_OR_ABORT(client->send_msg(EndMsg()), "client sent a clean EndMsg");
        } else {
            REQUIRE_OR_ABORT(client_send_raw_eof(client), "client half-closed its write side (raw EOF signalled)");
        }
        std::string tw;
        DoneResult term = settle_terminals(client, f.sched, 4000, &tw);
        REQUIRE(term.st.clean(), "sentinel-bounded settlement (client:" + tw + " sched:" + term.st.why() + ")");

        const bool normal = use_endmsg && deliver == 3;   /* all expected delivered, no local */
        std::vector<uint32_t> expect;
        if (!normal) for (int i = 0; i < deliver; ++i) expect.push_back(J[i]);
        const int expect_cancel = (!normal && deliver < 3) ? 1 : 0;

        std::vector<uint32_t> seq = done_order(term.frames);
        fprintf(stderr, "         (end=%s deliver=%d -> terminals %s cancels=%d)\n",
                use_endmsg ? "EndMsg" : "EOF", deliver, ids_to_string(seq).c_str(),
                count_cancellations(term.frames, cid));
        REQUIRE(seq == expect, "exact ordered settlement matches the plan");
        REQUIRE(count_cancellations(term.frames, cid) == expect_cancel, "exact client-id cancellation count");
        /* no terminal for a not-yet-delivered id ever */
        for (int i = deliver; i < 3; ++i)
            REQUIRE(count_id(term.frames, J[i]) == 0, "no terminal for an undelivered decision");

        /* trailing liveness: a fresh scalar client completes a real GetCS->S->
           UseCS->client exchange (a live scheduler forwards, so we must answer). */
        MsgChannel *live = connect_unix_bounded(f.socket_path, 3000);
        REQUIRE(live != nullptr, "daemon accepts a new client after teardown");
        if (live) {
            live->send_msg(make_getcs("post.cpp", 1));
            Msg *lfwd = wait_for_type(f.sched, Msg::GET_CS, 4000, true);
            REQUIRE(lfwd != nullptr, "post-teardown scalar GetCS reached the scheduler");
            uint32_t lc = 0;
            { GetCSMsg *g = dynamic_cast<GetCSMsg *>(lfwd); if (g) lc = g->client_id; }
            delete lfwd;
            f.sched->send_msg(UseCSMsg("x86_64", "10.0.0.9", 3632u, 7500, true, lc, 0));
            REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the trailing decision (sentinel)");
            { FrameSeq useq = read_until_status(live, 4000, false); std::string uy;
              /* scalar REMOTE path relays verbatim, preserving the forwarded client_id (lc). */
              const bool uok = usecs_seq_exact(useq, std::vector<ExpectUseCS>{ { 7500u, "10.0.0.9", 3632u, "x86_64", true, lc, 0u } }, &uy);
              REQUIRE(uok, "post-teardown scalar exchange completed exactly once, field-exact (" + uy + ")"); }
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
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500, true); delete m; }

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
            std::string tw;
            DoneResult term = settle_terminals(client, f.sched, 4000, &tw);
            REQUIRE(term.st.clean(), "sentinel-bounded settlement (client:" + tw + " sched:" + term.st.why() + ")");
            std::vector<uint32_t> seq = done_order(term.frames);
            fprintf(stderr, "         (queued -> terminals %s cancels=%d)\n",
                    ids_to_string(seq).c_str(), count_cancellations(term.frames, cid));
            /* all three recorded local entries are unfinished -> settled once each */
            REQUIRE(seq == (std::vector<uint32_t>{L1, L2, L3}),
                    "queued local entries settled once each by exact id");
            REQUIRE(count_cancellations(term.frames, cid) == 0,
                    "no cancellation when all expected decisions were recorded");
        } else if (scenario == "active") {
            /* release the blocker -> L1 becomes active; end while L1 is active */
            { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
            REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + L1 advanced");
            { std::string y; REQUIRE_OR_ABORT(read_one_local_usecs(client, L1, &y), "L1 delivered active after release (" + y + ")"); }
            REQUIRE(client->send_msg(EndMsg()), "client sent EndMsg with L1 active");
            std::string tw;
            DoneResult term = settle_terminals(client, f.sched, 4000, &tw);
            REQUIRE(term.st.clean(), "sentinel-bounded settlement (client:" + tw + " sched:" + term.st.why() + ")");
            std::vector<uint32_t> seq = done_order(term.frames);
            fprintf(stderr, "         (active -> terminals %s cancels=%d)\n",
                    ids_to_string(seq).c_str(), count_cancellations(term.frames, cid));
            REQUIRE(seq == (std::vector<uint32_t>{L1, L2, L3}),
                    "active + queued local entries settled once each");
            REQUIRE(count_cancellations(term.frames, cid) == 0, "no cancellation (all recorded)");
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
            REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + L1 advanced");
            uint32_t exp[3] = { L1, L2, L3 };
            for (int i = 0; i < 3; ++i) {
                std::string y;
                REQUIRE_OR_ABORT(read_one_local_usecs(client, exp[i], &y),
                                 "next local decision delivered in order via FIFO sentinel (" + y + ")");
                /* complete_local_entry collects + compares THIS entry's forwarded
                   JobDone exactly and (i<2) advances the successor. */
                REQUIRE(complete_local_entry(client, f.sched, f.work, exp[i], 0, 2),
                        "local entry completed + forwarded exactly via real lifecycle");
            }
            REQUIRE(client->send_msg(EndMsg()), "client sent a normal EndMsg after all local done");
            std::string tw;
            DoneResult term = settle_terminals(client, f.sched, 3000, &tw);
            REQUIRE(term.st.clean(), "sentinel-bounded settlement (client:" + tw + " sched:" + term.st.why() + ")");
            fprintf(stderr, "         (normal -> extra terminals %s cancels=%d)\n",
                    ids_to_string(done_order(term.frames)).c_str(), count_cancellations(term.frames, cid));
            REQUIRE(done_order(term.frames).empty(), "normal End emits no extra terminal");
            REQUIRE(count_cancellations(term.frames, cid) == 0, "normal End emits no cancellation");
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
        REQUIRE(expect_no_frame(f.sched, 1500, &s2, true), "count=0: no GetCS forwarded to S");
        /* same client count=1 works: one forwarded GetCS + one relayed UseCS */
        REQUIRE(client->send_msg(make_getcs("c0-then-1.cpp", 1)), "same client sent GetCS(count=1)");
        Msg *fwd = wait_for_type(f.sched, Msg::GET_CS, 4000, true);
        REQUIRE_OR_ABORT(fwd != nullptr, "count=1 forwarded to S after count=0");
        uint32_t cid = 0; { GetCSMsg *g = dynamic_cast<GetCSMsg *>(fwd); if (g) cid = g->client_id; }
        delete fwd;
        f.sched->send_msg(UseCSMsg("x86_64", "10.0.0.9", 3632u, 8000, true, cid, 0));
        /* two-stage FIFO: scheduler sentinel -> exact client UseCS sequence */
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the decision (sentinel)");
        FrameSeq useq = read_until_status(client, 4000, false);
        std::string uy;
        REQUIRE(usecs_seq_exact(useq, std::vector<ExpectUseCS>{ { 8000u, "10.0.0.9", 3632u, "x86_64", true, cid, 0u } }, &uy),
                "count=1 after count=0: exactly one field-exact UseCS via FIFO sentinel (" + uy + ")");
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
        Msg *fwd = wait_for_type(f.sched, Msg::GET_CS, 4000, true);
        REQUIRE_OR_ABORT(fwd != nullptr, "count=1 forwarded to S");
        uint32_t cid = 0; unsigned int fc = 1; { GetCSMsg *g = dynamic_cast<GetCSMsg *>(fwd); if (g) { cid = g->client_id; fc = g->count; } }
        delete fwd;
        REQUIRE(fc == 1, "forwarded count is exactly 1 (scalar path)");
        if (local) {
            f.sched->send_msg(UseCSMsg("x86_64", "127.0.0.1", 0, 8100, true, cid, 0));
        } else {
            f.sched->send_msg(UseCSMsg("x86_64", "10.7.7.7", 4444u, 8100, false, cid, 9));
        }
        /* two-stage FIFO: scheduler sentinel -> exactly one field-exact client UseCS
           (the sentinel guarantees no buffered second, so no separate no-second check).
           A remote relay preserves the original tuple; a local decision is a synthetic
           127.0.0.1:daemon_port UseCS (empty platform, client_id 1, got_env true). */
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the decision (sentinel)");
        FrameSeq useq = read_until_status(client, 4000, false);
        ExpectUseCS w = local
            ? ExpectUseCS{ 8100u, "127.0.0.1", 0u, "x86_64", true, cid, 0u }
            : ExpectUseCS{ 8100u, "10.7.7.7", 4444u, "x86_64", false, cid, 9u };
        std::string uy;
        const bool uok = usecs_seq_exact(useq, std::vector<ExpectUseCS>{ w }, &uy);
        if (!uok) fprintf(stderr, "         (scalar useq: %s)\n", uy.c_str());
        REQUIRE(uok, "exactly one field-exact UseCS via FIFO sentinel");
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
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500, true); delete m; }

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
        /* while the blocker holds: two-stage FIFO proves EXACTLY the remote decisions
           reach the client (in spec order) and every queued local/NoCS is withheld --
           the exact sequence subsumes the per-id delivered/leaked checks. */
        std::vector<uint32_t> want_queued;
        std::vector<ExpectUseCS> uwant_remote;
        for (int i = 0; i < 3; ++i) {
            if (spec[i] == 'R') uwant_remote.push_back({ ID[i], "10.5.5.5", 5555u, "x86_64", true, cid, 7u });
            else want_queued.push_back(ID[i]);
        }
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the decisions (sentinel)");
        FrameSeq dur = read_until_status(client, 4000, false);
        std::string uy;
        REQUIRE(usecs_seq_exact(dur, uwant_remote, &uy),
                "only the remote decisions delivered while blocked, field-exact, no queued leak (" + uy + ")");
        /* release -> the queued local/NoCS decisions deliver one at a time; each is
           completed through its REAL lifecycle and its forwarded terminal is proven
           exactly (a bare UseCS -> JobDone would be rejected and must not pass). */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + queued advanced");
        for (int i = 0; i < 3; ++i) {
            if (spec[i] == 'R') continue;
            const char *plat = (spec[i] == 'L') ? "x86_64" : "";   /* NoCS -> empty platform */
            std::string y;
            REQUIRE(read_one_local_usecs(client, ID[i], &y, plat),
                    "queued decision delivered in order via FIFO sentinel (" + y + ")");
            REQUIRE(complete_local_entry(client, f.sched, f.work, ID[i], 0, 2),
                    "queued local/NoCS entry completed + forwarded exactly via real lifecycle");
        }
        /* the remote decisions retained their field identity; complete each via the
           two-stage FIFO -- client sentinel proves the JobDone consumed, then the
           EXACT forwarded terminal is the scheduler lifecycle sequence. */
        for (const ExpectUseCS &re : uwant_remote) {
            const uint32_t rid = re.job_id;
            JobDoneMsg d(rid, 33, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1;
            REQUIRE(client->send_msg(d), "remote JobDone sent");
            { FrameSeq cs = read_until_status(client, 3000, false);
              REQUIRE_OR_ABORT(cs.clean() && cs.frames.empty(), "remote JobDone consumed; nothing delivered back"); }
            FrameSeq ds = read_until_status(f.sched, 3000, true);
            ExpectDone w{ rid, 33, false, 0, 1, 1, 0, 0, 0, 0, 0, 0, (uint32_t)JobDoneMsg::FROM_SUBMITTER, 2 };
            std::string why;
            REQUIRE(done_seq_exact(ds, std::vector<ExpectDone>{ w }, &why),
                    "remote completion forwarded exactly over every field (" + why + ")");
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
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500, true); delete m; }
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
        /* sched-FIFO sentinel: N4 was sent on this channel before our GET_INTERNALS,
           so its excess 107 (if any) precedes the STATUS_TEXT -- no client involved. */
        DoneResult term = read_sched_terminals(f.sched, 3000);
        REQUIRE(term.st.clean(), "sentinel-bounded scheduler terminals (" + term.st.why() + ")");
        fprintf(stderr, "         (N1=%d N4=%d)\n", count_id(term.frames, N1), count_id(term.frames, N4));
        REQUIRE(count_id(term.frames, N4) == 1, "4th distinct NoCS terminalized exactly once as excess");
        for (const DoneFrame &d : term.frames) if (d.job_id == N4) REQUIRE(d.exitcode == 107, "excess terminal carries exit 107");
        REQUIRE(count_id(term.frames, N1) == 0, "repeated NoCS(N1) ignored -- no excess terminal");
        /* release: exactly N1,N2,N3 deliver one at a time; N4 never delivered */
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + N1 advanced");
        uint32_t acc[3] = { N1, N2, N3 };
        for (int i = 0; i < 3; ++i) {
            std::string y;
            REQUIRE_OR_ABORT(read_one_local_usecs(client, acc[i], &y, ""),
                             "accepted NoCS decision delivered in order via FIFO sentinel (" + y + ")");
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
    { Msg *m = wait_for_type(f.sched, Msg::JOB_LOCAL_BEGIN, 1500, true); delete m; }
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
        REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + L1 advanced");
        { std::string y; REQUIRE_OR_ABORT(read_one_local_usecs(client, L1, &y, "x86_64"), "L1 local decision delivered (" + y + ")"); }
        CompileJob job;
        job.setLanguage(CompileJob::Lang_CXX); job.setCompilerName("g++");
        job.setJobID(L1); job.setEnvironmentVersion("__client"); job.setTargetPlatform("x86_64");
        job.setInputFile(f.work + "/a.cpp"); job.setOutputFile(f.work + "/a.o"); job.setWorkingDirectory(f.work);
        CompileFileMsg compile(&job);
        REQUIRE(client->send_msg(compile), "client sent production CompileFile(__client) for L1");
        Msg *jb = wait_for_type(f.sched, Msg::JOB_BEGIN, 4000, true);
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
        REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + L1 advanced");
        { std::string y; REQUIRE_OR_ABORT(read_one_local_usecs(client, ids[0], &y, "x86_64"), "L1 delivered active (" + y + ")"); }
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

        /* Exact EOF settlement (local-oracle 00:20): the client's bare JobDone(L1)
           carried no CompileFile->JobBegin, so L1 was NOT started -> rejected, and both
           L1 and the delivery-failed L2 are unfinished LOCAL entries settled by
           handle_end with the raw-EOF exit 118 (handle_activity get_msg==null path);
           one client-id cancellation covers the undelivered 3rd decision (2 of 3
           recorded).  The cancellation is the KNOWN final settlement frame, so reading
           the EXACT ordered sequence over every serialized field proves the old
           client's EOF handler ran; a single scheduler sentinel then requires an exact
           zero-frame tail.  (Fields captured from the product; deterministic across
           runs: settlements flags=FROM_SUBMITTER cc=1, cancellation job_id=unk=cid
           flags=FROM_SUBMITTER|unknown-client, exit 118 throughout.) */
        {
            const uint32_t FS = (uint32_t)JobDoneMsg::FROM_SUBMITTER;
            std::vector<ExpectDone> want = {
                { ids[0], 118, false, 0,   0, 0, 0, 0, 0, 0, 0, 0, FS,     1 },  /* L1 settled */
                { ids[1], 118, false, 0,   0, 0, 0, 0, 0, 0, 0, 0, FS,     1 },  /* L2 settled */
                { cid,    118, false, cid, 0, 0, 0, 0, 0, 0, 0, 0, FS | 2, 1 },  /* undelivered-tail cancellation */
            };
            std::string dw;
            REQUIRE(exact_terminals_then_quiet(f.sched, want, 6000, &dw),
                    "exact EOF settlement [L1,L2,cancel] over every field + zero tail (" + dw + ")");
        }

        /* capacity restored + real trailing exchange: a fresh scalar-local client
           gets a decision (proves the daemon is live and the slot is free). */
        MsgChannel *live = connect_unix_bounded(f.socket_path, 3000);
        REQUIRE_OR_ABORT(live != nullptr, "daemon survived the failed next-local delivery");
        live->send_msg(make_getcs("post.cpp", 1));
        Msg *lf = wait_for_type(f.sched, Msg::GET_CS, 4000, true);
        REQUIRE(lf != nullptr, "trailing scalar GetCS reached S");
        uint32_t lc = 0; { GetCSMsg *g = dynamic_cast<GetCSMsg *>(lf); if (g) lc = g->client_id; }
        delete lf;
        f.sched->send_msg(UseCSMsg("x86_64", "127.0.0.1", 0, 8650, true, lc, 0));
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the trailing decision (sentinel)");
        { FrameSeq useq = read_until_status(live, 4000, false); std::string uy;
          /* scalar-local relay: client_id rewritten to 1, port to daemon_port(0) (see scheduler_use_cs). */
          const bool uok = usecs_seq_exact(useq, std::vector<ExpectUseCS>{ { 8650u, "127.0.0.1", 0u, "x86_64", true, 1u, 0u } }, &uy);
          REQUIRE(uok, "trailing scalar-local exchange completed, field-exact (" + uy + ")"); }
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
        REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + L1 advanced");
        { std::string y; REQUIRE_OR_ABORT(read_one_local_usecs(client, ids[0], &y, "x86_64"), "L1 active, L2 queued (" + y + ")"); }
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
        std::vector<ExpectUseCS> uwant;
        for (int i = 0; i < N; ++i) {
            char host[32]; snprintf(host, sizeof host, "10.1.%d.%d", N & 255, (i + 1) & 255);
            f.sched->send_msg(UseCSMsg("x86_64", host, 4000u + i, base + i, true, cid, 10 + i));
            uwant.push_back({ base + (uint32_t)i, host, 4000u + (uint32_t)i, "x86_64", true, cid, (uint32_t)(10 + i) });
        }
        /* two-stage FIFO: scheduler sentinel -> exact client UseCS sequence */
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the N decisions (sentinel)");
        FrameSeq useq = read_until_status(client, 6000, false);
        std::string uy;
        REQUIRE_OR_ABORT(usecs_seq_exact(useq, uwant, &uy), "exact client UseCS sequence via FIFO sentinel (" + uy + ")");
        /* complete: distinct exit + distinct real/user per entry.  client sentinel
           proves the JobDones were consumed (client receives nothing back), then the
           EXACT scheduler lifecycle sequence is the forwarded terminals in order. */
        for (int i = 0; i < N; ++i) { JobDoneMsg d(base + i, 30 + i, JobDoneMsg::FROM_SUBMITTER); d.real_msec = i + 1; d.user_msec = i + 1; client->send_msg(d); }
        { FrameSeq cs = read_until_status(client, 3000, false);
          REQUIRE_OR_ABORT(cs.clean() && cs.frames.empty(), "client inputs consumed; nothing delivered back to the client"); }
        FrameSeq dseq = read_until_status(f.sched, 4000, true);
        std::vector<ExpectDone> want;
        for (int i = 0; i < N; ++i)
            want.push_back({ base + (uint32_t)i, 30 + i, false, 0,
                             (uint32_t)(i + 1), (uint32_t)(i + 1), 0, 0, 0, 0, 0, 0,
                             (uint32_t)JobDoneMsg::FROM_SUBMITTER, 1 });
        std::string why;
        REQUIRE(done_seq_exact(dseq, want, &why), "exact ordered terminals over every field (" + why + ")");
        /* NORMAL End (all delivered+completed) settles nothing: no terminal follows */
        REQUIRE(client->send_msg(EndMsg()), "client sent a normal EndMsg");
        FrameSeq afterseq = read_until_status(f.sched, 2500, true);
        REQUIRE(afterseq.clean() && afterseq.frames.empty(), "normal End emits no extra terminal or cancellation");
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
        REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + L1 advanced");
        { std::string y; REQUIRE_OR_ABORT(read_one_local_usecs(client, ids[0], &y, "x86_64"), "L1 delivered and active; L2 queued (" + y + ")"); }

        /* premature JobDone for the still-queued L2: rejected, never forwarded.
           Sentinel-bounded: the client sentinel proves the premature JobDone was
           consumed (read + rejected), so a clean/empty scheduler read is definitive. */
        { JobDoneMsg d(ids[1], 55, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::string pw;
        DoneResult premature = forwarded_terminals(client, f.sched, 1500, &pw);
        REQUIRE(premature.st.clean(), "sentinel-bounded forwarded terminals (client:" + pw + " sched:" + premature.st.why() + ")");
        REQUIRE(count_id(premature.frames, ids[1]) == 0, "premature JobDone for the queued L2 is NOT forwarded to S");

        /* complete L1 for real -> forwarded once; the lane frees and L2 (still
           recorded, NOT completed by the premature frame) auto-binds. */
        CompileJob j1; j1.setLanguage(CompileJob::Lang_CXX); j1.setCompilerName("g++");
        j1.setJobID(ids[0]); j1.setEnvironmentVersion("__client"); j1.setTargetPlatform("x86_64");
        j1.setInputFile(f.work + "/a.cpp"); j1.setOutputFile(f.work + "/a.o"); j1.setWorkingDirectory(f.work);
        REQUIRE(client->send_msg(CompileFileMsg(&j1)), "L1 CompileFile(__client)");
        { Msg *jb = wait_for_type(f.sched, Msg::JOB_BEGIN, 4000, true); REQUIRE(jb != nullptr, "L1 JobBegin via ordinary local path"); delete jb; }
        { JobDoneMsg d(ids[0], 11, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::string t1w;
        DoneResult t1 = forwarded_terminals(client, f.sched, 3000, &t1w);
        REQUIRE(t1.st.clean(), "sentinel-bounded forwarded terminals (client:" + t1w + " sched:" + t1.st.why() + ")");
        REQUIRE(count_id(t1.frames, ids[0]) == 1, "L1 completion forwarded exactly once");

        /* L2 must now deliver -- proof the premature JobDone did not complete it.
           Trigger (L1 completion) was already forwarded + client-sentinel-consumed via
           forwarded_terminals above; the next client sentinel drains the L2 UseCS
           emitted by that same handler. */
        { std::string y; REQUIRE_OR_ABORT(read_one_local_usecs(client, ids[1], &y, "x86_64"),
                         "queued L2 delivered after L1, premature JobDone did not complete it (" + y + ")"); }
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
        REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + first entry advanced");
        for (int i = 0; i < 2; ++i) {
            /* deliver entry i -- iter 0 triggered by the blocker release above, iter 1
               by iter 0's completion (forwarded+consumed at the tail of the loop). */
            { std::string y; REQUIRE_OR_ABORT(read_one_local_usecs(client, ids[i], &y, "x86_64"),
                             "entry delivered in order (" + y + ")"); }
            /* real production CompileFile(__client) for THIS entry's distinct id */
            CompileJob job; job.setLanguage(CompileJob::Lang_CXX); job.setCompilerName("g++");
            job.setJobID(ids[i]); job.setEnvironmentVersion("__client"); job.setTargetPlatform("x86_64");
            char in[64], out[64]; snprintf(in, sizeof in, "%s/e%d.cpp", f.work.c_str(), i); snprintf(out, sizeof out, "%s/e%d.o", f.work.c_str(), i);
            job.setInputFile(in); job.setOutputFile(out); job.setWorkingDirectory(f.work);
            REQUIRE(client->send_msg(CompileFileMsg(&job)), "entry CompileFile(__client) sent");
            /* JobBegin must carry THIS entry's id -- never the prior (stale) job */
            Msg *jb = wait_for_type(f.sched, Msg::JOB_BEGIN, 4000, true);
            REQUIRE_OR_ABORT(jb != nullptr, "entry emitted JobBegin");
            { JobBeginMsg *b = dynamic_cast<JobBeginMsg *>(jb); REQUIRE(b && b->job_id == ids[i], "JobBegin carries THIS entry's distinct job id (no stale carry-over)"); }
            delete jb;
            /* complete this entry -> forwarded once; releases the compile job and
               binds the successor (i==0) */
            { JobDoneMsg d(ids[i], 20 + i, JobDoneMsg::FROM_SUBMITTER); d.real_msec = i + 1; d.user_msec = i + 1; client->send_msg(d); }
            std::string tw;
            DoneResult t = forwarded_terminals(client, f.sched, 3000, &tw);
            REQUIRE(t.st.clean(), "sentinel-bounded forwarded terminals (client:" + tw + " sched:" + t.st.why() + ")");
            REQUIRE(count_id(t.frames, ids[i]) == 1, "entry completion forwarded exactly once");
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
        REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + L1 advanced");
        { std::string y; REQUIRE_OR_ABORT(read_one_local_usecs(client, L1, &y, "x86_64"), "L1 delivered and slot-charged (" + y + ")"); }

        /* (a) CompileFile with an UNMATCHED id: rejected -> no JobBegin, no adopt. */
        {
            CompileJob bad; bad.setLanguage(CompileJob::Lang_CXX); bad.setCompilerName("g++");
            bad.setJobID(9999); bad.setEnvironmentVersion("__client"); bad.setTargetPlatform("x86_64");
            bad.setInputFile(f.work + "/x.cpp"); bad.setOutputFile(f.work + "/x.o"); bad.setWorkingDirectory(f.work);
            REQUIRE(client->send_msg(CompileFileMsg(&bad)), "client sent CompileFile with an unmatched id");
            Msg *jb = wait_for_type(f.sched, Msg::JOB_BEGIN, 1500, true);
            REQUIRE(jb == nullptr, "unmatched CompileFile emits NO JobBegin (id not adopted)");
            delete jb;
        }

        /* (b) JobDone for the delivered-but-not-started L1: rejected, not forwarded. */
        { JobDoneMsg d(L1, 44, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::string ew;
        DoneResult early = forwarded_terminals(client, f.sched, 1500, &ew);
        REQUIRE(early.st.clean(), "sentinel-bounded forwarded terminals (client:" + ew + " sched:" + early.st.why() + ")");
        REQUIRE(count_id(early.frames, L1) == 0, "JobDone for a delivered-but-not-started entry is NOT forwarded");

        /* real CompileFile(L1) -> JobBegin(L1) -> LOCAL_COMPILE_STARTED */
        CompileJob job; job.setLanguage(CompileJob::Lang_CXX); job.setCompilerName("g++");
        job.setJobID(L1); job.setEnvironmentVersion("__client"); job.setTargetPlatform("x86_64");
        job.setInputFile(f.work + "/a.cpp"); job.setOutputFile(f.work + "/a.o"); job.setWorkingDirectory(f.work);
        REQUIRE(client->send_msg(CompileFileMsg(&job)), "client sent the real CompileFile(L1)");
        { Msg *jb = wait_for_type(f.sched, Msg::JOB_BEGIN, 4000, true);
          REQUIRE_OR_ABORT(jb != nullptr, "real CompileFile(L1) emitted JobBegin");
          JobBeginMsg *b = dynamic_cast<JobBeginMsg *>(jb); REQUIRE(b && b->job_id == L1, "JobBegin carries L1"); delete jb; }

        /* JobDone(L1) now accepted (STARTED) -> forwarded exactly once */
        { JobDoneMsg d(L1, 11, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; client->send_msg(d); }
        std::string tw;
        DoneResult t = forwarded_terminals(client, f.sched, 3000, &tw);
        REQUIRE(t.st.clean(), "sentinel-bounded forwarded terminals (client:" + tw + " sched:" + t.st.why() + ")");
        REQUIRE(count_id(t.frames, L1) == 1, "JobDone for the STARTED entry forwarded exactly once");
    }
done:
    delete blocker; delete client; teardown_farm(f, &clean);
    REQUIRE(clean, "iceccd exited cleanly");
    return failures;
}

/* Group 5 -- BOUNDED-CLEANUP proof, NOT an ordering witness (local-oracle 20:28 /
   22:28).  Force the scheduler send to FAIL at JobBegin for a delivered local entry:
   freeze the daemon, close the scheduler AHEAD of the CompileFile, resume; the
   send_scheduler(JobBegin) fails, the client is torn down, and the charged slot is
   restored -- proven by the daemon reconnecting and a fresh blocker getting the slot.
   This asserts the OBSERVABLE behaviour (no leak, no JobBegin to a live scheduler,
   capacity restored), which is identical on 1b75124 and a34e825.
   The a34e825 change itself -- committing JobBegin BEFORE marking LOCAL_COMPILE_
   STARTED -- is a SOURCE-INSPECTED internal invariant with NO external behavioral
   witness: both orders `return false` and tear the client down, and the entry's
   STARTED-vs-DELIVERED state does not change teardown settlement (verified: GREEN on
   1b75124/a34e825/HEAD).  It is AWAITING FORMAL INTEGRATION (local-oracle 22:52): the
   current model trace does NOT observe it -- there is no formal/ tree on this branch,
   and bigoracle's LifecycleAuthorityStartedWitness observes the scheduler assignment
   Waiting->Started from a worker Begin, not this daemon-side send-before-state
   sequence.  It is never claimed as a RED/GREEN row here, and no existing model trace
   is claimed to observe it. */
static int case_jobbegin_send_fail(const char *iceccd)
{
    Farm f; MsgChannel *blocker = nullptr, *client = nullptr; bool clean = false;
    const uint32_t L1 = 8950;
    const uint32_t cid = audit_setup(iceccd, f, blocker, client, 1, &L1);
    REQUIRE_OR_ABORT(cid != 0, "audit farm + one local decision set up");
    {
        { JobDoneMsg d(0, 0, JobDoneMsg::FROM_SUBMITTER); d.real_msec = 1; d.user_msec = 1; blocker->send_msg(d); }
        REQUIRE_OR_ABORT(consumed_barrier(blocker), "blocker JobDone consumed + L1 advanced");
        { std::string y; REQUIRE_OR_ABORT(read_one_local_usecs(client, L1, &y, "x86_64"),
                         "L1 delivered + slot charged, LOCAL_DELIVERED_SLOT_CHARGED (" + y + ")"); }
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
        /* no JobBegin for L1 reaches the NEW scheduler (the failed send was to the old
           one).  Sentinel-bounded on the freshly re-activated channel (sched_barrier
           above proved it live): a stray JobBegin lands in .st.unexpected, a stray
           terminal in .frames -- both fail the checks.  read_sched_terminals' own
           STATUS_TEXT sentinel replaces the fixed drain window. */
        {
            DoneResult jb = read_sched_terminals(f.sched, 1200);
            REQUIRE(jb.st.clean(), "sentinel-bounded scheduler terminals (" + jb.st.why() + ")");
            REQUIRE(jb.frames.empty(), "no terminal/JobBegin for L1 on the reconnected scheduler");
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
      REQUIRE(!r.st.clean() && r.st.term == Term::EOF_CLOSED, "collect_job_done fails closed on eof (EOF-in-completion-window)");
      delete p.a; }
    /* 5. buffered-extra-UseCS: a second buffered UseCS is detected, not ignored. */
    { ChannelPair p = make_channel_pair();
      REQUIRE_OR_ABORT(p.a && p.b, "self-control pair 5 established");
      p.b->send_msg(UseCSMsg("x86_64", "10.0.0.1", 1u, 100, true, 1, 0));
      p.b->send_msg(UseCSMsg("x86_64", "10.0.0.2", 2u, 101, true, 1, 0));
      p.b->flush_pending(); usleep(50 * 1000);
      UseCSResult one = capture_use_cs(p.a, 1, 1500);
      REQUIRE(one.st.clean(), "clean client stream (no eof/read-error/unexpected)");
      REQUIRE(one.st.clean() && one.frames.size() == 1 && one.frames[0].job_id == 100, "captured exactly the first UseCS");
      REQUIRE(!no_use_cs(p.a, 800), "a buffered extra UseCS is detected, not silently ignored (buffered-extra-UseCS)");
      delete p.a; delete p.b; }
done:
    return failures;
}

/* observe_client_eof self-controls (local-oracle 00:12): the exact orderly-EOF observer
   must distinguish all four outcomes.  Driven over a socketpair where p.b is the
   "daemon" side we control and p.a is the terminating client we observe. */
static int case_eof_controls(const char *iceccd)
{
    (void)iceccd;
    /* EOF_OK: the peer closes orderly (FIN) with nothing sent -> clean recv()==0. */
    { ChannelPair p = make_channel_pair();
      REQUIRE_OR_ABORT(p.a && p.b, "eof-control pair 1 established");
      delete p.b;   /* daemon closes -> FIN to p.a */
      REQUIRE(observe_client_eof(p.a, 1500) == EndObs::EOF_OK, "EOF_OK on an orderly peer close");
      delete p.a; }
    /* UNEXPECTED_DATA: a terminating client must receive nothing; a byte/frame fails. */
    { ChannelPair p = make_channel_pair();
      REQUIRE_OR_ABORT(p.a && p.b, "eof-control pair 2 established");
      p.b->send_msg(JobDoneMsg(1, 0, JobDoneMsg::FROM_SUBMITTER)); p.b->flush_pending();
      REQUIRE(observe_client_eof(p.a, 1500) == EndObs::UNEXPECTED_DATA, "UNEXPECTED_DATA when the peer sends a frame");
      delete p.a; delete p.b; }
    /* TIMEOUT: the peer stays open and silent -> no close within the window. */
    { ChannelPair p = make_channel_pair();
      REQUIRE_OR_ABORT(p.a && p.b, "eof-control pair 3 established");
      REQUIRE(observe_client_eof(p.a, 300) == EndObs::TIMEOUT, "TIMEOUT when the peer neither closes nor sends");
      delete p.a; delete p.b; }
    /* POLL_ERROR: a null channel is the defensive error path (no valid fd to poll). */
    REQUIRE(observe_client_eof(nullptr, 300) == EndObs::POLL_ERROR, "POLL_ERROR on a null channel");
done:
    return failures;
}

/* FIFO-sentinel reader self-controls (local-oracle 5258904367): read_until_status /
   usecs_seq_exact / done_seq_exact must detect missing, extra, reordered, and
   unrelated frames and fail closed on eof -- driven over a socketpair so the exact
   pre-sentinel sequence is controlled. */
static int case_fifo_controls(const char *iceccd)
{
    (void)iceccd;
    const ExpectUseCS A{ 100, "10.0.0.1", 1u, "x86_64", true, 7, 3 };
    const ExpectUseCS B{ 101, "10.0.0.2", 2u, "x86_64", true, 7, 4 };
    auto mk = []() { return make_channel_pair(); };
    /* clean: exactly [A,B] then STATUS_TEXT -> accepted */
    { ChannelPair p = mk(); REQUIRE_OR_ABORT(p.a && p.b, "fifo pair clean");
      p.b->send_msg(UseCSMsg("x86_64","10.0.0.1",1u,100,true,7,3));
      p.b->send_msg(UseCSMsg("x86_64","10.0.0.2",2u,101,true,7,4));
      p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, false); std::string y;
      REQUIRE(usecs_seq_exact(r, std::vector<ExpectUseCS>{A,B}, &y), "clean [A,B] accepted (" + y + ")");
      delete p.a; delete p.b; }
    /* missing: only [A] then STATUS_TEXT, expected [A,B] -> count mismatch */
    { ChannelPair p = mk(); REQUIRE_OR_ABORT(p.a && p.b, "fifo pair missing");
      p.b->send_msg(UseCSMsg("x86_64","10.0.0.1",1u,100,true,7,3));
      p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, false); std::string y;
      REQUIRE(!usecs_seq_exact(r, std::vector<ExpectUseCS>{A,B}, &y), "a MISSING frame is rejected");
      delete p.a; delete p.b; }
    /* extra: [A,B,B] then STATUS_TEXT, expected [A,B] -> count mismatch */
    { ChannelPair p = mk(); REQUIRE_OR_ABORT(p.a && p.b, "fifo pair extra");
      p.b->send_msg(UseCSMsg("x86_64","10.0.0.1",1u,100,true,7,3));
      p.b->send_msg(UseCSMsg("x86_64","10.0.0.2",2u,101,true,7,4));
      p.b->send_msg(UseCSMsg("x86_64","10.0.0.2",2u,101,true,7,4));
      p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, false); std::string y;
      REQUIRE(!usecs_seq_exact(r, std::vector<ExpectUseCS>{A,B}, &y), "an EXTRA frame is rejected");
      delete p.a; delete p.b; }
    /* reordered: [B,A] then STATUS_TEXT, expected [A,B] -> field mismatch at pos 0 */
    { ChannelPair p = mk(); REQUIRE_OR_ABORT(p.a && p.b, "fifo pair reorder");
      p.b->send_msg(UseCSMsg("x86_64","10.0.0.2",2u,101,true,7,4));
      p.b->send_msg(UseCSMsg("x86_64","10.0.0.1",1u,100,true,7,3));
      p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, false); std::string y;
      REQUIRE(!usecs_seq_exact(r, std::vector<ExpectUseCS>{A,B}, &y), "a REORDERED sequence is rejected");
      delete p.a; delete p.b; }
    /* unrelated: [A, JobBegin] then STATUS_TEXT -> non-USE_CS frame rejected */
    { ChannelPair p = mk(); REQUIRE_OR_ABORT(p.a && p.b, "fifo pair unrelated");
      p.b->send_msg(UseCSMsg("x86_64","10.0.0.1",1u,100,true,7,3));
      p.b->send_msg(JobBeginMsg(999, 1));
      p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, false); std::string y;
      REQUIRE(!usecs_seq_exact(r, std::vector<ExpectUseCS>{A}, &y), "an UNRELATED frame is rejected");
      delete p.a; delete p.b; }
    /* eof: [A] then peer closes with NO STATUS_TEXT -> not clean (no-status/eof) */
    { ChannelPair p = mk(); REQUIRE_OR_ABORT(p.a && p.b, "fifo pair eof");
      p.b->send_msg(UseCSMsg("x86_64","10.0.0.1",1u,100,true,7,3)); p.b->flush_pending();
      usleep(30 * 1000); delete p.b;
      FrameSeq r = read_until_status(p.a, 1500, false);
      REQUIRE(!r.clean(), "a truncated stream with no STATUS_TEXT is rejected (fail closed)");
      delete p.a; }
done:
    return failures;
}

/* JobDone-side controls (local-oracle 5258904367 #5): done_seq_exact must reject a
   reordered or extra JobDone, and done_frames_exact must be field-at-a-time load-
   bearing.  Driven over a socketpair so the exact sequence + fields are controlled. */
static int case_done_controls(const char *iceccd)
{
    (void)iceccd;
    const uint32_t FS = (uint32_t)JobDoneMsg::FROM_SUBMITTER;
    const ExpectDone A{ 100, 11, false, 0, 1, 1, 0, 0, 0, 0, 0, 0, FS, 2 };
    const ExpectDone B{ 101, 12, false, 0, 2, 2, 0, 0, 0, 0, 0, 0, FS, 2 };
    auto snd = [FS](MsgChannel *c, const ExpectDone &e) {
        JobDoneMsg d((int)e.job_id, e.exitcode, FS);
        d.real_msec = e.real_msec; d.user_msec = e.user_msec; d.client_count = e.client_count;
        c->send_msg(d);
    };
    /* clean [A,B] accepted */
    { ChannelPair p = make_channel_pair(); REQUIRE_OR_ABORT(p.a && p.b, "done pair clean");
      snd(p.b, A); snd(p.b, B); p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, false); std::string y;
      REQUIRE(done_seq_exact(r, std::vector<ExpectDone>{A,B}, &y), "clean JobDone [A,B] accepted (" + y + ")");
      delete p.a; delete p.b; }
    /* reordered [B,A] rejected */
    { ChannelPair p = make_channel_pair(); REQUIRE_OR_ABORT(p.a && p.b, "done pair reorder");
      snd(p.b, B); snd(p.b, A); p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, false); std::string y;
      REQUIRE(!done_seq_exact(r, std::vector<ExpectDone>{A,B}, &y), "a REORDERED JobDone sequence is rejected");
      delete p.a; delete p.b; }
    /* extra [A,B,B] rejected */
    { ChannelPair p = make_channel_pair(); REQUIRE_OR_ABORT(p.a && p.b, "done pair extra");
      snd(p.b, A); snd(p.b, B); snd(p.b, B); p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, false); std::string y;
      REQUIRE(!done_seq_exact(r, std::vector<ExpectDone>{A,B}, &y), "an EXTRA JobDone is rejected");
      delete p.a; delete p.b; }
    /* field-at-a-time: [A] matches; mutating any expected field makes it fail */
    { ChannelPair p = make_channel_pair(); REQUIRE_OR_ABORT(p.a && p.b, "done pair fields");
      snd(p.b, A); p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, false); std::string y;
      REQUIRE_OR_ABORT(done_seq_exact(r, std::vector<ExpectDone>{A}, &y), "base JobDone matches");
      REQUIRE_OR_ABORT(r.frames.size() == 1, "one JobDone captured");
      JobDoneMsg *d = dynamic_cast<JobDoneMsg *>(r.frames[0]);
      DoneFrame g{ d->job_id, d->exitcode, d->is_from_server(), d->flags, d->unknown_job_client_id(),
                   d->real_msec, d->user_msec, d->sys_msec, d->pfaults, d->in_compressed, d->in_uncompressed,
                   d->out_compressed, d->out_uncompressed, d->client_count };
      auto fails = [&](ExpectDone w, const char *f) { std::string yy;
          REQUIRE(!done_frames_exact(std::vector<DoneFrame>{g}, std::vector<ExpectDone>{w}, &yy),
                  std::string("DoneFrame ") + f + " is load-bearing"); };
      { ExpectDone w=A; w.job_id ^= 1u;       fails(w,"job_id"); }
      { ExpectDone w=A; w.exitcode ^= 1;      fails(w,"exitcode"); }
      { ExpectDone w=A; w.from_server = !w.from_server; fails(w,"from_server"); }
      { ExpectDone w=A; w.unknown_client ^= 1u; fails(w,"unknown_client"); }
      { ExpectDone w=A; w.real_msec ^= 1u;    fails(w,"real_msec"); }
      { ExpectDone w=A; w.user_msec ^= 1u;    fails(w,"user_msec"); }
      { ExpectDone w=A; w.flags ^= 1u;        fails(w,"flags"); }
      { ExpectDone w=A; w.client_count ^= 1u; fails(w,"client_count"); }
      { ExpectDone w=A; w.sys_msec ^= 1u;        fails(w,"sys_msec"); }
      { ExpectDone w=A; w.pfaults ^= 1u;         fails(w,"pfaults"); }
      { ExpectDone w=A; w.in_compressed ^= 1u;   fails(w,"in_compressed"); }
      { ExpectDone w=A; w.in_uncompressed ^= 1u; fails(w,"in_uncompressed"); }
      { ExpectDone w=A; w.out_compressed ^= 1u;  fails(w,"out_compressed"); }
      { ExpectDone w=A; w.out_uncompressed ^= 1u; fails(w,"out_uncompressed"); }
      delete p.a; delete p.b; }
done:
    return failures;
}

/* Reader termination + policy controls (local-oracle 22:28): a faithful peer that
   READS the GET_INTERNALS request before emitting the sentinel; explicit TIMEOUT
   (peer open, no sentinel) distinct from EOF; a deterministic READ_ERROR; and the
   channel-specific STATS policy (skipped when allowed, captured when not). */
static int case_reader_controls(const char *iceccd)
{
    (void)iceccd;
    const ExpectUseCS A{ 100, "10.0.0.1", 1u, "x86_64", true, 7, 3 };
    /* 1. faithful peer: a thread reads the request, THEN emits [UseCS, STATUS_TEXT] */
    { ChannelPair p = make_channel_pair(); REQUIRE_OR_ABORT(p.a && p.b, "reader pair faithful");
      std::thread peer([&]{
          Msg *req = wait_for_type(p.b, Msg::GET_INTERNALS, 2000, true);
          if (req) { delete req;
              p.b->send_msg(UseCSMsg("x86_64", "10.0.0.1", 1u, 100, true, 7, 3));
              p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending(); }
      });
      FrameSeq r = read_until_status(p.a, 2000, false);
      peer.join();
      std::string y;
      REQUIRE(usecs_seq_exact(r, std::vector<ExpectUseCS>{A}, &y), "faithful peer: request consumed before sentinel (" + y + ")");
      delete p.a; delete p.b; }
    /* 2. explicit TIMEOUT: peer open, sends a UseCS but no STATUS_TEXT */
    { ChannelPair p = make_channel_pair(); REQUIRE_OR_ABORT(p.a && p.b, "reader pair timeout");
      p.b->send_msg(UseCSMsg("x86_64", "10.0.0.1", 1u, 100, true, 7, 3)); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 500, false);
      REQUIRE(!r.clean() && r.term == Term::TIMEOUT, "no sentinel in the window -> TIMEOUT (distinct from eof/read-error)");
      delete p.a; delete p.b; }
    /* 3. deterministic READ_ERROR: shut down p.a's write side -> the GET_INTERNALS send fails */
    { ChannelPair p = make_channel_pair(); REQUIRE_OR_ABORT(p.a && p.b, "reader pair rderr");
      shutdown(p.a->fd, SHUT_WR);
      FrameSeq r = read_until_status(p.a, 800, false);
      REQUIRE(!r.clean() && r.term == Term::READ_ERROR, "a failed request send -> READ_ERROR (distinct from eof/timeout)");
      delete p.a; delete p.b; }
    /* 4a. STATS policy (allowed=scheduler): a periodic STATS is skipped */
    { ChannelPair p = make_channel_pair(); REQUIRE_OR_ABORT(p.a && p.b, "reader pair stats-allow");
      p.b->send_msg(StatsMsg());
      p.b->send_msg(UseCSMsg("x86_64", "10.0.0.1", 1u, 100, true, 7, 3));
      p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, true);
      std::string y;
      REQUIRE(usecs_seq_exact(r, std::vector<ExpectUseCS>{A}, &y), "allow_stats: a periodic STATS is skipped (" + y + ")");
      delete p.a; delete p.b; }
    /* 4b. STATS policy (not allowed=client): a STATS is captured as an unexpected frame */
    { ChannelPair p = make_channel_pair(); REQUIRE_OR_ABORT(p.a && p.b, "reader pair stats-reject");
      p.b->send_msg(StatsMsg()); p.b->send_msg(StatusTextMsg("s")); p.b->flush_pending();
      FrameSeq r = read_until_status(p.a, 1500, false);
      REQUIRE(r.term == Term::STATUS_SENTINEL && r.frames.size() == 1
              && (Msg::Value)(*r.frames[0]) == Msg::STATS,
              "client channel: a STATS is captured (unexpected), not silently skipped");
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
        REQUIRE_OR_ABORT(sched_barrier(f.sched, 4000), "scheduler consumed the decision (sentinel)");
        FrameSeq useq = read_until_status(client, 4000, false);
        REQUIRE_OR_ABORT(useq.clean() && useq.frames.size() == 1 && (*useq.frames[0] == Msg::USE_CS),
                         "one UseCS delivered on a clean FIFO-sentinel stream");
        UseCSMsg *um = dynamic_cast<UseCSMsg *>(useq.frames[0]);
        const SeenUseCS s{ um->job_id, um->hostname, um->port, um->host_platform, um->got_env != 0, um->client_id, um->matched_job_id };
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
    else if (c == "eof-controls")       rc = case_eof_controls(argv[1]);
    else if (c == "fifo-controls")      rc = case_fifo_controls(argv[1]);
    else if (c == "done-controls")      rc = case_done_controls(argv[1]);
    else if (c == "reader-controls")    rc = case_reader_controls(argv[1]);
    else { fprintf(stderr, "unknown case: %s\n", c.c_str()); return 2; }

    fprintf(stderr, "%s [%s] (%d failure%s)\n",
            rc ? "RESULT: FAIL" : "RESULT: PASS", c.c_str(), rc, rc == 1 ? "" : "s");
    return rc ? 1 : 0;
}
