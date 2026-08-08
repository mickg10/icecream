/*
    Scheduler-level gate for the deferrable-transport change: a submitter
    that stops draining its socket must not keep receiving assignments.

    The channel unit test (backpressure) proves the deferrable send's
    ordering and deadline mechanics; this test proves the SCHEDULER uses
    them to bound damage.  Every observation here is protocol-44.

    Topology: one compile daemon whose environment PLATFORM is an ~8KB
    string -- that string rides inside every UseCS reply, so the clogged
    pipe (scheduler sndbuf 4096 via the preload shim + the submitter's
    small rcvbuf) backs up after one or two replies, far below the
    worker's capacity (max_kids 4 + preload 2 = 6).  The dispatch gate,
    not the worker limit, is therefore what stops a flood.

      1. GATE (time-discriminated): submitter C floods and never reads.
         The healthy submitter H's first completion lands within seconds,
         because the gate leaves worker slots free; ungated, the flood
         consumes the worker and nothing completes before the ~30s
         deadline teardown (measured 2.0s vs 30.0s).
      2. SCALE: 40,000 further requests join C's group.  Skipping a
         clogged submitter must be by GROUP, not one job at a time (the
         one-at-a-time walk re-finds its node linearly each step --
         quadratic in the backlog); H's completions must stay fast with
         the 40k backlog queued.
      3. ORDER: a second clogged submitter that resumes reading gets
         every reply, in exactly request order.
      4. DEADLINE: C never resumes; the scheduler disconnects it at the
         30s deferred-output deadline (the kernel TCP_USER_TIMEOUT is
         shim-stripped and heartbeats defeat the silence prune, so
         nothing else can be the closer -- and the log must NAME the
         deadline), with H unaffected throughout.

    Lifecycle: one cleanup owner tears down the scheduler child and every
    channel on every exit path, including SIGINT/SIGTERM; the child gets
    a parent-death signal on Linux; ports are probed and startup is
    retried boundedly; the scheduler runs under a unique per-run netname
    so it can never advertise itself as a default-farm scheduler.
*/

#include "../services/comm.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <stdint.h>
#include <chrono>
#include <string>
#include <vector>

typedef std::chrono::steady_clock Clock;
static double secs_since(Clock::time_point t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf(ok ? "ok       - %s\n" : "FAILED   - %s\n", what);
    if (!ok) {
        ++failures;
    }
    fflush(stdout);
}

/* ---- lifecycle: one owner for every exit path -------------------------- */

static pid_t sched_pid = -1;
/* The handler's view: volatile sig_atomic_t is the only object type with
   defined semantics for asynchronous handler reads.  Synchronized with
   sched_pid ONLY inside masked transitions.  Zero means "no child".  */
static volatile sig_atomic_t sig_sched_pid = 0;
static std::vector<MsgChannel *> channels;
static char logname[64];

/* Direct, unmasked last resort on a FAILED mask transition: proceeding
   would mutate ownership while claiming a protection that does not exist,
   so the test kills what the handler could see and refuses instead.  */
static void mask_failure_abort(const char *where)
{
    fprintf(stderr, "schedgate: %s: mask transition failed; aborting\n", where);
    const sig_atomic_t p = sig_sched_pid;
    if (p > 0) {
        kill((pid_t)p, SIGKILL);
    }
    _exit(2);
}

/* Publish both views; the pid-to-sig_atomic_t conversion is CHECKED (the
   standard does not guarantee every pid_t fits).  An unpublishable child
   is the caller's to kill and reap -- never half-owned.  */
static bool publish_sched(pid_t pid)
{
    if (pid > 0 && (uintmax_t)pid > (uintmax_t)SIG_ATOMIC_MAX) {
        return false;
    }
    sched_pid = pid;
    sig_sched_pid = pid > 0 ? (sig_atomic_t)pid : 0;
    return true;
}

/* The single scheduler-reap state machine, used by BOTH the startup retry
   path and final cleanup.  Terminal states -- an observed reap or ECHILD
   (no owned child left) -- never flow into a later kill.  */
