/*
    ICECC_LOCAL_LOCK_QUEUE: the local preprocessor slot wait is
    work-conserving.

    The historical wait blocks on the slot getpid() % slots, so a waiter can
    sleep on a busy slot while another slot is free.  A build tool spawns
    its wrappers with a fixed pid stride, which piles the waiters onto a
    fraction of the slots (measured on a 24-CPU client: ~125 waiters on
    only 12-14 slots).  In queued mode a waiter never commits to one slot:
    the head of the pool's queue lock rescans every slot until one frees.

    Against a PRIVATE four-slot pool in a temporary directory:
      1. four holders take every slot and report which one they hold;
      2. a legacy waiter (env unset) blocks on its pid-derived slot, and a
         queued waiter (env set) waits on the queue;
      3. releasing one holder whose slot is NOT the legacy waiter's must
         hand that slot to the queued waiter promptly, while the legacy
         waiter stays blocked: it is still waiting on a held slot;
      4. releasing the remaining holders lets the legacy waiter finish.
*/

#include "../client/util.h"

#include <sys/types.h>
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
#include <vector>

static int failures = 0;

static void check(bool ok, const char *what)
{
    printf(ok ? "ok       - %s\n" : "FAILED   - %s\n", what);
    if (!ok) {
        ++failures;
    }
    fflush(stdout);
}

static std::string held_slot_name()
{
    char path[64], target[4096];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", dcc_locked_fd());
    const ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n <= 0) {
        return "";
    }
    target[n] = '\0';
    const char *base = strrchr(target, '/');
    return base ? base + 1 : target;
}

static std::string slot_name(int slot)
{
    return slot == 0 ? std::string("local_lock") : "local_lock" + std::to_string(slot);
}

/* Read one line from fd within timeout_ms; "" on timeout or EOF.  */
static std::string read_line(int fd, int timeout_ms)
{
    std::string line;
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        const int r = select(fd + 1, &set, nullptr, nullptr, &tv);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return "";
        }
        char c;
        if (read(fd, &c, 1) != 1) {
            return "";
        }
        if (c == '\n') {
            return line;
        }
        line += c;
    }
}

static pid_t spawn(bool queued, const char *dir, int report_fd, int release_fd)
{
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    if (queued) {
        setenv("ICECC_LOCAL_LOCK_QUEUE", "1", 1);
    } else {
        unsetenv("ICECC_LOCAL_LOCK_QUEUE");
    }
    // Forked children inherit every pipe end still open in the parent at
    // fork time: a later holder keeps the write end of an earlier holder's
    // release pipe, so closing that pipe in the parent never EOFs it and
    // the earlier holder never exits.  Holders must be released by their
    // OWN write end only: close every inherited fd that is not ours.
    for (int fd = 3; fd < 32; ++fd) {
        if (fd != report_fd && fd != release_fd) {
            close(fd);
        }
    }
    if (!dcc_lock_host_at(dir, 4)) {
        _exit(3);
    }
    const std::string line = held_slot_name() + "\n";
    if (write(report_fd, line.data(), line.size()) != (ssize_t)line.size()) {
        _exit(5);
    }
    if (release_fd >= 0) {
        char c;
        while (read(release_fd, &c, 1) < 0 && errno == EINTR) {
        }
    }
    _exit(0);
}

int main()
{
    char tmpl[] = "/tmp/icecc-lockqueue-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (dir == nullptr) {
        perror("mkdtemp");
        return 2;
    }
    std::vector<pid_t> children;

    // 1. four holders, each with its own release pipe
    int holder_report[2];
    if (pipe(holder_report) != 0) {
        perror("pipe");
        return 2;
    }
    std::vector<std::string> held(4);
    std::vector<int> release(4, -1);
    for (int i = 0; i < 4; ++i) {
        int rel[2];
        if (pipe(rel) != 0) {
            perror("pipe");
            return 2;
        }
        const pid_t pid = spawn(i % 2 == 0, dir, holder_report[1], rel[0]);
        close(rel[0]);
        release[i] = rel[1];
        children.push_back(pid);
        held[i] = read_line(holder_report[0], 2000);
    }
    bool distinct = true;
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            distinct = distinct && !held[i].empty() && held[i] != held[j];
        }
    }
    check(distinct, "four holders own four distinct slots");

    // 2. a legacy waiter and a queued waiter
    int legacy_report[2], queued_report[2];
    if (pipe(legacy_report) != 0 || pipe(queued_report) != 0) {
        perror("pipe");
        return 2;
    }
    const pid_t legacy = spawn(false, dir, legacy_report[1], -1);
    children.push_back(legacy);
    const std::string legacy_slot = slot_name(legacy % 4);
    const pid_t queued = spawn(true, dir, queued_report[1], -1);
    children.push_back(queued);
    check(read_line(legacy_report[0], 300).empty() &&
          read_line(queued_report[0], 300).empty(),
          "both waiters block while every slot is held");

    // 3. free a slot the legacy waiter is NOT sleeping on
    int victim = -1;
    for (int i = 0; i < 4; ++i) {
        if (held[i] != legacy_slot) {
            victim = i;
            break;
        }
    }
    check(victim >= 0, "a holder outside the legacy waiter's slot exists");
    if (victim >= 0) {
        close(release[victim]);
        release[victim] = -1;
        const std::string got = read_line(queued_report[0], 1000);
        check(got == held[victim], "queued waiter takes the freed slot within 1 s");
        check(read_line(legacy_report[0], 300).empty(),
              "legacy waiter stays blocked on its own (still held) slot");
    }

    // 4. release everything: the legacy waiter must then finish
    for (int i = 0; i < 4; ++i) {
        if (release[i] >= 0) {
            close(release[i]);
            release[i] = -1;
        }
    }
    check(read_line(legacy_report[0], 2000) == legacy_slot,
          "legacy waiter finishes on its pid-derived slot once released");

    for (pid_t pid : children) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }
    for (int slot = 0; slot < 4; ++slot) {
        unlink((std::string(dir) + "/" + slot_name(slot)).c_str());
    }
    unlink((std::string(dir) + "/local_lock_queue").c_str());
    rmdir(dir);
    printf("%s\n", failures ? "lockqueue: FAILED" : "lockqueue: PASS");
    return failures ? 1 : 0;
}
