/*
    Integration red/green test for scheduler behaviour when a submitter
    daemon stops draining its socket (issue #1: "Scheduler nukes entire
    submitter on transient send timeout").

    Starts a real icecc-scheduler, logs in a fake compile server and a fake
    submitter daemon using the project's own MsgChannel protocol code, has
    the submitter request a couple of thousand jobs and then stop reading
    its socket for longer than the 30s send timeout -- exactly what a build
    machine wedged under a highly parallel build does -- then drains
    everything and checks the outcome.

    The conditions of the issue report are recreated deliberately via the
    preloaded sndbuf_shim.so:

      - ICECC_TEST_SNDBUF shrinks the scheduler's per-connection send
        buffer so a few thousand dispatch replies actually jam the
        connection (with modern autotuned buffers, megabytes of kernel
        buffering would otherwise absorb everything).

      - ICECC_TEST_STRIP_USER_TIMEOUT disables the 9s TCP_USER_TIMEOUT that
        MsgChannel arms on every TCP channel (present in every release since
        2020, including the issue report's 1.4.90).  The production failure
        involves a SLOWLY-DRAINING submitter: it keeps ACKing and freeing
        dribbles of buffer, which resets the kernel timer (TCP_USER_TIMEOUT
        requires zero forward progress) while never freeing enough space
        within 30s -- so the application timeout governs even with the
        option armed.  This harness uses a full stop as a deterministic
        stand-in for that slow drain, and a full stop WOULD trip the kernel
        timer at ~9s on every variant alike, masking the application-level
        behaviour under test; stripping the option isolates that behaviour.

    Note the submitter's SO_RCVBUF is set to 64KiB, not smaller: on Linux
    loopback the MSS is ~64KiB, and a zero-window connection whose receive
    buffer is below one MSS never reopens its window (silly window syndrome
    avoidance), which would freeze the final drain for every variant alike.

    The desired contract, identical for every scheduler variant under test:

      1. every requested job gets exactly one dispatch reply (USE_CS),
         parseable and intact, once the submitter drains;
      2. the submitter connection survives the backpressure window;
      3. the compile server connection survives;
      4. the scheduler stays responsive throughout: a pre-established
         control connection on the text port gets a reply to "listcs"
         within a few seconds at all times.

    Usage:
      schedbp <icecc-scheduler> <sndbuf_shim.so> [jobs] [clog-seconds] [stall]

    The optional "stall" mode inverts the scenario: the submitter never
    drains at all, and the harness asserts the scheduler enforces its
    application-level bound on deferred dispatch output (~30s) by tearing the
    stalled submitter down -- neither instantly (which would mean a kernel
    timeout leaked in) nor never (which would mean undelivered bytes and
    WAITINGFORCS jobs can linger forever).  Because a run where the send
    buffers were never really shrunk cannot jam and therefore cannot trigger
    the teardown, stall mode also serves as the positive proof that the
    backpressure scenario actually engages (the normal mode cannot assert
    that from its side of the sockets).

    Exit code 0 if the contract holds, 1 otherwise.
*/

#include "comm.h"
#include "logging.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

using Clock = std::chrono::steady_clock;

// Compatibility with the 1.4-era Msg API (pre icecc/icecream#610:
// `enum MsgType Msg::type` with M_* constants; this tree uses the Msg::Value
// comparison operators).  Lets the harness compile against a 1.4-era
// libicecc so daemons of either protocol generation can be pointed at any
// scheduler build for cross-version testing.  The auto-detect keys on a
// macro specific to this fork's lineage, so it only distinguishes these
// trees from 1.4-era ones; compiling against a pristine upstream master
// (new Msg API, no such macro) would need the first branch forced.
#ifdef PROTOCOL_VERSION_JOB_TIMING
#define MSG_IS(m, what) (*(m) == Msg::what)
#else
#define MSG_IS(m, what) ((m)->type == M_##what)
#endif