static void end_scheduler(pid_t pid)
{
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        const pid_t r = waitpid(pid, nullptr, WNOHANG);
        if (r == pid) {
            return;                       // reaped: terminal
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != ECHILD) {
                /* Unexpected: the child may be alive; do not let the caller
                   clear ownership on a guess.  */
                perror("waitpid");
                mask_failure_abort("end_scheduler/waitpid");
            }
            return;                       // ECHILD: not ours, terminal
        }
        usleep(100 * 1000);
    }
    kill(pid, SIGKILL);
    for (;;) {
        const pid_t r = waitpid(pid, nullptr, 0);
        if (r == pid || (r < 0 && errno == ECHILD)) {
            return;
        }
        if (r < 0 && errno != EINTR) {
            perror("waitpid");
            mask_failure_abort("end_scheduler/waitpid2");
        }
    }
}

/* A failed mask change must NOT proceed under the pretense of
   protection.  */
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

static void cleanup(void)
{
    for (MsgChannel *ch : channels) {
        delete ch;
    }
    channels.clear();
    /* The reap-to-clear transition happens with the handled signals
       blocked, so the handler can never see a reaped-but-uncleared pid.  */
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

/* Async-signal-safe by construction: kill(2) on the known-positive child
   and _exit(2), nothing else.  The full cleanup routine allocates and
   deletes C++ objects and must never run from a handler; what must not
   leak on this path is the scheduler child, and killing it is safe.  */
static void on_signal(int)
{
    const sig_atomic_t p = sig_sched_pid;
    if (p > 0) {
        kill((pid_t)p, SIGKILL);
    }
    _exit(2);
}

static int die(const char *msg)
{
    fprintf(stderr, "schedgate: %s\n", msg);
    cleanup();
    return 2;
}

/* ---- plumbing ----------------------------------------------------------- */

static std::string big_platform()
{
    std::string p = "x86_64-gatetest-";
    while (p.size() < 8000) {
        p += "0123456789abcdef";
    }
    return p;
}

static int tcp_connect(int port, int rcvbuf)
{
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (rcvbuf > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
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

static MsgChannel *connect_daemon(int port, int rcvbuf)
{
    int fd = tcp_connect(port, rcvbuf);
    if (fd < 0) {
        return nullptr;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    return track(Service::createChannel(fd, (struct sockaddr *)&sa, sizeof(sa)));
}

static MsgChannel *login_submitter(int port, const char *name, int rcvbuf)
{
    MsgChannel *ch = connect_daemon(port, rcvbuf);
    if (!ch) {
        return nullptr;
    }
    LoginMsg login(0, name, big_platform(), 0);
    login.envs.push_back(std::make_pair(big_platform(), std::string("gate-env")));
    login.max_kids = 0;
    login.noremote = true;
    login.chroot_possible = false;
    if (!ch->send_msg(login)) {
        return nullptr;
    }
    return ch;
}

static bool send_request(MsgChannel *ch, unsigned cid, const std::string &platform,
                         int niceness = 0)
{
    char fname[48];
    snprintf(fname, sizeof(fname), "gate%u.cpp", cid);
    GetCSMsg g(Environments{std::make_pair(platform, std::string("gate-env"))},
               fname, CompileJob::Lang_CXX, 1, platform, 0, std::string(), 0,
               0, niceness);
    g.client_id = cid;
    return ch->send_msg(g);
}

/* How many times the scheduler has logged a deferral so far: the explicit
   state barrier the timing phases wait on, so "the clog armed" is observed
   rather than assumed from a sleep.  */
static int deferring_count(void)
{
    FILE *lf = fopen(logname, "r");
    if (!lf) {
        return -1;
    }
    int n = 0;
    char line[4096];
    while (fgets(line, sizeof(line), lf)) {
        if (strstr(line, "deferring")) {
            ++n;
        }
    }
    fclose(lf);
    return n;
}

/* Every fake daemon that must outlive a wait registers here; heartbeat()
   is woven into ALL wait loops so no phase can silently cross the
   scheduler's 36s silence prune.  (A 30s log-poll without beats once
   pruned every daemon mid-phase; the per-peer skip deltas -- 0, 0, 28 --
   were the giveaway.)  */
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

static bool log_contains(const char *needle)
{
    FILE *lf = fopen(logname, "r");
    if (!lf) {
        return false;
    }
    bool found = false;
    char line[8192];
    while (fgets(line, sizeof(line), lf)) {
        if (strstr(line, needle)) {
            found = true;
            break;
        }
    }
    fclose(lf);
    return found;
}

static bool wait_log_contains(const char *needle, int timeout_sec)
{
    const Clock::time_point t0 = Clock::now();
    while (secs_since(t0) < timeout_sec) {
        if (log_contains(needle)) {
            return true;
        }
        heartbeat();
        usleep(200 * 1000);
    }
    return false;
}

static bool wait_deferring_above(int floor, int timeout_sec)
{
    const Clock::time_point t0 = Clock::now();
    while (secs_since(t0) < timeout_sec) {
        if (deferring_count() > floor) {
            return true;
        }
        heartbeat();
        usleep(200 * 1000);
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <icecc-scheduler> <sndbuf_shim.so>\n", argv[0]);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    snprintf(logname, sizeof(logname), "schedgate-%d.log", (int)getpid());
    char netname[48];
    snprintf(netname, sizeof(netname), "gatetest%d", (int)getpid());

    /* Bounded allocation+startup attempts: probe a pair, spawn, classify.
       A child exit is a lost race (or exec failure, which no retry can
       help); a silent child is reaped and the next pair tried.  */
    int port = 0;
    {
        int cand = 26500 + (getpid() % 2000);
        for (int attempt = 0; attempt < 5 && port == 0; ++attempt) {
            int chosen = 0;
            for (; cand < 26500 + 4000; cand += 2) {
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
                return die("cannot mask signals for fork");
            }
            const pid_t child = fork();
            if (child == 0) {
                if (!unblock_handled(&oldmask)) {
                    _exit(8);   // never exec with the mask in an unknown state
                }
                signal(SIGPIPE, SIG_DFL);
#ifdef __linux__
                prctl(PR_SET_PDEATHSIG, SIGKILL);
                /* Close the fork-to-prctl window: if the parent died in
                   between, the death signal was never armed for it.  */
                if (getppid() != parent_before) {
                    _exit(1);
                }
#endif
                setenv("LD_PRELOAD", argv[2], 1);
                setenv("ICECC_TEST_SNDBUF", "4096", 1);
                /* The kernel's 9s TCP_USER_TIMEOUT would declare our
                   full-stop stand-in dead long before the 30s application
                   deadline; the production failure is a SLOWLY draining
                   peer whose ACKs keep resetting that kernel timer, so the
                   shim strips the option to reach the same application
                   path.  */
                setenv("ICECC_TEST_STRIP_USER_TIMEOUT", "1", 1);
                FILE *lf = fopen(logname, "w");
                if (lf) {
                    dup2(fileno(lf), 1);
                    dup2(fileno(lf), 2);
                }
                char portbuf[16];
                snprintf(portbuf, sizeof(portbuf), "%d", chosen);
                execl(argv[1], argv[1], "-p", portbuf, "-n", netname, "-vvv",
                      (char *)nullptr);
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
                /* The WNOHANG observation REAPS on success, so it and the
                   clear are one masked transition: neither the handler nor
                   a later die()->cleanup() can ever kill a reaped pid.  */
                int status = 0;
                sigset_t om;
                if (!block_handled(&om)) {
                    mask_failure_abort("classify/block");
                }
                pid_t r;
                for (;;) {
                    r = waitpid(child, &status, WNOHANG);
                    if (r >= 0 || errno != EINTR) {
                        break;             // 0 = still running; child = reaped
                    }
                }
                if (r == child) {
                    publish_sched(-1);     // reaped: cleared under the mask
                } else if (r < 0) {
                    if (errno != ECHILD) {
                        /* An unexpected error is NOT evidence the child is
                           gone; discarding ownership here could leak a live
                           scheduler.  Abort by name, which kills the
                           published child.  */
                        perror("waitpid");
                        mask_failure_abort("classify/waitpid");
                    }
                    publish_sched(-1);     // ECHILD: nothing of ours remains
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
                const int probe = tcp_connect(chosen, 0);
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
                    mask_failure_abort("retry-teardown/block");
                }
                if (!exited) {
                    end_scheduler(child);
                }
                publish_sched(-1);
                if (!unblock_handled(&old2)) {
                    mask_failure_abort("retry-teardown/restore");
                }
            }
        }
        if (port == 0 || sched_pid < 0) {
            return die("scheduler could not be started after retries");
        }
    }

    /* The compile daemon: capacity 6 (max_kids 4 + preload 2), above the
       one-or-two 8KB replies the clogged pipe holds.  */
    MsgChannel *cs = connect_daemon(port, 0);
    if (!cs) {
        return die("compile daemon connect failed");
    }
    {
        LoginMsg login(10245, "gatecs", big_platform(), 0);
        login.envs.push_back(std::make_pair(big_platform(), std::string("gate-env")));
        login.max_kids = 4;
        login.noremote = false;
        login.chroot_possible = true;
        if (!cs->send_msg(login)) {
            return die("compile daemon login failed");
        }
    }

    /* Heartbeats defeat the 36s silence prune; a heartbeating C also makes
       the deadline attribution exact (it TALKS but never reads).  */
    MsgChannel *C = nullptr;
    MsgChannel *cs2 = nullptr;
    auto owner_for = [&](unsigned daemon_port) -> MsgChannel * {
        return (daemon_port == 10246 && cs2) ? cs2 : cs;
    };
    beat_channels.push_back(cs);
    /* The scheduler re-arms its listener at most once per second and only
       when its poll wakes; traffic on the daemon channel is the wake.  */
    auto wake_listener = [&]() {
        sleep(1);
        StatsMsg st;
        cs->send_msg(st);
        usleep(300 * 1000);
    };

    wake_listener();
    MsgChannel *H = login_submitter(port, "gatehealthy", 0);
    if (!H) {
        return die("healthy submitter connect/login failed");
    }
    wake_listener();
    C = login_submitter(port, "gateclogged", 4096);
    if (!C) {
        return die("clogged submitter connect/login failed");
    }
    beat_channels.push_back(C);
    wake_listener();

    auto h_completes_one = [&](unsigned cid, double secs, int niceness = 0) -> bool {
        if (!send_request(H, cid, big_platform(), niceness)) {
            return false;
        }
        const Clock::time_point t0 = Clock::now();
        while (secs_since(t0) < secs) {
            heartbeat();
            Msg *m = H->get_msg(1);
            if (!m) {
                continue;
            }
            unsigned jid = 0, jport = 0;
            if (*m == Msg::USE_CS) {
                UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                if (u && u->client_id == cid) {
                    jid = u->job_id;
                    jport = u->port;
                }
            }
            delete m;
            if (jid) {
                /* Begin/Done must come from the daemon the job was
                   assigned to, or the scheduler kicks the impostor.  */
                MsgChannel *owner = owner_for(jport);
                JobBeginMsg jb(jid, 1);
                if (!owner->send_msg(jb)) {
                    return false;
                }
                JobDoneMsg jd(jid, 0, JobDoneMsg::FROM_SERVER);
                return owner->send_msg(jd);
            }
        }
        return false;
    };

    check(h_completes_one(100, 30), "healthy submitter completes a job before any clog");

    /* ---- 1. the dispatch gate, time-discriminated ---------------------- */
    const int defer_floor = deferring_count();
    const unsigned FLOOD = 12;
    const Clock::time_point t_flood = Clock::now();
    for (unsigned i = 0; i < FLOOD; ++i) {
        if (!send_request(C, 200 + i, big_platform())) {
            return die("flood request send failed");
        }
    }
    /* Explicit state barrier: the clog has ARMED (observed, not slept
       for) before the timing window opens.  */
    check(wait_deferring_above(defer_floor, 15),
          "the flood armed deferred output (the state the gate acts on)");

    double first_completion = -1;
    int h_completed = 0;
    for (unsigned i = 0; i < 5; ++i) {
        if (h_completes_one(300 + i, 60)) {
            ++h_completed;
            if (first_completion < 0) {
                first_completion = secs_since(t_flood);
            }
        }
    }
    printf("# schedgate: healthy completions %d/5, first at %.1fs after the flood\n",
           h_completed, first_completion);
    check(h_completed == 5, "the healthy submitter completes all five requests");
    check(first_completion >= 0 && first_completion < 20.0,
          "the first completion lands DURING the clog (a worker slot stayed free"
          " because the flood was gated at the pipe; without the dispatch gate the"
          " worker is consumed and nothing completes before the ~30s teardown)");

    /* ---- 2. scale: the skip must be by group, not by job --------------- */
    {
        const std::string small = "x86_64-gatetest-small";
        for (unsigned i = 0; i < 40000; ++i) {
            if (!send_request(C, 10000 + i, small)) {
                return die("bulk request send failed");
            }
            if ((i % 2000) == 0) {
                heartbeat();   // a slow host must not cross the silence prune mid-flood
            }
        }
        /* Admission barrier: a returned send_msg() proves only that the
           frames left the test; the timing window must not begin until
           the scheduler has PARSED and QUEUED them all.  Requests on one
           channel are ordered, so the final request's NEW line in the
           scheduler's own log proves every earlier one was admitted.  */
        check(wait_log_contains("gate49999.cpp", 60),
              "all 40000 requests were admitted before the timing window");
        const Clock::time_point t_bulk = Clock::now();
        const bool done = h_completes_one(350, 60);
        const double took = secs_since(t_bulk);
        printf("# schedgate: healthy completion with a 40000-request clogged"
               " backlog took %.1fs\n", took);
        check(done && took < 15.0,
              "a 40000-request clogged backlog does not delay healthy work:"
              " the gate skips the GROUP (one-at-a-time skipping re-finds its"
              " node linearly per step -- quadratic -- and fails this bound)");
    }

    /* ---- 2b. many pending GROUPS ----------------------------------------
       A group is keyed by (submitter, niceness), so several clogged
       submitters at several niceness levels put a RUN of pending-output
       groups ahead of healthy work.  The position carries its group
       iterator, so crossing the run is one step per group; this phase
       covers that path (today's other phases never queue more than one
       pending group).  */
    {
        /* This phase adds ~6 pre-gate dispatches (each extra submitter
           receives one or two replies before its pipe fills and the gate
           arms).  The 6-reservation first worker would be consumed by
           those alone, so a second, wide worker joins first -- the phase
           measures the GROUP WALK, not worker capacity.  */
        wake_listener();
        cs2 = connect_daemon(port, 0);
        if (!cs2) {
            return die("second worker connect failed");
        }
        {
            LoginMsg login(10246, "gatecs2", big_platform(), 0);
            login.envs.push_back(std::make_pair(big_platform(), std::string("gate-env")));
            login.max_kids = 40;
            login.noremote = false;
            login.chroot_possible = true;
            if (!cs2->send_msg(login)) {
                return die("second worker login failed");
            }
            /* A fresh daemon reports load 1000 (overloaded) until its
               first stats; without this it is ineligible and every
               pre-gate dispatch lands on the small first worker.  */
            StatsMsg st;
            if (!cs2->send_msg(st)) {
                return die("second worker stats failed");
            }
        }
        beat_channels.push_back(cs2);
        MsgChannel *extra[3];
        for (int e = 0; e < 3; ++e) {
            wake_listener();
            char nm[32];
            snprintf(nm, sizeof(nm), "gatemany%d", e);
            extra[e] = login_submitter(port, nm, 4096);
            if (!extra[e]) {
                return die("many-group submitter connect/login failed");
            }
            beat_channels.push_back(extra[e]);
        }
        /* PRIME each peer into pending output FIRST, with a sacrificial
           niceness-8 group: its first request dispatches, its ~8KB reply
           jams the tiny pipe, and pending arms; its second request then
           sits queued so the gate's named skip trace can prove the state.
           Only AFTER all three peers are provably pending are the
           measured groups enqueued -- the gate then blocks every dispatch
           from these peers, so no measured group can lose a member and
           all 21 are still live when the healthy walk begins.  Priming at
           niceness 8 also keeps the sacrificial groups BEHIND the
           niceness-7 healthy request: they are never part of the measured
           walk.  */
        for (int e = 0; e < 3; ++e) {
            /* FOUR primers: ~33KB of replies against ~16KB of combined
               socket buffering, so the jam cannot fully drain into the
               peer's buffers and silently clear pending (two primers
               could -- observed as one peer pending and two spotless).
               The undispatched remainder also keeps a queued group alive
               for the named skip evidence below.  */
            for (int k = 0; k < 4; ++k) {
                if (!send_request(extra[e], 59000 + e * 10 + k, big_platform(), 8)) {
                    return die("primer request send failed");
                }
            }
        }
        bool all_pending = true;
        for (int e = 0; e < 3 && all_pending; ++e) {
            char needle[64];
            snprintf(needle, sizeof(needle),
                     "pending output on gatemany%d, skipping", e);
            all_pending = wait_log_contains(needle, 30);
        }
        check(all_pending,
              "every extra submitter is provably PENDING before the measured"
              " groups exist (named skip evidence, per peer)");

        for (int e = 0; e < 3; ++e) {
            for (int n = 0; n < 7; ++n) {
                /* two requests per (submitter, niceness) group */
                for (int k = 0; k < 2; ++k) {
                    if (!send_request(extra[e], 60000 + e * 100 + n * 10 + k,
                                      big_platform(), n)) {
                        return die("many-group request send failed");
                    }
                }
            }
        }
        /* Per CHANNEL: the last request on each of the three independent
           connections must be ADMITTED (ordering holds per channel, so
           each final marker proves all of that channel's frames were
           parsed).  */
        bool all_admitted = true;
        for (int e = 0; e < 3 && all_admitted; ++e) {
            char marker[32];
            snprintf(marker, sizeof(marker), "gate%d.cpp", 60000 + e * 100 + 61);
            all_admitted = wait_log_contains(marker, 30);
        }
        check(all_admitted, "every extra submitter's final request was admitted");

        /* Phase-local floor, captured AFTER every admission barrier and
           immediately before the healthy request: each measured-request
           admission can itself trigger a queue walk and add skip records,
           so a floor taken any earlier would let those walks satisfy the
           assertion below without the healthy-triggered walk contributing
           anything.  */
        int skip_floor[3];
        for (int e = 0; e < 3; ++e) {
            char needle[64];
            snprintf(needle, sizeof(needle),
                     "pending output on gatemany%d, skipping", e);
            skip_floor[e] = 0;
            FILE *lf = fopen(logname, "r");
            if (lf) {
                char line[8192];
                while (fgets(line, sizeof(line), lf)) {
                    if (strstr(line, needle)) {
                        ++skip_floor[e];
                    }
                }
                fclose(lf);
            }
        }

        /* The healthy request runs at NICENESS 7: groups sort by ascending
           niceness, so it sits BEHIND all 21 extra groups (0-6) and the
           walk must cross every one of them.  */
        const Clock::time_point t_mg = Clock::now();
        const bool done = h_completes_one(360, 60, 7);
        const double took = secs_since(t_mg);
        printf("# schedgate: healthy completion behind 21 pending-output groups"
               " took %.1fs\n", took);
        check(done && took < 10.0,
              "a run of pending-output groups (3 submitters x 7 niceness"
              " levels) is crossed group-by-group and does not delay healthy"
              " work");
        /* Evidence the walk really crossed all SEVEN measured groups of
           each peer: one named skip per group per walk, counted
           phase-locally (the floor excludes the priming skips).  A peer
           whose measured groups had collapsed to fewer members would show
           fewer than seven new skips.  */
        bool counted_all = true;
        for (int e = 0; e < 3; ++e) {
            char needle[64];
            snprintf(needle, sizeof(needle),
                     "pending output on gatemany%d, skipping", e);
            int now = 0;
            FILE *lf = fopen(logname, "r");
            if (lf) {
                char line[8192];
                while (fgets(line, sizeof(line), lf)) {
                    if (strstr(line, needle)) {
                        ++now;
                    }
                }
                fclose(lf);
            }
            const int delta = now - skip_floor[e];
            printf("# schedgate: measured-walk skips for gatemany%d: %d\n", e, delta);
            if (delta < 7) {
                counted_all = false;
            }
        }
        check(counted_all,
              "the healthy walk crossed at least SEVEN live groups per peer"
              " (all 21 measured groups existed and were skipped)");
    }

    /* ---- 3. drain order ------------------------------------------------- */
    {
        wake_listener();
        MsgChannel *C2 = login_submitter(port, "gateorder", 4096);
        if (!C2) {
            return die("order submitter connect/login failed");
        }
        const int c2_floor = deferring_count();
        for (unsigned i = 0; i < 10; ++i) {
            if (!send_request(C2, 400 + i, big_platform())) {
                return die("order request send failed");
            }
        }
        /* Barrier: C2 really entered deferral before it starts reading, so
           the drain exercises resume-from-pending, not plain buffering.  */
        check(wait_deferring_above(c2_floor, 15),
              "the order flood armed deferred output before the drain");
        std::vector<unsigned> got;
        const Clock::time_point t0 = Clock::now();
        while (got.size() < 10 && secs_since(t0) < 60) {
            heartbeat();
            Msg *m = C2->get_msg(1);
            if (!m) {
                continue;
            }
            if (*m == Msg::USE_CS) {
                UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                if (u && u->client_id >= 400 && u->client_id < 410) {
                    got.push_back(u->client_id);
                    MsgChannel *owner = owner_for(u->port);
                    JobBeginMsg jb(u->job_id, 1);
                    owner->send_msg(jb);
                    JobDoneMsg jd(u->job_id, 0, JobDoneMsg::FROM_SERVER);
                    owner->send_msg(jd);
                }
            }
            delete m;
        }
        bool ordered = got.size() == 10;
        for (size_t i = 0; ordered && i < got.size(); ++i) {
            ordered = got[i] == 400 + i;
        }
        printf("# schedgate: order drain got %zu/10 replies\n", got.size());
        check(ordered, "a resumed submitter receives EVERY reply, in request order");
    }

    /* ---- 4. the deadline ------------------------------------------------ */
    {
        bool c_dead = false;
        const Clock::time_point t0 = Clock::now();
        while (!c_dead && secs_since(t0) < 60) {
            heartbeat();
            StatsMsg st;
            if (!C->send_msg(st)) {
                c_dead = true;
                break;
            }
            sleep(1);
        }
        check(c_dead,
              "the scheduler disconnects a submitter that never drains at the"
              " deferred-output deadline");
        bool deadline_named = false;
        FILE *lf = fopen(logname, "r");
        if (lf) {
            char line[4096];
            while (fgets(line, sizeof(line), lf)) {
                if (strstr(line, "did not drain its socket")) {
                    deadline_named = true;
                }
            }
            fclose(lf);
        }
        check(deadline_named,
              "and its log names the deferred-output deadline as the cause");
    }
    check(h_completes_one(500, 30), "the healthy submitter is unaffected after the teardown");

    cleanup();
    if (failures) {
        printf("RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
