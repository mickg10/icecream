/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Stephan Kulow <coolo@suse.de>
                  2002, 2003 by Martin Pool <mbp@samba.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

//#define ICECC_DEBUG 1
#ifndef _GNU_SOURCE
// getopt_long
#define _GNU_SOURCE 1
#endif
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <getopt.h>
#include <limits>

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pwd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/utsname.h>

#ifdef HAVE_ARPA_NAMESER_H
#  include <arpa/nameser.h>
#endif

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#include <arpa/inet.h>

#ifdef HAVE_RESOLV_H
#  include <resolv.h>
#endif
#include <netdb.h>

#ifndef RUSAGE_SELF
#  define RUSAGE_SELF (0)
#endif
#ifndef RUSAGE_CHILDREN
#  define RUSAGE_CHILDREN (-1)
#endif

#ifdef HAVE_LIBCAP_NG
#  include <cap-ng.h>
#endif

#include <archive.h>

#include <chrono>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <map>
#include <algorithm>
#include <set>
#include <fstream>
#include <sstream>
#include <string>

#include "ncpus.h"
#include "exitcode.h"
#include "serve.h"
#include "workit.h"
#include "logging.h"
#include "utf8.h"
#include <comm.h>
#include "load.h"
#include "environment.h"
#include "platform.h"
#include "util.h"
#include "getifaddrs.h"

static std::string pidFilePath;
static volatile sig_atomic_t exit_main_loop = 0;

#ifndef __attribute_warn_unused_result__
#define __attribute_warn_unused_result__
#endif

using namespace std;

static uint64_t monotonic_msec()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct FdSnapshot {
    long open_count;
    rlim_t soft_limit;
    rlim_t hard_limit;
    FdSnapshot()
        : open_count(-1)
        , soft_limit(RLIM_INFINITY)
        , hard_limit(RLIM_INFINITY) {}
};

static FdSnapshot collect_fd_snapshot()
{
    FdSnapshot snapshot;

    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) == 0) {
        snapshot.soft_limit = lim.rlim_cur;
        snapshot.hard_limit = lim.rlim_max;
    }

    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        return snapshot;
    }

    long count = 0;
    while (readdir(dir) != nullptr) {
        ++count;
    }
    closedir(dir);

    // ".", "..", and the descriptor used by opendir() itself.
    if (count >= 3) {
        count -= 3;
    } else {
        count = 0;
    }

    snapshot.open_count = count;
    return snapshot;
}

/* Linux argv and file names are byte strings, not UTF-8, but the responses
   and JSONL we emit declare UTF-8.  json_escape() validates as it escapes
   and replaces any malformed sequence with U+FFFD, so a stray 0x80-0xff
   byte in a path or command line cannot produce a document that a strict
   parser rejects.  Validation is full scalar-value checking (utf8.h) --
   overlongs, surrogates, values above U+10FFFF and C0/C1/F5-FF leads are
   all replaced, not just malformed continuation shapes.  */

/* Storage sink for state JSONL records with its own thread: open() and
   write() on a regular file can block for as long as the storage stack
   wants regardless of O_NONBLOCK, so no storage call may run on the
   daemon's control loop.  The loop only appends to a bounded in-memory
   queue (drop-oldest, per-record cap); this thread does all I/O and simply
   sleeps through backoff.

   The thread starts lazily on the first enqueue -- which happens in the
   running main loop, safely after daemonize()'s forks.  Compile children
   forked later never touch the writer, and the sweep in serve.cpp closes
   its descriptor in the child.  */
class AsyncJsonlWriter {
public:
    ~AsyncJsonlWriter() { stop(); }

    void set_path(const std::string &path) { path_ = path; }
    const std::string &path() const { return path_; }

    void enqueue(const std::string &line)
    {
        if (path_.empty()) {
            return;
        }
        if (line.size() > max_record_bytes) {
            /* One pathological record must not displace the whole queue's
               worth of history (the queue bound alone would let a single
               record approach 4 MiB).  */
            ++oversized_;
            return;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (!started_) {
            started_ = true;
            thread_ = std::thread(&AsyncJsonlWriter::run, this);
        }
        queue_.push_back(line);
        queue_.back() += '\n';
        queued_bytes_ += queue_.back().size();
        while (queued_bytes_ > max_queued_bytes && queue_.size() > 1) {
            /* Drop the oldest record: recent telemetry is the useful kind
               when storage is misbehaving.  */
            queued_bytes_ -= queue_.front().size();
            queue_.pop_front();
            ++dropped_;
        }
        queued_snapshot_ = queued_bytes_;
        cond_.notify_one();
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!started_) {
                return;
            }
            stopping_ = true;
        }
        cond_.notify_one();
        thread_.join();
        started_ = false;
        stopping_ = false;
    }

    uint64_t dropped() const { return dropped_.load(); }
    uint64_t oversized() const { return oversized_.load(); }
    uint64_t open_failures() const { return open_failures_.load(); }
    uint64_t write_failures() const { return write_failures_.load(); }
    size_t queued_bytes() const { return queued_snapshot_.load(); }

private:
    void run()
    {
        int fd = -1;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            cond_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                break;   // stopping, and everything already flushed
            }
            std::string rec = std::move(queue_.front());
            queue_.pop_front();
            queued_bytes_ -= rec.size();
            queued_snapshot_ = queued_bytes_;
            lock.unlock();

            bool ok = true;
            if (fd < 0) {
                fd = ::open(path_.c_str(),
                            O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
                if (fd < 0) {
                    ++open_failures_;
                    ok = false;
                }
            }
            if (ok) {
                size_t done = 0;
                while (done < rec.size()) {
                    ssize_t n = ::write(fd, rec.data() + done, rec.size() - done);
                    if (n < 0 && errno == EINTR) {
                        continue;
                    }
                    if (n <= 0) {
                        ++write_failures_;
                        close(fd);
                        fd = -1;
                        ok = false;
                        break;
                    }
                    done += size_t(n);
                }
            }

            lock.lock();
            if (!ok && !stopping_) {
                /* Requeue the record at the front (bounded by the normal
                   drop-oldest policy) and back off five seconds before the
                   next attempt -- in this thread, sleeping is free.  */
                queue_.push_front(std::move(rec));
                queued_bytes_ += queue_.front().size();
                queued_snapshot_ = queued_bytes_;
                cond_.wait_for(lock, std::chrono::seconds(5),
                               [&] { return stopping_; });
            }
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    static const size_t max_queued_bytes = 4 * 1024 * 1024;
    static const size_t max_record_bytes = 256 * 1024;
    std::string path_;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<std::string> queue_;
    size_t queued_bytes_ = 0;
    bool started_ = false;
    bool stopping_ = false;
    std::thread thread_;
    std::atomic<uint64_t> dropped_{0};
    std::atomic<uint64_t> oversized_{0};
    std::atomic<uint64_t> open_failures_{0};
    std::atomic<uint64_t> write_failures_{0};
    std::atomic<size_t> queued_snapshot_{0};
};

static string json_escape(const string &s)
{
    string out;
    out.reserve(s.size() + 16);

    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[7];
                snprintf(buf, sizeof(buf), "\\u%04x", int(c));
                out += buf;
            } else if (c < 0x80) {
                out += char(c);
            } else {
                /* Validate the multi-byte sequence before copying it; any
                   malformed byte becomes U+FFFD so the output is always
                   well-formed UTF-8 (see the note above this function).  */
                const size_t len = utf8_scalar_sequence_length(
                    reinterpret_cast<const unsigned char *>(s.data()) + i,
                    s.size() - i);
                if (len >= 2) {
                    out.append(s, i, len);
                    i += len - 1;
                } else {
                    out += "\xef\xbf\xbd";   // U+FFFD REPLACEMENT CHARACTER
                }
            }
        }
    }

    return out;
}

static bool string_ends_with(const string &value, const char *suffix)
{
    const size_t suffix_len = strlen(suffix);
    return value.size() >= suffix_len
           && value.compare(value.size() - suffix_len, suffix_len, suffix) == 0;
}

static string local_job_kind_from_outfile(const string &outfile, bool fulljob)
{
    if (fulljob) {
        return "fulljob";
    }

    if (outfile.empty()) {
        return "unknown";
    }

    const string::size_type slash = outfile.find_last_of('/');
    const string base = (slash == string::npos) ? outfile : outfile.substr(slash + 1);

    if (string_ends_with(base, ".o") || string_ends_with(base, ".obj")) {
        return "object";
    }
    if (string_ends_with(base, ".a") || string_ends_with(base, ".lib")) {
        return "archive";
    }
    if (string_ends_with(base, ".so") || base.find(".so.") != string::npos || string_ends_with(base, ".dylib")) {
        return "sharedlib";
    }
    if (string_ends_with(base, ".exe")) {
        return "executable";
    }

    return "other";
}

static bool cmdline_is_preprocess_only(const string &cmdline)
{
    if (cmdline.empty()) {
        return false;
    }

    istringstream in(cmdline);
    string token;
    while (in >> token) {
        if (token == "-E") {
            return true;
        }
        if (token.size() >= 2 && token[0] == '-' && token[1] == 'M'
                && token != "-MD" && token != "-MMD"
                && token != "-MF" && token != "-MT"
                && token != "-MQ" && token != "-MG"
                && token != "-MP") {
            return true;
        }
        if (token.rfind("-Wp,-M", 0) == 0
                && token.rfind("-Wp,-MD", 0) != 0
                && token.rfind("-Wp,-MMD", 0) != 0
                && token.rfind("-Wp,-MF", 0) != 0
                && token.rfind("-Wp,-MT", 0) != 0
                && token.rfind("-Wp,-MQ", 0) != 0) {
            return true;
        }
    }

    return false;
}

static bool classify_local_preprocess_job(uint32_t local_flags, const string &cmdline, bool fulljob)
{
    if (fulljob) {
        return false;
    }
    if (local_flags & JobLocalBeginMsg::LocalFlagPreprocessOnly) {
        return true;
    }
    return cmdline_is_preprocess_only(cmdline);
}

static string shell_quote_arg(const string &arg)
{
    if (arg.empty()) {
        return "''";
    }

    if (arg.find_first_of(" \t\r\n'\"`$\\|&;<>()[\\]{}*?!") == string::npos) {
        return arg;
    }

    string quoted = "'";
    for (char c : arg) {
        if (c == '\'') {
            quoted += "'\"'\"'";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

static string command_line_from_args(const vector<string> &args)
{
    string out;
    out.reserve(512);
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) {
            out += ' ';
        }
        out += shell_quote_arg(args[i]);
    }
    return out;
}

static string command_line_from_proc_pid(pid_t pid)
{
    if (pid <= 0) {
        return string();
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return string();
    }

    string raw;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        raw.append(buf, size_t(n));
        if (raw.size() > 65536) {
            break;
        }
    }
    close(fd);

    if (raw.empty()) {
        return string();
    }

    vector<string> args;
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = raw.find('\0', start);
        if (end == string::npos) {
            end = raw.size();
        }
        if (end > start) {
            args.push_back(raw.substr(start, end - start));
        }
        if (end == raw.size()) {
            break;
        }
        start = end + 1;
    }

    if (args.empty()) {
        return string();
    }
    return command_line_from_args(args);
}

static string command_line_from_peer_socket(int fd)
{
#if defined(__linux__) && defined(SO_PEERCRED)
    if (fd < 0) {
        return string();
    }
    struct ucred peer;
    socklen_t peer_len = sizeof(peer);
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0) {
        return string();
    }
    return command_line_from_proc_pid(peer.pid);
#else
    (void)fd;
    return string();
#endif
}

static string command_line_from_compile_job(const CompileJob *job)
{
    if (!job) {
        return string();
    }

    vector<string> args;
    if (!job->compilerName().empty()) {
        args.push_back(job->compilerName());
    } else {
        args.push_back("<compiler>");
    }

    for (const string &flag : job->allFlags()) {
        args.push_back(flag);
    }

    if (!job->inputFile().empty()) {
        args.push_back(job->inputFile());
    }

    if (!job->outputFile().empty()) {
        args.push_back("-o");
        args.push_back(job->outputFile());
    }

    return command_line_from_args(args);
}

static const uint64_t waitforcs_latency_bucket_upper_bounds_msec[] = {
    1, 2, 5, 10, 20, 50, 100, 250, 500, 1000, 2000, 5000, 10000
};

static const size_t waitforcs_latency_bucket_count =
    sizeof(waitforcs_latency_bucket_upper_bounds_msec) / sizeof(waitforcs_latency_bucket_upper_bounds_msec[0]);

struct Client {
public:
    /*
     * UNKNOWN: Client was just created - not supposed to be long term
     * GOTNATIVE: Client asked us for the native env - this is the first step
     * PENDING_USE_CS: We have a CS from scheduler and need to tell the client
     *          as soon as there is a spot available on the local machine
     * JOBDONE: This was compiled by a local client and we got a jobdone - awaiting END
     * LINKJOB: This is a local-only job by a local client we told the scheduler about
     *          and await the finish of it
     * TOINSTALL: We're receiving an environment transfer and wait for it to complete.
     * WAITINSTALL: Client is waiting for the environment transfer unpacking child to finish.
     * TOCOMPILE: We're supposed to compile it ourselves
     * WAITFORCS: Client asked for a CS and we asked the scheduler - waiting for its answer
     * WAITCOMPILE: Client got a CS and will ask him now (it's not me)
     * CLIENTWORK: Client is busy working and we reserve the spot (job_id is set if it's a scheduler job)
     * WAITFORCHILD: Client is waiting for the compile job to finish.
     * WAITCREATEENV: We're waiting for icecc-create-env to finish.
     */
    enum Status { UNKNOWN, GOTNATIVE, PENDING_USE_CS, JOBDONE, LINKJOB, TOINSTALL, WAITINSTALL, TOCOMPILE,
                  WAITFORCS, WAITCOMPILE, CLIENTWORK, WAITFORCHILD, WAITCREATEENV,
                  LASTSTATE = WAITCREATEENV
                } status;
    Client() {
        created_ts = time(nullptr);
        created_msec = monotonic_msec();
        status_since_msec = created_msec;
        last_waitforcs_msec = 0;
        env_bytes_received = 0;
        timing_submit_ts = 0;
        timing_enqueue_msec = 0;
        timing_start_msec = 0;
        timing_finish_msec = 0;
        timing_waitforcs_msec = 0;
        timing_local_queue_msec = 0;
        timing_exec_msec = 0;
        timing_scheduler_job_id = 0;
        timing_compile_job_id = 0;
        timing_exitcode = 0;
        has_timing = false;
        has_timing_exitcode = false;
        local_preprocess = false;
        running_preprocess = false;
        job_id = 0;
        last_known_job_id = 0;
        channel = nullptr;
        job = nullptr;
        usecsmsg = nullptr;
        client_id = 0;
        niceness = 0;
        status = UNKNOWN;
        pipe_from_child = -1;
        pipe_to_child = -1;
        child_pid = -1;
        fulljob = false;
    }

    void set_status(Status new_status, const char* why = nullptr) {
        status = new_status;
        status_since_msec = monotonic_msec();
        status_why = why ? why : "";
    }

    static string status_str(Status status) {
        switch (status) {
        case UNKNOWN:
            return "unknown";
        case GOTNATIVE:
            return "gotnative";
        case PENDING_USE_CS:
            return "pending_use_cs";
        case JOBDONE:
            return "jobdone";
        case LINKJOB:
            return "linkjob";
        case TOINSTALL:
            return "toinstall";
        case WAITINSTALL:
            return "waitinstall";
        case TOCOMPILE:
            return "tocompile";
        case WAITFORCS:
            return "waitforcs";
        case CLIENTWORK:
            return "clientwork";
        case WAITCOMPILE:
            return "waitcompile";
        case WAITFORCHILD:
            return "waitforchild";
        case WAITCREATEENV:
            return "waitcreateenv";
        }

        assert(false);
        return string(); // shutup gcc
    }

    ~Client() {
        status = (Status) - 1;
        delete channel;
        channel = nullptr;
        delete usecsmsg;
        usecsmsg = nullptr;
        delete job;
        job = nullptr;

        if (pipe_from_child >= 0) {
            if (-1 == close(pipe_from_child) && (errno != EBADF)){
                log_perror("Failed to close pipe from child process");
            }
        }
        if (pipe_to_child >= 0) {
            if (-1 == close(pipe_to_child) && (errno != EBADF)){
                log_perror("Failed to close pipe to child process");
            }
        }

    }
    uint32_t job_id;
    string outfile; // only useful for LINKJOB or TOINSTALL/WAITINSTALL
    MsgChannel *channel;
    UseCSMsg *usecsmsg;
    CompileJob *job;
    int client_id;
    uint32_t niceness; // nice priority (0-20), for PENDING_USE_CS
    // pipe from child process with end status, only valid if WAITFORCHILD or TOINSTALL/WAITINSTALL
    int pipe_from_child;
    // pipe to child process, only valid if TOINSTALL/WAITINSTALL
    int pipe_to_child;
    pid_t child_pid;
    bool fulljob; // during LINKJOB and CLIENTWORK, reserve all slots if set
    string pending_create_env; // only for WAITCREATEENV
    uint64_t created_msec;
    time_t created_ts;
    uint64_t status_since_msec;
    uint64_t last_waitforcs_msec;
    uint64_t env_bytes_received;
    uint32_t last_known_job_id;
    string status_why;
    string local_reason;
    string command_line;
    uint32_t timing_submit_ts;
    uint32_t timing_enqueue_msec;
    uint32_t timing_start_msec;
    uint32_t timing_finish_msec;
    uint32_t timing_waitforcs_msec;
    uint32_t timing_local_queue_msec;
    uint32_t timing_exec_msec;
    uint32_t timing_scheduler_job_id;
    uint32_t timing_compile_job_id;
    int timing_exitcode;
    bool has_timing;
    bool has_timing_exitcode;
    string timing_mode;
    bool local_preprocess;
    bool running_preprocess;

    string dump() const {
        uint64_t age_msec = monotonic_msec() - status_since_msec;
        string ret = status_str(status) + " age_msec=" + toString(age_msec);
        if (last_waitforcs_msec) {
            ret += " last_waitforcs_msec=" + toString(last_waitforcs_msec);
        }
        if (!status_why.empty()) {
            ret += " why=" + status_why;
        }
        ret += " " + channel->dump();

        switch (status) {
        case LINKJOB:
            return ret + " ClientID: " + toString(client_id) + " " + outfile + (fulljob ? " (full)" : "")
                + " local_reason=" + (local_reason.empty() ? string("unknown") : local_reason)
                + " PID: " + toString(child_pid);
        case TOINSTALL:
        case WAITINSTALL:
            return ret + " ClientID: " + toString(client_id) + " " + outfile + " PID: " + toString(child_pid)
                + " env_bytes_received=" + toString(env_bytes_received);
        case WAITFORCHILD:
            return ret + " ClientID: " + toString(client_id) + " PID: " + toString(child_pid) + " PFD: " + toString(pipe_from_child);
        case WAITCREATEENV:
            return ret + " " + toString(client_id) + " " + pending_create_env;
        default:
            ret += " ClientID: " + toString(client_id);
            if (job_id) {
                ret += " Job ID: " + toString(job_id);
                if (usecsmsg)
                    ret += " CompileServer: " + usecsmsg->hostname;
            }
            if (niceness != 0)
                ret += " Nice: " + toString(niceness);
            return ret;
        }
    }
};

static string client_command_line_for_display(const Client *client)
{
    if (!client) {
        return string();
    }

    string cmdline = client->command_line;
    if (cmdline.empty()) {
        cmdline = command_line_from_compile_job(client->job);
    }

    const size_t kMaxLen = 4096;
    if (cmdline.size() > kMaxLen) {
        cmdline.resize(kMaxLen);
        cmdline += " ...[truncated]";
    }

    return cmdline;
}

class Clients : public map<MsgChannel*, Client*>
{
public:
    Clients() {
        active_processes = 0;
    }
    unsigned int active_processes;

    Client *find_by_client_id(int id) const {
        for (auto it : *this)
            if (it.second->client_id == id) {
                return it.second;
            }

        return nullptr;
    }

    Client *find_by_pid(pid_t pid) const {
        for (auto it : *this)
            if (it.second->child_pid == pid) {
                return it.second;
            }

        return nullptr;
    }

    Client *first() {
        iterator it = begin();

        if (it == end()) {
            return nullptr;
        }

        Client *cl = it->second;
        return cl;
    }

    string dump_status(Client::Status s) const {
        int count = 0;

        for (auto it : *this) {
            if (it.second->status == s) {
                count++;
            }
        }

        if (count) {
            return toString(count) + " " + Client::status_str(s) + ", ";
        }

        return string();
    }

    string dump_per_status() const {
        string s;

        for (Client::Status i = Client::UNKNOWN; i <= Client::LASTSTATE;
                i = Client::Status(int(i) + 1)) {
            s += dump_status(i);
        }

        return s;
    }
    Client *get_earliest_client(Client::Status s) const {
        // TODO: possibly speed this up in adding some sorted lists
        Client *client = nullptr;
        int min_client_id = 0;
        uint32_t min_niceness = std::numeric_limits<uint32_t>::max();

        for (auto it : *this) {
            if (it.second->status == s && (!min_client_id || min_client_id > it.second->client_id)
                && it.second->niceness < min_niceness ) {
                client = it.second;
                min_client_id = client->client_id;
                min_niceness = client->niceness;
            }
        }

        return client;
    }
};

static int set_new_pgrp()
{
    /* If we're a session group leader, then we are not able to call
     * setpgid().  However, setsid will implicitly have put us into a new
     * process group, so we don't have to do anything. */

    /* Does everyone have getpgrp()?  It's in POSIX.1.  We used to call
     * getpgid(0), but that is not available on BSD/OS. */
    int pgrp_id = getpgrp();

    if (-1 == pgrp_id){
        log_perror("Failed to get process group ID");
        return EXIT_DISTCC_FAILED;
    }

    if (pgrp_id == getpid()) {
        trace() << "already a process group leader\n";
        return 0;
    }

    if (setpgid(0, 0) == 0) {
        trace() << "entered process group\n";
        return 0;
    }

    trace() << "setpgid(0, 0) failed: " << strerror(errno) << endl;
    return EXIT_DISTCC_FAILED;
}

static void dcc_daemon_terminate(int);

/**
 * Catch all relevant termination signals.  Set up in parent and also
 * applies to children.
 **/
void dcc_daemon_catch_signals()
{
    /* SIGALRM is caught to allow for built-in timeouts when running test
     * cases. */

    signal(SIGTERM, &dcc_daemon_terminate);
    signal(SIGINT, &dcc_daemon_terminate);
    signal(SIGALRM, &dcc_daemon_terminate);
}

pid_t dcc_master_pid;

/**
 * Called when a daemon gets a fatal signal.
 *
 * Some cleanup is done only if we're the master/parent daemon.
 **/
static void dcc_daemon_terminate(int whichsig)
{
    /**
     * This is a signal handler. don't do stupid stuff.
     * Don't call printf. and especially don't call the log_*() functions.
     */

    if (exit_main_loop > 1) {
        // The > 1 is because we get one more signal from the kill(0,...) below.
        // hmm, we got killed already twice. try better
        static const char msg[] = "forced exit.\n";
        ignore_result(write(STDERR_FILENO, msg, strlen( msg )));
        _exit(1);
    }

    // make BSD happy
    signal(whichsig, dcc_daemon_terminate);

    bool am_parent = (getpid() == dcc_master_pid);

    if (am_parent && exit_main_loop == 0) {
        /* kill whole group */
        kill(0, whichsig);

        /* Remove pid file */
        unlink(pidFilePath.c_str());
    }

    ++exit_main_loop;
}

void usage(const char *reason = nullptr)
{
    if (reason) {
        cerr << reason << endl;
    }

    cerr << "usage: iceccd [-n <netname>] [-m <max_processes>] [--max-preprocess <max_preprocesses>] [--fulljob-policy <compile-lane|exclusive>] [--no-remote] [-d|--daemonize] [-l logfile] [-s <schedulerhost[:port]>]"
        " [-v[v[v]]] [-u|--user-uid <user_uid>] [-b <env-basedir>] [--cache-limit <MB>] [-N <node_name>] [-i|--interface <net_interface>] [-p|--port <port>]"
        " [--state-jsonl <path>] [--state-interval <sec>] [--state-log]"
        " [--webgui] [--webgui-port <port>] [--webgui-addr <addr>]" << endl;
    exit(1);
}

struct timeval last_stat;

// Initial rlimit for a compile job, measured in megabytes.  Will vary with
// the amount of available memory.
int mem_limit = 100;

// Minimum rlimit for a compile job, measured in megabytes.
const int min_mem_limit = 100;

unsigned int max_kids = 0;
unsigned int max_preprocess_kids = 0;
unsigned int preprocess_active_processes = 0;
// number of running whole-node (fulljob) local jobs; while nonzero the
// preprocess lane is closed (the compile lane is closed by the fulljob's
// full slot reservation)
unsigned int fulljob_active = 0;

/* What a fulljob's reservation means is a resource-policy choice, not a
   fixed truth (see aidocs divergence review, section 3):
   - compile-lane (default): reserve every compile slot -- the historical
     observable behavior -- while the bounded preprocess lane keeps
     running.  Best aggregate throughput; link steps share the node with
     lightweight preprocessing.
   - exclusive: whole-node isolation.  The fulljob starts only when both
     local lanes are idle and closes both while it runs; local admission
     of other jobs pauses while one waits (remote TOCOMPILE service for
     other submitters is deliberately NOT paused -- a local link must not
     head-of-line-block the cluster).  Meaningful mainly on --no-remote
     submitter daemons, where big local links live.  */
enum FulljobPolicy { FULLJOB_COMPILE_LANE, FULLJOB_EXCLUSIVE };
FulljobPolicy fulljob_policy = FULLJOB_COMPILE_LANE;
const size_t insights_graph_minutes = 100;
const size_t insights_retention_minutes = 120;
// visibility for the clock-correction paths above
uint64_t insights_clock_resets = 0;
uint64_t insights_dropped_jobs = 0;

size_t cache_size_limit = 256 * 1024 * 1024;

struct NativeEnvironment {
    string name; // the hash
    // Timestamps for files including compiler binaries, if they have changed since the time
    // the native env was built, it needs to be rebuilt.
    map<string, time_t> filetimes;
    time_t last_use;
    size_t size; // tarball size
    int create_env_pipe; // if in progress of creating the environment
    NativeEnvironment() : last_use( 0 ), size( 0 ), create_env_pipe( 0 ) {}
};

struct ReceivedEnvironment {
    ReceivedEnvironment() : last_use( 0 ), size( 0 ) {}
    time_t last_use;
    size_t size; // directory size
};

struct JobHistoryEntry {
    uint64_t seq;
    time_t start_ts;
    time_t end_ts;
    uint64_t start_msec;
    uint64_t end_msec;
    uint64_t duration_msec;
    int client_id;
    // Compiler exit status when the client reported one via JobTimingMsg,
    // otherwise the daemon teardown code (see end_code).
    int exitcode;
    // Daemon-side teardown code (118 close / 119 EndMsg / ...), kept as a
    // transport diagnostic distinct from the compiler result.
    int end_code;
    uint32_t scheduler_job_id;
    uint32_t compile_job_id;
    string final_status;
    string final_why;
    string target;
    string environment;
    string usecs_host;
    uint16_t usecs_port;
    bool usecs_got_env;
    string outfile;
    string cmdline;
    string channel;
    bool client_timing;
    uint32_t client_submit_ts;
    uint32_t client_enqueue_msec;
    uint32_t client_start_msec;
    uint32_t client_finish_msec;
    uint32_t client_waitforcs_msec;
    uint32_t client_local_queue_msec;
    uint32_t client_exec_msec;
    string client_mode;
};

struct InsightsMinuteEntry {
    time_t minute_ts;
    uint32_t jobs_total;
    uint32_t jobs_remote;
    uint32_t jobs_local;
    uint32_t jobs_preprocess;
    uint32_t jobs_failed;
    uint32_t jobs_timed;
    uint64_t queue_sum_msec;
    uint64_t exec_sum_msec;
    uint64_t waitforcs_sum_msec;
    uint32_t queue_samples;
    uint32_t exec_samples;
    uint32_t waitforcs_samples;

    InsightsMinuteEntry()
        : minute_ts(0)
        , jobs_total(0)
        , jobs_remote(0)
        , jobs_local(0)
        , jobs_preprocess(0)
        , jobs_failed(0)
        , jobs_timed(0)
        , queue_sum_msec(0)
        , exec_sum_msec(0)
        , waitforcs_sum_msec(0)
        , queue_samples(0)
        , exec_samples(0)
        , waitforcs_samples(0) {}
};

