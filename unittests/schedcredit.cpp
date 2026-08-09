/*
    Scheduler gates for the dispatch-credit / stall-report change
    (BP-1 Stage A): a submitter's unconfirmed assignments are bounded by a
    credit, a stalled assignment is REPORTED -- never removed, never
    released -- and a late-thawing client still reconciles exactly once.

    Modes (argv[2]):

      credit     - a flood of never-confirmed requests receives exactly the
                   effective credit's worth of assignments and no more;
                   confirming some releases exactly that many further
                   assignments.
      report     - crossing the report threshold produces EXACTLY ONE
                   warning per continuous episode; resolving the episode
                   and creating a new one produces exactly one more.
      retention  - the ownership proof: past the threshold the assignment
                   is retained (job in listjobs, worker reservation held,
                   credit outstanding); a LATE JobBegin credits exactly
                   once; a late JobDone ends it exactly once (job gone,
                   reservation released, one monitor terminal event);
                   freed capacity accepts new work.
      noreader   - the honest stopped-daemon model: with credit 1 and a
                   submitter whose process stops reading entirely (the
                   kernel keeps ACKing its tiny reply), neither connection
                   failure nor any drain deadline can fire; the submitter
                   is reported, capped at its credit, NOT removed, and a
                   healthy peer keeps progressing.
      clientstall- a submitting daemon proxies every wrapper on its host;
                   one wrapper frozen after its assignment must not
                   starve, disconnect, or destroy its healthy siblings on
                   the SAME connection.
      mixedrole  - a mixed-role host at full remote credit still receives
                   the submitter-local decision (local slots are not idled
                   by remote debits, and the local decision is not debited).
      clamp      - a discriminating nonzero farm shrink: after removing a
                   large worker, the reduced ceiling (surviving slots - 1)
                   blocks a job a stale larger aggregate would have
                   dispatched.
      relisten   - a second daemon login must complete within the listener
                   re-arm bound while the daemon listener is de-armed, with
                   no external stimulus (the control listener stays polled).

    Every observation is over the scheduler's own interfaces: the message
    flow, the text control port (listcs fields admitted_total/gen/
    outstanding, listjobs job lines), and a monitor connection.  No
    preload instrument is needed, so these gates run on every platform.
*/

#include "../services/comm.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <chrono>
#include <string>
#include <algorithm>
#include <vector>

typedef std::chrono::steady_clock Clock;
static double secs_since(Clock::time_point t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

static int failures = 0;
static int checks_executed = 0;
static void check(bool ok, const char *what)
{
    ++checks_executed;
    printf(ok ? "ok       - %s\n" : "FAILED   - %s\n", what);
    if (!ok) {
        ++failures;
    }
    fflush(stdout);
}

/* ---- lifecycle (the corrected shape: signal-defined published state,
   checked masks, one reap owner whose terminal states never flow into a
   later kill) ------------------------------------------------------------ */

static pid_t sched_pid = -1;
static volatile sig_atomic_t sig_sched_pid = 0;
static std::vector<MsgChannel *> channels;
static char logname[64];

static void mask_failure_abort(const char *where)
{
    fprintf(stderr, "schedcredit: %s: mask transition failed; aborting\n", where);
    const sig_atomic_t p = sig_sched_pid;
    if (p > 0) {
        kill((pid_t)p, SIGKILL);
    }
    _exit(2);
}

static bool publish_sched(pid_t pid)
{
    if (pid > 0 && (uintmax_t)pid > (uintmax_t)SIG_ATOMIC_MAX) {
        return false;
    }
    sched_pid = pid;
    sig_sched_pid = pid > 0 ? (sig_atomic_t)pid : 0;
    return true;
}

static void on_signal(int)
{
    const sig_atomic_t p = sig_sched_pid;
    if (p > 0) {
        kill((pid_t)p, SIGKILL);
    }
    _exit(2);
}

static bool block_handled(sigset_t *old)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &set, old) != 0) {
        perror("sigprocmask");
        return false;
    }
    return true;
}

static bool unblock_handled(const sigset_t *old)
{
    if (sigprocmask(SIG_SETMASK, old, nullptr) != 0) {
        perror("sigprocmask");
        return false;
    }
    return true;
}

/* Terminal only on a confirmed reap or ECHILD; anything else aborts by
   name rather than pretending the child is gone.  */
/* Requested shutdown returns a typed verdict and the harness asserts
   the expected clean exit: an unexpected signal, nonzero status, or
   kill escalation is a red result, not a discarded int.  */
enum ShutdownResult { SHUTDOWN_CLEAN, SHUTDOWN_UNEXPECTED, SHUTDOWN_ESCALATED };
static ShutdownResult end_scheduler_checked(pid_t pid)
{
    if (pid <= 0) {
        return SHUTDOWN_UNEXPECTED;
    }
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int st = -1;
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            return (WIFEXITED(st) && WEXITSTATUS(st) == 0)
                ? SHUTDOWN_CLEAN : SHUTDOWN_UNEXPECTED;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ECHILD) {
                /* reaped elsewhere: ownership was recorded, so this is an
                   unexpected path, not a clean verdict */
                return SHUTDOWN_UNEXPECTED;
            }
            mask_failure_abort("end_scheduler/waitpid");
        }
        usleep(100 * 1000);
    }
    kill(pid, SIGKILL);
    for (;;) {
        const pid_t r = waitpid(pid, nullptr, 0);
        if (r == pid || (r < 0 && errno == ECHILD)) {
            return SHUTDOWN_ESCALATED;
        }
        if (r < 0 && errno != EINTR) {
            mask_failure_abort("end_scheduler/waitpid2");
        }
    }
}

