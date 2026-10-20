/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <limits.h>

#include <sys/types.h>
#include <pwd.h>

#include <sys/stat.h>
#include <sys/file.h>

#include <vector>

#include "client.h"
#include "exitcode.h"
#include "job.h"
#include "logging.h"
#include "ncpus.h"
#include "util.h"

using namespace std;

extern bool explicit_color_diagnostics;
extern bool explicit_no_show_caret;

/**
 * Set the `FD_CLOEXEC' flag of DESC if VALUE is nonzero,
 * or clear the flag if VALUE is 0.
 *
 * From the GNU C Library examples.
 *
 * @returns 0 on success, or -1 on error with `errno' set.
 **/
int set_cloexec_flag(int desc, int value)
{
    int oldflags = fcntl(desc, F_GETFD, 0);

    /* If reading the flags failed, return error indication now. */
    if (oldflags < 0) {
        return oldflags;
    }

    /* Set just the flag we want to set. */
    if (value != 0) {
        oldflags |= FD_CLOEXEC;
    } else {
        oldflags &= ~FD_CLOEXEC;
    }

    /* Store modified flag word in the descriptor. */
    return fcntl(desc, F_SETFD, oldflags);
}

/**
 * Ignore or unignore SIGPIPE.
 *
 * The server and child ignore it, because distcc code wants to see
 * EPIPE errors if something goes wrong.  However, for invoked
 * children it is set back to the default value, because they may not
 * handle the error properly.
 **/
int dcc_ignore_sigpipe(int val)
{
    if (signal(SIGPIPE, val ? SIG_IGN : SIG_DFL) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, " << (val ? "ignore" : "default") << ") failed: "
                      << strerror(errno) << endl;
        return EXIT_DISTCC_FAILED;
    }

    return 0;
}

/**
 * Get an exclusive, non-blocking lock on a file using whatever method
 * is available on this system.
 *
 * @retval 0 if we got the lock
 * @retval -1 with errno set if the file is already locked.
 **/
static int sys_lock(int fd, bool block)
{
#if defined(F_SETLK)
    struct flock lockparam;

    lockparam.l_type = F_WRLCK;
    lockparam.l_whence = SEEK_SET;
    lockparam.l_start = 0;
    lockparam.l_len = 0;        /* whole file */

    return fcntl(fd, block ? F_SETLKW : F_SETLK, &lockparam);
#elif defined(HAVE_FLOCK)
    return flock(fd, LOCK_EX | (block ? 0 : LOCK_NB));
#elif defined(HAVE_LOCKF)
    return lockf(fd, block ? F_LOCK : F_TLOCK, 0);
#else
#  error "No supported lock method.  Please port this code."
#endif
}


static volatile int lock_fd = -1;

void dcc_unlock()
{
    // This must be safe to use from a signal handler.
    if (lock_fd != -1)
        close(lock_fd); // All our current locks can just be closed.
    lock_fd = -1;
}


/**
 * Open a lockfile, creating if it does not exist.
 **/
static bool dcc_open_lockfile(const string &fname, int &plockfd)
{
    /* Create if it doesn't exist.  We don't actually do anything with
     * the file except lock it.
     *
     * The file is created with the loosest permissions allowed by the user's
     * umask, to give the best chance of avoiding problems if they should
     * happen to use a shared lock dir. */
    plockfd = open(fname.c_str(), O_WRONLY | O_CREAT, 0666);

    if (plockfd == -1 && errno != EEXIST) {
        log_error() << "failed to create " << fname << ": " << strerror(errno) << endl;
        return false;
    }

    set_cloexec_flag(plockfd, true);

    return true;
}

int dcc_locked_fd()
{
    return lock_fd;
}

/* The no-fork local build execs the compiler in THIS process.  The slot
   lock is an fcntl record lock on a close-on-exec fd, so exec would close
   the fd and release the slot the moment the compiler starts -- leaving
   daemonless local-fallback concurrency effectively unbounded (measured:
   a farm-and-daemon outage put one compiler per submitted job on the
   machine, not one per CPU).  Clearing close-on-exec keeps the fd -- and
   with it the record lock -- alive for exactly the compiler's lifetime;
   the kernel releases both when the compiler exits.

   Returns false when the flag could not be cleared: the caller must then
   NOT take the no-fork exec (it would run unbounded); forking instead
   keeps the record lock in the parent, which holds it through the wait.
   Descriptor 0 is a valid lock fd (stdin may be closed), so the test is
   against the -1 sentinel, not positivity.  */
