/*
 * Test-only preload gate for the cache-service READY publication.
 *
 * The test gives the child a gate descriptor and the READY write descriptor.
 * This shim pauses just before the first exact READY write.  The parent can
 * then observe the bound listener, close the READY reader, and release the
 * write.  No production binary is linked with this file.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef ssize_t (*write_function)(int, const void *, size_t);

static int parse_fd(const char *name)
{
    const char *value = getenv(name);
    if (value == NULL || *value == '\0')
        return -1;
    char *end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 0x7fffffffL)
        return -1;
    return (int)parsed;
}

static int write_one(write_function real_write, int fd, char byte)
{
    for (;;) {
        const ssize_t result = real_write(fd, &byte, 1);
        if (result == 1)
            return 1;
        if (result < 0 && errno == EINTR)
            continue;
        return 0;
    }
}

ssize_t write(int fd, const void *buffer, size_t count)
{
    static write_function real_write;
    static int ready_fd = -2;
    static int gate_fd = -2;
    static int acknowledgement_fd = -2;
    static int gate_waited;
    if (real_write == NULL) {
        *(void **)(&real_write) = dlsym(RTLD_NEXT, "write");
        if (real_write == NULL) {
            errno = ENOSYS;
            return -1;
        }
    }
    if (ready_fd == -2) {
        ready_fd = parse_fd("ICECC_TEST_READY_WRITE_FD");
        gate_fd = parse_fd("ICECC_TEST_READY_GATE_FD");
        acknowledgement_fd = parse_fd("ICECC_TEST_READY_ACK_FD");
    }
    if (!gate_waited && fd == ready_fd && gate_fd >= 0 && count == 6 &&
        memcmp(buffer, "READY\n", 6) == 0) {
        gate_waited = 1;
        if (acknowledgement_fd >= 0 && !write_one(real_write, acknowledgement_fd, 1))
            return -1;
        char token = 0;
        for (;;) {
            const ssize_t result = read(gate_fd, &token, 1);
            if (result == 1 || (result < 0 && errno != EINTR))
                break;
        }
    }
    return real_write(fd, buffer, count);
}
