/*
    The local preprocessor slot wait is work-conserving: a waiter never
    commits to one slot, so a freed slot is picked up by SOME waiter.  The
    historical wait blocked on the single slot getpid() % slots, so a
    waiter could sleep on a busy slot while another sat free (a fixed
    wrapper pid stride piled the waiters onto a fraction of the slots).

    Against a PRIVATE four-slot pool in a temporary directory:
      1. four holders take every slot and report which one they hold;
      2. with every slot held, two waiters both block;
      3. the holder of a slot that is NEITHER waiter's getpid() % slots
         target -- the slot the deleted historical wait would have slept
         on -- is released: some waiter must take that slot within 1 s,
         which the historical wait could not do (both waiters would stay
         asleep on their own still-held slots);
      4. releasing the remaining holders lets the still-queued waiter
         take one of their slots, and closing the waiters' release pipes
         (each waiter holds its slot until then) lets both exit.
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
#include <chrono>
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

/* Read one line from fd within timeout_ms; "" on timeout or EOF.  The
   budget is ONE steady_clock deadline for the whole call: rebuilding the
   full timeout on every iteration would let a signal interrupt stretch
   "within 1 s" without bound.  */
static std::string read_line(int fd, int timeout_ms)
{
    std::string line;
    const auto deadline = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= std::chrono::milliseconds(0)) {
            return "";
        }
        /* Assigned member-wise, not brace-initialised: the rep of a
           chrono duration is long long, and a narrowing brace-init would
           be a build error under -Werror.  */
        struct timeval tv;
        tv.tv_sec = left.count() / 1000;
        tv.tv_usec = (left.count() % 1000) * 1000;
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

/* Read one line from EITHER fd within timeout_ms; "" on timeout.  *from is
   set to the fd that reported, or left at -1 on timeout.  Which waiter wins
   the freed slot is not knowable in advance, so both pipes are watched.
   Once the first byte arrives, the REST of that line is already in the
   pipe: a report is one write() of a short line, well under PIPE_BUF, so
   the kernel hands it over atomically -- reading it must stick to the fd
   the first byte came from and never re-select, or a racing second report
   could interleave into garbage.  The budget is one steady_clock deadline
   for the whole call (same reason as read_line).  */
static std::string read_either_line(int fd1, int fd2, int timeout_ms, int *from)
{
    *from = -1;
    const auto deadline = std::chrono::steady_clock::now()
                         + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd1, &set);
        FD_SET(fd2, &set);
        const int maxfd = (fd1 > fd2 ? fd1 : fd2) + 1;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= std::chrono::milliseconds(0)) {
            return "";
        }
        struct timeval tv;   // member-wise, like read_line: no narrowing
        tv.tv_sec = left.count() / 1000;
        tv.tv_usec = (left.count() % 1000) * 1000;
        const int r = select(maxfd, &set, nullptr, nullptr, &tv);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return "";
        }
        /* Both may be readable on a tie; which report is read first only
           decides the LABEL, the lines themselves cannot mix.  */
        const int fd = FD_ISSET(fd1, &set) ? fd1 : fd2;
        std::string line;
        for (;;) {
            char c;
            if (read(fd, &c, 1) != 1) {
                return "";
            }
            if (c == '\n') {
                *from = fd;
                return line;
            }
            line += c;
        }
    }
}

