/*
    LD_PRELOAD shim for the scheduler backpressure integration test
    (schedbp.cpp).  Two independent, env-gated behaviours:

    ICECC_TEST_SNDBUF=<bytes>
        Shrinks the send buffer of every socket the preloaded process
        accept()s, so that a peer which stops reading jams the connection
        after a few kilobytes instead of after several hundred kilobytes of
        kernel buffering.  Setting SO_SNDBUF explicitly also disables the
        kernel's send-buffer autotuning, keeping the jam point deterministic.

    ICECC_TEST_RCVBUF=<bytes>
        Shrinks SO_RCVBUF on every socket the preloaded process creates,
        BEFORE it connects (so the TCP window is negotiated small).  Preload
        into a daemon to make its scheduler connection jam realistically:
        dispatch replies are ~60 bytes, so with default receive buffers a
        backlog of thousands still fits and backpressure never engages.

    ICECC_TEST_STRIP_USER_TIMEOUT=1
        Makes setsockopt(IPPROTO_TCP, TCP_USER_TIMEOUT) a no-op.  MsgChannel
        arms a 9s TCP_USER_TIMEOUT on every TCP channel (a no-op on AF_UNIX);
        with it in place the kernel declares a completely stalled
        (zero-window, zero-progress) peer dead after ~9s, pre-empting the
        30s application-level send timeout in flush_writebuf().  The
        production failure is a SLOWLY-draining peer, whose ACKs keep
        resetting the kernel timer so the application timeout governs; the
        harness's full-stop stand-in would otherwise be killed by the kernel
        first, so the option is stripped to reach the same application path.
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

int socket(int domain, int type, int protocol)
{
    static int (*real_socket)(int, int, int);
    if (!real_socket)
        real_socket = must_dlsym("socket");
    int fd = real_socket(domain, type, protocol);
    int saved_errno = errno;
    if (fd >= 0) {
        int bytes = env_int("ICECC_TEST_RCVBUF");
        if (bytes > 0)
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
    }
    errno = saved_errno;
    return fd;
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
