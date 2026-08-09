/*
    Connectivity-probe state machine regression (issue #4 item 4).

    Drives CompileServer's inbound-connectivity test directly -- no
    scheduler process -- through every transition the state machine must
    survive:

      - pending-success and pending-failure completions;
      - a synchronous connect() success, which the old code misclassified
        as a failure (it judged the new attempt against the PREVIOUS
        attempt's start clock);
      - resolver failure (the old code dereferenced a null hostent);
      - socket() and fcntl() failure injection (bounded failure
        transitions, never a crash or a hang);
      - more consecutive failures than the backoff table has entries (the
        old code indexed sizeof(table) -- the BYTE count -- slots past the
        end);
      - a leaping wall clock, which must not move any deadline (they are
        monotonic now).

    Failure injection comes from the conn_shim.so preload, armed per
    scenario with setenv/unsetenv (the shim reads the environment at call
    time).  Run under ASan/UBSan by the *-run.sh wrapper where available.
*/

#include "../scheduler/compileserver.h"
#include "../services/comm.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

/* A CompileServer whose probe target we control.  The channel fd is a
   socketpair end -- the connectivity path never uses it.  */
static CompileServer *make_cs(const char *host, int port)
{
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
        return nullptr;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    CompileServer *cs = new CompileServer(sp[0], (struct sockaddr *)&sa,
                                          sizeof(sa), false);
    close(sp[1]);
    cs->name = host;
    cs->setRemotePort((unsigned int)port);
    return cs;
}

/* Drive one full attempt to its terminal transition: start, then poll the
   probe fd to completion and apply the verdict exactly as the scheduler
   main loop does.  Returns true when the attempt ended in the accepting
   state (check-back scheduled) and false when it ended in backoff.  */
static bool drive_attempt(CompileServer *cs, int wait_ms)
{
    cs->startInConnectionTest();
    const int deadline_ms = wait_ms;
    int waited = 0;
    while (cs->getConnectionInProgress() && waited < deadline_ms) {
        struct pollfd pf;
        pf.fd = cs->getInFd();
        pf.events = POLLIN | POLLOUT;
        pf.revents = 0;
        const int r = poll(&pf, 1, 50);
        waited += 50;
        if (r > 0 || cs->getConnectionTimeout() == 0) {
            /* One verdict read per wake -- SO_ERROR is clear-on-read (the
               scheduler main loop follows the same rule).  */
            const bool up = cs->isConnected();
            if (r > 0 && up) {
                cs->updateInConnectivity(true);
                return true;
            }
            if (!up) {
                cs->updateInConnectivity(false);
                return false;
            }
        }
    }
    return !cs->getConnectionInProgress() && cs->getNextTimeout() > 50;
}

