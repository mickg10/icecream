/*
    Storage-failure and lifecycle tests for the state-writer process
    (daemon/statewriter.cpp), exercising the module directly:

      - delivery: framed records for both sinks land intact, in order;
      - partial pipe writes: a tiny pipe forces the daemon-side offset
        logic; every record still arrives exactly once, whole;
      - temporary open failure: an unwritable directory delays but does
        not lose subsequent records once permissions return (writer-side
        5s backoff), and the daemon side never blocks;
      - queue overflow: with the writer stopped, the bound holds and the
        oldest complete frames are dropped and counted;
      - oversized records are rejected and counted, never queued;
      - bounded shutdown: a SIGSTOPped writer cannot block shutdown()
        longer than its drain interval;
      - state-log sink survives rotation (open-append-close per record).
*/

#include "statewriter.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
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

static double now_s()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static std::vector<std::string> read_lines(const std::string &path)
{
    std::vector<std::string> lines;
    std::ifstream in(path.c_str());
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }
    return lines;
}

static bool wait_for_lines(const std::string &path, size_t want, double timeout_s)
{
    const double t0 = now_s();
    while (now_s() - t0 < timeout_s) {
        if (read_lines(path).size() >= want) {
            return true;
        }
        usleep(50 * 1000);
    }
    return false;
}

// Writer pid, for SIGSTOP tests: the only child this test process has.
static pid_t writer_pid()
{
    // /proc scan: our direct children.
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "pgrep -P %d", (int)getpid());
    FILE *p = popen(cmd, "r");
    pid_t pid = -1;
    if (p) {
        if (fscanf(p, "%d", &pid) != 1) {
            pid = -1;
        }
        pclose(p);
    }
    return pid;
}

static void test_delivery_and_partial_writes(const std::string &dir)
{
    const std::string jsonl = dir + "/out.jsonl";
    const std::string slog = dir + "/state.log";
    StateWriter w;
    REQUIRE(w.start(jsonl, slog), "writer process starts");

    // Records larger than the default pipe buffer in aggregate: the
    // nonblocking pump must hit EAGAIN and resume mid-frame.
    const std::string big(60000, 'x');
    for (int i = 0; i < 40; ++i) {
        std::ostringstream o;
        o << "{\"seq\":" << i << ",\"pad\":\"" << big << "\"}";
        w.enqueue(StateWriter::SINK_JSONL, o.str());
        w.pump();
    }
    w.enqueue(StateWriter::SINK_STATELOG, "statelog-line-1");
    const double t0 = now_s();
    while (w.pump() && now_s() - t0 < 20) {
        usleep(10 * 1000);
    }
    REQUIRE(wait_for_lines(jsonl, 40, 20), "all 40 large records arrive");
    const std::vector<std::string> lines = read_lines(jsonl);
    bool intact = lines.size() == 40;
    for (size_t i = 0; intact && i < lines.size(); ++i) {
        std::ostringstream expect;
        expect << "{\"seq\":" << i << ",\"pad\":\"" << big << "\"}";
        intact = lines[i] == expect.str();
    }
    REQUIRE(intact, "records are intact, whole and in order (offset logic)");
    REQUIRE(wait_for_lines(slog, 1, 10), "state-log sink written");

    // rotation: rename the state log; the next record recreates it
    rename(slog.c_str(), (slog + ".1").c_str());
    w.enqueue(StateWriter::SINK_STATELOG, "statelog-line-2");
    w.pump();
    REQUIRE(wait_for_lines(slog, 1, 10), "state-log recreated after rotation");

    REQUIRE(w.dropped() == 0, "no records dropped in the delivery test");
    w.shutdown(3000);
    REQUIRE(!w.started(), "writer exited on shutdown");
}

static void test_open_failure_recovery(const std::string &dir)
{
    /* A MISSING parent directory denies open() for every uid -- a chmod-000
       denial does not exist for root, and rpm %check runs as root.  */
    const std::string sub = dir + "/not-yet-created";
    const std::string jsonl = sub + "/out.jsonl";
    StateWriter w;
    REQUIRE(w.start(jsonl, ""), "writer starts against a missing directory");

    w.enqueue(StateWriter::SINK_JSONL, "{\"blocked\":1}");
    w.pump();
    usleep(300 * 1000);
    REQUIRE(read_lines(jsonl).empty(), "nothing written while the directory is missing");

    mkdir(sub.c_str(), 0755);
    // The writer backs off five seconds between open attempts.
    REQUIRE(wait_for_lines(jsonl, 1, 12),
            "record arrives once the directory exists (bounded backoff, nothing lost)");
    w.shutdown(3000);
}