/* Signal-handler-safe legacy shape (cleanup paths that cannot assert).  */
static void end_scheduler(pid_t pid)
{
    (void)end_scheduler_checked(pid);
}

static void cleanup(void)
{
    for (MsgChannel *ch : channels) {
        delete ch;
    }
    channels.clear();
    sigset_t old;
    if (!block_handled(&old)) {
        mask_failure_abort("cleanup/block");
    }
    if (sched_pid > 0) {
        end_scheduler(sched_pid);
        publish_sched(-1);
    }
    if (!unblock_handled(&old)) {
        mask_failure_abort("cleanup/restore");
    }
}

static int die(const char *msg)
{
    fprintf(stderr, "schedcredit: %s\n", msg);
    cleanup();
    return 2;
}

/* ---- plumbing ----------------------------------------------------------- */

static const char *kPlatform = "x86_64-credittest";
static const char *kEnv = "credit-env";

static int tcp_connect(int port)
{
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool port_pair_free(int port)
{
    for (int off = 0; off < 2; ++off) {
        const int fd = socket(PF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return false;
        }
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port + off);
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        const bool ok = bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
        close(fd);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static MsgChannel *track(MsgChannel *ch)
{
    if (ch) {
        channels.push_back(ch);
    }
    return ch;
}

static MsgChannel *connect_channel(int port)
{
    int fd = tcp_connect(port);
    if (fd < 0) {
        return nullptr;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    return track(Service::createChannel(fd, (struct sockaddr *)&sa, sizeof(sa)));
}

static ssize_t read_deadline(int fd, char *buf, size_t cap, int per_read_ms)
{
    struct pollfd pf;
    pf.fd = fd;
    pf.events = POLLIN;
    const int r = poll(&pf, 1, per_read_ms);
    if (r <= 0) {
        return -1;
    }
    return read(fd, buf, cap);
}

/* The scheduler re-arms its daemon listener at most once per second and
   only when its poll wakes.  With no daemon traffic yet, nothing wakes it
   -- standalone runs were saved by stray broadcast packets, which is
   exactly the nondeterminism a gate must not depend on.  A control-port
   round trip is the deterministic wake: that listener is not throttled,
   and its traffic drives the main loop around to re-arm the daemon
   listener.  */
static void wake_scheduler(int port)
{
    const int fd = tcp_connect(port + 1);
    if (fd >= 0) {
        char buf[256];
        read_deadline(fd, buf, sizeof(buf), 2000);
        close(fd);
    }
    usleep(300 * 1000);
}

static MsgChannel *login_daemon(int port, const char *name, unsigned lport,
                                int max_kids, bool noremote)
{
    MsgChannel *ch = nullptr;
    for (int attempt = 0; attempt < 4 && !ch; ++attempt) {
        wake_scheduler(port);
        sleep(1);              // step past the once-per-second accept re-arm
        wake_scheduler(port);
        ch = connect_channel(port);
    }
    if (!ch) {
        return nullptr;
    }
    LoginMsg login(lport, name, kPlatform, 0);
    login.envs.push_back(std::make_pair(std::string(kPlatform), std::string(kEnv)));
    login.max_kids = max_kids;
    login.noremote = noremote;
    login.chroot_possible = !noremote;
    if (!ch->send_msg(login)) {
        return nullptr;
    }
    return ch;
}

static bool send_request(MsgChannel *ch, unsigned cid,
                         const char *preferred = "")
{
    char fname[48];
    snprintf(fname, sizeof(fname), "credit%u.cpp", cid);
    GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
               fname, CompileJob::Lang_CXX, 1, kPlatform, 0,
               std::string(preferred), 0, 0, 0);
    g.client_id = cid;
    return ch->send_msg(g);
}

static std::vector<MsgChannel *> beat_channels;
static Clock::time_point last_beat = Clock::now();
static void heartbeat(void)
{
    if (secs_since(last_beat) < 5) {
        return;
    }
    last_beat = Clock::now();
    for (MsgChannel *ch : beat_channels) {
        StatsMsg st;
        ch->send_msg(st);
    }
}

/* One control-port round trip with DEADLINES on every read and on the
   whole exchange: a control regression must be a bounded test failure,
   never a suite hang.  */


/* Typed management exchange: a reply is usable ONLY when its status is
   COMPLETE (the "200 done" terminator arrived inside the whole-exchange
   budget).  Every other status is an observation failure that the
   caller must treat as such -- the old string-returning helper handed
   back whatever partial bytes accumulated, so a dead scheduler or a
   truncated reply could read as an empty-but-plausible result.  */
struct ControlReply {
    enum Status { CONNECT_FAILED, GREETING_FAILED, INCOMPLETE, COMPLETE };
    Status status;
    std::string text;
};

static ControlReply control_checked(int port, const char *cmd)
{
    ControlReply r;
    r.status = ControlReply::CONNECT_FAILED;
    const int fd = tcp_connect(port + 1);
    if (fd < 0) {
        return r;
    }
    char buf[4096];
    const Clock::time_point t0 = Clock::now();
    const double budget = 10.0;   // whole exchange, absolute
    auto remaining_ms = [&]() -> int {
        const double left = budget - secs_since(t0);
        return left <= 0 ? 0 : (int)(left * 1000);
    };
    ssize_t n = read_deadline(fd, buf, sizeof(buf), remaining_ms());  // greeting
    if (n <= 0) {
        close(fd);
        r.status = ControlReply::GREETING_FAILED;
        return r;
    }
    dprintf(fd, "%s\nquit\n", cmd);
    r.status = ControlReply::INCOMPLETE;
    while (remaining_ms() > 0) {
        n = read_deadline(fd, buf, sizeof(buf) - 1, remaining_ms());
        if (n <= 0) {
            break;
        }
        buf[n] = 0;
        r.text += buf;
        if (r.text.find("200 done") != std::string::npos) {
            r.status = ControlReply::COMPLETE;
            break;
        }
    }
    close(fd);
    return r;
}

/* Legacy shape for existing call sites: COMPLETE text or EMPTY -- an
   incomplete reply can no longer masquerade as content, because only a
   terminator-bearing exchange returns any bytes at all.  */
static std::string control(int port, const char *cmd)
{
    const ControlReply r = control_checked(port, cmd);
    return r.status == ControlReply::COMPLETE ? r.text : std::string();
}

/* listcs field for a named submitter: "... name (...) ... field<value>" */
static long long submitter_field(int port, const char *name, const char *field)
{
    const std::string text = control(port, "listcs");
    size_t pos = text.find(std::string(" ") + name + " ");
    if (pos == std::string::npos) {
        pos = text.find(std::string(" ") + name + "(");
        if (pos == std::string::npos) {
            return -1;
        }
    }
    const size_t eol = text.find('\n', pos);
    const size_t f = text.find(field, pos);
    if (f == std::string::npos || (eol != std::string::npos && f > eol)) {
        return -1;
    }
    return atoll(text.c_str() + f + strlen(field));
}

static bool job_in_scheduler(int port, unsigned job_id)
{
    const std::string text = control(port, "listjobs");
    char needle[32];
    snprintf(needle, sizeof(needle), " %u ", job_id);
    /* job lines are indented; the id is the first token */
    return text.find(needle) != std::string::npos
        || text.find(std::string("\n") + std::to_string(job_id) + " ") != std::string::npos;
}

static int log_count(const char *needle)
{
    FILE *lf = fopen(logname, "r");
    if (!lf) {
        return -1;
    }
    int n = 0;
    char line[4096];
    while (fgets(line, sizeof(line), lf)) {
        if (strstr(line, needle)) {
            ++n;
        }
    }
    fclose(lf);
    return n;
}

/* Drain one channel briefly, collecting UseCS job ids for a client range.  */
/* Collect UseCS replies for [lo, hi].  With want > 0, returns as soon as
   that many arrive; with want == 0, observes the FULL window (the shape a
   cap assertion needs: "no more than N arrive, ever").  */
static int drain_usecs(MsgChannel *ch, unsigned lo, unsigned hi,
                       std::vector<unsigned> *jids, double secs, int want = 0)
{
    int got = 0;
    const Clock::time_point t0 = Clock::now();
    while (secs_since(t0) < secs && (want == 0 || got < want)) {
        heartbeat();
        Msg *m = ch->get_msg(1);
        if (!m) {
            continue;
        }
        if (*m == Msg::USE_CS) {
            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
            if (u && u->client_id >= lo && u->client_id <= hi) {
                ++got;
                if (jids) {
                    jids->push_back(u->job_id);
                }
            }
        }
        delete m;
    }
    return got;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <icecc-scheduler> "
                "credit|report|retention|noreader|clientstall|mixedrole|clamp|relisten\n",
                argv[0]);
        return 2;
    }
    const std::string mode = argv[2];
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    snprintf(logname, sizeof(logname), "schedcredit-%d.log", (int)getpid());
    char netname[48];
    snprintf(netname, sizeof(netname), "credittest%d", (int)getpid());

    /* bounded probe/spawn/classify startup, per the established pattern */
    int port = 0;
    {
        int cand = 21500 + (getpid() % 2000);
        for (int attempt = 0; attempt < 5 && port == 0; ++attempt) {
            int chosen = 0;
            for (; cand < 21500 + 4000; cand += 2) {
                if (port_pair_free(cand)) {
                    chosen = cand;
                    cand += 2;
                    break;
                }
            }
            if (!chosen) {
                return die("no free port pair");
            }
            const pid_t parent_before = getpid();
            sigset_t oldmask;
            if (!block_handled(&oldmask)) {
                mask_failure_abort("startup/block");
            }
            const pid_t child = fork();
            if (child == 0) {
                if (!unblock_handled(&oldmask)) {
                    _exit(8);
                }
                signal(SIGPIPE, SIG_DFL);
#ifdef __linux__
                prctl(PR_SET_PDEATHSIG, SIGKILL);
                if (getppid() != parent_before) {
                    _exit(1);
                }
#endif
                FILE *lf = fopen(logname, "w");
                if (lf) {
                    dup2(fileno(lf), 1);
                    dup2(fileno(lf), 2);
                }
                char portbuf[16];
                snprintf(portbuf, sizeof(portbuf), "%d", chosen);
                if (mode == "noreader") {
                    execl(argv[1], argv[1], "-p", portbuf, "-n", netname, "-vvv",
                          "--dispatch-stall-report-after=10",
                          "--max-outstanding-dispatches=1", (char *)nullptr);
                } else {
                    execl(argv[1], argv[1], "-p", portbuf, "-n", netname, "-vvv",
                          "--dispatch-stall-report-after=10", (char *)nullptr);
                }
                _exit(127);
            }
            if (!publish_sched(child > 0 ? child : -1)) {
                kill(child, SIGKILL);
                while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
                }
                if (!unblock_handled(&oldmask)) {
                    mask_failure_abort("startup/restore-unpublishable");
                }
                return die("child pid does not fit sig_atomic_t");
            }
            if (!unblock_handled(&oldmask)) {
                mask_failure_abort("startup/restore");
            }
            if (child < 0) {
                return die("fork");
            }
            bool up = false, exited = false, exec_failed = false;
            for (int i = 0; i < 100 && !up; ++i) {
                int status = 0;
                sigset_t om;
                if (!block_handled(&om)) {
                    mask_failure_abort("classify/block");
                }
                pid_t r;
                for (;;) {
                    r = waitpid(child, &status, WNOHANG);
                    if (r >= 0 || errno != EINTR) {
                        break;
                    }
                }
                if (r == child) {
                    publish_sched(-1);
                } else if (r < 0) {
                    if (errno == ECHILD) {
                        publish_sched(-1);
                    } else {
                        mask_failure_abort("classify/waitpid");
                    }
                }
                if (!unblock_handled(&om)) {
                    mask_failure_abort("classify/restore");
                }
                if (r == child || r < 0) {
                    exited = true;
                    exec_failed = r == child
                        && WIFEXITED(status) && WEXITSTATUS(status) == 127;
                    break;
                }
                const int probe = tcp_connect(chosen);
                if (probe >= 0) {
                    close(probe);
                    up = true;
                    break;
                }
                usleep(100 * 1000);
            }
            if (exec_failed) {
                return die("scheduler exec failed");
            }
            if (up) {
                port = chosen;
                break;
            }
            {
                sigset_t old2;
                if (!block_handled(&old2)) {
                    mask_failure_abort("retry/block");
                }
                if (!exited) {
                    end_scheduler(child);
                }
                publish_sched(-1);
                if (!unblock_handled(&old2)) {
                    mask_failure_abort("retry/restore");
                }
            }
        }
        if (port == 0 || sched_pid < 0) {
            return die("scheduler could not be started after retries");
        }
    }

    /* topology: one worker with generous slots, one main submitter, one
       healthy peer.  The worker heartbeats a zero-load stats immediately
       (a fresh daemon reports load 1000 and is ineligible until then).  */
    MsgChannel *cs = login_daemon(port, "creditcs", 10245, 40, false);
    if (!cs) {
        return die("worker connect/login failed");
    }
    {
        StatsMsg st;
        if (!cs->send_msg(st)) {
            return die("worker stats failed");
        }
    }
    beat_channels.push_back(cs);
    auto wake_listener = [&]() {
        sleep(1);
        StatsMsg st;
        cs->send_msg(st);
        usleep(300 * 1000);
    };

    wake_listener();
    MsgChannel *sub = login_daemon(port, "creditsub", 0, 0, true);
    if (!sub) {
        return die("submitter connect/login failed");
    }
    beat_channels.push_back(sub);
    wake_listener();
    MsgChannel *peer = login_daemon(port, "creditpeer", 0, 0, true);
    if (!peer) {
        return die("peer connect/login failed");
    }
    beat_channels.push_back(peer);
    wake_listener();

    auto confirm = [&](unsigned jid, bool done) {
        JobBeginMsg jb(jid, 1);
        cs->send_msg(jb);
        if (done) {
            JobDoneMsg jd(jid, 0, JobDoneMsg::FROM_SERVER);
            cs->send_msg(jd);
        }
    };
    auto peer_completes_one = [&](unsigned cid, double secs) -> bool {
        if (!send_request(peer, cid)) {
            return false;
        }
        std::vector<unsigned> jids;
        const Clock::time_point t0 = Clock::now();
        while (secs_since(t0) < secs) {
            heartbeat();
            Msg *m = peer->get_msg(1);
            if (!m) {
                continue;
            }
            unsigned jid = 0;
            if (*m == Msg::USE_CS) {
                UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                if (u && u->client_id == cid) {
                    jid = u->job_id;
                }
            }
            delete m;
            if (jid) {
                confirm(jid, true);
                return true;
            }
        }
        return false;
    };

    if (mode == "credit") {
        /* Effective credit here: min(32, farm slots 40 - 1) = 32.  Flood 50
           never-confirmed requests: exactly 32 assignments may arrive.  */
        for (unsigned i = 0; i < 50; ++i) {
            if (!send_request(sub, 100 + i)) {
                return die("flood send failed");
            }
        }
        std::vector<unsigned> jids;
        const int first = drain_usecs(sub, 100, 149, &jids, 8);
        printf("# credit: assignments before any confirmation: %d\n", first);
        check(first == 32,
              "exactly the effective dispatch credit (32) was delivered to an"
              " unconfirming submitter");
        check(submitter_field(port, "creditsub", "outstanding=") == 32,
              "the scheduler's own accounting shows the full credit outstanding");
        check(peer_completes_one(900, 30),
              "a healthy peer is served while the flood is capped");

        /* Confirm 10: exactly 10 more dispatch.  */
        if (jids.size() < 10) {
            check(false, "not enough assignments to continue the release phase");
            cleanup();
            printf("RESULT: FAIL (%d)\n", failures);
            return 1;
        }
        for (int i = 0; i < 10; ++i) {
            confirm(jids[i], false);
        }
        const int more = drain_usecs(sub, 100, 149, &jids, 8);
        printf("# credit: assignments after confirming 10: %d\n", more);
        check(more == 10, "confirming 10 assignments releases exactly 10 more");
    } else if (mode == "report") {
        /* One frozen assignment; cross the 10s threshold; exactly one
           warning; resolve it; a NEW frozen assignment crosses again;
           exactly one more.  */
        check(send_request(sub, 200), "first frozen request sent");
        std::vector<unsigned> jids;
        check(drain_usecs(sub, 200, 200, &jids, 5, 1) == 1, "first assignment received");
        if (jids.empty()) {
            cleanup();
            printf("RESULT: FAIL (%d)\n", failures);
            return 1;
        }
        const Clock::time_point t0 = Clock::now();
        while (secs_since(t0) < 16) {
            heartbeat();
            Msg *m = sub->get_msg(1);
            delete m;
        }
        int w = log_count("is not progressing");
        printf("# report: warnings after first episode: %d\n", w);
        check(w == 1, "a continuous stall episode is reported EXACTLY once");

        confirm(jids[0], true);   // resolve the episode
        usleep(1500 * 1000);
        check(send_request(sub, 201), "second frozen request sent");
        std::vector<unsigned> jids2;
        check(drain_usecs(sub, 201, 201, &jids2, 5, 1) == 1, "second assignment received");
        if (jids2.empty()) {
            cleanup();
            printf("RESULT: FAIL (%d)\n", failures);
            return 1;
        }
        const Clock::time_point t1 = Clock::now();
        while (secs_since(t1) < 16) {
            heartbeat();
            Msg *m = sub->get_msg(1);
            delete m;
        }
        w = log_count("is not progressing");
        printf("# report: warnings after second episode: %d\n", w);
        check(w == 2, "a genuinely new episode is reported exactly once more");
    } else if (mode == "retention") {
        /* monitor first, so the terminal-event count is complete */
        MsgChannel *mon = connect_channel(port);
        check(mon != nullptr, "monitor connected");
        if (mon) {
            mon->send_msg(MonLoginMsg());
        }
        wake_listener();

        check(send_request(sub, 300), "frozen request sent");
        std::vector<unsigned> jids;
        check(drain_usecs(sub, 300, 300, &jids, 5, 1) == 1, "assignment received");
        const unsigned frozen = jids.empty() ? 0 : jids[0];
        check(frozen != 0, "frozen job id known");

        const Clock::time_point t0 = Clock::now();
        while (secs_since(t0) < 14) {
            heartbeat();
            Msg *m = sub->get_msg(1);
            delete m;
        }
        /* Past the threshold: RETAINED, not removed, not released.  */
        check(log_count("is not progressing") >= 1, "the stall was reported");
        check(job_in_scheduler(port, frozen),
              "the assignment is still in the scheduler's job map past the threshold");
        check(submitter_field(port, "creditsub", "outstanding=") == 1,
              "its dispatch credit is still debited");
        check(submitter_field(port, "creditcs", "jobs=") >= 1,
              "the worker reservation is retained");
        check(log_count("remove daemon creditsub") == 0,
              "the submitting daemon was NOT removed");

        /* LATE thaw: Begin credits exactly once...  */
        {
            JobBeginMsg jb(frozen, 1);
            check(cs->send_msg(jb), "late JobBegin sent");
            const Clock::time_point tb = Clock::now();
            bool credited = false;
            while (!credited && secs_since(tb) < 10) {
                heartbeat();
                credited = submitter_field(port, "creditsub", "outstanding=") == 0;
                if (!credited) {
                    usleep(200 * 1000);
                }
            }
            check(credited, "the late JobBegin credited the retained assignment");
            check(job_in_scheduler(port, frozen),
                  "the job is still owned after its late Begin (now compiling)");
        }
        /* ...and Done ends it exactly once.  */
        {
            JobDoneMsg jd(frozen, 0, JobDoneMsg::FROM_SERVER);
            check(cs->send_msg(jd), "late JobDone sent");
            const Clock::time_point td = Clock::now();
            bool gone = false;
            while (!gone && secs_since(td) < 10) {
                heartbeat();
                gone = !job_in_scheduler(port, frozen);
                if (!gone) {
                    usleep(200 * 1000);
                }
            }
            check(gone, "the late completion removed the job exactly once");
            /* The worker has no other live job in this mode, so its job
               list dropping to zero proves the RETAINED reservation itself
               was released -- the later peer completion alone could have
               used any of the worker's other slots.  */
            check(submitter_field(port, "creditcs", "jobs=") == 0,
                  "the retained worker reservation was released (worker job"
                  " list is empty before any new work)");
        }
        /* the monitor saw exactly one Begin and one terminal Done */
        if (mon) {
            int b = 0, d = 0;
            const Clock::time_point tm = Clock::now();
            while (secs_since(tm) < 4) {
                Msg *m = mon->get_msg(1);
                if (!m) {
                    continue;
                }
                if (*m == Msg::MON_JOB_BEGIN) {
                    MonJobBeginMsg *x = dynamic_cast<MonJobBeginMsg *>(m);
                    if (x && x->job_id == frozen) {
                        ++b;
                    }
                } else if (*m == Msg::MON_JOB_DONE) {
                    MonJobDoneMsg *x = dynamic_cast<MonJobDoneMsg *>(m);
                    if (x && x->job_id == frozen) {
                        ++d;
                    }
                }
                delete m;
            }
            printf("# retention: monitor saw begin=%d done=%d\n", b, d);
            check(b == 1 && d == 1,
                  "the monitor saw exactly one Begin and one terminal Done");
        }
        check(peer_completes_one(901, 30), "freed capacity accepts new work");
    } else if (mode == "noreader") {
        /* credit 1 (scheduler started with --max-outstanding-dispatches=1).
           Three requests; then the submitter goes SILENT -- no reads.  The
           replies are tiny (short platform), so nothing arms any transport
           deadline; the kernel ACKs.  Stage-A behavior: reported, capped,
           retained, NOT removed; the peer progresses.  */
        for (unsigned i = 0; i < 3; ++i) {
            if (!send_request(sub, 400 + i)) {
                return die("noreader send failed");
            }
        }
        /* barrier: the one-credit assignment exists and holds a worker
           reservation before the retention window opens */
        {
            const Clock::time_point tb = Clock::now();
            bool ready = false;
            while (!ready && secs_since(tb) < 20) {
                heartbeat();
                ready = submitter_field(port, "creditsub", "outstanding=") == 1
                    && submitter_field(port, "creditcs", "jobs=") >= 1;
                if (!ready) {
                    usleep(200 * 1000);
                }
            }
            check(ready, "barrier: the credit-1 assignment AND its worker"
                         " reservation exist before the retention window");
        }
        const Clock::time_point t0 = Clock::now();
        long long min_worker = 1000000;
        while (secs_since(t0) < 16) {
            heartbeat();
            const long long wj = submitter_field(port, "creditcs", "jobs=");
            if (wj >= 0 && wj < min_worker) {
                min_worker = wj;
            }
            usleep(500 * 1000);
        }
        check(submitter_field(port, "creditsub", "outstanding=") == 1,
              "exactly the one-credit assignment remains outstanding");
        check(min_worker >= 1, "the worker reservation stayed owned throughout");
        check(log_count("is not progressing") >= 1, "the silent submitter was reported");
        check(log_count("remove daemon creditsub") == 0, "and NOT removed");
        check(peer_completes_one(902, 30), "a healthy peer keeps progressing");
    } else if (mode == "clientstall") {
        /* One connection, many wrappers.  Wrapper 500 freezes after its
           assignment; siblings 510.. keep confirming across the threshold
           and must be served throughout; the daemon survives; the stall is
           reported once.  */
        check(send_request(sub, 500), "frozen wrapper's request sent");
        std::vector<unsigned> jids;
        check(drain_usecs(sub, 500, 500, &jids, 5, 1) == 1, "frozen wrapper assigned");

        int served = 0;
        const Clock::time_point t0 = Clock::now();
        unsigned cid = 510;
        while (secs_since(t0) < 22) {
            heartbeat();
            if (!send_request(sub, cid)) {
                break;
            }
            std::vector<unsigned> got;
            if (drain_usecs(sub, cid, cid, &got, 5, 1) == 1) {
                confirm(got[0], true);
                ++served;
            }
            ++cid;
        }
        printf("# clientstall: healthy siblings served across the threshold: %d\n", served);
        check(served >= 10,
              "healthy wrappers on the SAME connection keep being served");
        check(!sub->at_eof(), "the daemon was NOT disconnected");
        check(log_count("remove daemon creditsub") == 0, "and NOT removed");
        check(log_count("is not progressing") == 1, "the stall was reported exactly once");
        check(submitter_field(port, "creditsub", "outstanding=") >= 1,
              "the frozen assignment is RETAINED (nothing deleted an assignment"
              " nobody cancelled)");
    } else if (mode == "mixedrole") {
        /* The submitter is MIXED-ROLE: it re-logs-in below with local
           slots.  Exhaust its remote credit against the named worker, then
           an ordinary request must still receive the submitter-LOCAL
           decision (NoCS at protocol >= 37), because local work reserves
           no farm slot and is never debited.  Folding the credit into the
           group gate idled exactly this machine.  */
        MsgChannel *mixed = login_daemon(port, "creditmixed", 10247, 4, false);
        if (!mixed) {
            return die("mixed-role login failed");
        }
        {
            StatsMsg st;
            if (!mixed->send_msg(st)) {
                return die("mixed stats failed");
            }
        }
        beat_channels.push_back(mixed);
        wake_listener();

        /* Exhaust the remote credit: prefer the OTHER worker so nothing
           lands locally.  Effective ceiling = min(32, slots-1); slots =
           40 + 4 = 44 -> 32.  */
        for (unsigned i = 0; i < 40; ++i) {
            if (!send_request(mixed, 600 + i, "creditcs")) {
                return die("mixed flood send failed");
            }
        }
        std::vector<unsigned> jids;
        const int got = drain_usecs(mixed, 600, 639, &jids, 8);
        printf("# mixedrole: remote assignments before confirmation: %d\n", got);
        check(got == 32, "the remote credit ceiling was reached (32)");
        check(submitter_field(port, "creditmixed", "outstanding=") == 32,
              "outstanding sits at the ceiling");

        /* Now the discriminator: an UNPREFERRED request while at the
           ceiling.  It must be placed LOCALLY (NoCS), not starved.  */
        check(send_request(mixed, 700), "ordinary request sent at full credit");
        bool local_decision = false;
        const Clock::time_point t0 = Clock::now();
        while (!local_decision && secs_since(t0) < 20) {
            heartbeat();
            Msg *m = mixed->get_msg(1);
            if (!m) {
                continue;
            }
            if (*m == Msg::NO_CS) {
                NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                if (n && n->client_id == 700) {
                    local_decision = true;
                }
            }
            delete m;
        }
        check(local_decision,
              "a mixed-role submitter at full remote credit still receives the"
              " submitter-LOCAL decision (local slots must not be idled by"
              " remote debits)");
        check(submitter_field(port, "creditmixed", "outstanding=") == 32,
              "the local decision was not debited (outstanding unchanged)");
    } else if (mode == "relisten") {
        /* Two-session LIVENESS observation through the listener re-arm
           path -- deliberately NOT claimed as a deadline regression.  It
           does not prove B connected inside the one-second de-armed
           interval, and the other logged-in daemons' timers still wake the
           scheduler loop here, so a missing poll-deadline cap is NOT
           red-detectable in this topology (verified: reverting only the
           cap still passes).  The discriminating deadline regression
           belongs to the isolated idle-scheduler fixture.  What this DOES
           prove: two raw back-to-back daemon logins each receive their own
           CS_CONF promptly, with no control query, Stats heartbeat, or
           broadcast between them.  */
        auto raw_login_await_conf = [&](const char *name, double secs,
                                        MsgChannel **out) -> double {
            MsgChannel *d = connect_channel(port);
            if (!d) { return -1.0; }
            *out = d;   /* the caller owns and closes the session */
            LoginMsg login(0, name, kPlatform, 0);
            login.envs.push_back(std::make_pair(std::string(kPlatform), std::string(kEnv)));
            login.max_kids = 0;
            login.noremote = true;
            login.chroot_possible = false;
            const Clock::time_point t0 = Clock::now();
            if (!d->send_msg(login)) { return -1.0; }
            while (secs_since(t0) < secs) {
                Msg *m = d->get_msg(1);
                if (!m) { continue; }
                const bool conf = *m == Msg::CS_CONF;
                delete m;
                if (conf) { return secs_since(t0); }
            }
            return -1.0;   /* no CS_CONF within the bound */
        };

        /* Daemon A: its accept sets next_listen = now+1.  */
        MsgChannel *relA = nullptr;
        MsgChannel *relB = nullptr;
        const double ta = raw_login_await_conf("relistenA", 10, &relA);
        check(ta >= 0, "daemon A logged in (CS_CONF received)");

        /* Daemon B: connects INSIDE A's ~1s de-armed window, no stimulus.
           This proves LIVENESS through the re-arm path -- a second daemon
           still logs in and receives its own CS_CONF while the daemon
           listener is de-armed.  It is deliberately NOT asserted as a red
           proof of the timeout-cap specifically: in this topology the other
           logged-in daemons' connection-test timers already keep the poll
           interval short, so reverting only the cap does NOT delay B here
           (verified: the red variant also completes promptly).  The
           discriminating cap regression needs an IDLE scheduler with no
           competing timers, which the planned scheduler-test fixture
           provides; it is deferred there rather than shipped as a
           non-discriminating gate labelled a proof.  */
        const double tb = raw_login_await_conf("relistenB", 10, &relB);
        printf("# relisten: second login CS_CONF in %.2fs\n", tb);
        check(tb >= 0,
              "a second daemon logs in and receives CS_CONF through the"
              " re-arm path (liveness; the discriminating deadline"
              " regression is deferred to the idle-scheduler fixture)");
        /* Close both sessions deliberately, so the scheduler sees orderly
           EOFs before the harness tears down.  connect_channel() tracks
           every channel for cleanup(), so closure must go through the
           tracking list -- a direct delete here would double-free at
           cleanup.  */
        for (MsgChannel *rel : { relA, relB }) {
            if (rel) {
                channels.erase(std::remove(channels.begin(), channels.end(), rel),
                               channels.end());
                delete rel;
            }
        }
    } else if (mode == "clamp") {
        /* Discrimination boundary, measured: the negative control for this
           gate reverts the aggregate-refresh MECHANISM (freeze once
           nonzero) -- that run dispatches the 4th job on the stale 44-slot
           total and fails here.  A PARTIAL revert (only the setter's or
           only the removal's dirty marking) is NOT red-detectable across
           scheduler turns, because prune_servers() warm-refreshes the
           aggregate every pass; the dirty flag's observable window is a
           single dispatch batch, and placing a removal and a dispatch
           decision in one turn needs a deterministic barrier -- fixture
           territory, recorded there.  */
        /* Discriminating NONZERO shrink.  Two workers: the default 40-slot
           'creditcs' and an added 4-slot 'creditsmall'.  Put exactly THREE
           unconfirmed jobs on the 4-slot worker, then remove the 40-slot
           worker.  The 4-slot worker keeps a free 4th slot.

             fresh aggregate = 4  -> ceiling 3 -> the 4th job is BLOCKED
             stale aggregate = 44 -> ceiling 32 -> the 4th job dispatches

           So the 4th job's fate is decided ENTIRELY by whether the clamp
           saw the removal.  (The old zero-worker check could not
           discriminate: after removal nothing is eligible regardless.)  */
        MsgChannel *small = login_daemon(port, "creditsmall", 10248, 4, false);
        if (!small) {
            return die("small worker login failed");
        }
        {
            StatsMsg st;
            if (!small->send_msg(st)) {
                return die("small worker stats failed");
            }
        }
        beat_channels.push_back(small);
        wake_scheduler(port);

        /* Three unconfirmed jobs pinned to the small worker.  */
        for (unsigned i = 0; i < 3; ++i) {
            if (!send_request(sub, 800 + i, "creditsmall")) {
                return die("clamp pin send failed");
            }
        }
        std::vector<unsigned> jids;
        const int pinned = drain_usecs(sub, 800, 802, &jids, 8, 3);
        check(pinned == 3, "three jobs pinned to the 4-slot worker");
        check(submitter_field(port, "creditsub", "outstanding=") == 3,
              "outstanding sits at 3 (fresh ceiling of 3, derived from four surviving slots; the 4th job is what the shrink must block)");

        /* Remove the big worker.  Barrier on the scheduler noticing.  */
        for (std::vector<MsgChannel *>::iterator it = beat_channels.begin();
                it != beat_channels.end(); ++it) {
            if (*it == cs) { beat_channels.erase(it); break; }
        }
        for (std::vector<MsgChannel *>::iterator it = channels.begin();
                it != channels.end(); ++it) {
            if (*it == cs) { channels.erase(it); break; }
        }
        delete cs;
        cs = small;   // confirms/wakes now go to the surviving worker
        {
            const Clock::time_point tb = Clock::now();
            bool gone = false;
            while (!gone && secs_since(tb) < 15) {
                wake_scheduler(port);
                gone = control(port, "listcs").find("creditcs (") == std::string::npos
                    && control(port, "listcs").find("creditcs(") == std::string::npos;
                if (!gone) { usleep(200 * 1000); }
            }
            check(gone, "the 40-slot worker was removed from the farm");
        }

        /* The 4th job, eligible for the surviving 4-slot worker's free
           slot.  Fresh clamp (slots=4, ceiling=3) must BLOCK it; a stale
           clamp (slots=44, ceiling=32) would dispatch it.  */
        check(send_request(sub, 803, "creditsmall"), "fourth job sent post-shrink");
        std::vector<unsigned> j4;
        const int got4 = drain_usecs(sub, 803, 803, &j4, 8, 1);
        printf("# clamp: 4th-job dispatches after shrink to 4 slots: %d (want 0)\n", got4);
        check(got4 == 0,
              "the reduced ceiling (slots-1=3) blocks the 4th job: the clamp"
              " saw the removal (a stale 44-slot aggregate would have"
              " dispatched it into the free 4th slot)");
    } else {
        return die("unknown mode");
    }

    /* Requested shutdown must be CLEAN: check liveness first (an
       already-dead scheduler must fail here, not vanish into cleanup),
       then require the expected exit.  */
    if (sched_pid > 0) {
        check(waitpid(sched_pid, nullptr, WNOHANG) == 0,
              "the scheduler is alive before the requested shutdown");
        sigset_t old;
        if (!block_handled(&old)) {
            mask_failure_abort("verdict/block");
        }
        const ShutdownResult sr = end_scheduler_checked(sched_pid);
        publish_sched(-1);
        if (!unblock_handled(&old)) {
            mask_failure_abort("verdict/restore");
        }
        check(sr == SHUTDOWN_CLEAN,
              "the requested shutdown exited cleanly (status 0, no escalation)");
    }
    cleanup();
    /* A mode that reached here without executing a single assertion is a
       broken test, not a passing one -- an empty branch (or a splice error
       that shadows the real branch) must never print PASS.  */
    if (failures) {
        printf("RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    if (checks_executed == 0) {
        printf("RESULT: FAIL (mode '%s' executed no assertions)\n", mode.c_str());
        return 1;
    }
    printf("# %s: %d assertions executed\n", mode.c_str(), checks_executed);
    printf("RESULT: PASS\n");
    unlink(logname);
    return 0;
}