static double secs_since(Clock::time_point t0)
{
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

static int failures = 0;

#define REQUIRE(cond, what)                                             \
    do {                                                                \
        if (cond) {                                                     \
            fprintf(stderr, "ok       - %s\n", what);                   \
        } else {                                                        \
            fprintf(stderr, "FAILED   - %s\n", what);                   \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static const char *kPlatform = "x86_64";
static const char *kEnv = "testenv";
static const unsigned int kCsPort = 10245;

static pid_t start_scheduler(const std::string &binary, const std::string &shim,
                             int port, const std::string &logfile,
                             const char *verbosity, const char *extra_arg = nullptr)
{
    pid_t pid = fork();
    if (pid != 0) {
        return pid;    // -1 (fork failure) is handled by the caller
    }

    // child: scheduler under issue-report conditions (see file comment).
    // Undo the parent's SIGPIPE ignore first: SIG_IGN survives execl, and the
    // scheduler must run with the disposition it would have in production.
    signal(SIGPIPE, SIG_DFL);
    setenv("LD_PRELOAD", shim.c_str(), 1);
    setenv("ICECC_TEST_SNDBUF", "4096", 1);
    setenv("ICECC_TEST_STRIP_USER_TIMEOUT", "1", 1);
    FILE *lf = fopen(logfile.c_str(), "w");
    if (lf) {
        dup2(fileno(lf), 1);
        dup2(fileno(lf), 2);
    }
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    if (extra_arg) {
        execl(binary.c_str(), binary.c_str(), "-p", portbuf, verbosity, extra_arg, (char *)nullptr);
    } else {
        execl(binary.c_str(), binary.c_str(), "-p", portbuf, verbosity, (char *)nullptr);
    }
    perror("execl icecc-scheduler");
    _exit(127);
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

/* One control-port round trip returning the scheduler's lifetime
   jobs_admitted counter, or -1 if it cannot be read.  Used to baseline and
   then observe the flood's admission from the server's own accounting.  */
/* Per-submitter admitted total from listcs ("admitted_total=N" on the line
   whose node name matches).  Scoping the barrier to the FLOOD submitter is
   what makes it honest: the global counter includes the healthy
   submitter's concurrent traffic, which inflated every delta by ~4.  */
static long long query_submitter_field(int port, const char *name, const char *field);

static long long query_submitter_admitted(int port, const char *name)
{
    return query_submitter_field(port, name, "admitted_total=");
}

/* Connection stamp for the same line: admitted_total restarts with each
   connection object, so a baseline/delta pair is only meaningful while the
   generation is unchanged.  Every baseline below pairs with one of these.  */
static long long query_submitter_generation(int port, const char *name)
{
    return query_submitter_field(port, name, "gen=");
}

static long long query_submitter_outstanding(int port, const char *name)
{
    return query_submitter_field(port, name, "outstanding=");
}

static long long query_submitter_field(int port, const char *name, const char *field)
{
    const int fd = tcp_connect(port + 1, 0);
    if (fd < 0) {
        return -1;
    }
    char buf[16384];
    struct pollfd pfd = { fd, POLLIN, 0 };
    if (poll(&pfd, 1, 5000) > 0) {
        ssize_t n = read(fd, buf, sizeof(buf));
        (void)n;
    }
    long long admitted = -1;
    if (write(fd, "listcs\n", 7) == 7) {
        std::string reply;
        const Clock::time_point t0 = Clock::now();
        while (secs_since(t0) < 5) {
            struct pollfd rp = { fd, POLLIN, 0 };
            if (poll(&rp, 1, 200) <= 0) {
                continue;
            }
            const ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                break;
            }
            buf[n] = 0;
            reply += buf;
            if (reply.find("200 done") != std::string::npos) {
                break;
            }
        }
        /* Find the line for THIS node.  The match must be delimiter-aware:
           listcs prints " <node> (<ip>:<port>) ...", and "fakesub" is a
           prefix of "fakesub2", so a bare substring search reads the wrong
           submitter's counter -- which is exactly the mistake that made the
           global barrier dishonest in the first place.  */
        const std::string needle = std::string(" ") + name + " (";
        size_t pos = 0;
        while ((pos = reply.find(needle, pos)) != std::string::npos) {
            const size_t eol = reply.find('\n', pos);
            const std::string line = reply.substr(pos, eol == std::string::npos
                                                       ? std::string::npos : eol - pos);
            const size_t sp = line.find(field);
            if (sp != std::string::npos) {
                admitted = atoll(line.c_str() + sp + strlen(field));
                break;
            }
            pos = (eol == std::string::npos) ? reply.size() : eol;
        }
    }
    close(fd);
    return admitted;
}

static long long query_jobs_admitted(int port)
{
    const int fd = tcp_connect(port + 1, 0);
    if (fd < 0) {
        return -1;
    }
    char buf[8192];
    struct pollfd pfd = { fd, POLLIN, 0 };
    if (poll(&pfd, 1, 5000) > 0) {          // absorb the greeting
        ssize_t n = read(fd, buf, sizeof(buf));
        (void)n;
    }
    long long admitted = -1;
    if (write(fd, "estimates\n", 10) == 10) {
        std::string reply;
        const Clock::time_point t0 = Clock::now();
        while (secs_since(t0) < 5) {
            struct pollfd rp = { fd, POLLIN, 0 };
            if (poll(&rp, 1, 200) <= 0) {
                continue;      // scheduler busy; keep waiting within the 5s
            }
            const ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                break;
            }
            buf[n] = 0;
            reply += buf;
            const size_t pos = reply.find("jobs_admitted=");
            if (pos != std::string::npos) {
                admitted = atoll(reply.c_str() + pos + strlen("jobs_admitted="));
                break;
            }
        }
    }
    close(fd);
    return admitted;
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
    return Service::createChannel(fd, (struct sockaddr *)&sa, sizeof(sa));
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <icecc-scheduler> <sndbuf_shim.so> [jobs] [clog-seconds] [mode] [farm-slots]\n",
                argv[0]);
        return 2;
    }
    const std::string scheduler_bin = argv[1];
    const std::string shim = argv[2];
    int njobs = argc > 3 ? atoi(argv[3]) : 4000;
    const bool stall_mode = argc > 5 && strcmp(argv[5], "stall") == 0;
    /* "expect-deferral": assert the run actually engaged backpressure, so a
       quick test cannot silently stop governing the admission gate if buffer
       sizes change.  */
    /* "gate": fast deterministic assertion of the BP-1 admission invariant
       for every-commit runs -- while a submitter stops reading, the
       scheduler must stop granting it assignments (bounded by the dispatch
       credit) while other submitters keep being served.  It does not wait
       for any teardown deadline.  */
    const bool gate_mode = argc > 5 && strcmp(argv[5], "gate") == 0;
    /* "perf": the measurement mode for perfgate.sh.  Identical workload, but
       the scheduler runs at production verbosity -- at -vvv the trace stream
       (a flushed write per line, several lines per job) dominates the
       profile (~87%% of scheduler time at depth 20k) and its writeback
       stalls produce multi-second latency spikes that say nothing about the
       dispatch path being measured.  The log-parsing assertions are skipped;
       reply integrity, responsiveness bounds and the phase distributions
       remain.  */
    const bool perf_mode = argc > 5 && strcmp(argv[5], "perf") == 0;
    /* "multicount": one GetCS asking for N jobs must yield exactly N
       replies.  The scheduler materialises at most a bounded number per
       main-loop iteration and resumes the remainder on later iterations;
       dropping the tail would silently break the wire contract (count is
       the number of replies the caller waits for).  njobs is the count.  */
    const bool multicount_mode = argc > 5 && strcmp(argv[5], "multicount") == 0;
    /* "contract": the general request contract beyond single count=N --
       per-connection FIFO of queued requests, count=0 semantics, concurrent
       multi-daemon expansion, disconnect/fd-reuse hygiene, and sibling-chain
       stability across resume steps.  njobs is the primary count and should
       exceed the per-step bound (64) so expansion actually resumes.  */
    const bool contract_mode = argc > 5 && strcmp(argv[5], "contract") == 0;
    /* "promotion": the SCH-1 hard-promotion gate.  A queued request older
       than the 60s bound must be dispatched ahead of a fresh request whose
       score is numerically far superior.  Runs at the PRODUCTION bound (no
       test-only knob), so the mode takes ~70s of wall time by design.  */
    const bool promotion_mode = argc > 5 && strcmp(argv[5], "promotion") == 0;
    /* "heterogeneous": the SCH-3 circular-traversal gate.  With the scored
       head unservable NOW (its only capable farm host is at capacity), a
       compatible job for a DIFFERENT platform must still be dispatched --
       the selection walk has to continue past the head, circularly.  */
    const bool heterogeneous_mode = argc > 5 && strcmp(argv[5], "heterogeneous") == 0;
    /* "stallevict": the BP-1 liveness bound.  With --dispatch-stall-timeout
       at its 10s floor, a submitter whose dispatched jobs never reach
       JobBegin is evicted at the bound -- not before it, and not never --
       while a healthy submitter keeps being served throughout.  */
    const bool stallevict_mode = argc > 5 && strcmp(argv[5], "stallevict") == 0;
    /* "leastbusy": the SCH-6 selection gate, run with -a least_busy.  With
       every host in its preload zone (count == maxJobs) the picker must
       still assign work -- the two-pass bucketed form selected an empty set
       and answered "no suitable host" while preload capacity existed -- and
       with unequal occupancies the emptier host must win.  */
    const bool leastbusy_mode = argc > 5 && strcmp(argv[5], "leastbusy") == 0;
    /* "mixedrole": the realistic topology.  The submitter also has compile
       capacity, so the scheduler can place work back on it (NoCS / local
       UseCS).  Local decisions reserve no farm slot and must NOT consume
       remote dispatch credit -- charging them let an ordinary developer
       machine gate itself out of a live farm.  With no remote capacity
       available, every decision is local, and outstanding_dispatches must
       stay 0 however many are made.  */
    const bool mixedrole_mode = argc > 5 && strcmp(argv[5], "mixedrole") == 0;
    /* Optional farm size (argv[6], gate mode): the fake compile server
       advertises exactly this many slots instead of "always enough".  The
       scheduler then clamps the per-submitter dispatch credit to slots-1,
       and the gate asserts THAT bound -- the small-farm case where a full
       credit of 32 could otherwise reserve the whole farm.  */
    const int farm_slots = argc > 6 ? atoi(argv[6]) : 0;
    if (farm_slots < 0 || farm_slots > 100000) {
        fprintf(stderr, "implausible farm_slots argument\n");
        return 2;
    }
    // Normal mode must stay under the scheduler's 30s deferred-send bound
    // (measured from jam onset, ~1-2s into the clog): it asserts that a
    // TRANSIENT stall loses nothing.  Stall mode goes well past the bound
    // to assert the bound itself.
    int clog_s = argc > 4 ? atoi(argv[4]) : (stall_mode ? 75 : 25);
    if (njobs < 1 || njobs > 100000 || clog_s < 1 || clog_s > 600) {
        fprintf(stderr, "implausible jobs/clog arguments\n");
        return 2;
    }
    const int port = 25000 + (getpid() % 1000);

    signal(SIGPIPE, SIG_IGN);

    fprintf(stderr, "# port=%d jobs=%d clog=%ds\n", port, njobs, clog_s);
    /* Per-mode log name: `make -j check` may run the quick and stress
       scripts concurrently from the same directory, and a shared log file
       once turned a real FAIL into a recorded PASS.  */
    const std::string sched_log = std::string("schedbp-scheduler-")
        + (argc > 5 ? argv[5] : "default") + ".log";
    pid_t sched = start_scheduler(scheduler_bin, shim, port, sched_log,
                                  perf_mode ? "-v" : "-vvv",
                                  stallevict_mode ? "--dispatch-stall-timeout=10"
                                  : (leastbusy_mode ? "--algorithm=least_busy" : nullptr));
    if (sched < 0) {
        perror("fork");
        return 2;
    }

    // Wait for the scheduler to accept connections instead of trusting a
    // fixed sleep; also notice an exec failure (child exits 127) instead of
    // reporting it as a connect failure.
    {
        bool up = false;
        for (int i = 0; i < 100 && !up; ++i) {
            int status = 0;
            if (waitpid(sched, &status, WNOHANG) == sched) {
                if (WIFEXITED(status)) {
                    fprintf(stderr, "scheduler exited during startup (exit code %d%s)\n",
                            WEXITSTATUS(status),
                            WEXITSTATUS(status) == 127 ? ", exec failed" : "");
                } else {
                    fprintf(stderr, "scheduler died during startup (signal %d)\n",
                            WIFSIGNALED(status) ? WTERMSIG(status) : 0);
                }
                return 2;
            }
            int probe = tcp_connect(port, 0);
            if (probe >= 0) {
                close(probe);
                up = true;
                break;
            }
            usleep(100 * 1000);
        }
        if (!up) {
            fprintf(stderr, "scheduler never started listening on port %d\n", port);
            kill(sched, SIGTERM);
            waitpid(sched, nullptr, 0);
            return 2;
        }
    }

    // ---- fake compile server ----------------------------------------------
    MsgChannel *cs = connect_daemon(port, 0);
    if (!cs) {
        fprintf(stderr, "cannot connect CS to scheduler\n");
        kill(sched, SIGTERM);
        waitpid(sched, nullptr, 0);
        return 2;
    }
    {
        LoginMsg login(kCsPort, "fakecs", kPlatform, 0);
        login.envs.push_back(std::make_pair(kPlatform, kEnv));
        /* mixedrole withdraws remote capacity so every decision is local. */
        login.max_kids = mixedrole_mode ? 0
                                        : (farm_slots > 0 ? farm_slots : njobs + 16);
        login.noremote = false;
        login.chroot_possible = true;
        if (!cs->send_msg(login)) {
            fprintf(stderr, "CS login failed\n");
            delete cs;
            kill(sched, SIGTERM);
            waitpid(sched, nullptr, 0);
            return 2;
        }
        StatsMsg stats;   // load == 0: fully available
        cs->send_msg(stats);
    }

    std::atomic<bool> shutdown{false};
    std::atomic<bool> cs_alive{true};
    /* Job ids whose UseCS a submitter has received.  The compile server
       confirms them with JobBeginMsg, exactly as a real CS does once the
       client contacts it -- this is the progress signal the scheduler's
       dispatch credit is released by.  A frozen submitter never enqueues
       here, which is precisely why its credit stays held.  */
    std::mutex confirm_mutex;
    struct Confirm {
        unsigned int job_id;
        bool begin;
        bool done;
        unsigned int real_msec;   // reported compile time when done
    };
    std::vector<Confirm> to_confirm;
    auto enqueue_confirm = [&](unsigned int job_id, bool begin, bool done,
                               unsigned int real_msec) {
        std::lock_guard<std::mutex> lock(confirm_mutex);
        to_confirm.push_back(Confirm{job_id, begin, done, real_msec});
    };
    auto confirm_job = [&](unsigned int job_id) {
        enqueue_confirm(job_id, true, true, 0);
    };
    /* Split halves, for modes that must hold a slot busy (begin without
       done) or report a chosen runtime (feeds the estimate cache).  */
    auto begin_job = [&](unsigned int job_id) {
        enqueue_confirm(job_id, true, false, 0);
    };
    auto finish_job = [&](unsigned int job_id, unsigned int real_msec) {
        enqueue_confirm(job_id, false, true, real_msec);
    };
    std::thread cs_thread([&] {
        // keep the CS side drained, confirm assignments promptly (a real CS
        // sends JobBegin as soon as the client connects), and periodically
        // say we are idle
        Clock::time_point last_stats = Clock::now();
        while (!shutdown) {
            Msg *m = cs->get_msg(0, true);
            delete m;
            if (cs->at_eof()) {
                cs_alive = false;
                return;
            }
            {
                std::vector<Confirm> batch;
                {
                    std::lock_guard<std::mutex> lock(confirm_mutex);
                    batch.swap(to_confirm);
                }
                for (const Confirm &c : batch) {
                    if (c.begin) {
                        JobBeginMsg jb(c.job_id, 1);
                        if (!cs->send_msg(jb)) {
                            cs_alive = false;
                            return;
                        }
                    }
                    /* Completing promptly is what recycles the slot.  On a
                       small farm this lets a healthy submitter keep flowing
                       through the capacity the clamp reserves for it;
                       unconfirmed (never-read) assignments keep holding
                       their slots, exactly like a real dead client.  Modes
                       that hold a slot busy enqueue the begin half only.  */
                    if (c.done) {
                        JobDoneMsg jd(c.job_id, 0, JobDoneMsg::FROM_SERVER);
                        jd.real_msec = c.real_msec;
                        if (!cs->send_msg(jd)) {
                            cs_alive = false;
                            return;
                        }
                    }
                }
            }
            if (secs_since(last_stats) > 10) {
                StatsMsg stats;
                cs->send_msg(stats);
                last_stats = Clock::now();
            }
            usleep(20 * 1000);
        }
    });

    // ---- responsiveness probe: pre-established text-port control channel ---
    // (Fresh accept()s are a poor liveness signal: the scheduler adds its
    // listen fds to the poll set only when time() >= next_listen, so a new
    // connection made right after an accept cycle can sit unaccepted for the
    // remainder of a long poll() sleep -- multi-second latency on a perfectly
    // healthy idle scheduler.  An established control connection measures
    // true main-loop latency instead.)
    std::atomic<double> worst_reply{0.0};
    std::atomic<bool> probe_died{false};
    const Clock::time_point t_prog = Clock::now();
    std::atomic<double> probe_pending_since{-1.0};   // seconds since t_prog, -1 = idle
    /* Phase-tagged latency samples: 0 = ingress (requests still being
       submitted), 1 = drain (queue emptying after the flood).  The perf gate
       needs both distributions -- a scheduler that answers promptly once all
       requests have arrived but not while accepting them still misses the
       operational target.  Every sample is retained; percentiles are
       computed at the end.  */
    std::atomic<int> probe_phase{0};
    std::mutex sample_mutex;
    std::vector<std::pair<int, double>> probe_samples;
    std::atomic<double> phase_start_s{0.0};
    std::atomic<double> ingress_duration_s{0.0};
    std::atomic<double> drain_end_s{0.0};
    auto print_phase_summary = [&]() {
        /* Machine-readable per-phase distributions for the perf gate.  */
        std::lock_guard<std::mutex> lock(sample_mutex);
        for (int ph = 0; ph <= 1; ++ph) {
            std::vector<double> v;
            for (const auto &sample : probe_samples) {
                if (sample.first == ph) {
                    v.push_back(sample.second);
                }
            }
            std::sort(v.begin(), v.end());
            auto pctl = [&](double p) {
                if (v.empty()) {
                    return 0.0;
                }
                size_t idx = (size_t)(p * (v.size() - 1) + 0.5);
                return v[idx];
            };
            const double dur = ph == 0
                ? ingress_duration_s.load()
                : (drain_end_s.load() > 0
                       ? drain_end_s.load()
                             - (phase_start_s.load() + ingress_duration_s.load())
                       : 0.0);
            fprintf(stderr,
                    "# perf %s samples=%zu duration=%.2f p95=%.3f p99=%.3f max=%.3f\n",
                    ph == 0 ? "ingress" : "drain", v.size(), dur,
                    pctl(0.95), pctl(0.99), v.empty() ? 0.0 : v.back());
        }
    };
    int ctrl = tcp_connect(port + 1, 0);
    if (ctrl < 0) {
        fprintf(stderr, "cannot connect control channel\n");
        shutdown = true;
        cs_thread.join();
        delete cs;
        kill(sched, SIGTERM);
        waitpid(sched, nullptr, 0);
        return 2;
    }
    std::thread probe_thread([&] {
        char buf[4096];
        // Warm up: the first exchange includes the delayed accept (see
        // above) and the control login, so it can take many seconds on a
        // perfectly healthy scheduler.  Do one untimed round trip before
        // measuring.
        if (write(ctrl, "listcs\n", 7) == 7) {
            struct pollfd pfd = { ctrl, POLLIN, 0 };
            if (poll(&pfd, 1, 30 * 1000) > 0) {
                ssize_t n = read(ctrl, buf, sizeof(buf));
                (void)n;
            }
        }
        while (!shutdown) {
            // discard any stale buffered reply fragments so the next read is
            // guaranteed to be a response to the request sent below
            for (;;) {
                struct pollfd pfd = { ctrl, POLLIN, 0 };
                if (poll(&pfd, 1, 0) <= 0) {
                    break;
                }
                if (read(ctrl, buf, sizeof(buf)) <= 0) {
                    probe_died = true;
                    return;
                }
            }
            Clock::time_point t0 = Clock::now();
            const int phase_at_send = probe_phase.load();
            probe_pending_since = secs_since(t_prog);
            if (write(ctrl, "listcs\n", 7) != 7) {
                probe_died = true;
                return;
            }
            bool got_reply = false;
            while (!got_reply && secs_since(t0) < 120 && !shutdown) {
                struct pollfd pfd = { ctrl, POLLIN, 0 };
                if (poll(&pfd, 1, 200) > 0) {
                    ssize_t n = read(ctrl, buf, sizeof(buf));
                    if (n <= 0) {
                        probe_died = true;
                        return;
                    }
                    got_reply = true;
                }
            }
            // An unanswered round is an outage at least as long as the time
            // waited -- count it, or a scheduler that never replies again
            // would score better than a slow one.
            double lat = secs_since(t0);
            if (lat > 1.0) {
                fprintf(stderr, "# control reply%s took %.1fs\n",
                        got_reply ? "" : " (still unanswered)", lat);
            }
            double w = worst_reply.load();
            while (lat > w && !worst_reply.compare_exchange_weak(w, lat)) {
            }
            {
                std::lock_guard<std::mutex> lock(sample_mutex);
                probe_samples.emplace_back(phase_at_send, lat);
            }
            probe_pending_since = -1.0;
            if (!shutdown && probe_phase.load() != 0) {
                usleep(100 * 1000);   // drain cadence; ingress probes run
                                      // back-to-back (the phase is short)
            }
        }
    });

    // ---- second, healthy submitter (must keep making progress) -------------
    MsgChannel *sub2 = connect_daemon(port, 0);
    std::atomic<int> healthy_replies{0};
    std::atomic<bool> healthy_alive{true};
    if (!sub2) {
        fprintf(stderr, "cannot connect second submitter\n");
        shutdown = true; probe_thread.join(); cs_thread.join();
        close(ctrl); delete cs; kill(sched, SIGTERM); waitpid(sched, nullptr, 0);
        return 2;
    }
    {
        LoginMsg login(0, "fakesub2", kPlatform, 0);
        login.envs.push_back(std::make_pair(kPlatform, kEnv));
        login.max_kids = 0;
        login.noremote = true;
        login.chroot_possible = false;
        sub2->send_msg(login);
    }
    std::thread healthy_thread([&] {
        /* leastbusy asserts exact occupancy counts; a concurrent stream
           holding 0-1 slots at random moments would make them flake.  The
           connection stays (the scheduler keeps a second submitter), the
           traffic parks.  */
        if (leastbusy_mode) {
            while (!shutdown) {
                usleep(100 * 1000);
            }
            return;
        }
        // request one job at a time and drain replies promptly
        unsigned int cid = 900000;
        while (!shutdown) {
            char fname[64];
            snprintf(fname, sizeof(fname), "healthy%u.cpp", cid);
            GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                       fname, CompileJob::Lang_CXX, 1, kPlatform, 0, std::string(), 0, 0, 0);
            g.client_id = cid++;
            if (!sub2->send_msg(g)) {
                healthy_alive = false;
                return;
            }
            Msg *m = sub2->get_msg(2, true);
            if (m) {
                if (MSG_IS(m, USE_CS) || MSG_IS(m, NO_CS)) {
                    ++healthy_replies;
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u) {
                        confirm_job(u->job_id);
                    }
                }
                delete m;
            } else if (sub2->at_eof()) {
                healthy_alive = false;
                return;
            }
            usleep(100 * 1000);
        }
    });

    // ---- fake submitter ----------------------------------------------------
    // 64KiB rcvbuf: small enough that the dispatch replies jam, large enough
    // (>= one loopback MSS) that the window reopens when we drain at the end.
    MsgChannel *sub = connect_daemon(port, 64 * 1024);
    if (!sub) {
        fprintf(stderr, "cannot connect submitter to scheduler\n");
        shutdown = true;
        probe_thread.join();
        cs_thread.join();
        close(ctrl);
        delete cs;
        kill(sched, SIGTERM);
        waitpid(sched, nullptr, 0);
        return 2;
    }
    {
        LoginMsg login(mixedrole_mode ? 10246 : 0, "fakesub", kPlatform, 0);
        login.envs.push_back(std::make_pair(kPlatform, kEnv));
        /* Pure-submitter topology by default (max_kids=0 keeps the local
           arm unreachable, which is why five review rounds never saw the
           local-credit defect).  mixedrole gives it real capacity, which is
           what every developer machine actually looks like.  */
        login.max_kids = mixedrole_mode ? 60 : 0;
        login.noremote = mixedrole_mode ? false : true;
        login.chroot_possible = mixedrole_mode ? true : false;
        if (!sub->send_msg(login)) {
            fprintf(stderr, "submitter login failed\n");
            shutdown = true;
            probe_thread.join();
            cs_thread.join();
            close(ctrl);
            delete sub;
            delete cs;
            kill(sched, SIGTERM);
            waitpid(sched, nullptr, 0);
            return 2;
        }
    }
    // let any login-time chatter (CS_CONF, ...) arrive and be consumed
    for (;;) {
        Msg *m = sub->get_msg(2, true);
        if (!m) {
            break;
        }
        delete m;
    }

    if (mixedrole_mode) {
        /* Take the fake compile server out of service so the ONLY placement
           available is local.  Every reply is then a local decision.  */
        fprintf(stderr, "# mixed-role: submitter has capacity; remote CS has no free slots\n");
        for (int i = 1; i <= njobs; ++i) {
            char fname[64];
            snprintf(fname, sizeof(fname), "local%04d.cpp", i);
            GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                       fname, CompileJob::Lang_CXX, 1, kPlatform, 0, std::string(), 0, 0, 0);
            g.client_id = (unsigned)i;
            if (!sub->send_msg(g)) {
                fprintf(stderr, "FAILED   - submitter died requesting local job %d\n", i);
                ++failures;
                break;
            }
        }
        /* The submitter has finite local capacity, so the scheduler will
           place up to that many and queue the rest -- correct behaviour.
           What matters is that placement does NOT stop at the old credit
           ceiling of 32 and that outstanding stays 0 throughout.  */
        int local_replies = 0, remote_replies = 0;
        const Clock::time_point t0 = Clock::now();
        while (local_replies + remote_replies < njobs && secs_since(t0) < 25) {
            Msg *m = sub->get_msg(2);
            if (!m) {
                continue;
            }
            if (MSG_IS(m, NO_CS)) {
                ++local_replies;        // placed back on this host
            } else if (MSG_IS(m, USE_CS)) {
                ++remote_replies;
            }
            delete m;
        }
        const long long outstanding = query_submitter_outstanding(port, "fakesub");
        fprintf(stderr, "# local replies=%d remote=%d outstanding_dispatches=%lld\n",
                local_replies, remote_replies, outstanding);
        REQUIRE(local_replies > 32,
                "local placement continued past the old credit ceiling of 32");
        REQUIRE(remote_replies == 0,
                "no remote decision was made (remote capacity was withdrawn)");
        REQUIRE(outstanding == 0,
                "local decisions consume no remote dispatch credit (BP-1)");
        shutdown = true;
        probe_thread.join();
        cs_thread.join();
        healthy_thread.join();
        close(ctrl);
        delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        waitpid(sched, nullptr, 0);
        if (failures) {
            fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
            return 1;
        }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (multicount_mode) {
        fprintf(stderr, "# requesting ONE message with count=%d\n", njobs);
        GetCSMsg gcs(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                     "multicount.cpp", CompileJob::Lang_CXX, (unsigned)njobs,
                     kPlatform, 0, std::string(), 0, 0, 0);
        gcs.client_id = 1;
        if (!sub->send_msg(gcs)) {
            fprintf(stderr, "FAILED   - submitter died sending the multi-count request\n");
            ++failures;
        }
        /* Collect replies until the count is satisfied or we give up.  */
        int replies = 0;
        const Clock::time_point t0 = Clock::now();
        while (replies < njobs && secs_since(t0) < 60) {
            Msg *m = sub->get_msg(2);
            if (!m) {
                continue;
            }
            if (MSG_IS(m, USE_CS)) {
                UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                if (u) {
                    confirm_job(u->job_id);   // release the dispatch credit
                }
                ++replies;
            } else if (MSG_IS(m, NO_CS)) {
                NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                if (n) {
                    confirm_job(n->job_id);
                }
                ++replies;
            }
            delete m;
        }
        fprintf(stderr, "# replies for count=%d: %d\n", njobs, replies);
        REQUIRE(replies == njobs,
                "a count=N request yields exactly N replies (resumable expansion)");
        REQUIRE(worst_reply.load() < 5.0, "scheduler stayed responsive");
        shutdown = true;
        probe_thread.join();
        cs_thread.join();
        healthy_thread.join();
        close(ctrl);
        delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        waitpid(sched, nullptr, 0);
        if (failures) {
            fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
            return 1;
        }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (contract_mode) {
        /* The general request contract, beyond the single count=N case:

             1. two queued requests on ONE connection are admitted whole and
                in arrival order (per-connection FIFO, not a one-slot record
                that loses the second request's tail);
             2. count=0 asks for zero replies and admits nothing;
             3. a second daemon expanding a large count concurrently gets its
                exact count too (rotation, no lowest-fd monopoly);
             4. a daemon that disconnects mid-expansion leaves nothing behind:
                a fresh connection (likely reusing the fd) receives no
                unsolicited replies and its own request works;
             5. every job of a resumed request carries the SAME master id in
                the scheduler log (sibling chain survives resume steps).  */
        const unsigned c1 = (unsigned)njobs;          // large: forces resume steps
        const unsigned c2 = (unsigned)(njobs * 4 / 5);
        const long long admitted0 = query_submitter_admitted(port, "fakesub");
        const long long generation0 = query_submitter_generation(port, "fakesub");

        auto send_count = [&](MsgChannel *ch, unsigned count, unsigned client_id,
                              const char *fname) {
            GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                       fname, CompileJob::Lang_CXX, count, kPlatform, 0,
                       std::string(), 0, 0, 0);
            g.client_id = client_id;
            return ch->send_msg(g);
        };

        // ---- case 1+2: back-to-back requests plus a count=0 on one channel
        fprintf(stderr, "# contract: count=%u then count=%u then count=0 on one connection\n", c1, c2);
        REQUIRE(send_count(sub, c1, 101, "contractA.cpp"), "request A sent");
        REQUIRE(send_count(sub, c2, 102, "contractB.cpp"), "request B sent");
        REQUIRE(send_count(sub, 0,  103, "contractZ.cpp"), "count=0 request sent");

        // ---- case 3: a second daemon expands a large count concurrently
        MsgChannel *subB = connect_daemon(port, 0);
        REQUIRE(subB != nullptr, "concurrent daemon connected");
        std::atomic<int> repliesB{0};
        std::thread subB_thread([&] {
            if (!subB) {
                return;
            }
            LoginMsg login(0, "fakesub3", kPlatform, 0);
            login.envs.push_back(std::make_pair(kPlatform, kEnv));
            login.max_kids = 0;
            login.noremote = true;
            if (!subB->send_msg(login)) {
                return;
            }
            if (!send_count(subB, c1, 201, "contractC.cpp")) {
                return;
            }
            const Clock::time_point t0 = Clock::now();
            while (repliesB < (int)c1 && secs_since(t0) < 120) {
                Msg *m = subB->get_msg(2);
                if (!m) {
                    continue;
                }
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u && u->client_id == 201) {
                        confirm_job(u->job_id);
                        ++repliesB;
                    }
                } else if (MSG_IS(m, NO_CS)) {
                    NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                    if (n && n->client_id == 201) {
                        confirm_job(n->job_id);
                        ++repliesB;
                    }
                }
                delete m;
            }
        });

        // ---- collect replies for A/B/zero on the main channel
        std::vector<unsigned> ids1, ids2;
        int replies_zero = 0;
        {
            const Clock::time_point t0 = Clock::now();
            while ((ids1.size() < c1 || ids2.size() < c2) && secs_since(t0) < 120) {
                Msg *m = sub->get_msg(2);
                if (!m) {
                    continue;
                }
                unsigned jid = 0, cid = 0;
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u) { jid = u->job_id; cid = u->client_id; }
                } else if (MSG_IS(m, NO_CS)) {
                    NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                    if (n) { jid = n->job_id; cid = n->client_id; }
                }
                if (jid) {
                    confirm_job(jid);
                    if (cid == 101) {
                        ids1.push_back(jid);
                    } else if (cid == 102) {
                        ids2.push_back(jid);
                    } else if (cid == 103) {
                        ++replies_zero;
                    }
                }
                delete m;
            }
        }
        subB_thread.join();
        fprintf(stderr, "# contract: A=%zu/%u B=%zu/%u zero=%d concurrent=%d/%u\n",
                ids1.size(), c1, ids2.size(), c2, replies_zero, repliesB.load(), c1);
        REQUIRE(ids1.size() == c1, "first queued request yields exactly its count");
        REQUIRE(ids2.size() == c2, "second queued request yields exactly its count");
        REQUIRE(replies_zero == 0, "count=0 yields zero replies");
        REQUIRE(repliesB == (int)c1, "a concurrently expanding daemon gets its exact count");
        {
            /* Admission order: job ids are globally monotonic, so FIFO per
               connection means every id of A precedes every id of B.  */
            unsigned max1 = 0, min2 = ~0u;
            for (unsigned id : ids1) { if (id > max1) { max1 = id; } }
            for (unsigned id : ids2) { if (id < min2) { min2 = id; } }
            REQUIRE(!ids1.empty() && !ids2.empty() && max1 < min2,
                    "requests on one connection are admitted in arrival order");
        }
        {
            const long long admitted1 = query_submitter_admitted(port, "fakesub");
            const long long generation1 = query_submitter_generation(port, "fakesub");
            REQUIRE(generation1 == generation0 && generation0 > 0,
                    "connection generation unchanged, so the baseline/delta window is valid");
            REQUIRE(admitted1 - admitted0 == (long long)(c1 + c2),
                    "admitted counter moved by exactly the two real counts (count=0 admitted nothing)");
        }

        // ---- case 5: sibling chain identical across resume steps
        {
            std::map<unsigned, unsigned> master_of;   // job id -> logged master id
            FILE *lf = fopen(sched_log.c_str(), "r");
            if (lf) {
                char line[4096];
                while (fgets(line, sizeof(line), lf)) {
                    const char *nw = strstr(line, "NEW ");
                    if (!nw) {
                        continue;
                    }
                    const char *ms = strstr(nw, " master=");
                    if (!ms) {
                        continue;
                    }
                    master_of[(unsigned)atoi(nw + 4)] = (unsigned)atoi(ms + 8);
                }
                fclose(lf);
            }
            unsigned min1 = ~0u;
            for (unsigned id : ids1) { if (id < min1) { min1 = id; } }
            int chained = 0, broken = 0;
            for (unsigned id : ids1) {
                if (id == min1) {
                    continue;             // the master itself carries no tag
                }
                std::map<unsigned, unsigned>::const_iterator mit = master_of.find(id);
                if (mit != master_of.end() && mit->second == min1) {
                    ++chained;
                } else {
                    ++broken;
                }
            }
            fprintf(stderr, "# contract: master chain %d chained / %d broken (master=%u)\n",
                    chained, broken, min1);
            REQUIRE(broken == 0 && chained == (int)c1 - 1,
                    "every resumed job of request A logs the ORIGINAL master id");
        }

        // ---- case 4: disconnect mid-expansion, then a fresh connection
        {
            MsgChannel *subC = connect_daemon(port, 0);
            REQUIRE(subC != nullptr, "doomed daemon connected");
            if (subC) {
                LoginMsg login(0, "fakesub4", kPlatform, 0);
                login.envs.push_back(std::make_pair(kPlatform, kEnv));
                login.max_kids = 0;
                login.noremote = true;
                REQUIRE(subC->send_msg(login), "doomed daemon logged in");
                REQUIRE(send_count(subC, 200, 301, "contractD.cpp"),
                        "doomed daemon requested count=200");
                int seen = 0;
                const Clock::time_point t0 = Clock::now();
                while (seen < 10 && secs_since(t0) < 30) {
                    Msg *m = subC->get_msg(2);
                    if (!m) {
                        continue;
                    }
                    if (MSG_IS(m, USE_CS) || MSG_IS(m, NO_CS)) {
                        ++seen;   // deliberately NOT confirmed: it is about to die
                    }
                    delete m;
                }
                REQUIRE(seen == 10, "doomed daemon saw the first ten replies");
                delete subC;      // abrupt close, ~190 jobs still expanding
            }

            /* The very next accept() is the natural fd-reuse candidate.  */
            MsgChannel *subD = connect_daemon(port, 0);
            REQUIRE(subD != nullptr, "successor daemon connected");
            if (subD) {
                LoginMsg login(0, "fakesub5", kPlatform, 0);
                login.envs.push_back(std::make_pair(kPlatform, kEnv));
                login.max_kids = 0;
                login.noremote = true;
                REQUIRE(subD->send_msg(login), "successor logged in");
                int unsolicited = 0;
                const Clock::time_point t0 = Clock::now();
                while (secs_since(t0) < 4) {
                    Msg *m = subD->get_msg(1);
                    if (!m) {
                        continue;
                    }
                    if (MSG_IS(m, USE_CS) || MSG_IS(m, NO_CS)) {
                        ++unsolicited;
                    }
                    delete m;
                }
                REQUIRE(unsolicited == 0,
                        "a connection reusing the fd inherits NO replies from the dead request");
                REQUIRE(send_count(subD, 5, 401, "contractE.cpp"),
                        "successor sent its own count=5");
                int got = 0;
                const Clock::time_point t1 = Clock::now();
                while (got < 5 && secs_since(t1) < 30) {
                    Msg *m = subD->get_msg(2);
                    if (!m) {
                        continue;
                    }
                    if (MSG_IS(m, USE_CS)) {
                        UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                        if (u && u->client_id == 401) { confirm_job(u->job_id); ++got; }
                    } else if (MSG_IS(m, NO_CS)) {
                        NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                        if (n && n->client_id == 401) { confirm_job(n->job_id); ++got; }
                    }
                    delete m;
                }
                REQUIRE(got == 5, "successor's own request works normally");
                delete subD;
            }
        }

        REQUIRE(worst_reply.load() < 5.0, "scheduler stayed responsive throughout");
        if (subB) {
            delete subB;
        }
        shutdown = true;
        probe_thread.join();
        cs_thread.join();
        healthy_thread.join();
        close(ctrl);
        delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        waitpid(sched, nullptr, 0);
        if (failures) {
            fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
            return 1;
        }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (promotion_mode) {
        /* SCH-1: the 60s hard-promotion rule, at the production bound.

           One farm slot.  Warm two estimate-cache keys through real
           JobDone reports: quickjob.cpp ~100ms, bigjob.cpp ~90000ms.  Hold
           the slot busy and let a quickjob request age past the bound;
           then submit a fresh bigjob whose score (2*estimate) outranks the
           old request's age-score for another ~120s.  When the slot frees,
           numeric selection would pick the bigjob -- ONLY the hard rule
           dispatches the old request first.  The healthy submitter's
           continuous stream runs throughout, so the promoted request also
           beats a live population of younger competitors.  */
        auto send_named = [&](const char *fname, unsigned cid) {
            GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                       fname, CompileJob::Lang_CXX, 1, kPlatform, 0,
                       std::string(), 0, 0, 0);
            g.client_id = cid;
            return sub->send_msg(g);
        };
        /* One reply for a specific client id; confirms/finishes others'
           replies as they pass so the farm keeps recycling.  */
        auto await_reply = [&](unsigned cid, double timeout_s) -> unsigned {
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < timeout_s) {
                Msg *m = sub->get_msg(2);
                if (!m) {
                    continue;
                }
                unsigned jid = 0, got_cid = 0;
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u) { jid = u->job_id; got_cid = u->client_id; }
                } else if (MSG_IS(m, NO_CS)) {
                    NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                    if (n) { jid = n->job_id; got_cid = n->client_id; }
                }
                if (jid && got_cid == cid) {
                    delete m;
                    return jid;
                }
                if (jid) {
                    confirm_job(jid);   // someone else's; keep the farm alive
                }
                delete m;
            }
            return 0;
        };

        fprintf(stderr, "# promotion: warming estimates (3x quick@100ms, 3x big@90000ms)\n");
        for (int round = 0; round < 3; ++round) {
            unsigned jid = 0;
            REQUIRE(send_named("quickjob.cpp", 6001) && (jid = await_reply(6001, 30)) != 0,
                    "quick warm round dispatched");
            begin_job(jid); finish_job(jid, 100);
            REQUIRE(send_named("bigjob.cpp", 6002) && (jid = await_reply(6002, 30)) != 0,
                    "big warm round dispatched");
            begin_job(jid); finish_job(jid, 90000);
        }

        /* Hold the host COMPLETELY: the slot and the preload window (a
           one-slot host accepts maxJobs + 1 + maxJobs/4 = 2 assignments).
           With only the slot held, the aged request below would simply be
           preloaded during the wait and the promotion decision would never
           run.  */
        fprintf(stderr, "# promotion: occupying the slot and the preload window\n");
        unsigned occ1 = 0, occ2 = 0;
        REQUIRE(send_named("occupier.cpp", 6003) && (occ1 = await_reply(6003, 30)) != 0,
                "occupier 1 dispatched");
        /* Begin IMMEDIATELY: on a one-slot farm the dispatch credit clamps
           to one, so an assigned-but-unbegun job gates its submitter and
           nothing else of ours -- occupier 2 included -- can dispatch.
           Begin (without done) holds the assignment while releasing the
           credit.  */
        begin_job(occ1);
        REQUIRE(send_named("occupier2.cpp", 6004) && (occ2 = await_reply(6004, 60)) != 0,
                "occupier 2 assigned (preload window)");
        begin_job(occ2);

        REQUIRE(send_named("quickjob.cpp", 7001), "old request submitted");
        const Clock::time_point t_old = Clock::now();
        fprintf(stderr, "# promotion: aging the request past the 60s bound (production constant)\n");
        int premature = 0;
        while (secs_since(t_old) < 61.5) {
            Msg *m = sub->get_msg(2);
            if (!m) {
                continue;
            }
            if (MSG_IS(m, USE_CS) || MSG_IS(m, NO_CS)) {
                ++premature;   // nothing may dispatch while the slot is held
            }
            delete m;
        }
        REQUIRE(premature == 0, "no dispatch while the only slot was held");

        REQUIRE(send_named("bigjob.cpp", 7002), "fresh high-score request submitted");
        usleep(500 * 1000);
        finish_job(occ1, 30000);   // free ONE window: the next choice is the test

        /* The FIRST dispatch to this submitter after the release must be the
           overdue quickjob, although the fresh bigjob outscores it by ~2x.  */
        unsigned first_cid = 0, first_jid = 0;
        {
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < 30 && first_jid == 0) {
                Msg *m = sub->get_msg(2);
                if (!m) {
                    continue;
                }
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u) { first_jid = u->job_id; first_cid = u->client_id; }
                } else if (MSG_IS(m, NO_CS)) {
                    NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                    if (n) { first_jid = n->job_id; first_cid = n->client_id; }
                }
                delete m;
            }
        }
        fprintf(stderr, "# promotion: first dispatch after release went to client_id=%u\n", first_cid);
        REQUIRE(first_jid != 0, "a dispatch followed the slot release");
        REQUIRE(first_cid == 7001,
                "the overdue request was promoted over the numerically superior one (SCH-1)");
        if (first_jid) {
            confirm_job(first_jid);
        }
        REQUIRE(await_reply(7002, 60) != 0, "the high-score request follows (liveness)");
        begin_job(occ2); finish_job(occ2, 1000);   // leave a clean farm
        REQUIRE(worst_reply.load() < 5.0, "scheduler stayed responsive");

        shutdown = true;
        probe_thread.join(); cs_thread.join(); healthy_thread.join();
        close(ctrl);
        delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        waitpid(sched, nullptr, 0);
        if (failures) { fprintf(stderr, "RESULT: FAIL (%d)\n", failures); return 1; }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (heterogeneous_mode) {
        /* SCH-3: the circular selection walk.  The scored head (an old
           x86_64 request) is unservable NOW -- its only capable host holds
           its single slot -- while a younger aarch64 request is servable on
           a second host.  A forward-only walk from the head reports "no
           suitable host" and the aarch64 request starves; the circular
           repair must dispatch it immediately.  */
        MsgChannel *csB = connect_daemon(port, 0);
        REQUIRE(csB != nullptr, "second farm host connected");
        std::mutex bconfirm_mutex;
        std::vector<unsigned> b_to_confirm;
        std::atomic<bool> csB_alive{true};
        std::thread csB_thread([&] {
            if (!csB) { return; }
            LoginMsg login(10261, "fakecsB", "aarch64", 0);
            login.envs.push_back(std::make_pair(std::string("aarch64"), std::string(kEnv)));
            login.max_kids = 4;
            login.noremote = false;
            login.chroot_possible = true;
            if (!csB->send_msg(login)) { csB_alive = false; return; }
            StatsMsg stats;
            csB->send_msg(stats);
            Clock::time_point last_stats = Clock::now();
            while (!shutdown) {
                Msg *m = csB->get_msg(0, true);
                delete m;
                if (csB->at_eof()) { csB_alive = false; return; }
                std::vector<unsigned> batch;
                {
                    std::lock_guard<std::mutex> lock(bconfirm_mutex);
                    batch.swap(b_to_confirm);
                }
                for (unsigned jid : batch) {
                    JobBeginMsg jb(jid, 1);
                    JobDoneMsg jd(jid, 0, JobDoneMsg::FROM_SERVER);
                    if (!csB->send_msg(jb) || !csB->send_msg(jd)) {
                        csB_alive = false;
                        return;
                    }
                }
                if (secs_since(last_stats) > 10) {
                    StatsMsg st;
                    csB->send_msg(st);
                    last_stats = Clock::now();
                }
                usleep(20 * 1000);
            }
        });

        auto send_platform = [&](const char *fname, unsigned cid, const char *platform) {
            GetCSMsg g(Environments{std::make_pair(std::string(platform), std::string(kEnv))},
                       fname, CompileJob::Lang_CXX, 1, platform, 0,
                       std::string(), 0, 0, 0);
            g.client_id = cid;
            return sub->send_msg(g);
        };

        /* Hold the x86_64 host COMPLETELY: its single compile slot and its
           preload window (maxJobs + 1 + maxJobs/4 = 2 assignments for a
           one-slot host).  One occupier holds the slot; the second parks in
           the preload window.  Otherwise the head below would simply be
           preloaded and the walk under test never runs.  */
        unsigned occ1 = 0, occ2 = 0;
        REQUIRE(send_platform("occupier.cpp", 8000, kPlatform), "occupier 1 submitted");
        REQUIRE(send_platform("occupier2.cpp", 8005, kPlatform), "occupier 2 submitted");
        {
            const Clock::time_point t0 = Clock::now();
            while ((occ1 == 0 || occ2 == 0) && secs_since(t0) < 30) {
                Msg *m = sub->get_msg(2);
                if (!m) { continue; }
                UseCSMsg *u = MSG_IS(m, USE_CS) ? dynamic_cast<UseCSMsg *>(m) : nullptr;
                if (u && u->client_id == 8000) { occ1 = u->job_id; }
                if (u && u->client_id == 8005) { occ2 = u->job_id; }
                delete m;
            }
        }
        REQUIRE(occ1 != 0 && occ2 != 0,
                "both occupiers assigned to the x86_64 host (slot + preload window)");
        begin_job(occ1);

        /* Old head: x86_64, currently unservable.  Age it a moment so it
           outranks the aarch64 request on every numeric ordering.  */
        REQUIRE(send_platform("headx86.cpp", 8001, kPlatform), "x86 head submitted");
        sleep(3);
        REQUIRE(send_platform("armjob.cpp", 8002, "aarch64"), "aarch64 request submitted");

        /* The aarch64 request must dispatch promptly to fakecsB while the
           head stays queued.  */
        unsigned arm_jid = 0, arm_port = 0; int head_replies = 0;
        {
            const Clock::time_point t0 = Clock::now();
            while (arm_jid == 0 && secs_since(t0) < 20) {
                Msg *m = sub->get_msg(2);
                if (!m) { continue; }
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u && u->client_id == 8002) { arm_jid = u->job_id; arm_port = u->port; }
                    else if (u && u->client_id == 8001) { ++head_replies; }
                } else if (MSG_IS(m, NO_CS)) {
                    NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                    if (n && n->client_id == 8001) { ++head_replies; }
                }
                delete m;
            }
        }
        fprintf(stderr, "# heterogeneous: aarch64 dispatched=%s port=%u, head replies meanwhile=%d\n",
                arm_jid ? "yes" : "NO", arm_port, head_replies);
        REQUIRE(arm_jid != 0,
                "a compatible request behind an unservable head is dispatched (SCH-3 circular walk)");
        /* The hostname field carries the address; the advertised remote
           port is what distinguishes the two local fake hosts.  */
        REQUIRE(arm_port == 10261, "it went to the aarch64 host");
        REQUIRE(head_replies == 0, "the unservable head stayed queued, not bounced");
        if (arm_jid) {
            std::lock_guard<std::mutex> lock(bconfirm_mutex);
            b_to_confirm.push_back(arm_jid);
        }

        /* Liveness: free one x86_64 assignment; the head must now dispatch
           (into the freed preload window).  */
        finish_job(occ1, 1000);
        unsigned head_jid = 0;
        {
            const Clock::time_point t0 = Clock::now();
            while (head_jid == 0 && secs_since(t0) < 30) {
                Msg *m = sub->get_msg(2);
                if (!m) { continue; }
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u && u->client_id == 8001) { head_jid = u->job_id; }
                }
                delete m;
            }
        }
        REQUIRE(head_jid != 0, "the head dispatches once its host has capacity");
        if (head_jid) {
            confirm_job(head_jid);
        }
        REQUIRE(worst_reply.load() < 5.0, "scheduler stayed responsive");

        shutdown = true;
        probe_thread.join(); cs_thread.join(); healthy_thread.join(); csB_thread.join();
        close(ctrl);
        delete sub; delete sub2; delete cs; delete csB;
        kill(sched, SIGTERM);
        waitpid(sched, nullptr, 0);
        if (failures) { fprintf(stderr, "RESULT: FAIL (%d)\n", failures); return 1; }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (stallevict_mode) {
        /* BP-1's liveness bound, at the 10s CLI floor: a submitter whose
           dispatched jobs never reach JobBegin is evicted AT the bound --
           demonstrably not before it, and not never -- and the healthy
           submitter is served straight through the event.  */
        fprintf(stderr, "# stallevict: flooding %d jobs, never confirming\n", njobs);
        for (int i = 1; i <= njobs; ++i) {
            char fname[64];
            snprintf(fname, sizeof(fname), "stall%04d.cpp", i);
            GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                       fname, CompileJob::Lang_CXX, 1, kPlatform, 0,
                       std::string(), 0, 0, 0);
            g.client_id = i;
            if (!sub->send_msg(g)) {
                fprintf(stderr, "FAILED   - submitter died while flooding\n");
                ++failures;
                break;
            }
        }
        /* Read the assignments (delivery is not the stall condition --
           JobBegin is) but confirm NOTHING.  */
        int delivered = 0;
        const Clock::time_point t0 = Clock::now();
        bool evicted = false;
        double evict_s = -1;
        while (secs_since(t0) < 40) {
            Msg *m = sub->get_msg(1);
            if (m) {
                if (MSG_IS(m, USE_CS) || MSG_IS(m, NO_CS)) {
                    ++delivered;
                }
                delete m;
            }
            if (sub->at_eof()) {
                evicted = true;
                evict_s = secs_since(t0);
                break;
            }
        }
        const int healthy_at_evict = healthy_replies.load();
        fprintf(stderr, "# stallevict: delivered=%d evicted=%s at %.1fs (bound 10s), healthy so far=%d\n",
                delivered, evicted ? "yes" : "NO", evict_s, healthy_at_evict);
        REQUIRE(delivered > 0, "assignments were delivered before the stall");
        REQUIRE(evicted, "the stalled submitter was evicted");
        REQUIRE(evict_s >= 9.0, "eviction respected the bound (not premature)");
        REQUIRE(evict_s <= 25.0, "eviction happened promptly after the bound");
        /* The healthy submitter must keep completing work after the event.  */
        {
            const Clock::time_point th = Clock::now();
            while (healthy_replies.load() < healthy_at_evict + 5 && secs_since(th) < 30) {
                usleep(100 * 1000);
            }
        }
        fprintf(stderr, "# stallevict: healthy now=%d (was %d)\n",
                healthy_replies.load(), healthy_at_evict);
        REQUIRE(healthy_replies.load() >= healthy_at_evict + 5,
                "the healthy submitter kept being served through the eviction");
        REQUIRE(healthy_alive.load(), "the healthy submitter connection survived");
        REQUIRE(worst_reply.load() < 5.0, "scheduler stayed responsive");

        shutdown = true;
        probe_thread.join(); cs_thread.join(); healthy_thread.join();
        close(ctrl);
        delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        waitpid(sched, nullptr, 0);
        if (failures) { fprintf(stderr, "RESULT: FAIL (%d)\n", failures); return 1; }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (leastbusy_mode) {
        /* Second x86_64 host so occupancy comparison has two operands.  */
        MsgChannel *csC = connect_daemon(port, 0);
        REQUIRE(csC != nullptr, "second farm host connected");
        std::mutex cconfirm_mutex;
        std::vector<std::pair<unsigned, bool> > c_to_confirm;   // jid, done?
        std::atomic<bool> csC_alive{true};
        std::thread csC_thread([&] {
            if (!csC) { return; }
            LoginMsg login(10262, "fakecsC", kPlatform, 0);
            login.envs.push_back(std::make_pair(std::string(kPlatform), std::string(kEnv)));
            login.max_kids = 2;
            login.noremote = false;
            login.chroot_possible = true;
            if (!csC->send_msg(login)) { csC_alive = false; return; }
            StatsMsg st0;
            csC->send_msg(st0);
            Clock::time_point last_stats = Clock::now();
            while (!shutdown) {
                Msg *m = csC->get_msg(0, true);
                delete m;
                if (csC->at_eof()) { csC_alive = false; return; }
                std::vector<std::pair<unsigned, bool> > batch;
                {
                    std::lock_guard<std::mutex> lock(cconfirm_mutex);
                    batch.swap(c_to_confirm);
                }
                for (const auto &e : batch) {
                    if (!e.second) {
                        JobBeginMsg jb(e.first, 1);
                        if (!csC->send_msg(jb)) { csC_alive = false; return; }
                    } else {
                        JobDoneMsg jd(e.first, 0, JobDoneMsg::FROM_SERVER);
                        if (!csC->send_msg(jd)) { csC_alive = false; return; }
                    }
                }
                if (secs_since(last_stats) > 10) {
                    StatsMsg st;
                    csC->send_msg(st);
                    last_stats = Clock::now();
                }
                usleep(20 * 1000);
            }
        });
        auto beginC = [&](unsigned jid) {
            std::lock_guard<std::mutex> lock(cconfirm_mutex);
            c_to_confirm.push_back(std::make_pair(jid, false));
        };
        auto doneC = [&](unsigned jid) {
            std::lock_guard<std::mutex> lock(cconfirm_mutex);
            c_to_confirm.push_back(std::make_pair(jid, true));
        };

        auto send_one = [&](unsigned cid, const char *fname) {
            GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                       fname, CompileJob::Lang_CXX, 1, kPlatform, 0,
                       std::string(), 0, 0, 0);
            g.client_id = cid;
            return sub->send_msg(g);
        };
        /* Reply for a specific client id; returns job id and the host's
           advertised port through out-params.  */
        auto await_use = [&](unsigned cid, double timeout_s,
                             unsigned *jid_out, unsigned *port_out) {
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < timeout_s) {
                Msg *m = sub->get_msg(2);
                if (!m) { continue; }
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u && u->client_id == cid) {
                        *jid_out = u->job_id;
                        *port_out = u->port;
                        delete m;
                        return true;
                    }
                }
                delete m;
            }
            return false;
        };
        auto begin_on = [&](unsigned jid, unsigned csport) {
            if (csport == 10262) { beginC(jid); } else { begin_job(jid); }
        };
        auto done_on = [&](unsigned jid, unsigned csport) {
            if (csport == 10262) { doneC(jid); } else { finish_job(jid, 100); }
        };

        /* The second host must be REGISTERED before any fill is submitted,
           or the early fills see a one-host farm and the spread assertion
           measures a registration race instead of the picker.  */
        {
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < 30
                   && query_submitter_field(port, "fakecsC", "jobs=") < 0) {
                usleep(100 * 1000);
            }
            REQUIRE(query_submitter_field(port, "fakecsC", "jobs=") >= 0,
                    "second host registered before the fill");
        }

        /* Phase 1 -- fill every compile slot on both hosts (2+2), begun so
           the occupancy is real.  */
        struct Held { unsigned jid, port; };
        std::vector<Held> held;
        for (int i = 0; i < 4; ++i) {
            unsigned jid = 0, csport = 0;
            char fname[32];
            snprintf(fname, sizeof(fname), "fill%d.cpp", i);
            REQUIRE(send_one(9100 + i, fname), "fill job submitted");
            REQUIRE(await_use(9100 + i, 30, &jid, &csport), "fill job assigned");
            begin_on(jid, csport);
            held.push_back(Held{jid, csport});
        }
        int on_a = 0, on_c = 0;
        for (const Held &h : held) {
            if (h.port == 10262) { ++on_c; } else { ++on_a; }
        }
        fprintf(stderr, "# leastbusy: filled slots a=%d c=%d\n", on_a, on_c);
        REQUIRE(on_a == 2 && on_c == 2,
                "least_busy spread the fill evenly across equal hosts");

        /* Phase 2 -- every host now sits AT maxJobs (the preload zone).
           The picker must still assign: the bucketed form selected an empty
           set here and stalled until something completed.  */
        unsigned pjid = 0, pport = 0;
        REQUIRE(send_one(9200, "preload.cpp"), "preload-zone probe submitted");
        const bool assigned = await_use(9200, 10, &pjid, &pport);
        fprintf(stderr, "# leastbusy: preload-zone probe assigned=%s port=%u\n",
                assigned ? "yes" : "NO", pport);
        REQUIRE(assigned,
                "work is still assigned when every host is in its preload zone (SCH-6)");
        if (assigned) {
            begin_on(pjid, pport);
            done_on(pjid, pport);
        }

        /* Phase 3 -- unequal occupancy: empty host C completely, leave host
           A full.  The next job must land on C (0/2 beats 2/2 exactly).  */
        for (const Held &h : held) {
            if (h.port == 10262) { done_on(h.jid, h.port); }
        }
        usleep(500 * 1000);   // let the ENDs land
        unsigned qjid = 0, qport = 0;
        REQUIRE(send_one(9300, "emptier.cpp"), "occupancy probe submitted");
        REQUIRE(await_use(9300, 15, &qjid, &qport), "occupancy probe assigned");
        fprintf(stderr, "# leastbusy: occupancy probe went to port=%u (want 10262)\n", qport);
        REQUIRE(qport == 10262, "the emptier host wins the occupancy comparison");
        if (qjid) { begin_on(qjid, qport); done_on(qjid, qport); }
        for (const Held &h : held) {
            if (h.port != 10262) { done_on(h.jid, h.port); }
        }
        REQUIRE(worst_reply.load() < 5.0, "scheduler stayed responsive");

        shutdown = true;
        probe_thread.join(); cs_thread.join(); healthy_thread.join(); csC_thread.join();
        close(ctrl);
        delete sub; delete sub2; delete cs; delete csC;
        kill(sched, SIGTERM);
        waitpid(sched, nullptr, 0);
        if (failures) { fprintf(stderr, "RESULT: FAIL (%d)\n", failures); return 1; }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    fprintf(stderr, "# requesting %d jobs\n", njobs);
    /* Phase discipline (perf gate): everything before this point was setup
       -- discard those samples so 'ingress' means the flood, nothing else.  */
    /* Baseline the scheduler's lifetime admitted counter BEFORE the flood:
       it counts every submitter (the healthy one included), so comparing it
       directly against njobs declares ingress over early -- observed as
       627/600 and 325/300 in review.  The barrier below requires the
       DELTA.  */
    const long long admitted_baseline = query_submitter_admitted(port, "fakesub");
    const long long generation_baseline = query_submitter_generation(port, "fakesub");
    REQUIRE(admitted_baseline >= 0,
            "flood-submitter admission baseline was read before the flood");
    {
        std::lock_guard<std::mutex> lock(sample_mutex);
        probe_samples.clear();
    }
    phase_start_s = secs_since(t_prog);
    probe_phase = 0;
    for (int i = 1; i <= njobs; ++i) {
        char fname[64];
        snprintf(fname, sizeof(fname), "file%04d.cpp", i);
        GetCSMsg gcs(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                     fname, CompileJob::Lang_CXX, 1, kPlatform, 0, std::string(), 0, 0, 0);
        gcs.client_id = i;
        if (!sub->send_msg(gcs)) {
            fprintf(stderr, "FAILED   - submitter died while requesting job %d\n", i);
            ++failures;
            break;
        }
    }
    /* Server-observed barrier, on the DELTA over the baseline: the sender
       finishing send_msg() proves nothing about admission -- requests can
       still be buffered ahead of the scheduler.  Not reaching the barrier
       is a hard failure: a silently sender-observed boundary would make
       every ingress measurement below meaningless.  */
    {
        long long admitted_delta = -1;
        const Clock::time_point tb = Clock::now();
        while (secs_since(tb) < 120) {
            const long long now_admitted = query_submitter_admitted(port, "fakesub");
            if (now_admitted < 0) {
                usleep(200 * 1000);          // transient; retry within the 120s
                continue;
            }
            admitted_delta = now_admitted - admitted_baseline;
            if (admitted_delta >= (long long)njobs) {
                break;
            }
            usleep(100 * 1000);
        }
        fprintf(stderr, "# scheduler admitted %lld/%d (delta over baseline %lld)\n",
                admitted_delta, njobs, admitted_baseline);
        REQUIRE(query_submitter_generation(port, "fakesub") == generation_baseline,
                "connection generation unchanged across the barrier window");
        REQUIRE(admitted_delta >= (long long)njobs,
                "scheduler admitted the whole flood before the drain phase");
        if (admitted_delta < (long long)njobs) {
            /* Phase labels would be fiction from here on.  */
            fprintf(stderr, "RESULT: FAIL (ingress barrier not reached)\n");
            shutdown = true;
            probe_thread.join();
            cs_thread.join();
            healthy_thread.join();
            close(ctrl);
            delete sub; delete sub2; delete cs;
            kill(sched, SIGTERM);
            waitpid(sched, nullptr, 0);
            return 1;
        }
    }
    ingress_duration_s = secs_since(t_prog) - phase_start_s.load();
    probe_phase = 1;   // scheduler has admitted the flood: drain begins

    if (gate_mode) {
        fprintf(stderr, "# gate mode: submitter stops reading for %ds\n", clog_s);
        /* Count assignments PER SUBMITTER from the scheduler's own log.
           "put <id> in joblist of <cs>" names the COMPILE SERVER, so the
           submitter is recovered through the job id: "NEW <id>
           client=<submitter>" at creation, then each "put <id>" attributed
           through that map.  The name comparison is delimiter-aware --
           "fakesub" is a prefix of "fakesub2".  */
        auto count_assignments_to = [&](const char *name) {
            std::map<int, std::string> owner;
            int n = 0;
            const size_t name_len = strlen(name);
            FILE *lf = fopen(sched_log.c_str(), "r");
            if (lf) {
                char line[4096];
                while (fgets(line, sizeof(line), lf)) {
                    const char *nw = strstr(line, "NEW ");
                    const char *cl = nw ? strstr(nw, " client=") : nullptr;
                    if (nw && cl) {
                        const int id = atoi(nw + 4);
                        const char *cname = cl + strlen(" client=");
                        const char *cend = cname;
                        while (*cend && !isspace((unsigned char)*cend)) {
                            ++cend;
                        }
                        owner[id] = std::string(cname, cend - cname);
                        continue;
                    }
                    const char *put = strstr(line, "put ");
                    if (put && strstr(put, " in joblist of ")) {
                        const int id = atoi(put + 4);
                        auto it = owner.find(id);
                        if (it != owner.end()
                            && it->second.size() == name_len
                            && it->second.compare(name) == 0) {
                            ++n;
                        }
                    }
                }
                fclose(lf);
            }
            return n;
        };
        const int healthy_before = healthy_replies.load();
        const int paused_before = count_assignments_to("fakesub");
        const int healthy_puts_before = count_assignments_to("fakesub2");
        for (int i = 0; i < clog_s; ++i) {
            sleep(1);
        }
        /* Only assignments made DURING the freeze are attributable to the
           gate; earlier ones were confirmed normally.  Attribution is per
           submitter, so the healthy submitter's traffic can no longer mask
           an over-grant to the paused one.  */
        const int paused_puts = count_assignments_to("fakesub") - paused_before;
        const int healthy_puts = count_assignments_to("fakesub2") - healthy_puts_before;
        const int healthy_gain = healthy_replies.load() - healthy_before;
        /* The scheduler clamps the credit to farm slots - 1 on small farms;
           mirror that here.  +1 allows the assignment already in flight when
           the freeze began.  */
        int credit = 32;
        if (farm_slots > 0 && credit >= farm_slots) {
            credit = farm_slots > 1 ? farm_slots - 1 : 1;
        }
        fprintf(stderr, "# paused-submitter assignments while stalled: %d (credit %d),"
                        " healthy assignments: %d, healthy replies: %d\n",
                paused_puts, credit, healthy_puts, healthy_gain);
        REQUIRE(paused_puts <= credit + 1,
                "dispatch to the non-reading submitter stopped at its credit (BP-1)");
        REQUIRE(healthy_alive.load() && healthy_gain > 0,
                "a healthy submitter kept receiving replies while the other stalled");
        REQUIRE(healthy_puts > 0,
                "the healthy submitter kept receiving assignments while the other stalled");
        REQUIRE(worst_reply.load() < 5.0, "scheduler stayed responsive");
        REQUIRE(!probe_died.load(), "control connection survived");

        shutdown = true;
        probe_thread.join();
        cs_thread.join();
        healthy_thread.join();
        close(ctrl);
        delete sub;
        delete sub2;
        delete cs;
        kill(sched, SIGTERM);
        int status = 0;
        waitpid(sched, &status, 0);
        if (failures) {
            fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
            return 1;
        }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (stall_mode) {
        // ---- stall mode: never drain; the scheduler must cut us loose ------
        fprintf(stderr, "# stall mode: submitter never reads; waiting for the "
                "scheduler to enforce its deferred-send bound...\n");
        // Passive fd-watching cannot detect the teardown here: the
        // scheduler's FIN/RST cannot traverse our deliberately-zero receive
        // window.  Probe actively instead -- a small send to a torn-down
        // peer draws an RST and poisons the channel within a round trip.
        int death_t = -1;
        for (int i = 0; i < clog_s; ++i) {
            sleep(1);
#ifdef ICECC_MSGCHANNEL_HAS_DEFERRED_SEND
            bool ping_ok = sub->send_msg(PingMsg(),
                    MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable);
#else
            // 1.4-era API: a plain send is fine here -- our own send
            // direction is never backed up (the scheduler always reads).
            bool ping_ok = sub->send_msg(PingMsg());
#endif
            if (!ping_ok || sub->at_eof()) {
                death_t = i;
                break;
            }
        }
        fprintf(stderr, "# submitter connection torn down at t=%ds\n", death_t);
        /* Teardown is expected only when the stall actually armed a deferred
           send (unread socket -> 30s bound).  With the dispatch-credit gate
           the queued bytes may fit entirely in the socket buffers, in which
           case the submitter looks alive at TCP level and its exposure is
           bounded by the credit instead; the separate 180s unconfirmed-
           dispatch bound covers that case and is too slow for this run.  */
        bool deferral_armed = false;
        {
            FILE *lf = fopen(sched_log.c_str(), "r");
            if (lf) {
                char line[4096];
                while (fgets(line, sizeof(line), lf)) {
                    if (strstr(line, "deferring")) { deferral_armed = true; break; }
                }
                fclose(lf);
            }
        }
        fprintf(stderr, "# deferred send armed during the stall: %s\n",
                deferral_armed ? "yes" : "no (credit gate capped the backlog first)");
        if (deferral_armed) {
            REQUIRE(death_t >= 0, "scheduler tore down the never-draining submitter");
        }
        // BP-1 regression guard: a submitter with deferred (undelivered)
        // dispatch output must stop receiving new assignments.  The gate is
        // per-backlog-episode, so assignments continue while the kernel
        // still accepts bytes (sndbuf + peer rcvbuf, ~72 KiB here, i.e.
        // ~1-2.5k replies) and stop for good once that capacity is full --
        // the same physical exposure the old blocking send had, without the
        // wedge.  Pre-fix behaviour granted ALL requests; assert we stop
        // well short of that.
        {
            FILE *lf = fopen(sched_log.c_str(), "r");
            int puts_total = -1, puts_after_defer = -1;
            if (lf) {
                char line[4096];
                bool deferred_seen = false;
                puts_total = puts_after_defer = 0;
                while (fgets(line, sizeof(line), lf)) {
                    if (strstr(line, "deferring")) {
                        deferred_seen = true;
                    } else if (strstr(line, " in joblist of ")) {
                        ++puts_total;
                        if (deferred_seen) {
                            ++puts_after_defer;
                        }
                    }
                }
                fclose(lf);
            }
            fprintf(stderr, "# assignments: total=%d after-first-deferral=%d (of %d requests)\n",
                    puts_total, puts_after_defer, njobs);
            /* Exact bound: after the first deferral the stalled submitter may
               hold at most its dispatch credit (32) plus the assignment whose
               bytes became partial.  Anything beyond that means the scheduler
               kept reserving farm slots the client cannot use.  */
            REQUIRE(puts_after_defer >= 0 && puts_after_defer <= 33,
                    "at most credit+1 assignments after the first deferral (BP-1)");
            fprintf(stderr, "# healthy submitter replies during the stall: %d\n",
                    healthy_replies.load());
            REQUIRE(healthy_alive.load() && healthy_replies.load() > 0,
                    "a healthy submitter kept receiving dispatches throughout");
        }
        // The bound is 30s of undelivered output.  Much earlier means some
        // kernel-level timeout leaked into the test conditions; never (or
        // only at the very end of the window) means deferred bytes and the
        // submitter's WAITINGFORCS jobs can linger without bound.
        if (deferral_armed) {
            REQUIRE(death_t >= 20 && death_t <= 50,
                    "teardown honoured the ~30s deferred-send age bound");
        }
        REQUIRE(cs_alive.load(), "compile server connection survived");
        {
            double pending = probe_pending_since.load();
            if (pending >= 0) {
                double outage = secs_since(t_prog) - pending;
                double w = worst_reply.load();
                while (outage > w && !worst_reply.compare_exchange_weak(w, outage)) {
                }
            }
            fprintf(stderr, "# final worst control-reply latency %.1fs\n", worst_reply.load());
        }
        drain_end_s = secs_since(t_prog);
        print_phase_summary();
        REQUIRE(worst_reply.load() < 5.0,
                "scheduler stayed responsive (control replies < 5s)");
        REQUIRE(!probe_died.load(), "control connection survived");

        shutdown = true;
        probe_thread.join();
        cs_thread.join();
        healthy_thread.join();
        close(ctrl);
        delete sub;
        delete sub2;
        delete cs;
        kill(sched, SIGTERM);
        int status = 0;
        waitpid(sched, &status, 0);

        if (failures) {
            fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
            return 1;
        }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    // ---- the backpressure window: read nothing at all ----------------------
    fprintf(stderr, "# submitter reads nothing for %ds...\n", clog_s);
    for (int i = 0; i < clog_s; ++i) {
        sleep(1);
    }

    // ---- drain and account -------------------------------------------------
    std::map<unsigned int, int> replies;   // client_id -> count
    int parsed = 0;
    bool channel_error = false;

    fprintf(stderr, "# draining\n");
    Clock::time_point drain_start = Clock::now();
    Clock::time_point last_progress = Clock::now();
    while (secs_since(drain_start) < 120) {
        Msg *m = sub->get_msg(3, true);
        if (!m) {
            if (sub->at_eof()) {
                channel_error = true;
                break;
            }
            // a lull is only conclusive after 15s without any progress --
            // a busy scheduler can legitimately pause mid-drain
            if (secs_since(last_progress) > 15) {
                break;
            }
            continue;
        }
        last_progress = Clock::now();
        if (MSG_IS(m, USE_CS)) {
            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
            if (u && u->port == kCsPort && u->client_id >= 1
                    && u->client_id <= (unsigned)njobs) {
                ++replies[u->client_id];
                ++parsed;
                confirm_job(u->job_id);
            } else {
                fprintf(stderr, "# corrupt USE_CS: client_id=%u port=%u host=%s\n",
                        u ? u->client_id : 0, u ? u->port : 0,
                        u ? u->hostname.c_str() : "?");
                channel_error = true;
            }
        } else if (MSG_IS(m, NO_CS)) {
            fprintf(stderr, "# unexpected NO_CS\n");
        }
        delete m;
        if (parsed == njobs) {
            while ((m = sub->get_msg(1, true)) != nullptr) {
                delete m;
            }
            break;
        }
    }

    int exactly_once = 0;
    for (const auto &kv : replies) {
        if (kv.second == 1) {
            ++exactly_once;
        }
    }

    fprintf(stderr, "# %d parseable USE_CS replies, %d/%d jobs answered exactly once, "
            "channel_error=%d, worst control-reply latency %.1fs\n",
            parsed, exactly_once, njobs, (int)channel_error, worst_reply.load());

    REQUIRE(!channel_error && !sub->at_eof(),
            "submitter connection survived the backpressure window");
    REQUIRE(exactly_once == njobs,
            "every job got exactly one intact USE_CS reply after draining");
    REQUIRE(cs_alive.load(), "compile server connection survived");
    // include a probe round that is still waiting for its reply right now:
    // that outage is already at least this long
    {
        double pending = probe_pending_since.load();
        if (pending >= 0) {
            double outage = secs_since(t_prog) - pending;
            double w = worst_reply.load();
            while (outage > w && !worst_reply.compare_exchange_weak(w, outage)) {
            }
        }
        fprintf(stderr, "# final worst control-reply latency %.1fs\n", worst_reply.load());
    }
    drain_end_s = secs_since(t_prog);
    print_phase_summary();
    REQUIRE(worst_reply.load() < 5.0,
            "scheduler stayed responsive (control replies < 5s)");
    // A scheduler wedged in blocking sends for > MAX_SCHEDULER_PING stops
    // processing its control clients' commands, so prune_servers() kills
    // them: losing the control connection is itself unresponsiveness.
    REQUIRE(!probe_died.load(), "control connection survived (not pruned by a wedged scheduler)");


    shutdown = true;
    probe_thread.join();
    cs_thread.join();
    healthy_thread.join();
    close(ctrl);
    delete sub;
    delete sub2;
    delete cs;
    kill(sched, SIGTERM);
    int status = 0;
    waitpid(sched, &status, 0);

    if (failures) {
        fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