bool dcc_lock_keep_across_exec()
{
    if (lock_fd == -1) {
        return true;    // no lock held; nothing to preserve
    }
    if (set_cloexec_flag(lock_fd, false) != 0) {
        log_error() << "cannot clear close-on-exec on the local-build slot lock (fd "
                    << lock_fd << "): " << strerror(errno) << endl;
        return false;
    }
    return true;
}

/* The local slot wait is work-conserving: a waiter never commits to one
   slot.  The historical wait did -- it blocked on the single slot chosen
   as getpid() % slots, and a build tool spawns its compiler wrappers with
   a fixed pid stride, so the blocked waiters piled onto a fraction of the
   slots while the others were released into a void: on a 24-CPU client
   under a -j600 build, /proc/locks showed ~125 waiters spread over only
   12-14 of the 24 slots, and slots sat free while waiters existed in
   30-60% of the busy samples -- the local preprocessor running at roughly
   half its slot capacity exactly when the farm was starved for
   preprocessed sources.

   The queue protocol, on the same slot files as the historical wait:
     1. scan every slot non-blocking from a well-mixed starting offset;
     2. if all are busy, block on the pool's queue lock (`local_lock_queue`,
        an fcntl lock like the slots, so a dead waiter frees it);
     3. the queue holder alone rescans every slot each millisecond until
        one is free, takes it, and releases the queue lock to the next
        waiter.
   One poller per pool keeps the cost to ~24 fcntl calls per millisecond
   while any freed slot is reused within about a millisecond.  New arrivals
   may take a free slot in step 1 ahead of the queue; that only keeps the
   pool busier.

   Older icecc binaries that still block on a single slot contend for the
   same pool: the slot files, the lock dir and the fcntl lock type are
   unchanged, and they never open the queue file.

   If the queue lock itself cannot be taken -- the queue file cannot be
   opened, or fcntl fails with anything but EINTR -- the waiter logs it
   once and degrades to polling the same millisecond rescan WITHOUT the
   queue: still work-conserving, at the price of one poller per such
   waiter instead of one per pool.  A slot whose fcntl fails with a
   persistent non-busy error (ENOLCK, EIO, ...) is logged once, closed
   and dropped from the waiter's pool; once no slot fd is left the wait
   gives up and returns false (callers fail with EXIT_DISTCC_FAILED),
   exactly as the historical wait did on its one bad slot.  No path ever
   returns to the historical single-slot wait.  */

static unsigned dcc_lock_spread_offset()
{
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t x = (static_cast<uint64_t>(getpid()) << 32) ^
                 static_cast<uint64_t>(now.tv_nsec) ^
                 static_cast<uint64_t>(now.tv_sec);
    // splitmix64 finalizer: every input bit reaches the low bits the modulo
    // keeps, so a pid stride no longer pins the offset to a residue class.
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    x ^= x >> 31;
    return static_cast<unsigned>(x >> 32);
}

static string dcc_lock_slot_name(const string &fname, int lock)
{
    if (lock == 0) { // 1st keep without the 0 for backwards compatibility
        return fname;
    }
    char num[20];
    sprintf(num, "%d", lock);
    return fname + num;
}

static bool dcc_lock_try_slots(vector<int> &fds, unsigned start)
{
    const size_t count = fds.size();
    for (size_t k = 0; k < count; ++k) {
        const size_t slot = (start + k) % count;
        if (fds[slot] == -1) {
            continue;
        }
        if (sys_lock(fds[slot], false) == 0) {
            lock_fd = fds[slot];
            fds[slot] = -1;
            return true;
        }
        if (errno != EAGAIN && errno != EACCES
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
            && errno != EWOULDBLOCK
#endif
           ) {
            log_error() << "lock slot " << slot << " failed: " << strerror(errno) << endl;
            /* A persistent non-busy error (ENOLCK, EIO, ...) would spin the
               rescan and relog every millisecond forever: close the fd and
               drop the slot from this waiter's pool, exactly once.  */
            ::close(fds[slot]);
            fds[slot] = -1;
        }
    }
    return false;
}

