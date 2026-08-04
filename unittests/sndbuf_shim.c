/*
    LD_PRELOAD shim for the scheduler backpressure integration test
    (schedbp.cpp).  Two independent, env-gated behaviours:

    ICECC_TEST_SNDBUF=<bytes>
        Shrinks the send buffer of every socket the preloaded process
        accept()s, so that a peer which stops reading jams the connection
        after a few kilobytes instead of after several hundred kilobytes of
        kernel buffering.  Setting SO_SNDBUF explicitly also disables the
        kernel's send-buffer autotuning, keeping the jam point deterministic.

    ICECC_TEST_STRIP_USER_TIMEOUT=1
        Makes setsockopt(IPPROTO_TCP, TCP_USER_TIMEOUT) a no-op.  MsgChannel
        arms a 9s TCP_USER_TIMEOUT on every channel; with it in place the
        kernel declares a completely stalled (zero-window) peer dead after
        ~9s, which pre-empts the 30s application-level send timeout in
        flush_writebuf().  Builds without TCP_USER_TIMEOUT (e.g. icecream
        1.4.90 as deployed in the issue report) do reach the 30s timeout;
        stripping the option lets the test reproduce that exact scenario.
*/

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>

static int env_int(const char *name)
{
    const char *v = getenv(name);
    return (v && *v) ? atoi(v) : 0;
}

static int shrink(int fd)
{
    /* Preserve errno for the caller: accept() may have failed, and the
       scheduler branches on EINTR/EAGAIN/EMFILE from the interposed call.
       getenv/atoi/setsockopt must not be allowed to clobber it. */
    int saved_errno = errno;
    if (fd >= 0) {
        int bytes = env_int("ICECC_TEST_SNDBUF");
        if (bytes > 0 && setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                                    &bytes, sizeof(bytes)) != 0) {
            static int warned;
            if (!warned) {
                warned = 1;
                fprintf(stderr, "sndbuf_shim: SO_SNDBUF shrink failed; "
                        "backpressure scenario will not engage\n");
            }
        }
    }
    errno = saved_errno;
    return fd;
}

static void *must_dlsym(const char *name)
{
    void *sym = dlsym(RTLD_NEXT, name);
    if (!sym) {
        fprintf(stderr, "sndbuf_shim: dlsym(%s) failed: %s\n", name, dlerror());
        abort();
    }
    return sym;
}

int accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    static int (*real_accept)(int, struct sockaddr *, socklen_t *);
    if (!real_accept)
        real_accept = must_dlsym("accept");
    return shrink(real_accept(fd, addr, len));
}

int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    static int (*real_accept4)(int, struct sockaddr *, socklen_t *, int);
    if (!real_accept4)
        real_accept4 = must_dlsym("accept4");
    return shrink(real_accept4(fd, addr, len, flags));
}

int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen)
{
    static int (*real_setsockopt)(int, int, int, const void *, socklen_t);
    if (!real_setsockopt)
        real_setsockopt = must_dlsym("setsockopt");
#ifdef TCP_USER_TIMEOUT
    if (level == IPPROTO_TCP && optname == TCP_USER_TIMEOUT
            && env_int("ICECC_TEST_STRIP_USER_TIMEOUT"))
        return 0;
#endif
    return real_setsockopt(fd, level, optname, optval, optlen);
}
