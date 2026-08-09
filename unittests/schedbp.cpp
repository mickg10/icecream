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
#include <fcntl.h>
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
#include <set>
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
                             const char *verbosity, const char *extra_arg = nullptr,
                             const char *extra_arg2 = nullptr)
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
    if (extra_arg && extra_arg2) {
        execl(binary.c_str(), binary.c_str(), "-p", portbuf, verbosity, extra_arg, extra_arg2,
              (char *)nullptr);
    } else if (extra_arg) {
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

/* Bounded loopback connect: nonblocking connect completed with poll under
   the caller's deadline, then restored to blocking.  A wedged listener can
   otherwise park a blocking connect() outside every exchange clock.  */
static int tcp_connect_bounded(int port, int timeout_ms)
{
    int fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (errno != EINPROGRESS) {
            close(fd);
            return -1;
        }
        struct pollfd pf = { fd, POLLOUT, 0 };
        if (poll(&pf, 1, timeout_ms) <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    if (fcntl(fd, F_SETFL, flags) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The one control-exchange primitive every observation helper uses.  A
   reply is COMPLETE only when the "200 done" terminator arrived inside the
   whole-exchange deadline; INCOMPLETE (timeout/EOF mid-reply) and FAILED
   (no connection, no greeting, short write) are distinct, and callers must
   treat both as observation failures -- NEVER as an empty reply.  This is
   the repair for two helpers that fail-opened: one initialized its count
   to zero after a successful command write even when the terminator never
   came, so a dead scheduler read as "job absent"; the other accepted a
   field parsed from a partial line.  */
struct CtrlReply {
    enum Status { FAILED, INCOMPLETE, COMPLETE };
    Status status;
    std::string text;
};

static CtrlReply ctrl_exchange(int port, const char *command,
                               int deadline_ms = 8000)
{
    CtrlReply r;
    r.status = CtrlReply::FAILED;
    const Clock::time_point t0 = Clock::now();
    auto left_ms = [&]() -> int {
        const int ms = deadline_ms - (int)(secs_since(t0) * 1000.0);
        return ms > 0 ? ms : 0;
    };
    const int fd = tcp_connect_bounded(port + 1, deadline_ms);
    if (fd < 0) {
        return r;
    }
    char buf[16384];
    {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, left_ms()) <= 0) {
            close(fd);
            return r;   /* FAILED: no greeting */
        }
        const ssize_t g = read(fd, buf, sizeof(buf) - 1);
        if (g <= 0) {
            close(fd);
            return r;
        }
    }
    std::string cmdline = std::string(command) + "\n";
    if (write(fd, cmdline.c_str(), cmdline.size()) != (ssize_t)cmdline.size()) {
        close(fd);
        return r;
    }
    r.status = CtrlReply::INCOMPLETE;   /* command sent; reply not yet terminal */
    while (left_ms() > 0) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, left_ms()) <= 0) {
            break;
        }
        const ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            break;
        }
        buf[n] = 0;
        r.text += buf;
        if (r.text.find("200 done") != std::string::npos) {
            r.status = CtrlReply::COMPLETE;
            break;
        }
    }
    close(fd);
    return r;
}

/* Strict nonnegative integer parse at `p`: digits, bounded, terminated by
   a delimiter (space/newline/slash/end).  Returns -1 on anything else --
   a malformed row must fail the observation, not contribute zero.  */
static long long parse_field_ll(const char *p)
{
    if (!p || *p < '0' || *p > '9') {
        return -1;
    }
    char *end = nullptr;
    errno = 0;
    const long long v = strtoll(p, &end, 10);
    if (errno != 0 || v < 0 || end == p) {
        return -1;
    }
    if (*end != 0 && *end != ' ' && *end != '\n' && *end != '\r' && *end != '/') {
        return -1;
    }
    return v;
}

/* One control-port round trip returning the scheduler's lifetime
   jobs_admitted counter, or -1 if it cannot be read.  Used to baseline and
   then observe the flood's admission from the server's own accounting.  */
/* Per-submitter admitted total from listcs ("admitted_total=N" on the line
   whose node name matches).  Scoping the barrier to the FLOOD submitter is
   what makes it honest: the global counter includes the healthy
   submitter's concurrent traffic, which inflated every delta by ~4.  */
static long long query_submitter_field(int port, const char *name, const char *field);
/* Same round trip for an arbitrary control command: returns the numeric
   value of `field` on the first reply line containing `needle`.  */
static long long query_control_field(int port, const char *command,
                                     const char *needle_str, const char *field)
{
    const CtrlReply r = ctrl_exchange(port, command);
    if (r.status != CtrlReply::COMPLETE) {
        return -1;   /* observation failure: never a value */
    }
    const size_t pos = r.text.find(needle_str);
    if (pos == std::string::npos) {
        return -1;
    }
    const size_t eol = r.text.find('\n', pos);
    const std::string line = r.text.substr(pos, eol == std::string::npos
                                                ? std::string::npos : eol - pos);
    const size_t sp = line.find(field);
    if (sp == std::string::npos) {
        return -1;
    }
    return parse_field_ll(line.c_str() + sp + strlen(field));
}

/* Full COMPLETE control reply text (or empty on failure) -- for reading
   the internals snapshot fields.  */
static std::string control_dump(int port, const char *command)
{
    const CtrlReply r = ctrl_exchange(port, command);
    return r.status == CtrlReply::COMPLETE ? r.text : std::string();
}

static long long query_control_count(int port, const char *command, const char *needle_str)
{
    const CtrlReply r = ctrl_exchange(port, command);
    if (r.status != CtrlReply::COMPLETE) {
        return -1;   /* observation failure: never "zero matches" */
    }
    long long count = 0;
    size_t pos = 0;
    while ((pos = r.text.find(needle_str, pos)) != std::string::npos) {
        ++count;
        pos += strlen(needle_str);
    }
    return count;
}

static bool job_in_scheduler(int port, unsigned job_id)
{
    char needle[64];
    snprintf(needle, sizeof(needle), " %u ", job_id);
    /* dump_job() prints "<id> <STATE> ... " so the id is the first field of
       a line that begins with a space.  */
    return query_control_count(port, "listjobs", needle) > 0;
}

/* Total advertised compile slots across the whole fake farm: the sum of
   the MAX half of every "jobs=cur/max" in listcs.  Derived from the live
   topology so the capacity bound in the fairness bracket carries no
   unexplained constant.  */
static long long total_farm_capacity(int port)
{
    /* Sum the MAX half of every "jobs=cur/max" from one COMPLETE listcs
       snapshot.  Bounded end-to-end (nonblocking connect under the same
       deadline as the reads), strict per-row parse, and -1 on ANY failure
       so the caller rejects the observation: a truncated or malformed
       measurement must never become a small capacity that makes the
       backlog inequality trivially true.  */
    const CtrlReply r = ctrl_exchange(port, "listcs");
    if (r.status != CtrlReply::COMPLETE) {
        return -1;
    }
    long long total = 0;
    size_t pos = 0;
    while ((pos = r.text.find("jobs=", pos)) != std::string::npos) {
        const size_t slash = r.text.find('/', pos);
        const size_t eol = r.text.find('\n', pos);
        if (slash == std::string::npos || (eol != std::string::npos && slash > eol)) {
            return -1;   /* malformed row */
        }
        const long long v = parse_field_ll(r.text.c_str() + slash + 1);
        if (v < 0) {
            return -1;   /* malformed max half */
        }
        total += v;
        pos = slash + 1;
    }
    return total > 0 ? total : -1;   /* a real farm has capacity */
}

static long long worker_job_count(int port, const char *worker)
{
    /* listcs prints "... jobs=<current>/<max> ..." for each host.  */
    char needle[64];
    snprintf(needle, sizeof(needle), " %s (", worker);
    return query_control_field(port, "listcs", needle, "jobs=");
}

/* Delimiter-aware per-node field from a COMPLETE listcs snapshot: the
   node-name match requires " <name> (" so a prefix ("fakesub" inside
   "fakesub2") can never select the wrong row, and the value parse is
   strict (digits + delimiter) so a malformed row fails the observation
   instead of contributing a number.  */
static long long field_from_snapshot(const std::string &snapshot,
                                     const char *name, const char *field)
{
    const std::string needle = std::string(" ") + name + " (";
    size_t pos = snapshot.find(needle);
    if (pos == std::string::npos) {
        return -1;
    }
    const size_t eol = snapshot.find('\n', pos);
    const std::string line = snapshot.substr(pos, eol == std::string::npos
                                                  ? std::string::npos : eol - pos);
    const size_t sp = line.find(field);
    if (sp == std::string::npos) {
        return -1;
    }
    return parse_field_ll(line.c_str() + sp + strlen(field));
}

static long long query_submitter_field(int port, const char *name, const char *field)
{
    const CtrlReply r = ctrl_exchange(port, "listcs");
    if (r.status != CtrlReply::COMPLETE) {
        return -1;
    }
    return field_from_snapshot(r.text, name, field);
}

static long long query_submitter_admitted(int port, const char *name)
{
    return query_submitter_field(port, name, "admitted_total=");
}

static long long query_submitter_generation(int port, const char *name)
{
    return query_submitter_field(port, name, "gen=");
}

