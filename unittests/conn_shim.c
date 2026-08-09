/*
    Failure-injection preload for the connectivity regression.  Each
    interception is armed by an environment variable read at call time, so
    the test arms and disarms scenarios in-process with setenv/unsetenv:

      ICECC_CONN_FAIL_SOCKET=1    socket(AF_INET, SOCK_STREAM) fails EMFILE
      ICECC_CONN_FAIL_FCNTL=1     fcntl(F_SETFL) fails EINVAL
      ICECC_CONN_FAIL_RESOLVE=1   gethostbyname() returns NULL
      ICECC_CONN_SYNC_CONNECT=1   connect() completes synchronously: the
                                  real (nonblocking) connect is driven to
                                  completion with poll and returns 0 on
                                  success -- the deterministic form of the
                                  kernel's occasional immediate loopback
                                  success
      ICECC_CONN_TIME_JUMP=1      time() leaps +7777s per call -- a wall
                                  clock no deadline may depend on
*/
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>

static int armed(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] == '1';
}

int socket(int domain, int type, int protocol)
{
    static int (*real)(int, int, int) = 0;
    if (!real) {
        real = dlsym(RTLD_NEXT, "socket");
    }
    if (armed("ICECC_CONN_FAIL_SOCKET") && domain == AF_INET
        && (type & SOCK_STREAM)) {
        errno = EMFILE;
        return -1;
    }
    return real(domain, type, protocol);
}

int fcntl(int fd, int cmd, ...)
{
    static int (*real)(int, int, ...) = 0;
    if (!real) {
        real = dlsym(RTLD_NEXT, "fcntl");
    }
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    if (armed("ICECC_CONN_FAIL_FCNTL") && cmd == F_SETFL) {
        errno = EINVAL;
        return -1;
    }
    return real(fd, cmd, arg);
}

struct hostent *gethostbyname(const char *name)
{
    static struct hostent *(*real)(const char *) = 0;
    if (!real) {
        real = dlsym(RTLD_NEXT, "gethostbyname");
    }
    if (armed("ICECC_CONN_FAIL_RESOLVE")) {
        return 0;
    }
    return real(name);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    static int (*real)(int, const struct sockaddr *, socklen_t) = 0;
    if (!real) {
        real = dlsym(RTLD_NEXT, "connect");
    }
    const int rc = real(fd, addr, len);
    if (armed("ICECC_CONN_SYNC_CONNECT") && rc < 0
        && (errno == EINPROGRESS || errno == EAGAIN)) {
        struct pollfd pf;
        pf.fd = fd;
        pf.events = POLLOUT;
        pf.revents = 0;
        if (poll(&pf, 1, 3000) > 0) {
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0
                && err == 0) {
                return 0;
            }
            errno = err ? err : ECONNREFUSED;
            return -1;
        }
        errno = ETIMEDOUT;
        return -1;
    }
    return rc;
}

time_t time(time_t *tloc)
{
    static time_t (*real)(time_t *) = 0;
    static long jumps = 0;
    if (!real) {
        real = dlsym(RTLD_NEXT, "time");
    }
    time_t t = real(0);
    if (armed("ICECC_CONN_TIME_JUMP")) {
        t += (time_t)(++jumps * 7777);
    }
    if (tloc) {
        *tloc = t;
    }
    return t;
}