static void test_overflow_and_oversize(const std::string &dir)
{
    const std::string jsonl = dir + "/overflow.jsonl";
    StateWriter w;
    REQUIRE(w.start(jsonl, ""), "writer starts for overflow test");
    const pid_t pid = writer_pid();
    REQUIRE(pid > 0, "writer pid found");
    kill(pid, SIGSTOP);   // stop consuming: the pipe and then the queue fill

    const std::string rec(100000, 'y');
    for (int i = 0; i < 100; ++i) {      // ~10 MB against a 4 MB bound
        w.enqueue(StateWriter::SINK_JSONL, rec);
    }
    REQUIRE(w.queued_bytes() <= 4 * 1024 * 1024,
            "queue bound holds under overflow");
    REQUIRE(w.dropped() > 0, "oldest frames dropped and counted");

    w.enqueue(StateWriter::SINK_JSONL, std::string(300000, 'z'));
    REQUIRE(w.oversized() == 1, "oversized record rejected and counted");

    kill(pid, SIGCONT);
    const double t0 = now_s();
    w.shutdown(5000);
    REQUIRE(now_s() - t0 < 10, "shutdown returns promptly after SIGCONT");
}

static void test_bounded_shutdown(const std::string &dir)
{
    const std::string jsonl = dir + "/shutdown.jsonl";
    StateWriter w;
    REQUIRE(w.start(jsonl, ""), "writer starts for shutdown test");
    const pid_t pid = writer_pid();
    kill(pid, SIGSTOP);   // simulate a writer stuck in storage
    w.enqueue(StateWriter::SINK_JSONL, "{\"x\":1}");
    const double t0 = now_s();
    w.shutdown(1000);
    const double took = now_s() - t0;
    REQUIRE(took < 3.0, "shutdown bounded by the drain interval (stuck writer killed)");
    REQUIRE(!w.started(), "writer gone after bounded shutdown");
}

/* Issue #3: after the daemon's GENERIC waitpid(-1) sweep reaps a dead
   writer, nothing reset the class state -- started() stayed true forever,
   telemetry said writer_alive:true, and enqueued records were silently
   dropped.  The class contract this locks in: alive() must transition to
   false even when the reap was STOLEN by an external waitpid (the ECHILD
   branch), and after that poll started() reports false too.  The daemon-
   side half of the fix (actually CALLING alive() from the loop and gating
   enqueue/telemetry on it) lives in daemon/main.cpp.  */
static void test_death_after_external_reap(const std::string &dir)
{
    const std::string jsonl = dir + "/death.jsonl";
    StateWriter w;
    REQUIRE(w.start(jsonl, ""), "writer starts for death test");
    const pid_t pid = writer_pid();
    REQUIRE(pid > 0, "writer pid found");
    REQUIRE(w.started() && w.alive(), "writer is up");

    kill(pid, SIGKILL);
    /* The daemon's generic zombie sweep gets there first.  */
    {
        int status = 0;
        pid_t r;
        for (int i = 0; i < 100; ++i) {
            r = waitpid(-1, &status, WNOHANG);
            if (r == pid) {
                break;
            }
            usleep(50 * 1000);
        }
        REQUIRE(r == pid, "the external sweep reaped the writer");
    }

    REQUIRE(w.started(), "started() alone still claims the writer is up --"
                         " the trap the daemon fell into");
    REQUIRE(!w.alive(), "alive() detects the death despite the stolen reap"
                        " (ECHILD branch)");
    REQUIRE(!w.started(), "and after that poll, started() is honest too");
    w.shutdown(500);
}

int main()
{
    char tmpl[] = "/tmp/statewriterXXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "cannot create temp dir\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);

    test_delivery_and_partial_writes(dir);
    test_open_failure_recovery(dir);
    test_overflow_and_oversize(dir);
    test_death_after_external_reap(dir);
    test_bounded_shutdown(dir);

    char cleanup[256];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", dir);
    if (system(cleanup) != 0) {
        fprintf(stderr, "warning: temp cleanup failed\n");
    }

    if (failures) {
        fprintf(stderr, "RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    fprintf(stderr, "RESULT: PASS\n");
    return 0;
}