static long long query_submitter_outstanding(int port, const char *name)
{
    return query_submitter_field(port, name, "outstanding=");
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
    /* "stallcredit": a submitter whose dispatched jobs never reach JobBegin
       is bounded by its dispatch CREDIT, not removed.  It reads its replies
       -- so it is a responsive connection whose wrappers stalled, not a
       dead daemon -- and the scheduler must deliver exactly the credit,
       report the stall, retain everything, and keep serving its healthy
       peer.  Removing a genuinely absent daemon is the deferred-output
       deadline's job (see the stall/transient modes), not this bound's.  */
    const bool stallcredit_mode = argc > 5 && strcmp(argv[5], "stallcredit") == 0;
    /* "leastbusy": the SCH-6 selection gate, run with -a least_busy.  With
       every host in its preload zone (count == maxJobs) the picker must
       still assign work -- the two-pass bucketed form selected an empty set
       and answered "no suitable host" while preload capacity existed -- and
       with unequal occupancies the emptier host must win.  */
    const bool leastbusy_mode = argc > 5 && strcmp(argv[5], "leastbusy") == 0;
    /* "clientstall": the client-isolation gate.  A submitting daemon proxies
       EVERY compiler wrapper on its host.  One wrapper frozen after its
       assignment holds a dispatch debit that JobBegin never credits; the
       scheduler used to answer that by removing the whole daemon, voiding
       every healthy sibling's work.  Only the stale assignment may be
       expired: the daemon stays registered and its other clients keep
       being served across the bound.  */
    const bool clientstall_mode = argc > 5 && strcmp(argv[5], "clientstall") == 0;
    /* "teardown": a job already COMPILING on a live worker must survive its
       SUBMITTER disconnecting -- the worker is still running the compiler,
       so the scheduler must retain the job and the worker reservation until
       the worker's real JobDone, not delete it (which frees the slot under
       a running compile and strands the real JobDone as an unknown id).
       The counterexample this locks out is stock upstream behaviour that a
       formal ownership model surfaced.  */
    const bool teardown_mode = argc > 5 && strcmp(argv[5], "teardown") == 0;
    /* "internalsuaf": fan out `internals` to two workers, then disconnect
       them mid-transaction so the finalize path iterates a target whose
       CompileServer was just freed -- the exact use-after-free the
       round-2 re-review found in the pointer-keyed transaction.  Green
       under ASan proves the value-record fix; the pointer-keyed variant
       is the red control.  */
    const bool internalsuaf_mode = argc > 5 && strcmp(argv[5], "internalsuaf") == 0;
    /* "duplocal": a duplicate local-job Begin for a client-local id that is
       already mapped must be an idempotent no-op -- one allocation, one
       monitor Begin, one terminal -- and a duplicate/unknown local Done
       must not emit a terminal for global id 0.  Correction B.2a; the
       oracle noted B had no discriminating trace.  */
    const bool duplocal_mode = argc > 5 && strcmp(argv[5], "duplocal") == 0;
    /* "exhaust": a batch whose remaining members exceed the wire-id domain
       must fail FATALLY -- the submitter generation closed, no staged
       survivors, unrelated peers live -- not park and retry the same
       member forever.  Correction B.2b; driven by a tiny id domain.  */
    const bool exhaust_mode = argc > 5 && strcmp(argv[5], "exhaust") == 0;
    if (exhaust_mode) {
        /* The forked scheduler inherits this: a domain of 8 makes a
           count=20 batch un-completable.  */
        setenv("ICECC_TEST_JOB_ID_DOMAIN", "8", 1);
    }
    /* "internalsrace": the same-turn flush/reply race.  With tick
       promotion disabled (below), the ONLY way a reply is accepted is the
       STATUS_TEXT handler's same-turn recheck -- so this deterministically
       isolates that fix.  */
    const bool internalsrace_mode = argc > 5 && strcmp(argv[5], "internalsrace") == 0;

    if (internalsrace_mode) {
        setenv("ICECC_TEST_INTERNALS_NO_TICK_PROMOTE", "1", 1);
    }
    /* "internalsguard": the active control issues a SECOND command mid
       fan-out; the same-control guard must fail its exact generation (its
       reply would otherwise bypass the reserved-tail accounting).  Keep
       the txn alive with the no-tick-promote knob + a silent worker.  */
    const bool internalsguard_mode = argc > 5 && strcmp(argv[5], "internalsguard") == 0;
    if (internalsguard_mode) {
        setenv("ICECC_TEST_INTERNALS_NO_TICK_PROMOTE", "1", 1);
    }
    /* "retention": the proof that Stage-A retention is CORRECT, not merely
       non-destructive.  A quiet-but-healthy daemon crossing the report
       threshold, a sibling legitimately running across it, dispatch
       continuing while a wrapper is stuck, exactly-once reporting, and --
       the central property -- a LATE THAW whose Begin/Done still reconcile
       the retained assignment exactly once.  */
    const bool retention_mode = argc > 5 && strcmp(argv[5], "retention") == 0;
    /* "noreader": the honest stopped-daemon model.  The submitter's process
       stops consuming its socket entirely, but with a one-assignment credit
       the single small reply fits in the kernel's buffers, so TCP keeps
       ACKing and NO deferred-output episode ever arms.  Neither connection
       failure nor the 30s drain deadline can fire; Stage A's documented
       behaviour is that the assignment stays owned -- possibly until the
       daemon resumes or the connection really ends -- while the submitter
       is reported, is capped at its credit, and everyone else progresses.  */
    const bool noreader_mode = argc > 5 && strcmp(argv[5], "noreader") == 0;
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
    signal(SIGPIPE, SIG_IGN);

    /* Unique per RUN, not just per mode: two copies of one mode can run
       concurrently, and a shared log file once turned a real FAIL into a
       recorded PASS in this project.  */
    char runtag[64];
    snprintf(runtag, sizeof(runtag), "%s-%d", argc > 5 ? argv[5] : "default", (int)getpid());
    const std::string sched_log = std::string("schedbp-scheduler-") + runtag + ".log";

    const char *sched_extra =
        (stallcredit_mode || clientstall_mode || retention_mode || noreader_mode)
            ? "--dispatch-stall-report-after=10"
        : (leastbusy_mode ? "--algorithm=least_busy" : nullptr);
    const char *sched_extra2 = noreader_mode ? "--max-outstanding-dispatches=1" : nullptr;

    /* Allocation and startup are ONE bounded attempt loop.  The probe below
       closes its sockets before the child binds, so another process can
       take the pair in that window; previously that surfaced as the child
       exiting during startup, which returned immediately without ever
       reaching the retry.  Each attempt probes a fresh pair, spawns, and
       classifies the result: listener reachable is success; exit 127 is an
       exec/configuration failure no retry can help; any other exit or ten
       silent seconds is a lost race, so reap and try the next pair.  */
    int port = 0;
    pid_t sched = -1;
    {
        int cand = 25000 + (getpid() % 1000);
        for (int attempt = 0; attempt < 5 && sched < 0; ++attempt) {
            int chosen = 0;
            for (; cand < 25000 + 4000; cand += 2) {
                bool both_free = true;
                for (int off = 0; off < 2 && both_free; ++off) {
                    const int fd = socket(PF_INET, SOCK_STREAM, 0);
                    if (fd < 0) { both_free = false; break; }
                    int on = 1;
                    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
                    struct sockaddr_in sa;
                    memset(&sa, 0, sizeof(sa));
                    sa.sin_family = AF_INET;
                    sa.sin_port = htons(cand + off);
                    sa.sin_addr.s_addr = htonl(INADDR_ANY);
                    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
                        both_free = false;
                    }
                    close(fd);
                }
                if (both_free) { chosen = cand; cand += 2; break; }
            }
            if (chosen == 0) {
                fprintf(stderr, "no free scheduler port pair found\n");
                return 2;
            }
            pid_t child = start_scheduler(scheduler_bin, shim, chosen, sched_log,
                                          perf_mode ? "-v" : "-vvv", sched_extra, sched_extra2);
            if (child < 0) {
                perror("fork");
                return 2;
            }
            bool up = false, exec_failed = false, exited = false;
            for (int i = 0; i < 100 && !up; ++i) {
                int status = 0;
                if (waitpid(child, &status, WNOHANG) == child) {
                    exited = true;
                    exec_failed = WIFEXITED(status) && WEXITSTATUS(status) == 127;
                    fprintf(stderr, "scheduler exited during startup (%s %d)\n",
                            WIFEXITED(status) ? "exit code" : "signal",
                            WIFEXITED(status) ? WEXITSTATUS(status)
                                              : (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
                    break;
                }
                int probe = tcp_connect(chosen, 0);
                if (probe >= 0) {
                    close(probe);
                    up = true;
                    break;
                }
                usleep(100 * 1000);
            }
            if (exec_failed) {
                return 2;
            }
            if (up) {
                sched = child;
                port = chosen;
                break;
            }
            if (!exited) {
                kill(child, SIGTERM);
                waitpid(child, nullptr, 0);
            }
            fprintf(stderr, "startup attempt %d on port %d lost; retrying\n",
                    attempt + 1, chosen);
        }
        if (sched < 0) {
            fprintf(stderr, "scheduler could not be started after retries\n");
            return 2;
        }
    }

    fprintf(stderr, "# port=%d jobs=%d clog=%ds\n", port, njobs, clog_s);

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
        unsigned int out_uncompressed;   // >= 4096 to reach add_job_stats' body
    };
    std::vector<Confirm> to_confirm;
    auto enqueue_confirm = [&](unsigned int job_id, bool begin, bool done,
                               unsigned int real_msec) {
        std::lock_guard<std::mutex> lock(confirm_mutex);
        to_confirm.push_back(Confirm{job_id, begin, done, real_msec, 8192});
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
    /* The scheduler PROBES every remote-capable daemon's advertised port
       and marks it not-accepting -- hence ineligible for dispatch -- when
       the connect is refused.  The old probe misread a refused connect as
       a success (SO_ERROR is clear-on-read, and the verdict was polled
       twice per wake), which MASKED that this harness never listened on
       the ports its fake workers advertise: the suite only stayed green
       through that product bug.  With the probe honest, the fake workers
       must be genuinely reachable -- accept and immediately close.  */
    auto probe_acceptor = [&](int port) {
        const int lfd = socket(AF_INET, SOCK_STREAM, 0);
        if (lfd < 0) { return; }
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in la;
        memset(&la, 0, sizeof(la));
        la.sin_family = AF_INET;
        la.sin_addr.s_addr = htonl(INADDR_ANY);
        la.sin_port = htons((uint16_t)port);
        if (bind(lfd, (struct sockaddr *)&la, sizeof(la)) != 0
                || listen(lfd, 16) != 0) {
            close(lfd);
            return;
        }
        while (!shutdown) {
            struct pollfd pf = { lfd, POLLIN, 0 };
            if (poll(&pf, 1, 200) > 0) {
                const int c = accept(lfd, nullptr, nullptr);
                if (c >= 0) { close(c); }
            }
        }
        close(lfd);
    };
    /* One acceptor per port any fake worker in this harness advertises
       (fakecs, the teardown alt worker, fakecsB/aarch64, fakecsC).  */
    for (const int accept_port : { (int)kCsPort, 10250, 10261, 10262 }) {
        std::thread t([&, accept_port] { probe_acceptor(accept_port); });
        t.detach();
    }

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
                        jd.user_msec = c.real_msec;
                        jd.out_uncompressed = c.out_uncompressed;
                        jd.in_uncompressed = c.out_uncompressed;
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
        if (leastbusy_mode || retention_mode || teardown_mode || internalsuaf_mode
                || duplocal_mode || exhaust_mode || internalsrace_mode
                || internalsguard_mode) {
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

        // ---- case 5: request-lifetime environment pinning at count=2000
        // The request offers TWO environments; dispatching the master pins
        // every sibling to the master's choice.  The observable is the
        // scheduler's own queue: after the first reply, listrequests must
        // show ZERO queued siblings still carrying several environments.
        // The id-based anchor reproducibly failed here (255 chained / 1744
        // broken): the master dispatched and completed between admission
        // steps and every later sibling escaped the pinning.  master= log
        // lines stay as a diagnostic; the queue census is the semantic
        // oracle.
        {
            const unsigned big = 2000;
            GetCSMsg g(Environments{
                           std::make_pair(std::string(kPlatform), std::string(kEnv)),
                           std::make_pair(std::string(kPlatform), std::string("testenv2"))},
                       "pinning.cpp", CompileJob::Lang_CXX, big, kPlatform, 0,
                       std::string(), 0, 0, 0);
            g.client_id = 501;
            REQUIRE(sub->send_msg(g), "count=2000 dual-environment request sent");

            std::vector<unsigned> big_ids;
            unsigned first_id = 0;
            bool census_taken = false;
            long long census_multi = -1, census_count = -1;
            const Clock::time_point t0 = Clock::now();
            while (big_ids.size() < big && secs_since(t0) < 240) {
                Msg *m = sub->get_msg(2);
                if (!m) { continue; }
                unsigned jid = 0;
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u && u->client_id == 501) { jid = u->job_id; }
                } else if (MSG_IS(m, NO_CS)) {
                    NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                    if (n && n->client_id == 501) { jid = n->job_id; }
                }
                if (jid) {
                    big_ids.push_back(jid);
                    confirm_job(jid);
                    if (!first_id || jid < first_id) { first_id = jid; }
                    if (!census_taken) {
                        /* First reply: the master has been dispatched, so
                           the narrowing has run.  Census the queue NOW,
                           while ~1900+ siblings are still queued.  */
                        census_taken = true;
                        census_multi = query_control_field(port, "listrequests",
                                                           "submitter=fakesub ", "multi_env=");
                        census_count = query_control_field(port, "listrequests",
                                                           "submitter=fakesub ", "count=");
                    }
                }
                delete m;
            }
            fprintf(stderr, "# contract: pinning replies=%zu/%u census count=%lld multi_env=%lld\n",
                    big_ids.size(), big, census_count, census_multi);
            REQUIRE(big_ids.size() == big, "count=2000 yields exactly 2000 replies");
            REQUIRE(census_taken && census_count >= 1000,
                    "the census sampled a still-loaded queue (test precondition)");
            REQUIRE(census_multi == 0,
                    "no queued sibling still carries several environments (request-lifetime pinning)");

            std::map<unsigned, unsigned> master_of;
            FILE *lf = fopen(sched_log.c_str(), "r");
            if (lf) {
                char line[4096];
                while (fgets(line, sizeof(line), lf)) {
                    const char *nw = strstr(line, "NEW ");
                    const char *ms = nw ? strstr(nw, " master=") : nullptr;
                    if (nw && ms) {
                        master_of[(unsigned)atoi(nw + 4)] = (unsigned)atoi(ms + 8);
                    }
                }
                fclose(lf);
            }
            int chained = 0, broken = 0;
            for (unsigned id : big_ids) {
                if (id == first_id) { continue; }
                std::map<unsigned, unsigned>::const_iterator mit = master_of.find(id);
                if (mit != master_of.end() && mit->second == first_id) { ++chained; }
                else { ++broken; }
            }
            fprintf(stderr, "# contract: master chain %d chained / %d broken (diagnostic)\n",
                    chained, broken);
            REQUIRE(broken == 0 && chained == (int)big - 1,
                    "every sibling logs the ORIGINAL master id (diagnostic)");
        }

        // ---- case 4: disconnect mid-expansion, then a fresh connection
        {
            long long doomed_generation = -1;
            MsgChannel *subC = connect_daemon(port, 0);
            REQUIRE(subC != nullptr, "doomed daemon connected");
            if (subC) {
                LoginMsg login(0, "fakesub4", kPlatform, 0);
                login.envs.push_back(std::make_pair(kPlatform, kEnv));
                login.max_kids = 0;
                login.noremote = true;
                REQUIRE(subC->send_msg(login), "doomed daemon logged in");
                /* The precondition must be OBSERVED, not assumed: the first
                   version of this case waited for ten replies, and the log
                   showed all 200 admissions had completed long before the
                   close.  Even counter polling loses the race -- a 2000-job
                   admission finishes in tens of milliseconds.  Deterministic
                   variant: close IMMEDIATELY after sending, so the FIN is
                   read in the same drain as the first 64-job quantum and
                   teardown provably interrupts the expansion; the scheduler
                   log then shows how many NEW records the request got, and
                   the assertion holds that number strictly inside
                   (0, requested).  The generation stamps are the connection
                   identities: the successor must carry a LATER one.  */
                {
                    const Clock::time_point tg = Clock::now();
                    while (secs_since(tg) < 10
                           && (doomed_generation =
                               query_submitter_generation(port, "fakesub4")) < 0) {
                        usleep(50 * 1000);
                    }
                    REQUIRE(doomed_generation > 0, "doomed daemon registered (generation read)");
                }
                REQUIRE(send_count(subC, 2000, 301, "contractD.cpp"),
                        "doomed daemon requested count=2000");
                delete subC;      // abrupt close: FIN races the expansion
                subC = nullptr;
                usleep(500 * 1000);   // let teardown finish server-side
                int admitted_doomed = 0;
                {
                    FILE *lf = fopen(sched_log.c_str(), "r");
                    if (lf) {
                        char line[4096];
                        while (fgets(line, sizeof(line), lf)) {
                            if (strstr(line, "NEW ") && strstr(line, "contractD.cpp")) {
                                ++admitted_doomed;
                            }
                        }
                        fclose(lf);
                    }
                }
                fprintf(stderr, "# contract: teardown with admitted=%d/2000 (gen=%lld)\n",
                        admitted_doomed, doomed_generation);
                REQUIRE(admitted_doomed >= 64 && admitted_doomed <= 1900,
                        "teardown demonstrably interrupted the expansion (0 < admitted < requested)");
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
                {
                    long long g5 = -1;
                    const Clock::time_point tg = Clock::now();
                    while (secs_since(tg) < 10
                           && (g5 = query_submitter_generation(port, "fakesub5")) < 0) {
                        usleep(100 * 1000);
                    }
                    fprintf(stderr, "# contract: successor gen=%lld (doomed gen was earlier)\n", g5);
                    REQUIRE(g5 > doomed_generation && doomed_generation > 0,
                            "the successor carries a LATER connection generation (distinct identity)");
                }
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

        // ---- case 6: pending-vs-ingress fairness under two LONG expansions
        // Two daemons expand count=4000 each while the main connection
        // submits thirty count=1 requests.  Each small request must
        // complete within a bounded interval WHILE the large expansions are
        // in flight -- eventual completion (case 3) is not fairness.  The
        // explicit service bound: a pending-first turn spends at most one
        // 64-job quantum, so ingress and the control plane always get the
        // rest of the budget.
        {
            /* Large enough that the contended admission window comfortably
               spans a rapid burst of ten small requests.  */
            const unsigned bigN = 24000;
            std::atomic<int> repliesF1{0}, repliesF2{0};
            std::atomic<bool> stop_bigs{false};
            std::atomic<bool> reply_budget_armed{false};
            std::atomic<int> reply_budget_remaining{0};
            auto big_daemon = [&](const char *name, unsigned cid,
                                  std::atomic<int> *counter) {
                MsgChannel *ch = connect_daemon(port, 0);
                if (!ch) { return; }
                LoginMsg login(0, name, kPlatform, 0);
                login.envs.push_back(std::make_pair(kPlatform, kEnv));
                login.max_kids = 0;
                login.noremote = true;
                if (!ch->send_msg(login)) { delete ch; return; }
                GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                           "fair.cpp", CompileJob::Lang_CXX, bigN, kPlatform, 0,
                           std::string(), 0, 0, 0);
                g.client_id = cid;
                if (!ch->send_msg(g)) { delete ch; return; }
                /* Progress-based: the property here is STARVATION-freedom
                   and bracketed small-request latency, not bulk throughput
                   -- a loaded box legitimately runs at a fraction of idle
                   speed, and exact-completion accounting is already proven
                   by the multicount case at a size every box can finish.
                   Drain while replies arrive; a 60s stall is a FAILURE (a
                   starved queue, not a slow one); when the main flow has
                   its brackets it raises stop_bigs and the remainder is
                   ended with the batch cancel -- which is itself coverage:
                   a many-thousand-member cancel must leave no ghosts.  */
                const Clock::time_point t0 = Clock::now();
                Clock::time_point last_progress = t0;
                bool stalled = false;
                while (*counter < (int)bigN && !stop_bigs.load()) {
                    if (secs_since(last_progress) >= 60) {
                        stalled = true;
                        break;
                    }
                    Msg *m = ch->get_msg(2);
                    if (!m) { continue; }
                    if (MSG_IS(m, USE_CS)) {
                        UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                        if (u && u->client_id == cid) {
                            confirm_job(u->job_id);
                            ++*counter;
                            last_progress = Clock::now();
                            /* CONSTRUCT the contended window: once enough
                               replies have arrived to open it, pace
                               consumption so a fast host cannot drain the
                               whole expansion before the ten small requests
                               run.  While the ARITHMETIC BUDGET is armed
                               (during the small-request bracket), each
                               further reply spends from a fixed shared
                               budget and consumption PAUSES when it is
                               exhausted: with admission nondecreasing, the
                               pre-bracket margin minus this budget bounds
                               the whole interval from below -- the durable
                               proof the two endpoint samples alone cannot
                               give.  */
                            if (reply_budget_armed.load()) {
                                int b = reply_budget_remaining.load();
                                while (b > 0
                                       && !reply_budget_remaining.compare_exchange_weak(b, b - 1)) {}
                                while (b <= 0 && reply_budget_armed.load()
                                       && !stop_bigs.load()) {
                                    usleep(20 * 1000);
                                    b = reply_budget_remaining.load();
                                }
                            }
                            if (*counter > 200) {
                                usleep(3 * 1000);
                            }
                        }
                    }
                    delete m;
                }
                if (stalled) {
                    *counter = -1;   // starved: poison the count so the gate fails
                } else if (*counter < (int)bigN) {
                    /* Told to stop with a remainder queued: cancel the whole
                       batch by client id and bounce any replies already in
                       flight, exactly as a real daemon would for a client
                       that is gone.  */
                    JobDoneMsg d(0, 255, JobDoneMsg::FROM_SUBMITTER);
                    d.set_unknown_job_client_id(cid);
                    ch->send_msg(d);
                    const Clock::time_point tb = Clock::now();
                    while (secs_since(tb) < 3) {
                        Msg *m = ch->get_msg(1);
                        if (!m) { continue; }
                        if (MSG_IS(m, USE_CS)) {
                            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                            if (u && u->client_id == cid) {
                                JobDoneMsg bounce(u->job_id, 107, JobDoneMsg::FROM_SUBMITTER);
                                ch->send_msg(bounce);
                            }
                        }
                        delete m;
                    }
                }
                delete ch;
            };
            std::thread f1([&] { big_daemon("fakesub7", 701, &repliesF1); });
            std::thread f2([&] { big_daemon("fakesub8", 801, &repliesF2); });

            usleep(200 * 1000);
            /* The contended window is bracketed from SCHEDULER-side state
               at both ends: admitted_total - replies_consumed > total farm
               capacity for each big submitter proves the scheduler still
               holds queued backlog (more admitted-but-unconsumed work than
               could possibly be dispatched-or-in-flight).  admitted is read
               before the reply counter, so the difference is a lower bound.
               The big daemons pace consumption so this holds throughout the
               small requests on every host.  */
            long long bracket_vals[4] = { -1, -1, -1, -1 };
            long long gen_before[2] = { -1, -1 };
            std::string topo_before;
            long long farm_capacity = -1;
            auto both_active = [&](int slot) {
                /* The CONTENDED WINDOW must be proven from SCHEDULER-side
                   state, conservatively: reader-side reply counters lag the
                   wire, so "replies < admitted" alone could hold while every
                   remaining reply is already queued in socket buffers and
                   the scheduler holds nothing.  The proof used here:

                     admitted_total - replies_consumed > total farm capacity

                   for EACH big submitter -- more admitted-but-unconsumed
                   work than could possibly be dispatched-or-in-flight means
                   some of it is still QUEUED in the scheduler.  admitted is
                   read BEFORE the reply counter, so the difference is a
                   lower bound.  (An earlier form required admission itself
                   to be incomplete, which a fast host finishes before the
                   first sample; a second form used reader counters alone,
                   which cannot see scheduler state.)  */
                /* ONE complete, strictly parsed snapshot supplies the
                   capacity, both admitted counters, and both connection
                   generations -- no mixing of observations from different
                   instants.  Any incomplete exchange fails the sample.  */
                const CtrlReply snap = ctrl_exchange(port, "listcs");
                if (snap.status != CtrlReply::COMPLETE) {
                    return false;
                }
                long long cap = 0;
                {
                    size_t pos = 0;
                    while ((pos = snap.text.find("jobs=", pos)) != std::string::npos) {
                        const size_t slash = snap.text.find('/', pos);
                        const size_t eol = snap.text.find('\n', pos);
                        if (slash == std::string::npos
                            || (eol != std::string::npos && slash > eol)) {
                            return false;
                        }
                        const long long v = parse_field_ll(snap.text.c_str() + slash + 1);
                        if (v < 0) {
                            return false;
                        }
                        cap += v;
                        pos = slash + 1;
                    }
                }
                if (cap <= 0) {
                    return false;
                }
                farm_capacity = cap;
                const long long a7 = field_from_snapshot(snap.text, "fakesub7", "admitted_total=");
                const long long a8 = field_from_snapshot(snap.text, "fakesub8", "admitted_total=");
                const long long g7 = field_from_snapshot(snap.text, "fakesub7", "gen=");
                const long long g8 = field_from_snapshot(snap.text, "fakesub8", "gen=");
                /* Worker-topology stability: capacity must be an UPPER
                   bound for the whole interval, so the post bracket
                   requires the identical worker set (names+capacities,
                   captured as the sorted jobs= rows) -- a worker joining
                   mid-bracket would raise capacity and invalidate the
                   margin arithmetic.  */
                std::string topo;
                {
                    /* Identity + MAX capacity only: the cur half of
                       jobs=cur/max is live occupancy and changes with
                       every dispatch -- it is not topology.  */
                    std::vector<std::string> rows;
                    size_t tpos = 0;
                    while ((tpos = snap.text.find(" jobs=", tpos)) != std::string::npos) {
                        const size_t bol = snap.text.rfind('\n', tpos);
                        const size_t slash = snap.text.find('/', tpos);
                        const size_t eol = snap.text.find('\n', tpos);
                        if (slash == std::string::npos
                            || (eol != std::string::npos && slash > eol)) {
                            tpos += 6;
                            continue;
                        }
                        const size_t name_start = bol == std::string::npos ? 0 : bol;
                        std::string row = snap.text.substr(name_start, tpos - name_start);
                        const size_t cap_end = snap.text.find(' ', slash);
                        row += snap.text.substr(slash,
                                                cap_end == std::string::npos ? std::string::npos
                                                                             : cap_end - slash);
                        rows.push_back(row);
                        tpos += 6;
                    }
                    std::sort(rows.begin(), rows.end());
                    for (const std::string &r : rows) { topo += r; }
                }
                if (slot == 0) {
                    gen_before[0] = g7;
                    gen_before[1] = g8;
                    topo_before = topo;
                } else {
                    /* An admitted counter is only comparable within one
                       connection generation, and the margin arithmetic
                       only holds under an unchanged worker topology.  */
                    if (g7 != gen_before[0] || g8 != gen_before[1]) {
                        return false;
                    }
                    if (topo != topo_before) {
                        fprintf(stderr, "# contract: worker topology changed"
                                " across the bracket; sample rejected\n");
                        return false;
                    }
                }
                const long long r7 = repliesF1.load();
                const long long r8 = repliesF2.load();
                bracket_vals[slot] = a7;
                bracket_vals[slot + 1] = a8;
                return a7 > 0 && a8 > 0
                    && (a7 - r7) > farm_capacity
                    && (a8 - r8) > farm_capacity;
            };
            /* CONSTRUCTED contended window: the big daemons pace their
               consumption (above), so admitted outruns consumed and the
               backlog stays scheduler-queued on EVERY host.  Wait, bounded,
               until both_active() proves the window is open by the
               admitted-minus-consumed capacity inequality -- no fast-host
               skip, because the window is built rather than hoped for.  */
            bool contended_before = false;
            {
                const Clock::time_point tb = Clock::now();
                while (!contended_before && secs_since(tb) < 30) {
                    contended_before = both_active(0);
                    if (!contended_before) {
                        usleep(200 * 1000);
                    }
                }
            }
            /* ARITHMETIC INTERVAL PROOF.  Two endpoint samples cannot show
               the backlog predicate held BETWEEN them.  Instead: capture
               the pre-bracket margins, arm a fixed shared reply budget the
               two big consumers may spend while the smalls run (they pause
               when it is exhausted), and require

                   margin_i > farm_capacity + BUDGET      (each big)

               Admission is nondecreasing, so the margin can shrink only by
               consumed replies <= BUDGET: the backlog stayed above
               farm_capacity for the WHOLE interval, not just at the ends.  */
            const int kReplyBudget = 400;
            const long long replies_before7 = repliesF1.load();
            const long long replies_before8 = repliesF2.load();
            const long long margin7 = bracket_vals[0] - replies_before7;
            const long long margin8 = bracket_vals[1] - replies_before8;
            REQUIRE(margin7 > farm_capacity + kReplyBudget,
                    "big submitter 1's pre-bracket margin covers capacity"
                    " plus the whole reply budget");
            REQUIRE(margin8 > farm_capacity + kReplyBudget,
                    "big submitter 2's pre-bracket margin covers capacity"
                    " plus the whole reply budget");
            reply_budget_remaining = kReplyBudget;
            reply_budget_armed = true;

            int slow = 0;
            int measured = 0;
            double worst_small = 0;
            for (int i = 0; i < 10; ++i) {
                ++measured;
                char fname[32];
                snprintf(fname, sizeof(fname), "small%d.cpp", i);
                REQUIRE(send_count(sub, 1, 9500 + i, fname), "small request sent");
                const Clock::time_point ts = Clock::now();
                unsigned jid = 0;
                while (jid == 0 && secs_since(ts) < 10) {
                    Msg *m = sub->get_msg(1);
                    if (!m) { continue; }
                    unsigned cid = 0;
                    if (MSG_IS(m, USE_CS)) {
                        UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                        if (u) { cid = u->client_id; if (cid == 9500u + i) { jid = u->job_id; } }
                        if (u && cid != 9500u + i) { confirm_job(u->job_id); }
                    } else if (MSG_IS(m, NO_CS)) {
                        NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                        if (n) { cid = n->client_id; if (cid == 9500u + i) { jid = n->job_id; } }
                    }
                    delete m;
                }
                const double took = secs_since(ts);
                if (took > worst_small) { worst_small = took; }
                if (jid) { confirm_job(jid); }
                if (!jid || took > 3.0) { ++slow; }
            }
            const long long spent7 = repliesF1.load() - replies_before7;
            const long long spent8 = repliesF2.load() - replies_before8;
            reply_budget_armed = false;
            REQUIRE(spent7 + spent8 <= kReplyBudget,
                    "the big consumers stayed within the armed reply budget"
                    " (measured, not assumed)");
            {
                const long long proven7 = margin7 - kReplyBudget - farm_capacity;
                const long long proven8 = margin8 - kReplyBudget - farm_capacity;
                fprintf(stderr, "# contract: fairness interval margins:"
                        " min proven above capacity = %lld (big1) / %lld (big2),"
                        " budget spent %lld+%lld of %d\n",
                        proven7, proven8, spent7, spent8, kReplyBudget);
            }
            const bool contended_after = both_active(2);
            /* Observe actual expansion progress before ending them: the
               smalls take ~2s, and stopping there would prove nothing about
               the bigs.  Progress-based: fail only if a big stops advancing
               for 60s (starvation), not for being slow.  */
            {
                const Clock::time_point tw = Clock::now();
                int last1 = repliesF1.load(), last2 = repliesF2.load();
                Clock::time_point lastadv = tw;
                while ((repliesF1.load() < 500 || repliesF2.load() < 500)
                        && repliesF1.load() < (int)bigN && repliesF2.load() < (int)bigN
                        && secs_since(lastadv) < 60) {
                    usleep(200 * 1000);
                    if (repliesF1.load() != last1 || repliesF2.load() != last2) {
                        last1 = repliesF1.load();
                        last2 = repliesF2.load();
                        lastadv = Clock::now();
                    }
                }
            }
            stop_bigs = true;
            f1.join();
            f2.join();
            fprintf(stderr, "# contract: fairness big=%d+%d/%u bracket=%d/%d"
                    " admitted(before=%lld,%lld after=%lld,%lld) measured=%d"
                    " small worst=%.2fs slow=%d\n",
                    repliesF1.load(), repliesF2.load(), bigN,
                    contended_before, contended_after,
                    bracket_vals[0], bracket_vals[1], bracket_vals[2], bracket_vals[3],
                    measured, worst_small, slow);
            REQUIRE(repliesF1.load() >= 500 && repliesF2.load() >= 500,
                    "both long expansions progressed without a starvation stall");
            {
                /* The cancelled remainders may be many thousands of members:
                   the batch cancel must leave NOTHING of either daemon in
                   the scheduler's job map.  */
                long long g7 = -1, g8 = -1;
                const Clock::time_point tg = Clock::now();
                while (secs_since(tg) < 30) {
                    g7 = query_control_count(port, "listjobs", "sub:fakesub7");
                    g8 = query_control_count(port, "listjobs", "sub:fakesub8");
                    if (g7 == 0 && g8 == 0) { break; }
                    usleep(300 * 1000);
                }
                fprintf(stderr, "# contract: fairness remainder ghosts=%lld+%lld\n", g7, g8);
                REQUIRE(g7 == 0 && g8 == 0,
                        "a many-thousand-member batch cancel leaves no ghosts");
            }
            REQUIRE(contended_before && contended_after,
                    "the ten small requests ran demonstrably inside a"
                    " constructed contended window (scheduler-side backlog"
                    " proven at both brackets by admitted-minus-consumed >"
                    " farm capacity)");
            REQUIRE(slow == 0,
                    "every measured small request completed within its bounded interval");
            REQUIRE(worst_reply.load() < 5.0,
                    "control latency stayed bounded while the large requests were incomplete");
        }

        // ---- case 7: pre-reply cancellation owns the whole batch
        // A client that dies in WAITFORCS is cancelled by (submitter,
        // client_id) -- and count>1 gives every sibling that one id, so the
        // cancellation names the batch.  7a cancels while the batch is
        // STAGED (the FIN-style same-drain trick: the cancel is processed
        // with the expansion record live), which was a use-after-free
        // before batch ownership; 7b cancels an ACTIVATED batch whose tail
        // is still queued, which used to delete only the LAST match and
        // leave the rest as undispatchable ghosts in the jobs map.
        {
            MsgChannel *subF = connect_daemon(port, 0);
            REQUIRE(subF != nullptr, "cancel-test daemon connected");
            if (subF) {
                LoginMsg login(0, "fakesub9", kPlatform, 0);
                login.envs.push_back(std::make_pair(kPlatform, kEnv));
                login.max_kids = 0;
                login.noremote = true;
                REQUIRE(subF->send_msg(login), "cancel-test daemon logged in");
                usleep(300 * 1000);

                // 7a: cancel while staged
                REQUIRE(send_count(subF, 2000, 601, "cancelA.cpp"),
                        "count=2000 sent (to be cancelled while staged)");
                {
                    JobDoneMsg d(0, 255, JobDoneMsg::FROM_SUBMITTER);
                    d.set_unknown_job_client_id(601);
                    REQUIRE(subF->send_msg(d), "staged-batch cancellation sent in the same drain");
                }
                /* Whether any replies arrive depends on TCP segmentation:
                   if the request and the cancel coalesce into one drain the
                   batch dies fully staged (zero replies); if the scheduler
                   polls between the two segments, up to credit-many members
                   dispatch first.  Both are legitimate schedules.  What a
                   REAL daemon does with a reply for a vanished client is
                   answer JobDone(107, FROM_SUBMITTER) -- that bounce is
                   what credits the dispatch debit -- so this fake does the
                   same; without it the leftover debits pin the submitter at
                   its credit and every later assertion wedges (issue #2's
                   third symptom was exactly that).  */
                int stray601 = 0;
                {
                    const Clock::time_point t0 = Clock::now();
                    while (secs_since(t0) < 3) {
                        Msg *m = subF->get_msg(1);
                        if (!m) { continue; }
                        if (MSG_IS(m, USE_CS)) {
                            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                            if (u && u->client_id == 601) {
                                ++stray601;
                                JobDoneMsg bounce(u->job_id, 107, JobDoneMsg::FROM_SUBMITTER);
                                subF->send_msg(bounce);
                            }
                        }
                        delete m;
                    }
                }
                fprintf(stderr, "# contract: replies before the staged cancel landed: %d\n",
                        stray601);
                REQUIRE(stray601 <= 32,
                        "replies for a cancelled batch are bounded by the dispatch credit");
                {
                    int got = 0;
                    REQUIRE(send_count(subF, 50, 602, "cancelB.cpp"),
                            "fresh request after the staged cancel");
                    const Clock::time_point t0 = Clock::now();
                    while (got < 50 && secs_since(t0) < 300) {
                        Msg *m = subF->get_msg(2);
                        if (!m) { continue; }
                        if (MSG_IS(m, USE_CS)) {
                            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                            if (u && u->client_id == 602) { confirm_job(u->job_id); ++got; }
                        }
                        delete m;
                    }
                    REQUIRE(got == 50, "the daemon completes a fresh request after the staged cancel");
                }

                // 7b: cancel an activated batch with a queued tail
                REQUIRE(send_count(subF, 100, 603, "cancelC.cpp"),
                        "count=100 sent (tail to be cancelled while queued)");
                std::vector<unsigned> dispatched;
                {
                    /* The dispatch credit (32) bounds unconfirmed
                       assignments, so exactly 32 dispatch and 68 stay
                       queued with no server -- the cancellation's target.
                       Progress-based wait: a loaded box is slow, not
                       broken.  */
                    const Clock::time_point t0 = Clock::now();
                    Clock::time_point tp = t0;
                    while (dispatched.size() < 32
                            && secs_since(tp) < 60 && secs_since(t0) < 300) {
                        Msg *m = subF->get_msg(2);
                        if (!m) { continue; }
                        if (MSG_IS(m, USE_CS)) {
                            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                            if (u && u->client_id == 603) {
                                dispatched.push_back(u->job_id);
                                tp = Clock::now();
                            }
                        }
                        delete m;
                    }
                }
                fprintf(stderr, "# contract: 7b dispatched=%zu (want 32)\n", dispatched.size());
                REQUIRE(dispatched.size() == 32,
                        "exactly the credit-many members dispatched before the cancel");
                {
                    JobDoneMsg d(0, 255, JobDoneMsg::FROM_SUBMITTER);
                    d.set_unknown_job_client_id(603);
                    REQUIRE(subF->send_msg(d), "activated-batch cancellation sent");
                }
                usleep(500 * 1000);
                for (unsigned jid : dispatched) {
                    confirm_job(jid);   // the in-flight members finish normally
                }
                {
                    /* Ghost census: after the queued tail is cancelled and
                       the in-flight members complete, NOTHING of this
                       daemon may remain in the scheduler's jobs map.  The
                       one-at-a-time cancellation left 67 dequeued-but-live
                       ghosts here.  */
                    long long ghosts = -1;
                    const Clock::time_point t0 = Clock::now();
                    while (secs_since(t0) < 20) {
                        ghosts = query_control_count(port, "listjobs", "sub:fakesub9");
                        if (ghosts == 0) { break; }
                        usleep(200 * 1000);
                    }
                    fprintf(stderr, "# contract: cancel ghosts remaining=%lld\n", ghosts);
                    REQUIRE(ghosts == 0,
                            "no undispatchable ghost jobs survive a batch cancellation");
                }
                {
                    int got = 0;
                    REQUIRE(send_count(subF, 5, 604, "cancelD.cpp"),
                            "final request after the activated cancel");
                    const Clock::time_point t0 = Clock::now();
                    while (got < 5 && secs_since(t0) < 120) {
                        Msg *m = subF->get_msg(2);
                        if (!m) { continue; }
                        if (MSG_IS(m, USE_CS)) {
                            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                            if (u && u->client_id == 604) { confirm_job(u->job_id); ++got; }
                        }
                        delete m;
                    }
                    REQUIRE(got == 5, "the daemon still works after both cancellations");
                }
                // 7c: same client id in BOTH domains at once.  Daemons
                // reuse client ids, so a cancel can arrive while an EARLIER
                // fully-admitted request's tail is queued and a LATER
                // request with the same id is still staged.  The handler
                // used to stop after destroying the staged record, leaving
                // the queued tail as credit-pinning ghosts (issue #2).
                {
                    REQUIRE(send_count(subF, 100, 605, "cancelE.cpp"),
                            "count=100 sent (the tail will be queued when the cancel lands)");
                    std::vector<unsigned> dispatched605;
                    const Clock::time_point t0 = Clock::now();
                    Clock::time_point tp = t0;
                    while (dispatched605.size() < 32
                            && secs_since(tp) < 60 && secs_since(t0) < 300) {
                        Msg *m = subF->get_msg(2);
                        if (!m) { continue; }
                        if (MSG_IS(m, USE_CS)) {
                            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                            if (u && u->client_id == 605) {
                                dispatched605.push_back(u->job_id);
                                tp = Clock::now();
                            }
                        }
                        delete m;
                    }
                    fprintf(stderr, "# contract: 7c dispatched=%zu (want 32)\n", dispatched605.size());
                    REQUIRE(dispatched605.size() == 32,
                            "the earlier request is fully admitted with a queued tail");
                    /* Same id again, big enough that it is still staged when
                       the cancel is processed right behind it.  */
                    REQUIRE(send_count(subF, 2000, 605, "cancelF.cpp"),
                            "a second request REUSES the client id while staged");
                    {
                        JobDoneMsg d(0, 255, JobDoneMsg::FROM_SUBMITTER);
                        d.set_unknown_job_client_id(605);
                        REQUIRE(subF->send_msg(d), "cancel sent with members in both domains");
                    }
                    /* Real-daemon fidelity: bounce any replies that were in
                       flight, then let the in-flight members finish.  */
                    {
                        const Clock::time_point tb = Clock::now();
                        while (secs_since(tb) < 3) {
                            Msg *m = subF->get_msg(1);
                            if (!m) { continue; }
                            if (MSG_IS(m, USE_CS)) {
                                UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                                if (u && u->client_id == 605) {
                                    JobDoneMsg bounce(u->job_id, 107, JobDoneMsg::FROM_SUBMITTER);
                                    subF->send_msg(bounce);
                                }
                            }
                            delete m;
                        }
                    }
                    for (unsigned jid : dispatched605) {
                        confirm_job(jid);
                    }
                    {
                        long long ghosts = -1;
                        const Clock::time_point tg = Clock::now();
                        while (secs_since(tg) < 20) {
                            ghosts = query_control_count(port, "listjobs", "sub:fakesub9");
                            if (ghosts == 0) { break; }
                            usleep(200 * 1000);
                        }
                        fprintf(stderr, "# contract: both-domain cancel ghosts remaining=%lld\n",
                                ghosts);
                        REQUIRE(ghosts == 0,
                                "queued members of the SAME id do not survive a cancel that"
                                " also destroys a staged record");
                    }
                    {
                        int got = 0;
                        REQUIRE(send_count(subF, 5, 606, "cancelG.cpp"),
                                "request after the both-domain cancel");
                        const Clock::time_point tf = Clock::now();
                        while (got < 5 && secs_since(tf) < 120) {
                            Msg *m = subF->get_msg(2);
                            if (!m) { continue; }
                            if (MSG_IS(m, USE_CS)) {
                                UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                                if (u && u->client_id == 606) { confirm_job(u->job_id); ++got; }
                            }
                            delete m;
                        }
                        REQUIRE(got == 5, "the daemon is not wedged after the both-domain cancel");
                    }
                }
                // 7d: cancel arriving after EVERY member dispatched.  The
                // sweep finds nothing scheduler-owned; that is a SUCCESS,
                // not an error -- the daemon's 107 bounces settle the
                // dispatched members.  The old return value signalled
                // "connection deleted" to the drain loop for a live
                // connection.
                {
                    REQUIRE(send_count(subF, 20, 607, "cancelH.cpp"),
                            "count=20 sent (small enough to dispatch fully)");
                    std::vector<unsigned> dispatched607;
                    const Clock::time_point t0 = Clock::now();
                    Clock::time_point tp = t0;
                    while (dispatched607.size() < 20
                            && secs_since(tp) < 60 && secs_since(t0) < 300) {
                        Msg *m = subF->get_msg(2);
                        if (!m) { continue; }
                        if (MSG_IS(m, USE_CS)) {
                            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                            if (u && u->client_id == 607) {
                                dispatched607.push_back(u->job_id);
                                tp = Clock::now();
                            }
                        }
                        delete m;
                    }
                    REQUIRE(dispatched607.size() == 20, "every member dispatched before the cancel");
                    {
                        JobDoneMsg d(0, 255, JobDoneMsg::FROM_SUBMITTER);
                        d.set_unknown_job_client_id(607);
                        REQUIRE(subF->send_msg(d), "zero-match cancellation sent");
                    }
                    /* The daemon settles the dispatched members: 107 each,
                       IMMEDIATELY behind the cancel on the same connection --
                       if the cancel's return value stops the drain, these are
                       exactly the messages that stall.  */
                    for (unsigned jid : dispatched607) {
                        JobDoneMsg bounce(jid, 107, JobDoneMsg::FROM_SUBMITTER);
                        REQUIRE(subF->send_msg(bounce), "107 bounce sent");
                    }
                    {
                        long long left = -1;
                        const Clock::time_point tg = Clock::now();
                        while (secs_since(tg) < 20) {
                            left = query_control_count(port, "listjobs", "sub:fakesub9");
                            if (left == 0) { break; }
                            usleep(200 * 1000);
                        }
                        REQUIRE(left == 0,
                                "all dispatched members settled via the 107 bounces");
                    }
                    REQUIRE(!subF->at_eof(),
                            "the connection survived a cancellation that matched nothing");
                    {
                        int got = 0;
                        REQUIRE(send_count(subF, 5, 608, "cancelI.cpp"),
                                "request after the zero-match cancel");
                        const Clock::time_point tf = Clock::now();
                        while (got < 5 && secs_since(tf) < 120) {
                            Msg *m = subF->get_msg(2);
                            if (!m) { continue; }
                            if (MSG_IS(m, USE_CS)) {
                                UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                                if (u && u->client_id == 608) { confirm_job(u->job_id); ++got; }
                            }
                            delete m;
                        }
                        REQUIRE(got == 5, "the daemon still works after the zero-match cancel");
                    }
                }
                delete subF;
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
        std::vector<unsigned> b_to_confirm;    // begin + done
        std::vector<unsigned> bbegin_only;     // hold a slot busy
        std::vector<unsigned> bdone_only;      // release a held slot
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
                std::vector<unsigned> batch, beg, don;
                {
                    std::lock_guard<std::mutex> lock(bconfirm_mutex);
                    batch.swap(b_to_confirm);
                    beg.swap(bbegin_only);
                    don.swap(bdone_only);
                }
                for (unsigned jid : beg) {
                    JobBeginMsg jb(jid, 1);
                    if (!csB->send_msg(jb)) { csB_alive = false; return; }
                }
                for (unsigned jid : don) {
                    JobDoneMsg jd(jid, 0, JobDoneMsg::FROM_SERVER);
                    if (!csB->send_msg(jd)) { csB_alive = false; return; }
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

        /* Phase 2 -- TRUE circular wrap.  The forward case above catches a
           walk that advances its position but keeps testing the old job;
           it cannot catch a walk that lost its wrap, because the servable
           job sits AFTER the scored head.  Here the scored winner sits in
           the LAST request group and the servable job in an EARLIER one:
           reaching it requires wrapping past the end back to the origin.

           Estimates make the ordering deterministic: warm bigx2.cpp to
           ~90s and wraparm.cpp to ~100ms through real completion reports,
           so the fresh bigx2 outranks the older wraparm on score.  */
        {
            auto warm_named = [&](const char *fname, unsigned cid,
                                  unsigned real_msec, const char *pref) -> bool {
                GetCSMsg g(Environments{std::make_pair(
                               std::string(strcmp(pref, "fakecsB") == 0 ? "aarch64" : kPlatform),
                               std::string(kEnv))},
                           fname, CompileJob::Lang_CXX, 1,
                           strcmp(pref, "fakecsB") == 0 ? "aarch64" : kPlatform,
                           0, std::string(pref), 0, 0, 0);
                g.client_id = cid;
                if (!sub->send_msg(g)) { return false; }
                const Clock::time_point t0 = Clock::now();
                while (secs_since(t0) < 30) {
                    Msg *m = sub->get_msg(2);
                    if (!m) { continue; }
                    UseCSMsg *u = MSG_IS(m, USE_CS) ? dynamic_cast<UseCSMsg *>(m) : nullptr;
                    if (u && u->client_id == cid) {
                        const unsigned jid = u->job_id;
                        const unsigned cport = u->port;
                        delete m;
                        if (cport == 10261) {
                            std::lock_guard<std::mutex> lock(bconfirm_mutex);
                            b_to_confirm.push_back(jid);   // begins+dones with real=0
                        } else {
                            begin_job(jid);
                            finish_job(jid, real_msec);
                        }
                        return true;
                    }
                    delete m;
                }
                return false;
            };
            /* x86 capacity: occ2 still holds one window; one is free for
               the warms.  Warm the big key on the x86 host with 90s
               reports; the arm job's key stays cold (fallback), which is
               fine -- 2*90000 dominates any fallback plus seconds of age.  */
            for (int r = 0; r < 3; ++r) {
                REQUIRE(warm_named("bigx2.cpp", 8100 + r, 90000, ""),
                        "wrap warm round dispatched");
            }
            /* Refill the x86 host completely (occ2 + one more).  */
            unsigned occ3 = 0, occ3port = 0;
            REQUIRE(send_platform("occupier3.cpp", 8200, kPlatform), "occupier 3 submitted");
            {
                const Clock::time_point t0 = Clock::now();
                while (occ3 == 0 && secs_since(t0) < 30) {
                    Msg *m = sub->get_msg(2);
                    if (!m) { continue; }
                    UseCSMsg *u = MSG_IS(m, USE_CS) ? dynamic_cast<UseCSMsg *>(m) : nullptr;
                    if (u && u->client_id == 8200) { occ3 = u->job_id; occ3port = u->port; }
                    delete m;
                }
            }
            REQUIRE(occ3 != 0 && occ3port != 10261, "occupier 3 parked on the x86 host");
            begin_job(occ3);

            /* BOTH hosts must be fully held while both jobs enqueue, or
               the target simply dispatches the moment it arrives and the
               later "success" reads a buffered reply -- the first version
               of this phase did exactly that (the trace showed the target
               put BEFORE the winner even existed).  Hold the aarch64 host
               too, enqueue target then winner, prove both are queued, and
               only then free aarch64 capacity.  */
            unsigned occB1 = 0, occB2 = 0;
            REQUIRE(send_platform("occupierB1.cpp", 8500, "aarch64"), "aarch64 occupier 1 submitted");
            REQUIRE(send_platform("occupierB2.cpp", 8501, "aarch64"), "aarch64 occupier 2 submitted");
            {
                /* csB has max_kids=4: window = 4 + preload 2 = 6.  Two
                   long-running occupiers are not enough; fill it with 6.  */
                REQUIRE(send_platform("occupierB3.cpp", 8502, "aarch64"), "aarch64 occupier 3 submitted");
                REQUIRE(send_platform("occupierB4.cpp", 8503, "aarch64"), "aarch64 occupier 4 submitted");
                REQUIRE(send_platform("occupierB5.cpp", 8504, "aarch64"), "aarch64 occupier 5 submitted");
                REQUIRE(send_platform("occupierB6.cpp", 8505, "aarch64"), "aarch64 occupier 6 submitted");
                int held = 0;
                const Clock::time_point t0 = Clock::now();
                while (held < 6 && secs_since(t0) < 30) {
                    Msg *m = sub->get_msg(2);
                    if (!m) { continue; }
                    UseCSMsg *u = MSG_IS(m, USE_CS) ? dynamic_cast<UseCSMsg *>(m) : nullptr;
                    if (u && u->client_id >= 8500 && u->client_id <= 8505) {
                        if (u->client_id == 8500) { occB1 = u->job_id; }
                        if (u->client_id == 8501) { occB2 = u->job_id; }
                        /* begin WITHOUT done on the csB side so the whole
                           window stays busy */
                        std::lock_guard<std::mutex> lock(bconfirm_mutex);
                        bbegin_only.push_back(u->job_id);
                        ++held;
                    }
                    delete m;
                }
                REQUIRE(held == 6, "the aarch64 host is completely held (slots + preload)");
            }

            REQUIRE(send_platform("wraparm.cpp", 8300, "aarch64"), "wrap target submitted");
            MsgChannel *subE = connect_daemon(port, 0);
            REQUIRE(subE != nullptr, "wrap-winner daemon connected");
            unsigned arm2_jid = 0, arm2_port = 0;
            if (subE) {
                LoginMsg login(0, "fakesub6", kPlatform, 0);
                login.envs.push_back(std::make_pair(kPlatform, kEnv));
                login.max_kids = 0;
                login.noremote = true;
                REQUIRE(subE->send_msg(login), "wrap-winner daemon logged in");
                usleep(300 * 1000);
                GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                           "bigx2.cpp", CompileJob::Lang_CXX, 1, kPlatform, 0,
                           std::string(), 0, 0, 0);
                g.client_id = 8400;
                REQUIRE(subE->send_msg(g), "high-score winner submitted from the LAST group");

                /* PRECONDITION, queried not assumed: both requests are
                   queued (two groups), nothing dispatchable.  */
                {
                    /* The EXACT two groups -- the healthy stream also keeps
                       a queued group, so counting any two submitter= lines
                       could pass without the winner ever being processed.  */
                    long long tgt = -1, win = -1;
                    const Clock::time_point t0 = Clock::now();
                    while (secs_since(t0) < 10) {
                        tgt = query_control_count(port, "listrequests", " submitter=fakesub ");
                        win = query_control_count(port, "listrequests", " submitter=fakesub6 ");
                        if (tgt >= 1 && win >= 1) { break; }
                        usleep(100 * 1000);
                    }
                    REQUIRE(tgt >= 1 && win >= 1,
                            "the target group AND the winner group are QUEUED before any capacity frees");
                }

                /* Free ONE aarch64 assignment while x86 stays full: the
                   walk starts at the winner (last group) and only a wrap
                   reaches the earlier target.  */
                {
                    std::lock_guard<std::mutex> lock(bconfirm_mutex);
                    bdone_only.push_back(occB1);   // already begun; done frees the slot
                }
                const Clock::time_point t0 = Clock::now();
                while (arm2_jid == 0 && secs_since(t0) < 20) {
                    Msg *m = sub->get_msg(2);
                    if (!m) { continue; }
                    UseCSMsg *u = MSG_IS(m, USE_CS) ? dynamic_cast<UseCSMsg *>(m) : nullptr;
                    if (u && u->client_id == 8300) { arm2_jid = u->job_id; arm2_port = u->port; }
                    delete m;
                }
            }
            fprintf(stderr, "# heterogeneous: wrap target dispatched=%s port=%u\n",
                    arm2_jid ? "yes" : "NO", arm2_port);
            REQUIRE(arm2_jid != 0,
                    "a servable job BEFORE the scored winner is reached (true circular wrap)");
            REQUIRE(arm2_port == 10261, "it went to the aarch64 host");
            if (arm2_jid) {
                std::lock_guard<std::mutex> lock(bconfirm_mutex);
                b_to_confirm.push_back(arm2_jid);
            }
            /* Liveness: free the x86 host; the winner must dispatch.  */
            finish_job(occ3, 1000);
            unsigned win_jid = 0;
            if (subE) {
                const Clock::time_point t0 = Clock::now();
                while (win_jid == 0 && secs_since(t0) < 30) {
                    Msg *m = subE->get_msg(2);
                    if (!m) { continue; }
                    UseCSMsg *u = MSG_IS(m, USE_CS) ? dynamic_cast<UseCSMsg *>(m) : nullptr;
                    if (u && u->client_id == 8400) { win_jid = u->job_id; }
                    delete m;
                }
            }
            REQUIRE(win_jid != 0, "the blocked winner runs once its host returns");
            if (win_jid) { confirm_job(win_jid); }
            if (subE) { delete subE; }
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

    if (stallcredit_mode) {
        /* BP-1's liveness bound, at the 10s CLI floor: a submitter whose
           dispatched jobs never reach JobBegin is reported AT the bound --
           demonstrably not before it, and not never -- and the healthy
           submitter is served straight through the event.  */
        fprintf(stderr, "# stallcredit: flooding %d jobs, never confirming\n", njobs);
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
           JobBegin is) but confirm NOTHING.  The deadline is measured from
           the FIRST unconfirmed assignment: that is when the oldest debit's
           stall clock starts.  */
        int delivered = 0;
        const Clock::time_point t0 = Clock::now();
        Clock::time_point t_first = t0;
        const int healthy_at_flood = healthy_replies.load();
        int healthy_mid = -1;
        bool removed = false;
        double report_after_first_s = -1;
        while (secs_since(t0) < 40) {
            Msg *m = sub->get_msg(1);
            if (m) {
                if (MSG_IS(m, USE_CS) || MSG_IS(m, NO_CS)) {
                    if (delivered == 0) {
                        t_first = Clock::now();
                    }
                    ++delivered;
                }
                delete m;
            }
            if (healthy_mid < 0 && delivered > 0
                    && Clock::now() - t_first > std::chrono::seconds(6)) {
                healthy_mid = healthy_replies.load();
            }
            if (sub->at_eof()) {
                removed = true;
                report_after_first_s = std::chrono::duration<double>(
                    Clock::now() - t_first).count();
                break;
            }
        }
        const int healthy_at_report = healthy_replies.load();
        fprintf(stderr, "# stallcredit: delivered=%d removed=%s at %.1fs after first delivery"
                " (bound 10s), healthy flood=%d mid=%d report=%d\n",
                delivered, removed ? "yes" : "NO", report_after_first_s,
                healthy_at_flood, healthy_mid, healthy_at_report);
        /* The dispatch credit is exact: 32 unconfirmed assignments (the
           configured default; the farm is large enough that no clamp
           applies), and nothing more afterwards.

           This submitter READS its replies -- it is a connection whose
           simulated wrappers never reach the worker, not a dead daemon.
           The scheduler must therefore quarantine it from new assignments
           and keep everything else intact.  Removing a responsive daemon on
           the strength of "no JobBegin" cannot distinguish 32 frozen
           wrappers from one dead host, so that is no longer the rule;
           connection failure and the deferred-output deadline remain the
           paths that remove a genuinely dead daemon.  */
        REQUIRE(delivered == 32, "exactly the effective dispatch credit was delivered");
        REQUIRE(!removed,
                "a READING submitter is NOT removed (its wrappers stalled, not it); the"
                " dispatch credit is what bounds the farm slots it can hold");
        {
            bool reported = false;
            FILE *lf = fopen(sched_log.c_str(), "r");
            if (lf) {
                char line[4096];
                while (fgets(line, sizeof(line), lf)) {
                    if (strstr(line, "is not progressing")) { reported = true; }
                }
                fclose(lf);
            }
            REQUIRE(reported, "the scheduler reported the stalled assignments at the bound");
        }
        /* Progress DURING the stall window: the frozen peer must not drag
           anyone else down while its clock runs.  */
        REQUIRE(healthy_mid > healthy_at_flood,
                "the healthy submitter progressed during the stall window");
        /* The healthy submitter must keep completing work after the event.  */
        {
            const Clock::time_point th = Clock::now();
            while (healthy_replies.load() < healthy_at_report + 5 && secs_since(th) < 30) {
                usleep(100 * 1000);
            }
        }
        fprintf(stderr, "# stallcredit: healthy now=%d (was %d)\n",
                healthy_replies.load(), healthy_at_report);
        REQUIRE(healthy_replies.load() >= healthy_at_report + 5,
                "the healthy submitter kept being served throughout");
        REQUIRE(healthy_alive.load(), "the healthy submitter connection survived");
        REQUIRE(!sub->at_eof(), "the stalled submitter's own connection survived");
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

    if (noreader_mode) {
        auto send_for = [&](MsgChannel *ch, unsigned cid, const char *fname) {
            GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                       fname, CompileJob::Lang_CXX, 1, kPlatform, 0, std::string(), 0, 0, 0);
            g.client_id = cid;
            return ch->send_msg(g);
        };

        /* Three requests, then the submitter's userspace goes silent: we
           never call get_msg() on `sub` again until teardown.  The kernel
           keeps ACKing, so with a one-assignment credit whose single small
           reply fits in the socket buffers there is nothing for deferred
           output to arm on.  */
        REQUIRE(send_for(sub, 8100, "silent1.cpp"), "first request sent");
        REQUIRE(send_for(sub, 8101, "silent2.cpp"), "second request sent");
        REQUIRE(send_for(sub, 8102, "silent3.cpp"), "third request sent");

        fprintf(stderr, "# noreader: submitter goes silent across the 10s threshold\n");
        /* Barrier first: sampling before the assignment exists would record
           a legitimate pre-assignment zero and fail on machine speed.  The
           retention window's minimum starts only once the state to retain
           is actually there.  */
        {
            const Clock::time_point tb = Clock::now();
            while (secs_since(tb) < 30) {
                if (query_submitter_outstanding(port, "fakesub") == 1
                        && worker_job_count(port, "fakecs") >= 1) {
                    break;
                }
                usleep(200 * 1000);
            }
            const bool barrier_ready =
                query_submitter_outstanding(port, "fakesub") == 1
                && worker_job_count(port, "fakecs") >= 1;
            REQUIRE(barrier_ready,
                    "barrier: the credit-1 assignment AND its worker reservation"
                    " exist before the retention window");
        }
        long long min_worker_jobs = 1000000;
        const int healthy_before = healthy_replies.load();
        const Clock::time_point t0 = Clock::now();
        while (secs_since(t0) < 16) {
            const long long wj = worker_job_count(port, "fakecs");
            if (wj >= 0 && wj < min_worker_jobs) { min_worker_jobs = wj; }
            usleep(500 * 1000);
        }

        /* Exactly the credit was assigned and it is still outstanding.  */
        const long long outstanding = query_submitter_outstanding(port, "fakesub");
        fprintf(stderr, "# noreader: outstanding=%lld min_worker_jobs=%lld\n",
                outstanding, min_worker_jobs);
        REQUIRE(outstanding == 1,
                "exactly the one-assignment credit was delivered and remains outstanding");
        REQUIRE(min_worker_jobs >= 1,
                "the worker reservation stayed owned throughout the silent window");

        {
            bool deferring = false, reported = false, removed = false, drained = false;
            FILE *lf = fopen(sched_log.c_str(), "r");
            if (lf) {
                char line[4096];
                while (fgets(line, sizeof(line), lf)) {
                    if (strstr(line, "deferring")) { deferring = true; }
                    if (strstr(line, "is not progressing")) { reported = true; }
                    if (strstr(line, "remove daemon fakesub")) { removed = true; }
                    if (strstr(line, "did not drain its socket")) { drained = true; }
                }
                fclose(lf);
            }
            REQUIRE(!deferring,
                    "no deferred-output episode armed (the kernel ACKed the tiny reply)");
            REQUIRE(reported, "the silent submitter was reported at the threshold");
            REQUIRE(!removed && !drained,
                    "and NOT removed: with no pending userspace bytes neither connection"
                    " failure nor the drain deadline can fire -- the documented Stage-A"
                    " behaviour is that the reservation persists");
        }

        REQUIRE(healthy_replies.load() > healthy_before,
                "another submitter kept progressing on the remaining capacity");
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

    if (retention_mode) {
        /* The healthy_thread would supply unrelated confirmations and mask
           exactly what these cases test; it parks itself for this mode.  */
        auto send_for = [&](unsigned cid, const char *fname) {
            GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                       fname, CompileJob::Lang_CXX, 1, kPlatform, 0, std::string(), 0, 0, 0);
            g.client_id = cid;
            return sub->send_msg(g);
        };
        auto await_for = [&](unsigned cid, double secs) -> unsigned {
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < secs) {
                Msg *m = sub->get_msg(1);
                if (!m) { continue; }
                unsigned jid = 0, got = 0;
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u) { jid = u->job_id; got = u->client_id; }
                } else if (MSG_IS(m, NO_CS)) {
                    NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                    if (n) { jid = n->job_id; got = n->client_id; }
                }
                delete m;
                if (jid && got == cid) { return jid; }
            }
            return 0;
        };

        /* CASE 2 first: a sibling that legitimately RUNS across the bound.
           It confirms with JobBegin (it reached the worker) and only
           completes much later -- the pattern of a long compile.  It must
           survive the frozen peer's bound untouched.  */
        unsigned long_job = 0;
        REQUIRE(send_for(7100, "longsibling.cpp"), "long sibling requested");
        REQUIRE((long_job = await_for(7100, 30)) != 0, "long sibling assigned");
        begin_job(long_job);          // reached the worker; no done for a long time

        /* CASE 1: a wrapper freezes after its assignment, on a daemon that
           is otherwise QUIET -- no other completions at all.  */
        unsigned frozen_job = 0;
        REQUIRE(send_for(7200, "frozen.cpp"), "frozen wrapper requested");
        REQUIRE((frozen_job = await_for(7200, 30)) != 0, "frozen wrapper assigned");
        /* deliberately never confirmed */

        fprintf(stderr, "# retention: waiting out the threshold with a QUIET healthy daemon\n");
        const Clock::time_point t_bound = Clock::now();
        while (secs_since(t_bound) < 16) {
            Msg *m = sub->get_msg(1);      // stay responsive: we are NOT dead
            delete m;
        }

        REQUIRE(!sub->at_eof(),
                "a quiet but responsive daemon is NOT removed when one wrapper stalls");
        {
            bool quarantined = false, removed = false;
            FILE *lf = fopen(sched_log.c_str(), "r");
            if (lf) {
                char line[4096];
                while (fgets(line, sizeof(line), lf)) {
                    if (strstr(line, "is not progressing")) { quarantined = true; }
                    if (strstr(line, "remove daemon fakesub")) { removed = true; }
                }
                fclose(lf);
            }
            REQUIRE(quarantined, "the scheduler reported the unconfirmed assignment");
            REQUIRE(!removed, "the quiet healthy daemon was NOT removed");
        }
        /* The long sibling's completion must be CONSUMED by the scheduler.
           The dispatch-credit counter cannot show that: the sibling's Begin
           already credited its debit, so `outstanding` reads 1 (the frozen
           job) both before and after the Done is processed.  Observe the
           scheduler's own job map and the worker's job count instead.  */
        {
            REQUIRE(job_in_scheduler(port, long_job),
                    "the long sibling is in the scheduler's job map before its Done");
            const long long jobs_before = worker_job_count(port, "fakecs");
            REQUIRE(jobs_before >= 1, "the worker holds the sibling's reservation");

            JobDoneMsg jd(long_job, 0, JobDoneMsg::FROM_SERVER);
            REQUIRE(cs->send_msg(jd), "long sibling's completion sent");

            bool gone = false;
            const Clock::time_point t0 = Clock::now();
            while (!gone && secs_since(t0) < 15) {
                if (!job_in_scheduler(port, long_job)) { gone = true; break; }
                usleep(200 * 1000);
            }
            REQUIRE(gone, "the completion was consumed: the job left the scheduler's map");
            const long long jobs_after = worker_job_count(port, "fakecs");
            fprintf(stderr, "# retention: worker jobs %lld -> %lld after the sibling's Done\n",
                    jobs_before, jobs_after);
            REQUIRE(jobs_after == jobs_before - 1,
                    "the worker reservation was released exactly once");
            REQUIRE(job_in_scheduler(port, frozen_job),
                    "the frozen assignment is still OWNED (retention)");
        }

        /* Dispatch continues while a wrapper is stuck: ONE request, waited
           for by its own client id (a repeated id could be satisfied by a
           duplicate queued job and pass for the wrong request).  */
        unsigned resumed = 0;
        REQUIRE(send_for(7300, "afterprogress.cpp"), "post-threshold request sent");
        resumed = await_for(7300, 20);
        fprintf(stderr, "# retention: dispatch with a stuck wrapper present: %s\n",
                resumed ? "continues" : "BLOCKED");
        REQUIRE(resumed != 0,
                "a stuck wrapper does not block its host's other work while credit remains");
        confirm_job(resumed);

        /* Exactly-once reporting: one frozen assignment, many healthy
           siblings crossing the threshold, must produce ONE warning.  */
        {
            int warnings = 0;
            FILE *lf = fopen(sched_log.c_str(), "r");
            if (lf) {
                char line[4096];
                while (fgets(line, sizeof(line), lf)) {
                    if (strstr(line, "is not progressing")) { ++warnings; }
                }
                fclose(lf);
            }
            fprintf(stderr, "# retention: stall warnings emitted: %d\n", warnings);
            REQUIRE(warnings == 1,
                    "the stall is reported exactly once per episode, not once per poll");
        }

        /* A monitor watches the late thaw: the terminal transition must be
           announced EXACTLY once -- the old expiry design told monitors the
           job had finished while it could still run, which is the class of
           lie this observer exists to catch.  */
        MsgChannel *mon = connect_daemon(port, 0);
        REQUIRE(mon != nullptr, "monitor connected");
        if (mon) {
            mon->send_msg(MonLoginMsg());
        }

        /* THE STAGE-A OWNERSHIP PROOF: the frozen wrapper thaws long after
           the threshold.  Because the scheduler retained the job and the
           worker reservation, its late Begin/Done must still be recognized
           and must reconcile exactly once -- no unknown-job error, no
           double release.  This is the property that makes retention
           correct rather than merely non-destructive.  */
        {
            const long long before = query_submitter_outstanding(port, "fakesub");
            fprintf(stderr, "# retention: late thaw begins with outstanding=%lld\n", before);
            REQUIRE(before >= 1, "the frozen assignment was still owned before the thaw");

            JobBeginMsg jb(frozen_job, 1);
            REQUIRE(cs->send_msg(jb), "late JobBegin for the thawed wrapper sent");
            bool credited = false;
            const Clock::time_point t0 = Clock::now();
            while (!credited && secs_since(t0) < 15) {
                if (query_submitter_outstanding(port, "fakesub") == 0) { credited = true; break; }
                usleep(200 * 1000);
            }
            REQUIRE(credited, "the late JobBegin credited the retained assignment exactly once");

            /* Begin must NOT have ended the job: it is COMPILING now, and
               the worker still holds its reservation.  */
            REQUIRE(job_in_scheduler(port, frozen_job),
                    "the thawed job is still owned after its late Begin (now compiling)");
            const long long jobs_before_done = worker_job_count(port, "fakecs");

            JobDoneMsg jd(frozen_job, 0, JobDoneMsg::FROM_SERVER);
            REQUIRE(cs->send_msg(jd), "late JobDone for the thawed wrapper sent");
            bool ended = false;
            const Clock::time_point t1 = Clock::now();
            while (!ended && secs_since(t1) < 15) {
                if (!job_in_scheduler(port, frozen_job)) { ended = true; break; }
                usleep(200 * 1000);
            }
            REQUIRE(ended, "the late completion was consumed: the job left the scheduler's map");
            const long long jobs_after_done = worker_job_count(port, "fakecs");
            fprintf(stderr, "# retention: worker jobs %lld -> %lld after the late Done\n",
                    jobs_before_done, jobs_after_done);
            REQUIRE(jobs_after_done == jobs_before_done - 1,
                    "the retained worker reservation was released exactly once");
            REQUIRE(query_submitter_outstanding(port, "fakesub") == 0,
                    "accounting stayed converged after the late completion");

            /* The monitor saw the thaw exactly once in each direction.  */
            if (mon) {
                int mon_begins = 0, mon_dones = 0;
                const Clock::time_point tm = Clock::now();
                while (secs_since(tm) < 5) {
                    Msg *m = mon->get_msg(1);
                    if (!m) { continue; }
                    if (MSG_IS(m, MON_JOB_BEGIN)) {
                        MonJobBeginMsg *b = dynamic_cast<MonJobBeginMsg *>(m);
                        if (b && b->job_id == frozen_job) { ++mon_begins; }
                    } else if (MSG_IS(m, MON_JOB_DONE)) {
                        MonJobDoneMsg *d = dynamic_cast<MonJobDoneMsg *>(m);
                        if (d && d->job_id == frozen_job) { ++mon_dones; }
                    }
                    delete m;
                }
                fprintf(stderr, "# retention: monitor saw begin=%d done=%d for the thawed job\n",
                        mon_begins, mon_dones);
                REQUIRE(mon_begins == 1, "the monitor saw exactly one Begin for the thawed job");
                REQUIRE(mon_dones == 1, "the monitor saw exactly one terminal Done for it");
                delete mon;
                mon = nullptr;
            }

            /* And the freed capacity is really usable: a new assignment must
               be able to consume it.  */
            REQUIRE(send_for(7400, "afterreclaim.cpp"), "post-reclaim request sent");
            const unsigned reclaimed = await_for(7400, 20);
            REQUIRE(reclaimed != 0, "the reclaimed worker capacity accepts new work");
            confirm_job(reclaimed);
            {
                bool invariant_failure = false;
                FILE *lf = fopen(sched_log.c_str(), "r");
                if (lf) {
                    char line[4096];
                    while (fgets(line, sizeof(line), lf)) {
                        if (strstr(line, "invariant failure")
                                || strstr(line, "accounting mismatch")) {
                            invariant_failure = true;
                        }
                    }
                    fclose(lf);
                }
                REQUIRE(!invariant_failure,
                        "no accounting invariant failure across the late reconciliation");
            }
        }
        /* A SECOND, independent stall episode must be reported once more:
           the latch belongs to the episode, so resolving the first one must
           re-arm it for the next.  */
        {
            unsigned frozen2 = 0;
            REQUIRE(send_for(7500, "frozen2.cpp"), "second frozen wrapper requested");
            REQUIRE((frozen2 = await_for(7500, 30)) != 0, "second frozen wrapper assigned");
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < 16) {
                Msg *m = sub->get_msg(1);
                delete m;
            }
            int warnings = 0;
            FILE *lf = fopen(sched_log.c_str(), "r");
            if (lf) {
                char line[4096];
                while (fgets(line, sizeof(line), lf)) {
                    if (strstr(line, "is not progressing")) { ++warnings; }
                }
                fclose(lf);
            }
            fprintf(stderr, "# retention: warnings after a SECOND episode: %d\n", warnings);
            REQUIRE(warnings == 2,
                    "a new stall episode is reported exactly once more (the latch re-arms)");
            REQUIRE(job_in_scheduler(port, frozen2),
                    "the second frozen assignment is owned too");
        }
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

    if (internalsguard_mode) {
        MsgChannel *w = connect_daemon(port, 0);
        REQUIRE(w != nullptr, "guard worker connected");
        if (w) {
            LoginMsg login(10276, "fguard", kPlatform, 0);
            login.envs.push_back(std::make_pair(kPlatform, kEnv));
            login.max_kids = 4;
            REQUIRE(w->send_msg(login), "guard worker logged in");
            StatsMsg st; w->send_msg(st);
        }
        usleep(400 * 1000);
        REQUIRE(query_submitter_field(port, "fguard", "jobs=") >= 0,
                "guard worker logged in before the fan-out");

        const int ictrl = tcp_connect(port + 1, 0);
        REQUIRE(ictrl >= 0, "guard control connected");
        { char g[256]; struct pollfd gp = { ictrl, POLLIN, 0 };
          if (poll(&gp,1,3000)>0){ ssize_t z=read(ictrl,g,sizeof(g)); (void)z; } }
        { const char *icmd = "internals fguard\n";
          REQUIRE(write(ictrl, icmd, strlen(icmd)) == (ssize_t)strlen(icmd),
                  "internals command sent"); }
        usleep(400 * 1000);   /* txn active (no-tick-promote, silent worker) */

        /* Re-issue a DIFFERENT command on the SAME control while its
           transaction is live: the guard must fail this exact control.  */
        { const char *c2 = "listcs\n";
          (void)!write(ictrl, c2, strlen(c2)); }

        /* The active control connection must be closed (EOF) by the guard.  */
        bool closed = false;
        {
            const Clock::time_point t0 = Clock::now();
            char b[4096];
            while (!closed && secs_since(t0) < 8) {
                struct pollfd rp = { ictrl, POLLIN, 0 };
                if (poll(&rp,1,200) <= 0) { continue; }
                const ssize_t n = read(ictrl, b, sizeof(b)-1);
                if (n <= 0) { closed = true; }
            }
        }
        REQUIRE(closed,
                "the active control that re-issued a command mid-transaction"
                " was failed (its reply would bypass the reserved-tail bound)");

        /* A SEPARATE control confirms the settlement reason via the snapshot.  */
        {
            std::string reason;
            const Clock::time_point t0 = Clock::now();
            while (reason.empty() && secs_since(t0) < 8) {
                const std::string dump = control_dump(port, "listjobs");
                const size_t p2 = dump.find("internals_last_reason=");
                if (p2 != std::string::npos) {
                    const size_t st = p2 + strlen("internals_last_reason=");
                    const size_t en = dump.find_first_of(" \n", st);
                    reason = dump.substr(st, en == std::string::npos ? std::string::npos : en - st);
                }
                if (reason.empty() || reason == "") { usleep(200*1000); }
            }
            REQUIRE(reason == "active-control-reissued",
                    "the settlement snapshot records the same-control violation");
        }
        REQUIRE(waitpid(sched, nullptr, WNOHANG) == 0, "scheduler alive");
        REQUIRE(!probe_died.load(), "control probe never lost the scheduler");

        close(ictrl);
        shutdown = true;
        probe_thread.join(); cs_thread.join(); healthy_thread.join();
        close(ctrl);
        delete w; delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        int st = -1;
        REQUIRE(waitpid(sched, &st, 0) == sched, "scheduler reaped");
        REQUIRE(WIFEXITED(st) && WEXITSTATUS(st) == 0, "clean shutdown");
        if (failures) { fprintf(stderr, "RESULT: FAIL (%d)\n", failures); return 1; }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (internalsrace_mode) {
        MsgChannel *w = connect_daemon(port, 0);
        REQUIRE(w != nullptr, "race worker connected");
        if (w) {
            LoginMsg login(10275, "frace", kPlatform, 0);
            login.envs.push_back(std::make_pair(kPlatform, kEnv));
            login.max_kids = 4;
            REQUIRE(w->send_msg(login), "race worker logged in");
            StatsMsg st; w->send_msg(st);
        }
        usleep(400 * 1000);
        {
            long long present = query_submitter_field(port, "frace", "jobs=");
            REQUIRE(present >= 0, "race worker is logged in before the fan-out");
        }
        const int ictrl = tcp_connect(port + 1, 0);
        REQUIRE(ictrl >= 0, "race control connected");
        { char g[256]; struct pollfd gp = { ictrl, POLLIN, 0 };
          if (poll(&gp,1,3000)>0){ ssize_t z=read(ictrl,g,sizeof(g)); (void)z; } }
        { const char *icmd = "internals frace\n";
          REQUIRE(write(ictrl, icmd, strlen(icmd)) == (ssize_t)strlen(icmd),
                  "internals command sent"); }
        /* Give the request time to flush (tick promotion is disabled), then
           the worker replies: the ONLY acceptance path is the handler
           recheck.  */
        usleep(400 * 1000);
        REQUIRE(w->send_msg(StatusTextMsg("frace internal status: ok\n")),
                "worker replied after its request flushed");

        std::string reply;
        {
            const Clock::time_point t0 = Clock::now();
            char b[4096];
            while (reply.find("200 done") == std::string::npos && secs_since(t0) < 20) {
                struct pollfd rp = { ictrl, POLLIN, 0 };
                if (poll(&rp,1,200) <= 0) { continue; }
                const ssize_t n = read(ictrl, b, sizeof(b)-1);
                if (n <= 0) { break; }
                b[n]=0; reply += b;
            }
        }
        REQUIRE(reply.find("200 done") != std::string::npos, "fan-out completed");
        REQUIRE(reply.find("frace internal status: ok") != std::string::npos,
                "the same-turn reply was ACCEPTED and forwarded (handler"
                " recheck promoted SEND_PENDING; next-turn-only promotion"
                " would time it out)");
        REQUIRE(reply.find("frace not reporting") == std::string::npos,
                "the target was not timed out");
        REQUIRE(waitpid(sched, nullptr, WNOHANG) == 0, "scheduler alive");
        REQUIRE(!probe_died.load(), "control probe never lost the scheduler");

        close(ictrl);
        shutdown = true;
        probe_thread.join(); cs_thread.join(); healthy_thread.join();
        close(ctrl);
        delete w; delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        int st = -1;
        REQUIRE(waitpid(sched, &st, 0) == sched, "scheduler reaped");
        REQUIRE(WIFEXITED(st) && WEXITSTATUS(st) == 0, "clean shutdown");
        if (failures) { fprintf(stderr, "RESULT: FAIL (%d)\n", failures); return 1; }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (exhaust_mode) {
        auto login_submitter = [&](MsgChannel *ch, const char *name) {
            LoginMsg login(0, name, kPlatform, 0);
            login.envs.push_back(std::make_pair(kPlatform, kEnv));
            login.max_kids = 0;
            login.noremote = true;
            return ch && ch->send_msg(login);
        };
        auto get_cs = [&](MsgChannel *ch, unsigned count, unsigned cid, const char *f) {
            GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                       f, CompileJob::Lang_CXX, count, kPlatform, 0, std::string(), 0, 0, 0);
            g.client_id = cid;
            return ch->send_msg(g);
        };
        MsgChannel *subX = connect_daemon(port, 0);
        MsgChannel *subP = connect_daemon(port, 0);
        REQUIRE(subX && subP, "exhaust submitter + peer connected");
        REQUIRE(login_submitter(subX, "fexX"), "exhaust submitter logged in");
        REQUIRE(login_submitter(subP, "fexP"), "peer submitter logged in");
        usleep(400 * 1000);

        /* A batch of 20 into an 8-id domain: the preflight must fail it
           fatally.  */
        REQUIRE(get_cs(subX, 20, 8801, "exhaust.cpp"), "count=20 batch sent");
        usleep(600 * 1000);

        /* No staged survivors: listjobs shows no member of this batch.  */
        long long survivors = -1;
        {
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < 8) {
                survivors = query_control_count(port, "listjobs", "exhaust.cpp");
                if (survivors == 0) { break; }
                usleep(200 * 1000);
            }
        }
        REQUIRE(survivors == 0,
                "the un-completable batch left NO staged survivors (fatal"
                " exhaustion closed the generation; the pre-fix park-and-retry"
                " leaves the domain-full staged members behind)");

        /* The submitter generation was closed: subX sees EOF.  */
        {
            bool closed = false;
            const Clock::time_point t0 = Clock::now();
            while (!closed && secs_since(t0) < 5) {
                Msg *m = subX->get_msg(1);
                if (m) { delete m; }
                if (subX->at_eof()) { closed = true; }
                else if (!m) { usleep(100 * 1000); }
            }
            REQUIRE(closed, "the exhausted submitter's connection was closed");
        }

        /* An unrelated peer stays live and is served.  */
        REQUIRE(get_cs(subP, 1, 8802, "peer.cpp"), "peer request sent");
        unsigned pjid = 0;
        {
            const Clock::time_point t0 = Clock::now();
            while (pjid == 0 && secs_since(t0) < 20) {
                Msg *m = subP->get_msg(2);
                if (!m) { continue; }
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u && u->client_id == 8802) { pjid = u->job_id; }
                }
                delete m;
            }
        }
        REQUIRE(pjid != 0, "an unrelated peer is still served after the fatal batch");
        if (pjid) { confirm_job(pjid); }

        REQUIRE(waitpid(sched, nullptr, WNOHANG) == 0, "scheduler alive");
        REQUIRE(!probe_died.load(), "control probe never lost the scheduler");

        shutdown = true;
        probe_thread.join(); cs_thread.join(); healthy_thread.join();
        close(ctrl);
        delete subX; delete subP; delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        int st = -1;
        REQUIRE(waitpid(sched, &st, 0) == sched, "scheduler reaped");
        REQUIRE(WIFEXITED(st) && WEXITSTATUS(st) == 0, "clean shutdown");
        if (failures) { fprintf(stderr, "RESULT: FAIL (%d)\n", failures); return 1; }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (duplocal_mode) {
        /* A logged-in daemon (host running local jobs) and a monitor.  */
        MsgChannel *dl = connect_daemon(port, 0);
        REQUIRE(dl != nullptr, "duplocal daemon connected");
        if (dl) {
            LoginMsg login(10270, "fdup", kPlatform, 0);
            login.envs.push_back(std::make_pair(kPlatform, kEnv));
            login.max_kids = 4;
            REQUIRE(dl->send_msg(login), "duplocal daemon logged in");
            StatsMsg st; dl->send_msg(st);
        }
        MsgChannel *mon = connect_daemon(port, 0);
        REQUIRE(mon != nullptr, "monitor connected");
        if (mon) { mon->send_msg(MonLoginMsg()); }
        usleep(400 * 1000);

        /* Baseline allocator state before the trace.  */
        const long long live0 = query_control_field(port, "listjobs", "alloc_live=", "alloc_live=");
        const long long issued0 = query_control_field(port, "listjobs", "alloc_issued=", "alloc_issued=");
        const long long viol0 = query_control_field(port, "listjobs", "id_release_violations=", "id_release_violations=");
        REQUIRE(live0 >= 0 && issued0 >= 0 && viol0 >= 0,
                "allocator baseline readable");

        /* The ruled trace: Begin(k), Begin(k), Done(k), Done(k) for one
           client-local id.  */
        const int k = 4242;
        REQUIRE(dl->send_msg(JobLocalBeginMsg(k, "duplocal.cpp", true)),
                "first local Begin sent");
        usleep(150 * 1000);
        REQUIRE(dl->send_msg(JobLocalBeginMsg(k, "duplocal.cpp", true)),
                "duplicate local Begin sent");
        usleep(150 * 1000);
        REQUIRE(dl->send_msg(JobLocalDoneMsg(k)), "first local Done sent");
        usleep(150 * 1000);
        REQUIRE(dl->send_msg(JobLocalDoneMsg(k)), "duplicate local Done sent");
        usleep(400 * 1000);

        /* Count the monitor's local-job events.  */
        int begins = 0, dones = 0, done_id_zero = 0;
        unsigned begin_global = 0, done_global = 0;
        {
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < 3) {
                Msg *m = mon->get_msg(1);
                if (!m) { continue; }
                if (MSG_IS(m, MON_LOCAL_JOB_BEGIN)) {
                    MonLocalJobBeginMsg *b2 = dynamic_cast<MonLocalJobBeginMsg *>(m);
                    if (b2) { ++begins; begin_global = b2->job_id; }
                } else if (MSG_IS(m, JOB_LOCAL_DONE)) {
                    JobLocalDoneMsg *d = dynamic_cast<JobLocalDoneMsg *>(m);
                    if (d) { ++dones; done_global = d->job_id; if (d->job_id == 0) { ++done_id_zero; } }
                }
                delete m;
            }
        }
        REQUIRE(begins == 1,
                "the duplicate local Begin allocated nothing and emitted no"
                " second monitor Begin (idempotent)");
        REQUIRE(dones == 1,
                "exactly one local terminal fired");
        REQUIRE(done_id_zero == 0,
                "no local terminal was emitted for global id 0 (the old"
                " operator[] default-insert bug)");
        REQUIRE(begin_global != 0 && dones == 1,
                "the terminal named the real global id, not 0");
        REQUIRE(done_global == begin_global,
                "the local Done named the SAME global id the Begin allocated");
        {
            const long long live1 = query_control_field(port, "listjobs", "alloc_live=", "alloc_live=");
            const long long issued1 = query_control_field(port, "listjobs", "alloc_issued=", "alloc_issued=");
            const long long viol1 = query_control_field(port, "listjobs", "id_release_violations=", "id_release_violations=");
            REQUIRE(live1 == live0,
                    "allocator live count returned to baseline (the id was released)");
            REQUIRE(issued1 - issued0 == 1,
                    "exactly ONE id was allocated across the whole trace");
            REQUIRE(viol1 - viol0 == 0,
                    "no id-release accounting violations during the trace");
        }
        /* The scheduler's own duplicate counter must show exactly one.  */
        {
            long long dup = -1;
            const Clock::time_point t0 = Clock::now();
            while (dup < 1 && secs_since(t0) < 8) {
                dup = query_control_field(port, "listjobs", "dup_local_begin=", "dup_local_begin=");
                if (dup < 1) { usleep(200 * 1000); }
            }
            REQUIRE(dup == 1, "the scheduler counted exactly one duplicate local Begin");
        }
        REQUIRE(waitpid(sched, nullptr, WNOHANG) == 0, "scheduler alive");
        REQUIRE(!probe_died.load(), "control probe never lost the scheduler");

        shutdown = true;
        probe_thread.join(); cs_thread.join(); healthy_thread.join();
        close(ctrl);
        delete dl; delete mon; delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        int st = -1;
        REQUIRE(waitpid(sched, &st, 0) == sched, "scheduler reaped");
        REQUIRE(WIFEXITED(st) && WEXITSTATUS(st) == 0, "clean shutdown");
        if (failures) { fprintf(stderr, "RESULT: FAIL (%d)\n", failures); return 1; }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (internalsuaf_mode) {
        /* Two extra workers become internals targets (fakecs is the
           third).  They never reply, so at the command deadline -- or
           immediately once all targets are terminal -- finalize()
           iterates every target record; a disconnected target whose
           CompileServer was freed is the UAF window.  */
        MsgChannel *csA = connect_daemon(port, 0);
        MsgChannel *csB = connect_daemon(port, 0);
        REQUIRE(csA && csB, "two internals-target workers connected");
        if (csA) {
            LoginMsg la(10255, "fcsA", kPlatform, 0);
            la.envs.push_back(std::make_pair(kPlatform, kEnv));
            la.max_kids = 4;
            REQUIRE(csA->send_msg(la), "worker A logged in");
            StatsMsg st; csA->send_msg(st);
        }
        if (csB) {
            LoginMsg lb(10256, "fcsB", kPlatform, 0);
            lb.envs.push_back(std::make_pair(kPlatform, kEnv));
            lb.max_kids = 4;
            REQUIRE(csB->send_msg(lb), "worker B logged in");
            StatsMsg st; csB->send_msg(st);
        }
        /* PRECONDITION: both workers must be logged in (present in
           listcs) before the fan-out, or they would not be captured as
           targets and the freed-target window would never be exercised.
           Poll, bounded.  */
        {
            const Clock::time_point t0 = Clock::now();
            bool both = false;
            while (!both && secs_since(t0) < 15) {
                const long long a = query_submitter_field(port, "fcsA", "jobs=");
                const long long b = query_submitter_field(port, "fcsB", "jobs=");
                both = (a >= 0 && b >= 0);
                if (!both) { usleep(200 * 1000); }
            }
            REQUIRE(both, "both internals-target workers are logged in before the fan-out");
        }

        /* A dedicated control connection issues the fan-out, SCOPED to the
           two disconnectable workers (so finalize runs over exactly them
           -- not the always-live fakecs -- the moment both disconnect).  */
        const int ictrl = tcp_connect(port + 1, 0);
        REQUIRE(ictrl >= 0, "internals control connected");
        {
            char g[256]; struct pollfd gp = { ictrl, POLLIN, 0 };
            if (poll(&gp, 1, 3000) > 0) { ssize_t z = read(ictrl, g, sizeof(g)); (void)z; }
        }
        { const char *icmd = "internals fcsA fcsB\n";
          REQUIRE(write(ictrl, icmd, strlen(icmd)) == (ssize_t)strlen(icmd),
                  "internals command sent"); }
        /* Let both request frames flush (targets reach WAITING_REPLY),
           then disconnect ONLY csA and keep csB connected-but-silent.
           csB holds the transaction open, so finalize runs at the command
           DEADLINE -- long after csA's CompileServer was freed -- and
           iterates csA's freed record.  This maximizes and guarantees the
           freed-before-finalize gap (the both-disconnect path reached
           finalize too promptly for ASan to observe the free).  */
        usleep(400 * 1000);
        delete csA; csA = nullptr;

        /* finalize() (immediate, all-terminal) or the deadline must
           produce a complete 200 done without touching freed memory.  */
        std::string reply;
        {
            const Clock::time_point t0 = Clock::now();
            char b[4096];
            while (reply.find("200 done") == std::string::npos
                   && secs_since(t0) < 20) {
                struct pollfd rp = { ictrl, POLLIN, 0 };
                if (poll(&rp, 1, 200) <= 0) { continue; }
                const ssize_t n = read(ictrl, b, sizeof(b) - 1);
                if (n <= 0) { break; }
                b[n] = 0; reply += b;
            }
        }
        REQUIRE(reply.find("200 done") != std::string::npos,
                "the fan-out completed with a terminal frame after both"
                " targets disconnected mid-transaction");
        REQUIRE(reply.find("fcsA") != std::string::npos
                && reply.find("fcsB") != std::string::npos,
                "finalize iterated BOTH targets: fcsA (disconnected, its"
                " CompileServer freed ~10s before finalize) and fcsB"
                " (timed out) -- the freed-record iteration is the UAF window");
        if (csB) { delete csB; csB = nullptr; }
        REQUIRE(waitpid(sched, nullptr, WNOHANG) == 0,
                "the scheduler survived finalize over a freed target"
                " (the pointer-keyed variant use-after-frees here)");
        REQUIRE(!probe_died.load(), "the control probe never lost the scheduler");
        REQUIRE(worst_reply.load() < 5.0, "scheduler stayed responsive");

        close(ictrl);
        shutdown = true;
        probe_thread.join(); cs_thread.join(); healthy_thread.join();
        close(ctrl);
        delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        int st = -1;
        REQUIRE(waitpid(sched, &st, 0) == sched, "scheduler reaped");
        REQUIRE(WIFEXITED(st) && WEXITSTATUS(st) == 0, "clean shutdown");
        if (failures) { fprintf(stderr, "RESULT: FAIL (%d)\n", failures); return 1; }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (teardown_mode) {
        MsgChannel *subT = connect_daemon(port, 0);
        REQUIRE(subT != nullptr, "teardown submitter connected");
        if (subT) {
            LoginMsg login(0, "fakesubT", kPlatform, 0);
            login.envs.push_back(std::make_pair(kPlatform, kEnv));
            login.max_kids = 0;
            login.noremote = true;
            REQUIRE(subT->send_msg(login), "teardown submitter logged in");
        }
        MsgChannel *mon = connect_daemon(port, 0);
        REQUIRE(mon != nullptr, "monitor connected");
        if (mon) {
            mon->send_msg(MonLoginMsg());
        }
        usleep(300 * 1000);

        /* One request; drain its UseCS to learn the assigned job id (one
           worker fakecs, so it lands there).  */
        GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                   "teardown.cpp", CompileJob::Lang_CXX, 1, kPlatform, 0, std::string(), 0, 0, 0);
        g.client_id = 7700;
        REQUIRE(subT && subT->send_msg(g), "teardown request sent");
        unsigned int jid = 0;
        {
            const Clock::time_point t0 = Clock::now();
            while (jid == 0 && secs_since(t0) < 30) {
                Msg *m = subT->get_msg(2);
                if (!m) { continue; }
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u && u->client_id == 7700) { jid = u->job_id; }
                }
                delete m;
            }
        }
        REQUIRE(jid != 0, "the job was assigned to a worker");

        /* JobBegin, no Done: the job is now COMPILING on the worker.  */
        begin_job(jid);
        {
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < 15) {
                if (job_in_scheduler(port, jid)
                        && worker_job_count(port, "fakecs") >= 1) {
                    break;
                }
                usleep(200 * 1000);
            }
        }
        REQUIRE(job_in_scheduler(port, jid), "the job is COMPILING before the disconnect");
        REQUIRE(worker_job_count(port, "fakecs") >= 1,
                "the worker holds its reservation before the disconnect");

        /* Duplicate begin from the assigned worker while ATTACHED: the
           transition check must ignore it -- no timestamp reset, no second
           monitor begin event -- and count it.  */
        begin_job(jid);
        {
            long long rejects = -1;
            const Clock::time_point t0 = Clock::now();
            while (rejects < 1 && secs_since(t0) < 10) {
                rejects = query_control_field(port, "listjobs",
                                              "nonwaiting_begin_rejects=",
                                              "nonwaiting_begin_rejects=");
                if (rejects < 1) { usleep(200 * 1000); }
            }
            REQUIRE(rejects == 1,
                    "the duplicate begin was counted and ignored (attached)");
        }
        REQUIRE(job_in_scheduler(port, jid),
                "the job survived the duplicate begin");

        /* The submitter disconnects mid-compile.  */
        delete subT;
        subT = nullptr;

        /* It must be RETAINED: give handle_end time to process the EOF,
           then assert the job and reservation still stand and no premature
           terminal event fired.  */
        sleep(3);
        REQUIRE(job_in_scheduler(port, jid),
                "the COMPILING job is RETAINED after its submitter disconnects"
                " (deleting it would free the worker under a running compile)");
        REQUIRE(worker_job_count(port, "fakecs") >= 1,
                "the worker reservation is retained across the disconnect");
        int done_before = 0;
        {
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < 2) {
                Msg *m = mon->get_msg(1);
                if (!m) { continue; }
                if (MSG_IS(m, MON_JOB_DONE)) {
                    MonJobDoneMsg *d = dynamic_cast<MonJobDoneMsg *>(m);
                    if (d && d->job_id == jid) { ++done_before; }
                }
                delete m;
            }
        }
        REQUIRE(done_before == 0,
                "no premature terminal event fired for the retained job");

        /* The detached identity stays observable: listjobs must name the
           gone submitter, never a bare "<>".  */
        REQUIRE(query_control_count(port, "listjobs", "fakesubT<detached>") == 1,
                "listjobs shows the detached job's submitter-name snapshot");

        /* Duplicate begin while DETACHED: same rule, counted and ignored.  */
        begin_job(jid);
        {
            long long rejects = -1;
            const Clock::time_point t0 = Clock::now();
            while (rejects < 2 && secs_since(t0) < 10) {
                rejects = query_control_field(port, "listjobs",
                                              "nonwaiting_begin_rejects=",
                                              "nonwaiting_begin_rejects=");
                if (rejects < 2) { usleep(200 * 1000); }
            }
            REQUIRE(rejects == 2,
                    "the duplicate begin was counted and ignored (detached)");
        }

        /* TERMINAL AUTHORITY: a REPLACEMENT submitter connection (the
           natural successor of the disconnected daemon -- same node name,
           fresh connection) sends a non-worker JobDone for the retained
           id.  A null submitter must not act as a wildcard: the completion
           must be IGNORED and counted, the job and the worker reservation
           must stand, and the sender must NOT be kicked (a stale id after
           a vanished client is normal daemon behaviour).  */
        MsgChannel *subT2 = connect_daemon(port, 0);
        REQUIRE(subT2 != nullptr, "replacement submitter connected");
        if (subT2) {
            LoginMsg login(0, "fakesubT", kPlatform, 0);
            login.envs.push_back(std::make_pair(kPlatform, kEnv));
            login.max_kids = 0;
            login.noremote = true;
            REQUIRE(subT2->send_msg(login), "replacement submitter logged in");
            usleep(300 * 1000);
            JobDoneMsg stale(jid, 0, JobDoneMsg::FROM_SUBMITTER);
            REQUIRE(subT2->send_msg(stale), "stale non-worker completion sent");
        }
        {
            long long rejects = -1;
            const Clock::time_point t0 = Clock::now();
            while (rejects < 1 && secs_since(t0) < 10) {
                rejects = query_control_field(port, "listjobs",
                                              "detached_terminal_rejects=",
                                              "detached_terminal_rejects=");
                if (rejects < 1) { usleep(200 * 1000); }
            }
            REQUIRE(rejects == 1,
                    "the stale completion was rejected and counted");
        }
        REQUIRE(job_in_scheduler(port, jid),
                "the retained job survived the stale non-worker completion"
                " (a null submitter is not a terminal wildcard)");
        REQUIRE(worker_job_count(port, "fakecs") >= 1,
                "the worker reservation survived the stale completion");

        /* The worker's real completion reconciles it exactly once (a nonzero
           runtime keeps add_job_stats on its normal path).  */
        finish_job(jid, 100);
        {
            const Clock::time_point t0 = Clock::now();
            while (job_in_scheduler(port, jid) && secs_since(t0) < 15) {
                usleep(200 * 1000);
            }
        }
        /* CRASH-SENSITIVE: the earlier version of this fix crashed the
           scheduler on the retained job's deletion, and a boolean
           job_in_scheduler() read the death as "job absent" -> false PASS.
           Require, after the real Done: a LIVE scheduler, a COMPLETE
           listjobs showing the job gone, a COMPLETE listcs showing the
           worker reservation released, and the probe never dying.  */
        REQUIRE(waitpid(sched, nullptr, WNOHANG) == 0,
                "the scheduler is still alive after the retained job completes"
                " (the previous fix crashed here)");
        REQUIRE(!probe_died.load(), "the control probe never lost the scheduler");
        {
            /* Poll a COMPLETE listjobs (query_control_count returns -1 on an
               incomplete/failed reply, which must NOT read as "absent").  */
            long long present = -1;
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < 10) {
                present = query_control_count(port, "listjobs", "teardown.cpp");
                if (present == 0) { break; }
                usleep(200 * 1000);
            }
            REQUIRE(present == 0,
                    "listjobs completes and shows the job absent (a failed"
                    " query returns -1, never a false absent)");
            long long wj = worker_job_count(port, "fakecs");
            const Clock::time_point t1 = Clock::now();
            while (wj != 0 && secs_since(t1) < 10) {
                usleep(200 * 1000);
                wj = worker_job_count(port, "fakecs");
            }
            REQUIRE(wj == 0,
                    "listcs completes and shows the worker reservation released");
        }
        int done_after = 0;
        {
            const Clock::time_point t0 = Clock::now();
            while (secs_since(t0) < 3) {
                Msg *m = mon->get_msg(1);
                if (!m) { continue; }
                if (MSG_IS(m, MON_JOB_DONE)) {
                    MonJobDoneMsg *d = dynamic_cast<MonJobDoneMsg *>(m);
                    if (d && d->job_id == jid) { ++done_after; }
                }
                delete m;
            }
        }
        REQUIRE(done_after == 1,
                "the monitor saw exactly one terminal event, at the real completion");

        /* The replacement submitter was not kicked: it can still submit
           and be served.  */
        {
            GetCSMsg g2(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                        "afterward.cpp", CompileJob::Lang_CXX, 1, kPlatform, 0, std::string(), 0, 0, 0);
            g2.client_id = 7701;
            REQUIRE(subT2 && subT2->send_msg(g2), "replacement submitter still writable");
            unsigned int jid3 = 0;
            const Clock::time_point t0 = Clock::now();
            while (jid3 == 0 && secs_since(t0) < 20) {
                Msg *m = subT2->get_msg(2);
                if (!m) { continue; }
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u && u->client_id == 7701) { jid3 = u->job_id; }
                }
                delete m;
            }
            REQUIRE(jid3 != 0,
                    "the replacement submitter is still served after the"
                    " stale completion (it was not kicked)");
            if (jid3) {
                confirm_job(jid3);
                const Clock::time_point t1 = Clock::now();
                while (job_in_scheduler(port, jid3) && secs_since(t1) < 15) {
                    usleep(200 * 1000);
                }
            }
        }

        /* SIBLING TERMINAL: the worker itself disconnects while holding a
           DETACHED job.  A second worker on a distinct platform makes the
           placement deterministic; its disconnect must end the job exactly
           once with no null dereference.  */
        {
            static const char *kAltPlat = "x86_64-alt";
            static const char *kAltEnv = "altenv";
            MsgChannel *cs2 = connect_daemon(port, 0);
            REQUIRE(cs2 != nullptr, "second worker connected");
            unsigned int jid2 = 0;
            MsgChannel *subU = nullptr;
            if (cs2) {
                LoginMsg login(10250, kAltPlat, kAltPlat, 0);
                login.envs.push_back(std::make_pair(kAltPlat, kAltEnv));
                login.max_kids = 2;
                login.chroot_possible = true;
                REQUIRE(cs2->send_msg(login), "second worker logged in");
                StatsMsg st;   /* fresh daemons report load=1000 until the
                                  first stats: send one immediately */
                cs2->send_msg(st);
                usleep(300 * 1000);

                subU = connect_daemon(port, 0);
                REQUIRE(subU != nullptr, "sibling submitter connected");
                if (subU) {
                    LoginMsg slog(0, "fakesubU", kAltPlat, 0);
                    slog.envs.push_back(std::make_pair(kAltPlat, kAltEnv));
                    slog.max_kids = 0;
                    slog.noremote = true;
                    REQUIRE(subU->send_msg(slog), "sibling submitter logged in");
                    usleep(300 * 1000);
                    GetCSMsg g3(Environments{std::make_pair(std::string(kAltPlat), std::string(kAltEnv))},
                                "sibling.cpp", CompileJob::Lang_CXX, 1, kAltPlat, 0, std::string(), 0, 0, 0);
                    g3.client_id = 7800;
                    REQUIRE(subU->send_msg(g3), "sibling request sent");
                    const Clock::time_point t0 = Clock::now();
                    while (jid2 == 0 && secs_since(t0) < 20) {
                        Msg *m = subU->get_msg(2);
                        if (!m) { continue; }
                        if (MSG_IS(m, USE_CS)) {
                            UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                            if (u && u->client_id == 7800) { jid2 = u->job_id; }
                        }
                        delete m;
                    }
                }
            }
            REQUIRE(jid2 != 0, "the sibling job was assigned (to the alt worker)");
            if (jid2 && cs2) {
                JobBeginMsg jb(jid2, 1);
                REQUIRE(cs2->send_msg(jb), "alt worker began the sibling job");
                const Clock::time_point t0 = Clock::now();
                while (secs_since(t0) < 15 && !job_in_scheduler(port, jid2)) {
                    usleep(200 * 1000);
                }
                REQUIRE(job_in_scheduler(port, jid2), "sibling job is COMPILING");

                delete subU;   /* submitter disconnects -> job detaches */
                subU = nullptr;
                sleep(2);
                REQUIRE(job_in_scheduler(port, jid2),
                        "the sibling job is retained after ITS submitter left");

                delete cs2;    /* now the WORKER disconnects */
                cs2 = nullptr;
                long long present = -1;
                const Clock::time_point t1 = Clock::now();
                while (secs_since(t1) < 15) {
                    present = query_control_count(port, "listjobs", "sibling.cpp");
                    if (present == 0) { break; }
                    usleep(200 * 1000);
                }
                REQUIRE(present == 0,
                        "the worker's disconnect ended the detached job"
                        " (complete listjobs shows it gone; no crash)");
                int done2 = 0;
                const Clock::time_point t2 = Clock::now();
                while (secs_since(t2) < 3) {
                    Msg *m = mon->get_msg(1);
                    if (!m) { continue; }
                    if (MSG_IS(m, MON_JOB_DONE)) {
                        MonJobDoneMsg *d = dynamic_cast<MonJobDoneMsg *>(m);
                        if (d && d->job_id == jid2) { ++done2; }
                    }
                    delete m;
                }
                REQUIRE(done2 == 1,
                        "the worker disconnect produced exactly one terminal"
                        " event for the detached job");
            } else {
                delete subU;
                delete cs2;
            }
        }

        /* Final liveness, RE-CHECKED AFTER the last management queries so
           an exit during them cannot escape, then the requested shutdown
           must exit cleanly (SIGTERM -> exit_main_loop -> status 0).  */
        REQUIRE(worst_reply.load() < 5.0, "scheduler stayed responsive");
        REQUIRE(!probe_died.load(), "the control probe never lost the scheduler");
        REQUIRE(waitpid(sched, nullptr, WNOHANG) == 0,
                "the scheduler is alive after every management query,"
                " immediately before the requested shutdown");

        shutdown = true;
        probe_thread.join(); cs_thread.join(); healthy_thread.join();
        close(ctrl);
        delete subT2; delete mon; delete sub; delete sub2; delete cs;
        kill(sched, SIGTERM);
        int st = -1;
        REQUIRE(waitpid(sched, &st, 0) == sched, "the scheduler was reaped");
        REQUIRE(WIFEXITED(st) && WEXITSTATUS(st) == 0,
                "the requested shutdown exited cleanly (status 0)");
        if (failures) { fprintf(stderr, "RESULT: FAIL (%d)\n", failures); return 1; }
        fprintf(stderr, "RESULT: PASS\n");
        return 0;
    }

    if (clientstall_mode) {
        /* One connection (the daemon) carrying several client ids (the
           wrappers).  One id is deliberately never confirmed -- that is the
           frozen wrapper -- while the others behave normally throughout.  */
        const unsigned frozen_cid = 4242;
        auto send_for = [&](unsigned cid, const char *fname) {
            GetCSMsg g(Environments{std::make_pair(std::string(kPlatform), std::string(kEnv))},
                       fname, CompileJob::Lang_CXX, 1, kPlatform, 0, std::string(), 0, 0, 0);
            g.client_id = cid;
            return sub->send_msg(g);
        };

        fprintf(stderr, "# clientstall: one wrapper (client_id %u) freezes after its assignment\n",
                frozen_cid);
        REQUIRE(send_for(frozen_cid, "frozen.cpp"), "frozen wrapper's request sent");
        unsigned frozen_job = 0;
        {
            const Clock::time_point t0 = Clock::now();
            while (frozen_job == 0 && secs_since(t0) < 30) {
                Msg *m = sub->get_msg(2);
                if (!m) { continue; }
                UseCSMsg *u = MSG_IS(m, USE_CS) ? dynamic_cast<UseCSMsg *>(m) : nullptr;
                if (u && u->client_id == frozen_cid) { frozen_job = u->job_id; }
                delete m;   /* deliberately NOT confirmed: the wrapper is frozen */
            }
        }
        REQUIRE(frozen_job != 0, "the frozen wrapper received its assignment");

        /* Healthy siblings on the SAME daemon connection, running across
           the whole stall bound.  */
        int healthy_done = 0;
        const Clock::time_point t_start = Clock::now();
        unsigned cid = 5000;
        while (secs_since(t_start) < 22) {          // > the 10s configured bound
            char fname[32];
            snprintf(fname, sizeof(fname), "healthy%u.cpp", cid);
            if (!send_for(cid, fname)) {
                break;                               // connection died: caught below
            }
            const Clock::time_point tj = Clock::now();
            bool got = false;
            while (!got && secs_since(tj) < 5) {
                Msg *m = sub->get_msg(1);
                if (!m) { continue; }
                unsigned jid = 0, got_cid = 0;
                if (MSG_IS(m, USE_CS)) {
                    UseCSMsg *u = dynamic_cast<UseCSMsg *>(m);
                    if (u) { jid = u->job_id; got_cid = u->client_id; }
                } else if (MSG_IS(m, NO_CS)) {
                    NoCSMsg *n = dynamic_cast<NoCSMsg *>(m);
                    if (n) { jid = n->job_id; got_cid = n->client_id; }
                }
                if (jid && got_cid == cid) { confirm_job(jid); ++healthy_done; got = true; }
                delete m;
            }
            ++cid;
            usleep(200 * 1000);
        }
        fprintf(stderr, "# clientstall: healthy siblings served across the bound: %d\n", healthy_done);

        REQUIRE(!sub->at_eof(),
                "the daemon was NOT removed because one of its wrappers stalled");
        REQUIRE(healthy_done >= 10,
                "healthy wrappers on the same daemon kept being served across the bound");

        /* The stale assignment must be RETAINED, not released: its UseCS is
           already in the frozen wrapper's hands, so the worker reservation
           has to stand until something actually cancels or completes it.
           Releasing it would let the worker be overcommitted and a late
           thaw execute against a job the scheduler had written off.  */
        const long long outstanding = query_submitter_outstanding(port, "fakesub");
        fprintf(stderr, "# clientstall: outstanding dispatches after the bound: %lld\n", outstanding);
        REQUIRE(outstanding >= 1,
                "the stale assignment and its worker reservation are RETAINED, not released");
        {
            bool quarantined = false, daemon_removed = false, expired = false;
            FILE *lf = fopen(sched_log.c_str(), "r");
            if (lf) {
                char line[4096];
                while (fgets(line, sizeof(line), lf)) {
                    if (strstr(line, "is not progressing")) { quarantined = true; }
                    if (strstr(line, "remove daemon fakesub")) { daemon_removed = true; }
                    if (strstr(line, "expiring assignment")) { expired = true; }
                }
                fclose(lf);
            }
            REQUIRE(quarantined, "the scheduler reported the unconfirmed assignment");
            REQUIRE(!daemon_removed, "the scheduler did NOT remove the submitting daemon");
            REQUIRE(!expired, "the scheduler did NOT delete an assignment nobody cancelled");
        }
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
            login.max_kids = 8;
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

        /* Phase 1 -- four held jobs against a 2-slot host A and an 8-slot
           host C.  The exact-fraction comparison makes the sequence
           deterministic REGARDLESS of the first (tied 0/2 vs 0/8) pick:
           whoever wins job 1, jobs 2-4 must land so the result is one on A
           and three on C -- floor-bucketing would call 1/2 and 1/8 equal
           and round-robin them 2/2.  */
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
        fprintf(stderr, "# leastbusy: fill spread a=%d c=%d (2-slot vs 8-slot)\n", on_a, on_c);
        REQUIRE(on_a == 1 && on_c == 3,
                "exact fractions place the fill 1:3 across unequal hosts");

        /* Phase 2 -- unequal-denominator decision: A at 1/2, C at 3/8.
           3*2 < 1*8, so the probe must go to C although C holds MORE
           jobs.  */
        {
            unsigned jid = 0, csport = 0;
            REQUIRE(send_one(9150, "unequal.cpp"), "unequal-denominator probe submitted");
            REQUIRE(await_use(9150, 15, &jid, &csport), "unequal probe assigned");
            fprintf(stderr, "# leastbusy: 3/8-vs-1/2 probe went to port=%u (want 10262)\n", csport);
            REQUIRE(csport == 10262,
                    "the lower FRACTION wins although it holds more jobs (cross multiplication)");
            begin_on(jid, csport);
            held.push_back(Held{jid, csport});
        }

        /* Phase 3 -- exact normalized tie: A at 1/2, C at 4/8; 1*8 == 4*2.
           Probe 1 lands at the tie and then COMPLETES, restoring the exact
           equality before probe 2 is submitted; round-robin must then pick
           the OTHER host at the identical tie.  (Without the restore, probe
           2's choice would be ordinary least-occupancy, not tie handling.)  */
        {
            /* Probe 1 lands at the tie, then COMPLETES, restoring the exact
               1/2 == 4/8 state before probe 2 -- otherwise probe 2's choice
               is ordinary least-occupancy, not tie handling.  Round-robin
               must then pick the OTHER host at the identical tie.  */
            unsigned j1 = 0, p1 = 0, j2 = 0, p2 = 0;
            REQUIRE(send_one(9160, "tie1.cpp"), "tie probe 1 submitted");
            REQUIRE(await_use(9160, 15, &j1, &p1), "tie probe 1 assigned");
            begin_on(j1, p1);
            done_on(j1, p1);
            usleep(500 * 1000);   // the completion restores the exact tie
            REQUIRE(send_one(9161, "tie2.cpp"), "tie probe 2 submitted");
            REQUIRE(await_use(9161, 15, &j2, &p2), "tie probe 2 assigned");
            begin_on(j2, p2);
            held.push_back(Held{j2, p2});
            fprintf(stderr, "# leastbusy: tie probes went to ports %u and %u\n", p1, p2);
            REQUIRE(p1 != p2,
                    "round-robin picks the OTHER host at an identical exact tie (1/2 == 4/8)");
        }

        /* Phase 4 -- fill to maxJobs everywhere, then the preload-zone
           probe: the picker must still assign (the bucketed form selected
           an EMPTY set here and stalled).  State: A 2/2; C needs 8/8.  */
        {
            /* Fill exactly the remaining capacity of BOTH hosts: the tie
               phase's outcome decides which host probe 2 occupies, so the
               remaining free-slot count is computed, not assumed.  After
               these tops both hosts sit AT maxJobs regardless of the path
               the earlier probes took.  */
            int cur_a = 0, cur_c = 0;
            for (const Held &h : held) {
                if (h.port == 10262) { ++cur_c; } else { ++cur_a; }
            }
            const int need_total = (2 - cur_a) + (8 - cur_c);
            for (int i = 0; i < need_total; ++i) {
                unsigned jid = 0, csport = 0;
                char fname[32];
                snprintf(fname, sizeof(fname), "top%d.cpp", i);
                REQUIRE(send_one(9170 + i, fname), "top-up job submitted");
                REQUIRE(await_use(9170 + i, 15, &jid, &csport), "top-up job assigned");
                begin_on(jid, csport);
                held.push_back(Held{jid, csport});
            }
        }
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

        /* Phase 5 -- empty host C completely, leave host A full.  The next
           job must land on C (0/8 beats 2/2 exactly).  */
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