int main()
{
    /* A live listener: the success target.  */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in la;
    memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET;
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    la.sin_port = 0;
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&la, sizeof(la)) != 0
        || listen(lfd, 8) != 0) {
        fprintf(stderr, "cannot build listener\n");
        return 1;
    }
    socklen_t lalen = sizeof(la);
    getsockname(lfd, (struct sockaddr *)&la, &lalen);
    const int live_port = ntohs(la.sin_port);

    /* A refusing target: bind without listen, then close -- connections to
       the port are refused.  */
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in ca;
    memset(&ca, 0, sizeof(ca));
    ca.sin_family = AF_INET;
    ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ca.sin_port = 0;
    bind(cfd, (struct sockaddr *)&ca, sizeof(ca));
    socklen_t calen = sizeof(ca);
    getsockname(cfd, (struct sockaddr *)&ca, &calen);
    const int dead_port = ntohs(ca.sin_port);
    close(cfd);

    /* 1. pending-success: nonblocking connect to the live listener
       completes via poll; the verdict is success and the next probe is
       the ~60s check-back, not a backoff entry.  */
    {
        CompileServer *cs = make_cs("127.0.0.1", live_port);
        const bool ok = drive_attempt(cs, 3000);
        check(ok, "pending connect to a live listener completes as success");
        check(!cs->getConnectionInProgress(), "the probe fd was closed (success path)");
        check(cs->getNextTimeout() > 30, "success schedules the check-back, not a backoff");
        delete cs;
    }

    /* 2. pending-failure: a refused connect ends in the first backoff
       entry (2s), and the probe fd is closed exactly once.  */
    {
        CompileServer *cs = make_cs("127.0.0.1", dead_port);
        const bool ok = drive_attempt(cs, 3000);
        check(!ok, "pending connect to a refusing port completes as failure");
        check(!cs->getConnectionInProgress(), "the probe fd was closed (failure path)");
        const time_t nt = cs->getNextTimeout();
        check(nt >= 1 && nt <= 2, "the first failure schedules the first backoff entry");
        delete cs;
    }

    /* 3. synchronous connect success -- the misclassification regression.
       The shim completes connect() inline and returns 0.  The OLD code
       judged this against the PREVIOUS attempt's start time: after any
       earlier attempt more than 5s in the past, getConnectionTimeout()
       read 0 and the immediate success was recorded as a FAILURE.  The
       fix starts the attempt clock before connect and takes the SO_ERROR
       verdict directly.  */
    {
        CompileServer *cs = make_cs("127.0.0.1", live_port);
        /* age an earlier failed attempt so the old code's stale clock trap
           is armed */
        cs->updateInConnectivity(false);
        sleep(3);
        setenv("ICECC_CONN_SYNC_CONNECT", "1", 1);
        /* wait out the 2s backoff from the seeded failure */
        while (cs->getNextTimeout() > 0) {
            usleep(100 * 1000);
        }
        cs->startInConnectionTest();
        unsetenv("ICECC_CONN_SYNC_CONNECT");
        check(!cs->getConnectionInProgress(),
              "a synchronous connect reached a terminal transition inline");
        check(cs->getNextTimeout() > 30,
              "the synchronous success was classified as SUCCESS"
              " (the old code recorded it as a failure)");
        delete cs;
    }

    /* 4. resolver failure: must be a bounded failure transition, not a
       null dereference.  */
    {
        CompileServer *cs = make_cs("127.0.0.1", live_port);
        setenv("ICECC_CONN_FAIL_RESOLVE", "1", 1);
        cs->startInConnectionTest();
        unsetenv("ICECC_CONN_FAIL_RESOLVE");
        check(!cs->getConnectionInProgress(),
              "resolver failure ends the attempt (no probe left open)");
        check(cs->getNextTimeout() >= 1,
              "resolver failure schedules a backoff (bounded failure)");
        delete cs;
    }

    /* 5. socket() failure injection.  */
    {
        CompileServer *cs = make_cs("127.0.0.1", live_port);
        setenv("ICECC_CONN_FAIL_SOCKET", "1", 1);
        cs->startInConnectionTest();
        unsetenv("ICECC_CONN_FAIL_SOCKET");
        check(!cs->getConnectionInProgress() && cs->getNextTimeout() >= 1,
              "socket() failure is a bounded failure transition");
        delete cs;
    }

    /* 6. fcntl() failure injection.  */
    {
        CompileServer *cs = make_cs("127.0.0.1", live_port);
        setenv("ICECC_CONN_FAIL_FCNTL", "1", 1);
        cs->startInConnectionTest();
        unsetenv("ICECC_CONN_FAIL_FCNTL");
        check(!cs->getConnectionInProgress() && cs->getNextTimeout() >= 1,
              "fcntl() failure is a bounded failure transition");
        delete cs;
    }

    /* 7. more failures than the table has entries: the delay saturates at
       the last entry (4096s) and the index stays in bounds (an ASan build
       aborts here on the old byte-count table_size).  */
    {
        CompileServer *cs = make_cs("127.0.0.1", dead_port);
        bool bounded = true;
        time_t last_nt = 0;
        for (int i = 0; i < 20; ++i) {
            cs->updateInConnectivity(false);
            last_nt = cs->getNextTimeout();
            if (last_nt < 1 || last_nt > 4096) {
                bounded = false;
            }
        }
        check(bounded, "20 consecutive failures never leave the backoff table");
        check(last_nt > 4000, "the delay saturates at the final entry");
        delete cs;
    }

    /* 8. wall-clock jump: with the deadlines monotonic, a leaping time()
       must not move them.  The OLD wall-clock code read a huge forward
       jump as \"deadline passed\" (retry storm) or a backward jump as a
       far-future deadline (probe stall).  */
    {
        CompileServer *cs = make_cs("127.0.0.1", dead_port);
        cs->updateInConnectivity(false);   /* schedule the 2s backoff */
        setenv("ICECC_CONN_TIME_JUMP", "1", 1);
        const time_t nt = cs->getNextTimeout();
        cs->startInConnectionTest();       /* inside the backoff: must not start */
        const bool no_probe = !cs->getConnectionInProgress();
        unsetenv("ICECC_CONN_TIME_JUMP");
        check(nt >= 1 && nt <= 2, "a leaping wall clock does not move the backoff deadline");
        check(no_probe, "a leaping wall clock cannot force a premature retry");
        delete cs;
    }

    close(lfd);
    if (failures) {
        printf("RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    if (checks_executed == 0) {
        printf("RESULT: FAIL (no assertions executed)\n");
        return 1;
    }
    printf("# connectivity: %d assertions executed\n", checks_executed);
    printf("RESULT: PASS\n");
    return 0;
}