static bool dcc_lock_host_queued(const string &fname, int max_cpu)
{
    vector<int> fds(max_cpu, -1);
    bool opened = false;
    for (int lock = 0; lock < max_cpu; ++lock) {
        int fd = -1;
        if (dcc_open_lockfile(dcc_lock_slot_name(fname, lock), fd) && fd != -1) {
            fds[lock] = fd;
            opened = true;
        }
    }
    auto close_rest = [&fds]() {
        for (int &fd : fds) {
            if (fd != -1) {
                ::close(fd);
                fd = -1;
            }
        }
    };
    if (!opened) {
        return false;
    }

    unsigned start = dcc_lock_spread_offset();
    if (dcc_lock_try_slots(fds, start)) {
        close_rest();
        return true;
    }

    /* Block for the queue lock unless it cannot be taken at all.  EINTR
       from a stray signal is retried; any other failure logs once and
       degrades this waiter to the same millisecond rescan WITHOUT the
       queue (one poller per waiter instead of one per pool) rather than
       ever returning to a single-slot wait.  */
    int queue_fd = -1;
    if (dcc_open_lockfile(fname + "_queue", queue_fd) && queue_fd != -1) {
        bool held = false;
        for (;;) {
            if (sys_lock(queue_fd, true) == 0) {
                held = true;
                break;
            }
            if (errno != EINTR) {
                log_error() << "lock " << fname << "_queue failed: "
                            << strerror(errno) << endl;
                break;
            }
        }
        if (!held) {
            ::close(queue_fd);
            queue_fd = -1;
        }
    }
    bool got = false;
    while (!(got = dcc_lock_try_slots(fds, ++start))) {
        bool any_open = false;
        for (int fd : fds) {
            if (fd != -1) {
                any_open = true;
                break;
            }
        }
        if (!any_open) {
            /* Every slot fd was dropped (persistent lock error) or closed:
               the pool is dead for this waiter, and polling it would spin
               forever.  Fail like the historical wait did on its one bad
               slot -- callers map false to EXIT_DISTCC_FAILED.  */
            if (queue_fd != -1) {
                ::close(queue_fd);
            }
            return false;
        }
        struct timespec pause = {0, 1000000};
        nanosleep(&pause, nullptr);
    }
    if (queue_fd != -1) {
        ::close(queue_fd); // releases the queue to the next waiter
    }
    close_rest();
    return got;
}

/* The slot pool, with the directory and size as explicit arguments: the
   production wrapper supplies the shared per-user directory and one slot
   per online CPU, and the lock-lifetime regression test supplies a private
   directory and a fixed two-slot pool so it can never throttle -- or be
   perturbed by -- real local builds.  */
bool dcc_lock_host_at(const string &lockdir, int max_cpu)
{
    assert(lock_fd == -1);

    if (mkdir(lockdir.c_str(), 0700) && errno != EEXIST) {
        log_perror("mkdir") << "\t" << lockdir << endl;
        return false;
    }

    string fname = lockdir + "/local_lock";
    /* lock_fd is deliberately NOT parked at 0 while probing: descriptor 0
       is a valid open() result when stdin is closed, so 0 must remain
       distinguishable as "a real lock fd".  -1 is the only not-locked
       sentinel.  */
    if (max_cpu < 1) {
        max_cpu = 1;
    }
    return dcc_lock_host_queued(fname, max_cpu);
}

bool dcc_lock_host()
{
    string dir = "/tmp/.icecream-";
    struct passwd *pwd = getpwuid(getuid());

    if (pwd) {
        dir += pwd->pw_name;
    } else {
        char buffer[12];
        sprintf(buffer, "%ld", (long)getuid());
        dir += buffer;
    }

    int max_cpu = 1;
    dcc_ncpus(&max_cpu);
    return dcc_lock_host_at(dir, max_cpu);
}

