/*
    The daemonless local build bounds concurrency with one fcntl record
    lock per online CPU (dcc_lock_host), and execs the compiler WITHOUT
    forking.  An fcntl record lock survives exec only while its descriptor
    stays open -- and the lock fd is opened close-on-exec, so until
    dcc_lock_keep_across_exec() the slot was released the moment the
    compiler started: a farm-and-daemon outage ran one compiler per JOB
    instead of one per CPU (measured 32 simultaneous compilers on a
    16-CPU host).

    This gate tests the kernel lifetime invariant directly, without
    compilers or process-name sampling (a sampler can miss a short peak;
    the first version of the outage harness proved that the hard way):

      1. N helpers each take one of the N slots, clear close-on-exec the
         way build_local now does, and exec a blocking stub.  One helper
         runs with STDIN CLOSED so its lock lands on descriptor 0 -- the
         valid fd the first fix skipped with its `> 0` test.
      2. With all N slots held by EXEC'D processes, an N+1st acquisition
         must block: if any slot lock died at exec, it completes and the
         gate fails.
      3. Killing one stub must unblock the waiter: the bound is the slot
         pool, not something stricter.
*/

#include "../client/util.h"
#include "../services/ncpus.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int failures = 0;

static void check(bool ok, const char *what)
{
    if (ok) {
        printf("ok       - %s\n", what);
    } else {
        printf("FAILED   - %s\n", what);
        ++failures;
    }
    fflush(stdout);
}

/* One byte on the pipe means "I hold my slot (and have exec'd or am about
   to)".  The stub the holders exec must block forever and touch nothing.  */
static pid_t spawn_holder(int notify_fd, bool close_stdin)
{
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    if (close_stdin) {
        close(0);   /* the next open() -- the lock file -- returns fd 0 */
    }
    if (!dcc_lock_host()) {
        _exit(3);
    }
    if (!dcc_lock_keep_across_exec()) {
        _exit(4);
    }
    if (write(notify_fd, "L", 1) != 1) {
        _exit(5);
    }
    /* Exec: the whole point.  If the lock dies here, the slot frees while
       the "compiler" runs and the waiter below sails through.  */
    execl("/bin/sleep", "sleep", "600", (char *)nullptr);
    _exit(6);
}

static bool wait_bytes(int fd, int want, int timeout_sec)
{
    int got = 0;
    char c;
    while (got < want) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv = { timeout_sec, 0 };
        const int r = select(fd + 1, &rd, nullptr, nullptr, &tv);
        if (r <= 0) {
            return false;
        }
        if (read(fd, &c, 1) != 1) {
            return false;
        }
        ++got;
    }
    return true;
}

int main()
{
    int ncpus = 1;
    dcc_ncpus(&ncpus);
    /* The pool size is the box's CPU count; the invariant is the same at
       any size, and capping keeps the gate cheap on very wide machines.
       Capping is safe because the surplus slots are NEVER taken: the
       waiter can only be blocked by the N we hold if the pool arithmetic
       and lock lifetimes are right for those N.  */
    printf("# lockexec: slot pool = %d cpus\n", ncpus);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return 2;
    }

    pid_t holders[1024];
    int nheld = 0;
    for (int i = 0; i < ncpus && i < 1024; ++i) {
        /* The FIRST holder runs with stdin closed: its lock fd is 0, the
           descriptor the original fix's `> 0` guard would have skipped --
           that regression frees this slot at exec and fails step 2.  */
        holders[nheld] = spawn_holder(pipefd[1], i == 0);
        if (holders[nheld] < 0) {
            perror("fork");
            return 2;
        }
        ++nheld;
    }
    check(wait_bytes(pipefd[0], nheld, 30), "every holder acquired a slot (incl. one on fd 0)");

    /* All slots are now held by processes that have EXEC'D.  A further
       acquisition must block.  Run it as a child so a hang cannot wedge
       the suite: silence for the window IS the pass.  */
    pid_t waiter = fork();
    if (waiter == 0) {
        if (!dcc_lock_host()) {
            _exit(3);
        }
        if (write(pipefd[1], "W", 1) != 1) {
            _exit(5);
        }
        _exit(0);
    }
    check(!wait_bytes(pipefd[0], 1, 3),
          "with every slot held by an exec'd process, the next acquisition BLOCKS"
          " (a byte here means a lock died at exec)");

    /* Free slots until the waiter completes.  dcc_lock_host blocks on ONE
       pid-derived slot, and there is no way to know from outside which
       holder owns it -- killing a single fixed holder unblocked the waiter
       only when the pids happened to line up (a 1-in-ncpus flake in the
       first version of this gate).  Killing holders one at a time still
       proves the property that matters: an exec'd holder's exit -- and
       nothing less -- is what releases its slot.  The fd-0 holder dies
       first, so ITS slot is demonstrably released by ITS exit like any
       other.  */
    bool unblocked = false;
    for (int i = 0; i < nheld && !unblocked; ++i) {
        kill(holders[i], SIGKILL);
        waitpid(holders[i], nullptr, 0);
        unblocked = wait_bytes(pipefd[0], 1, 2);
    }
    check(unblocked,
          "killing exec'd holders unblocks the waiter (exit, and nothing less,"
          " releases a slot)");
    waitpid(waiter, nullptr, 0);

    for (int i = 0; i < nheld; ++i) {
        kill(holders[i], SIGKILL);
        waitpid(holders[i], nullptr, 0);
    }

    if (failures) {
        printf("RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
