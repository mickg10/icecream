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
                             const char *verbosity)
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
    execl(binary.c_str(), binary.c_str(), "-p", portbuf, verbosity, (char *)nullptr);
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
    pid_t sched = start_scheduler(scheduler_bin, shim, port, "schedbp-scheduler.log",
                                  perf_mode ? "-v" : "-vvv");
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
        login.max_kids = farm_slots > 0 ? farm_slots : njobs + 16;
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
    std::vector<unsigned int> to_confirm;
    auto confirm_job = [&](unsigned int job_id) {
        std::lock_guard<std::mutex> lock(confirm_mutex);
        to_confirm.push_back(job_id);
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
                std::vector<unsigned int> batch;
                {
                    std::lock_guard<std::mutex> lock(confirm_mutex);
                    batch.swap(to_confirm);
                }
                for (unsigned int jid : batch) {
                    JobBeginMsg jb(jid, 1);
                    if (!cs->send_msg(jb)) {
                        cs_alive = false;
                        return;
                    }
                    /* Complete the job immediately so the slot recycles.
                       On a small farm this is what lets a healthy submitter
                       keep flowing through the capacity the clamp reserves
                       for it; unconfirmed (never-read) assignments keep
                       holding their slots, exactly like a real dead client.  */
                    JobDoneMsg jd(jid, 0, JobDoneMsg::FROM_SERVER);
                    if (!cs->send_msg(jd)) {
                        cs_alive = false;
                        return;
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
            fprintf(stderr,
                    "# perf %s samples=%zu p95=%.3f p99=%.3f max=%.3f\n",
                    ph == 0 ? "ingress" : "drain", v.size(),
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
            for (int i = 0; i < 2 && !shutdown; ++i) {
                usleep(100 * 1000);
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
        LoginMsg login(0, "fakesub", kPlatform, 0);
        login.envs.push_back(std::make_pair(kPlatform, kEnv));
        login.max_kids = 0;    // never eligible for local fallback
        login.noremote = true;
        login.chroot_possible = false;
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

    fprintf(stderr, "# requesting %d jobs\n", njobs);
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
    probe_phase = 1;   // ingress over: everything from here is drain

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
            FILE *lf = fopen("schedbp-scheduler.log", "r");
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
            FILE *lf = fopen("schedbp-scheduler.log", "r");
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
            FILE *lf = fopen("schedbp-scheduler.log", "r");
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