static pid_t spawn(const char *dir, int report_fd, int release_fd)
{
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    // Forked children inherit every pipe end still open in the parent at
    // fork time: a later child keeps the write end of an earlier child's
    // release pipe, so closing that pipe in the parent never EOFs it and
    // the earlier child never exits.  Holders and waiters must each be
    // released by their OWN write end only: close every inherited fd that
    // is not ours.
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

/* Kill and reap every child so far (the cleanup at the end of main
   reaps again -- waitpid on an already-reaped pid is ECHILD, which it
   tolerates -- but a -1 must never reach it: kill(-1, SIGKILL) would
   signal everything this test can, the make check runner included).  */
static void reap_all(const std::vector<pid_t> &children)
{
    for (pid_t pid : children) {
        if (pid > 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
    }
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
            reap_all(children);
            return 2;
        }
        const pid_t pid = spawn(dir, holder_report[1], rel[0]);
        close(rel[0]);
        release[i] = rel[1];
        if (pid <= 0) {
            // fork failed: never push -1 -- cleanup would kill(-1).
            perror("fork");
            close(release[i]);
            release[i] = -1;
            reap_all(children);
            return 2;
        }
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

    // 2. two waiters, each with its own release pipe so it HOLDS its slot
    //    after reporting (a waiter that exits releases its slot, which
    //    the other waiter would grab and report on too); both must block
    //    while every slot is held.
    int wait_report[2][2];
    int wait_release[2] = { -1, -1 };
    pid_t waiter[2] = { -1, -1 };
    for (int w = 0; w < 2; ++w) {
        if (pipe(wait_report[w]) != 0) {
            perror("pipe");
            reap_all(children);
            return 2;
        }
        int rel[2];
        if (pipe(rel) != 0) {
            perror("pipe");
            reap_all(children);
            return 2;
        }
        const pid_t pid = spawn(dir, wait_report[w][1], rel[0]);
        close(rel[0]);
        wait_release[w] = rel[1];
        if (pid <= 0) {
            // fork failed: never push -1 -- cleanup would kill(-1).
            perror("fork");
            close(wait_release[w]);
            wait_release[w] = -1;
            reap_all(children);
            return 2;
        }
        children.push_back(pid);
        waiter[w] = pid;
    }
    check(read_line(wait_report[0][0], 300).empty() &&
          read_line(wait_report[1][0], 300).empty(),
          "both waiters block while every slot is held");

    /* 3. The slot each waiter's DELETED historical wait would have slept
       on, getpid() % slots -- computable in the parent, since fork()
       hands the child's pid back.  With four slots and two waiters at
       most two slots are excluded, so some holder always sits on a slot
       that is neither waiter's target: freeing it must hand the slot to
       SOME waiter within 1 s, which the historical wait could not do.  */
    const std::string legacy_target[2] = {
        slot_name(waiter[0] % 4),
        slot_name(waiter[1] % 4),
    };
    int victim = -1;
    for (int i = 0; i < 4; ++i) {
        if (!held[i].empty() && held[i] != legacy_target[0] && held[i] != legacy_target[1]) {
            victim = i;
            break;
        }
    }
    check(victim >= 0, "a holder off both waiters' historical targets exists");
    int got_fd = -1;
    if (victim >= 0) {
        close(release[victim]);
        release[victim] = -1;
        const std::string got = read_either_line(wait_report[0][0], wait_report[1][0],
                                                 1000, &got_fd);
        check(got == held[victim],
              "some waiter takes the freed non-target slot within 1 s");
    }

    // 4. release the remaining holders: the still-queued waiter must take
    //    one of their slots and report within 2 s (a waiter holds its slot
    //    until its release pipe closes, so exactly one report was in
    //    flight above and this one is the other waiter's).  Then close the
    //    waiters' release pipes so both holding children exit.
    for (int i = 0; i < 4; ++i) {
        if (release[i] >= 0) {
            close(release[i]);
            release[i] = -1;
        }
    }
    int took_fd = -1;
    if (got_fd == wait_report[0][0] || got_fd == wait_report[1][0]) {
        const int other_fd = got_fd == wait_report[0][0] ? wait_report[1][0]
                                                         : wait_report[0][0];
        took_fd = read_line(other_fd, 2000).empty() ? -1 : other_fd;
    }
    check((got_fd == wait_report[0][0] || took_fd == wait_report[0][0]) &&
          (got_fd == wait_report[1][0] || took_fd == wait_report[1][0]),
          "the still-queued waiter takes a slot once the remaining holders"
          " are released");
    for (int w = 0; w < 2; ++w) {
        if (wait_release[w] >= 0) {
            close(wait_release[w]);
            wait_release[w] = -1;
        }
    }

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
