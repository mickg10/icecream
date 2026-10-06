/* stdfdworkit -- farm-free workit-level regression for the remote-compile
 * fd bug (virtu-fdfix).
 *
 * workit is forked from the daemon per job and runs this child-side sequence
 * (workit.cpp:410-457, verbatim shape): close(sock_out[0]);
 * dup2(sock_out[1], STDOUT_FILENO); close(sock_out[1]);
 * close(sock_err[0]); dup2(sock_err[1], STDERR_FILENO); close(sock_err[1]);
 * close(sock_in[1]); dup2(sock_in[0], STDIN_FILENO); close(sock_in[0]);
 * close(main_sock[0]); fcntl(main_sock[1], F_SETFD, FD_CLOEXEC);
 * close(death_pipe[0]); close(death_pipe[1]); ... execv(compiler).
 *
 * When the process is forked with fds 1/2 CLOSED, pipe() allocates the
 * LOWEST free descriptors: pipe(sock_err) lands exactly on (1, 2), and the
 * dance then closes the stdout it just installed (close(sock_err[0]) ==
 * close(1) in the squatted layout), dup2(2, 2) is a self-no-op, and
 * close(sock_err[1]) kills stderr outright -- the compiler is exec'd with
 * 1/2 closed (in a chroot without /dev/null, clang's
 * FixupStandardFileDescriptors exits(1) instantly).
 *
 * This test reproduces the EXACT sequence WITHOUT a farm: five pipe pairs
 * created with 1/2 closed, the child-side dance, and an exec of a helper
 * that reports the open-ness of its OWN 0/1/2 over a REPORT pipe created
 * before anything is closed (so it lives at >= 3 in both legs and the
 * dance never touches it).  The same source compiles at c8bb615f
 * (__has_include): fd_guards.h does not exist there, the relocation call
 * compiles out, and the legacy sequence runs -- RED.
 *
 * RED  at c8bb615f: the squatted dance execs the helper with 1/2 closed ->
 *                   HELPER reports CLOSED -> FAIL (loudly, on the saved
 *                   copy of the real stderr).
 * GREEN at virtu-fdfix: move_pipe_ends_off_standard_fds() moves every pipe
 *                   end >= 3 before the dance; the helper's 0/1/2 are the
 *                   real in/out/err pipe ends, all open -> OPEN -> PASS.
 */
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "exitcode.h"

#if __has_include("../services/fd_guards.h")
#include "../services/fd_guards.h"
#define HAVE_FD_GUARDS 1
#else
#define HAVE_FD_GUARDS 0
#endif

/* The report fd the exec'd helper writes its verdict to. */
#define REPORT_FD 4

static int real_stderr = -1;

static int fail(const char *why)
{
    /* This process closed its OWN 1/2 to model the buggy inheritance; the
       verdict still has to reach the harness. */
    dprintf(real_stderr, "stdfdworkit: FAIL: %s\n", why);
    return 1;
}

/* The sanitize CLOEXEC regression: a std fd reopened by
 * sanitize_standard_fds() must NOT carry FD_CLOEXEC, or one exec later
 * (sidecar, create-env, any helper that does not re-dup2 the slot) it is
 * closed again -- the same squat class.  The reopened slot is exercised
 * through a REAL exec: fork with each closed-set variant, sanitize, exec
 * a helper that reports (over a report pipe created BEFORE the closes, so
 * it lives >= 3) whether its own 0/1/2 are open.
 *
 * RED for the pre-fix sanitize (open with O_CLOEXEC landing on the slot:
 * the {1,2} and {0,1,2} variants exec the helper with the reopened fds
 * CLOEXEC-set -> closed at exec); GREEN after. */
