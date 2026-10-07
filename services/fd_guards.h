/* fd_guards.h -- keep the standard descriptors open and sane.
 *
 * iceccd can start (and fork job children) with fds 1/2 closed -- a container
 * start or a supervisor may close its own std fds before exec, and every child
 * inherits that state.  That state is poison for workit's pipe sequence:
 * pipe() allocates the LOWEST free descriptors, so with 1/2 closed
 * pipe(sock_err) lands exactly on (1, 2) and the child-side
 * close(sock_err[0]) then closes the stdout that dup2(sock_out[1], 1) just
 * installed, dup2(2, 2) is a self-no-op, and close(sock_err[1]) closes stderr
 * outright -- the compiler is exec'd with 1/2 closed (and in a chroot without
 * /dev/null, clang's FixupStandardFileDescriptors then dies instantly).
 *
 * sanitize_standard_fds() re-opens /dev/null onto any CLOSED standard fd
 * (O_RDWR; 0/1/2 all get it) and is POSIX-close: it must run BEFORE any
 * chroot/chdir that hides the host fs.  It is idempotent and cheap (three
 * fcntl checks and at most two opens), so it costs nothing on a healthy start.
 *
 * standard_fds_open() (the observation form used by tests) reports whether
 * 0/1/2 are all open WITHOUT changing anything.
 */
#ifndef ICECC_FD_GUARDS_H
#define ICECC_FD_GUARDS_H

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

namespace icecc {

/* Re-open /dev/null onto any closed fd among 0/1/2.  Returns true when all
   three descriptors are open afterwards; false only when /dev/null itself
   cannot be opened (in which case nothing was changed on 0/1/2 -- the caller
   decides whether that is fatal).  Never touches an ALREADY-OPEN std fd:
   a daemon whose stdout is a real pipe keeps its real pipe. */
inline bool sanitize_standard_fds() noexcept
{
    int null_fd = -1;
    for (int fd = 0; fd <= 2; ++fd) {
        if (::fcntl(fd, F_GETFD) >= 0)
            continue;
        if (null_fd < 0) {
            /* NO O_CLOEXEC: when this open lands ON a std slot (fd 1 or 2
               closed -> lowest free), the descriptor becomes the std fd
               itself and must stay open across EVERY exec (sidecar, create-
               env, helpers), not just the compiler path.  dup2 would clear
               the flag on a copy, but the null_fd == fd case skips the dup2
               and would leave CLOEXEC set -- the same squat class one exec
               later.  A helper fd above 2 (mixed closed sets) is closed
               explicitly below. */
            null_fd = ::open("/dev/null", O_RDWR);
            if (null_fd < 0)
                return false;
        }
        /* dup2(x, x) is the documented no-op -- skip it when null_fd IS the
           target number (already handled by the open landing there). */
        if (null_fd != fd && ::dup2(null_fd, fd) < 0)
            return false;
    }
    if (null_fd >= 0 && null_fd > 2)
        (void)::close(null_fd);
    for (int fd = 0; fd <= 2; ++fd) {
        if (::fcntl(fd, F_GETFD) < 0)
            return false;
        /* The std fd must survive exec: clear any inherited CLOEXEC (an
           already-open std fd could legitimately carry it; a reopened one
           does not since the open is bare). */
        const int flags = ::fcntl(fd, F_GETFD);
        if ((flags & FD_CLOEXEC) && ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) < 0)
            return false;
    }
    return true;
}

/* Test/observation form: are 0/1/2 all open right now? */
inline bool standard_fds_open() noexcept
{
    return ::fcntl(0, F_GETFD) >= 0 && ::fcntl(1, F_GETFD) >= 0 &&
           ::fcntl(2, F_GETFD) >= 0;
}

/* The classic child-side std-fd installation (workit's exact sequence):
   close the read end of the out pipe and dup2 its write end onto stdout,
   same for err onto stderr and in onto stdin, closing each original after
   its dup2 -- then close the socketpair's child end's sibling and mark the
   kept end CLOEXEC, and close both death-pipe ends.  EVERY step runs
   whatever the previous one returned (the pre-factoring inline sequence
   behaved exactly that way -- an early return would skip main_sock's
   CLOEXEC and the death-pipe closes); the AGGREGATE result is false iff
   any dup2 or the CLOEXEC fcntl failed.  Each end MUST be off the std
   slots already (call move_pipe_ends_off_standard_fds first) or the
   sequence self-clobbers -- that is the bug this pair exists to prevent. */
inline bool install_pipe_fds_as_standard(int sock_out[2], int sock_err[2],
                                         int sock_in[2], int main_sock[2],
                                         int death_pipe[2]) noexcept
{
    bool ok = true;
    if ((-1 == close(sock_out[0])) && (errno != EBADF)) { /* best-effort */ }
    if (-1 == dup2(sock_out[1], STDOUT_FILENO))
        ok = false;
    if ((-1 == close(sock_out[1])) && (errno != EBADF)) { /* best-effort */ }

    if ((-1 == close(sock_err[0])) && (errno != EBADF)) { /* best-effort */ }
    if (-1 == dup2(sock_err[1], STDERR_FILENO))
        ok = false;
    if ((-1 == close(sock_err[1])) && (errno != EBADF)) { /* best-effort */ }

    if ((-1 == close(sock_in[1])) && (errno != EBADF)) { /* best-effort */ }
    if (-1 == dup2(sock_in[0], STDIN_FILENO))
        ok = false;
    if ((-1 == close(sock_in[0])) && (errno != EBADF)) { /* best-effort */ }

    if ((-1 == close(main_sock[0])) && (errno != EBADF)) { /* best-effort */ }
    if (-1 == fcntl(main_sock[1], F_SETFD, FD_CLOEXEC))
        ok = false;
    if ((-1 == close(death_pipe[0])) && (errno != EBADF)) { /* best-effort */ }
    if ((-1 == close(death_pipe[1])) && (errno != EBADF)) { /* best-effort */ }
    return ok;
}

/* Squat guard for pipe pairs.  pipe() allocates the LOWEST free descriptors,
   so a process forked (or started) with a closed std fd gets pipe ends
   landing exactly on 1/2 -- and the classic child-side setup sequence
   (close(read_end); dup2(write_end, STDOUT); close(write_end); ... for each
   of out/err/in) then closes the stdout it just installed, dup2(2, 2) is a
   self-no-op, and close(sock_err[1]) closes stderr outright: the exec'd
   compiler ends up with 1/2 closed and, in a chroot without /dev/null,
   clang's FixupStandardFileDescriptors dies instantly.  Move every pipe end
   that squatted a std slot to a descriptor >= 3 (F_DUPFD: same open-file
   description, CLOEXEC not set -- the child manages its own) BEFORE the
   child-side dup2/close sequence runs, making it squat-proof regardless of
   the fd state we inherited.  Returns true when no end is left on a std fd.

   fd_list is a null-terminated array of pointers to each end (an end may
   already be >= 3; ends are rewritten in place).  sanitize_standard_fds()
   keeps 0/1/2 open in the first place; this is the belt to that braces. */
inline bool move_pipe_ends_off_standard_fds(int **fd_list) noexcept
{
    for (int **p = fd_list; p && *p; ++p) {
        int fd = **p;
        if (fd >= 0 && fd < 3) {
            int moved = ::fcntl(fd, F_DUPFD, 3);
            if (moved < 0)
                return false;
            (void)::close(fd);
            **p = moved;
        }
    }
    return true;
}

}  // namespace icecc

#endif