struct WebConnection {
    int fd;
    string inbuf;
    string outbuf;
    size_t outbuf_ofs;            // bytes of outbuf already written (offset drain)
    bool close_after_write;
    bool response_started;        // first response byte hit the socket
    bool reject_input;            // terminal condition (413): stop reading
    uint64_t created_msec;
    uint64_t last_activity_msec;  // last successful read/write progress
    WebConnection() : fd(-1), outbuf_ofs(0), close_after_write(true),
                      response_started(false), reject_input(false),
                      created_msec(0), last_activity_msec(0) {}
};

// Availability limits for the trusted-network web GUI: a small connection
// cap, an idle deadline, and a hard lifetime.  These bound the fd/memory
// share one misbehaving or forgotten client can take from the single
// event loop.
static const size_t web_max_connections = 64;
static const uint64_t web_idle_deadline_msec = 30 * 1000;
static const uint64_t web_lifetime_deadline_msec = 300 * 1000;

struct Daemon {
    Clients clients;
    // Installed environments received from other nodes. The key is
    // (job->targetPlatform() + "/" job->environmentVersion()).
    map<string, ReceivedEnvironment> received_environments;
    // Map of native environments, the basic one(s) containing just the compiler
    // and possibly more containing additional files (such as compiler plugins).
    // The key is the compiler name and a concatenated list of the additional files
    // (or just the compiler name for the basic ones).
    map<string, NativeEnvironment> native_environments;
    string envbasedir;
    uid_t user_uid;
    gid_t user_gid;
    int warn_icecc_user_errno;
    int tcp_listen_fd;
    int tcp_listen_local_fd; // if tcp_listen is bound to a specific network interface, this one is bound to lo interface
    int unix_listen_fd;
    string machine_name;
    string nodename;
    bool noremote;
    bool custom_nodename;
    size_t cache_size;
    map<int, Client*> fd2client;
    int new_client_id;
    string remote_name;
    time_t next_scheduler_connect;
    unsigned long icecream_load;
    struct timeval icecream_usage;
    int current_load;
    int num_cpus;
    MsgChannel *scheduler;
    DiscoverSched *discover;
    string netname;
    string schedname;
    int scheduler_port;
    string daemon_interface;
    int daemon_port;
    unsigned int supported_features;

    int max_scheduler_pong;
    int max_scheduler_ping;
    unsigned int current_kids;
    uint64_t accept_errors_total;
    uint64_t accept_emfile_errors;
    int last_accept_errno;
    time_t last_accept_errno_ts;

    // Optional periodic dumps for production debugging.
    std::string state_jsonl_path;
    unsigned int state_dump_interval_s;
    bool state_dump_log;
    uint64_t next_state_dump_msec;
    size_t state_dump_worst_clients;
    vector<uint64_t> waitforcs_use_cs_hist;
    vector<uint64_t> waitforcs_no_cs_hist;
    uint64_t waitforcs_use_cs_samples;
    uint64_t waitforcs_no_cs_samples;
    uint64_t waitforcs_use_cs_sum_msec;
    uint64_t waitforcs_no_cs_sum_msec;
    uint64_t waitforcs_use_cs_max_msec;
    uint64_t waitforcs_no_cs_max_msec;

    bool webgui_enabled;
    int webgui_port;
    string webgui_addr;
    bool webgui_best_effort;
    int web_listen_fd;
    map<int, WebConnection> web_connections;
    uint64_t web_accept_backoff_until_msec = 0;
    size_t job_history_capacity;
    uint64_t next_job_history_seq;
    deque<JobHistoryEntry> job_history;
    deque<InsightsMinuteEntry> insights_history;

    Daemon() {
        warn_icecc_user_errno = 0;
        if (getuid() == 0) {
            struct passwd *pw = getpwnam("icecc");

            if (pw) {
                user_uid = pw->pw_uid;
                user_gid = pw->pw_gid;
            } else {
                warn_icecc_user_errno = errno ? errno : ENOENT; // apparently errno can be 0 on error here
                user_uid = 65534;
                user_gid = 65533;
            }
        } else {
            user_uid = getuid();
            user_gid = getgid();
        }

        envbasedir = "/var/tmp/icecc-envs";
        tcp_listen_fd = -1;
        tcp_listen_local_fd = -1;
        unix_listen_fd = -1;
        new_client_id = 0;
        next_scheduler_connect = 0;
        cache_size = 0;
        noremote = false;
        custom_nodename = false;
        icecream_load = 0;
        icecream_usage.tv_sec = icecream_usage.tv_usec = 0;
        current_load = - 1000;
        num_cpus = 0;
        scheduler = nullptr;
        discover = nullptr;
        scheduler_port = 8765;
        daemon_interface = "";
        daemon_port = 10245;
        max_scheduler_pong = MAX_SCHEDULER_PONG;
        max_scheduler_ping = MAX_SCHEDULER_PING;
        current_kids = 0;
        accept_errors_total = 0;
        accept_emfile_errors = 0;
        last_accept_errno = 0;
        last_accept_errno_ts = 0;
        state_dump_interval_s = 0;
        state_dump_log = false;
        next_state_dump_msec = 0;
        state_dump_worst_clients = 10;
        waitforcs_use_cs_hist.assign(waitforcs_latency_bucket_count + 1, 0);
        waitforcs_no_cs_hist.assign(waitforcs_latency_bucket_count + 1, 0);
        waitforcs_use_cs_samples = 0;
        waitforcs_no_cs_samples = 0;
        waitforcs_use_cs_sum_msec = 0;
        waitforcs_no_cs_sum_msec = 0;
        waitforcs_use_cs_max_msec = 0;
        waitforcs_no_cs_max_msec = 0;
        webgui_enabled = false;
        webgui_port = 8768;
        webgui_addr = "127.0.0.1";
        webgui_best_effort = false;
        web_listen_fd = -1;
        job_history_capacity = 20000;
        next_job_history_seq = 1;
    }

    ~Daemon() {
        delete discover;
    }