static int sanitize_cloexec_test()
{
    static const int closed_sets[][3] = {
        {1, 2, -1}, {0, 1, 2}, {2, -1, -1},
    };

    for (size_t variant = 0; variant < sizeof(closed_sets) / sizeof(closed_sets[0]);
         ++variant) {
        int report_pipe[2];
        if (pipe(report_pipe) < 0)
            return fail("pipe(report_pipe) [cloexec variant]");

        pid_t pid = fork();
        if (pid < 0)
            return fail("fork [cloexec variant]");
        if (pid == 0) {
            close(report_pipe[0]);
            for (int i = 0; i < 3 && closed_sets[variant][i] >= 0; ++i)
                close(closed_sets[variant][i]);
#if HAVE_FD_GUARDS
            if (!icecc::sanitize_standard_fds())
                _exit(EXIT_DISTCC_FAILED);
#else
            /* At c8bb615f the sanitize does not exist: exec WITHOUT it, so
               this test is RED there (the {1,2}/{0,1,2} variants exec the
               helper with the std fds actually closed -> CLOSED).  The
               {2}-only variant passes pre-fix by accident (fd 0/1 open at
               exec) -- the CLOEXEC half of the bug needs the fixed code to
               exist at all, which is why the whole leg is compile-gated. */
            for (int fd = 0; fd <= 2; ++fd) {
                if (::fcntl(fd, F_GETFD) < 0) {
                    /* best-effort reopen so the RED failure is the exec
                       verdict, not a bogus early exit path */
                    const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
                    if (null_fd >= 0 && null_fd != fd)
                        (void)!::dup2(null_fd, fd);
                }
            }
#endif
            if (dup2(report_pipe[1], REPORT_FD) < 0)
                _exit(EXIT_DISTCC_FAILED);
            if (report_pipe[1] != REPORT_FD)
                close(report_pipe[1]);
            const char *argv[] = {"/bin/sh", "-c",
                "[ -L /proc/self/fd/0 ] && [ -L /proc/self/fd/1 ] && "
                "[ -L /proc/self/fd/2 ] && printf OK >&4 || printf CLOSED >&4",
                nullptr};
            execv("/bin/sh", (char *const *)argv);
            _exit(127);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) != pid)
            return fail("waitpid [cloexec variant]");
        close(report_pipe[1]);
        if (-1 == fcntl(report_pipe[0], F_SETFL, O_NONBLOCK))
            return fail("fcntl O_NONBLOCK [cloexec variant]");
        char buf[16];
        const ssize_t n = read(report_pipe[0], buf, sizeof(buf) - 1);
        if (n <= 0)
            return fail("no report from the cloexec helper (exec lost the reopened fd)");
        buf[n] = '\0';
        if (strcmp(buf, "OK") != 0)
            return fail("reopened std fd did not survive exec (CLOEXEC left set)");
    }
    return 0;
}

int main()
{
    /* Save the real stderr (dup lands >= 3), then build the report pipe,
     * THEN close 1/2 so the five workit pipes squat the std slots exactly
     * as in the bug (lowest-free allocation). */
    real_stderr = dup(STDERR_FILENO);
    if (real_stderr < 0)
        return 1;

    if (sanitize_cloexec_test() != 0)
        return 1;
    dprintf(real_stderr, "stdfdworkit: OK (sanitize leaves no CLOEXEC on reopened std fds; exec'd helper sees 0/1/2 open)\n");
    if (real_stderr > 2)
        close(real_stderr);
    real_stderr = dup(STDERR_FILENO);

    int report_pipe[2];
    if (pipe(report_pipe) < 0)
        return fail("pipe(report_pipe)");

    int sock_err[2], sock_out[2], sock_in[2], main_sock[2], death_pipe[2];

    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    if (pipe(sock_err) < 0) return fail("pipe(sock_err)");
    if (pipe(sock_out) < 0) return fail("pipe(sock_out)");
    if (pipe(sock_in) < 0) return fail("pipe(sock_in)");
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, main_sock) < 0)
        return fail("socketpair(main_sock)");
    if (pipe(death_pipe) < 0) return fail("pipe(death_pipe)");

#if HAVE_FD_GUARDS
    /* The fix (virtu-fdfix): every pipe end off the std slots BEFORE the
       child-side dance runs.  At c8bb615f (no fd_guards.h) this compiles
       out and the legacy squatted dance runs -- the RED leg. */
    int *fd_list[] = {
        &sock_err[0], &sock_err[1],
        &sock_out[0], &sock_out[1],
        &sock_in[0], &sock_in[1],
        &main_sock[0], &main_sock[1],
        &death_pipe[0], &death_pipe[1],
        nullptr,
    };
    if (!icecc::move_pipe_ends_off_standard_fds(fd_list))
        return fail("move_pipe_ends_off_standard_fds");
