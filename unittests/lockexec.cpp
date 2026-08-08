/*
    The daemonless local build bounds concurrency with one fcntl record
    lock per online CPU (dcc_lock_host), and execs the compiler WITHOUT
    forking.  An fcntl record lock survives exec only while its descriptor
    stays open -- and the lock fd is opened close-on-exec, so before
    dcc_lock_keep_across_exec() the slot was released the moment the
    compiler started: a farm-and-daemon outage ran one compiler per JOB
    instead of one per CPU (measured 32 simultaneous compilers on a
    16-CPU host).

    This gate tests the kernel lifetime invariant directly, with no
    compilers and no process-name sampling, against a PRIVATE two-slot
    pool (dcc_lock_host_at) in a temporary directory -- it can neither
    throttle nor be perturbed by real local builds in the shared per-user
    pool, and it runs in constant time on any machine.

    Each holder mirrors the production sequence exactly: acquire a slot,
    dcc_lock_keep_across_exec(), then EXEC -- a re-exec of this very
    binary in --held mode, whose first act IN THE POST-EXEC IMAGE is to
    report the inherited lock fd and block.  Readiness is therefore only
    signalled after the exec the lock must survive.  One holder runs with
    stdin closed, so its lock rides descriptor 0 -- the valid fd an
    earlier version of the fix skipped -- and its report proves fd 0
    really is the lock (asserted on the reported number, not inferred).

      1. two holders take both slots and exec;
      2. a third acquisition (--waiter) must BLOCK: if either slot's lock
         died at exec, it completes and the gate fails;
      3. killing the exec'd holders unblocks the waiter: exit, and
         nothing less, releases a slot.  (The waiter blocks on a
         pid-derived slot, so a single targeted kill cannot be asserted;
         holders are released one at a time and the property claimed is
         release-on-exit, nothing stronger.)
*/

#include "../client/util.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <string>

static int failures = 0;

static void check(bool ok, const char *what)
{
    printf(ok ? "ok       - %s\n" : "FAILED   - %s\n", what);
    if (!ok) {
        ++failures;
    }
    fflush(stdout);
}

/* ---- child modes (post-fork / post-exec images) ----------------------- */

/* --held <notify_fd> <lock_fd>: we are PAST the exec.  Report the lock fd
   the pre-exec code held and block forever.  */
static int held_main(int notify_fd, int lock_fd)
{
    char buf[16];
    const int n = snprintf(buf, sizeof(buf), "H%d\n", lock_fd);
    if (write(notify_fd, buf, n) != n) {
        _exit(5);
    }
    for (;;) {
        pause();
    }
}

/* --holder <notify_fd> <dir> <slots> <close_stdin>: acquire exactly as the
   production wrapper does, then exec ourselves into --held.  */
static int holder_main(const char *self, int notify_fd, const char *dir,
                       int slots, bool close_stdin)
{
    if (close_stdin) {
        close(0);   /* the next open() -- the lock file -- returns fd 0 */
    }
    if (!dcc_lock_host_at(dir, slots)) {
        _exit(3);
    }
    if (!dcc_lock_keep_across_exec()) {
        _exit(4);
    }
    char fdbuf[16], lockbuf[16];
    snprintf(fdbuf, sizeof(fdbuf), "%d", notify_fd);
    snprintf(lockbuf, sizeof(lockbuf), "%d", dcc_locked_fd());
    execl(self, self, "--held", fdbuf, lockbuf, (char *)nullptr);
    _exit(6);
}

/* --waiter <notify_fd> <dir> <slots>: block acquiring, then report.  */
static int waiter_main(int notify_fd, const char *dir, int slots)
{
    if (!dcc_lock_host_at(dir, slots)) {
        _exit(3);
    }
    if (write(notify_fd, "W\n", 2) != 2) {
        _exit(5);
    }
    _exit(0);
}

/* ---- parent ------------------------------------------------------------ */

/* Normal ownership state...  */
static pid_t pids[3] = { -1, -1, -1 };
/* ...and the handler's view of it: volatile sig_atomic_t is the only
   object type with defined semantics for asynchronous handler reads.  The
   two are kept synchronized ONLY inside masked transitions -- the mask
   prevents a half-done transition from being observed at all, and the
   sig_atomic_t mirror makes the read itself well-defined.  Zero means "no
   child"; pids fit in sig_atomic_t (guaranteed >= int range on POSIX).  */
static volatile sig_atomic_t sig_pids[3] = { 0, 0, 0 };
static std::string tempdir;

/* Every ownership transition -- fork-to-publish and reap-to-clear --
   happens with SIGINT/SIGTERM blocked, so the handler can never observe a
   half-updated slot: no missed child (signal between fork and store) and
   no stale kill (signal between reap and clear).  A failed mask change
   must NOT proceed under the pretense of protection.  */
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

static void unblock_handled(const sigset_t *old)
{
    if (sigprocmask(SIG_SETMASK, old, nullptr) != 0) {
        perror("sigprocmask");
    }
}

static void publish(int slot, pid_t pid)
{
    pids[slot] = pid;
    sig_pids[slot] = pid > 0 ? (sig_atomic_t)pid : 0;
}


/* Reap one child, tolerating EINTR and reporting ECHILD honestly; only a
   CONFIRMED reap (or the kernel saying the child does not exist) marks the
   slot free.  */
static bool reap(pid_t pid)
{
    for (;;) {
        const pid_t r = waitpid(pid, nullptr, 0);
        if (r == pid) {
            return true;
        }
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r < 0 && errno == ECHILD) {
            return true;    // not ours any more; nothing left to reap
        }
        perror("waitpid");
        return false;
    }
}