    bool reannounce_environments() __attribute_warn_unused_result__;
    void answer_client_requests();
    bool handle_transfer_env(Client *client, EnvTransferMsg *msg) __attribute_warn_unused_result__;
    bool handle_env_install_child_done(Client *client);
    bool finish_transfer_env(Client *client, bool cancel = false);
    bool handle_get_native_env(Client *client, GetNativeEnvMsg *msg) __attribute_warn_unused_result__;
    bool finish_get_native_env(Client *client, string env_key);
    void handle_old_request();
    bool handle_compile_file(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_activity(Client *client) __attribute_warn_unused_result__;
    bool handle_file_chunk_env(Client *client, Msg *msg) __attribute_warn_unused_result__;
    void handle_end(Client *client, int exitcode);
    int scheduler_get_internals() __attribute_warn_unused_result__;
    void clear_children();
    int scheduler_use_cs(UseCSMsg *msg) __attribute_warn_unused_result__;
    int scheduler_no_cs(NoCSMsg *msg) __attribute_warn_unused_result__;
    bool handle_get_cs(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_local_job(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_job_done(Client *cl, JobDoneMsg *m) __attribute_warn_unused_result__;
    bool handle_job_timing(Client *client, JobTimingMsg *m) __attribute_warn_unused_result__;
    bool handle_compile_done(Client *client) __attribute_warn_unused_result__;
    bool handle_verify_env(Client *client, VerifyEnvMsg *msg) __attribute_warn_unused_result__;
    bool handle_blacklist_host_env(Client *client, Msg *msg) __attribute_warn_unused_result__;
    int handle_cs_conf(ConfCSMsg *msg);
    string dump_internals() const;
    string determine_nodename();
    void determine_system();
    void determine_supported_features();
    bool maybe_stats(bool force_check = false);
    bool send_scheduler(const Msg &msg) __attribute_warn_unused_result__;
    void record_waitforcs_latency(bool use_cs, uint64_t latency_msec);
    void close_scheduler();
    bool reconnect();
    int working_loop();
    bool setup_listen_fds();
    bool setup_listen_tcp_fd( int& fd, const string& interface );
    bool setup_listen_unix_fd();
    bool setup_web_listen_fd();
    void close_web();
    void handle_web_accept();
    void handle_web_connection(int fd, short revents);
    void drop_web_connection(int fd);
    string webgui_html() const;
    string webgui_insights_html() const;
    string webgui_insights_jobs_html() const;
    string dump_clients_json() const;
    string dump_job_history_json(size_t limit, uint64_t before_seq = 0) const;
    string dump_insights_series_json(size_t minutes);
    string dump_insights_jobs_json(time_t minute_ts, size_t limit);
    void remember_finished_job(const Client *client, int exitcode);
    void update_insights_history(const JobHistoryEntry &entry);
    void prune_insights_history(time_t now_ts);
    static bool should_track_client_job(const Client *client);
    static bool parse_http_request(const string &request, string &method, string &path);
    static size_t parse_jobs_limit(const string &path);
    static bool parse_query_u64(const string &path, const char *key, uint64_t *value);
    bool queue_web_response(int fd, int status_code, const char *status_text,
                            const char *content_type, const string &body) __attribute_warn_unused_result__;
    void note_accept_error(const char *where, int err);
    void check_cache_size(const string &new_env);
    void remove_native_environment(const string& env_key);
    void remove_environment(const string& env_key);
    bool create_env_finished(string env_key);

    void maybe_dump_state();
    std::string dump_state_json() const;
    /* Telemetry output must never gate job control: a slow, full or
       transiently unavailable filesystem would otherwise delay client
       handling, child reaping and scheduler traffic on every completed job.
       Lines are queued in bounded memory and drained from the main loop
       under a strict per-iteration budget through one persistent
       descriptor; when storage cannot keep up the OLDEST records are
       dropped and counted, because compile scheduling is the higher-
       priority correctness path.  */
    bool append_state_jsonl_line(const std::string &line);
    AsyncJsonlWriter state_writer;
};

bool Daemon::setup_listen_fds()
{
    tcp_listen_fd = -1;
    tcp_listen_local_fd = -1;
    unix_listen_fd = -1;

    if (!noremote) { // if we only listen to local clients, there is no point in going TCP
        if( !setup_listen_tcp_fd( tcp_listen_fd, daemon_interface ))
            return false;
        // We should always listen on the loopback interface, so if we're binding only
        // to a specific interface, bind also to the loopback.
        if( !daemon_interface.empty()) {
            if( !setup_listen_tcp_fd( tcp_listen_local_fd, "lo" ))
                return false;
        }
    }
    if( !setup_listen_unix_fd())
        return false;
    if (!setup_web_listen_fd())
        return false;
    return true;
}

bool Daemon::setup_listen_tcp_fd( int& fd, const string& interface )
{
    if( !interface.empty())
        trace() << "starting to listen on interface " << interface << endl;
    else
        trace() << "starting to listen on all interfaces" << endl;

    if ((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        log_perror("Failed to create TCP listen socket.");
        return false;
    }

    int optval = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        log_perror("Failed to set 'Reuse Address(SO_REUSEADDR)' option on TCP Listen Socket");
        return false;
    }

    struct sockaddr_in myaddr;
    if (!build_address_for_interface(myaddr, interface, daemon_port)) {
        return false;
    }

    int count = 5;
    while (count) {
        if (::bind(fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0) {
            log_perror("Failed to bind address to TCP listen socket");
            sleep(2);
            if (!--count) {
                return false;
            }
            continue;
        } else {
            break;
        }
    }

    if (listen(fd, 1024) < 0) {
        log_perror("Failed to set TCP socket for listening to incoming connections");
        return false;
    }

    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return true;
}

bool Daemon::setup_listen_unix_fd()
{
    if ((unix_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        log_perror("Failed to create a Unix scoket for listening");
        return false;
    }

    struct sockaddr_un myaddr;

    memset(&myaddr, 0, sizeof(myaddr));

    myaddr.sun_family = AF_UNIX;

    bool reset_umask = false;
    mode_t old_umask = 0;

    if (getenv("ICECC_TEST_SOCKET") == nullptr) {
#ifdef HAVE_LIBCAP_NG
        // We run as system daemon (UID has been already changed).
        if (capng_have_capability( CAPNG_EFFECTIVE, CAP_SYS_CHROOT )) {
#else
        if (getuid() == 0) {
#endif
            string default_socket = "/var/run/icecc/iceccd.socket";
            strncpy(myaddr.sun_path, default_socket.c_str() , sizeof(myaddr.sun_path) - 1);
            myaddr.sun_path[sizeof(myaddr.sun_path) - 1] = '\0';
            if(default_socket.length() > sizeof(myaddr.sun_path) - 1) {
                log_error() << "default socket path too long for sun_path" << endl;
            }
            if (-1 == unlink(myaddr.sun_path) && errno != ENOENT){
                log_perror("unlink failed") << "\t" << myaddr.sun_path << endl;
            }
            old_umask = umask(0);
            reset_umask = true;
        } else { // Started by user.
            if( getenv( "HOME" )) {
                string socket_path = getenv("HOME");
                socket_path.append("/.iceccd.socket");
                strncpy(myaddr.sun_path, socket_path.c_str(), sizeof(myaddr.sun_path) - 1);
                myaddr.sun_path[sizeof(myaddr.sun_path) - 1] = '\0';
                if(socket_path.length() > sizeof(myaddr.sun_path) - 1) {
                    log_error() << "$HOME/.iceccd.socket path too long for sun_path" << endl;
                }
                if (-1 == unlink(myaddr.sun_path) && errno != ENOENT){
                    log_perror("unlink failed") << "\t" << myaddr.sun_path << endl;
                }
            } else {
                log_error() << "launched by user, but $HOME not set" << endl;
                return false;
            }
        }
    } else {
        string test_socket = getenv("ICECC_TEST_SOCKET");
        strncpy(myaddr.sun_path, test_socket.c_str(), sizeof(myaddr.sun_path) - 1);
        myaddr.sun_path[sizeof(myaddr.sun_path) - 1] = '\0';
        if(test_socket.length() > sizeof(myaddr.sun_path) - 1) {
            log_error() << "$ICECC_TEST_SOCKET path too long for sun_path" << endl;
        }
        if (-1 == unlink(myaddr.sun_path) && errno != ENOENT){
            log_perror("unlink failed") << "\t" << myaddr.sun_path << endl;
        }
        old_umask = umask(0);
        reset_umask = true;
    }

    if (::bind(unix_listen_fd, (struct sockaddr*)&myaddr, sizeof(myaddr)) < 0) {
        log_perror("Failed to bind address to unix listen socket");

        if (reset_umask) {
            umask(old_umask);
        }

        return false;
    }

    if (reset_umask) {
        umask(old_umask);
    }

    if (listen(unix_listen_fd, 1024) < 0) {
        log_perror("Failed to set unix socket for listening");
        return false;
    }

    fcntl(unix_listen_fd, F_SETFD, FD_CLOEXEC);

    return true;
}

bool Daemon::setup_web_listen_fd()
{
    if (!webgui_enabled) {
        return true;
    }

    if (web_listen_fd != -1) {
        return true;
    }

    auto parse_webgui_bind_addr = [](const string &input, in_addr &out_addr, string &normalized) {
        string candidate = input;
        if (candidate.empty() || candidate == "localhost") {
            candidate = "127.0.0.1";
        }
        if (inet_pton(AF_INET, candidate.c_str(), &out_addr) != 1) {
            return false;
        }
        normalized = candidate;
        return true;
    };

    web_listen_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (web_listen_fd < 0) {
        if (webgui_best_effort) {
            log_warning() << "Failed to create web gui socket: " << strerror(errno) << endl;
            webgui_enabled = false;
            return true;
        }
        log_perror("Failed to create web gui socket");
        return false;
    }

    int optval = 1;
    if (setsockopt(web_listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        if (webgui_best_effort) {
            log_warning() << "Failed to set SO_REUSEADDR on web gui socket: " << strerror(errno) << endl;
        } else {
            log_perror("Failed to set SO_REUSEADDR on web gui socket");
        }
        if (-1 == close(web_listen_fd) && (errno != EBADF)) {
            log_perror("Failed to close web gui socket");
        }
        web_listen_fd = -1;
        if (webgui_best_effort) {
            webgui_enabled = false;
            return true;
        }
        return false;
    }

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(webgui_port);
    string normalized_addr;
    if (!parse_webgui_bind_addr(webgui_addr, addr.sin_addr, normalized_addr)) {
        if (webgui_best_effort) {
            log_warning() << "Invalid web gui bind address '" << webgui_addr
                          << "'. Use an IPv4 address or 'localhost'." << endl;
        } else {
            log_error() << "Invalid web gui bind address '" << webgui_addr
                        << "'. Use an IPv4 address or 'localhost'." << endl;
        }
        if (-1 == close(web_listen_fd) && (errno != EBADF)) {
            log_perror("Failed to close web gui socket");
        }
        web_listen_fd = -1;
        if (webgui_best_effort) {
            webgui_enabled = false;
            return true;
        }
        return false;
    }
    webgui_addr = normalized_addr;

    if (::bind(web_listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        if (webgui_best_effort) {
            log_warning() << "Failed to bind web gui on " << webgui_addr << ":" << webgui_port
                          << ": " << strerror(errno) << endl;
        } else {
            log_error() << "Failed to bind web gui on " << webgui_addr << ":" << webgui_port
                        << ": " << strerror(errno) << endl;
        }
        if (-1 == close(web_listen_fd) && (errno != EBADF)) {
            log_perror("Failed to close web gui socket");
        }
        web_listen_fd = -1;
        if (webgui_best_effort) {
            webgui_enabled = false;
            return true;
        }
        return false;
    }

    if (listen(web_listen_fd, 128) < 0) {
        if (webgui_best_effort) {
            log_warning() << "Failed to listen on web gui socket: " << strerror(errno) << endl;
        } else {
            log_perror("Failed to listen on web gui socket");
        }
        if (-1 == close(web_listen_fd) && (errno != EBADF)) {
            log_perror("Failed to close web gui socket");
        }
        web_listen_fd = -1;
        if (webgui_best_effort) {
            webgui_enabled = false;
            return true;
        }
        return false;
    }

    fcntl(web_listen_fd, F_SETFD, FD_CLOEXEC);
    int flags = fcntl(web_listen_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(web_listen_fd, F_SETFL, flags | O_NONBLOCK);
    }

    log_info() << "web gui listening on http://" << webgui_addr << ":" << webgui_port << endl;
    return true;
}

void Daemon::close_web()
{
    for (auto it = web_connections.begin(); it != web_connections.end(); ++it) {
        if (-1 == close(it->first) && (errno != EBADF)) {
            log_perror("Failed to close web gui client socket");
        }
    }
    web_connections.clear();

    if (web_listen_fd != -1) {
        if (-1 == close(web_listen_fd) && (errno != EBADF)) {
            log_perror("Failed to close web gui listen socket");
        }
        web_listen_fd = -1;
    }
}

void Daemon::drop_web_connection(int fd)
{
    if (-1 == close(fd) && (errno != EBADF)) {
        log_perror("Failed to close web gui connection");
    }
    web_connections.erase(fd);
}

void Daemon::note_accept_error(const char *where, int err)
{
    ++accept_errors_total;
    if (err == EMFILE || err == ENFILE) {
        ++accept_emfile_errors;
    }
    last_accept_errno = err;
    last_accept_errno_ts = time(nullptr);

    static uint64_t last_log_msec = 0;
    const uint64_t now_msec = monotonic_msec();
    /* EMFILE/ENFILE get a tighter window rather than an exemption: under fd
       exhaustion this path can run every loop iteration, and the fd
       snapshot + log line below are exactly the kind of work a starved
       daemon cannot afford at that rate.  Counters above stay exact.  */
    const uint64_t window_msec = (err == EMFILE || err == ENFILE) ? 1000 : 5000;
    if (now_msec - last_log_msec < window_msec) {
        return;
    }
    last_log_msec = now_msec;

    const FdSnapshot fd = collect_fd_snapshot();
    ostringstream msg;
    msg << "accept failed (" << where << "): " << strerror(err) << " (errno " << err << ")";
    if (fd.open_count >= 0) {
        msg << ", open_fds=" << fd.open_count;
        if (fd.soft_limit != RLIM_INFINITY) {
            msg << ", nofile_soft_limit=" << (unsigned long long)fd.soft_limit;
        }
    }
    log_error() << msg.str() << endl;
}

/* Returns false if the connection was dropped (erased) -- callers hold a
   reference into web_connections and MUST return immediately, or they use
   a destroyed object.  */
bool Daemon::queue_web_response(int fd, int status_code, const char *status_text,
                                const char *content_type, const string &body)
{
    auto it = web_connections.find(fd);
    if (it == web_connections.end()) {
        return false;
    }

    if (it->second.response_started
            && it->second.outbuf_ofs < it->second.outbuf.size()) {
        /* Part of an earlier response is already on the wire; replacing the
           rest would splice two HTTP responses into one stream.  The
           connection is unrecoverable -- drop it.  */
        drop_web_connection(fd);
        return false;
    }

    ostringstream out;
    out << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    out << "Content-Type: " << content_type << "\r\n";
    out << "Cache-Control: no-store\r\n";
    out << "Connection: close\r\n";
    out << "Content-Length: " << body.size() << "\r\n";
    out << "\r\n";
    out << body;

    it->second.outbuf = out.str();
    it->second.outbuf_ofs = 0;
    it->second.response_started = false;
    it->second.close_after_write = true;
    /* One request per connection: stop reading as soon as a response is
       queued, so no further input can grow or race the write path.  */
    it->second.reject_input = true;
    return true;
}

bool Daemon::parse_http_request(const string &request, string &method, string &path)
{
    size_t line_end = request.find("\r\n");
    if (line_end == string::npos) {
        line_end = request.find('\n');
    }
    if (line_end == string::npos) {
        return false;
    }

    string line = request.substr(0, line_end);
    istringstream in(line);
    string version;
    if (!(in >> method >> path >> version)) {
        return false;
    }
    return true;
}

size_t Daemon::parse_jobs_limit(const string &path)
{
    /* A page is bounded independently of retention: at the 4 KiB display cap
       a full 20,000-entry dump reached ~91 MB and hundreds of milliseconds
       of synchronous work in the daemon's only loop.  Deeper history is
       reachable by paging with ?before=<seq>.  */
    static const size_t kDefaultLimit = 200;
    static const size_t kMaxLimit = 1000;

    size_t query_pos = path.find('?');
    if (query_pos == string::npos) {
        return kDefaultLimit;
    }
    string query = path.substr(query_pos + 1);

    size_t limit_pos = query.find("limit=");
    if (limit_pos == string::npos) {
        return kDefaultLimit;
    }

    const char *limit_text = query.c_str() + limit_pos + strlen("limit=");
    char *end = nullptr;
    unsigned long limit = strtoul(limit_text, &end, 10);
    if (end == limit_text) {
        return kDefaultLimit;
    }

    if (limit == 0) {
        return kDefaultLimit;
    }

    if (limit > kMaxLimit) {
        limit = kMaxLimit;
    }
    return size_t(limit);
}

bool Daemon::parse_query_u64(const string &path, const char *key, uint64_t *value)
{
    if (!key || !*key || !value) {
        return false;
    }

    const size_t query_pos = path.find('?');
    if (query_pos == string::npos) {
        return false;
    }
    const string query = path.substr(query_pos + 1);
    const string needle = string(key) + "=";
    size_t pos = 0;
    while (pos < query.size()) {
        const size_t amp = query.find('&', pos);
        if (query.compare(pos, needle.size(), needle) == 0) {
            const char *begin = query.c_str() + pos + needle.size();
            char *parse_end = nullptr;
            const unsigned long long parsed = strtoull(begin, &parse_end, 10);
            if (parse_end == begin) {
                return false;
            }
            *value = parsed;
            return true;
        }
        if (amp == string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return false;
}

string Daemon::webgui_html() const
{
    return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>iceccd web gui</title>
  <style>
    :root {
      --bg-0: #06090f;
      --bg-1: #11192a;
      --bg-card: rgba(12, 21, 37, 0.86);
      --fg: #eff6ff;
      --fg-dim: #96acc7;
      --border: #2a3a53;
      --good: #22c55e;
      --warn: #f59e0b;
      --bad: #ef4444;
      --info: #3b82f6;
      --violet: #8b5cf6;
      --teal: #14b8a6;
      --shadow: 0 14px 36px rgba(2, 8, 23, 0.55);
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      color: var(--fg);
      font-family: "JetBrains Mono", "IBM Plex Mono", "SFMono-Regular", Menlo, Consolas, monospace;
      background:
        radial-gradient(1400px 680px at 8% -20%, rgba(20, 184, 166, 0.22), transparent 65%),
        radial-gradient(1200px 640px at 88% -25%, rgba(139, 92, 246, 0.24), transparent 68%),
        linear-gradient(160deg, var(--bg-1), var(--bg-0));
      min-height: 100vh;
    }
    .shell {
      max-width: 1700px;
      margin: 0 auto;
      padding: 18px 16px 22px;
    }
    .header {
      display: flex;
      justify-content: space-between;
      gap: 12px;
      align-items: baseline;
      flex-wrap: wrap;
      margin-bottom: 10px;
    }
    .title {
      margin: 0;
      font-size: 24px;
      letter-spacing: 0.4px;
      color: #f8fbff;
      text-shadow: 0 3px 12px rgba(0, 0, 0, 0.33);
    }
    .meta-line {
      margin-top: 3px;
      color: var(--fg-dim);
      font-size: 12px;
    }
    .chip {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      border: 1px solid var(--border);
      border-radius: 999px;
      padding: 6px 12px;
      font-size: 12px;
      color: var(--fg-dim);
      background: rgba(9, 15, 28, 0.65);
    }
    .dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: var(--warn);
      box-shadow: 0 0 8px rgba(245, 158, 11, 0.6);
    }
    .dot.good {
      background: var(--good);
      box-shadow: 0 0 9px rgba(34, 197, 94, 0.7);
    }
    .metrics {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(205px, 1fr));
      gap: 10px;
    }
    .card {
      border: 1px solid var(--border);
      border-radius: 13px;
      padding: 12px 12px 10px;
      background: var(--bg-card);
      box-shadow: var(--shadow);
      backdrop-filter: blur(3px);
    }
    .label {
      color: var(--fg-dim);
      text-transform: uppercase;
      font-size: 11px;
      letter-spacing: 0.4px;
    }
    .value {
      margin-top: 4px;
      font-size: 22px;
      font-weight: 700;
      line-height: 1.1;
      color: #f8fbff;
    }
    .value.small {
      font-size: 17px;
    }
    .good { color: var(--good); }
    .warn { color: var(--warn); }
    .bad { color: var(--bad); }
    .info { color: #8db8ff; }
    .layout {
      display: grid;
      grid-template-columns: 0.95fr 1.05fr;
      gap: 12px;
      margin-top: 12px;
    }
    .panel {
      border: 1px solid var(--border);
      border-radius: 13px;
      background: rgba(8, 14, 24, 0.82);
      box-shadow: var(--shadow);
      overflow: hidden;
    }
    .panel-head {
      padding: 10px 12px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 10px;
      border-bottom: 1px solid rgba(44, 61, 84, 0.68);
      background: linear-gradient(180deg, rgba(18, 31, 50, 0.75), rgba(9, 17, 29, 0.75));
    }
    .panel-title {
      margin: 0;
      font-size: 13px;
      letter-spacing: 0.3px;
      text-transform: uppercase;
      color: #b8c8dc;
    }
    .panel-sub {
      font-size: 11px;
      color: var(--fg-dim);
    }
    .content {
      padding: 10px 12px 12px;
    }
    .bars {
      display: grid;
      gap: 6px;
    }
    .bar-row {
      display: grid;
      grid-template-columns: 140px 1fr 44px;
      gap: 8px;
      align-items: center;
      font-size: 11px;
    }
    .bar-name {
      color: #bdd0ea;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .bar-wrap {
      height: 9px;
      border-radius: 999px;
      background: rgba(42, 58, 83, 0.65);
      border: 1px solid rgba(53, 74, 103, 0.75);
      overflow: hidden;
    }
    .bar-fill {
      height: 100%;
      background: linear-gradient(90deg, var(--teal), var(--violet));
      box-shadow: 0 0 8px rgba(20, 184, 166, 0.32);
    }
    .bar-val {
      text-align: right;
      color: #a8bfd8;
      font-size: 10px;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 11px;
    }
    th, td {
      text-align: left;
      padding: 7px 8px;
      border-bottom: 1px solid rgba(41, 58, 81, 0.72);
      vertical-align: top;
    }
    th {
      position: sticky;
      top: 0;
      background: #111e31;
      color: #cbdaec;
      z-index: 1;
      font-weight: 700;
    }
    tbody tr:nth-child(even) {
      background: rgba(10, 16, 30, 0.42);
    }
    tbody tr.clickable-row {
      cursor: pointer;
    }
    tbody tr.clickable-row:hover {
      background: rgba(33, 53, 82, 0.66);
    }
    td.dim {
      color: var(--fg-dim);
    }
    .status {
      font-weight: 700;
      text-transform: lowercase;
    }
    .scroll {
      max-height: 355px;
      overflow: auto;
    }
    .control {
      display: inline-flex;
      align-items: center;
      gap: 7px;
      color: var(--fg-dim);
      font-size: 11px;
    }
    select {
      background: rgba(9, 15, 26, 0.96);
      color: #dbeafe;
      border: 1px solid #3b5273;
      border-radius: 7px;
      padding: 2px 5px;
      font-family: inherit;
      font-size: 11px;
    }
    .modal {
      position: fixed;
      inset: 0;
      background: rgba(2, 6, 14, 0.72);
      display: grid;
      place-items: center;
      z-index: 1000;
      padding: 16px;
    }
    .modal.hidden {
      display: none;
    }
    .modal-card {
      width: min(1100px, 100%);
      max-height: min(82vh, 720px);
      border: 1px solid var(--border);
      border-radius: 12px;
      background: rgba(10, 17, 29, 0.98);
      box-shadow: 0 20px 46px rgba(2, 8, 23, 0.72);
      display: grid;
      grid-template-rows: auto auto 1fr;
      overflow: hidden;
    }
    .modal-head {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 10px 12px;
      border-bottom: 1px solid rgba(44, 61, 84, 0.7);
      background: linear-gradient(180deg, rgba(18, 31, 50, 0.8), rgba(9, 17, 29, 0.8));
      color: #c4d5eb;
      text-transform: uppercase;
      letter-spacing: 0.3px;
      font-size: 12px;
      font-weight: 700;
    }
    .modal-close {
      border: 1px solid #3b5273;
      background: rgba(9, 15, 26, 0.96);
      color: #dbeafe;
      border-radius: 7px;
      cursor: pointer;
      padding: 2px 9px;
      font-family: inherit;
      font-size: 16px;
      line-height: 1.1;
    }
    .modal-sub {
      padding: 8px 12px;
      color: var(--fg-dim);
      border-bottom: 1px solid rgba(36, 53, 77, 0.72);
      font-size: 11px;
    }
    .modal-body {
      margin: 0;
      padding: 10px 12px 12px;
      overflow: auto;
      white-space: pre-wrap;
      word-break: break-word;
      color: #e5eefc;
      font-size: 12px;
      line-height: 1.4;
    }
    @media (max-width: 1180px) {
      .layout {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <div class="shell">
    <div class="header">
      <div>
        <h1 class="title">iceccd live dashboard</h1>
        <div class="meta-line" id="meta">connecting...</div>
      </div>
      <div style="display:flex; gap:8px; align-items:center;">
        <a href="/insights" class="chip" style="text-decoration:none; color:#dbeafe;">insights</a>
        <div class="chip">
          <span class="dot" id="scheduler-dot"></span>
          <span id="scheduler-chip">scheduler: unknown</span>
        </div>
      </div>
    </div>

    <div class="metrics">
      <div class="card"><div class="label">Scheduler</div><div class="value small" id="scheduler">-</div></div>
      <div class="card"><div class="label">Compile slots</div><div class="value" id="slots">-</div></div>
      <div class="card"><div class="label">Preprocess slots</div><div class="value" id="preprocess-slots">-</div></div>
      <div class="card"><div class="label">Connected clients</div><div class="value info" id="clients-total">-</div></div>
      <div class="card"><div class="label">FD usage</div><div class="value" style="color:#fbbf24" id="fds">-</div></div>
      <div class="card"><div class="label">Waiting for scheduler</div><div class="value warn" id="waitforcs">-</div></div>
      <div class="card"><div class="label">Waiting remote compile</div><div class="value" style="color:#a78bfa" id="waitcompile">-</div></div>
      <div class="card"><div class="label">Local queue</div><div class="value" style="color:#60a5fa" id="tocompile">-</div></div>
      <div class="card"><div class="label">Local child running</div><div class="value good" id="waitforchild">-</div></div>
      <div class="card"><div class="label">Current load</div><div class="value" id="load">-</div></div>
      <div class="card"><div class="label">Queue delay (ms)</div><div class="value small" id="queue-delay">-</div></div>
      <div class="card"><div class="label">Wait-for-CS (ms)</div><div class="value small" id="waitforcs-ms">-</div></div>
      <div class="card"><div class="label">Compile exec (ms)</div><div class="value small" id="exec-ms">-</div></div>
      <div class="card"><div class="label">Queue by mode p95</div><div class="value small" id="queue-mode">-</div></div>
      <div class="card"><div class="label">Exec by mode p95</div><div class="value small" id="exec-mode">-</div></div>
      <div class="card"><div class="label">Timing coverage / rate</div><div class="value small" id="timing-coverage">-</div></div>
    </div>

    <div class="layout">
      <div class="panel">
        <div class="panel-head">
          <h2 class="panel-title">Status Distribution</h2>
          <div class="panel-sub">share of live clients</div>
        </div>
        <div class="content">
          <div class="bars" id="status-bars"></div>
        </div>
      </div>
      <div class="panel">
        <div class="panel-head">
          <h2 class="panel-title">Running / Pending Clients</h2>
          <div class="panel-sub" id="clients-sub">0 rows</div>
        </div>
        <div class="scroll">
          <table>
            <thead>
              <tr><th>client</th><th>status</th><th>age(ms)</th><th>job</th><th>target/env</th><th>host</th><th>why/local_reason</th></tr>
            </thead>
            <tbody id="clients-body"></tbody>
          </table>
        </div>
      </div>
    </div>

    <div class="panel" style="margin-top:12px;">
      <div class="panel-head">
        <h2 class="panel-title">Recent Jobs (in-memory ring buffer)</h2>
        <div class="control">
          <label for="job-limit">rows</label>
          <select id="job-limit">
            <option value="200">200</option>
            <option value="500" selected>500</option>
            <option value="1000">1000</option>
                      </select>
          <span id="jobs-sub">0 rows</span>
        </div>
      </div>
      <div class="scroll" style="max-height:460px;">
        <table>
          <thead>
            <tr><th>seq</th><th>client</th><th>duration(ms)</th><th>timing src/mode</th><th>timing ms (q/s/f/w/e)</th><th>exit</th><th>final</th><th>scheduler/compile job</th><th>target/env</th><th>remote host</th><th>why</th></tr>
          </thead>
          <tbody id="jobs-body"></tbody>
        </table>
      </div>
    </div>
  </div>
  <div class="modal hidden" id="cmdline-modal" role="dialog" aria-modal="true" aria-label="job command line">
    <div class="modal-card">
      <div class="modal-head">
        <span>Job command line</span>
        <button type="button" class="modal-close" id="cmdline-close" aria-label="Close">×</button>
      </div>
      <div class="modal-sub" id="cmdline-sub">-</div>
      <pre class="modal-body" id="cmdline-body"></pre>
    </div>
  </div>
  <script>
    function fmt(value) { return value === null || value === undefined || value === "" ? "-" : String(value); }
    function setText(id, value) { document.getElementById(id).textContent = fmt(value); }
    function statusClass(status) {
      if (status === "waitforcs") return "warn";
      if (status === "waitcompile" || status === "clientwork" || status === "waitforchild") return "info";
      if (status === "jobdone") return "good";
      if (status === "unknown") return "bad";
      return "";
    }
    function statusLabel(status, row) {
      if (status === "linkjob") return "localjob(queue)";
      const why = row ? (row.why || row.final_why || "") : "";
      if (status === "clientwork" && row && (row.local_job || why === "handle_old_request: local job started")) {
        return "localjob(running)";
      }
      return status;
    }
    function whyLabel(row) {
      if (!row) return "-";
      const why = row.why || row.final_why || "";
      const localReason = row.local_reason || "";
      if (row.local_job) {
        return localReason ? `${why} [${localReason}]` : why;
      }
      return why;
    }
    function timingSummary(row) {
      if (!row) return "-";
      return `q:${fmt(row.enqueue_msec)} s:${fmt(row.start_compile_msec)} f:${fmt(row.finish_msec)} w:${fmt(row.waitforcs_msec)} e:${fmt(row.exec_msec)}`;
    }
    function quantile(values, p) {
      if (!values.length) return null;
      const sorted = [...values].sort((a, b) => a - b);
      const idx = Math.min(sorted.length - 1, Math.max(0, Math.floor((sorted.length - 1) * p)));
      return sorted[idx];
    }
    function summarizeMetric(values) {
      if (!values.length) return "-";
      const max = Math.max(...values);
      const p50 = quantile(values, 0.50);
      const p95 = quantile(values, 0.95);
      return `p50 ${p50} | p95 ${p95} | max ${max} | n=${values.length}`;
    }
    function summarizeModeP95(remoteValues, localValues) {
      const remoteP95 = remoteValues.length ? quantile(remoteValues, 0.95) : "-";
      const localP95 = localValues.length ? quantile(localValues, 0.95) : "-";
      return `r ${remoteP95} (${remoteValues.length}) | l ${localP95} (${localValues.length})`;
    }
    function makeCell(tr, text, className) {
      const td = document.createElement("td");
      td.textContent = fmt(text);
      if (className) td.className = className;
      tr.appendChild(td);
    }
    function showCmdline(row) {
      const modal = document.getElementById("cmdline-modal");
      const cmd = (row && row.cmdline) ? String(row.cmdline) : "";
      const fallback = (row && row.outfile) ? `outfile=${row.outfile}` : "";
      const status = row ? (row.status || row.final_status) : "";
      const timing = (row && row.timing_source)
        ? ` source=${fmt(row.timing_source)} mode=${fmt(row.mode)} timing=${timingSummary(row)}`
        : "";
      document.getElementById("cmdline-sub").textContent =
        `client=${fmt(row && row.client_id)} status=${fmt(status)} scheduler_job=${fmt(row && row.scheduler_job_id)}${timing}`;
      document.getElementById("cmdline-body").textContent =
        cmd || fallback || "Command line not available for this row.";
      modal.classList.remove("hidden");
    }
    function hideCmdline() {
      document.getElementById("cmdline-modal").classList.add("hidden");
    }
    function renderStatusBars(byStatus, total) {
      const bars = document.getElementById("status-bars");
      bars.innerHTML = "";
      const rows = [];
      for (const [name, meta] of Object.entries(byStatus)) {
        const count = Number(meta && meta.count || 0);
        if (!count) continue;
        rows.push({ name, count });
      }
      rows.sort((a, b) => b.count - a.count);
      if (!rows.length) {
        const empty = document.createElement("div");
        empty.className = "panel-sub";
        empty.textContent = "No active clients";
        bars.appendChild(empty);
        return;
      }
      for (const row of rows) {
        const pct = total > 0 ? (100 * row.count / total) : 0;
        const root = document.createElement("div");
        root.className = "bar-row";
        const name = document.createElement("div");
        name.className = "bar-name";
        name.textContent = statusLabel(row.name);
        const wrap = document.createElement("div");
        wrap.className = "bar-wrap";
        const fill = document.createElement("div");
        fill.className = "bar-fill";
        fill.style.width = `${Math.max(2, pct)}%`;
        wrap.appendChild(fill);
        const val = document.createElement("div");
        val.className = "bar-val";
        val.textContent = `${row.count} (${pct.toFixed(1)}%)`;
        root.appendChild(name);
        root.appendChild(wrap);
        root.appendChild(val);
        bars.appendChild(root);
      }
    }
    function renderClients(rows) {
      const body = document.getElementById("clients-body");
      body.innerHTML = "";
      const limit = 600;
      const clipped = rows.slice(0, limit);
      for (const row of clipped) {
        const tr = document.createElement("tr");
        tr.className = "clickable-row";
        tr.title = "Click to view command line";
        tr.addEventListener("click", () => showCmdline(row));
        const job = row.job || {};
        const usecs = row.usecs || {};
        makeCell(tr, row.client_id);
        makeCell(tr, statusLabel(row.status, row), `status ${statusClass(row.status)}`);
        makeCell(tr, row.age_msec);
        makeCell(tr, row.scheduler_job_id);
        makeCell(tr, `${fmt(job.target)} / ${fmt(job.env)}`);
        makeCell(tr, `${fmt(usecs.hostname)}:${fmt(usecs.port)}`);
        makeCell(tr, whyLabel(row), "dim");
        body.appendChild(tr);
      }
      setText("clients-sub", `${rows.length} rows`);
    }
    function renderJobs(rows) {
      const body = document.getElementById("jobs-body");
      body.innerHTML = "";
      for (const row of rows) {
        const tr = document.createElement("tr");
        tr.className = "clickable-row";
        tr.title = "Click to view command line";
        tr.addEventListener("click", () => showCmdline(row));
        makeCell(tr, row.seq);
        makeCell(tr, row.client_id);
        makeCell(tr, row.duration_msec);
        makeCell(tr, `${fmt(row.timing_source)} / ${fmt(row.mode)}`);
        makeCell(tr, timingSummary(row), "dim");
        makeCell(tr, row.exitcode);
        makeCell(tr, statusLabel(row.final_status, row), `status ${statusClass(row.final_status)}`);
        makeCell(tr, `${fmt(row.scheduler_job_id)} / ${fmt(row.compile_job_id)}`);
        makeCell(tr, `${fmt(row.target)} / ${fmt(row.environment)}`);
        makeCell(tr, `${fmt(row.usecs_host)}:${fmt(row.usecs_port)}`);
        makeCell(tr, row.final_why, "dim");
        body.appendChild(tr);
      }
      setText("jobs-sub", `${rows.length} rows`);
    }
    /* Fetches must not pile up or hang: a refresh runs every two seconds,
       so without a deadline and an in-flight guard a stalled daemon leaves
       the page silently showing old numbers while requests accumulate.  */
    let refreshInFlight = false;
    let lastSuccessMs = 0;
    async function fetchJson(url, timeoutMs) {
      const ctrl = new AbortController();
      const timer = setTimeout(() => ctrl.abort(), timeoutMs || 5000);
      try {
        const res = await fetch(url, { cache: "no-store", signal: ctrl.signal });
        if (!res.ok) {
          throw new Error(`HTTP ${res.status} for ${url}`);
        }
        return await res.json();
      } finally {
        clearTimeout(timer);
      }
    }
    function freshnessText() {
      if (!lastSuccessMs) {
        return "no successful refresh yet";
      }
      const age = Math.round((Date.now() - lastSuccessMs) / 1000);
      return age <= 5 ? `updated ${age}s ago` : `STALE: last update ${age}s ago`;
    }

    async function refresh() {
      if (refreshInFlight) {
        return;   // a previous refresh is still outstanding
      }
      refreshInFlight = true;
      try {
        const limit = Number(document.getElementById("job-limit").value || "500");
        const [state, clients, jobs] = await Promise.all([
          fetchJson("/api/state", 5000),
          fetchJson("/api/clients", 5000),
          fetchJson(`/api/jobs?limit=${limit}`, 8000)
        ]);
        lastSuccessMs = Date.now();

        const by = state.clients.by_status;
        const schedulerConnected = !!state.scheduler.connected;
        const topLocalReasons = Object.entries(((state.clients || {}).local_jobs || {}).by_reason || {})
          .sort((a, b) => Number(b[1] || 0) - Number(a[1] || 0))
          .slice(0, 3)
          .map(([name, count]) => `${name}:${count}`)
          .join(", ");
        const topWaitHosts = Object.entries((state.clients || {}).waitcompile_by_host || {})
          .sort((a, b) => Number((b[1] || {}).count || 0) - Number((a[1] || {}).count || 0))
          .slice(0, 3)
          .map(([host, meta]) => `${host}:${meta.count}`)
          .join(", ");
        const waitforcsCombined = ((state.waitforcs_latency_msec || {}).combined || {});
        let metaText = `${state.node} | ts=${state.ts} | ${freshnessText()}`;
        if (topLocalReasons) metaText += ` | local_reasons=${topLocalReasons}`;
        if (topWaitHosts) metaText += ` | waitcompile_hosts=${topWaitHosts}`;
        if (waitforcsCombined.samples) {
          metaText += ` | waitforcs_avg=${waitforcsCombined.avg_msec}ms`;
        }
        setText("meta", metaText);
        setText("scheduler", schedulerConnected ? state.scheduler.name : "disconnected");
        const compileUsed = Number((state.slots || {}).used || 0);
        const compileMax = Number((state.slots || {}).max_kids || 0);
        const preprocessUsed = Number((state.slots || {}).active_preprocesses || 0);
        const preprocessMax = Number((state.slots || {}).max_preprocess_kids || 0);
        setText("slots", `${compileUsed} / ${compileMax}`);
        setText("preprocess-slots", `${preprocessUsed} / ${preprocessMax}`);
        setText("clients-total", state.clients.total);
        if (state.fds && state.fds.soft_limit > 0) {
          setText("fds", `${state.fds.open} / ${state.fds.soft_limit} (${state.fds.util_pct}%)`);
        } else if (state.fds) {
          setText("fds", state.fds.open);
        } else {
          setText("fds", "-");
        }
        setText("waitforcs", by.waitforcs.count);
        setText("waitcompile", by.waitcompile.count);
        setText("tocompile", by.tocompile.count);
        setText("waitforchild", by.waitforchild.count);
        setText("load", state.stats.current_load);
        setText("scheduler-chip", schedulerConnected ? `scheduler: ${state.scheduler.name}` : "scheduler: disconnected");
        document.getElementById("scheduler-dot").className = schedulerConnected ? "dot good" : "dot";

        const queueDelay = [];
        const queueDelayRemote = [];
        const queueDelayLocal = [];
        const waitforcsTimes = [];
        const execTimes = [];
        const execTimesRemote = [];
        const execTimesLocal = [];
        const allJobs = jobs.jobs || [];
        let timedByClient = 0;
        let newestEndTs = 0;
        for (const row of allJobs) {
          const endTs = Number(row.end_ts);
          if (Number.isFinite(endTs) && endTs > newestEndTs) {
            newestEndTs = endTs;
          }
        }
        let jobsLastMin = 0;
        for (const row of (jobs.jobs || [])) {
          const mode = String(row.mode || "").toLowerCase();
          const isRemoteMode = mode.startsWith("remote");
          const waitforcs = Number(row.waitforcs_msec);
          const localQueue = Number(row.local_queue_msec);
          const exec = Number(row.exec_msec);
          let queueValue = 0;
          if (Number.isFinite(localQueue) && localQueue > 0) {
            queueValue = localQueue;
          } else if (Number.isFinite(waitforcs) && waitforcs > 0) {
            queueValue = waitforcs;
          }
          if (queueValue > 0) {
            queueDelay.push(queueValue);
            if (isRemoteMode) {
              queueDelayRemote.push(queueValue);
            } else {
              queueDelayLocal.push(queueValue);
            }
          }
          if (Number.isFinite(waitforcs) && waitforcs > 0) waitforcsTimes.push(waitforcs);
          if (Number.isFinite(exec) && exec > 0) {
            execTimes.push(exec);
            if (isRemoteMode) {
              execTimesRemote.push(exec);
            } else {
              execTimesLocal.push(exec);
            }
          }
          if (String(row.timing_source || "") === "client") {
            ++timedByClient;
          }
          const endTs = Number(row.end_ts);
          if (newestEndTs > 0 && Number.isFinite(endTs) && endTs >= newestEndTs - 60) {
            ++jobsLastMin;
          }
        }
        setText("queue-delay", summarizeMetric(queueDelay));
        setText("waitforcs-ms", summarizeMetric(waitforcsTimes));
        setText("exec-ms", summarizeMetric(execTimes));
        setText("queue-mode", summarizeModeP95(queueDelayRemote, queueDelayLocal));
        setText("exec-mode", summarizeModeP95(execTimesRemote, execTimesLocal));
        const coverage = allJobs.length ? Math.round(100 * timedByClient / allJobs.length) : 0;
        setText("timing-coverage", `${coverage}% client | ${jobsLastMin}/min`);

        renderStatusBars(by, Number(state.clients.total || 0));

        renderClients(clients.clients || []);
        renderJobs(jobs.jobs || []);
        if (jobs.truncated) {
          /* Say so rather than implying the table is the whole history.  */
          setText("meta", document.getElementById("meta").textContent
            + ` | showing ${jobs.returned} of ${jobs.size}`);
        }
      } catch (error) {
        /* Keep the failure and the age of the last good data both visible:
           previously only a metadata line changed while every card kept its
           stale numbers.  */
        setText("meta", `error: ${error} | ${freshnessText()}`);
      } finally {
        refreshInFlight = false;
      }
    }
    document.getElementById("cmdline-close").addEventListener("click", hideCmdline);
    document.getElementById("cmdline-modal").addEventListener("click", (event) => {
      if (event.target === event.currentTarget) {
        hideCmdline();
      }
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        hideCmdline();
      }
    });
    document.getElementById("job-limit").addEventListener("change", refresh);
    setInterval(refresh, 2000);
    refresh();
  </script>
</body>
</html>)HTML";
}

string Daemon::webgui_insights_html() const
{
    return string(R"HTML(<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>iceccd insights</title>
  <style>
    :root {
      --bg: #050b15;
      --panel: rgba(13, 22, 36, 0.94);
      --border: #2a3f5e;
      --text: #dbeafe;
      --dim: #91a7c6;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      background: radial-gradient(1300px 700px at 0% 0%, #111f37 0%, var(--bg) 58%);
      color: var(--text);
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", monospace;
      padding: 16px;
    }
    .top {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 12px;
      gap: 10px;
    }
    .title {
      margin: 0;
      font-size: 20px;
      letter-spacing: 0.25px;
      text-transform: uppercase;
    }
    .meta {
      color: var(--dim);
      font-size: 12px;
    }
    .btn {
      display: inline-flex;
      align-items: center;
      text-decoration: none;
      color: var(--text);
      border: 1px solid var(--border);
      border-radius: 9px;
      background: rgba(12, 22, 38, 0.95);
      padding: 6px 10px;
      font-size: 12px;
    }
    .cards {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(170px, 1fr));
      gap: 8px;
      margin-bottom: 10px;
    }
    .card {
      border: 1px solid var(--border);
      border-radius: 10px;
      background: var(--panel);
      padding: 8px 10px;
      min-height: 60px;
    }
    .label {
      color: var(--dim);
      font-size: 11px;
      text-transform: uppercase;
      letter-spacing: 0.35px;
    }
    .value {
      margin-top: 5px;
      font-size: 15px;
      color: #f8fbff;
    }
    .grid {
      display: grid;
      gap: 10px;
      grid-template-columns: repeat(2, minmax(0, 1fr));
    }
    .panel {
      border: 1px solid var(--border);
      border-radius: 10px;
      background: var(--panel);
      padding: 8px 10px 10px;
    }
    .panel h2 {
      margin: 0 0 6px;
      font-size: 12px;
      letter-spacing: 0.35px;
      color: #c7d7ef;
      text-transform: uppercase;
    }
    .panel .sub {
      color: var(--dim);
      font-size: 11px;
      margin-bottom: 6px;
    }
    canvas {
      width: 100%;
      height: 180px;
      display: block;
      border: 1px solid rgba(47, 70, 102, 0.65);
      border-radius: 8px;
      background: rgba(6, 12, 22, 0.8);
    }
    @media (max-width: 1100px) {
      .grid {
        grid-template-columns: 1fr;
      }
    }
  </style>
</head>
<body>
  <div class="top">
    <div>
      <h1 class="title">iceccd insights</h1>
      <div class="meta" id="meta">connecting...</div>
    </div>
    <a class="btn" href="/">back to dashboard</a>
  </div>

  <div class="cards">
    <div class="card"><div class="label">Compile Slots</div><div class="value" id="compile-slots">-</div></div>
    <div class="card"><div class="label">Preprocess Slots</div><div class="value" id="preprocess-slots">-</div></div>
    <div class="card"><div class="label">Wait Compile</div><div class="value" id="waitcompile">-</div></div>
    <div class="card"><div class="label">Pending UseCS</div><div class="value" id="pending-usecs">-</div></div>
    <div class="card"><div class="label">Local Queue</div><div class="value" id="local-queue">-</div></div>
    <div class="card"><div class="label">Latest jobs/min</div><div class="value" id="jobs-per-minute">-</div></div>
    <div class="card"><div class="label">Queue avg / Exec avg</div><div class="value" id="queue-exec-avg">-</div></div>
    <div class="card"><div class="label">Timing coverage</div><div class="value" id="timing-coverage">-</div></div>
  </div>

  <div class="grid">
    <div class="panel">
      <h2>Jobs per minute</h2>
      <div class="sub">click a point to open jobs for that minute</div>
      <canvas id="chart-jobs"></canvas>
    </div>
    <div class="panel">
      <h2>Queue / exec avg (ms)</h2>
      <div class="sub">per-minute averages from in-memory history</div>
      <canvas id="chart-latency"></canvas>
    </div>
    <div class="panel">
      <h2>Mode split and failures</h2>
      <div class="sub">remote/local/preprocess/fail jobs per minute</div>
      <canvas id="chart-modes"></canvas>
    </div>
    <div class="panel">
      <h2>Slot utilization (%)</h2>
      <div class="sub">live rolling trend</div>
      <canvas id="chart-slots"></canvas>
    </div>
  </div>

  <script>
    const slotHistory = [];
    const slotHistoryMax = 180;
    let jobsBuckets = [];
    let jobsPointMap = [];

    function fmt(v) {
      return v === null || v === undefined || Number.isNaN(v) ? "-" : String(v);
    }

    function setText(id, value) {
      document.getElementById(id).textContent = fmt(value);
    }

    function getCanvasContext(canvasId) {
      const canvas = document.getElementById(canvasId);
      const rect = canvas.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      const width = Math.max(360, Math.floor(rect.width * dpr));
      const height = Math.max(180, Math.floor(rect.height * dpr));
      canvas.width = width;
      canvas.height = height;
      const ctx = canvas.getContext("2d");
      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = "rgba(6,12,22,0.95)";
      ctx.fillRect(0, 0, width, height);
      return { canvas, ctx, dpr, width, height };
    }

    function drawLineChart(canvasId, data, lines, yMin, yMax, suffix, pointMap) {
      const { ctx, dpr, width, height } = getCanvasContext(canvasId);
      const padLeft = 44;
      const padRight = 8;
      const padTop = 10;
      const padBottom = 18;
      const plotW = Math.max(20, width - padLeft - padRight);
      const plotH = Math.max(20, height - padTop - padBottom);

      ctx.strokeStyle = "rgba(54,74,104,0.65)";
      ctx.lineWidth = 1;
      for (let i = 0; i <= 4; ++i) {
        const y = padTop + (plotH * i / 4);
        ctx.beginPath();
        ctx.moveTo(padLeft, y);
        ctx.lineTo(width - padRight, y);
        ctx.stroke();
      }

      const range = Math.max(1, yMax - yMin);
      const count = data.length;
      const xFor = (idx) => count <= 1 ? padLeft : padLeft + (plotW * idx / (count - 1));
      const yFor = (value) => padTop + plotH - (plotH * (value - yMin) / range);

      if (pointMap) {
        pointMap.length = 0;
      }

      lines.forEach((line, lineIndex) => {
        ctx.strokeStyle = line.color;
        ctx.lineWidth = 1.8 * dpr;
        ctx.beginPath();
        for (let i = 0; i < count; ++i) {
          const point = data[i];
          const v = Number(point[line.key] || 0);
          const x = xFor(i);
          const y = yFor(v);
          if (i === 0) ctx.moveTo(x, y);
          else ctx.lineTo(x, y);
          if (lineIndex === 0 && pointMap) {
            pointMap.push({ x, y, minute_ts: point.minute_ts, value: v });
          }
        }
        ctx.stroke();
      });

      ctx.fillStyle = "#95a8c4";
      ctx.font = `${11 * dpr}px ui-monospace,monospace`;
      ctx.fillText(`${Math.round(yMax)}${suffix}`, 4 * dpr, (padTop + 8 * dpr));
      ctx.fillText(`${Math.round(yMin)}${suffix}`, 4 * dpr, (padTop + plotH));

      let legendX = padLeft;
      const legendY = height - 5 * dpr;
      lines.forEach((line) => {
        ctx.fillStyle = line.color;
        ctx.fillRect(legendX, legendY - 8 * dpr, 10 * dpr, 2.5 * dpr);
        legendX += 14 * dpr;
        ctx.fillStyle = "#cbdaf0";
        ctx.fillText(line.label, legendX, legendY - 6 * dpr);
        legendX += (line.label.length * 7 + 14) * dpr;
      });
    }

    function seriesMax(data, keys, fallback) {
      let max = fallback;
      for (const row of data) {
        for (const key of keys) {
          const v = Number(row[key] || 0);
          if (v > max) max = v;
        }
      }
      return max;
    }

    function drawJobsChart() {
      const jobsMax = Math.max(5, seriesMax(jobsBuckets, ["jobs_total", "jobs_remote", "jobs_local"], 0));
      drawLineChart("chart-jobs", jobsBuckets, [
        { key: "jobs_total", color: "#4ade80", label: "total" },
        { key: "jobs_remote", color: "#60a5fa", label: "remote" },
        { key: "jobs_local", color: "#f59e0b", label: "local" }
      ], 0, jobsMax, "", jobsPointMap);
    }

    function drawLatencyChart() {
      const max = Math.max(10, seriesMax(jobsBuckets, ["queue_avg_msec", "exec_avg_msec", "waitforcs_avg_msec"], 0));
      drawLineChart("chart-latency", jobsBuckets, [
        { key: "queue_avg_msec", color: "#22d3ee", label: "queue_avg" },
        { key: "exec_avg_msec", color: "#34d399", label: "exec_avg" },
        { key: "waitforcs_avg_msec", color: "#f97316", label: "waitforcs_avg" }
      ], 0, max, "ms");
    }

    function drawModesChart() {
      const max = Math.max(5, seriesMax(jobsBuckets, ["jobs_preprocess", "jobs_failed"], 0));
      drawLineChart("chart-modes", jobsBuckets, [
        { key: "jobs_preprocess", color: "#fbbf24", label: "preprocess" },
        { key: "jobs_failed", color: "#ef4444", label: "failed" }
      ], 0, max, "");
    }

    function drawSlotsChart() {
      drawLineChart("chart-slots", slotHistory, [
        { key: "compile_pct", color: "#60a5fa", label: "compile" },
        { key: "preprocess_pct", color: "#fbbf24", label: "preprocess" }
      ], 0, 100, "%");
    }

    function bindJobsPointClick() {
      const canvas = document.getElementById("chart-jobs");
      canvas.addEventListener("click", (event) => {
        if (!jobsPointMap.length) return;
        const rect = canvas.getBoundingClientRect();
        const dpr = window.devicePixelRatio || 1;
        const x = (event.clientX - rect.left) * dpr;
        let best = null;
        let bestDist = Number.MAX_SAFE_INTEGER;
        for (const point of jobsPointMap) {
          const dist = Math.abs(point.x - x);
          if (dist < bestDist) {
            bestDist = dist;
            best = point;
          }
        }
        if (best && bestDist <= 12 * dpr) {
          window.location.href = `/insights-jobs?minute=${best.minute_ts}`;
        }
      });
    }

    async function refresh() {
      try {
        const [stateRes, seriesRes] = await Promise.all([
          fetch("/api/state", { cache: "no-store" }),
          fetch("/api/insights-series?minutes=100", { cache: "no-store" })
        ]);
        if (!stateRes.ok || !seriesRes.ok) {
          throw new Error(`HTTP ${stateRes.status}/${seriesRes.status}`);
        }
        const state = await stateRes.json();
        const series = await seriesRes.json();
        jobsBuckets = Array.isArray(series.buckets) ? series.buckets : [];

        const slots = state.slots || {};
        const by = ((state.clients || {}).by_status || {});
        const compileUsed = Number(slots.used || 0);
        const compileMax = Math.max(1, Number(slots.max_kids || 1));
        const preprocessUsed = Number(slots.active_preprocesses || 0);
        const preprocessMax = Math.max(1, Number(slots.max_preprocess_kids || 1));
        const waitCompile = Number(((by.waitcompile || {}).count) || 0);
        const pendingUseCs = Number(((by.pending_use_cs || {}).count) || 0);
        const localQueue = Number((((state.clients || {}).local_jobs || {}).queued) || 0);

        const latest = jobsBuckets.length ? jobsBuckets[jobsBuckets.length - 1] : {};
        setText("meta", `${state.node} | refreshed=${new Date().toLocaleTimeString()} | retention=${series.retention_minutes}m | graph=${series.graph_minutes}m`);
        setText("compile-slots", `${compileUsed} / ${compileMax} (${Math.round(100 * compileUsed / compileMax)}%)`);
        setText("preprocess-slots", `${preprocessUsed} / ${preprocessMax} (${Math.round(100 * preprocessUsed / preprocessMax)}%)`);
        setText("waitcompile", waitCompile);
        setText("pending-usecs", pendingUseCs);
        setText("local-queue", localQueue);
        setText("jobs-per-minute", latest.jobs_total || 0);
        setText("queue-exec-avg", `${latest.queue_avg_msec || 0} / ${latest.exec_avg_msec || 0} ms`);
        setText("timing-coverage", `${latest.timing_coverage_pct || 0}%`);

        slotHistory.push({
          minute_ts: Number(state.ts || 0),
          compile_pct: (100 * compileUsed / compileMax),
          preprocess_pct: (100 * preprocessUsed / preprocessMax)
        });
        if (slotHistory.length > slotHistoryMax) {
          slotHistory.splice(0, slotHistory.length - slotHistoryMax);
        }

        drawJobsChart();
        drawLatencyChart();
        drawModesChart();
        drawSlotsChart();
      } catch (error) {
        setText("meta", "error: " + error);
      }
    }

    bindJobsPointClick();
    setInterval(refresh, 2000);
    refresh();
  </script>
</body>
</html>)HTML");
}

string Daemon::webgui_insights_jobs_html() const
{
    return string(R"HTML(<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>iceccd insights jobs</title>
  <style>
    :root {
      --bg: #050b15;
      --panel: rgba(13, 22, 36, 0.94);
      --border: #2a3f5e;
      --text: #dbeafe;
      --dim: #91a7c6;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      background: radial-gradient(1200px 650px at 0% 0%, #111f37 0%, var(--bg) 58%);
      color: var(--text);
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", monospace;
      padding: 14px;
    }
    .top {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 10px;
      gap: 10px;
    }
    .title { margin: 0; font-size: 18px; text-transform: uppercase; letter-spacing: 0.25px; }
    .meta { color: var(--dim); font-size: 12px; }
    .btn {
      display: inline-flex;
      align-items: center;
      text-decoration: none;
      color: var(--text);
      border: 1px solid var(--border);
      border-radius: 9px;
      background: rgba(12, 22, 38, 0.95);
      padding: 6px 10px;
      font-size: 12px;
    }
    .panel {
      border: 1px solid var(--border);
      border-radius: 10px;
      background: var(--panel);
      overflow: hidden;
    }
    .scroll {
      overflow: auto;
      max-height: calc(100vh - 120px);
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 12px;
    }
    th, td {
      padding: 6px 8px;
      border-bottom: 1px solid rgba(39, 56, 80, 0.7);
      vertical-align: top;
      text-align: left;
      white-space: nowrap;
    }
    th {
      position: sticky;
      top: 0;
      background: rgba(17, 29, 47, 0.96);
      color: #bfd0e8;
      text-transform: uppercase;
      letter-spacing: 0.25px;
      font-size: 11px;
      z-index: 2;
    }
    td.cmdline {
      max-width: 760px;
      white-space: pre-wrap;
      word-break: break-word;
      color: #dce8f9;
    }
  </style>
</head>
<body>
  <div class="top">
    <div>
      <h1 class="title">insights jobs</h1>
      <div class="meta" id="meta">loading...</div>
    </div>
    <a class="btn" href="/insights">back to insights</a>
  </div>
  <div class="panel">
    <div class="scroll">
      <table>
        <thead>
          <tr>
            <th>seq</th><th>client</th><th>end</th><th>dur(ms)</th><th>mode</th><th>q/w/e(ms)</th><th>exit</th><th>status</th><th>scheduler/compile</th><th>host</th><th>target/env</th><th>cmdline</th>
          </tr>
        </thead>
        <tbody id="jobs-body"></tbody>
      </table>
    </div>
  </div>
  <script>
    function fmt(v) { return v === null || v === undefined || v === "" ? "-" : String(v); }
    function parseMinute() {
      const params = new URLSearchParams(window.location.search);
      const minute = Number(params.get("minute") || 0);
      return Number.isFinite(minute) && minute > 0 ? minute : 0;
    }
    function toTime(ts) {
      if (!ts) return "-";
      const d = new Date(ts * 1000);
      return d.toISOString().replace("T", " ").replace(".000Z", "Z");
    }
    async function refresh() {
      const minute = parseMinute();
      if (!minute) {
        document.getElementById("meta").textContent = "missing minute query parameter";
        return;
      }
      try {
        const res = await fetch(`/api/insights-jobs?minute=${minute}&limit=5000`, { cache: "no-store" });
        if (!res.ok) {
          throw new Error(`HTTP ${res.status}`);
        }
        const payload = await res.json();
        document.getElementById("meta").textContent =
          `minute=${toTime(payload.minute_ts)} | returned=${fmt(payload.returned)} | retention=${fmt(payload.retention_minutes)}m`;
        const body = document.getElementById("jobs-body");
        body.innerHTML = "";
        for (const row of (payload.jobs || [])) {
          const tr = document.createElement("tr");
          const q = Number(row.queue_msec || 0);
          const w = Number(row.waitforcs_msec || 0);
          const e = Number(row.exec_msec || 0);
          const cells = [
            row.seq,
            row.client_id,
            toTime(row.end_ts),
            row.duration_msec,
            row.mode,
            `${q}/${w}/${e}`,
            row.exitcode,
            row.final_status,
            `${fmt(row.scheduler_job_id)}/${fmt(row.compile_job_id)}`,
            `${fmt(row.usecs_host)}:${fmt(row.usecs_port)}`,
            `${fmt(row.target)}/${fmt(row.environment)}`
          ];
          for (const value of cells) {
            const td = document.createElement("td");
            td.textContent = fmt(value);
            tr.appendChild(td);
          }
          const cmdTd = document.createElement("td");
          cmdTd.className = "cmdline";
          cmdTd.textContent = fmt(row.cmdline);
          tr.appendChild(cmdTd);
          body.appendChild(tr);
        }
      } catch (error) {
        document.getElementById("meta").textContent = "error: " + error;
      }
    }
    refresh();
  </script>
</body>
</html>)HTML");
}

string Daemon::dump_clients_json() const
{
    const uint64_t now_msec = monotonic_msec();
    vector<pair<uint64_t, const Client *>> ordered;
    ordered.reserve(clients.size());

    for (const auto &it : clients) {
        const Client *client = it.second;
        ordered.push_back(make_pair(now_msec - client->status_since_msec, client));
    }

    sort(ordered.begin(), ordered.end(),
         [](const pair<uint64_t, const Client *> &a, const pair<uint64_t, const Client *> &b) {
             return a.first > b.first;
         });

    ostringstream o;
    o << "{";
    o << "\"type\":\"iceccd_clients\",";
    o << "\"ts\":" << (long long)time(nullptr) << ",";
    o << "\"mono_msec\":" << (unsigned long long)now_msec << ",";
    o << "\"total\":" << ordered.size() << ",";
    o << "\"clients\":[";

    for (size_t i = 0; i < ordered.size(); ++i) {
        const uint64_t age_msec = ordered[i].first;
        const Client *client = ordered[i].second;
        const bool local_job = (client->status == Client::LINKJOB)
                               || (client->status == Client::CLIENTWORK
                                   && client->status_why == "handle_old_request: local job started");
        const string local_job_kind = local_job
                                      ? local_job_kind_from_outfile(client->outfile, client->fulljob)
                                      : string();
        const string local_reason = local_job
                                    ? (client->local_reason.empty() ? string("unknown") : client->local_reason)
                                    : string();
        const string cmdline = client_command_line_for_display(client);
        if (i) {
            o << ",";
        }

        o << "{";
        o << "\"client_id\":" << client->client_id << ",";
        o << "\"status\":\"" << Client::status_str(client->status) << "\",";
        o << "\"age_msec\":" << (unsigned long long)age_msec << ",";
        o << "\"why\":\"" << json_escape(client->status_why) << "\",";
        o << "\"local_job\":" << (local_job ? "true" : "false") << ",";
        o << "\"local_job_kind\":\"" << json_escape(local_job_kind) << "\",";
        o << "\"local_reason\":\"" << json_escape(local_reason) << "\",";
        o << "\"cmdline\":\"" << json_escape(cmdline) << "\",";
        o << "\"scheduler_job_id\":" << client->last_known_job_id << ",";
        o << "\"last_waitforcs_msec\":" << (unsigned long long)client->last_waitforcs_msec << ",";
        o << "\"env_bytes_received\":" << (unsigned long long)client->env_bytes_received << ",";
        o << "\"job\":";
        if (client->job) {
            o << "{";
            o << "\"job_id\":" << client->job->jobID() << ",";
            o << "\"target\":\"" << json_escape(client->job->targetPlatform()) << "\",";
            o << "\"env\":\"" << json_escape(client->job->environmentVersion()) << "\"";
            o << "}";
        } else {
            o << "null";
        }
        o << ",";
        o << "\"usecs\":";
        if (client->usecsmsg) {
            o << "{";
            o << "\"hostname\":\"" << json_escape(client->usecsmsg->hostname) << "\",";
            o << "\"port\":" << client->usecsmsg->port << ",";
            o << "\"got_env\":" << (client->usecsmsg->got_env ? "true" : "false") << ",";
            o << "\"host_platform\":\"" << json_escape(client->usecsmsg->host_platform) << "\",";
            o << "\"matched_job_id\":" << client->usecsmsg->matched_job_id;
            o << "}";
        } else {
            o << "null";
        }
        o << ",";
        o << "\"outfile\":\"" << json_escape(client->outfile) << "\",";
        o << "\"channel\":\"" << json_escape(client->channel ? client->channel->dump() : string()) << "\"";
        o << "}";
    }

    o << "]";
    o << "}";
    return o.str();
}

bool Daemon::should_track_client_job(const Client *client)
{
    if (!client) {
        return false;
    }

    if (client->job || client->job_id || client->last_known_job_id || client->usecsmsg || !client->outfile.empty()) {
        return true;
    }

    switch (client->status) {
    case Client::PENDING_USE_CS:
    case Client::JOBDONE:
    case Client::LINKJOB:
    case Client::TOCOMPILE:
    case Client::WAITFORCS:
    case Client::WAITCOMPILE:
    case Client::CLIENTWORK:
    case Client::WAITFORCHILD:
        return true;
    default:
        return false;
    }
}

static uint32_t clamp_u32(uint64_t value)
{
    return value > 0xffffffffULL ? 0xffffffffU : uint32_t(value);
}

static void finalize_job_timing(JobHistoryEntry *entry, const Client *client)
{
    if (!entry || !client) {
        return;
    }

    if (!entry->client_timing) {
        entry->client_submit_ts = client->created_ts;
        entry->client_enqueue_msec = 0;
        entry->client_start_msec = clamp_u32(client->last_waitforcs_msec);
        entry->client_finish_msec = clamp_u32(entry->duration_msec);
        entry->client_waitforcs_msec = clamp_u32(client->last_waitforcs_msec);
        entry->client_local_queue_msec = entry->client_start_msec;
        if (entry->client_finish_msec < entry->client_start_msec) {
            entry->client_finish_msec = entry->client_start_msec;
        }
        entry->client_exec_msec = entry->client_finish_msec - entry->client_start_msec;
    }

    if (!entry->client_submit_ts) {
        entry->client_submit_ts = client->created_ts;
    }
    if (!entry->client_finish_msec) {
        entry->client_finish_msec = clamp_u32(entry->duration_msec);
    }
    if (entry->client_start_msec < entry->client_enqueue_msec) {
        entry->client_start_msec = entry->client_enqueue_msec;
    }
    if (entry->client_finish_msec < entry->client_start_msec) {
        entry->client_finish_msec = entry->client_start_msec;
    }
    if (entry->client_waitforcs_msec > entry->client_finish_msec) {
        entry->client_waitforcs_msec = entry->client_finish_msec;
    }
    if (!entry->client_local_queue_msec && entry->client_start_msec >= entry->client_enqueue_msec) {
        entry->client_local_queue_msec = entry->client_start_msec - entry->client_enqueue_msec;
    }
    if (!entry->client_exec_msec && entry->client_finish_msec >= entry->client_start_msec) {
        entry->client_exec_msec = entry->client_finish_msec - entry->client_start_msec;
    }
    if (!entry->client_mode.empty()) {
        return;
    }
    if (!entry->usecs_host.empty() && entry->usecs_host != "127.0.0.1") {
        entry->client_mode = "remote";
    } else if (entry->usecs_host == "127.0.0.1") {
        entry->client_mode = "local_via_scheduler";
    } else {
        entry->client_mode = "local";
    }
}

void Daemon::remember_finished_job(const Client *client, int exitcode)
{
    /* History and insight buckets exist to serve the web GUI, the JSONL
       stream, and the periodic state dump.  A daemon with none of those
       enabled must not pay their memory cost (the 20k-entry deque retains
       command lines, channels and paths -- tens of MB on busy submitters).  */
    if (!webgui_enabled && state_jsonl_path.empty() && !state_dump_log) {
        return;
    }
    if (!should_track_client_job(client)) {
        return;
    }

    JobHistoryEntry entry;
    entry.seq = next_job_history_seq++;
    entry.start_ts = client->created_ts;
    entry.end_ts = time(nullptr);
    entry.start_msec = client->created_msec;
    entry.end_msec = monotonic_msec();
    entry.duration_msec = entry.end_msec >= entry.start_msec ? (entry.end_msec - entry.start_msec) : 0;
    entry.client_id = client->client_id;
    /* Prefer the compiler's real exit status reported via JobTimingMsg; the
       daemon-side teardown code (118/119/...) is kept separately in
       end_code as a transport diagnostic.  */
    entry.exitcode = (client->has_timing && client->has_timing_exitcode)
        ? client->timing_exitcode : exitcode;
    entry.end_code = exitcode;
    entry.scheduler_job_id = client->last_known_job_id;
    entry.compile_job_id = client->job ? client->job->jobID() : 0;
    entry.final_status = Client::status_str(client->status);
    entry.final_why = client->status_why;
    entry.target = client->job ? client->job->targetPlatform() : string();
    entry.environment = client->job ? client->job->environmentVersion() : string();
    entry.usecs_host = client->usecsmsg ? client->usecsmsg->hostname : string();
    entry.usecs_port = client->usecsmsg ? client->usecsmsg->port : 0;
    entry.usecs_got_env = client->usecsmsg ? client->usecsmsg->got_env : false;
    entry.outfile = client->outfile;
    entry.cmdline = client_command_line_for_display(client);
    entry.channel = client->channel ? client->channel->dump() : string();
    entry.client_timing = client->has_timing;
    entry.client_submit_ts = client->timing_submit_ts;
    entry.client_enqueue_msec = client->timing_enqueue_msec;
    entry.client_start_msec = client->timing_start_msec;
    entry.client_finish_msec = client->timing_finish_msec;
    entry.client_waitforcs_msec = client->timing_waitforcs_msec;
    entry.client_local_queue_msec = client->timing_local_queue_msec;
    entry.client_exec_msec = client->timing_exec_msec;
    entry.client_mode = client->timing_mode;
    finalize_job_timing(&entry, client);
    if (entry.client_timing && client->timing_scheduler_job_id) {
        entry.scheduler_job_id = client->timing_scheduler_job_id;
    }
    if (entry.client_timing && client->timing_compile_job_id) {
        entry.compile_job_id = client->timing_compile_job_id;
    }

    if (job_history.size() >= job_history_capacity) {
        job_history.pop_front();
    }
    job_history.push_back(entry);
    update_insights_history(entry);

    if (state_dump_log || !state_jsonl_path.empty()) {
        ostringstream o;
        o << "{";
        o << "\"type\":\"iceccd_job_timing\",";
        o << "\"ts\":" << (long long)time(nullptr) << ",";
        o << "\"mono_msec\":" << (unsigned long long)monotonic_msec() << ",";
        o << "\"node\":\"" << json_escape(nodename) << "\",";
        o << "\"client_id\":" << entry.client_id << ",";
        o << "\"scheduler_job_id\":" << entry.scheduler_job_id << ",";
        o << "\"compile_job_id\":" << entry.compile_job_id << ",";
        o << "\"exitcode\":" << entry.exitcode << ",";
        o << "\"end_code\":" << entry.end_code << ",";
        o << "\"final_status\":\"" << json_escape(entry.final_status) << "\",";
        o << "\"final_why\":\"" << json_escape(entry.final_why) << "\",";
        o << "\"mode\":\"" << json_escape(entry.client_mode) << "\",";
        o << "\"timing_source\":\"" << (entry.client_timing ? "client" : "daemon") << "\",";
        o << "\"submit_ts\":" << (long long)entry.client_submit_ts << ",";
        o << "\"enqueue_msec\":" << entry.client_enqueue_msec << ",";
        o << "\"start_msec\":" << entry.client_start_msec << ",";
        o << "\"finish_msec\":" << entry.client_finish_msec << ",";
        o << "\"waitforcs_msec\":" << entry.client_waitforcs_msec << ",";
        o << "\"local_queue_msec\":" << entry.client_local_queue_msec << ",";
        o << "\"exec_msec\":" << entry.client_exec_msec << ",";
        o << "\"cmdline\":\"" << json_escape(entry.cmdline) << "\"";
        o << "}";
        const string line = o.str();
        if (state_dump_log && logfile_error) {
            /* No explicit flush: forcing a storage write per record from the
               control loop is exactly the latency coupling OBS-2 removes.
               The stream's own buffering (and SIGHUP log reopen) flushes.  */
            (*logfile_error) << line << "\n";
        }
        append_state_jsonl_line(line);
    }
}

string Daemon::dump_job_history_json(size_t limit, uint64_t before_seq) const
{
    /* Newest first.  `before_seq` is a cursor: only entries with a smaller
       sequence number are returned, so a client can page through history
       without the daemon ever building an unbounded response.  */
    size_t start = 0;
    if (before_seq) {
        while (start < job_history.size()
               && job_history[job_history.size() - 1 - start].seq >= before_seq) {
            ++start;
        }
    }
    const size_t available = job_history.size() - start;
    if (limit == 0 || limit > available) {
        limit = available;
    }

    ostringstream o;
    o << "{";
    o << "\"type\":\"iceccd_job_history\",";
    o << "\"ts\":" << (long long)time(nullptr) << ",";
    o << "\"capacity\":" << job_history_capacity << ",";
    o << "\"size\":" << job_history.size() << ",";
    o << "\"returned\":" << limit << ",";
    /* Honest truncation metadata: the UI can say "N of M" and page instead
       of silently implying it showed everything.  */
    o << "\"truncated\":" << ((start + limit < job_history.size()) ? "true" : "false") << ",";
    o << "\"next_before_seq\":"
      << ((limit && start + limit < job_history.size())
          ? (unsigned long long)job_history[job_history.size() - 1 - (start + limit - 1)].seq
          : 0ULL) << ",";
    o << "\"jobs\":[";

    for (size_t i = 0; i < limit; ++i) {
        const JobHistoryEntry &entry = job_history[job_history.size() - 1 - (start + i)];
        if (i) {
            o << ",";
        }

        o << "{";
        o << "\"seq\":" << (unsigned long long)entry.seq << ",";
        o << "\"client_id\":" << entry.client_id << ",";
        o << "\"start_ts\":" << (long long)entry.start_ts << ",";
        o << "\"end_ts\":" << (long long)entry.end_ts << ",";
        o << "\"start_msec\":" << (unsigned long long)entry.start_msec << ",";
        o << "\"end_msec\":" << (unsigned long long)entry.end_msec << ",";
        o << "\"duration_msec\":" << (unsigned long long)entry.duration_msec << ",";
        o << "\"exitcode\":" << entry.exitcode << ",";
        o << "\"end_code\":" << entry.end_code << ",";
        o << "\"scheduler_job_id\":" << entry.scheduler_job_id << ",";
        o << "\"compile_job_id\":" << entry.compile_job_id << ",";
        o << "\"final_status\":\"" << json_escape(entry.final_status) << "\",";
        o << "\"final_why\":\"" << json_escape(entry.final_why) << "\",";
        o << "\"target\":\"" << json_escape(entry.target) << "\",";
        o << "\"environment\":\"" << json_escape(entry.environment) << "\",";
        o << "\"usecs_host\":\"" << json_escape(entry.usecs_host) << "\",";
        o << "\"usecs_port\":" << entry.usecs_port << ",";
        o << "\"usecs_got_env\":" << (entry.usecs_got_env ? "true" : "false") << ",";
        o << "\"outfile\":\"" << json_escape(entry.outfile) << "\",";
        o << "\"cmdline\":\"" << json_escape(entry.cmdline) << "\",";
        o << "\"channel\":\"" << json_escape(entry.channel) << "\",";
        o << "\"timing_source\":\"" << (entry.client_timing ? "client" : "daemon") << "\",";
        o << "\"mode\":\"" << json_escape(entry.client_mode) << "\",";
        o << "\"submit_ts\":" << (long long)entry.client_submit_ts << ",";
        o << "\"enqueue_msec\":" << entry.client_enqueue_msec << ",";
        o << "\"start_compile_msec\":" << entry.client_start_msec << ",";
        o << "\"finish_msec\":" << entry.client_finish_msec << ",";
        o << "\"waitforcs_msec\":" << entry.client_waitforcs_msec << ",";
        o << "\"local_queue_msec\":" << entry.client_local_queue_msec << ",";
        o << "\"exec_msec\":" << entry.client_exec_msec;
        o << "}";
    }

    o << "]";
    o << "}";
    return o.str();
}

void Daemon::prune_insights_history(time_t now_ts)
{
    if (now_ts <= 0) {
        now_ts = time(nullptr);
    }
    const time_t cutoff = now_ts - time_t(insights_retention_minutes * 60);
    while (!insights_history.empty() && (insights_history.front().minute_ts + 60) <= cutoff) {
        insights_history.pop_front();
    }
}

void Daemon::update_insights_history(const JobHistoryEntry &entry)
{
    if (entry.end_ts <= 0) {
        return;
    }
    const time_t minute_ts = entry.end_ts - (entry.end_ts % 60);
    if (insights_history.empty() || insights_history.back().minute_ts < minute_ts) {
        /* Wall-clock steps must not drive allocation.  A forward jump (an
           RTC correction, NTP catching up on a machine that booted with a
           bad clock) could otherwise append one bucket per missing minute
           in an unbounded loop -- millions of buckets, in the main loop,
           before retention pruning ever runs.  Clamp the backfill to the
           retention window, and treat anything larger as a discontinuity:
           the old series describes a different timeline, so reset it.  */
        const time_t gap_minutes = insights_history.empty()
            ? 0 : (minute_ts - insights_history.back().minute_ts) / 60;
        if (gap_minutes > time_t(insights_retention_minutes)) {
            log_warning() << "insights: clock discontinuity of " << gap_minutes
                          << " minutes - resetting the series" << endl;
            insights_history.clear();
            ++insights_clock_resets;
        }
        time_t next_minute = insights_history.empty()
                             ? minute_ts
                             : (insights_history.back().minute_ts + 60);
        while (next_minute <= minute_ts) {
            InsightsMinuteEntry bucket;
            bucket.minute_ts = next_minute;
            insights_history.push_back(bucket);
            next_minute += 60;
        }
    } else if (insights_history.back().minute_ts > minute_ts) {
        for (auto rit = insights_history.rbegin(); rit != insights_history.rend(); ++rit) {
            if (rit->minute_ts == minute_ts) {
                break;
            }
            if (rit + 1 == insights_history.rend()) {
                return;
            }
        }
    }

    InsightsMinuteEntry *bucket = nullptr;
    for (auto rit = insights_history.rbegin(); rit != insights_history.rend(); ++rit) {
        if (rit->minute_ts == minute_ts) {
            bucket = &(*rit);
            break;
        }
        if (rit->minute_ts < minute_ts) {
            break;
        }
    }
    if (!bucket) {
        /* The job's minute is older than anything retained (a backward clock
           step, or a very late record).  Dropping it is correct -- but it
           must be visible, not silent.  */
        ++insights_dropped_jobs;
        return;
    }

    ++bucket->jobs_total;
    if (entry.exitcode != 0) {
        ++bucket->jobs_failed;
    }
    const bool remote_job = !entry.client_mode.empty() && entry.client_mode.find("remote") == 0;
    if (remote_job) {
        ++bucket->jobs_remote;
    } else {
        ++bucket->jobs_local;
    }
    if (cmdline_is_preprocess_only(entry.cmdline)) {
        ++bucket->jobs_preprocess;
    }
    if (entry.client_timing) {
        ++bucket->jobs_timed;
    }

    uint32_t queue_msec = entry.client_local_queue_msec;
    if (!queue_msec) {
        queue_msec = entry.client_waitforcs_msec;
    }
    if (queue_msec) {
        bucket->queue_sum_msec += queue_msec;
        ++bucket->queue_samples;
    }
    if (entry.client_exec_msec) {
        bucket->exec_sum_msec += entry.client_exec_msec;
        ++bucket->exec_samples;
    }
    if (entry.client_waitforcs_msec) {
        bucket->waitforcs_sum_msec += entry.client_waitforcs_msec;
        ++bucket->waitforcs_samples;
    }

    prune_insights_history(entry.end_ts);
}

string Daemon::dump_insights_series_json(size_t minutes)
{
    if (minutes == 0) {
        minutes = insights_graph_minutes;
    }
    if (minutes > insights_retention_minutes) {
        minutes = insights_retention_minutes;
    }

    const time_t now = time(nullptr);
    prune_insights_history(now);
    const time_t current_minute = now - (now % 60);
    const time_t start_minute = current_minute - time_t((minutes - 1) * 60);

    map<time_t, const InsightsMinuteEntry *> by_minute;
    for (const auto &bucket : insights_history) {
        by_minute[bucket.minute_ts] = &bucket;
    }

    ostringstream o;
    o << "{";
    o << "\"type\":\"iceccd_insights_series\",";
    o << "\"ts\":" << (long long)now << ",";
    o << "\"retention_minutes\":" << insights_retention_minutes << ",";
    o << "\"graph_minutes\":" << minutes << ",";
    /* The newest bucket covers a minute still in progress, so its counts are
       partial by construction: a UI that plots it as a completed rate shows
       a dip after every rollover.  Publish the boundary and a rate computed
       only from complete minutes, so the client does not have to guess.  */
    o << "\"current_minute_ts\":" << (long long)current_minute << ",";
    {
        uint64_t complete_jobs = 0;
        size_t complete_minutes = 0;
        for (const auto &b : insights_history) {
            if (b.minute_ts >= current_minute || b.minute_ts < start_minute) {
                continue;
            }
            complete_jobs += b.jobs_total;
            ++complete_minutes;
        }
        o << "\"complete_minutes\":" << complete_minutes << ",";
        o << "\"jobs_per_minute_complete\":"
          << (complete_minutes ? double(complete_jobs) / double(complete_minutes) : 0.0) << ",";
    }
    o << "\"buckets\":[";

    for (size_t index = 0; index < minutes; ++index) {
        const time_t minute_ts = start_minute + time_t(index * 60);
        const InsightsMinuteEntry *bucket = nullptr;
        auto it = by_minute.find(minute_ts);
        if (it != by_minute.end()) {
            bucket = it->second;
        }
        if (index) {
            o << ",";
        }

        const uint32_t jobs_total = bucket ? bucket->jobs_total : 0;
        const uint32_t jobs_remote = bucket ? bucket->jobs_remote : 0;
        const uint32_t jobs_local = bucket ? bucket->jobs_local : 0;
        const uint32_t jobs_preprocess = bucket ? bucket->jobs_preprocess : 0;
        const uint32_t jobs_failed = bucket ? bucket->jobs_failed : 0;
        const uint32_t jobs_timed = bucket ? bucket->jobs_timed : 0;
        const uint32_t queue_avg = (bucket && bucket->queue_samples)
                                   ? uint32_t(bucket->queue_sum_msec / bucket->queue_samples)
                                   : 0;
        const uint32_t exec_avg = (bucket && bucket->exec_samples)
                                  ? uint32_t(bucket->exec_sum_msec / bucket->exec_samples)
                                  : 0;
        const uint32_t waitforcs_avg = (bucket && bucket->waitforcs_samples)
                                       ? uint32_t(bucket->waitforcs_sum_msec / bucket->waitforcs_samples)
                                       : 0;
        const uint32_t timing_coverage_pct = jobs_total
                                             ? uint32_t((100ULL * jobs_timed) / jobs_total)
                                             : 0;

        o << "{";
        o << "\"minute_ts\":" << (long long)minute_ts << ",";
        o << "\"partial\":" << (minute_ts >= current_minute ? "true" : "false") << ",";
        o << "\"jobs_total\":" << jobs_total << ",";
        o << "\"jobs_remote\":" << jobs_remote << ",";
        o << "\"jobs_local\":" << jobs_local << ",";
        o << "\"jobs_preprocess\":" << jobs_preprocess << ",";
        o << "\"jobs_failed\":" << jobs_failed << ",";
        o << "\"queue_avg_msec\":" << queue_avg << ",";
        o << "\"exec_avg_msec\":" << exec_avg << ",";
        o << "\"waitforcs_avg_msec\":" << waitforcs_avg << ",";
        o << "\"timing_coverage_pct\":" << timing_coverage_pct;
        o << "}";
    }

    o << "]";
    o << "}";
    return o.str();
}

string Daemon::dump_insights_jobs_json(time_t minute_ts, size_t limit)
{
    if (limit == 0) {
        limit = 500;
    }
    if (limit > 5000) {
        limit = 5000;
    }

    const time_t now = time(nullptr);
    prune_insights_history(now);
    const time_t minute_start = minute_ts - (minute_ts % 60);
    const time_t minute_end = minute_start + 60;
    const time_t cutoff = now - time_t(insights_retention_minutes * 60);

    ostringstream o;
    o << "{";
    o << "\"type\":\"iceccd_insights_jobs\",";
    o << "\"ts\":" << (long long)now << ",";
    o << "\"minute_ts\":" << (long long)minute_start << ",";
    o << "\"retention_minutes\":" << insights_retention_minutes << ",";
    o << "\"limit\":" << limit << ",";
    o << "\"jobs\":[";

    size_t returned = 0;
    if (minute_start >= cutoff) {
        for (auto it = job_history.rbegin(); it != job_history.rend(); ++it) {
            const JobHistoryEntry &entry = *it;
            if (entry.end_ts < minute_start) {
                break;
            }
            if (entry.end_ts >= minute_end) {
                continue;
            }
            if (entry.end_ts < cutoff) {
                continue;
            }
            if (returned) {
                o << ",";
            }
            ++returned;

            o << "{";
            o << "\"seq\":" << (unsigned long long)entry.seq << ",";
            o << "\"client_id\":" << entry.client_id << ",";
            o << "\"end_ts\":" << (long long)entry.end_ts << ",";
            o << "\"duration_msec\":" << (unsigned long long)entry.duration_msec << ",";
            o << "\"exitcode\":" << entry.exitcode << ",";
        o << "\"end_code\":" << entry.end_code << ",";
            o << "\"final_status\":\"" << json_escape(entry.final_status) << "\",";
            o << "\"final_why\":\"" << json_escape(entry.final_why) << "\",";
            o << "\"scheduler_job_id\":" << entry.scheduler_job_id << ",";
            o << "\"compile_job_id\":" << entry.compile_job_id << ",";
            o << "\"mode\":\"" << json_escape(entry.client_mode) << "\",";
            o << "\"target\":\"" << json_escape(entry.target) << "\",";
            o << "\"environment\":\"" << json_escape(entry.environment) << "\",";
            o << "\"usecs_host\":\"" << json_escape(entry.usecs_host) << "\",";
            o << "\"usecs_port\":" << entry.usecs_port << ",";
            o << "\"waitforcs_msec\":" << entry.client_waitforcs_msec << ",";
            o << "\"queue_msec\":" << (entry.client_local_queue_msec ? entry.client_local_queue_msec : entry.client_waitforcs_msec) << ",";
            o << "\"exec_msec\":" << entry.client_exec_msec << ",";
            o << "\"cmdline\":\"" << json_escape(entry.cmdline) << "\"";
            o << "}";

            if (returned >= limit) {
                break;
            }
        }
    }

    o << "],";
    o << "\"returned\":" << returned;
    o << "}";
    return o.str();
}

void Daemon::handle_web_accept()
{
    if (web_listen_fd < 0) {
        return;
    }

    for (;;) {
        sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        int fd = accept(web_listen_fd, reinterpret_cast<sockaddr *>(&addr), &addr_len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            if (errno == EMFILE || errno == ENFILE) {
                /* A level-triggered listener with no fd available would spin
                   the loop at 100% CPU; disarm accepting briefly.  */
                web_accept_backoff_until_msec = monotonic_msec() + 1000;
            }
            note_accept_error("webgui", errno);
            return;
        }

        if (web_connections.size() >= web_max_connections) {
            /* Best-effort refusal; the GUI is a diagnostic tool, compile
               traffic must keep the fd budget.  */
            static const char refuse[] =
                "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n"
                "Content-Length: 0\r\n\r\n";
            ssize_t unused = write(fd, refuse, sizeof(refuse) - 1);
            (void)unused;
            close(fd);
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);

        WebConnection conn;
        conn.fd = fd;
        conn.created_msec = monotonic_msec();
        conn.last_activity_msec = conn.created_msec;
        web_connections[fd] = conn;
    }
}

void Daemon::handle_web_connection(int fd, short revents)
{
    auto it = web_connections.find(fd);
    if (it == web_connections.end()) {
        return;
    }

    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        drop_web_connection(fd);
        return;
    }

    WebConnection &conn = it->second;

    if ((revents & POLLIN) && !conn.reject_input) {
        for (;;) {
            char buf[4096];
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                conn.last_activity_msec = monotonic_msec();
                conn.inbuf.append(buf, n);
                if (conn.inbuf.size() > 64 * 1024) {
                    /* Terminal: stop reading for good and free the input;
                       the connection only lives to drain the 413.  */
                    conn.reject_input = true;
                    conn.inbuf.clear();
                    conn.inbuf.shrink_to_fit();
                    if (!queue_web_response(fd, 413, "Payload Too Large",
                                            "text/plain; charset=utf-8",
                                            "request too large\n")) {
                        return;   // connection erased; `conn` is dangling
                    }
                    break;
                }
                continue;
            }
            if (n == 0) {
                drop_web_connection(fd);
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            drop_web_connection(fd);
            return;
        }

        /* The request-line parser accepts bare-LF lines, so the terminator
           check must too, or such a request idles until the deadline.  */
        if (conn.outbuf.empty() && !conn.reject_input
                && (conn.inbuf.find("\r\n\r\n") != string::npos
                    || conn.inbuf.find("\n\n") != string::npos)) {
            string method;
            string path;
            if (!parse_http_request(conn.inbuf, method, path)) {
                if (!queue_web_response(fd, 400, "Bad Request", "text/plain; charset=utf-8", "bad request\n")) {
                    return;
                }
            } else if (method != "GET") {
                if (!queue_web_response(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "only GET supported\n")) {
                    return;
                }
            } else {
                string route = path;
                const size_t query_pos = route.find('?');
                if (query_pos != string::npos) {
                    route.erase(query_pos);
                }

                if (route == "/" || route == "/index.html") {
                    if (!queue_web_response(fd, 200, "OK", "text/html; charset=utf-8", webgui_html())) {
                        return;
                    }
                } else if (route == "/insights" || route == "/insights.html") {
                    if (!queue_web_response(fd, 200, "OK", "text/html; charset=utf-8", webgui_insights_html())) {
                        return;
                    }
                } else if (route == "/insights-jobs" || route == "/insights-jobs.html") {
                    if (!queue_web_response(fd, 200, "OK", "text/html; charset=utf-8", webgui_insights_jobs_html())) {
                        return;
                    }
                } else if (route == "/api/state") {
                    if (!queue_web_response(fd, 200, "OK", "application/json; charset=utf-8", dump_state_json())) {
                        return;
                    }
                } else if (route == "/api/clients") {
                    if (!queue_web_response(fd, 200, "OK", "application/json; charset=utf-8", dump_clients_json())) {
                        return;
                    }
                } else if (route == "/api/jobs") {
                    const size_t limit = parse_jobs_limit(path);
                    uint64_t before_seq = 0;
                    parse_query_u64(path, "before", &before_seq);
                    if (!queue_web_response(fd, 200, "OK", "application/json; charset=utf-8",
                                            dump_job_history_json(limit, before_seq))) {
                        return;
                    }
                } else if (route == "/api/insights-series") {
                    uint64_t minutes = insights_graph_minutes;
                    parse_query_u64(path, "minutes", &minutes);
                    if (!queue_web_response(fd, 200, "OK", "application/json; charset=utf-8",
                                       dump_insights_series_json(size_t(minutes)))) {
                        return;
                    }
                } else if (route == "/api/insights-jobs") {
                    uint64_t minute_ts = 0;
                    if (!parse_query_u64(path, "minute", &minute_ts)) {
                        if (!queue_web_response(fd, 400, "Bad Request", "text/plain; charset=utf-8",
                                           "missing minute query parameter\n")) {
                            return;
                        }
                    } else {
                        uint64_t limit = 500;
                        parse_query_u64(path, "limit", &limit);
                        if (!queue_web_response(fd, 200, "OK", "application/json; charset=utf-8",
                                           dump_insights_jobs_json((time_t)minute_ts, (size_t)limit))) {
                            return;
                        }
                    }
                } else if (route == "/api/internals") {
                    if (!queue_web_response(fd, 200, "OK", "text/plain; charset=utf-8", dump_internals())) {
                        return;
                    }
                } else {
                    if (!queue_web_response(fd, 404, "Not Found", "text/plain; charset=utf-8", "not found\n")) {
                        return;
                    }
                }
            }
        }
    }

    if ((revents & POLLOUT) && conn.outbuf_ofs < conn.outbuf.size()) {
        while (conn.outbuf_ofs < conn.outbuf.size()) {
            ssize_t n = write(fd, conn.outbuf.data() + conn.outbuf_ofs,
                              conn.outbuf.size() - conn.outbuf_ofs);
            if (n > 0) {
                /* Offset drain: repeated front-erases made large responses
                   quadratic in memory traffic.  */
                conn.outbuf_ofs += n;
                conn.response_started = true;
                conn.last_activity_msec = monotonic_msec();
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                break;
            }
            drop_web_connection(fd);
            return;
        }

        if (conn.outbuf_ofs >= conn.outbuf.size()) {
            conn.outbuf.clear();
            conn.outbuf.shrink_to_fit();
            conn.outbuf_ofs = 0;
            if (conn.close_after_write) {
                drop_web_connection(fd);
                return;
            }
        }
    }
}

void Daemon::determine_system()
{
    struct utsname uname_buf;

    if (uname(&uname_buf)) {
        log_perror("uname call failed. Unable to determine system node name and platform");
        return;
    }

    if (nodename.length() && (nodename != uname_buf.nodename)) {
        custom_nodename  = true;
    }

    if (!custom_nodename) {
        nodename = uname_buf.nodename;
    }

    machine_name = determine_platform();
}

string Daemon::determine_nodename()
{
    if (custom_nodename && !nodename.empty()) {
        return nodename;
    }

    // perhaps our host name changed due to network change?
    struct utsname uname_buf;

    if (!uname(&uname_buf)) {
        nodename = uname_buf.nodename;
    }

    return nodename;
}

void Daemon::determine_supported_features()
{
    supported_features = 0;
    struct archive* a = archive_read_new();
    static bool test_disable = false;
    // Make one of the two remotes in tests say it doesn't support xz/zstd tarballs.
    if( getenv( "ICECC_TESTS" ) != nullptr && nodename == "remoteice2" )
        test_disable = true;
    (void)test_disable;
#ifdef HAVE_LIBARCHIVE_XZ
    if( !test_disable && archive_read_support_filter_xz(a) >= ARCHIVE_WARN ) // includes ARCHIVE_OK
        supported_features = supported_features | NODE_FEATURE_ENV_XZ;
#endif
#ifdef HAVE_LIBARCHIVE_ZSTD
    if( !test_disable && archive_read_support_filter_zstd(a) >= ARCHIVE_WARN ) // includes ARCHIVE_OK
        supported_features = supported_features | NODE_FEATURE_ENV_ZSTD;
#endif
    // sanity checks
    if( archive_read_support_filter_gzip(a) < ARCHIVE_WARN ) // error
        log_error() << "No support for uncompressing gzip available." << endl;
    if( archive_read_support_format_tar(a) < ARCHIVE_WARN ) // error
        log_error() << "No support for unpacking tar available." << endl;
    archive_read_free(a);
}

bool Daemon::send_scheduler(const Msg& msg)
{
    if (!scheduler) {
        log_warning() << "no scheduler" << endl;
        return false;
    }

    if (!scheduler->send_msg(msg)) {
        log_error() << "sending message to scheduler failed.." << endl;
        close_scheduler();
        return false;
    }

    return true;
}

bool Daemon::reannounce_environments()
{
    log_info() << "reannounce_environments " << endl;
    LoginMsg lmsg(0, nodename, "", supported_features);
    lmsg.envs = available_environments(envbasedir);
    return send_scheduler(lmsg);
}

void Daemon::close_scheduler()
{
    if (!scheduler) {
        return;
    }

    delete scheduler;
    scheduler = nullptr;
    delete discover;
    discover = nullptr;
    next_scheduler_connect = time(nullptr) + 20 + (rand() & 31);
    static bool fast_reconnect = getenv( "ICECC_TESTS" ) != nullptr;
    if( fast_reconnect )
        next_scheduler_connect = time(nullptr) + 3;
}

bool Daemon::maybe_stats(bool force_check)
{
    struct timeval now;
    gettimeofday(&now, nullptr);

    time_t diff_sent = (now.tv_sec - last_stat.tv_sec) * 1000 + (now.tv_usec - last_stat.tv_usec) / 1000;

    if (diff_sent >= max_scheduler_pong * 1000 || force_check) {
        StatsMsg msg;
        unsigned int memory_fillgrade;
        unsigned long idleLoad = 0;
        unsigned long niceLoad = 0;

        fill_stats(idleLoad, niceLoad, memory_fillgrade, &msg, clients.active_processes);

        time_t diff_stat = (now.tv_sec - last_stat.tv_sec) * 1000 + (now.tv_usec - last_stat.tv_usec) / 1000;
        last_stat = now;

        /* icecream_load contains time in milliseconds we have used for icecream */
        /* idle time could have been used for icecream, so claim it */
        icecream_load += idleLoad * diff_stat / 1000;

        /* add the time of our childrens, but only the time since the last run */
        struct rusage ru;

        if (!getrusage(RUSAGE_CHILDREN, &ru)) {
            uint32_t ice_msec = ((ru.ru_utime.tv_sec - icecream_usage.tv_sec) * 1000
                                 + (ru.ru_utime.tv_usec - icecream_usage.tv_usec) / 1000) / num_cpus;

            /* heuristics when no child terminated yet: account 25% of total nice as our clients */
            if (!ice_msec && current_kids) {
                ice_msec = (niceLoad * diff_stat) / (4 * 1000);
            }

            icecream_load += ice_msec * diff_stat / 1000;

            icecream_usage.tv_sec = ru.ru_utime.tv_sec;
            icecream_usage.tv_usec = ru.ru_utime.tv_usec;
        }

        unsigned int idle_average = icecream_load;

        if (diff_sent) {
            idle_average = icecream_load * 1000 / diff_sent;
        }

        if (idle_average > 1000)
           idle_average = 1000;

        msg.load = std::max((1000 - idle_average), memory_fillgrade);

#ifdef HAVE_SYS_VFS_H
        struct statfs buf;
        int ret = statfs(envbasedir.c_str(), &buf);

        // Require at least 25MiB of free disk space per build.
        if (!ret && long(buf.f_bavail) < ((long(max_kids + 1 - current_kids) * 25 * 1024 * 1024) / buf.f_bsize)) {
            msg.load = 1000;
        }

#endif

        mem_limit = std::max(int(msg.freeMem / std::min(std::max(max_kids, 1U), 4U)), min_mem_limit);

        if (abs(int(msg.load) - current_load) >= 100
            || (msg.load == 1000 && current_load != 1000)
            || (msg.load != 1000 && current_load == 1000)) {
            if (!send_scheduler(msg)) {
                return false;
            }
        }

        icecream_load = 0;
        current_load = msg.load;
    }

    return true;
}

string Daemon::dump_internals() const
{
    string result;
    const uint64_t now_msec = monotonic_msec();

    result += "Node Name: " + nodename + "\n";
    result += "  Remote name: " + remote_name + "\n";

    struct StatusAgg {
        uint32_t count = 0;
        uint64_t total_age_msec = 0;
        uint64_t max_age_msec = 0;
    };
    struct HostStatusAgg {
        uint32_t count = 0;
        uint64_t total_age_msec = 0;
        uint64_t max_age_msec = 0;
    };
    vector<StatusAgg> status_aggs(Client::LASTSTATE + 1);
    map<string, HostStatusAgg> waitcompile_by_host;
    for (const auto &it : clients) {
        const Client *client = it.second;
        StatusAgg &agg = status_aggs[int(client->status)];
        ++agg.count;
        const uint64_t age_msec = now_msec - client->status_since_msec;
        agg.total_age_msec += age_msec;
        agg.max_age_msec = std::max(agg.max_age_msec, age_msec);
        if (client->status == Client::WAITCOMPILE) {
            string host = "unknown";
            if (client->usecsmsg && !client->usecsmsg->hostname.empty()) {
                host = client->usecsmsg->hostname;
            }
            HostStatusAgg &host_agg = waitcompile_by_host[host];
            ++host_agg.count;
            host_agg.total_age_msec += age_msec;
            host_agg.max_age_msec = std::max(host_agg.max_age_msec, age_msec);
        }
    }

    result += "  Clients: " + toString(clients.size()) + " (" + clients.dump_per_status() + ")\n";
    result += "  Slots: active_processes=" + toString(clients.active_processes)
        + ", current_kids=" + toString(current_kids)
        + ", used=" + toString(current_kids + clients.active_processes)
        + ", max_kids=" + toString(max_kids)
        + ", max_preprocess_kids=" + toString(max_preprocess_kids) + "\n";

    const FdSnapshot fd_snapshot = collect_fd_snapshot();
    result += "  FDs: open=" + toString(fd_snapshot.open_count);
    if (fd_snapshot.soft_limit != RLIM_INFINITY) {
        result += ", soft_limit=" + toString((unsigned long long)fd_snapshot.soft_limit);
    } else {
        result += ", soft_limit=unlimited";
    }
    if (fd_snapshot.hard_limit != RLIM_INFINITY) {
        result += ", hard_limit=" + toString((unsigned long long)fd_snapshot.hard_limit);
    } else {
        result += ", hard_limit=unlimited";
    }
    result += ", accept_errors_total=" + toString((unsigned long long)accept_errors_total)
        + ", accept_emfile_errors=" + toString((unsigned long long)accept_emfile_errors)
        + "\n";

    if (scheduler) {
        time_t scheduler_last_talk_age_s = time(nullptr) - scheduler->last_talk;
        if (scheduler_last_talk_age_s < 0) {
            scheduler_last_talk_age_s = 0;
        }
        result += "  Scheduler: connected name=" + scheduler->name
            + " proto=" + toString(scheduler->protocol)
            + " last_talk_age_s=" + toString(scheduler_last_talk_age_s) + "\n";
    } else {
        time_t retry_in_s = next_scheduler_connect - time(nullptr);
        if (retry_in_s < 0) {
            retry_in_s = 0;
        }
        result += "  Scheduler: disconnected next_retry_in_s=" + toString(retry_in_s) + "\n";
    }

    auto append_status_wait = [&](Client::Status status, const string &label) {
        const StatusAgg &agg = status_aggs[int(status)];
        if (!agg.count) {
            return;
        }
        result += "  Wait " + label
            + ": count=" + toString(agg.count)
            + " avg_age_msec=" + toString(agg.total_age_msec / agg.count)
            + " max_age_msec=" + toString(agg.max_age_msec) + "\n";
    };
    append_status_wait(Client::WAITFORCS, "scheduler(waitforcs)");
    append_status_wait(Client::WAITCOMPILE, "remote(waitcompile)");
    append_status_wait(Client::PENDING_USE_CS, "local_slot(pending_use_cs)");
    append_status_wait(Client::TOCOMPILE, "local_queue(tocompile)");
    append_status_wait(Client::WAITFORCHILD, "local_child(waitforchild)");
    append_status_wait(Client::WAITCREATEENV, "create_env(waitcreateenv)");
    if (!waitcompile_by_host.empty()) {
        string waitcompile_host_line;
        for (const auto &it : waitcompile_by_host) {
            if (!waitcompile_host_line.empty()) {
                waitcompile_host_line += ", ";
            }
            const HostStatusAgg &host_agg = it.second;
            const uint64_t avg_age_msec = host_agg.count ? (host_agg.total_age_msec / host_agg.count) : 0;
            waitcompile_host_line += it.first + "=" + toString(host_agg.count)
                + "(avg=" + toString(avg_age_msec)
                + " max=" + toString(host_agg.max_age_msec) + ")";
        }
        result += "  Wait remote(waitcompile) by host: " + waitcompile_host_line + "\n";
    }

    const uint64_t waitforcs_samples = waitforcs_use_cs_samples + waitforcs_no_cs_samples;
    if (waitforcs_samples) {
        const uint64_t waitforcs_sum_msec = waitforcs_use_cs_sum_msec + waitforcs_no_cs_sum_msec;
        const uint64_t waitforcs_max_msec = std::max(waitforcs_use_cs_max_msec, waitforcs_no_cs_max_msec);
        result += "  Scheduler decision latency (msec): all samples=" + toString(waitforcs_samples)
            + " avg=" + toString(waitforcs_sum_msec / waitforcs_samples)
            + " max=" + toString(waitforcs_max_msec)
            + ", use_cs samples=" + toString(waitforcs_use_cs_samples)
            + " avg=" + toString(waitforcs_use_cs_samples ? (waitforcs_use_cs_sum_msec / waitforcs_use_cs_samples) : 0)
            + " max=" + toString(waitforcs_use_cs_max_msec)
            + ", no_cs samples=" + toString(waitforcs_no_cs_samples)
            + " avg=" + toString(waitforcs_no_cs_samples ? (waitforcs_no_cs_sum_msec / waitforcs_no_cs_samples) : 0)
            + " max=" + toString(waitforcs_no_cs_max_msec) + "\n";
    }

    uint32_t local_jobs_queued = status_aggs[int(Client::LINKJOB)].count;
    uint32_t local_jobs_running = 0;
    map<string, uint32_t> local_jobs_by_kind;
    map<string, uint32_t> local_jobs_by_reason;
    for (const auto &it : clients) {
        const Client *client = it.second;
        if (client->status == Client::LINKJOB
                || (client->status == Client::CLIENTWORK
                    && client->status_why == "handle_old_request: local job started")) {
            if (client->status == Client::CLIENTWORK) {
                ++local_jobs_running;
            }
            ++local_jobs_by_kind[local_job_kind_from_outfile(client->outfile, client->fulljob)];
            const string reason = client->local_reason.empty() ? string("unknown") : client->local_reason;
            ++local_jobs_by_reason[reason];
        }
    }
    const uint32_t local_jobs_total = local_jobs_queued + local_jobs_running;
    if (local_jobs_total) {
        result += "  Local jobs (legacy status linkjob): queued=" + toString(local_jobs_queued)
            + " running=" + toString(local_jobs_running)
            + " total=" + toString(local_jobs_total) + "\n";
        string local_jobs_kind_line;
        for (const auto &it : local_jobs_by_kind) {
            if (!local_jobs_kind_line.empty()) {
                local_jobs_kind_line += ", ";
            }
            local_jobs_kind_line += it.first + "=" + toString(it.second);
        }
        if (!local_jobs_kind_line.empty()) {
            result += "  Local jobs by output kind: " + local_jobs_kind_line + "\n";
        }
        string local_jobs_reason_line;
        for (const auto &it : local_jobs_by_reason) {
            if (!local_jobs_reason_line.empty()) {
                local_jobs_reason_line += ", ";
            }
            local_jobs_reason_line += it.first + "=" + toString(it.second);
        }
        if (!local_jobs_reason_line.empty()) {
            result += "  Local jobs by reason: " + local_jobs_reason_line + "\n";
        }
    }

    {
        const StatusAgg &toinstall = status_aggs[int(Client::TOINSTALL)];
        const StatusAgg &waitinstall = status_aggs[int(Client::WAITINSTALL)];
        const uint32_t count = toinstall.count + waitinstall.count;
        if (count) {
            const uint64_t total_age_msec = toinstall.total_age_msec + waitinstall.total_age_msec;
            const uint64_t max_age_msec = std::max(toinstall.max_age_msec, waitinstall.max_age_msec);
            result += "  Wait env_install(toinstall+waitinstall): count=" + toString(count)
                + " avg_age_msec=" + toString(total_age_msec / count)
                + " max_age_msec=" + toString(max_age_msec) + "\n";
        }
    }

    for (const auto &it : fd2client)  {
        result += "  fd2client[" + toString(it.first) + "] = " + it.second->dump() + "\n";
    }

    for (const auto& client : clients)  {
        result += "  client " + toString(client.second->client_id) + ": " + client.second->dump() + "\n";
    }

    if (cache_size) {
        result += "  Cache Size: " + toString(cache_size) + "\n";
    }

    result += "  Architecture: " + machine_name + "\n";

    for (const auto & native_environment : native_environments) {
        result += "  NativeEnv (" + native_environment.first + "): " + native_environment.second.name
            + ", size " + toString(native_environment.second.size) + (native_environment.second.create_env_pipe ? " (creating)" : "" ) + "\n";
    }

    if (!received_environments.empty()) {
        result += "  Now: " + toString(time(nullptr)) + "\n";
        for (const auto& it : received_environments )
            result += "  ReceivedEnv[" + it.first  + "] last_use " + toString(it.second.last_use)
                + ", size " + toString(it.second.size) + "\n";
    }

    result += "  Current kids: " + toString(current_kids) + " (max: " + toString(max_kids) + ")\n";
    result += "  Active preprocess jobs: " + toString(preprocess_active_processes)
              + " (max: " + toString(max_preprocess_kids) + ")\n";

    result += "  Supported features: " + supported_features_to_string(supported_features) + "\n";

    if (scheduler) {
        result += "  Scheduler protocol: " + toString(scheduler->protocol) + "\n";
    }

    StatsMsg msg;
    unsigned int memory_fillgrade = 0;
    unsigned long idleLoad = 0;
    unsigned long niceLoad = 0;

    fill_stats(idleLoad, niceLoad, memory_fillgrade, &msg, clients.active_processes);
    result += "  cpu: " + toString(idleLoad) + " idle, "
              + toString(niceLoad) + " nice\n";
    result += "  load: " + toString(msg.loadAvg1 / 1000.) + ", icecream_load: "
              + toString(icecream_load) + "\n";
    result += "  memory: " + toString(memory_fillgrade)
              + " (free: " + toString(msg.freeMem) + ")\n";

    return result;
}

bool Daemon::append_state_jsonl_line(const std::string &line)
{
    /* All bounding, dropping and I/O live in the writer's own thread; this
       is a cheap in-memory append from the control loop's perspective.  */
    state_writer.enqueue(line);
    return true;
}

std::string Daemon::dump_state_json() const
{
    const time_t now_s = time(nullptr);
    const uint64_t now_msec = monotonic_msec();
    const FdSnapshot fd_snapshot = collect_fd_snapshot();

    StatsMsg msg;
    unsigned int memory_fillgrade = 0;
    unsigned long idleLoad = 0;
    unsigned long niceLoad = 0;
    fill_stats(idleLoad, niceLoad, memory_fillgrade, &msg, clients.active_processes);

    struct StatusAgg {
        uint32_t count = 0;
        uint64_t total_age_msec = 0;
        uint64_t max_age_msec = 0;
    };
    struct HostStatusAgg {
        uint32_t count = 0;
        uint64_t total_age_msec = 0;
        uint64_t max_age_msec = 0;
    };
    vector<StatusAgg> status_aggs(Client::LASTSTATE + 1);
    uint32_t local_jobs_queued = 0;
    uint32_t local_jobs_running = 0;
    map<string, uint32_t> local_jobs_by_kind;
    map<string, uint32_t> local_jobs_by_reason;
    map<string, HostStatusAgg> waitcompile_by_host;

    auto is_worst_candidate = [](Client::Status s) -> bool {
        switch (s) {
        case Client::LINKJOB:
        case Client::WAITFORCS:
        case Client::PENDING_USE_CS:
        case Client::WAITCOMPILE:
        case Client::CLIENTWORK:
        case Client::TOCOMPILE:
        case Client::WAITFORCHILD:
        case Client::TOINSTALL:
        case Client::WAITINSTALL:
        case Client::WAITCREATEENV:
            return true;
        default:
            return false;
        }
    };

    vector<pair<uint64_t, const Client *>> worst_clients;
    worst_clients.reserve(clients.size());

    for (const auto &it : clients) {
        const Client *client = it.second;
        StatusAgg &agg = status_aggs[int(client->status)];
        ++agg.count;
        const uint64_t age_msec = now_msec - client->status_since_msec;
        agg.total_age_msec += age_msec;
        agg.max_age_msec = std::max(agg.max_age_msec, age_msec);
        if (client->status == Client::WAITCOMPILE) {
            string host = "unknown";
            if (client->usecsmsg && !client->usecsmsg->hostname.empty()) {
                host = client->usecsmsg->hostname;
            }
            HostStatusAgg &host_agg = waitcompile_by_host[host];
            ++host_agg.count;
            host_agg.total_age_msec += age_msec;
            host_agg.max_age_msec = std::max(host_agg.max_age_msec, age_msec);
        }
        const bool is_local_queued = (client->status == Client::LINKJOB);
        const bool is_local_running = (client->status == Client::CLIENTWORK
                                       && client->status_why == "handle_old_request: local job started");
        if (is_local_queued || is_local_running) {
            if (is_local_queued) {
                ++local_jobs_queued;
            } else {
                ++local_jobs_running;
            }
            ++local_jobs_by_kind[local_job_kind_from_outfile(client->outfile, client->fulljob)];
            const string reason = client->local_reason.empty() ? string("unknown") : client->local_reason;
            ++local_jobs_by_reason[reason];
        }
        if (is_worst_candidate(client->status)) {
            worst_clients.push_back(make_pair(age_msec, client));
        }
    }

    std::sort(worst_clients.begin(), worst_clients.end(),
        [](const pair<uint64_t, const Client *> &a, const pair<uint64_t, const Client *> &b) {
            return a.first > b.first;
        });

    ostringstream o;
    o << "{";
    o << "\"type\":\"iceccd_state\",";
    o << "\"ts\":" << (long long)now_s << ",";
    o << "\"mono_msec\":" << (unsigned long long)now_msec << ",";
    o << "\"pid\":" << (long)getpid() << ",";
    o << "\"node\":\"" << json_escape(nodename) << "\",";
    o << "\"remote_name\":\"" << json_escape(remote_name) << "\",";
    o << "\"daemon_port\":" << daemon_port << ",";
    o << "\"netname\":\"" << json_escape(netname) << "\",";
    o << "\"noremote\":" << (noremote ? "true" : "false") << ",";

    o << "\"scheduler\":{";
    if (scheduler) {
        time_t scheduler_last_talk_age_s = time(nullptr) - scheduler->last_talk;
        if (scheduler_last_talk_age_s < 0) {
            scheduler_last_talk_age_s = 0;
        }
        o << "\"connected\":true,";
        o << "\"name\":\"" << json_escape(scheduler->name) << "\",";
        o << "\"protocol\":" << scheduler->protocol << ",";
        o << "\"last_talk_age_s\":" << (long long)scheduler_last_talk_age_s;
    } else {
        time_t retry_in_s = next_scheduler_connect - time(nullptr);
        if (retry_in_s < 0) {
            retry_in_s = 0;
        }
        o << "\"connected\":false,";
        o << "\"next_retry_in_s\":" << (long long)retry_in_s;
    }
    o << "},";

    o << "\"slots\":{";
    o << "\"max_kids\":" << max_kids << ",";
    o << "\"max_preprocess_kids\":" << max_preprocess_kids << ",";
    o << "\"fulljob_policy\":\""
      << (fulljob_policy == FULLJOB_EXCLUSIVE ? "exclusive" : "compile-lane") << "\",";
    o << "\"fulljob_active\":" << fulljob_active << ",";
    // (telemetry health is emitted separately below, see "telemetry")
    o << "\"current_kids\":" << current_kids << ",";
    o << "\"active_processes\":" << clients.active_processes << ",";
    o << "\"active_preprocesses\":" << preprocess_active_processes << ",";
    o << "\"used\":" << (current_kids + clients.active_processes);
    o << "},";

    o << "\"fds\":{";
    o << "\"open\":" << fd_snapshot.open_count << ",";
    if (fd_snapshot.soft_limit == RLIM_INFINITY) {
        o << "\"soft_limit\":-1,";
    } else {
        o << "\"soft_limit\":" << (unsigned long long)fd_snapshot.soft_limit << ",";
    }
    if (fd_snapshot.hard_limit == RLIM_INFINITY) {
        o << "\"hard_limit\":-1,";
    } else {
        o << "\"hard_limit\":" << (unsigned long long)fd_snapshot.hard_limit << ",";
    }
    long long headroom = -1;
    long long util_pct = -1;
    if (fd_snapshot.open_count >= 0 && fd_snapshot.soft_limit != RLIM_INFINITY) {
        headroom = (long long)fd_snapshot.soft_limit - (long long)fd_snapshot.open_count;
        if (headroom < 0) {
            headroom = 0;
        }
        if (fd_snapshot.soft_limit > 0) {
            util_pct = ((long long)fd_snapshot.open_count * 100) / (long long)fd_snapshot.soft_limit;
        }
    }
    o << "\"headroom\":" << headroom << ",";
    o << "\"util_pct\":" << util_pct << ",";
    o << "\"accept_errors_total\":" << (unsigned long long)accept_errors_total << ",";
    o << "\"accept_emfile_errors\":" << (unsigned long long)accept_emfile_errors << ",";
    o << "\"last_accept_errno\":" << last_accept_errno << ",";
    o << "\"last_accept_errno_ts\":" << (long long)last_accept_errno_ts;
    o << "},";

    o << "\"stats\":{";
    o << "\"current_load\":" << current_load << ",";
    o << "\"loadAvg1\":" << msg.loadAvg1 << ",";
    o << "\"loadAvg5\":" << msg.loadAvg5 << ",";
    o << "\"loadAvg10\":" << msg.loadAvg10 << ",";
    o << "\"cpu_idle\":" << idleLoad << ",";
    o << "\"cpu_nice\":" << niceLoad << ",";
    o << "\"memory_fillgrade\":" << memory_fillgrade << ",";
    o << "\"freeMemMB\":" << msg.freeMem;
    o << "},";

    o << "\"telemetry\":{"
      << "\"insights_clock_resets\":" << insights_clock_resets << ","
      << "\"insights_dropped_jobs\":" << insights_dropped_jobs << ","
      << "\"jsonl_dropped_records\":" << state_writer.dropped() << ","
      << "\"jsonl_oversized_records\":" << state_writer.oversized() << ","
      << "\"jsonl_queued_bytes\":" << state_writer.queued_bytes() << ","
      << "\"jsonl_open_failures\":" << state_writer.open_failures() << ","
      << "\"jsonl_write_failures\":" << state_writer.write_failures()
      << "},";
    o << "\"cache\":{";
    o << "\"cache_size\":" << (unsigned long long)cache_size << ",";
    o << "\"cache_size_limit\":" << (unsigned long long)cache_size_limit << ",";
    o << "\"native_envs\":" << native_environments.size() << ",";
    o << "\"received_envs\":" << received_environments.size();
    o << "},";

    o << "\"clients\":{";
    o << "\"total\":" << clients.size() << ",";
    o << "\"by_status\":{";
    bool first = true;
    for (Client::Status s = Client::UNKNOWN; s <= Client::LASTSTATE;
            s = Client::Status(int(s) + 1)) {
        const StatusAgg &agg = status_aggs[int(s)];
        const uint64_t avg_age_msec = agg.count ? (agg.total_age_msec / agg.count) : 0;
        if (!first) {
            o << ",";
        }
        first = false;
        o << "\"" << Client::status_str(s) << "\":{";
        o << "\"count\":" << agg.count << ",";
        o << "\"avg_age_msec\":" << (unsigned long long)avg_age_msec << ",";
        o << "\"max_age_msec\":" << (unsigned long long)agg.max_age_msec;
        o << "}";
    }
    o << "},";
    o << "\"waitcompile_by_host\":{";
    bool first_waitcompile_host = true;
    for (const auto &it : waitcompile_by_host) {
        if (!first_waitcompile_host) {
            o << ",";
        }
        first_waitcompile_host = false;
        const HostStatusAgg &host_agg = it.second;
        const uint64_t avg_age_msec = host_agg.count ? (host_agg.total_age_msec / host_agg.count) : 0;
        o << "\"" << json_escape(it.first) << "\":{";
        o << "\"count\":" << host_agg.count << ",";
        o << "\"avg_age_msec\":" << (unsigned long long)avg_age_msec << ",";
        o << "\"max_age_msec\":" << (unsigned long long)host_agg.max_age_msec;
        o << "}";
    }
    o << "},";

    o << "\"local_jobs\":{";
    o << "\"legacy_status_name\":\"linkjob\",";
    o << "\"queued\":" << local_jobs_queued << ",";
    o << "\"running\":" << local_jobs_running << ",";
    o << "\"total\":" << (local_jobs_queued + local_jobs_running) << ",";
    o << "\"by_kind\":{";
    bool first_local_kind = true;
    for (const auto &it : local_jobs_by_kind) {
        if (!first_local_kind) {
            o << ",";
        }
        first_local_kind = false;
        o << "\"" << json_escape(it.first) << "\":" << it.second;
    }
    o << "},";
    o << "\"by_reason\":{";
    bool first_local_reason = true;
    for (const auto &it : local_jobs_by_reason) {
        if (!first_local_reason) {
            o << ",";
        }
        first_local_reason = false;
        o << "\"" << json_escape(it.first) << "\":" << it.second;
    }
    o << "}";
    o << "},";

    const size_t worst_limit = std::min(state_dump_worst_clients, worst_clients.size());
    o << "\"worst_limit\":" << worst_limit << ",";
    o << "\"worst\":[";
    for (size_t i = 0; i < worst_limit; ++i) {
        const uint64_t age_msec = worst_clients[i].first;
        const Client *client = worst_clients[i].second;
        const bool local_job = (client->status == Client::LINKJOB)
                               || (client->status == Client::CLIENTWORK
                                   && client->status_why == "handle_old_request: local job started");
        const string local_job_kind = local_job
                                      ? local_job_kind_from_outfile(client->outfile, client->fulljob)
                                      : string();
        const string local_reason = local_job
                                    ? (client->local_reason.empty() ? string("unknown") : client->local_reason)
                                    : string();
        const string cmdline = client_command_line_for_display(client);
        if (i) {
            o << ",";
        }
        o << "{";
        o << "\"client_id\":" << client->client_id << ",";
        o << "\"status\":\"" << Client::status_str(client->status) << "\",";
        o << "\"local_job\":" << (local_job ? "true" : "false") << ",";
        o << "\"local_job_kind\":\"" << json_escape(local_job_kind) << "\",";
        o << "\"local_reason\":\"" << json_escape(local_reason) << "\",";
        o << "\"cmdline\":\"" << json_escape(cmdline) << "\",";
        o << "\"age_msec\":" << (unsigned long long)age_msec << ",";
        o << "\"why\":\"" << json_escape(client->status_why) << "\",";
        o << "\"last_waitforcs_msec\":" << (unsigned long long)client->last_waitforcs_msec << ",";
        o << "\"scheduler_job_id\":" << client->job_id << ",";
        o << "\"job\":";
        if (client->job) {
            o << "{";
            o << "\"job_id\":" << client->job->jobID() << ",";
            o << "\"target\":\"" << json_escape(client->job->targetPlatform()) << "\",";
            o << "\"env\":\"" << json_escape(client->job->environmentVersion()) << "\"";
            o << "}";
        } else {
            o << "null";
        }
        o << ",";
        o << "\"usecs\":";
        if (client->usecsmsg) {
            o << "{";
            o << "\"hostname\":\"" << json_escape(client->usecsmsg->hostname) << "\",";
            o << "\"port\":" << client->usecsmsg->port << ",";
            o << "\"got_env\":" << (client->usecsmsg->got_env ? "true" : "false") << ",";
            o << "\"host_platform\":\"" << json_escape(client->usecsmsg->host_platform) << "\",";
            o << "\"matched_job_id\":" << client->usecsmsg->matched_job_id;
            o << "}";
        } else {
            o << "null";
        }
        o << ",";
        o << "\"outfile\":\"" << json_escape(client->outfile) << "\",";
        o << "\"pending_create_env\":\"" << json_escape(client->pending_create_env) << "\",";
        o << "\"env_bytes_received\":" << (unsigned long long)client->env_bytes_received << ",";
        o << "\"channel\":\"" << json_escape(client->channel ? client->channel->dump() : string()) << "\"";
        o << "}";
    }
    o << "]";

    o << "}";

    const uint64_t combined_samples = waitforcs_use_cs_samples + waitforcs_no_cs_samples;
    const uint64_t combined_sum_msec = waitforcs_use_cs_sum_msec + waitforcs_no_cs_sum_msec;
    const uint64_t combined_max_msec = std::max(waitforcs_use_cs_max_msec, waitforcs_no_cs_max_msec);
    vector<uint64_t> waitforcs_combined_hist(waitforcs_latency_bucket_count + 1, 0);
    for (size_t i = 0; i < waitforcs_combined_hist.size(); ++i) {
        waitforcs_combined_hist[i] = waitforcs_use_cs_hist[i] + waitforcs_no_cs_hist[i];
    }

    o << ",\"waitforcs_latency_msec\":{";
    o << "\"bucket_upper_bounds\":[";
    for (size_t i = 0; i < waitforcs_latency_bucket_count; ++i) {
        if (i) {
            o << ",";
        }
        o << (unsigned long long)waitforcs_latency_bucket_upper_bounds_msec[i];
    }
    o << "],";
    auto emit_waitforcs_section = [&](const char *name, uint64_t samples, uint64_t sum_msec, uint64_t max_msec,
                                      const vector<uint64_t> &hist) {
        o << "\"" << name << "\":{";
        o << "\"samples\":" << (unsigned long long)samples << ",";
        o << "\"avg_msec\":" << (unsigned long long)(samples ? (sum_msec / samples) : 0) << ",";
        o << "\"max_msec\":" << (unsigned long long)max_msec << ",";
        o << "\"hist\":[";
        for (size_t i = 0; i < hist.size(); ++i) {
            if (i) {
                o << ",";
            }
            o << (unsigned long long)hist[i];
        }
        o << "]";
        o << "}";
    };
    emit_waitforcs_section("use_cs", waitforcs_use_cs_samples, waitforcs_use_cs_sum_msec,
                           waitforcs_use_cs_max_msec, waitforcs_use_cs_hist);
    o << ",";
    emit_waitforcs_section("no_cs", waitforcs_no_cs_samples, waitforcs_no_cs_sum_msec,
                           waitforcs_no_cs_max_msec, waitforcs_no_cs_hist);
    o << ",";
    emit_waitforcs_section("combined", combined_samples, combined_sum_msec,
                           combined_max_msec, waitforcs_combined_hist);
    o << "}";
    o << "}";

    return o.str();
}

void Daemon::maybe_dump_state()
{
    if (state_dump_interval_s == 0 || (state_jsonl_path.empty() && !state_dump_log)) {
        return;
    }

    const uint64_t now = monotonic_msec();
    if (!next_state_dump_msec) {
        next_state_dump_msec = now;
    }

    if (now < next_state_dump_msec) {
        return;
    }

    const string line = dump_state_json();

    if (state_dump_log) {
        // Write raw JSON to the same output as error logs, regardless of verbosity.
        // This keeps the line machine-parsable and avoids requiring `-vv`.
        // No explicit flush: a forced storage write per interval from the
        // control loop is the latency coupling OBS-2 removes.
        if (logfile_error) {
            (*logfile_error) << line << "\n";
        }
    }

    append_state_jsonl_line(line);

    const uint64_t interval_msec = uint64_t(state_dump_interval_s) * 1000;
    do {
        next_state_dump_msec += interval_msec;
    } while (next_state_dump_msec <= now);
}

void Daemon::record_waitforcs_latency(bool use_cs, uint64_t latency_msec)
{
    size_t bucket = waitforcs_latency_bucket_count;
    for (size_t i = 0; i < waitforcs_latency_bucket_count; ++i) {
        if (latency_msec <= waitforcs_latency_bucket_upper_bounds_msec[i]) {
            bucket = i;
            break;
        }
    }

    if (use_cs) {
        ++waitforcs_use_cs_samples;
        waitforcs_use_cs_sum_msec += latency_msec;
        waitforcs_use_cs_max_msec = std::max(waitforcs_use_cs_max_msec, latency_msec);
        ++waitforcs_use_cs_hist[bucket];
    } else {
        ++waitforcs_no_cs_samples;
        waitforcs_no_cs_sum_msec += latency_msec;
        waitforcs_no_cs_max_msec = std::max(waitforcs_no_cs_max_msec, latency_msec);
        ++waitforcs_no_cs_hist[bucket];
    }
}

int Daemon::scheduler_get_internals()
{
    trace() << "handle_get_internals " << dump_internals() << endl;
    return send_scheduler(StatusTextMsg(dump_internals())) ? 0 : 1;
}

int Daemon::scheduler_use_cs(UseCSMsg *msg)
{
    Client *c = clients.find_by_client_id(msg->client_id);
    trace() << "scheduler_use_cs " << msg->job_id << " " << msg->client_id
            << " " << c << " " << msg->hostname << " " << remote_name <<  endl;

    if (!c) {
        if (send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size()))) {
            return 0;
        }

        return 1;
    }

    if (c->status == Client::WAITFORCS) {
        c->last_waitforcs_msec = monotonic_msec() - c->status_since_msec;
        record_waitforcs_latency(true, c->last_waitforcs_msec);
    }

    if (msg->hostname == remote_name && int(msg->port) == daemon_port) {
        c->usecsmsg = new UseCSMsg(msg->host_platform, "127.0.0.1", daemon_port, msg->job_id, true, 1,
                                   msg->matched_job_id);
        c->set_status(Client::PENDING_USE_CS, "scheduler_use_cs: local compile");
    } else {
        c->usecsmsg = new UseCSMsg(msg->host_platform, msg->hostname, msg->port,
                                   msg->job_id, true, 1, msg->matched_job_id);

        if (!c->channel->send_msg(*msg)) {
            handle_end(c, 143);
            return 0;
        }

        c->set_status(Client::WAITCOMPILE, "scheduler_use_cs: remote compile");
    }

    c->job_id = msg->job_id;
    c->last_known_job_id = msg->job_id;

    return 0;
}

int Daemon::scheduler_no_cs(NoCSMsg *msg)
{
    Client *c = clients.find_by_client_id(msg->client_id);
    trace() << "scheduler_no_cs " << msg->job_id << " " << msg->client_id
            << " " << c << " " <<  endl;

    if (!c) {
        if (send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size()))) {
            return 0;
        }

        return 1;
    }

    if (c->status == Client::WAITFORCS) {
        c->last_waitforcs_msec = monotonic_msec() - c->status_since_msec;
        record_waitforcs_latency(false, c->last_waitforcs_msec);
    }

    c->usecsmsg = new UseCSMsg(string(), "127.0.0.1", daemon_port, msg->job_id, true, 1, 0);
    c->set_status(Client::PENDING_USE_CS, "scheduler_no_cs: local compile");

    c->job_id = msg->job_id;
    c->last_known_job_id = msg->job_id;

    return 0;

}

bool Daemon::handle_transfer_env(Client *client, EnvTransferMsg *emsg)
{
    log_info() << "handle_transfer_env, client status " << Client::status_str(client->status) <<  endl;

    assert(client->status != Client::TOINSTALL &&
           client->status != Client::WAITINSTALL &&
           client->status != Client::TOCOMPILE &&
           client->status != Client::WAITCOMPILE);
    assert(client->pipe_from_child < 0);
    assert(client->pipe_to_child < 0);

    string target = emsg->target;

    if (target.empty()) {
        target =  machine_name;
    }

    int pipe_from_child = -1;
    int pipe_to_child = -1;
    FileChunkMsg *fmsg = nullptr;

    pid_t pid = start_install_environment(envbasedir, target, emsg->name, client->channel,
                    pipe_to_child, pipe_from_child, fmsg, user_uid, user_gid, nice_level);

    if( pid <= 0 ) {
        delete fmsg;
        remove_environment_files(envbasedir, target + "/" + emsg->name);
        handle_end(client, 144);
        return false;
    }

    client->set_status(Client::TOINSTALL, "handle_transfer_env: receiving environment");
    client->env_bytes_received = 0;
    client->outfile = target + "/" + emsg->name;
    current_kids++;

    trace() << "PID of child thread running untaring environment: " << pid << endl;
    client->pipe_to_child = pipe_to_child;
    client->pipe_from_child = pipe_from_child;
    client->child_pid = pid;

    if (!handle_file_chunk_env(client, fmsg)) {
        delete fmsg;
        return false;
    }

    delete fmsg;
    return true;
}

bool Daemon::handle_file_chunk_env(Client *client, Msg *msg)
{
    /* this sucks, we can block when we're writing
       the file chunk to the child, but we can't let the child
       handle MsgChannel itself due to MsgChannel's stupid
       caching layer inbetween, which causes us to lose partial
       data after the END msg of the env transfer.  */

    assert(client);
    assert(client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL);
    assert(client->pipe_to_child >= 0);

    if (*msg == Msg::FILE_CHUNK) {
        FileChunkMsg *fcmsg = static_cast<FileChunkMsg *>(msg);
        client->env_bytes_received += fcmsg->len;
        ssize_t len = fcmsg->len;
        off_t off = 0;

        while (len) {
            ssize_t bytes = write(client->pipe_to_child, fcmsg->buffer + off, len);

            if (bytes < 0 && errno == EINTR) {
                continue;
            }
            if (bytes < 0 && errno == EPIPE) {
                // Broken pipe may mean the unpacking has failed, but it also may
                // mean the child has already finished successfully (it seems to happen,
                // maybe some tar implementations add needless trailing bytes?).
                // Wait for the child to finish to find out whether it was ok.
                return true;
            }

            if (bytes == -1) {
                log_perror("write to transfer env pipe failed.");
                handle_end(client, 137);
                return false;
            }

            len -= bytes;
            off += bytes;
        }

        return true;
    }

    if (*msg == Msg::END) {
        trace() << "received end of environment, waiting for child" << endl;
        close(client->pipe_to_child);
        client->pipe_to_child = -1;
        if( client->child_pid >= 0 ) {
            // Transfer done, wait for handle_transfer_env_child_done() to finish the handling.
            client->set_status(Client::WAITINSTALL, "handle_file_chunk_env: waiting for env install child"); // Ignore further messages until child finishes.
            return true;
        }
        // Transfer done, child done, finish.
        return finish_transfer_env( client );
    }

    // unexpected message type
    log_error() << "protocol error while receiving environment (" << msg->to_string() << ")" << endl;
    handle_end(client, 138);
    return false;
}

bool Daemon::handle_env_install_child_done(Client *client)
{
    assert(client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL);
    assert(client->child_pid >= 0);
    assert(client->pipe_from_child >= 0);
    bool success = false;
    for (;;) {
        char resultByte;
        ssize_t n = ::read(client->pipe_from_child, &resultByte, 1);
        if (n == -1 && errno == EINTR)
            continue;
        // The child at the end of start_install_environment() writes status on success.
        if (n == 1 && resultByte == 0 )
            success = true;
        break;
    }
    log_info() << "handle_env_install_child_done PID " << client->child_pid << " for " << client->outfile
        << " status: " << ( success ? "success" : "failed" ) << endl;
    client->child_pid = -1;
    assert(current_kids > 0);
    current_kids--;
    if (client->pipe_from_child >= 0) {
        close(client->pipe_from_child);
        client->pipe_from_child = -1;
    }
    if( !success )
        return finish_transfer_env( client, true ); // cancel
    if( client->pipe_to_child >= 0 ) {
        // we still haven't received END message, wait for that
        assert( client->status == Client::TOINSTALL );
        return true;
    }
    // Child done, transfer done, finish.
    return finish_transfer_env( client );
}

bool Daemon::finish_transfer_env(Client *client, bool cancel)
{
    log_info() << "finish_transfer_env for " << client->outfile
        << ( cancel ? " (cancel)" : "" ) << endl;

    assert(client->outfile.size());
    assert(client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL);

    if (client->pipe_from_child >= 0) {
        assert( cancel ); // If not cancelled, this is closed by handle_env_install_child_done().
        close(client->pipe_from_child);
        client->pipe_from_child = -1;
    }
    if (client->pipe_to_child >= 0) {
        assert( cancel ); // If not cancelled, this is closed by handle_file_chunk_env().
        close(client->pipe_to_child);
        client->pipe_to_child = -1;
    }
    if (client->child_pid >= 0 ) {
        assert( cancel ); // If not cancelled, this is handled by handle_env_install_child_done().
        kill( client->child_pid, SIGTERM );
        int status;
        trace() << "finish_transfer_env kill and waiting for child PID " << client->child_pid <<endl;
        while (waitpid(client->child_pid, &status, 0) < 0 && errno == EINTR)
            ;
        client->child_pid = -1;
        assert(current_kids > 0);
        current_kids--;
    }

    size_t installed_size = 0;
    if( !cancel ) {
        installed_size = finalize_install_environment(envbasedir, client->outfile,
                            user_uid, user_gid);
        log_info() << "installed_size: " << installed_size << endl;
    }
    if( installed_size == 0 )
        remove_environment_files(envbasedir, client->outfile);

    client->set_status(Client::UNKNOWN, cancel ? "finish_transfer_env: canceled" : "finish_transfer_env: done");
    string current = client->outfile;
    client->outfile.clear();

    if (installed_size) {
        cache_size += installed_size;
        received_environments[current].last_use = time(nullptr);
        received_environments[current].size = installed_size;
        log_info() << "installed " << current << " size: " << installed_size
                    << " all: " << cache_size << endl;
    }

    check_cache_size(current);

    bool r = reannounce_environments(); // do that before the file compiles

    if (!maybe_stats(true)) { // update stats in case our disk is too full to accept more jobs
        r = false;
    }

    return r;
}

void Daemon::check_cache_size(const string &new_env)
{
    time_t now = time(nullptr);

    while (cache_size > cache_size_limit) {
        string oldest_received;
        string oldest_native;
        // I don't dare to use (time_t)-1
        time_t oldest_time = time(nullptr) + 90000;

        for (const auto& it : received_environments ) {
            trace() << "considering cached environment: " << it.first << " " << it.second.last_use << " " << oldest_time << endl;

            if (access(string(envbasedir + "/target=" + it.first + "/usr/bin/as").c_str(), X_OK) != 0) {
                trace() << string(envbasedir + "/target=" + it.first + "/usr/bin/as") << " is missing, removing environment" << endl;
                // force removing this one
                oldest_time = 0;
                oldest_received = it.first;
                break;
            }

            // ignore recently used envs (they might be in use _right_ now)
            int keep_timeout = 200;

            if (it.second.last_use < oldest_time && now - it.second.last_use > keep_timeout) {
                bool env_currently_in_use = false;

                for (Clients::const_iterator it2 = clients.begin(); it2 != clients.end(); ++it2)  {
                    if (it2->second->status == Client::TOCOMPILE
                            || it2->second->status == Client::TOINSTALL
                            || it2->second->status == Client::WAITINSTALL
                            || it2->second->status == Client::WAITFORCHILD) {

                        assert(it2->second->job);
                        string envforjob = it2->second->job->targetPlatform() + "/"
                                           + it2->second->job->environmentVersion();

                        if (envforjob == it.first) {
                            env_currently_in_use = true;
                        }
                    }
                }

                if (!env_currently_in_use) {
                    oldest_time = it.second.last_use;
                    oldest_received = it.first;
                }
            }
        }
        for (const auto& it : native_environments ) {
            trace() << "considering native environment: " << it.first << " " << it.second.last_use << " " << oldest_time << endl;

            if (!it.second.name.empty() && access(it.second.name.c_str(), R_OK) != 0) {
                trace() << it.second.name << " is missing, removing environment" << endl;
                // force removing this one
                oldest_time = 0;
                oldest_native = it.first;
                break;
            }

            // ignore recently used envs (they might be in use _right_ now)
            int keep_timeout = 200;

            // Allow removing native environments only after a longer period,
            // unless there are many native environments.
            if (native_environments.size() < 5) {
                keep_timeout = 24 * 60 * 60;    // 1 day
            }

            if (it.second.create_env_pipe) {
                keep_timeout = 365 * 24 * 60 * 60; // do not remove if it's still being created
            }

            if (it.second.last_use < oldest_time && now - it.second.last_use > keep_timeout) {
                oldest_time = it.second.last_use;
                oldest_native = it.first;
            }
        }

        if ((oldest_received.empty() || oldest_received == new_env)
            && (oldest_native.empty() || oldest_native == new_env)) {
            break;
        }

        if (!oldest_native.empty())
            remove_native_environment(oldest_native);
        else
            remove_environment(oldest_received);
    }
}

void Daemon::remove_native_environment(const string& env_key)
{
    assert(!env_key.empty());
    remove_native_environment_files(env_key);
    const NativeEnvironment &env = native_environments[env_key];
    trace() << "removing " << env.name << " " << env.size << endl;
    if (env.create_env_pipe) {
        if ((-1 == close(env.create_env_pipe)) && (errno != EBADF)){
            log_perror("close failed");
        }
        // TODO kill the still running icecc-create-env process?
    }
    assert( cache_size >= env.size );
    cache_size -= env.size;
    native_environments.erase(env_key);
}

void Daemon::remove_environment(const string& env_key)
{
    assert(!env_key.empty());
    remove_environment_files(envbasedir, env_key);
    const ReceivedEnvironment& env = received_environments[env_key];
    trace() << "removing " << envbasedir << "/target=" << env_key << " " << env.size << endl;
    assert( cache_size >= env.size );
    cache_size -= env.size;
    received_environments.erase(env_key);
}

bool Daemon::handle_get_native_env(Client *client, GetNativeEnvMsg *msg)
{
    string env_key;
    map<string, time_t> filetimes;
    struct stat st;

    string compiler = msg->compiler;
    // Older clients passed simply "gcc" or "clang" and not a binary.
    if( !IS_PROTOCOL_VERSION(41, client->channel) && compiler.find('/') == string::npos)
        compiler = "/usr/bin/" + compiler;

    string ccompiler = get_c_compiler(compiler);
    string cppcompiler = get_cpp_compiler(compiler);

    trace() << "get_native_env for " << msg->compiler
        << " (" << ccompiler << "," << cppcompiler << ")" << endl;

    if (stat(ccompiler.c_str(), &st) != 0) {
        log_error() << "Compiler binary " << ccompiler << " for environment not found." << endl;
        client->channel->send_msg(EndMsg());
        handle_end(client, 122);
        return false;
    }
    filetimes[ccompiler] = st.st_mtime;
    if (stat(cppcompiler.c_str(), &st) == 0) {
        // C++ compiler is optional.
        filetimes[cppcompiler] = st.st_mtime;
    }

    env_key = msg->compression + ":" + ccompiler;
    for (list<string>::const_iterator it = msg->extrafiles.begin();
            it != msg->extrafiles.end(); ++it) {
        env_key += ':';
        env_key += *it;

        if (stat(it->c_str(), &st) != 0) {
            log_error() << "Extra file " << *it << " for environment not found." << endl;
            client->channel->send_msg(EndMsg());
            handle_end(client, 122);
            return false;
        }

        filetimes[*it] = st.st_mtime;
    }

    if (native_environments[env_key].name.length()) {
        const NativeEnvironment &env = native_environments[env_key];

        if (env.filetimes != filetimes || access(env.name.c_str(), R_OK) != 0) {
            trace() << "native_env needs rebuild" << endl;
            remove_native_environment(env.name);
        }
    }

    trace() << "get_native_env " << native_environments[env_key].name
            << " (" << env_key << ")" << endl;

    client->set_status(Client::WAITCREATEENV, "handle_get_native_env: waiting for icecc-create-env");
    client->pending_create_env = env_key;

    if (native_environments[env_key].name.length()) { // already available
        return finish_get_native_env(client, env_key);
    } else {
        NativeEnvironment &env = native_environments[env_key]; // also inserts it
        if (!env.create_env_pipe) { // start creating it only if not already in progress
            env.filetimes = filetimes;
            trace() << "start_create_env " << env_key << endl;
            env.create_env_pipe = start_create_env(envbasedir, user_uid, user_gid, ccompiler,
                msg->extrafiles, msg->compression);
        } else {
            trace() << "waiting for already running create_env " << env_key << endl;
        }
    }
    return true;
}

bool Daemon::finish_get_native_env(Client *client, string env_key)
{
    assert(client->status == Client::WAITCREATEENV);
    assert(client->pending_create_env == env_key);
    UseNativeEnvMsg m(native_environments[env_key].name);

    if (!client->channel->send_msg(m)) {
        handle_end(client, 138);
        return false;
    }

    native_environments[env_key].last_use = time(nullptr);
    client->set_status(Client::GOTNATIVE, "finish_get_native_env: sent native env");
    client->pending_create_env.clear();
    return true;
}

bool Daemon::create_env_finished(string env_key)
{
    assert(native_environments.count(env_key));
    NativeEnvironment &env = native_environments[env_key];

    trace() << "create_env_finished " << env_key << endl;
    assert(env.create_env_pipe);
    size_t installed_size = finish_create_env(env.create_env_pipe, envbasedir, env.name);
    env.create_env_pipe = 0;

    // we only clean out cache on next target install
    cache_size += installed_size;
    trace() << "cache_size = " << cache_size << endl;

    if (!installed_size) {
        bool repeat = true;
        while(repeat) {
            repeat = false;
            for (Clients::const_iterator it = clients.begin(); it != clients.end(); ++it)  {
                if (it->second->pending_create_env == env_key) {
                    it->second->channel->send_msg(EndMsg());
                    handle_end(it->second, 121);
                    // The handle_end call invalidates our iterator, so break out of the loop,
                    // but try again just in case, until there's no match.
                    repeat = true;
                    break;
                }
            }
        }
        return false;
    }

    env.last_use = time(nullptr);
    env.size = installed_size;
    check_cache_size(env.name);

    for (Clients::const_iterator it = clients.begin(); it != clients.end(); ++it) {
        if (it->second->pending_create_env == env_key)
            finish_get_native_env(it->second, env_key);
    }
    return true;
}

bool Daemon::handle_job_done(Client *cl, JobDoneMsg *m)
{
    if (cl->status == Client::CLIENTWORK) {
        if (cl->running_preprocess) {
            if (preprocess_active_processes > 0) {
                --preprocess_active_processes;
            }
            cl->running_preprocess = false;
        } else if(cl->fulljob) {
            clients.active_processes -= std::max((unsigned int)1, max_kids);
            if (fulljob_active > 0) {
                --fulljob_active;
            }
        } else {
            clients.active_processes--;
        }
    }

    cl->set_status(Client::JOBDONE, "handle_job_done: reported to scheduler");
    JobDoneMsg *msg = static_cast<JobDoneMsg *>(m);
    trace() << "handle_job_done " << msg->job_id << " " << (cl->fulljob ? "(full) " : "")
        << msg->exitcode << endl;
    cl->fulljob = false;

    if (!m->is_from_server()
            && (m->user_msec + m->sys_msec) <= m->real_msec) {
        icecream_load += (m->user_msec + m->sys_msec) / num_cpus;
    }

    assert(msg->job_id == cl->job_id);
    cl->job_id = 0; // the scheduler doesn't have it anymore

    msg->client_count = clients.size();

    return send_scheduler(*msg);
}

void Daemon::handle_old_request()
{
    const unsigned int compile_limit = std::max((unsigned int)1, max_kids);
    const unsigned int preprocess_limit = std::max((unsigned int)1, max_preprocess_kids);

    while (true) {
        const bool compile_capacity = (current_kids + clients.active_processes) < compile_limit;
        const bool preprocess_capacity = (preprocess_active_processes < preprocess_limit);
        if (!compile_capacity && !preprocess_capacity) {
            break;
        }

        /* Select the next LINKJOB strictly by (niceness, client_id): the
           previous conjunction required BOTH a lower id and strictly lower
           niceness, so equal-priority jobs could never displace the first
           map-iteration candidate (pointer order, not FIFO) and a
           higher-priority later-id job was rejected outright.

           fulljob semantics: a fulljob reserves the ENTIRE node.  It starts
           only when both lanes are idle, and while one is waiting at the
           head of the queue nothing else is admitted (a drain barrier --
           otherwise a stream of small jobs would starve it forever, or
           preprocess work would overlap the very link step whose isolation
           fulljob promises).  While it runs, the compile lane is blocked by
           its full reservation and the preprocess lane by fulljob_active.  */
        /* Select the best ADMISSIBLE LINKJOB by (niceness, client_id).
           Admissibility must be part of the comparison, not a test applied
           to the global best: otherwise a blocked compile job hides an
           admissible preprocess job (and vice versa), leaving a whole lane
           idle.  The one intentional exception is an exclusive-policy
           fulljob at the head, which arms a drain barrier -- see below.  */
        Client *client = nullptr;
        Client *blocked_fulljob = nullptr;
        for (const auto &it : clients) {
            Client *candidate = it.second;
            if (candidate->status != Client::LINKJOB) {
                continue;
            }

            const bool preprocess_job = candidate->local_preprocess && !candidate->fulljob;
            bool admissible;
            if (candidate->fulljob) {
                admissible = (fulljob_policy == FULLJOB_EXCLUSIVE)
                    ? ((current_kids + clients.active_processes) == 0
                       && preprocess_active_processes == 0 && fulljob_active == 0)
                    /* compile-lane: historical semantics -- start when any
                       compile slot is free, then reserve them all; the
                       preprocess lane is unaffected.  */
                    : (compile_capacity && fulljob_active == 0);
            } else if (preprocess_job) {
                admissible = preprocess_capacity
                    && (fulljob_policy == FULLJOB_COMPILE_LANE || fulljob_active == 0);
            } else {
                admissible = compile_capacity && fulljob_active == 0;
            }

            if (!admissible) {
                if (candidate->fulljob && fulljob_policy == FULLJOB_EXCLUSIVE
                        && (blocked_fulljob == nullptr
                            || candidate->niceness < blocked_fulljob->niceness
                            || (candidate->niceness == blocked_fulljob->niceness
                                && candidate->client_id < blocked_fulljob->client_id))) {
                    blocked_fulljob = candidate;
                }
                continue;
            }

            if (client == nullptr
                || candidate->niceness < client->niceness
                || (candidate->niceness == client->niceness
                    && candidate->client_id < client->client_id)) {
                client = candidate;
            }
        }

        /* Exclusive-policy drain barrier: if a waiting fulljob outranks every
           admissible local candidate, hold LOCAL admissions so a stream of
           smaller local jobs cannot starve it.  Remote service
           (PENDING_USE_CS / TOCOMPILE below) always continues -- a local link
           must never head-of-line-block the cluster.  */
        if (blocked_fulljob
                && (client == nullptr
                    || blocked_fulljob->niceness < client->niceness
                    || (blocked_fulljob->niceness == client->niceness
                        && blocked_fulljob->client_id < client->client_id))) {
            client = nullptr;
        }

        if (client) {
            trace() << "send JobLocalBeginMsg to client" << endl;
            const bool preprocess_job = client->local_preprocess && !client->fulljob;

            if (!client->channel->send_msg(JobLocalBeginMsg())) {
                log_warning() << "can't send start message to client" << endl;
                handle_end(client, 112);
            } else {
                client->set_status(Client::CLIENTWORK, "handle_old_request: local job started");
                if (preprocess_job) {
                    client->running_preprocess = true;
                    ++preprocess_active_processes;
                    trace() << "pushed local preprocess job " << client->client_id << endl;
                } else if (client->fulljob) { // reserve the entire node
                    client->running_preprocess = false;
                    clients.active_processes += compile_limit;
                    ++fulljob_active;
                    trace() << "pushed full local job " << client->client_id << endl;
                } else {
                    client->running_preprocess = false;
                    clients.active_processes++;
                    trace() << "pushed local job " << client->client_id << endl;
                }
                if (!send_scheduler(JobLocalBeginMsg(client->client_id, client->outfile,
                        client->fulljob, client->local_reason, client->command_line,
                        preprocess_job ? JobLocalBeginMsg::LocalFlagPreprocessOnly
                                       : JobLocalBeginMsg::LocalFlagNone))) {
                    return;
                }
            }

            continue;
        }

        if (!compile_capacity) {
            break;
        }

        client = clients.get_earliest_client(Client::PENDING_USE_CS);

        if (client) {
            trace() << "pending " << client->dump() << endl;

            if (client->channel->send_msg(*client->usecsmsg)) {
                client->set_status(Client::CLIENTWORK, "handle_old_request: usecs delivered");
                /* we make sure we reserve a spot and the rest is done if the
                 * client contacts as back with a Compile request */
                clients.active_processes++;
            } else {
                handle_end(client, 129);
            }

            continue;
        }

        /* we don't want to handle TOCOMPILE jobs as long as our load
           is too high */
        if (current_load >= 1000) {
            break;
        }

        client = clients.get_earliest_client(Client::TOCOMPILE);

        if (client) {
            CompileJob *job = client->job;
            assert(job);
            int sock = -1;
            pid_t pid = -1;

            trace() << "request for job " << job->jobID() << endl;

            string envforjob = job->targetPlatform() + "/" + job->environmentVersion();
            received_environments[envforjob].last_use = time(nullptr);
            pid = handle_connection(envbasedir, job, client->channel, sock, mem_limit, user_uid, user_gid);
            trace() << "handle connection returned " << pid << endl;

            if (pid > 0) {
                current_kids++;
                client->set_status(Client::WAITFORCHILD, "handle_old_request: compiling locally (child running)");
                client->pipe_from_child = sock;
                client->child_pid = pid;

                if (!send_scheduler(JobBeginMsg(job->jobID(), clients.size()))) {
                    log_info() << "failed sending scheduler about " << job->jobID() << endl;
                }
            } else {
                handle_end(client, 117);
            }

            continue;
        }

        break;
    }
}

bool Daemon::handle_compile_done(Client *client)
{
    assert(client->status == Client::WAITFORCHILD);
    assert(client->child_pid > 0);
    assert(client->pipe_from_child >= 0);

    JobDoneMsg *msg = new JobDoneMsg(client->job->jobID(), -1, JobDoneMsg::FROM_SERVER, clients.size());
    assert(msg);
    assert(current_kids > 0);
    current_kids--;

    unsigned int job_stat[8];
    int end_status = 151;

    if (read(client->pipe_from_child, job_stat, sizeof(job_stat)) == sizeof(job_stat)) {
        msg->in_uncompressed = job_stat[JobStatistics::in_uncompressed];
        msg->in_compressed = job_stat[JobStatistics::in_compressed];
        msg->out_compressed = msg->out_uncompressed = job_stat[JobStatistics::out_uncompressed];
        end_status = msg->exitcode = job_stat[JobStatistics::exit_code];
        msg->real_msec = job_stat[JobStatistics::real_msec];
        msg->user_msec = job_stat[JobStatistics::user_msec];
        msg->sys_msec = job_stat[JobStatistics::sys_msec];
        msg->pfaults = job_stat[JobStatistics::sys_pfaults];
    }

    close(client->pipe_from_child);
    client->pipe_from_child = -1;
    string envforjob = client->job->targetPlatform() + "/" + client->job->environmentVersion();
    received_environments[envforjob].last_use = time(nullptr);
    if(end_status == EXIT_COMPILER_MISSING) { // Environment damaged?
        remove_environment(envforjob);
        if( !reannounce_environments())
            log_warning() << "failed reannounce environments after failed compile " << client->job->jobID() << endl;
    }

    if(!send_scheduler(*msg))
        log_warning() << "failed sending scheduler about compile done " << client->job->jobID() << endl;
    handle_end(client, end_status);
    delete msg;
    return false;
}

bool Daemon::handle_compile_file(Client *client, Msg *msg)
{
    CompileJob *job = dynamic_cast<CompileFileMsg *>(msg)->takeJob();
    assert(client);
    assert(job);
    client->job = job;
    if (client->command_line.empty()) {
        client->command_line = command_line_from_compile_job(job);
    }
    client->last_known_job_id = job->jobID();

    if (client->status == Client::CLIENTWORK) {
        assert(job->environmentVersion() == "__client");

        if (!send_scheduler(JobBeginMsg(job->jobID(), clients.size()))) {
            trace() << "can't reach scheduler to tell him about compile file job "
                    << job->jobID() << endl;
            return false;
        }

        // no scheduler is not an error case!
    } else {
        client->set_status(Client::TOCOMPILE, "handle_compile_file: queued for local compile");
    }

    return true;
}

bool Daemon::handle_verify_env(Client *client, VerifyEnvMsg *msg)
{
    assert(msg);
    bool ok = verify_env(client->channel, envbasedir, msg->target, msg->environment, user_uid, user_gid);
    trace() << "Verify environment done, " << (ok ? "success" : "failure") << ", environment " << msg->environment
            << " (" << msg->target << ")" << endl;
    VerifyEnvResultMsg resultmsg(ok);

    if (!client->channel->send_msg(resultmsg)) {
        log_error() << "sending verify end result failed.." << endl;
        return false;
    }

    return true;
}

bool Daemon::handle_blacklist_host_env(Client *client, Msg *msg)
{
    // just forward
    assert(dynamic_cast<BlacklistHostEnvMsg *>(msg));
    assert(client);
    (void)client;

    if (!scheduler) {
        return false;
    }

    return send_scheduler(*msg);
}

void Daemon::handle_end(Client *client, int exitcode)
{
    trace() << "handle_end " << client->client_id << " " << client->channel->name << endl;
#ifdef ICECC_DEBUG
    trace() << "handle_end " << client->dump() << endl;
    trace() << dump_internals() << endl;
#endif
    remember_finished_job(client, exitcode);
    fd2client.erase(client->channel->fd);

    if (client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL) {
        finish_transfer_env(client, true);
    }

    if (client->status == Client::CLIENTWORK) {
        if (client->running_preprocess) {
            if (preprocess_active_processes > 0) {
                --preprocess_active_processes;
            }
            client->running_preprocess = false;
        } else if(client->fulljob) {
            clients.active_processes -= std::max((unsigned int)1, max_kids);
            if (fulljob_active > 0) {
                --fulljob_active;
            }
        } else {
            clients.active_processes--;
        }
    }
    client->fulljob = false;

    if (client->status == Client::WAITCOMPILE && exitcode == 119) {
        /* the client sent us a real good bye, so forget about the scheduler */
        client->job_id = 0;
    }

    /* Delete from the clients map before send_scheduler, which causes a
       double deletion. */
    if (!clients.erase(client->channel)) {
        log_error() << "client can't be erased: " << client->channel << endl;
        flush_debug();
        log_error() << dump_internals() << endl;
        flush_debug();
        assert(false);
    }

    if (scheduler && client->status != Client::WAITFORCHILD) {
        int job_id = client->job_id;
        bool use_client_id = false;

        if (client->status == Client::TOCOMPILE) {
            job_id = client->job->jobID();
        }

        if (client->status == Client::WAITFORCS) {
            // We don't know the job id, because we haven't received a reply
            // from the scheduler yet. Use client_id to identify the job,
            // the scheduler will use it for matching.
            use_client_id = true;
            assert( client->client_id > 0 );
        }

        if (job_id > 0 || use_client_id) {
            JobDoneMsg::from_type flag = JobDoneMsg::FROM_SUBMITTER;

            switch (client->status) {
            case Client::TOCOMPILE:
                flag = JobDoneMsg::FROM_SERVER;
                break;
            case Client::UNKNOWN:
            case Client::GOTNATIVE:
            case Client::JOBDONE:
            case Client::WAITFORCHILD:
            case Client::LINKJOB:
            case Client::TOINSTALL:
            case Client::WAITINSTALL:
            case Client::WAITCREATEENV:
                assert(false);   // should not have a job_id
                break;
            case Client::WAITCOMPILE:
            case Client::PENDING_USE_CS:
            case Client::CLIENTWORK:
            case Client::WAITFORCS:
                flag = JobDoneMsg::FROM_SUBMITTER;
                break;
            }

            trace() << "scheduler->send_msg( JobDoneMsg( " << client->dump() << ", " << exitcode << "))\n";

            JobDoneMsg msg(job_id, exitcode, flag, clients.size());
            if( use_client_id ) {
                msg.set_unknown_job_client_id( client->client_id );
            }
            if (!send_scheduler(msg)) {
                trace() << "failed to reach scheduler for remote job done msg!" << endl;
            }
        } else if (client->status == Client::CLIENTWORK) {
            // Clientwork && !job_id == LINK
            trace() << "scheduler->send_msg( JobLocalDoneMsg( " << client->client_id << ") );\n";

            if (!send_scheduler(JobLocalDoneMsg(client->client_id))) {
                trace() << "failed to reach scheduler for local job done msg!" << endl;
            }
        }
    }

    delete client;
}

void Daemon::clear_children()
{
    while (!clients.empty()) {
        Client *cl = clients.first();
        handle_end(cl, 116);
    }

    while (current_kids > 0) {
        int status;
        pid_t child;

        while ((child = waitpid(-1, &status, 0)) < 0 && errno == EINTR) {}

        current_kids--;
    }

    // they should be all in clients too
    assert(fd2client.empty());

    fd2client.clear();
    new_client_id = 0;
    trace() << "cleared children\n";
}

bool Daemon::handle_get_cs(Client *client, Msg *msg)
{
    GetCSMsg *umsg = dynamic_cast<GetCSMsg *>(msg);
    assert(client);
    if (!umsg->command_summary.empty()) {
        client->command_line = umsg->command_summary;
    } else if (client->command_line.empty() && client->channel) {
        client->command_line = command_line_from_peer_socket(client->channel->fd);
    }
    client->niceness = umsg->niceness;
    client->last_waitforcs_msec = 0;
    client->set_status(Client::WAITFORCS, scheduler ? "handle_get_cs: sent GetCS to scheduler" : "handle_get_cs: scheduler missing");
    umsg->client_id = client->client_id;
    trace() << "handle_get_cs " << umsg->client_id << endl;

    if (!scheduler) {
        /* now the thing is this: if there is no scheduler
           there is no point in trying to ask him. So we just
           redefine this as local job */
        client->usecsmsg = new UseCSMsg(umsg->target, "127.0.0.1", daemon_port,
                                        umsg->client_id, true, 1, 0);
        client->set_status(Client::PENDING_USE_CS, "handle_get_cs: scheduler missing, local compile");
        client->job_id = umsg->client_id;
        client->last_known_job_id = umsg->client_id;
        return true;
    }

    umsg->client_count = clients.size();
    umsg->command_summary.clear();

    return send_scheduler(*umsg);
}

int Daemon::handle_cs_conf(ConfCSMsg *msg)
{
    max_scheduler_pong = msg->max_scheduler_pong;
    max_scheduler_ping = msg->max_scheduler_ping;
    return 0;
}

bool Daemon::handle_local_job(Client *client, Msg *msg)
{
    JobLocalBeginMsg* m = dynamic_cast<JobLocalBeginMsg *>(msg);
    client->set_status(Client::LINKJOB, "handle_local_job: local-only job (legacy status=linkjob)");
    client->outfile = m->outfile;
    client->fulljob = m->fulljob;
    client->local_reason = m->local_reason.empty() ? "unknown" : m->local_reason;
    client->command_line = m->cmdline;
    client->local_preprocess = classify_local_preprocess_job(m->local_flags, m->cmdline, m->fulljob);
    client->running_preprocess = false;
    if (client->command_line.empty() && client->channel) {
        client->command_line = command_line_from_peer_socket(client->channel->fd);
        client->local_preprocess = classify_local_preprocess_job(m->local_flags, client->command_line, m->fulljob);
    }
    return true;
}

bool Daemon::handle_job_timing(Client *client, JobTimingMsg *m)
{
    if (!client || !m) {
        return false;
    }

    client->timing_submit_ts = m->submit_ts;
    client->timing_enqueue_msec = m->enqueue_msec;
    client->timing_start_msec = m->start_msec;
    client->timing_finish_msec = m->finish_msec;
    client->timing_waitforcs_msec = m->waitforcs_msec;
    client->timing_local_queue_msec = m->local_queue_msec;
    client->timing_exec_msec = m->exec_msec;
    client->timing_scheduler_job_id = m->scheduler_job_id;
    client->timing_compile_job_id = m->compile_job_id;
    client->timing_mode = m->mode;
    /* The compiler's real exit status.  Without it, history records only
       the daemon's teardown code (118 on close, 119 on EndMsg), which made
       every submitter-side job look like a failure in the telemetry.  */
    client->timing_exitcode = m->exitcode;
    client->has_timing = true;
    client->has_timing_exitcode = true;
    if (m->scheduler_job_id) {
        client->last_known_job_id = m->scheduler_job_id;
    }

    return true;
}

bool Daemon::handle_activity(Client *client)
{
    assert(client->status != Client::TOCOMPILE && client->status != Client::WAITINSTALL);

    Msg *msg = client->channel->get_msg(0, true);

    if (!msg) {
        handle_end(client, 118);
        return false;
    }

    bool ret = false;

    if (client->status == Client::TOINSTALL) {
        ret = handle_file_chunk_env(client, msg);
        delete msg;
        return ret;
    }

    switch (*msg) {
    case Msg::GET_NATIVE_ENV:
        ret = handle_get_native_env(client, dynamic_cast<GetNativeEnvMsg *>(msg));
        break;
    case Msg::COMPILE_FILE:
        ret = handle_compile_file(client, msg);
        break;
    case Msg::TRANFER_ENV:
        ret = handle_transfer_env(client, dynamic_cast<EnvTransferMsg*>(msg));
        break;
    case Msg::GET_CS:
        ret = handle_get_cs(client, msg);
        break;
    case Msg::GET_INTERNALS:
        ret = client->channel->send_msg(StatusTextMsg(dump_internals()));
        break;
    case Msg::END:
        handle_end(client, 119);
        ret = false;
        break;
    case Msg::JOB_LOCAL_BEGIN:
        ret = handle_local_job(client, msg);
        break;
    case Msg::JOB_DONE:
        ret = handle_job_done(client, dynamic_cast<JobDoneMsg *>(msg));
        break;
    case Msg::JOB_TIMING:
        ret = handle_job_timing(client, dynamic_cast<JobTimingMsg *>(msg));
        break;
    case Msg::VERIFY_ENV:
        ret = handle_verify_env(client, dynamic_cast<VerifyEnvMsg *>(msg));
        break;
    case Msg::BLACKLIST_HOST_ENV:
        ret = handle_blacklist_host_env(client, msg);
        break;
    default:
        log_error() << "protocol error " << msg->to_string() << " on client "
                    << client->dump() << endl;
        client->channel->send_msg(EndMsg());
        handle_end(client, 120);
        ret = false;
    }

    delete msg;
    return ret;
}

void Daemon::answer_client_requests()
{
#ifdef ICECC_DEBUG

    if (clients.size() + current_kids) {
        log_info() << dump_internals() << endl;
    }

    log_info() << "clients " << clients.dump_per_status() << " " << current_kids
               << " (" << max_kids << ")" << endl;

#endif

    /* reap zombies */
    int status;

    while (waitpid(-1, &status, WNOHANG) < 0 && errno == EINTR) {}

    handle_old_request();

    /* collect the stats after the children exited icecream_load */
    if (scheduler) {
        maybe_stats();
    }

    vector< pollfd > pollfds;
    pollfds.reserve( fd2client.size() + 6 );
    pollfd pfd; // tmp varible

    if (tcp_listen_fd != -1) {
        pfd.fd = tcp_listen_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }
    if (tcp_listen_local_fd != -1) {
        pfd.fd = tcp_listen_local_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    pfd.fd = unix_listen_fd;
    pfd.events = POLLIN;
    pollfds.push_back(pfd);

    if (web_listen_fd != -1
            && monotonic_msec() >= web_accept_backoff_until_msec) {
        pfd.fd = web_listen_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    /* Reap web connections that exceeded their idle or lifetime deadline
       BEFORE registering them; poll_timeout is capped below so a silent
       socket cannot postpone this forever.  */
    {
        const uint64_t now_msec = monotonic_msec();
        for (auto it = web_connections.begin(); it != web_connections.end();) {
            const WebConnection &wc = it->second;
            const int wfd = it->first;
            ++it;
            if (now_msec - wc.last_activity_msec > web_idle_deadline_msec
                    || now_msec - wc.created_msec > web_lifetime_deadline_msec) {
                drop_web_connection(wfd);
            }
        }
    }

    for (const auto &it : web_connections) {
        pfd.fd = it.first;
        pfd.events = it.second.reject_input ? 0 : POLLIN;
        if (it.second.outbuf_ofs < it.second.outbuf.size()) {
            pfd.events |= POLLOUT;
        }
        if (!pfd.events) {
            pfd.events = POLLOUT;   // 413 drain in flight; wake on writable
        }
        pollfds.push_back(pfd);
    }

    for (auto it = fd2client.begin(); it != fd2client.end();) {
        int i = it->first;
        Client *client = it->second;
        MsgChannel *c = client->channel;
        ++it;
        /* don't select on a fd that we're currently not interested in.
           Avoids that we wake up on an event we're not handling anyway */
        assert(client);
        int current_status = client->status;
        bool ignore_channel = current_status == Client::WAITFORCHILD ||
                              current_status == Client::WAITINSTALL;

        /* when the remote host is full with work, the wait time for it to free up and
           fork a child to compile could be long. If the input is ready to read, we will read
           them and save it for the child; otherwise the write on the client side would be blocked */
        if ( current_status == Client::TOCOMPILE ||
             (!ignore_channel && (!c->has_msg() || handle_activity(client)))) {
            pfd.fd = i;
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }

        if ((current_status == Client::WAITFORCHILD
                || current_status == Client::TOINSTALL
                || current_status == Client::WAITINSTALL)
              && client->pipe_from_child != -1) {
            pfd.fd = client->pipe_from_child;
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }
    }

    if (scheduler) {
        pfd.fd = scheduler->fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    } else if (discover && discover->listen_fd() >= 0) {
        /* We don't explicitely check for discover->get_fd() being in
        the selected set below.  If it's set, we simply will return
        and our call will make sure we try to get the scheduler.  */
        pfd.fd = discover->listen_fd();
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    for (map<string, NativeEnvironment>::const_iterator it = native_environments.begin();
            it != native_environments.end(); ++it) {
        if (it->second.create_env_pipe) {
            pfd.fd = it->second.create_env_pipe;
            pfd.events = POLLIN;
            pollfds.push_back(pfd);
        }
    }

    int poll_timeout_msec = max_scheduler_pong * 1000;
    if (state_dump_interval_s && (!state_jsonl_path.empty() || state_dump_log)) {
        const uint64_t now = monotonic_msec();
        if (!next_state_dump_msec) {
            // Schedule the first dump immediately.
            next_state_dump_msec = now;
        }
        if (now >= next_state_dump_msec) {
            poll_timeout_msec = 0;
        } else {
            const uint64_t to_dump_msec = next_state_dump_msec - now;
            if (to_dump_msec < uint64_t(poll_timeout_msec)) {
                poll_timeout_msec = int(to_dump_msec);
            }
        }
    }

    if (!web_connections.empty() || web_accept_backoff_until_msec > monotonic_msec()) {
        /* Deadline reaping and accept re-arming must run even if every web
           socket stays silent.  */
        if (poll_timeout_msec < 0 || poll_timeout_msec > 1000) {
            poll_timeout_msec = 1000;
        }
    }

    /* Telemetry draining no longer involves this loop at all: records go to
       AsyncJsonlWriter's own thread, so a slow filesystem cannot stretch a
       loop iteration and no early wakeup is needed.  */

    int ret = poll(pollfds.data(), pollfds.size(), poll_timeout_msec);

    if (ret < 0 && errno != EINTR) {
        log_perror("poll");
        close_scheduler();
        return;
    }
    // Reset debug if needed, but only if we aren't waiting for any child processes to finish,
    // otherwise their debug output could end up reset in the middle (and flush log marks used
    // by tests could be written out before debug output from children).
    if( current_kids == 0 ) {
        reset_debug_if_needed();
    }

    if (ret > 0) {
        bool had_scheduler = scheduler;

        if (scheduler && pollfd_is_set(pollfds, scheduler->fd, POLLIN)) {
            while (!scheduler->read_a_bit() || scheduler->has_msg()) {
                Msg *msg = scheduler->get_msg(0, true);

                if (!msg) {
                    log_warning() << "scheduler closed connection" << endl;
                    close_scheduler();
                    clear_children();
                    return;
                }

                ret = 0;

                switch (*msg) {
                case Msg::PING:

                    if (!IS_PROTOCOL_VERSION(27, scheduler)) {
                        ret = !send_scheduler(PingMsg());
                    }

                    break;
                case Msg::USE_CS:
                    ret = scheduler_use_cs(static_cast<UseCSMsg *>(msg));
                    break;
                case Msg::NO_CS:
                    ret = scheduler_no_cs(static_cast<NoCSMsg *>(msg));
                    break;
                case Msg::GET_INTERNALS:
                    ret = scheduler_get_internals();
                    break;
                case Msg::CS_CONF:
                    ret = handle_cs_conf(static_cast<ConfCSMsg *>(msg));
                    break;
                default:
                    log_error() << "unknown scheduler type " << msg->to_string() << endl;
                    ret = 1;
                }

                delete msg;

                if (ret) {
                    close_scheduler();
                    return;
                }
            }
        }

        if (web_listen_fd != -1 && pollfd_is_set(pollfds, web_listen_fd, POLLIN)) {
            handle_web_accept();
        }

        for (auto it = web_connections.begin(); it != web_connections.end();) {
            const int fd = it->first;
            ++it;

            short revents = 0;
            for (const auto &pollfd : pollfds) {
                if (pollfd.fd == fd) {
                    revents = pollfd.revents;
                    break;
                }
            }
            if (revents) {
                handle_web_connection(fd, revents);
            }
        }

        int listen_fd = -1;

        if (tcp_listen_fd != -1 && pollfd_is_set(pollfds, tcp_listen_fd, POLLIN)) {
            listen_fd = tcp_listen_fd;
        }
        if (tcp_listen_local_fd != -1 && pollfd_is_set(pollfds, tcp_listen_local_fd, POLLIN)) {
            listen_fd = tcp_listen_local_fd;
        }
        if (pollfd_is_set(pollfds, unix_listen_fd, POLLIN)) {
            listen_fd = unix_listen_fd;
        }

        if (listen_fd != -1) {
            struct sockaddr cli_addr;
            socklen_t cli_len = sizeof cli_addr;
            int acc_fd = accept(listen_fd, &cli_addr, &cli_len);

            if (acc_fd < 0) {
                if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    note_accept_error("client", errno);
                }
            } else {
                MsgChannel *c = Service::createChannel(acc_fd, &cli_addr, cli_len);

                if (c) {
                    Client *client = new Client;
                    client->client_id = ++new_client_id;
                    client->channel = c;
                    clients[c] = client;

                    fd2client[c->fd] = client;
                    trace() << "accepted " << c->fd << " " << c->name << " as " << client->client_id << endl;

                    while (!c->read_a_bit() || c->has_msg()) {
                        if (!handle_activity(client)) {
                            break;
                        }

                        if (client->status == Client::TOCOMPILE
                                || client->status == Client::WAITFORCHILD
                                || client->status == Client::WAITINSTALL) {
                            break;
                        }
                    }
                }
            }
        } else {
            for (auto it = fd2client.begin(); it != fd2client.end();)  {
                int i = it->first;
                Client *client = it->second;
                MsgChannel *c = client->channel;
                assert(client);
                ++it;

                if (client->status == Client::WAITFORCHILD
                        && client->pipe_from_child >= 0
                        && pollfd_is_set(pollfds, client->pipe_from_child, POLLIN)) {
                    if (!handle_compile_done(client)) {
                        return;
                    }
                }
                if ((client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL)
                        && client->pipe_from_child >= 0
                        && pollfd_is_set(pollfds, client->pipe_from_child, POLLIN)) {
                    if (!handle_env_install_child_done(client)) {
                        return;
                    }
                }

                if (pollfd_is_set(pollfds, i, POLLIN)) {
                    if( client->status == Client::TOCOMPILE )
                    {
                        /* read as the preprocessed input is ready but don't process it and leave it to the child
                           if we didn't read it now, the client would be blocked and timed out */
                        c->read_a_bit();
                    }
                    else
                    {
                        assert(client->status != Client::TOCOMPILE && client->status != Client::WAITINSTALL);

                        while (!c->read_a_bit() || c->has_msg()) {
                            if (!handle_activity(client)) {
                                break;
                            }

                            if (client->status == Client::TOCOMPILE
                                || client->status == Client::WAITFORCHILD
                                || client->status == Client::WAITINSTALL) {
                                break;
                            }
                        }
                    }
                }
            }

            for (map<string, NativeEnvironment>::iterator it = native_environments.begin();
                 it != native_environments.end(); ) {
                if (it->second.create_env_pipe && pollfd_is_set(pollfds, it->second.create_env_pipe, POLLIN)) {
                    if(!create_env_finished(it->first))
                    {
                        native_environments.erase(it++);
                        continue;
                    }
                }
                ++it;
            }
        }

        if (had_scheduler && !scheduler) {
            clear_children();
            return;
        }

    }
}

bool Daemon::reconnect()
{
    if (scheduler) {
        return true;
    }

    if (!discover && next_scheduler_connect > time(nullptr)) {
        trace() << "Delaying reconnect." << endl;
        return false;
    }

#ifdef ICECC_DEBUG
    trace() << "reconn " << dump_internals() << endl;
#endif

    if (!discover || (nullptr == (scheduler = discover->try_get_scheduler()) && discover->timed_out())) {
        delete discover;
        discover = new DiscoverSched(netname, max_scheduler_pong, schedname, scheduler_port);
    }

    if (!scheduler) {
        log_warning() << "scheduler not yet found/selected." << endl;
        return false;
    }

    delete discover;
    discover = nullptr;
    sockaddr_in name;
    socklen_t len = sizeof(name);
    int error = getsockname(scheduler->fd, (struct sockaddr*)&name, &len);

    if (!error) {
        remote_name = inet_ntoa(name.sin_addr);
    } else {
        remote_name = string();
    }

    log_info() << "Connected to scheduler (I am known as " << remote_name << ")" << endl;
    current_load = -1000;
    gettimeofday(&last_stat, nullptr);
    icecream_load = 0;

    LoginMsg lmsg(daemon_port, determine_nodename(), machine_name, supported_features);
    lmsg.envs = available_environments(envbasedir);
    lmsg.max_kids = max_kids;
    lmsg.noremote = noremote;
    return send_scheduler(lmsg);
}

int Daemon::working_loop()
{
    for (;;) {
        reconnect();
        answer_client_requests();
        maybe_dump_state();

        if (exit_main_loop) {
            close_scheduler();
            clear_children();
            close_web();
            break;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int max_processes = -1;
    int max_preprocess_processes = -1;
    srand(time(nullptr) + getpid());

    Daemon d;

    int debug_level = Error;
    string logfile;
    bool detach = false;
    nice_level = 5; // defined in serve.h

    while (true) {
        int option_index = 0;
        static const struct option long_options[] = {
            { "netname", 1, nullptr, 'n' },
            { "max-processes", 1, nullptr, 'm' },
            { "max-preprocess", 1, nullptr, 0 },
            { "fulljob-policy", 1, nullptr, 0 },
            { "help", 0, nullptr, 'h' },
            { "daemonize", 0, nullptr, 'd'},
            { "log-file", 1, nullptr, 'l'},
            { "nice", 1, nullptr, 0},
            { "name", 1, nullptr, 'N'},
            { "scheduler-host", 1, nullptr, 's' },
            { "env-basedir", 1, nullptr, 'b' },
            { "user-uid", 1, nullptr, 'u'},
            { "cache-limit", 1, nullptr, 0},
            { "no-remote", 0, nullptr, 0},
            { "interface", 1, nullptr, 'i'},
            { "port", 1, nullptr, 'p'},
            { "state-jsonl", 1, nullptr, 0},
            { "state-interval", 1, nullptr, 0},
            { "state-log", 0, nullptr, 0},
            { "webgui", 0, nullptr, 0},
            { "webgui-port", 1, nullptr, 0},
            { "webgui-addr", 1, nullptr, 0},
            { nullptr, 0, nullptr, 0 }
        };

        const int c = getopt_long(argc, argv, "N:n:m:l:s:hvdb:u:i:p:", long_options, &option_index);

        if (c == -1) {
            break;    // eoo
        }

        switch (c) {
        case 0: {
            string optname = long_options[option_index].name;

            if (optname == "nice") {
                if (optarg && *optarg) {
                    errno = 0;
                    int tnice = atoi(optarg);

                    if (!errno) {
                        nice_level = tnice;
                    }
                } else {
                    usage("Error: --nice requires argument");
                }
            } else if (optname == "name") {
                if (optarg && *optarg) {
                    d.nodename = optarg;
                } else {
                    usage("Error: --name requires argument");
                }
            } else if (optname == "cache-limit") {
                if (optarg && *optarg) {
                    errno = 0;
                    int mb = atoi(optarg);

                    if (!errno) {
                        cache_size_limit = mb * 1024 * 1024;
                    }
                } else {
                    usage("Error: --cache-limit requires argument");
                }
            } else if (optname == "no-remote") {
                d.noremote = true;
            } else if (optname == "fulljob-policy") {
                if (optarg && strcmp(optarg, "exclusive") == 0) {
                    fulljob_policy = FULLJOB_EXCLUSIVE;
                } else if (optarg && strcmp(optarg, "compile-lane") == 0) {
                    fulljob_policy = FULLJOB_COMPILE_LANE;
                } else {
                    log_error() << "invalid --fulljob-policy='"
                                << (optarg ? optarg : "")
                                << "' (expected compile-lane or exclusive)" << endl;
                    usage();
                }
            } else if (optname == "max-preprocess") {
                if (optarg && *optarg) {
                    max_preprocess_processes = atoi(optarg);
                    if (max_preprocess_processes <= 0) {
                        usage("Error: --max-preprocess requires positive integer argument");
                    }
                } else {
                    usage("Error: --max-preprocess requires argument");
                }
            } else if (optname == "state-jsonl") {
                if (optarg && *optarg) {
                    d.state_jsonl_path = optarg;
                    d.state_writer.set_path(optarg);
                    if (!d.state_dump_interval_s) {
                        d.state_dump_interval_s = 30;
                    }
                } else {
                    usage("Error: --state-jsonl requires argument");
                }
            } else if (optname == "state-interval") {
                if (optarg && *optarg) {
                    errno = 0;
                    int interval = atoi(optarg);
                    if (errno || interval <= 0) {
                        usage("Error: --state-interval requires a positive integer");
                    }
                    d.state_dump_interval_s = interval;
                } else {
                    usage("Error: --state-interval requires argument");
                }
            } else if (optname == "state-log") {
                d.state_dump_log = true;
                if (!d.state_dump_interval_s) {
                    d.state_dump_interval_s = 30;
                }
            } else if (optname == "webgui") {
                d.webgui_enabled = true;
            } else if (optname == "webgui-port") {
                if (optarg && *optarg) {
                    errno = 0;
                    int port = atoi(optarg);
                    if (errno || port <= 0 || port > 65535) {
                        usage("Error: --webgui-port requires a valid TCP port");
                    }
                    d.webgui_port = port;
                    d.webgui_enabled = true;
                } else {
                    usage("Error: --webgui-port requires argument");
                }
            } else if (optname == "webgui-addr") {
                if (optarg && *optarg) {
                    string addr = optarg;
                    if (addr.empty()) {
                        usage("Error: --webgui-addr requires argument");
                    }
                    d.webgui_addr = addr;
                    d.webgui_enabled = true;
                } else {
                    usage("Error: --webgui-addr requires argument");
                }
            }

        }
        break;
        case 'd':
            detach = true;
            break;
        case 'N':

            if (optarg && *optarg) {
                d.nodename = optarg;
            } else {
                usage("Error: -N requires argument");
            }

            break;
        case 'l':

            if (optarg && *optarg) {
                logfile = optarg;
            } else {
                usage("Error: -l requires argument");
            }

            break;
        case 'v':

            if (debug_level < MaxVerboseLevel) {
                debug_level++;
            }

            break;
        case 'n':

            if (optarg && *optarg) {
                d.netname = optarg;
            } else {
                usage("Error: -n requires argument");
            }

            break;
        case 'm':

            if (optarg && *optarg) {
                max_processes = atoi(optarg);
            } else {
                usage("Error: -m requires argument");
            }

            break;
        case 's':

            if (optarg && *optarg) {
                string scheduler = optarg;
                size_t colon = scheduler.rfind( ':' );
                if( colon == string::npos ) {
                    d.schedname = scheduler;
                } else {
                    d.schedname = scheduler.substr(0, colon);
                    d.scheduler_port = atoi( scheduler.substr( colon + 1 ).c_str());
                    if( d.scheduler_port == 0 ) {
                        usage("Error: -s requires valid port if hostname includes colon");
                    }
                }
            } else {
                usage("Error: -s requires hostname argument");
            }

            break;
        case 'b':

            if (optarg && *optarg) {
                d.envbasedir = optarg;
            }

            break;
        case 'u':

            if (optarg && *optarg) {
                struct passwd *pw = getpwnam(optarg);

                if (!pw) {
                    usage("Error: -u requires a valid username");
                } else {
                    d.user_uid = pw->pw_uid;
                    d.user_gid = pw->pw_gid;
                    d.warn_icecc_user_errno = 0;

                    if (!d.user_gid || !d.user_uid) {
                        usage("Error: -u <username> must not be root");
                    }
                }
            } else {
                usage("Error: -u requires a valid username");
            }

            break;
        case 'i':

            if (optarg && *optarg) {
                string daemon_interface = optarg;
                if (daemon_interface.empty()) {
                    usage("Error: Invalid network interface specified");
                }

                d.daemon_interface = daemon_interface;
            } else {
                usage("Error: -i requires argument");
            }

            break;
        case 'p':

            if (optarg && *optarg) {
                d.daemon_port = atoi(optarg);

                if (0 == d.daemon_port) {
                    usage("Error: Invalid port specified");
                }
            } else {
                usage("Error: -p requires argument");
            }

            break;
        default:
            usage();
        }
    }

    if (d.warn_icecc_user_errno != 0) {
        log_errno("No icecc user on system. Falling back to nobody.", d.warn_icecc_user_errno);
    }

    umask(022);

    bool remote_disabled = false;
    if (getuid() == 0) {
        if (!logfile.length() && detach) {
            mkdir("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
            chmod("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
            ignore_result(chown("/var/log/icecc", d.user_uid, d.user_gid));
            logfile = "/var/log/icecc/iceccd.log";
        }

        mkdir("/var/run/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        chmod("/var/run/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        ignore_result(chown("/var/run/icecc", d.user_uid, d.user_gid));

#ifdef HAVE_LIBCAP_NG
        capng_clear(CAPNG_SELECT_BOTH);
        capng_update(CAPNG_ADD, (capng_type_t)(CAPNG_EFFECTIVE | CAPNG_PERMITTED), CAP_SYS_CHROOT);
        int r = capng_change_id(d.user_uid, d.user_gid,
                                (capng_flags_t)(CAPNG_DROP_SUPP_GRP | CAPNG_CLEAR_BOUNDING));
        if (r) {
            log_error() << "Error: capng_change_id failed: " << r << endl;
            exit(EXIT_SETUID_FAILED);
        }
#endif
    } else {
#ifdef HAVE_LIBCAP_NG
        // It's possible to have the capability even without being root.
        if (!capng_have_capability( CAPNG_EFFECTIVE, CAP_SYS_CHROOT )) {
#else
        {
#endif
            d.noremote = true;
            remote_disabled = true;
        }
    }

    setup_debug(debug_level, logfile);

    const char *web_hostport_env = getenv("ICECC_WEB_HOSTPORT");
    if (web_hostport_env && *web_hostport_env && d.webgui_enabled) {
        /* Explicit command-line configuration wins; the environment is a
           deployment convenience, not an override.  */
        log_info() << "ICECC_WEB_HOSTPORT ignored: web gui already configured "
                   << "on the command line (" << d.webgui_addr << ":" << d.webgui_port << ")" << endl;
    } else if (web_hostport_env && *web_hostport_env) {
        d.webgui_best_effort = true;

        const string web_hostport(web_hostport_env);
        const size_t colon = web_hostport.rfind(':');
        string web_host;
        string web_port_str;

        if (colon == string::npos) {
            web_host = d.webgui_addr;
            web_port_str = web_hostport;
        } else {
            web_host = web_hostport.substr(0, colon);
            web_port_str = web_hostport.substr(colon + 1);
        }

        if (web_host.empty()) {
            /* Same meaning as everywhere else: an empty host inherits the
               default bind address (loopback), it does not silently widen
               to all interfaces.  */
            web_host = d.webgui_addr;
        } else if (web_host == "localhost") {
            web_host = "127.0.0.1";
        }

        errno = 0;
        char *end = nullptr;
        const long web_port_long = strtol(web_port_str.c_str(), &end, 10);
        if (errno != 0 || end == web_port_str.c_str() || (end && *end) || web_port_long <= 0 || web_port_long > 65535) {
            log_warning() << "Ignoring ICECC_WEB_HOSTPORT='" << web_hostport
                          << "' (expected <addr>:<port> or <port>)" << endl;
        } else {
            d.webgui_enabled = true;
            d.webgui_addr = web_host;
            d.webgui_port = int(web_port_long);
            log_info() << "web gui configured from ICECC_WEB_HOSTPORT ("
                       << d.webgui_addr << ":" << d.webgui_port << ")" << endl;
        }
    }

    log_info() << "ICECREAM daemon " VERSION " starting up (nice level "
               << nice_level << ") " << endl;
    if (d.state_dump_interval_s && (!d.state_jsonl_path.empty() || d.state_dump_log)) {
        log_info() << "state dumps enabled: interval " << d.state_dump_interval_s << "s"
                   << ", jsonl=" << (d.state_jsonl_path.empty() ? "<disabled>" : d.state_jsonl_path)
                   << ", log=" << (d.state_dump_log ? "true" : "false") << endl;
    }
    if (d.webgui_enabled) {
        log_info() << "web gui requested on " << d.webgui_addr << ":" << d.webgui_port << endl;
    }
    if (remote_disabled)
        log_warning() << "Cannot use chroot, no remote jobs accepted." << endl;
    if (d.noremote)
        d.daemon_port = 0;

    d.determine_system();

    if (chdir("/") != 0) {
        log_error() << "failed to switch to root directory: "
                    << strerror(errno) << endl;
        exit(EXIT_DISTCC_FAILED);
    }

    if (detach)
        if (daemon(0, 0)) {
            log_perror("Failed to run as a daemon.");
            exit(EXIT_DISTCC_FAILED);
        }

    if (dcc_ncpus(&d.num_cpus) == 0) {
        log_info() << d.num_cpus << " CPU(s) online on this server" << endl;
    }

    if (max_processes < 0) {
        max_kids = d.num_cpus;
    } else {
        max_kids = max_processes;
    }

    if (max_preprocess_processes > 0) {
        max_preprocess_kids = (unsigned int)max_preprocess_processes;
    } else {
        /* Conservative capped default (min(2*compile capacity, 32)): the
           earlier 8x default allowed e.g. 512 concurrent cpp processes on a
           64-core host; niceness only shields CPU, not resident memory,
           page cache, fds, or temp-file pressure.  Operators who profiled
           their preprocess load can raise this explicitly via
           --max-preprocess.  */
        max_preprocess_kids = std::min(2u * std::max(1u, max_kids), 32u);
    }

    if (fulljob_policy == FULLJOB_EXCLUSIVE && !d.noremote) {
        /* On a remote-capable daemon the barrier cannot be honoured: the
           scheduler keeps assigning remote jobs, which refill the compile
           lane, so the waiting fulljob may never see an idle node.  Honest
           refusal beats a silently ineffective policy; whole-node isolation
           belongs on --no-remote submitter daemons (where big local links
           run).  A future version can announce the reservation to the
           scheduler and support this everywhere.  */
        log_error() << "--fulljob-policy=exclusive requires --no-remote "
                    << "(a remote-capable daemon cannot drain its compile lane; "
                    << "the scheduler keeps assigning jobs to it)" << endl;
        usage();
    }
    log_info() << "fulljob policy: "
               << (fulljob_policy == FULLJOB_EXCLUSIVE ? "exclusive (whole node)"
                                                       : "compile-lane reservation") << endl;
    log_info() << "allowing up to " << max_kids << " active compile jobs and "
               << max_preprocess_kids << " active preprocess jobs" << endl;

    d.determine_supported_features();
    log_info() << "supported features: " << supported_features_to_string(d.supported_features) << endl;

    int ret;

    /* Still create a new process group, even if not detached */
    trace() << "not detaching\n";

    if ((ret = set_new_pgrp()) != 0) {
        return ret;
    }

    /* Don't catch signals until we've detached or created a process group. */
    dcc_daemon_catch_signals();

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << endl;
        exit(EXIT_DISTCC_FAILED);
    }

    if (signal(SIGCHLD, SIG_DFL) == SIG_ERR) {
        log_warning() << "signal(SIGCHLD) failed: " << strerror(errno) << endl;
        exit(EXIT_DISTCC_FAILED);
    }

    /* This is called in the master daemon, whether that is detached or
     * not.  */
    dcc_master_pid = getpid();

    ofstream pidFile;
    string progName = argv[0];
    progName = find_basename(progName);
    pidFilePath = string(RUNDIR) + string("/") + progName + string(".pid");
    pidFile.open(pidFilePath.c_str());
    pidFile << dcc_master_pid << endl;
    pidFile.close();

    if (!cleanup_cache(d.envbasedir, d.user_uid, d.user_gid)) {
        return 1;
    }

    list<string> nl = get_netnames(200, d.scheduler_port);
    trace() << "Netnames:" << endl;

    for (list<string>::const_iterator it = nl.begin(); it != nl.end(); ++it) {
        trace() << *it << endl;
    }

    if (!d.setup_listen_fds()) { // error
        return 1;
    }

    return d.working_loop();
}