#endif

    pid_t pid = fork();
    if (pid < 0)
        return fail("fork");
    if (0 == pid) {
        /* child: close the report pipe's read end, then run the SHIPPED
           sequence -- install_pipe_fds_as_standard is the same function
           workit.cpp calls (fd_guards.h), so the test cannot drift from
           the daemon's wiring.  At c8bb615f (no fd_guards.h) the fallback
           below carries the legacy close/dup2 wiring verbatim. */
        close(report_pipe[0]);

#if HAVE_FD_GUARDS
        if (!icecc::install_pipe_fds_as_standard(sock_out, sock_err, sock_in,
                                                 main_sock, death_pipe))
            _exit(EXIT_DISTCC_FAILED);
#else
        /* Legacy wiring (pre-fdg_guards workit.cpp:410-457): kept in sync
           with the shipped sequence by review; only reached at c8bb615f. */
        if ((-1 == close(sock_out[0])) && (errno != EBADF)) _exit(EXIT_DISTCC_FAILED);
        if (-1 == dup2(sock_out[1], STDOUT_FILENO)) _exit(EXIT_DISTCC_FAILED);
        if ((-1 == close(sock_out[1])) && (errno != EBADF)) _exit(EXIT_DISTCC_FAILED);

        if ((-1 == close(sock_err[0])) && (errno != EBADF)) _exit(EXIT_DISTCC_FAILED);
        if (-1 == dup2(sock_err[1], STDERR_FILENO)) _exit(EXIT_DISTCC_FAILED);
        if ((-1 == close(sock_err[1])) && (errno != EBADF)) _exit(EXIT_DISTCC_FAILED);

        if ((-1 == close(sock_in[1])) && (errno != EBADF)) _exit(EXIT_DISTCC_FAILED);
        if (-1 == dup2(sock_in[0], STDIN_FILENO)) _exit(EXIT_DISTCC_FAILED);
        if ((-1 == close(sock_in[0])) && (errno != EBADF)) _exit(EXIT_DISTCC_FAILED);

        if ((-1 == close(main_sock[0])) && (errno != EBADF)) _exit(EXIT_DISTCC_FAILED);
        if (-1 == fcntl(main_sock[1], F_SETFD, FD_CLOEXEC)) _exit(EXIT_DISTCC_FAILED);
        if ((-1 == close(death_pipe[0])) && (errno != EBADF)) _exit(EXIT_DISTCC_FAILED);
        if ((-1 == close(death_pipe[1])) && (errno != EBADF)) _exit(EXIT_DISTCC_FAILED);
#endif

        /* The report fd: dup2 the report pipe's write end onto REPORT_FD
           (no CLOEXEC, so it survives the exec).  The helper is exec'd
           with the layout the dance installed: 0/1/2 = in/out/err pipes. */
        if (dup2(report_pipe[1], REPORT_FD) < 0) _exit(EXIT_DISTCC_FAILED);
        if (report_pipe[1] != REPORT_FD) close(report_pipe[1]);

        char helper_cmd[256];
        snprintf(helper_cmd, sizeof(helper_cmd),
                 "[ -L /proc/self/fd/0 ] && [ -L /proc/self/fd/1 ] && [ -L /proc/self/fd/2 ]"
                 " && printf OPEN >&%d || printf CLOSED >&%d",
                 REPORT_FD, REPORT_FD);
        const char *argv[] = {"/bin/sh", "-c", helper_cmd, nullptr};
        execv("/bin/sh", (char *const *)argv);
        _exit(127);  /* execv never returns on success */
    }

    /* parent: the child is gone; nonblocking read so an exec failure
       (no report) fails fast instead of hanging. */
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return fail("waitpid");
    close(report_pipe[1]);
    if (-1 == fcntl(report_pipe[0], F_SETFL, O_NONBLOCK))
        return fail("fcntl(report_pipe[0], O_NONBLOCK)");

    char buf[256];
    ssize_t n = read(report_pipe[0], buf, sizeof(buf) - 1);
    if (n < 0)
        return fail("read(report_pipe) failed");
    if (n == 0)
        return fail("no report: the helper never wrote (exec'd with closed fds?)");
    buf[n] = '\0';

    if (strcmp(buf, "OPEN") == 0) {
        dprintf(real_stderr, "stdfdworkit: OK (helper exec'd with 0/1/2 open%s)\n",
                HAVE_FD_GUARDS ? ", relocation active" : ", WITHOUT fd_guards");
        return 0;
    }
    if (strcmp(buf, "CLOSED") == 0) {
        return fail("helper was exec'd with a closed std fd (sequence self-clobber)");
    }
    return fail(buf);
}