static void cleanup(void)
{
    sigset_t old;
    const bool masked = block_handled(&old);
    for (int i = 0; i < 3; ++i) {
        if (pids[i] > 0) {
            if (kill(pids[i], SIGKILL) != 0 && errno != ESRCH) {
                perror("kill");
            }
            if (reap(pids[i])) {
                publish(i, -1);
            }
        }
    }
    if (masked) {
        unblock_handled(&old);
    }
    if (!tempdir.empty()) {
        const std::string base = tempdir + "/local_lock";
        unlink(base.c_str());
        unlink((base + "1").c_str());
        rmdir(tempdir.c_str());
        tempdir.clear();
    }
}

/* Async-signal-safe by construction: kill(2) on known-positive pids and
   _exit(2) only.  The full cleanup routine allocates and must never run
   from a handler; the temporary directory is abandoned on this path (the
   kernel reclaims the children, which is the part that must not leak).  */
static void on_signal(int)
{
    for (int i = 0; i < 3; ++i) {
        const sig_atomic_t p = sig_pids[i];
        if (p > 0) {
            kill((pid_t)p, SIGKILL);
        }
    }
    _exit(2);
}


/* One newline-terminated report, or empty on timeout.  */
static std::string read_report(int fd, int timeout_sec)
{
    std::string line;
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv = { timeout_sec, 0 };
        if (select(fd + 1, &rd, nullptr, nullptr, &tv) <= 0) {
            return std::string();
        }
        char c;
        if (read(fd, &c, 1) != 1) {
            return std::string();
        }
        if (c == '\n') {
            return line;
        }
        line += c;
    }
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "--held") == 0) {
        return held_main(atoi(argv[2]), atoi(argv[3]));
    }
    if (argc >= 6 && strcmp(argv[1], "--holder") == 0) {
        return holder_main(argv[0], atoi(argv[2]), argv[3], atoi(argv[4]),
                           atoi(argv[5]) != 0);
    }
    if (argc >= 5 && strcmp(argv[1], "--waiter") == 0) {
        return waiter_main(atoi(argv[2]), argv[3], atoi(argv[4]));
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    char tmpl[] = "/tmp/lockexecXXXXXX";
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        return 2;
    }
    tempdir = tmpl;
    const int SLOTS = 2;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        cleanup();
        return 2;
    }

    char fdbuf[16], slotbuf[16];
    snprintf(fdbuf, sizeof(fdbuf), "%d", pipefd[1]);
    snprintf(slotbuf, sizeof(slotbuf), "%d", SLOTS);

    auto spawn = [&](int slot, const char *mode, const char *extra) -> pid_t {
        sigset_t old;
        if (!block_handled(&old)) {
            return -1;    // unprotected transition: refuse to fork at all
        }
        const pid_t pid = fork();
        if (pid != 0) {
            if (pid > 0) {
                publish(slot, pid);    // both views, under the mask
            }
            unblock_handled(&old);
            return pid;
        }
        unblock_handled(&old);
        if (extra) {
            execl(argv[0], argv[0], mode, fdbuf, tempdir.c_str(), slotbuf,
                  extra, (char *)nullptr);
        } else {
            execl(argv[0], argv[0], mode, fdbuf, tempdir.c_str(), slotbuf,
                  (char *)nullptr);
        }
        _exit(7);
    };

    /* 1. two holders; the first with stdin closed.  Reports arrive from
       the post-exec image and carry the lock fd.  */
    spawn(0, "--holder", "1");
    spawn(1, "--holder", "0");
    if (pids[0] < 0 || pids[1] < 0) {
        perror("fork");
        cleanup();
        return 2;
    }
    bool saw_fd0 = false;
    int reports = 0;
    for (int i = 0; i < 2; ++i) {
        const std::string r = read_report(pipefd[0], 30);
        if (r.size() >= 2 && r[0] == 'H') {
            ++reports;
            if (atoi(r.c_str() + 1) == 0) {
                saw_fd0 = true;
            }
        }
    }
    check(reports == 2, "both holders reported from their post-exec image");
    check(saw_fd0, "the stdin-closed holder's lock really rides descriptor 0");

    /* 2. both slots held by exec'd processes: a further acquisition must
       block.  */
    spawn(2, "--waiter", nullptr);
    if (pids[2] < 0) {
        perror("fork");
        cleanup();
        return 2;
    }
    check(read_report(pipefd[0], 3).empty(),
          "with both slots held by exec'd processes, the next acquisition BLOCKS"
          " (a report here means a lock died at exec)");

    /* 3. release one holder at a time until the waiter frees.  */
    bool unblocked = false;
    for (int i = 0; i < 2 && !unblocked; ++i) {
        if (pids[i] > 0) {
            sigset_t old;
            const bool masked = block_handled(&old);
            if (kill(pids[i], SIGKILL) != 0 && errno != ESRCH) {
                perror("kill");
            }
            if (reap(pids[i])) {
                publish(i, -1);
            }
            if (masked) {
                unblock_handled(&old);
            }
        }
        unblocked = read_report(pipefd[0], 5) == "W";
    }
    check(unblocked, "killing exec'd holders unblocks the waiter (exit, and"
                     " nothing less, releases a slot)");
    {
        sigset_t old;
        const bool masked = block_handled(&old);
        if (pids[2] > 0 && reap(pids[2])) {
            publish(2, -1);
        }
        if (masked) {
            unblock_handled(&old);
        }
    }

    cleanup();
    if (failures) {
        printf("RESULT: FAIL (%d)\n", failures);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