bool color_output_possible()
{
    const char* term_env = getenv("TERM");

    return isatty(2) && term_env && strcasecmp(term_env, "DUMB");
}

bool compiler_has_color_output(const CompileJob &job)
{
    if (!color_output_possible())
        return false;

    // Clang has coloring.
    if (compiler_is_clang(job)) {
        return true;
    }
    if (const char* icecc_color_diagnostics = getenv("ICECC_COLOR_DIAGNOSTICS")) {
        return *icecc_color_diagnostics == '1';
    }
#ifdef HAVE_GCC_COLOR_DIAGNOSTICS
    return true;
#endif
    // GCC has it since 4.9, but that'd require detecting what GCC
    // version is used for the actual compile. However it requires
    // also GCC_COLORS to be set (and not empty), so use that
    // for detecting if GCC would use colors.
    if (const char *gcc_colors = getenv("GCC_COLORS")) {
        return (*gcc_colors != '\0');
    }
    return false;
}

// Whether icecream should add colors to the compiler output.
bool colorify_wanted(const CompileJob &job)
{
    if (compiler_has_color_output(job)) {
        return false; // -fcolor-diagnostics handling lets the compiler do it itself
    }
    if (explicit_color_diagnostics) { // colors explicitly enabled/disabled by an option
        return false;
    }
    if (getenv("ICECC_COLOR_DIAGNOSTICS") != nullptr)
        return false; // if set explicitly, assume icecream's colorify is not wanted

    if (getenv("EMACS")) {
        return false;
    }

    return color_output_possible();
}

void colorify_output(const string &_s_ccout)
{
    string s_ccout(_s_ccout);
    string::size_type end;

    while ((end = s_ccout.find('\n')) !=  string::npos) {

        string cline = s_ccout.substr(string::size_type(0), end);
        s_ccout = s_ccout.substr(end + 1);

        if (cline.find(": error:") != string::npos) {
            fprintf(stderr, "\x1b[1;31m%s\x1b[0m\n", cline.c_str());
        } else if (cline.find(": warning:") != string::npos) {
            fprintf(stderr, "\x1b[36m%s\x1b[0m\n", cline.c_str());
        } else {
            fprintf(stderr, "%s\n", cline.c_str());
        }
    }

    fprintf(stderr, "%s", s_ccout.c_str());
}


bool ignore_unverified()
{
    return getenv("ICECC_IGNORE_UNVERIFIED");
}

// GCC4.8+ has -fdiagnostics-show-caret, but when it prints the source code,
// it tries to find the source file on the disk, rather than printing the input
// it got like Clang does. This means that when compiling remotely, it of course
// won't find the source file in the remote chroot, and will disable the caret
// silently. As a workaround, make it possible to recompile locally if there's
// any stdout/stderr.
// Another way of handling this might be to send all the headers to the remote
// host, but this has been already tried in the sendheaders branch (for
// preprocessing remotely too) and performance-wise it just doesn't seem to
// be worth it.
bool output_needs_workaround(const CompileJob &job)
{
    if (compiler_is_clang(job))
        return false;
    if (explicit_no_show_caret)
        return false;
    if (const char* caret_workaround = getenv("ICECC_CARET_WORKAROUND"))
        return *caret_workaround == '1';
#ifdef HAVE_GCC_SHOW_CARET
    return true;
#endif
    return false;
}

int resolve_link(const std::string &file, std::string &resolved)
{
    char buf[PATH_MAX];
    buf[PATH_MAX - 1] = '\0';
    const int ret = readlink(file.c_str(), buf, sizeof(buf) - 1);
    const int errno_save = errno;

    if (ret <= 0) {
        return errno_save;
    }

    buf[ret] = 0;
    resolved = std::string(buf);
    return 0;
}

std::string get_cwd()
{
    static std::vector<char> buffer(1024);

    errno = 0;
    while (getcwd(&buffer[0], buffer.size() - 1) == nullptr && errno == ERANGE) {
        buffer.resize(buffer.size() + 1024);
        errno = 0;
    }
    if (errno != 0)
        return std::string();

    return string(&buffer[0]);
}
