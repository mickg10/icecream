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
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <unordered_map>
#include <unordered_set>
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
#include "statewriter.h"
#include "client_queue_order.h"
#include <comm.h>
#include "load.h"
#include "environment.h"
#include "platform.h"
#include "util.h"
#include "getifaddrs.h"
#include "p50_daemon_sidecar_adapter.h"
#include "p50_completion_record.h"
#include "p50_cache_recovery_policy.h"
#include "p50_daemon_cache_dispatch.h"
#include "p50_input_wait.h"
#include "connection_provenance.h"
#include "p50_source_arm_wait_lease.h"
#include "compiler_group_signal.h"

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

// The wire carries only this bounded relative budget.  Each F client retains
// an absolute steady-clock deadline; ordinary traffic, replay, and a renewed
// sidecar lease never extend it.
static constexpr uint64_t kP50SourceArmBudgetMsec = 60000;

/* The qualified topology supports four persistent relationships with up to
   thirty live jobs each. These pending poll records do not consume
   control-worker threads; keep their bound aligned with that 120-job topology
   while leaving worker admission separate. */
static constexpr size_t kMaxPendingP51SourceOperations = 120;

static constexpr bool p51_source_operation_capacity_available(
    size_t pending) noexcept
{
    return pending < kMaxPendingP51SourceOperations;
}

static_assert(p51_source_operation_capacity_available(119));
static_assert(!p51_source_operation_capacity_available(120));

static uint64_t p50_source_arm_budget_msec() noexcept
{
    uint64_t budget = kP50SourceArmBudgetMsec;
    // Tests may shorten the wait to exercise the silent-expiry sweep.  The
    // override is accepted only under ICECC_TESTS and remains inside the
    // same nonzero wire bound; production has one fixed 60-second budget.
    if (getenv("ICECC_TESTS") != nullptr) {
        const char *text = getenv("ICECC_TEST_P50_SOURCE_BUDGET_MSEC");
        if (text != nullptr && *text != '\0') {
            char *end = nullptr;
            const unsigned long long parsed = strtoull(text, &end, 10);
            if (end != text && *end == '\0' && parsed != 0 &&
                parsed <= P50SourceArmedFields::MaxSourceBudgetMsec) {
                budget = static_cast<uint64_t>(parsed);
            }
        }
    }
    return budget;
}

static uint64_t next_daemon_generation()
{
    // This counter is process-local and intentionally never belongs to the
    // scheduler reconnect/clear_children lifecycle.  A daemon instance gets
    // one immutable nonzero namespace for all accepted wrappers.
    static uint64_t generation = 0;
    if (generation == std::numeric_limits<uint64_t>::max())
        return generation; // retain nonzero state; allocation then fails closed
    return ++generation;
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
     * WAITP50INPUT: Protocol-50 CompileFile arm was accepted; wait for the
     *               exact ready envelope and sealed source FD before queueing
     *               a compiler.
     * TOCOMPILE: We're supposed to compile it ourselves
     * WAITFORCS: Client asked for a CS and we asked the scheduler - waiting for its answer
     * WAITCOMPILE: Client got a CS and will ask him now (it's not me)
     * CLIENTWORK: Client is busy working and we reserve the spot (job_id is set if it's a scheduler job)
     * WAITFORCHILD: Client is waiting for the compile job to finish.
     * WAITCREATEENV: We're waiting for icecc-create-env to finish.
     */
    enum Status { UNKNOWN, GOTNATIVE, PENDING_USE_CS, JOBDONE, LINKJOB, TOINSTALL, WAITINSTALL, WAITP50INPUT, TOCOMPILE,
                  WAITFORCS, FORWARDING_USE_CS, WAITCOMPILE, CLIENTWORK, WAITFORCHILD, WAITCREATEENV,
                  LASTSTATE = WAITCREATEENV
                } status;

    /* S2: a validated, assignment-bound cache-endpoint handoff for exactly
       one in-flight UseCS.  Populated only when the scheduler's UseCS
       carried BOTH a fully-valid cache tail (see
       cache_advertisement_is_valid_present) AND a nonzero P50 assignment
       identity -- the identity the daemon will actually claim the job
       with (see scheduler_use_cs).  Every field below is bound from the
       SAME UseCS frame: wireJobId/assignmentEpoch/assignmentNonce identify
       exactly which assignment this binding belongs to (so a later
       consumer can refuse to reuse it under a different assignment); host
       is the frame's single host authority for BOTH endpoints (the
       ordinary compile port and the cache port are two listeners on that
       one host); ordinaryPort is the selected F's compile port, matching
       reply.port on the wire.  Every field but `valid` is meaningless
       unless valid is true.  Retained alongside -- not inside -- the
       existing CompileJob *job below; this change makes no connection to
       host:cachePort. */
    struct CacheHandoff {
        bool valid;
        uint32_t wireJobId;
        uint64_t assignmentEpoch;
        uint64_t assignmentNonce;
        string host;
        uint32_t ordinaryPort;
        uint32_t cachePort;
        uint32_t cacheProtocol;
        uint32_t cacheProfileMask;
        uint64_t cGuid;
        uint64_t tuSeq;
        uint64_t routeStateGeneration;
        std::optional<icecc::p50::sidecar::ReadyLease> readyLease;
    };

    struct DeferredP50CacheFdRequest {
        P50CacheSessionFdRequestFields request;
        P50CacheFdReplyTicket replyTicket;
    };

    struct PendingP51SourceLease {
        P51SourceLeaseRequestFields request;
        P51CacheFdReplyTicket reply_ticket;
        icecc::p50::sidecar::ReadyLease ready_lease;
        std::chrono::steady_clock::time_point deadline{};
        std::unique_ptr<icecc::p50::local::UnixConnectOperation> connect;
        std::unique_ptr<icecc::p50::local::Connection> control;
        std::unique_ptr<icecc::p50::local::FrameOperation> frame;
        bool hello_sent = false;
    };

    struct PendingP51SourceArm {
        enum class Stage : uint8_t {
            Connecting = 0,
            HelloSend,
            HelloAckRead,
            ReservationSend,
            ReservationReplyRead,
            GoodbyeSend,
        };
        P51SourceArmFields arm;
        icecc::p50::sidecar::ReadyLease ready_lease;
        std::chrono::steady_clock::time_point deadline{};
        uint64_t deadline_msec = 0;
        icecc::p50::sidecar::AbsoluteMonotonicDeadline absolute_deadline{};
        std::unique_ptr<icecc::p50::local::UnixConnectOperation> connect;
        std::unique_ptr<icecc::p50::local::Connection> control;
        std::unique_ptr<icecc::p50::local::FrameOperation> frame;
        std::optional<P51SourceArmedFields> armed;
        uint16_t reservation_error_code = 0;
        Stage stage = Stage::Connecting;
    };

    struct PendingP51SourceCancel {
        enum class Stage : uint8_t {
            HelloSend = 0,
            HelloAckRead,
            CancelSend,
            CancelReplyRead,
            GoodbyeSend,
        };
        icecc::p50::local::P51SourceReservationCancel request;
        icecc::p50::sidecar::ReadyLease ready_lease;
        std::chrono::steady_clock::time_point deadline{};
        std::unique_ptr<icecc::p50::local::UnixConnectOperation> connect;
        std::unique_ptr<icecc::p50::local::Connection> control;
        std::unique_ptr<icecc::p50::local::FrameOperation> frame;
        Stage stage = Stage::HelloSend;
    };

    enum class P50InputLeaseState : uint8_t {
        None = 0,
        Active,
        AttemptSettled,
        TerminalSettled,
    };

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
        p50_input_fd = -1;
        p50_input_lease_state = P50InputLeaseState::None;
        cacheHandoff = CacheHandoff{};
        usecsmsg = nullptr;
        deferred_getcs = nullptr;
        deferred_getcs_waits_for_cache = false;
        deferred_p50_cache_fd_request.reset();
        getcs_published = false;
        getcs_outstanding = false;
        getcs_generation = 0;
        cache_offer_generation = 0;
        cache_offer = {};
        cache_offer_lease.reset();
        local_owner_generation = 0;
        getcs_expected = 0;
        getcs_delivered = 0;
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

    // These are the only legal F-daemon transitions for the two-phase P50
    // source seam.  The production private-listener adapter is intentionally
    // a later transplant; keeping the reducer here prevents future callers
    // from bypassing WAITP50INPUT and queueing a compiler early.
    bool arm_p50_source(const icecc::p50::P50SourceArm& source_arm) {
        if (!p50_input_wait.arm_input(source_arm))
            return false;
        set_status(WAITP50INPUT, "p50: source arm ACKed; waiting for exact input");
        return true;
    }

    bool accept_p50_input(const icecc::p50::P50InputReady& ready, int sealed_fd) {
        if (status != WAITP50INPUT || p50_input_fd >= 0 ||
            !p50_input_wait.accept_ready(ready, sealed_fd))
            return false;
        p50_input_fd = p50_input_wait.take_for_fork();
        if (p50_input_fd < 0)
            return false;
        set_status(TOCOMPILE, "p50: exact ready/sealed input attached");
        return true;
    }

    bool accept_p50_input(const P50SourceArmFields& fields,
                          const icecc::p50::P50InputReady& ready,
                          int sealed_fd) {
        if (status != WAITP50INPUT || p50_input_fd >= 0 ||
            !p50_source_arm_fields.has_value() ||
            *p50_source_arm_fields != fields ||
            !p50_input_wait.accept_ready(fields, ready, sealed_fd))
            return false;
        p50_input_fd = p50_input_wait.take_for_fork();
        if (p50_input_fd < 0)
            return false;
        set_status(TOCOMPILE, "p50: exact canonical ready/sealed input attached");
        return true;
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
        case WAITP50INPUT:
            return "waitp50input";
        case TOCOMPILE:
            return "tocompile";
        case WAITFORCS:
            return "waitforcs";
        case FORWARDING_USE_CS:
            return "forwarding_use_cs";
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
        delete deferred_getcs;
        deferred_getcs = nullptr;
        delete job;
        job = nullptr;

        if (p50_input_fd >= 0) {
            const int descriptor = p50_input_fd;
            p50_input_fd = -1;
            (void)close(descriptor);
        }

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
    GetCSMsg *deferred_getcs;   // G4: GetCS held during LOGIN_ATTEMPT, re-driven on ConfCS
    bool deferred_getcs_waits_for_cache;  // cache-capable request held only across a bounded in-progress sidecar replacement
    // A wrapper can receive UseCS immediately before the supervised C sidecar
    // reaches its bounded process-lifetime limit.  Keep that exact
    // assignment's one descriptor request private while the replacement
    // lifecycle advances; the successor READY lease services it without
    // closing the ordinary submitter proxy or minting another scheduler job.
    std::optional<DeferredP50CacheFdRequest>
        deferred_p50_cache_fd_request;
    std::unique_ptr<PendingP51SourceLease> pending_p51_source_lease;
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    uint32_t test_p51_source_lease_requests = 0;
#endif
    std::unique_ptr<PendingP51SourceArm> pending_p51_source_arm;
    bool getcs_published;       // G4 (17:20#1): true only once a GetCS for this client has been sent to S (which then owns it by client_id); a held request is PRIVATE until then
    bool getcs_outstanding;     // G4 (bigoracle 18:45 P0): a GetCS occupies this client from accept until client destruction; a second GetCS in ANY non-terminal state is rejected (not just while WAITFORCS)
    uint64_t getcs_generation;  // G4 (bigoracle 18:45 P0): the session generation the request was published under (0 = unpublished); a scheduler reply is honored only when it matches the current ACTIVE generation
    uint64_t cache_offer_generation;  // exact active scheduler generation that owns cache_offer
    P50CacheClientCapability cache_offer;  // exact post-intersection GetCS offer, including canonical absence
    std::optional<icecc::p50::sidecar::ReadyLease> cache_offer_lease;
    uint64_t local_owner_generation;  // G4 (18:21): session generation that owns a started LOCAL assignment (0 = UNOWNED_LOCAL, started during LOGIN_ATTEMPT/offline); only an ACTIVE_SESSION(g)-owned job emits JobLocalBegin/JobLocalDone to S
    // G4 (local-oracle 21:19): count>1 -> BATCH_LEDGER mode.  count<=1 keeps the
    // scalar SCALAR_ONE path untouched.  A request selects one immutable mode at
    // accept and never crosses.
    uint32_t getcs_expected;   // GetCSMsg::count captured at accept (0 for the SCALAR_ONE path)
    uint32_t getcs_delivered;  // UseCS decisions delivered to the client for a batch request
    std::vector<uint32_t> getcs_batch_jobids;  // exact job ids recorded for a batch request (dedup + teardown settlement)
    CompileJob *job;
    // One compiler-attempt-owned, sealed InputRecord descriptor.  It is
    // populated only after an exact authenticated sidecar attachment and is
    // transferred exactly once to handle_connection(); legacy jobs keep -1.
    int p50_input_fd;
    // The descriptor above can move into the compiler child, but this exact
    // logical-owner observation remains in the daemon parent until teardown.
    // An ordinary disconnect/cancel settles only the assignment attempt; a
    // terminal close is legal only after an explicit result disposition.
    std::optional<icecc::p50::InputFdRequest> p50_input_lease;
    P50InputLeaseState p50_input_lease_state;
    // P50 child records are readiness-driven in bounded nonblocking steps;
    // retaining the reader across POLLIN/POLLHUP prevents a partial writer or
    // write-before-close race from blocking the daemon event loop.
    std::optional<icecc::p50::P50CompletionRecordReader>
        p50_completion_reader;
    // Immutable accept-time wrapper provenance.  Async work carries only the
    // value lease; the daemon registry is the sole pointer re-entry point.
    ConnectionProvenance connection_provenance;
    CacheHandoff cacheHandoff;   // S2: see the struct's own comment above
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
    // F-side Protocol-50 wait owner.  The future attachment adapter must call
    // arm_input()/accept_ready(); it must never set TOCOMPILE directly for an
    // armed P50 job.
    icecc::p50::daemon::P50InputWaitState p50_input_wait;

    // Complete ordinary-wire source-arm ownership.  The compact reducer
    // projection above is only a state gate; these values are the authority
    // used for later CompileFile matching and teardown settlement.
    std::optional<P50SourceArmFields> p50_source_arm_fields;
    std::optional<P51SourceArmFields> p51_source_arm_fields;
    std::optional<P51SourceArmedFields> p51_source_armed_fields;
    std::optional<icecc::p50::sidecar::ReadyLease> p50_source_f_lease;
    std::optional<icecc::p50::sidecar::AbsoluteMonotonicDeadline>
        p51_source_absolute_deadline;
    // Retain the complete ACK, not just a compact lease projection.  The
    // later cache-session join must be derivable from the canonical arm plus
    // every F-side ACK fact without consulting a second mutable table.
    std::optional<P50SourceArmedMsg> p50_source_armed_ack;
    // Preserve the exact F generation alongside the immutable ReadyLease so
    // a later cache-session join cannot substitute a replacement generation.
    uint64_t p50_source_f_store_generation = 0;
    ConnectionProvenance p50_source_arm_provenance;
    uint64_t p50_source_deadline_msec = 0;
    bool p50_source_compile_pending = false;
    std::unique_ptr<icecc::p50::InputFdAttachmentOperation> p50_attachment;
    std::unique_ptr<CompileJob> p50_attachment_job;
    std::optional<icecc::p50::InputFdAttachmentResult> p50_attachment_result;
    std::optional<icecc::p50::InputFdRequest> p50_attachment_lease;
    uint64_t p50_attachment_started_msec = 0;
    // CACHE_SESSION transfers this wrapper's public descriptor to the sidecar
    // but its source-arm assignment remains live until the ordinary compile
    // connection (or its deadline) settles it. Such a wrapper has no fd to
    // poll; retaining it in Clients keeps the assignment owner alive.
    bool p50_cache_session_detached = false;

    bool arm_p50_source(const P50SourceArmFields& fields,
                        const icecc::p50::sidecar::ReadyLease& lease,
                        uint64_t observation,
                        uint64_t source_deadline_msec) {
        const uint64_t now = monotonic_msec();
        if (status != UNKNOWN || p50_source_arm_fields.has_value() ||
            !fields.valid() || !lease.valid() || observation == 0 ||
            lease.identity.generation == 0 ||
            source_deadline_msec <= now ||
            source_deadline_msec - now > P50SourceArmedFields::MaxSourceBudgetMsec ||
            fields.c_store_derivation_version != lease.store_derivation_version ||
            !connection_provenance.lease.valid()) {
            log_warning() << "P50 source arm installation failed: precondition"
                          << " status=" << static_cast<unsigned>(status)
                          << " retained=" << p50_source_arm_fields.has_value()
                          << " fields=" << fields.valid()
                          << " lease=" << lease.valid()
                          << " observation=" << (observation != 0)
                          << " generation=" << (lease.identity.generation != 0)
                          << " deadline_future=" << (source_deadline_msec > now)
                          << " budget_bounded="
                          << (source_deadline_msec > now &&
                              source_deadline_msec - now <=
                                  P50SourceArmedFields::MaxSourceBudgetMsec)
                          << " derivation="
                          << (fields.c_store_derivation_version ==
                              lease.store_derivation_version)
                          << " provenance=" << connection_provenance.lease.valid()
                          << endl;
            return false;
        }

        if (!p50_input_wait.arm_input(fields)) {
            log_warning() << "P50 source arm installation failed: input wait"
                          << " state="
                          << static_cast<unsigned>(p50_input_wait.state())
                          << " fields=" << fields.valid() << endl;
            return false;
        }

        ClaimAttemptCapability128 attempt_capability_1;
        ClaimAttemptCapability128 attempt_capability_2;
        if (!fresh_claim_attempt_capabilities(attempt_capability_1,
                                              attempt_capability_2)) {
            log_warning() << "P50 source arm installation failed: capability allocation"
                          << endl;
            p50_input_wait.close();
            return false;
        }

        // Install the exact owner and move to WAIT before emitting ACK.  A
        // peer that observes the ACK can therefore never race an unowned
        // attachment or a missing deadline.
        p50_source_arm_fields = fields;
        p50_source_f_lease = lease;
        p50_source_f_store_generation = lease.store_generation;
        p50_source_arm_provenance = connection_provenance;
        p50_source_deadline_msec = source_deadline_msec;
        p50_source_compile_pending = false;
        set_status(WAITP50INPUT,
                   "p50: exact source arm installed; waiting for input");

        const uint64_t remaining = source_deadline_msec - monotonic_msec();
        if (remaining == 0 || remaining > P50SourceArmedFields::MaxSourceBudgetMsec) {
            log_warning() << "P50 source arm installation failed: expired budget"
                          << " remaining_msec=" << remaining << endl;
            p50_input_wait.close();
            return false;
        }
        const P50SourceArmedMsg acknowledgement(
            fields, lease.identity.generation, lease.identity.attempt,
            p50_source_f_store_generation, lease.f_store_guid.bytes,
            lease.store_derivation_version, observation,
            static_cast<uint32_t>(remaining), attempt_capability_1,
            attempt_capability_2);
        p50_source_armed_ack = acknowledgement;
        if (!channel) {
            log_warning() << "P50 source arm installation failed: missing channel"
                          << endl;
            p50_input_wait.close();
            return false;
        }
        if (!acknowledgement.valid_payload()) {
            log_warning() << "P50 source arm installation failed: invalid acknowledgement"
                          << " store_generation="
                          << (p50_source_f_store_generation != 0)
                          << " budget_msec=" << remaining << endl;
            p50_input_wait.close();
            return false;
        }
        if (!channel->send_msg(acknowledgement)) {
            log_warning() << "P50 source arm installation failed: acknowledgement send"
                          << " protocol=" << channel->protocol << endl;
            p50_input_wait.close();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool source_budget_live() const noexcept {
        return p50_source_deadline_msec != 0 &&
               monotonic_msec() < p50_source_deadline_msec;
    }

    [[nodiscard]] bool source_wrapper_provenance_valid() const noexcept {
        if (!connection_provenance.lease.valid())
            return false;
        if (connection_provenance.listener == ListenerKind::UnixLocal)
            return connection_provenance.peer.complete();
        return connection_provenance.listener == ListenerKind::TcpRemote ||
               connection_provenance.listener == ListenerKind::TcpLoopback;
    }

    [[nodiscard]] bool source_arm_matches_compile_claim(
        const CompileJob& candidate) const {
        if (!p50_source_arm_fields.has_value() || !source_budget_live() ||
            p50_source_arm_provenance != connection_provenance ||
            !candidate.hasAssignmentIdentity() || candidate.jobID() == 0 ||
            candidate.jobID() != p50_source_arm_fields->wire_job_id ||
            candidate.assignmentEpoch() != p50_source_arm_fields->assignment_epoch ||
            candidate.assignmentNonce() != p50_source_arm_fields->assignment_nonce ||
            !candidate.usesP50Input()) {
            return false;
        }
        const CompileInputIdentity& input = candidate.compileInputIdentity();
        bool ack_matches_lease = false;
        if (p51_source_arm_fields.has_value() &&
            p51_source_armed_fields.has_value()) {
            const P51SourceArmedFields &acknowledgement =
                *p51_source_armed_fields;
            ack_matches_lease = p50_source_f_lease.has_value() &&
                acknowledgement.acknowledges(
                    P51SourceArmMsg{*p51_source_arm_fields}) &&
                acknowledgement.f_control_generation ==
                    p50_source_f_lease->identity.generation &&
                acknowledgement.f_control_attempt ==
                    p50_source_f_lease->identity.attempt &&
                acknowledgement.f_store_generation == p50_source_f_store_generation &&
                acknowledgement.f_store_guid == p50_source_f_lease->f_store_guid.bytes &&
                acknowledgement.f_store_derivation_version ==
                    p50_source_f_lease->store_derivation_version;
        } else if (p50_source_armed_ack.has_value() &&
                   p50_source_armed_ack->valid_payload()) {
            const P50SourceArmedMsg& acknowledgement = *p50_source_armed_ack;
            ack_matches_lease = p50_source_f_lease.has_value() &&
                acknowledgement.arm == *p50_source_arm_fields &&
                acknowledgement.f_control_generation ==
                    p50_source_f_lease->identity.generation &&
                acknowledgement.f_control_attempt ==
                    p50_source_f_lease->identity.attempt &&
                acknowledgement.f_store_generation == p50_source_f_store_generation &&
                acknowledgement.f_store_guid == p50_source_f_lease->f_store_guid.bytes &&
                acknowledgement.f_store_derivation_version ==
                    p50_source_f_lease->store_derivation_version;
        }
        return ack_matches_lease &&
               ((input.profile == CompileInputIdentity::P29V1Profile &&
                 p50_source_arm_fields->cache_profile == CACHE_PROFILE_P29V1) ||
                (input.profile == CompileInputIdentity::ZstdTuProfile &&
                 p50_source_arm_fields->cache_profile == CACHE_PROFILE_ZSTD_TU) ||
                (input.profile == CompileInputIdentity::ZstdRouteProfile &&
                 p50_source_arm_fields->cache_profile == CACHE_PROFILE_ZSTD_ROUTE)) &&
               input.c_store_guid == p50_source_arm_fields->c_store_guid &&
               input.attempt_id == p50_source_arm_fields->compiler_attempt &&
               input.request_id == p50_source_arm_fields->source_request_id;
    }

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
        return icecc::daemon_queue::select_earliest(*this, s);
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

    /* sig_atomic_t assignment is the only new operation here.  A forked
       compile worker uses its private copy to require that the compiler's
       terminating signal exactly match this owned worker shutdown. */
    workit_daemon_shutdown_signal = whichsig;

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

    cerr << "usage: iceccd [-n <netname>] [-m <max_processes>] [--max-preprocess <max_preprocesses>] [--no-remote] [-d|--daemonize] [-l logfile] [-s <schedulerhost[:port]>]"
        " [-v[v[v]]] [-u|--user-uid <user_uid>] [-b <env-basedir>] [--cache-limit <MB>] [-N <node_name>] [-i|--interface <net_interface>] [-p|--port <port>]"
        " [--state-jsonl <path>] [--state-interval <sec>] [--state-log]"
        " [--webgui] [--webgui-port <port>] [--webgui-addr <addr>]"
        " [--cache-service <absolute-path>] [--cache-runtime-dir <absolute-path>]" << endl;
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
// number of running fulljob (compile-lane reservation) local jobs; while
// nonzero the compile lane is closed by the fulljob's full slot
// reservation, and the bounded preprocess lane keeps running independently
unsigned int fulljob_active = 0;

/* A fulljob (e.g. a link step) reserves every compile slot -- the
   historical observable behavior -- while the bounded preprocess lane
   keeps running.  An optional whole-node "exclusive" policy existed
   briefly on this branch and was removed: it was extra policy surface
   with a known liveness gap, and nothing needed it.  */
const size_t insights_graph_minutes = 100;
const size_t insights_retention_minutes = 120;
/* Wall time the daemon started: the complete-minute rate divides by the
   number of ELAPSED complete minute slots (zero-job minutes included), not
   by the number of stored nonempty buckets -- an idle daemon must show a
   falling rate, not its last active minute forever.  */
static const time_t daemon_start_ts = time(nullptr);
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
// Drain enough ordinary accepts to cover one fully occupied large worker,
// while returning to already-established clients/children every turn.
static const size_t client_accept_batch_limit = 64;
// Accepted sockets negotiate independently in the main poll loop. Bound the
// pre-client population to at most one quarter of the process fd limit (and a
// hard ceiling), leaving descriptors for active clients, children, scheduler,
// sidecar and telemetry. Remote TCP may not consume the protected local
// quarter of this admission budget.
static const size_t pending_client_admission_hard_limit = 256;
static size_t pending_client_admission_limit() noexcept
{
    struct rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return 64;
    if (limit.rlim_cur == RLIM_INFINITY)
        return pending_client_admission_hard_limit;
    const rlim_t scaled = std::max<rlim_t>(limit.rlim_cur / 4, 1);
    return static_cast<size_t>(std::min<rlim_t>(
        scaled, pending_client_admission_hard_limit));
}

static size_t pending_remote_admission_limit(size_t total_limit) noexcept
{
    const size_t local_reserve = std::min<size_t>(
        64, std::max<size_t>(total_limit / 4, 1));
    return total_limit > local_reserve ? total_limit - local_reserve : 0;
}
static const uint64_t web_idle_deadline_msec = 30 * 1000;
static const uint64_t web_lifetime_deadline_msec = 300 * 1000;

struct PendingClientAdmission {
    std::unique_ptr<MsgChannel> channel;
    ListenerKind listener_kind = ListenerKind::TcpRemote;
    PeerCredentials peer_credentials{};
    uint64_t deadline_msec = 0;
};

struct Daemon {
    Clients clients;
    // The provenance namespace is daemon-lifetime state.  Neither scheduler
    // reconnect nor clear_children may reset it or recycle a sequence.
    uint64_t daemon_generation;
    ConnectionLeaseRegistry connection_leases;
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
    map<int, PendingClientAdmission> pending_client_admissions;
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
    size_t client_accept_cursor;

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

    // iceccd remains the sole public listener owner.  The adapter owns only
    // the supervised sidecar and its authenticated private relationship.
    icecc::p50::sidecar::CentralChildReaperRegistry cache_child_reaper;
    std::unique_ptr<icecc::p50::daemon::DaemonSidecarAdapter> cache_adapter;
    bool cache_adapter_start_attempted;
    vector<std::unique_ptr<Client::PendingP51SourceCancel>>
        pending_p51_source_cancels;
    vector<std::unique_ptr<Client::PendingP51SourceArm>>
        orphaned_p51_source_arms;
    uint64_t next_p50_arm_observation_id;
    icecc::p50::advertisement::Snapshot scheduler_cache_snapshot;
    bool scheduler_cache_snapshot_valid;
    // Last successful cache-capable remote assignment on this C daemon.
    // It is a soft scheduling hint only; every new assignment still performs
    // the complete authenticated CacheWire handshake.
    std::string cache_affinity_host;
    uint32_t cache_affinity_profile_mask;
    uint32_t cache_affinity_port;
    uint32_t cache_unavailable_profile_mask;
    uint64_t cache_route_state_generation;
    std::optional<icecc::p50::sidecar::ReadyLease> cache_route_state_lease;
    // Positive sidecar operation is opt-in until the installed service path
    // and its private runtime directory are supplied together.  There is no
    // PATH lookup and no implicit shared socket location.
    std::string cache_service_executable;
    std::string cache_runtime_directory;

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

    Daemon()
        : daemon_generation(next_daemon_generation())
        , connection_leases(daemon_generation) {
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
        cache_adapter_start_attempted = false;
        next_p50_arm_observation_id = 1;
        scheduler_cache_snapshot = {};
        scheduler_cache_snapshot_valid = false;
        cache_affinity_host.clear();
        cache_affinity_profile_mask = 0;
        cache_affinity_port = 0;
        cache_unavailable_profile_mask = 0;
        cache_route_state_generation = 1;
        cache_route_state_lease.reset();
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
        client_accept_cursor = 0;
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
        shutdown_cache_adapter();
        delete discover;
    }

    bool reannounce_environments(
        const icecc::p50::advertisement::Snapshot *cache_transition = nullptr)
        __attribute_warn_unused_result__;
    icecc::p50::advertisement::Snapshot cache_advertisement_snapshot() const noexcept;
    icecc::p50::advertisement::Snapshot scheduler_cache_advertisement_snapshot() const noexcept;
    void reconcile_cache_route_state() noexcept;
    bool cache_client_sidecar_ready() noexcept;
    bool cache_client_service_ready() noexcept;
    bool cache_sidecar_recovery_in_progress() noexcept;
    void resume_deferred_p50_cache_fd_requests() noexcept;
    bool configure_cache_adapter() noexcept;
    void poll_cache_adapter() noexcept;
    void shutdown_cache_adapter() noexcept;
    void answer_client_requests();
    bool expire_pending_client_admissions() noexcept;
    uint64_t next_pending_client_admission_deadline_msec() const noexcept;
    void service_pending_client_admissions(const vector<pollfd> &pollfds);
    void service_pending_client_admissions_now();
    void clear_pending_client_admissions() noexcept;
    bool handle_transfer_env(Client *client, EnvTransferMsg *msg) __attribute_warn_unused_result__;
    bool handle_env_install_child_done(Client *client);
    bool finish_transfer_env(Client *client, bool cancel = false);
    bool handle_get_native_env(Client *client, GetNativeEnvMsg *msg) __attribute_warn_unused_result__;
    bool finish_get_native_env(Client *client, string env_key);
    P50CacheClientCapability cache_capability_for_scheduler(
        P50CacheClientCapability capability) const noexcept;
    P50CacheClientCapability project_getcs_cache_route(
        GetCSMsg *request,
        P50CacheClientCapability capability);
    void handle_old_request();
    bool handle_compile_file(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool advance_p50_attachments(const std::vector<pollfd>& pollfds);
    bool handle_p50_source_arm(Client *client, P50SourceArmMsg *msg)
        __attribute_warn_unused_result__;
    bool handle_activity(Client *client) __attribute_warn_unused_result__;
    bool handle_file_chunk_env(Client *client, Msg *msg) __attribute_warn_unused_result__;
    void handle_end(Client *client, int exitcode);
    void settle_p50_input(Client *client,
                          icecc::p50::InputLifecycleAction action,
                          const char *reason) noexcept;
    void complete_p50_input_lifecycle(
        const icecc::p50::InputLifecycleResult& result,
        const char *reason) noexcept;
    bool expire_p50_source_waiters() __attribute_warn_unused_result__;
    bool invalidate_p50_source_waiters_for_lease() __attribute_warn_unused_result__;
    uint64_t next_p50_source_deadline_msec() const noexcept;
    int scheduler_get_internals() __attribute_warn_unused_result__;
    void clear_children();
    void begin_session_quiescence();
    void advance_session_quiescence();
    uint64_t next_session_quiescence_wakeup_msec() const noexcept;
    bool session_quiescence_pending() const noexcept;
    int scheduler_use_cs(UseCSMsg *msg) __attribute_warn_unused_result__;
    int scheduler_no_cs(NoCSMsg *msg) __attribute_warn_unused_result__;
    bool handle_get_cs(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_local_job(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_job_done(Client *cl, JobDoneMsg *m) __attribute_warn_unused_result__;
    bool handle_job_timing(Client *client, JobTimingMsg *m) __attribute_warn_unused_result__;
    bool handle_cache_session(Client *client, Msg *msg) __attribute_warn_unused_result__;
    bool handle_p50_cache_session_fd_request(
        Client *client, P50CacheSessionFdRequestMsg *msg,
        P50CacheFdReplyTicket reply_ticket = {})
        __attribute_warn_unused_result__;
    bool handle_p51_source_lease_request(
        Client *client, P51SourceLeaseRequestMsg *msg,
        P51CacheFdReplyTicket reply_ticket = {})
        __attribute_warn_unused_result__;
    bool handle_p51_source_arm(Client *client, P51SourceArmMsg *msg)
        __attribute_warn_unused_result__;
    bool advance_p51_source_arms(const std::vector<pollfd>& pollfds);
    void queue_p51_source_cancel(
        const P51SourceArmFields& arm,
        const P51SourceArmedFields& armed,
        const icecc::p50::sidecar::AbsoluteMonotonicDeadline& deadline,
        const icecc::p50::sidecar::ReadyLease& ready_lease) noexcept;
    void withdraw_p51_source_incarnation(
        const icecc::p50::sidecar::ReadyLease& ready_lease,
        const char* reason) noexcept;
    bool advance_p51_source_cancels(const std::vector<pollfd>& pollfds);
    bool advance_orphaned_p51_source_arms(
        const std::vector<pollfd>& pollfds);
    bool handle_p51_cache_link_session(
        Client *client, P51CacheLinkSessionMsg *msg)
        __attribute_warn_unused_result__;
    bool advance_p51_source_leases(const std::vector<pollfd>& pollfds);
    bool handle_compile_done(Client *client) __attribute_warn_unused_result__;
    bool handle_verify_env(Client *client, VerifyEnvMsg *msg) __attribute_warn_unused_result__;
    bool handle_blacklist_host_env(Client *client, Msg *msg) __attribute_warn_unused_result__;
    int handle_cs_conf(ConfCSMsg *msg);
    int handle_assign_prepare(AssignPrepareMsg *msg);
    int handle_revoke_before_start(RevokeBeforeStartMsg *msg);
    string dump_internals() const;
    string determine_nodename();
    void determine_system();
    void determine_supported_features();
    bool maybe_stats(bool force_check = false);
    bool send_scheduler(const Msg &msg) __attribute_warn_unused_result__;
    bool expire_scheduler_output();
    void record_waitforcs_latency(bool use_cs, uint64_t latency_msec);
    void close_scheduler(bool orderly_shutdown = false);
    void schedule_scheduler_reconnect();
    bool finish_scheduler_loss_if_needed();
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
    void complete_minute_rate(time_t now, size_t window_minutes,
                              double *rate, uint64_t *last_complete_jobs,
                              size_t *complete_slots) const;
    string dump_insights_jobs_json(time_t minute_ts, size_t limit, uint64_t before_seq);
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
    StateWriter state_writer;
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
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_perror("Failed to make TCP listen socket nonblocking");
        return false;
    }
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
    const int flags = fcntl(unix_listen_fd, F_GETFL, 0);
    if (flags < 0 ||
        fcntl(unix_listen_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_perror("Failed to make Unix listen socket nonblocking");
        return false;
    }

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
        /* allSettled, not all: with Promise.all a fast failure resolves the
           await while the other requests are still in flight, so the finally
           below would clear refreshInFlight and let a new refresh race the
           stragglers.  Settlement first, then propagate any failure.  */
        const settled = await Promise.allSettled([
          fetchJson("/api/state", 5000),
          fetchJson("/api/clients", 5000),
          fetchJson(`/api/jobs?limit=${limit}`, 8000)
        ]);
        const failed = settled.find((r) => r.status === "rejected");
        if (failed) {
          throw failed.reason;
        }
        const [state, clients, jobs] = settled.map((r) => r.value);
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
        }
        setText("queue-delay", summarizeMetric(queueDelay));
        setText("waitforcs-ms", summarizeMetric(waitforcsTimes));
        setText("exec-ms", summarizeMetric(execTimes));
        setText("queue-mode", summarizeModeP95(queueDelayRemote, queueDelayLocal));
        setText("exec-mode", summarizeModeP95(execTimesRemote, execTimesLocal));
        const coverage = allJobs.length ? Math.round(100 * timedByClient / allJobs.length) : 0;
        /* Server-computed rate over complete minutes (idle minutes in the
           denominator): the page-derived count both went stale on an idle
           node and saturated at the page size.  */
        const rates = state.rates || {};
        const completeRate = Number(rates.jobs_per_minute_complete_10m || 0);
        const lastMin = Number(rates.last_complete_minute_jobs || 0);
        setText("timing-coverage",
          `${coverage}% client | ${completeRate.toFixed(1)}/min (last complete: ${lastMin})`);

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
    <div class="card"><div class="label">Jobs/min (complete minutes)</div><div class="value" id="jobs-per-minute">-</div></div>
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

    /* Same lifecycle as the main dashboard: a deadline on every fetch and
       an in-flight guard, released only after settlement -- without the
       guard, setInterval starts a fresh refresh every two seconds while a
       stalled one is still pending.  */
    let insightsRefreshInFlight = false;
    function fetchWithDeadline(url, timeoutMs) {
      const ctrl = new AbortController();
      const timer = setTimeout(() => ctrl.abort(), timeoutMs);
      return fetch(url, { cache: "no-store", signal: ctrl.signal })
        .finally(() => clearTimeout(timer));
    }
    async function refresh() {
      if (insightsRefreshInFlight) {
        return;
      }
      insightsRefreshInFlight = true;
      try {
        const settledRes = await Promise.allSettled([
          fetchWithDeadline("/api/state", 5000),
          fetchWithDeadline("/api/insights-series?minutes=100", 8000)
        ]);
        const failedRes = settledRes.find((r) => r.status === "rejected");
        if (failedRes) {
          throw failedRes.reason;
        }
        const [stateRes, seriesRes] = settledRes.map((r) => r.value);
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
        /* Rate from COMPLETE minutes only (server-computed): the newest
           bucket covers a minute still in progress, so using it as the rate
           dips after every rollover and saturates while filling.  */
        const completeRate = Number(series.jobs_per_minute_complete || 0);
        setText("jobs-per-minute",
          `${completeRate.toFixed(1)} (current min so far: ${latest.partial ? (latest.jobs_total || 0) : "-"})`);
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
      } finally {
        insightsRefreshInFlight = false;
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
    <button id="load-more" style="display:none; margin:8px">Load older rows</button>
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
    let nextBefore = 0;      // cursor: 0 = start from the newest row
    let totalShown = 0;
    let pageInFlight = false;
    async function refresh(loadMore) {
      const minute = parseMinute();
      if (!minute) {
        document.getElementById("meta").textContent = "missing minute query parameter";
        return;
      }
      if (pageInFlight) {
        return;   // double-click on Load older must not append a page twice
      }
      pageInFlight = true;
      const moreBtnGuard = document.getElementById("load-more");
      if (moreBtnGuard) {
        moreBtnGuard.disabled = true;
      }
      try {
        const before = loadMore && nextBefore ? `&before=${nextBefore}` : "";
        const res = await fetch(`/api/insights-jobs?minute=${minute}&limit=500${before}`, { cache: "no-store" });
        if (!res.ok) {
          throw new Error(`HTTP ${res.status}`);
        }
        const payload = await res.json();
        nextBefore = Number(payload.next_before_seq || 0);
        totalShown = loadMore ? totalShown + Number(payload.returned || 0)
                              : Number(payload.returned || 0);
        const moreText = payload.truncated
          ? ` | more available (showing ${totalShown} so far)` : "";
        document.getElementById("meta").textContent =
          `minute=${toTime(payload.minute_ts)} | returned=${fmt(totalShown)}${moreText} | retention=${fmt(payload.retention_minutes)}m`;
        const moreBtn = document.getElementById("load-more");
        if (moreBtn) {
          moreBtn.style.display = payload.truncated ? "" : "none";
        }
        const body = document.getElementById("jobs-body");
        if (!loadMore) {
          body.innerHTML = "";
        }
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
      } finally {
        pageInFlight = false;
        const btn = document.getElementById("load-more");
        if (btn) {
          btn.disabled = false;
        }
      }
    }
    document.getElementById("load-more").addEventListener("click", () => refresh(true));
    refresh(false);
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
    case Client::WAITP50INPUT:
    case Client::WAITFORCS:
    case Client::FORWARDING_USE_CS:
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
        if (state_dump_log) {
            if (state_writer.alive()) {
                /* File I/O belongs to the writer process, not this loop.  */
                state_writer.enqueue(StateWriter::SINK_STATELOG, line);
            } else if (logfile_error) {
                (*logfile_error) << line << "\n";   // stderr configuration
            }
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

void Daemon::complete_minute_rate(time_t now, size_t window_minutes,
                                  double *rate, uint64_t *last_complete_jobs,
                                  size_t *complete_slots) const
{
    const time_t current_minute = now - (now % 60);
    /* First slot that is BOTH fully after daemon start and inside the
       requested window; every elapsed slot since then counts in the
       denominator, including idle ones.  */
    time_t window_start = current_minute - time_t(window_minutes) * 60;
    const time_t first_full_minute = (daemon_start_ts - (daemon_start_ts % 60)) + 60;
    if (window_start < first_full_minute) {
        window_start = first_full_minute;
    }
    size_t slots = 0;
    if (current_minute > window_start) {
        slots = size_t((current_minute - window_start) / 60);
    }
    uint64_t jobs_sum = 0;
    uint64_t last_jobs = 0;
    for (const auto &bucket : insights_history) {
        if (bucket.minute_ts >= window_start && bucket.minute_ts < current_minute) {
            jobs_sum += bucket.jobs_total;
        }
        if (bucket.minute_ts == current_minute - 60) {
            last_jobs = bucket.jobs_total;
        }
    }
    *rate = slots ? double(jobs_sum) / double(slots) : 0.0;
    *last_complete_jobs = last_jobs;
    *complete_slots = slots;
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
        /* Denominator = elapsed complete minute SLOTS in the window (idle
           minutes included; they are absent from insights_history but very
           much part of the rate) -- an idle daemon shows a falling rate,
           not its last active minute forever.  */
        double rate = 0.0;
        uint64_t last_jobs = 0;
        size_t slots = 0;
        complete_minute_rate(now, minutes, &rate, &last_jobs, &slots);
        o << "\"complete_minutes\":" << slots << ",";
        o << "\"jobs_per_minute_complete\":" << rate << ",";
        o << "\"last_complete_minute_jobs\":" << last_jobs << ",";
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

string Daemon::dump_insights_jobs_json(time_t minute_ts, size_t limit, uint64_t before_seq)
{
    /* Same cursor model as /api/jobs: bounded page (rows here carry full
       command lines, so the bound is lower), newest first, deeper history
       via ?before=<seq>, and honest truncation metadata.  */
    static const size_t kMaxLimit = 1000;
    if (limit == 0) {
        limit = 200;
    }
    if (limit > kMaxLimit) {
        limit = kMaxLimit;
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
    bool truncated = false;
    uint64_t last_seq = 0;
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
            if (before_seq && entry.seq >= before_seq) {
                continue;
            }
            if (returned >= limit) {
                truncated = true;   // at least one more matching row exists
                break;
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
            last_seq = entry.seq;
        }
    }

    o << "],";
    o << "\"returned\":" << returned << ",";
    o << "\"truncated\":" << (truncated ? "true" : "false") << ",";
    o << "\"next_before_seq\":" << (unsigned long long)(truncated ? last_seq : 0);
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
                        uint64_t limit = 200;
                        parse_query_u64(path, "limit", &limit);
                        uint64_t before_seq = 0;
                        parse_query_u64(path, "before", &before_seq);
                        if (!queue_web_response(fd, 200, "OK", "application/json; charset=utf-8",
                                           dump_insights_jobs_json((time_t)minute_ts, (size_t)limit, before_seq))) {
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

    // A scheduler that stops reading must not hold the shared daemon loop
    // inside a blocking send (and starve public protocol admission). Success
    // means committed to this session's FIFO, not acknowledged by S. All
    // subsequent lifecycle/advertisement frames use this same queue; loss
    // discards the queue with the existing generation cleanup.
    // Bound retained backlog at 4 MiB. Serialization can transiently add
    // one protocol-bounded frame before the post-send overflow check closes
    // the channel; this is not an unbounded producer queue.
    constexpr size_t pending_limit = 4 * 1024 * 1024;
    if ((scheduler->deferred_output_armed() &&
         monotonic_msec() >= scheduler->deferred_output_deadline_msec()) ||
        scheduler->pending_bytes() >= pending_limit) {
        log_warning() << "scheduler output backlog exhausted" << endl;
        close_scheduler();
        return false;
    }
    if (!scheduler->send_msg(msg, MsgChannel::SendNonBlocking |
                                  MsgChannel::SendDeferrable) ||
        scheduler->pending_bytes() > pending_limit) {
        log_error() << "sending message to scheduler failed.." << endl;
        close_scheduler();
        return false;
    }

    return true;
}

bool Daemon::expire_scheduler_output()
{
    if (!scheduler || !scheduler->deferred_output_armed() ||
        monotonic_msec() < scheduler->deferred_output_deadline_msec())
        return false;
    log_warning() << "scheduler deferred output deadline expired" << endl;
    close_scheduler();
    (void)finish_scheduler_loss_if_needed();
    return true;
}

static icecc::p50::advertisement::Snapshot canonical_cache_snapshot(
    const icecc::p50::advertisement::Snapshot& snapshot) noexcept
{
    return snapshot.present() ? snapshot : icecc::p50::advertisement::Snapshot{};
}

static void apply_cache_advertisement(
    LoginMsg& login, const icecc::p50::advertisement::Snapshot& requested)
{
    const auto snapshot = canonical_cache_snapshot(requested);
    login.setCacheAdvertisement(snapshot.endpoint_port, snapshot.protocol,
                                snapshot.profile_mask);
}

static bool exact_public_tcp_listener(int fd, uint32_t expected_port) noexcept
{
    if (fd < 0 || expected_port == 0 || expected_port > UINT16_MAX)
        return false;

    sockaddr_storage address{};
    socklen_t address_size = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) != 0)
        return false;

    uint32_t bound_port = 0;
    if (address.ss_family == AF_INET && address_size >= sizeof(sockaddr_in)) {
        bound_port = ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
    } else if (address.ss_family == AF_INET6 && address_size >= sizeof(sockaddr_in6)) {
        bound_port = ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    } else {
        return false;
    }

    int accepting = 0;
    socklen_t accepting_size = sizeof(accepting);
    return bound_port == expected_port
        && ::getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &accepting_size) == 0
        && accepting_size == sizeof(accepting) && accepting != 0;
}

/* G4 session activation (bigoracle 15:07): a non-null `scheduler` channel is
   only a LOGIN_ATTEMPT.  The session is ACTIVE -- owned, generation committed --
   only after the first ConfCS is consumed (handle_cs_conf).  A channel loss
   before ConfCS is NOT an established-session loss. */
static uint64_t scheduler_session_generation = 0;
static bool scheduler_session_active = false;
static bool scheduler_login_pending = false;
/* G4 (bigoracle 16:45): absolute monotonic deadline for the current
   LOGIN_ATTEMPT to receive ConfCS; 0 when not in a pending attempt. */
static const uint64_t kLoginActivationDeadlineMsec = 30 * 1000;
static uint64_t scheduler_login_deadline_msec = 0;
/* G4 (bigoracle 16:40): a pending-loss token recorded by a non-orderly
   close_scheduler() when an ACTIVE session's channel closes -- captured at
   CLOSE time (not snapshotted at turn start), so a session that activated and
   was then lost within a single turn is still cleaned up exactly once.
   finish_scheduler_loss_if_needed() consumes it; the counter is test-visible. */
static bool scheduler_loss_pending = false;
static uint64_t scheduler_loss_pending_generation = 0;
static unsigned int scheduler_loss_cleanup_attempts = 0;
/* G4 (17:20#3): set once the 64-bit session-generation space is exhausted
   (2^64 activations -- unreachable in practice).  Makes exhaustion TERMINAL:
   reconnect() then refuses further sessions rather than reuse a generation. */
static bool scheduler_generation_exhausted = false;

icecc::p50::advertisement::Snapshot
Daemon::cache_advertisement_snapshot() const noexcept
{
    return cache_adapter != nullptr
        ? canonical_cache_snapshot(cache_adapter->advertisement_snapshot())
        : icecc::p50::advertisement::Snapshot{};
}

icecc::p50::advertisement::Snapshot
Daemon::scheduler_cache_advertisement_snapshot() const noexcept
{
    auto snapshot = cache_advertisement_snapshot();
    if (scheduler != nullptr &&
        snapshot.protocol == CACHE_WIRE_REVISION_R2 &&
        !protocol_supports_cache_r2(scheduler->protocol))
        return {};
    return snapshot;
}

void Daemon::reconcile_cache_route_state() noexcept
{
    const icecc::p50::sidecar::ReadyLease *current = nullptr;
    /* Route state belongs to the authenticated C-sidecar incarnation.  An S
       disconnect suppresses publication, but must not discard a still-live
       sidecar's warm affinity or permanent local capability facts. */
    if (cache_adapter != nullptr && cache_adapter->authenticated()) {
        const auto& observed = cache_adapter->outer_current_ready_lease();
        if (observed.has_value() && observed->valid())
            current = &*observed;
    }

    const bool same_owner = current != nullptr &&
        cache_route_state_lease.has_value() &&
        icecc::p50::daemon::p50_ready_lease_observation_equal(
            *cache_route_state_lease, *current);
    if (same_owner)
        return;

    cache_affinity_host.clear();
    cache_affinity_profile_mask = 0;
    cache_affinity_port = 0;
    cache_unavailable_profile_mask = 0;
    cache_route_state_lease.reset();
    if (current != nullptr)
        cache_route_state_lease = *current;
    if (cache_route_state_generation !=
        std::numeric_limits<uint64_t>::max())
        ++cache_route_state_generation;
}

bool Daemon::cache_client_sidecar_ready() noexcept
{
    /* C capability belongs to the authenticated local sidecar lease, not to
       the F advertisement.  A submitter-only (--no-remote) daemon has a
       deliberately absent public snapshot while its local CacheWire control
       service is fully usable. */
    reconcile_cache_route_state();
    if (cache_adapter == nullptr || !cache_adapter->authenticated() ||
        !cache_route_state_lease.has_value()) {
        return false;
    }
    const auto& ready_lease = cache_adapter->outer_current_ready_lease();
    return ready_lease.has_value() && ready_lease->valid() &&
        icecc::p50::daemon::p50_ready_lease_observation_equal(
            *cache_route_state_lease, *ready_lease);
}

bool Daemon::cache_client_service_ready() noexcept
{
    /* Scheduler ownership gates publication and assignment use, while the
       authenticated sidecar lease itself may survive a scheduler bounce. */
    return scheduler_session_active && scheduler != nullptr &&
           cache_client_sidecar_ready();
}

bool Daemon::cache_sidecar_recovery_in_progress() noexcept
{
    if (cache_adapter == nullptr || !cache_adapter_start_attempted)
        return false;
    if (cache_adapter->state() == icecc::p50::daemon::AdapterState::Ready ||
        cache_adapter->state() == icecc::p50::daemon::AdapterState::Stopped ||
        cache_adapter->state() == icecc::p50::daemon::AdapterState::Failed ||
        cache_adapter->state() == icecc::p50::daemon::AdapterState::ShuttingDown)
        return false;
    if (cache_adapter->outer_launch_plan_active())
        return true;
    using LifecycleState = icecc::p50::sidecar::LifecycleState;
    switch (cache_adapter->outer_lifecycle_state()) {
    case LifecycleState::LaunchPrepared:
    case LifecycleState::ForkedAwaitExecAndReady:
    case LifecycleState::Ready:
    case LifecycleState::TerminatingGrace:
    case LifecycleState::TerminatingKill:
    case LifecycleState::ReapAndGroupCheck:
    case LifecycleState::RetryEligible:
        return true;
    case LifecycleState::Stopped:
    case LifecycleState::DegradedLegacy:
    case LifecycleState::FailedClosed:
        return false;
    }
    return false;
}

bool Daemon::reannounce_environments(
    const icecc::p50::advertisement::Snapshot *cache_transition)
{
    // A positive endpoint belongs only to an ACTIVE scheduler session.  The
    // initial Login for every connection is always canonical absence; the
    // exact current snapshot is reconciled after the first ConfCS activates
    // the session.
    icecc::p50::advertisement::Snapshot snapshot{};
    if (scheduler_session_active) {
        snapshot = cache_transition != nullptr
            ? canonical_cache_snapshot(*cache_transition)
            : scheduler_cache_advertisement_snapshot();
        if (scheduler != nullptr &&
            snapshot.protocol == CACHE_WIRE_REVISION_R2 &&
            !protocol_supports_cache_r2(scheduler->protocol))
            snapshot = {};
    }

    log_info() << "reannounce_environments cache=" << snapshot.endpoint_port
               << "/" << snapshot.protocol << "/" << snapshot.profile_mask << endl;
    LoginMsg lmsg(0, nodename, "", supported_features);
    apply_cache_advertisement(lmsg, snapshot);
    lmsg.envs = available_environments(envbasedir);
    if (!send_scheduler(lmsg))
        return false;
    scheduler_cache_snapshot = snapshot;
    scheduler_cache_snapshot_valid = true;
    return true;
}

/* S2 (BigOracle d23d9c5d HOLD, Gap 3 -- reused-client clearing): three
   separate sites (scheduler_no_cs's NoCS branch, handle_old_request's
   schedulerless-fallback branch, and handle_get_cs's no-scheduler branch)
   each clear c->cacheHandoff so a handoff retained from an EARLIER
   dispatch on this same (reused) Client can never leak into a LATER one
   that takes a path other than scheduler_use_cs.  No real wire flow can
   ever hand one of these sites a Client that both (a) genuinely retained
   a prior valid handoff and (b) is now making a second live GetCS
   decision: Client::getcs_outstanding (see its own comment) is set at
   accept and cleared only at client destruction, so a second GetCS on one
   Client is rejected in EVERY non-terminal state, not merely WAITFORCS --
   there is structurally no natural second decision cycle to observe this
   against.  This test-only hook -- armed only by an explicit env var,
   never on the production path, matching ICECC_TEST_USECS_CUT_AT's idiom
   (see scheduler_use_cs) -- poisons the targeted site's Client with a
   fabricated retained handoff immediately before its real (unmodified)
   clear statement runs, then records the post-clear result for
   dump_internals() to expose: an external test process has no other way
   to observe one Client's private field.  The env var may name more than
   one site, comma-separated, so a single daemon process (one fork, one
   set of env vars) can cover multiple sites across a test's lifetime
   PROVIDED the sites' real firings do not overlap in time -- each firing
   overwrites the single shared result below, so a caller must read (and
   assert on) one site's result before triggering the next armed site. */
static bool cache_handoff_clear_test_fired = false;
static std::string cache_handoff_clear_test_site;
static bool cache_handoff_clear_test_result_valid = true;
static uint32_t cache_handoff_clear_test_result_port = ~UINT32_C(0);
static uint32_t cache_handoff_clear_test_result_protocol = ~UINT32_C(0);
static uint32_t cache_handoff_clear_test_result_mask = ~UINT32_C(0);

static bool test_poison_site_armed(const char *armed, const char *site)
{
    if (!armed) {
        return false;
    }
    const size_t site_len = strlen(site);
    for (const char *p = armed; *p; ) {
        const char *comma = strchr(p, ',');
        const size_t token_len = comma ? (size_t)(comma - p) : strlen(p);
        if (token_len == site_len && strncmp(p, site, site_len) == 0) {
            return true;
        }
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return false;
}

static bool test_poison_cache_handoff_if_armed(Client *c, const char *site)
{
    const char *armed = getenv("ICECC_TEST_POISON_CACHE_HANDOFF_SITE");
    if (!test_poison_site_armed(armed, site)) {
        return false;
    }
    c->cacheHandoff = Client::CacheHandoff{
        true, UINT32_C(0xdeadbeef),
        UINT64_C(0x1111111111111111), UINT64_C(0x2222222222222222),
        "poison-host", UINT32_C(3333),
        UINT32_C(0x0000cafe), UINT32_C(9), UINT32_C(7),
        UINT64_C(0), UINT64_C(0), UINT64_C(0), std::nullopt};
    /* BigOracle blueprint (Gap 3 "Focused test"): also preload a STALE
       usecsmsg with a nonzero cache tail, standing in for whatever a
       reused Client might already carry.
       install_cache_absent_local_decision must delete this before
       installing its own canonical-absent replacement, never merely
       overwrite the pointer (BigOracle: the leak this function exists to
       fix structurally).  c->usecsmsg is null on every real path that
       reaches these three sites, so this is a fabricated precondition,
       same as the cacheHandoff poisoning above. */
    delete c->usecsmsg;
    c->usecsmsg = new UseCSMsg("x86_64", "stale-poison-host", UINT32_C(9999),
                               UINT32_C(0xfeedface), true, UINT32_C(4242),
                               UINT32_C(17),
                               UINT64_C(0x3333333333333333),
                               UINT64_C(0x4444444444444444),
                               UINT32_C(0x0000d00d), UINT32_C(9), UINT32_C(7));
    return true;
}

static void test_record_cache_handoff_clear(const Client *c, const char *site)
{
    cache_handoff_clear_test_fired = true;
    cache_handoff_clear_test_site = site;
    cache_handoff_clear_test_result_valid = c->cacheHandoff.valid;
    cache_handoff_clear_test_result_port = c->cacheHandoff.cachePort;
    cache_handoff_clear_test_result_protocol = c->cacheHandoff.cacheProtocol;
    cache_handoff_clear_test_result_mask = c->cacheHandoff.cacheProfileMask;
}

/* S2 (BigOracle blueprint, team-lead follow-up): the ONE place that
   transfers ownership of a Client's pending UseCS reply.  Every one of
   the FIVE sites that replace c->usecsmsg -- the three "no real worker
   snapshot" fallbacks below (via install_cache_absent_local_decision) and
   both scheduler_use_cs relay projections (the self-selected-F local
   rewrite and the remote-worker branch's introspection copy) -- did a
   bare pointer overwrite with no delete of the prior object, which leaks
   whenever a reused Client already held one.  This does not reason about
   whether the prior pointer is null (delete on a null pointer is always
   safe); it is unconditional so no call site has to get that reasoning
   right on its own. */
static void install_pending_usecs(Client *c, UseCSMsg *reply)
{
    delete c->usecsmsg;
    c->usecsmsg = reply;
}

/* S2 (BigOracle exact blueprint): the ONE choke point for installing a
   canonical cache-absent LOCAL decision on a (possibly reused) Client.
   scheduler_no_cs, handle_old_request's stranded-GetCS replay, and
   handle_get_cs's scheduler-absent fallback all share exactly this
   situation -- "no real worker snapshot at all" -- and each previously
   inlined its own cacheHandoff-clear + usecsmsg-install + set_status
   sequence independently, with no guard against installing a replacement
   usecsmsg over one that was already live: BigOracle flags this as a real
   leak on any path that reaches one of these fallbacks with
   c->usecsmsg already non-null.  Atomic:
     1. cacheHandoff reset to canonical absence.
     2. any PRIOR usecsmsg deleted before the replacement is installed
        (the leak fix, via install_pending_usecs above -- ownership
        transfers via unique_ptr so a caller cannot accidentally keep its
        own copy alive either).
     3. the replacement itself asserted canonical cache-absent (0/0/0) and
        wire-valid -- defense in depth: every caller already constructs it
        that way, but this function's whole contract is that IT is what
        guarantees cache-absence, not each caller's own care.
     4. status set to PENDING_USE_CS under the given reason. */
static void install_cache_absent_local_decision(Client &c,
                                                  std::unique_ptr<UseCSMsg> reply,
                                                  const char *why)
{
    assert(reply);
    assert(reply->cache_endpoint_port == 0 && reply->cache_protocol == 0
           && reply->cache_profile_mask == 0);
    assert(reply->valid_payload());
    c.cacheHandoff = Client::CacheHandoff{};
    install_pending_usecs(&c, reply.release());
    c.set_status(Client::PENDING_USE_CS, why);
}

/* Protocol-49 fulfillment state has one owner: the daemon event-loop thread.
   Records live for the scheduler epoch, not merely one TCP connection.  The
   full triple keys terminal outcomes, while the live wire-id index supports
   the explicitly weaker nonce-less compatibility modes in expected O(1). */
struct AssignmentKey {
    uint64_t epoch;
    uint32_t wire_id;
    uint64_t nonce;

    bool operator==(const AssignmentKey &other) const
    {
        return epoch == other.epoch && wire_id == other.wire_id
            && nonce == other.nonce;
    }
};

struct AssignmentKeyHash {
    size_t operator()(const AssignmentKey &key) const
    {
        size_t h = std::hash<uint64_t>()(key.epoch);
        h ^= std::hash<uint64_t>()(key.nonce) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>()(key.wire_id) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

struct WorkerAssignment {
    enum Phase { Reserved, Claimed, ClaimedOrLater, Orphaned } phase;
    AssignmentKey key;
    uint32_t claimant;
    bool ready_announced = false;
};

struct AssignmentTerminal {
    RevokeResultMsg::Result result;
};

static uint64_t assignment_scheduler_epoch = 0;
static ConfCSMsg::FenceMode assignment_fence_mode = ConfCSMsg::Legacy;
static std::unordered_map<uint32_t, WorkerAssignment> live_assignments;
static std::unordered_map<AssignmentKey, AssignmentTerminal,
                          AssignmentKeyHash> assignment_terminals;
static std::unordered_map<uint32_t, AssignmentKey> closed_wire_ids;
static std::unordered_set<uint64_t> retired_assignment_epochs;
static size_t assignment_terminal_limit()
{
    static const size_t limit = [] {
        const size_t production_limit = 65536;
        if (getenv("ICECC_TESTS") == nullptr) {
            return production_limit;
        }
        const char *text = getenv("ICECC_TEST_ASSIGNMENT_TABLE_LIMIT");
        if (text == nullptr || *text == '\0') {
            return production_limit;
        }
        char *end = nullptr;
        const unsigned long parsed = strtoul(text, &end, 10);
        if (end == text || *end != '\0' || parsed == 0
                || parsed > production_limit) {
            return production_limit;
        }
        return static_cast<size_t>(parsed);
    }();
    return limit;
}
static bool assignment_table_exhausted = false;
static bool assignment_epoch_history_exhausted = false;
static unsigned long assignment_prepares = 0;
static unsigned long assignment_ready_replies = 0;
static unsigned long assignment_revoke_results = 0;
static unsigned long assignment_claims = 0;
static unsigned long assignment_claim_rejects = 0;
static unsigned long assignment_stale_controls = 0;

static bool assignment_mode_prepares(ConfCSMsg::FenceMode mode)
{
    return mode == ConfCSMsg::Advisory
        || mode == ConfCSMsg::EnforcingCompat
        || mode == ConfCSMsg::StrictNonce;
}

static bool assignment_mode_enforces_prepare(ConfCSMsg::FenceMode mode)
{
    return mode == ConfCSMsg::EnforcingCompat
        || mode == ConfCSMsg::StrictNonce;
}

static bool replace_assignment_epoch()
{
    if (assignment_scheduler_epoch != 0
            && retired_assignment_epochs.find(assignment_scheduler_epoch)
                == retired_assignment_epochs.end()) {
        if (retired_assignment_epochs.size() >= assignment_terminal_limit()) {
            assignment_epoch_history_exhausted = true;
            return false;
        }
        retired_assignment_epochs.insert(assignment_scheduler_epoch);
    }
    live_assignments.clear();
    assignment_terminals.clear();
    closed_wire_ids.clear();
    assignment_scheduler_epoch = 0;
    assignment_fence_mode = ConfCSMsg::Legacy;
    assignment_table_exhausted = false;
    return true;
}

static bool assignment_capacity_available(const AssignmentKey &key)
{
    if (assignment_terminals.find(key) != assignment_terminals.end()) {
        return true;
    }
    if (assignment_table_exhausted
            || assignment_terminals.size() >= assignment_terminal_limit()) {
        assignment_table_exhausted = true;
        return false;
    }
    return true;
}

static bool retain_assignment_terminal(const AssignmentKey &key,
                                       RevokeResultMsg::Result result,
                                       bool blocks_compat_claim)
{
    auto existing = assignment_terminals.find(key);
    if (existing != assignment_terminals.end()) {
        if (existing->second.result != result) {
            log_error() << "conflicting terminal result for assignment "
                        << key.wire_id << endl;
            assignment_table_exhausted = true;
            return false;
        }
    } else {
        if (!assignment_capacity_available(key)) {
            log_error() << "protocol-49 epoch terminal table exhausted" << endl;
            return false;
        }
        assignment_terminals.emplace(key, AssignmentTerminal { result });
    }
    if (blocks_compat_claim) {
        closed_wire_ids.insert_or_assign(key.wire_id, key);
    }
    return true;
}

static void mark_assignment_claimed_or_later(uint32_t wire_id)
{
    auto it = live_assignments.find(wire_id);
    if (it != live_assignments.end() && it->second.phase == WorkerAssignment::Claimed) {
        it->second.phase = WorkerAssignment::ClaimedOrLater;
    }
}

static void finish_assignment_claim(uint32_t wire_id)
{
    auto it = live_assignments.find(wire_id);
    if (it == live_assignments.end()
            || (it->second.phase != WorkerAssignment::Claimed
                && it->second.phase != WorkerAssignment::ClaimedOrLater)) {
        return;
    }
    it->second.phase = WorkerAssignment::ClaimedOrLater;
    /* In ADVISORY the client may win before PREPARE is consumed.  Keep that
       nonce-zero placeholder live until PREPARE supplies its full identity;
       deleting it here would let late PREPARE regress the claim to Reserved. */
    if (it->second.key.nonce == 0) {
        return;
    }
    const AssignmentKey key = it->second.key;
    if (retain_assignment_terminal(
            key, RevokeResultMsg::ClaimedOrLater, true)) {
        live_assignments.erase(it);
    } else {
        it->second.phase = WorkerAssignment::Orphaned;
    }
}

static void close_assignment_transport_session()
{
    /* Link loss is an S-side terminal boundary, but the old client can still
       arrive at this daemon.  Close every known assignment and retain its
       rejection identity for the whole epoch.  A nonce-zero ADVISORY
       placeholder cannot be safely attributed after transport loss, so it
       remains an explicit Orphaned fail-closed wire-id record. */
    auto it = live_assignments.begin();
    while (it != live_assignments.end()) {
        WorkerAssignment &record = it->second;
        if (record.key.nonce == 0) {
            record.phase = WorkerAssignment::Orphaned;
            ++it;
            continue;
        }
        const RevokeResultMsg::Result result =
            record.phase == WorkerAssignment::Reserved
                ? RevokeResultMsg::Revoked
                : RevokeResultMsg::ClaimedOrLater;
        if (!retain_assignment_terminal(record.key, result, true)) {
            record.phase = WorkerAssignment::Orphaned;
            ++it;
            continue;
        }
        it = live_assignments.erase(it);
    }
}

/* P50_SOURCE_ARM is an assignment claim, not an advisory endpoint hint.  It
   may consume only the exact Reserved record for the active scheduler epoch;
   a later CompileFile from this same Client is idempotent and must not reopen
   Reserved or increment the claim counter. */
static bool authorize_source_arm_claim(const P50SourceArmFields& arm,
                                       uint32_t claimant)
{
    if (!scheduler_session_active || assignment_table_exhausted ||
        assignment_fence_mode == ConfCSMsg::Legacy || arm.wire_job_id == 0 ||
        arm.assignment_epoch == 0 || arm.assignment_nonce == 0 ||
        arm.assignment_epoch != assignment_scheduler_epoch) {
        return false;
    }

    auto live = live_assignments.find(arm.wire_job_id);
    if (live == live_assignments.end()) {
        return false;
    }
    WorkerAssignment& record = live->second;
    if (record.key.epoch != arm.assignment_epoch ||
        record.key.nonce != arm.assignment_nonce ||
        record.phase != WorkerAssignment::Reserved) {
        return false;
    }
    record.phase = WorkerAssignment::Claimed;
    record.claimant = claimant;
    ++assignment_claims;
    return true;
}

/* A rejected source frame still needs a scheduler-authoritative disposition
   when it names one exact live assignment.  Once the complete triple matches,
   consume Reserved as the worker's observed claim (or accept the same
   claimant's already-claimed record), bind the token to Client, and let the
   normal FROM_SERVER JobDone path settle it exactly once.  A random/stale
   triple is never allowed to settle another wire id. */
static bool bind_source_assignment_for_settlement(
    const P50SourceArmFields& arm, uint32_t claimant)
{
    if (!scheduler_session_active || assignment_table_exhausted ||
        assignment_fence_mode == ConfCSMsg::Legacy || arm.wire_job_id == 0 ||
        arm.assignment_epoch == 0 || arm.assignment_nonce == 0 ||
        arm.assignment_epoch != assignment_scheduler_epoch) {
        return false;
    }
    auto live = live_assignments.find(arm.wire_job_id);
    if (live == live_assignments.end()) {
        return false;
    }
    WorkerAssignment& record = live->second;
    if (record.key.epoch != arm.assignment_epoch ||
        record.key.nonce != arm.assignment_nonce) {
        return false;
    }
    if (record.phase == WorkerAssignment::Reserved) {
        record.phase = WorkerAssignment::Claimed;
        record.claimant = claimant;
        ++assignment_claims;
        return true;
    }
    return record.phase == WorkerAssignment::Claimed &&
           record.claimant == claimant;
}

static bool authorize_assignment_claim(const CompileJob &job, uint32_t claimant,
                                       int claimant_protocol)
{
    const uint32_t wire_id = job.jobID();
    /* A remote claim never has authority without the scheduler-assigned wire
       identity.  CompileFile's wholly-absent P50 identity is frame-valid for
       the local CLIENTWORK path, but that path bypasses this function.  Reject
       wire zero here, before even ADVISORY can create a live placeholder. */
    if (wire_id == 0) {
        ++assignment_claim_rejects;
        return false;
    }
    if (assignment_fence_mode == ConfCSMsg::Legacy) {
        return true;   // exact protocol-48/default-disabled behavior
    }
    if (!scheduler_session_active || assignment_table_exhausted) {
        ++assignment_claim_rejects;
        return false;
    }
    auto live = live_assignments.find(wire_id);
    if (live != live_assignments.end()) {
        WorkerAssignment &record = live->second;
        const bool full_identity = job.hasAssignmentIdentity();
        const bool identity_matches = full_identity
            && record.key.epoch == job.assignmentEpoch()
            && record.key.nonce == job.assignmentNonce();
        if (record.key.epoch == assignment_scheduler_epoch
                && record.phase == WorkerAssignment::Claimed
                && record.claimant == claimant && identity_matches) {
            /* P50_SOURCE_ARM already claimed this exact assignment for this
               Client.  CompileFile proves the same claimant but does not
               consume a second dispatch credit or reopen the record. */
            return true;
        }
        const bool legacy_compat_claim = !full_identity
            && assignment_fence_mode != ConfCSMsg::StrictNonce;
        if (record.key.epoch != assignment_scheduler_epoch
                || record.phase != WorkerAssignment::Reserved
                || (!identity_matches && !legacy_compat_claim)) {
            ++assignment_claim_rejects;
            return false;
        }
        record.phase = WorkerAssignment::Claimed;
        record.claimant = claimant;
        ++assignment_claims;
        return true;
    }

    auto closed = closed_wire_ids.find(wire_id);
    if (closed != closed_wire_ids.end()
            && closed->second.epoch == assignment_scheduler_epoch) {
        ++assignment_claim_rejects;
        return false;
    }
    if (assignment_mode_enforces_prepare(assignment_fence_mode)) {
        /* ENFORCING_COMPAT deliberately keeps a real pre-v50 C interoperable
           with a current F.  Such a peer cannot receive or carry the v50
           assignment identity, so S correctly sends no PREPARE and its
           CompileFile is wholly nonce-less.  Admit only that directly
           negotiated legacy wire shape.  A v50 peer with a missing identity,
           an identity-bearing unknown claim, and every STRICT_NONCE claim
           remain fail-closed.  This is the historical legacy admission path,
           not an ADVISORY placeholder: no late PREPARE can exist for it. */
        const bool negotiated_legacy_claim =
            assignment_fence_mode == ConfCSMsg::EnforcingCompat &&
            claimant_protocol > 0 &&
            claimant_protocol < PROTOCOL_VERSION_ASSIGNMENT_IDENTITY &&
            !job.hasAssignmentIdentity();
        if (negotiated_legacy_claim) {
            trace() << "admitting negotiated legacy assignment claim "
                    << wire_id << " from protocol " << claimant_protocol
                    << endl;
            return true;
        }
        ++assignment_claim_rejects;
        return false;
    }

    if (job.hasAssignmentIdentity()
            && job.assignmentEpoch() != assignment_scheduler_epoch) {
        ++assignment_claim_rejects;
        return false;
    }

    /* ADVISORY admits an unknown nonce-less claim, but records ownership.
       A later PREPARE binds the nonce into this same record without changing
       Claimed/ClaimedOrLater back to Reserved. */
    if (live_assignments.size() + assignment_terminals.size()
            >= assignment_terminal_limit()) {
        assignment_table_exhausted = true;
        ++assignment_claim_rejects;
        return false;
    }
    WorkerAssignment placeholder;
    placeholder.phase = WorkerAssignment::Claimed;
    placeholder.key = AssignmentKey {
        assignment_scheduler_epoch, wire_id, job.assignmentNonce()
    };
    placeholder.claimant = claimant;
    live_assignments.emplace(wire_id, placeholder);
    ++assignment_claims;
    return true;
}

/* G4 (bigoracle 20:13:59/20:32:36): the authority for whether a GetCS-derived
   (submitter-side) assignment may emit scheduler lifecycle frames -- JobBegin
   (handle_compile_file), the forwarded JobDone (handle_job_done), and teardown
   settlement (handle_end).  Ownership is the request's OWN facts: published to S
   under the CURRENT active generation.  Never inferred from the scheduler
   pointer, a synthetic client-id job_id, or the connection status.  An
   unpublished schedulerless fallback compiles locally but emits no scheduler
   frame for its lifetime; a superseded-generation request is not announced on a
   reconnected session.  (Worker-side handle_compile_done is NOT governed by
   this -- it uses worker-assignment ownership.) */
static bool scheduler_owns_getcs_assignment(const Client *c)
{
    return scheduler_session_active
        && c->getcs_outstanding
        && c->getcs_published
        && c->getcs_generation == scheduler_session_generation;
}

void Daemon::schedule_scheduler_reconnect()
{
    next_scheduler_connect = time(nullptr) + 20 + (rand() & 31);
    static bool fast_reconnect = getenv("ICECC_TESTS") != nullptr;
    if (fast_reconnect)
        next_scheduler_connect = time(nullptr) + 3;
}

void Daemon::close_scheduler(bool orderly_shutdown)
{
    scheduler_cache_snapshot_valid = false;
    if (!scheduler) {
        return;
    }

    /* Record the pending-loss token from the ACTIVE state before it is cleared,
       unless this is an orderly shutdown or the session never activated (a
       failed LOGIN_ATTEMPT closes with no token). */
    if (!orderly_shutdown && scheduler_session_active) {
        scheduler_loss_pending = true;
        scheduler_loss_pending_generation = scheduler_session_generation;
    }
    delete scheduler;
    scheduler = nullptr;
    scheduler_session_active = false;
    scheduler_login_pending = false;
    scheduler_login_deadline_msec = 0;
    reconcile_cache_route_state();
    delete discover;
    discover = nullptr;
    schedule_scheduler_reconnect();
}

bool Daemon::configure_cache_adapter() noexcept
{
    if (cache_service_executable.empty() && cache_runtime_directory.empty())
        return true;

    const bool local_only_cache_adapter = noremote;
    if (daemon_port < 0 || daemon_port > UINT16_MAX ||
            (!local_only_cache_adapter &&
             !exact_public_tcp_listener(tcp_listen_fd,
                                        static_cast<uint32_t>(daemon_port)))) {
        log_error() << "cache sidecar requires the exact public TCP listener to be bound"
                    << " (noremote=" << noremote << " port=" << daemon_port
                    << " tcp_listen_fd=" << tcp_listen_fd << ")" << endl;
        return false;
    }

    try {
        icecc::p50::daemon::Config config;
        config.executable = cache_service_executable;
        config.runtime_directory = cache_runtime_directory;
        // The daemon-local namespace is already immutable and nonzero.  It is
        // never serialized as a steady_clock value and remains stable across
        // supervised sidecar retries while the launch allocator advances the
        // attempt component.
        config.generation = daemon_generation;
        config.expected_daemon_uid = static_cast<uint64_t>(::geteuid());
        config.expected_daemon_gid = static_cast<uint64_t>(::getegid());
        config.expected_service_uid = config.expected_daemon_uid;
        config.expected_service_gid = config.expected_daemon_gid;
        // A submitter-only daemon has no public worker listener.  Its cache
        // sidecar is still required for the authenticated C control handoff,
        // but it must remain local-only and therefore cannot advertise a
        // public cache endpoint to the scheduler.
        config.public_listener_port = local_only_cache_adapter
            ? 0 : static_cast<uint32_t>(daemon_port);
        config.readiness_timeout = std::chrono::milliseconds(5000);
        config.connect_timeout = std::chrono::milliseconds(1000);
        config.handoff_timeout = std::chrono::milliseconds(250);
        config.input_attachment_timeout = std::chrono::milliseconds(5000);
        config.input_lifecycle_timeout = std::chrono::milliseconds(5000);
        config.shutdown_timeout = std::chrono::milliseconds(1000);
        config.restart_window = std::chrono::milliseconds(10000);
        config.max_restarts = 3;
        // One attempt per event-loop turn keeps every recovery bounded while
        // the rolling outer budget still permits transient recovery.
        config.max_attempts_per_recovery = 1;
        config.central_reaper = &cache_child_reaper;
        config.clock_identity =
            icecc::p50::sidecar::process_monotonic_clock_identity();

        if (!icecc::p50::daemon::DaemonSidecarAdapter::valid_config(config)) {
            log_error() << "invalid cache sidecar configuration; refusing positive mode"
                        << endl;
            return false;
        }
        auto *created = new (std::nothrow)
            icecc::p50::daemon::DaemonSidecarAdapter(std::move(config));
        if (created == nullptr) {
            log_error() << "cannot allocate cache sidecar adapter" << endl;
            return false;
        }
        cache_adapter.reset(created);
        cache_adapter->observe_public_listener(
            !local_only_cache_adapter,
            local_only_cache_adapter ? 0 : static_cast<uint32_t>(daemon_port));
        cache_adapter_start_attempted = false;
        log_info() << "cache sidecar configured for public port " << daemon_port
                   << "; advertisement remains absent until scheduler activation and READY"
                   << endl;
        return true;
    } catch (...) {
        log_error() << "cannot construct cache sidecar configuration" << endl;
        cache_adapter.reset();
        return false;
    }
}

uint64_t Daemon::next_p50_source_deadline_msec() const noexcept
{
    uint64_t earliest = 0;
    for (const auto& entry : clients) {
        const Client *client = entry.second;
        if (client != nullptr && client->pending_p51_source_arm &&
            client->pending_p51_source_arm->deadline_msec != 0 &&
            (earliest == 0 ||
             client->pending_p51_source_arm->deadline_msec < earliest))
            earliest = client->pending_p51_source_arm->deadline_msec;
        if (client == nullptr || client->status != Client::WAITP50INPUT ||
            !client->p50_source_arm_fields.has_value() ||
            client->p50_source_deadline_msec == 0) {
            continue;
        }
        if (earliest == 0 || client->p50_source_deadline_msec < earliest) {
            earliest = client->p50_source_deadline_msec;
        }
    }
    return earliest;
}

bool Daemon::expire_p50_source_waiters()
{
    const uint64_t now = monotonic_msec();
    vector<Client*> expired;
    expired.reserve(clients.size());
    for (const auto& entry : clients) {
        Client *client = entry.second;
        if (client != nullptr && client->status == Client::WAITP50INPUT &&
            client->p50_source_arm_fields.has_value() &&
            client->p50_source_deadline_msec != 0 &&
            now >= client->p50_source_deadline_msec) {
            expired.push_back(client);
        }
    }

    for (Client *client : expired) {
        // The map membership check makes this safe when an earlier expiry
        // settlement caused scheduler-loss cleanup to erase the client.
        if (client == nullptr || client->channel == nullptr ||
            clients.find(client->channel) == clients.end()) {
            continue;
        }
        log_warning() << "P50 source arm deadline expired for client "
                      << client->client_id << endl;
        handle_end(client, 149);
        if (finish_scheduler_loss_if_needed()) {
            return true;
        }
    }
    return false;
}

bool Daemon::invalidate_p50_source_waiters_for_lease()
{
    /* An armed input transfer is owned by its exact authenticated sidecar
       lease, not by the scheduler session that selected the pair.  Existing
       work may finish across an S bounce; only a real sidecar lease
       withdrawal/replacement invalidates it. */
    const bool current_ready = cache_adapter != nullptr &&
        cache_adapter->authenticated() &&
        cache_adapter->outer_current_ready_lease().has_value() &&
        cache_adapter->outer_current_ready_lease()->valid();
    const icecc::p50::sidecar::ReadyLease *current_lease = nullptr;
    if (current_ready) {
        current_lease = &*cache_adapter->outer_current_ready_lease();
    }

    vector<Client*> stale;
    stale.reserve(clients.size());
    for (const auto& entry : clients) {
        Client *client = entry.second;
        if (client == nullptr || client->status != Client::WAITP50INPUT ||
            !client->p50_source_arm_fields.has_value()) {
            continue;
        }
        const bool owner_current = current_ready && current_lease != nullptr &&
            client->p50_source_f_lease.has_value() &&
            !icecc::p50::daemon::p50_wait_owner_replaced(
                *client->p50_source_f_lease, *current_lease);
        if (!owner_current) {
            stale.push_back(client);
        }
    }

    for (Client *client : stale) {
        if (client == nullptr || client->channel == nullptr ||
            clients.find(client->channel) == clients.end()) {
            continue;
        }
        log_warning() << "withdrawing P50 source owner after F lease withdrawal/replacement for client "
                      << client->client_id << endl;
        handle_end(client, 151);
        if (finish_scheduler_loss_if_needed()) {
            return true;
        }
    }
    return false;
}

void Daemon::poll_cache_adapter() noexcept
{
    if (cache_adapter == nullptr)
        return;

    reconcile_cache_route_state();

    const bool listener_bound = daemon_port > 0 && daemon_port <= UINT16_MAX
        && exact_public_tcp_listener(tcp_listen_fd,
                                     static_cast<uint32_t>(daemon_port));
    cache_adapter->observe_public_listener(
        listener_bound, listener_bound ? static_cast<uint32_t>(daemon_port) : 0);

    // Never start a service or publish presence during a mere scheduler
    // LOGIN_ATTEMPT.  Once a sidecar is authenticated, however, its lease is
    // C-local route ownership and survives an S disconnect.  Scheduler loss
    // withdraws publication and assignment waiters; it is not itself a
    // sidecar/runtime failure and therefore is not a replacement request.
    const bool scheduler_cache_owner =
        scheduler_session_active && cache_adapter != nullptr;
    if (!scheduler_cache_owner || scheduler == nullptr) {
        cache_adapter->outer_set_scheduler_owner(false);
        if (invalidate_p50_source_waiters_for_lease()) {
            return;
        }
        // A surviving sidecar remains C-local state while S is absent, but
        // its already-admitted lifecycle/input work still needs one bounded
        // reducer turn.  Without opening that turn here,
        // outer_immediate_turn_required() pins answer_client_requests() to
        // poll(..., 0) while reconnect() floods one delay line per spin.
        // Starting a new incarnation remains impossible because scheduler
        // ownership is false; this only drains finite work for A.
        if (cache_adapter_start_attempted) {
            (void)cache_adapter->outer_begin_turn(
                std::chrono::steady_clock::now(), nullptr);
        }
        return;
    }

    icecc::p50::advertisement::Update update;
    if (!cache_adapter_start_attempted) {
        cache_adapter_start_attempted = true;
        trace() << "cache sidecar outer lifecycle engaged (scheduler session active)" << endl;
    }
    cache_adapter->outer_set_scheduler_owner(true);
    // This call only resets the adapter's per-daemon-turn quota and publishes
    // the current level once an incarnation is already in flight.  It invokes
    // the reducer's begin() only for the initial Stopped incarnation; READY,
    // reaping, authentication, TERM/KILL, cleanup, and successor launch are
    // advanced exactly once after the single poll inventory below.
    (void)cache_adapter->outer_begin_turn(std::chrono::steady_clock::now(),
                                          &update);
    {
        static icecc::p50::daemon::AdapterState last_traced_state =
            icecc::p50::daemon::AdapterState::Stopped;
        static icecc::p50::sidecar::LifecycleState last_traced_lifecycle{};
        const auto adapter_state = cache_adapter->state();
        const auto lifecycle_state = cache_adapter->outer_lifecycle_state();
        if (adapter_state != last_traced_state ||
            !(lifecycle_state == last_traced_lifecycle)) {
            trace() << "cache sidecar adapter state=" << int(adapter_state)
                    << " lifecycle=" << int(static_cast<uint8_t>(lifecycle_state))
                    << endl;
            last_traced_state = adapter_state;
            last_traced_lifecycle = lifecycle_state;
        }
    }

    // A sidecar withdrawal/replacement is an ownership boundary.  Invalidate
    // old WAIT owners before publishing any replacement advertisement or
    // admitting a new source arm in the next event-loop phase.
    if (invalidate_p50_source_waiters_for_lease()) {
        return;
    }

    // Preserve the Controller's exact order.  In particular, a crash and
    // recovery observed in one poll is published absent before present.
    for (size_t index = 0; index < update.count; ++index) {
        if (!scheduler_session_active || scheduler == nullptr
                || !reannounce_environments(&update.transitions[index]))
            return;
    }

    // Reconnects intentionally begin with 0/0/0.  A Controller that was
    // already present may emit no new transition, so reconcile its level after
    // activation rather than relying only on edges.
    const auto current = cache_advertisement_snapshot();
    const auto scheduler_visible = scheduler_cache_advertisement_snapshot();
    // Keep the snapshot comparison tied to an active adapter.  This is an
    // actual production predicate (and not a source-gate marker): a stale
    // scheduler snapshot is never treated as authoritative while the outer
    // sidecar owner is absent.
    const bool snapshot_valid_for_adapter =
        scheduler_cache_snapshot_valid && cache_adapter != nullptr;
    if ((!snapshot_valid_for_adapter || scheduler_cache_snapshot != scheduler_visible)
            && !reannounce_environments(&scheduler_visible))
        return;

    /* Complete strict PREPAREs held during sidecar startup only after the
       same READY snapshot has been published to S.  This keeps the scheduler
       UseCS projection and the daemon's live endpoint state in one order. */
    if (cache_adapter->state() == icecc::p50::daemon::AdapterState::Ready &&
        current.present()) {
        for (auto& entry : live_assignments) {
            WorkerAssignment& assignment = entry.second;
            if (assignment.phase != WorkerAssignment::Reserved ||
                assignment.ready_announced)
                continue;
            if (!send_scheduler(AssignReadyMsg(
                    assignment.key.epoch, assignment.key.wire_id,
                    assignment.key.nonce)))
                return;
            assignment.ready_announced = true;
            ++assignment_ready_replies;
        }
    }
    reconcile_cache_route_state();
    resume_deferred_p50_cache_fd_requests();
}

void Daemon::resume_deferred_p50_cache_fd_requests() noexcept
{
    const bool service_ready = cache_client_service_ready();
    if (!service_ready && cache_sidecar_recovery_in_progress())
        return;

    /* handle_p50_cache_session_fd_request() may close and erase a client on
       revalidation failure, so snapshot channel keys and re-lookup each Client
       before re-entering it.  A failure for one request may erase another
       Client, making a raw-pointer snapshot unsafe.  Each deferred request
       belongs to a different live AF_UNIX connection. */
    std::vector<MsgChannel *> deferred;
    for (const auto& entry : clients) {
        if (entry.second != nullptr &&
            entry.second->deferred_p50_cache_fd_request.has_value()) {
            deferred.push_back(entry.first);
        }
    }

    for (MsgChannel *channel : deferred) {
        const auto found = clients.find(channel);
        if (found == clients.end() || found->second == nullptr)
            continue;
        Client *const client = found->second;
        if (client->channel != channel ||
            !client->deferred_p50_cache_fd_request.has_value()) {
            continue;
        }
        Client::DeferredP50CacheFdRequest deferred_request =
            std::move(*client->deferred_p50_cache_fd_request);
        client->deferred_p50_cache_fd_request.reset();
        trace() << "resuming deferred P50 C-cache control request for assignment "
                << deferred_request.request.wire_job_id << endl;
        P50CacheSessionFdRequestMsg message(deferred_request.request);
        const bool resumed =
            handle_p50_cache_session_fd_request(
                client, &message, std::move(deferred_request.replyTicket));
        if (!resumed && service_ready) {
            log_warning()
                << "deferred P50 C-cache control request failed revalidation for assignment "
                << deferred_request.request.wire_job_id << endl;
        }
    }
}

void Daemon::shutdown_cache_adapter() noexcept
{
    if (cache_adapter == nullptr)
        return;

    if (invalidate_p50_source_waiters_for_lease()) {
        log_warning() << "scheduler loss while invalidating P50 source owners" << endl;
    }

    icecc::p50::advertisement::Update update;
    // Shutdown is a reducer request only.  The daemon must not call the old
    // synchronous Supervisor::shutdown path or destroy an active lifecycle
    // before its central reaper/outer poll can prove teardown.
    cache_adapter->outer_set_scheduler_owner(false);
    cache_adapter->outer_request_shutdown(&update);
    for (size_t index = 0; index < update.count; ++index) {
        if (!scheduler_session_active || scheduler == nullptr
                || !reannounce_environments(&update.transitions[index]))
            break;
    }
    // Keep the adapter/reaper relationship alive until the outer lifecycle
    // has delivered exact TERM/KILL/reap/path proofs.  The normal loop owns
    // the next turns; destruction itself never performs emergency waiting.
}

/* G4: the single, exactly-once cleanup for the loss of an ESTABLISHED scheduler
   session.  It consumes the pending-loss token recorded by close_scheduler() at
   the moment an active session's channel closed, so it is correct even when the
   session activated and was then lost within one turn.  Every
   answer_client_requests() boundary that may have closed the channel calls this
   immediately after; a true result means "the session is gone, leave the turn
   now" so no further same-poll work proceeds against a dead session.  A failed
   LOGIN_ATTEMPT and an orderly shutdown record no token, so neither is treated
   as an established-session loss. */
bool Daemon::finish_scheduler_loss_if_needed()
{
    if (!scheduler_loss_pending) {
        return false;                    /* no pending established-session loss */
    }
    scheduler_loss_pending = false;                        /* consume the token, once */
    ++scheduler_loss_cleanup_attempts;                     /* test-visible */
    clear_children();
    return true;
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

/* Exact-identity handoff observability (issue #4 P0-A): the lifecycle
   gate reads these from dump_internals().  */
static unsigned long usecs_delivery_attempts = 0;
static unsigned long usecs_frames_committed = 0;
static unsigned long usecs_exact_aborts = 0;
static unsigned long duplicate_settlements_rejected = 0;
/* Exact-quiescence observability (issue #4 P0-C).  */
static unsigned long fsession_compilers_quiesced = 0;
static unsigned long fsession_kill_escalations = 0;
static unsigned long fsession_unowned_residue = 0;

/* Persistent exact child ownership (issue #4 correction D).  Every child
   the daemon forks is registered by pid with its process group, kind,
   the scheduler-session generation it belongs to, and its owning client;
   ordinary zombie reaping dispatches BY REGISTERED PID, and the
   session-loss barrier operates on exact records rather than a scalar.
   Failure to establish or verify ownership FAILS CLOSED: the daemon does
   not reconnect or advertise capacity while any old-generation compiler
   record remains.  */
struct ChildRecord {
    pid_t pid;
    pid_t pgid;
    enum Kind { COMPILER, ENV_INSTALL, STATE_WRITER, OTHER } kind;
    uint64_t session_generation;
    unsigned int owning_client_id;
    enum State { RUNNING, TERM_SENT, KILL_SENT, REAPED } state;
    /* Monotonic logical fact, independent of TERM/KILL/reap progress. */
    bool completion_observed;
    /* Compiler group signals are authorized only while the exact, unreaped
       leader remains our child.  The authority is monotonic: after the final
       group signal or any ECHILD/lost-anchor observation it is never restored.
       A retired record may only consume its leader status and poll absence. */
    icecc::daemon_child::SignalAuthority signal;
    /* Logical capacity and OS cleanup are distinct.  Completion releases a
       slot once while the waitable leader can remain as signal authority. */
    icecc::daemon_child::SlotAccounting slot;
    bool session_quiescence_target = false;
};
static std::map<pid_t, ChildRecord> child_registry;
static icecc::daemon_child::PosixSignalOperations child_signal_operations;
static icecc::daemon_child::ChildOwnershipGate child_ownership_gate;
struct SessionQuiescenceTarget { pid_t pid; pid_t pgid; uint64_t generation; };
struct SessionQuiescence {
    enum Phase { IDLE, TERM_GRACE, REAP, SETTLED } phase = IDLE;
    std::vector<SessionQuiescenceTarget> targets;
    uint64_t term_deadline_msec = 0;
    uint64_t reap_deadline_msec = 0;
    uint64_t next_observation_msec = 0;
    bool reap_deadline_reported = false;
    bool identity_failed = false;
};
static SessionQuiescence session_quiescence;

static void register_child(pid_t pid, pid_t pgid, ChildRecord::Kind kind,
                           unsigned int owning_client_id)
{
    ChildRecord rec;
    rec.pid = pid;
    rec.pgid = pgid;
    rec.kind = kind;
    rec.session_generation = scheduler_session_generation;
    rec.owning_client_id = owning_client_id;
    rec.state = ChildRecord::RUNNING;
    rec.completion_observed = false;
    rec.signal = icecc::daemon_child::SignalAuthority{};
    rec.slot = icecc::daemon_child::SlotAccounting{
        kind == ChildRecord::COMPILER
    };
    child_registry[pid] = rec;
}

static void unregister_child(pid_t pid)
{
    child_registry.erase(pid);
}

/* A result/EOF can become readable just before the worker exits.  Keep the
   exact PID registered until its wait status is consumed; erasing it first
   leaves an unreapable zombie because anonymous waitpid is intentionally not
   used beside the sidecar's exact reaper. */
static bool complete_child_registration(pid_t pid)
{
    auto record = child_registry.find(pid);
    if (record == child_registry.end()
            || record->second.completion_observed)
        return false;
    record->second.completion_observed = true;
    if (record->second.kind != ChildRecord::COMPILER)
        return true;
    return icecc::daemon_child::release_slot_once(record->second.slot);
}

/* Snapshot identities only: the canonical SignalAuthority and slot accounting
   remain in child_registry. The ordinary child sweep skips marked records. */
bool Daemon::session_quiescence_pending() const noexcept
{
    return session_quiescence.phase == SessionQuiescence::TERM_GRACE
        || session_quiescence.phase == SessionQuiescence::REAP;
}

uint64_t Daemon::next_session_quiescence_wakeup_msec() const noexcept
{
    if (!session_quiescence_pending())
        return 0;
    uint64_t wakeup = session_quiescence.next_observation_msec;
    // After an expired reap deadline, continue exact settlement observations
    // at the bounded cadence instead of polling continuously at the old time.
    if (session_quiescence.reap_deadline_reported)
        return wakeup;
    const uint64_t phase_deadline =
        session_quiescence.phase == SessionQuiescence::TERM_GRACE
            ? session_quiescence.term_deadline_msec
            : session_quiescence.reap_deadline_msec;
    return std::min(wakeup, phase_deadline);
}

void Daemon::begin_session_quiescence()
{
    if (session_quiescence_pending())
        return;
    session_quiescence = SessionQuiescence{};
    session_quiescence.identity_failed = child_ownership_gate.sticky_failure();
    child_ownership_gate.begin_barrier();
    session_quiescence.phase = SessionQuiescence::TERM_GRACE;
    const uint64_t now = monotonic_msec();
    session_quiescence.term_deadline_msec = now + 5000;
    session_quiescence.next_observation_msec = now + 50;

    clear_pending_client_admissions();
    while (!clients.empty()) {
        Client *cl = clients.first();
        handle_end(cl, 116);
    }
    close_assignment_transport_session();

    for (std::map<pid_t, ChildRecord>::iterator it = child_registry.begin();
            it != child_registry.end(); ++it) {
        ChildRecord &rec = it->second;
        if (rec.kind != ChildRecord::COMPILER
                || rec.session_generation > scheduler_session_generation)
            continue;
        rec.session_quiescence_target = true;
        session_quiescence.targets.push_back(
            SessionQuiescenceTarget{rec.pid, rec.pgid,
                                    rec.session_generation});
        if (rec.signal.active && !rec.signal.term_sent
                && !rec.signal.final_sent) {
            const icecc::daemon_child::SignalResult result =
                icecc::daemon_child::send_term_if_owned(
                    rec.signal, rec.pid, rec.pgid, child_signal_operations);
            if (result.invoked) {
                rec.state = ChildRecord::TERM_SENT;
                log_info() << "session quiescence TERM compiler pid="
                           << rec.pid << " pgid=" << rec.pgid
                           << " generation=" << rec.session_generation
                           << " errno=" << result.error << endl;
            }
        }
    }
    if (session_quiescence.targets.empty()) {
        session_quiescence.phase = SessionQuiescence::REAP;
        session_quiescence.reap_deadline_msec = now;
    }
}

void Daemon::advance_session_quiescence()
{
    if (!session_quiescence_pending())
        return;
    const uint64_t now = monotonic_msec();
    bool any_running_anchor = false;
    bool any_unsettled = false;
    bool missing_identity = false;
    size_t target_index = 0;
    while (target_index < session_quiescence.targets.size()) {
        const SessionQuiescenceTarget target =
            session_quiescence.targets[target_index];
        std::map<pid_t, ChildRecord>::iterator found =
            child_registry.find(target.pid);
        if (found == child_registry.end()) {
            missing_identity = true;
            ++target_index;
            continue;
        }
        ChildRecord &rec = found->second;
        if (!rec.session_quiescence_target || rec.pgid != target.pgid
                || rec.session_generation != target.generation) {
            missing_identity = true;
            ++target_index;
            continue;
        }
        if (rec.signal.active) {
            const icecc::daemon_child::AnchorObservation observation =
                icecc::daemon_child::observe_owned_anchor(
                    rec.signal, rec.pid, child_signal_operations);
            any_running_anchor |= observation
                == icecc::daemon_child::AnchorObservation::Running;
        }
        if (!rec.signal.active
                && icecc::daemon_child::settle_retired(
                    rec.signal, rec.pid, rec.pgid,
                    child_signal_operations)) {
            rec.state = ChildRecord::REAPED;
            log_info() << "session quiescence settled compiler pid="
                       << rec.pid << " pgid=" << rec.pgid
                       << " generation=" << rec.session_generation << endl;
            ++fsession_compilers_quiesced;
            const bool released =
                icecc::daemon_child::release_slot_once(rec.slot);
            if (released && current_kids > 0)
                --current_kids;
            else if (released) {
                child_ownership_gate.mark_sticky_failure();
                session_quiescence.identity_failed = true;
                log_error() << "session quiescence slot accounting underflow"
                            << endl;
            }
            child_registry.erase(found);
            session_quiescence.targets.erase(
                session_quiescence.targets.begin() + target_index);
        } else {
            any_unsettled = true;
            ++target_index;
        }
    }

    if (missing_identity) {
        const bool newly_failed = !session_quiescence.identity_failed;
        child_ownership_gate.mark_sticky_failure();
        session_quiescence.identity_failed = true;
        if (newly_failed)
            log_error() << "session quiescence snapshot identity missing or"
                        << " changed; refusing reconnect" << endl;
    }
    if (session_quiescence.phase == SessionQuiescence::TERM_GRACE
            && (!any_running_anchor || now >= session_quiescence.term_deadline_msec)) {
        for (std::vector<SessionQuiescenceTarget>::const_iterator target =
                 session_quiescence.targets.begin();
             target != session_quiescence.targets.end(); ++target) {
            std::map<pid_t, ChildRecord>::iterator found =
                child_registry.find(target->pid);
            if (found == child_registry.end())
                continue;
            ChildRecord &rec = found->second;
            if (rec.session_quiescence_target && rec.signal.active
                    && rec.pgid == target->pgid
                    && rec.session_generation == target->generation) {
                const icecc::daemon_child::SignalResult result =
                    icecc::daemon_child::send_final_if_owned(
                        rec.signal, rec.pid, rec.pgid,
                        child_signal_operations);
                if (result.invoked) {
                    ++fsession_kill_escalations;
                    rec.state = ChildRecord::KILL_SENT;
                    log_info() << "session quiescence KILL compiler pid="
                               << rec.pid << " pgid=" << rec.pgid
                               << " generation=" << rec.session_generation
                               << " errno=" << result.error << endl;
                }
            }
        }
        session_quiescence.phase = SessionQuiescence::REAP;
        session_quiescence.reap_deadline_msec = now + 5000;
    }

    if (session_quiescence.phase == SessionQuiescence::REAP
            && now >= session_quiescence.reap_deadline_msec
            && (any_unsettled || missing_identity)) {
        if (!session_quiescence.reap_deadline_reported)
            log_error() << "session quiescence reap deadline expired; retaining"
                        << " ownership and blocking reconnect/capacity" << endl;
        session_quiescence.reap_deadline_reported = true;
        child_ownership_gate.mark_recoverable_block();
    }
    if (!any_unsettled && !missing_identity
            && session_quiescence.phase == SessionQuiescence::REAP) {
        session_quiescence.phase = SessionQuiescence::SETTLED;
        if (current_kids != 0) {
            log_error() << "clear_children: " << current_kids
                        << " counted children had no owning compiler record;"
                        << " failing closed" << endl;
            fsession_unowned_residue += current_kids;
            child_ownership_gate.mark_sticky_failure();
            session_quiescence.identity_failed = true;
        } else {
            assert(fd2client.empty());
            assert(pending_client_admissions.empty());
            assert(connection_leases.size() == 0);
            fd2client.clear();
            new_client_id = 0;
        }
        child_ownership_gate.settle_exact(current_kids);
        log_info() << "session quiescence state phase=SETTLED targets="
                   << session_quiescence.targets.size()
                   << " current_kids=" << current_kids
                   << " ownership_failed=" << child_ownership_gate.admission_blocked()
                   << " ownership_faulted=" << child_ownership_gate.sticky_failure()
                   << " identity_failed=" << (session_quiescence.identity_failed ? 1 : 0)
                   << " scheduler_generation=" << scheduler_session_generation
                   << endl;
    }
    session_quiescence.next_observation_msec = now + 50;
}

string Daemon::dump_internals() const
{
    string result;
    const uint64_t now_msec = monotonic_msec();

    result += "Node Name: " + nodename + "\n";
    result += "  Remote name: " + remote_name + "\n";
    {
        char handoff[160];
        snprintf(handoff, sizeof(handoff),
                 "  UseCS handoff: attempts=%lu committed=%lu exact_aborts=%lu dup_settlements_rejected=%lu\n",
                 usecs_delivery_attempts, usecs_frames_committed,
                 usecs_exact_aborts, duplicate_settlements_rejected);
        result += handoff;
        snprintf(handoff, sizeof(handoff),
                 "  Session quiescence: compilers_quiesced=%lu kill_escalations=%lu unowned_residue=%lu ownership_failed=%d gen=%llu\n",
                 fsession_compilers_quiesced, fsession_kill_escalations,
                 fsession_unowned_residue,
                 child_ownership_gate.admission_blocked() ? 1 : 0,
                 (unsigned long long)scheduler_session_generation);
        result += handoff;
        snprintf(handoff, sizeof(handoff),
                 "  Scheduler loss: pending=%d pending_gen=%llu cleanup_attempts=%u exhausted=%d\n",
                 scheduler_loss_pending ? 1 : 0,
                 (unsigned long long)scheduler_loss_pending_generation,
                 scheduler_loss_cleanup_attempts,
                 scheduler_generation_exhausted ? 1 : 0);
        result += handoff;
        if (cache_handoff_clear_test_fired) {
            /* S2 Gap 3 test-only (see its own comment above): only ever
               present when ICECC_TEST_POISON_CACHE_HANDOFF_SITE armed a
               site and that site's real clear ran. */
            char handoff_clear[192];
            snprintf(handoff_clear, sizeof(handoff_clear),
                     "  Cache-handoff clear test: site=%s fired=1 valid=%d "
                     "port=%u protocol=%u mask=%u\n",
                     cache_handoff_clear_test_site.c_str(),
                     cache_handoff_clear_test_result_valid ? 1 : 0,
                     cache_handoff_clear_test_result_port,
                     cache_handoff_clear_test_result_protocol,
                     cache_handoff_clear_test_result_mask);
            result += handoff_clear;
        }
        char assignment[320];
        snprintf(assignment, sizeof(assignment),
                 "  Assignment fence: mode=%u epoch=%llu live=%zu retained=%zu closed=%zu retired=%zu exhausted=%d epoch_history_exhausted=%d prepares=%lu ready=%lu revokes=%lu claims=%lu rejected=%lu stale_control=%lu\n",
                 static_cast<unsigned int>(assignment_fence_mode),
                 (unsigned long long)assignment_scheduler_epoch,
                 live_assignments.size(), assignment_terminals.size(),
                 closed_wire_ids.size(), retired_assignment_epochs.size(),
                 assignment_table_exhausted ? 1 : 0,
                 assignment_epoch_history_exhausted ? 1 : 0,
                 assignment_prepares, assignment_ready_replies,
                 assignment_revoke_results, assignment_claims,
                 assignment_claim_rejects, assignment_stale_controls);
        result += assignment;
        for (std::map<pid_t, ChildRecord>::const_iterator cit = child_registry.begin();
                cit != child_registry.end(); ++cit) {
            snprintf(handoff, sizeof(handoff),
                     "  Child: pid=%d pgid=%d kind=%d gen=%llu client=%u state=%d completion=%d slot=%d authority=%d term=%d final=%d consumed=%d absent=%d\n",
                     (int)cit->second.pid, (int)cit->second.pgid,
                     (int)cit->second.kind,
                     (unsigned long long)cit->second.session_generation,
                     cit->second.owning_client_id, (int)cit->second.state,
                     cit->second.completion_observed ? 1 : 0,
                     cit->second.slot.active ? 1 : 0,
                     cit->second.signal.active ? 1 : 0,
                     cit->second.signal.term_sent ? 1 : 0,
                     cit->second.signal.final_sent ? 1 : 0,
                     cit->second.signal.leader_consumed ? 1 : 0,
                     cit->second.signal.group_absent ? 1 : 0);
            result += handoff;
        }
    }

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
    /* Framing, bounding and the nonblocking pipe write are all that happens
       here; every file operation lives in the writer PROCESS.  */
    state_writer.enqueue(StateWriter::SINK_JSONL, line);
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
        case Client::FORWARDING_USE_CS:
        case Client::PENDING_USE_CS:
        case Client::WAITCOMPILE:
        case Client::CLIENTWORK:
        case Client::TOCOMPILE:
        case Client::WAITP50INPUT:
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

    {
        double rate = 0.0;
        uint64_t last_jobs = 0;
        size_t slots = 0;
        complete_minute_rate(now_s, 10, &rate, &last_jobs, &slots);
        o << "\"rates\":{"
          << "\"jobs_per_minute_complete_10m\":" << rate << ","
          << "\"last_complete_minute_jobs\":" << last_jobs << ","
          << "\"complete_minute_slots\":" << slots
          << "},";
    }
    o << "\"telemetry\":{"
      << "\"insights_clock_resets\":" << insights_clock_resets << ","
      << "\"insights_dropped_jobs\":" << insights_dropped_jobs << ","
      << "\"jsonl_dropped_records\":" << state_writer.dropped() << ","
      << "\"jsonl_oversized_records\":" << state_writer.oversized() << ","
      << "\"jsonl_queued_bytes\":" << state_writer.queued_bytes() << ","
      << "\"writer_alive\":" << (state_writer.running() ? "true" : "false")
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
        // Machine-parsable regardless of verbosity.  File I/O belongs to the
        // writer process; the direct stream write remains only for the
        // stderr (no -l) configuration, where the target is a tty/pipe.
        if (state_writer.alive()) {
            state_writer.enqueue(StateWriter::SINK_STATELOG, line);
        } else if (logfile_error) {
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
    /* G4 (bigoracle 20:13:59): a reply during a LOGIN_ATTEMPT is authorized
       against nothing -- drop the attempt BEFORE any client lookup or
       terminalization, so no JobDone application frame is emitted on the pending
       channel.  Not active -> close_scheduler() records no loss token, so there
       is no established-session cleanup. */
    if (!scheduler_session_active) {
        log_warning() << "scheduler_use_cs before session active for client "
                      << msg->client_id << "; dropping attempt" << endl;
        return 1;    /* caller closes the pending channel */
    }
    Client *c = clients.find_by_client_id(msg->client_id);
    trace() << "scheduler_use_cs " << msg->job_id << " " << msg->client_id
            << " " << c << " " << msg->hostname << " " << remote_name <<  endl;

    if (!c) {
        if (send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size(),
                                      msg->assignmentEpoch(), msg->assignmentNonce(),
                                      msg->cGuid(), msg->tuSeq()))) {
            return 0;
        }

        return 1;
    }

    if (c->getcs_expected > 1) {
        /* G4 BATCH_LEDGER (local-oracle 21:19/21:42): a count>1 request receives
           up to `expected` distinct decisions.  Authorize by (published, current
           generation, not-yet-complete) -- NOT by WAITFORCS, which only the
           first reply satisfies.  Dedup exact job ids (ignore, never terminalize
           the original); terminalize a stale/excess reply by its exact job id.
           Each accepted decision is relayed to the client and recorded for exact
           teardown settlement; the request completes at delivered == expected. */
        /* Authorize: published under the current active generation. */
        if (!(c->getcs_published
                && c->getcs_generation == scheduler_session_generation)) {
            log_warning() << "scheduler_use_cs batch stale/unpublished job " << msg->job_id
                          << " client " << msg->client_id << "; terminalizing" << endl;
            return send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size(),
                                             msg->assignmentEpoch(), msg->assignmentNonce(),
                                             msg->cGuid(), msg->tuSeq())) ? 0 : 1;
        }
        /* Dedup BEFORE the excess check (bigoracle 00:35 #3): a duplicate exact
           job id -- even one arriving after completion -- is ignored, never
           terminalized as excess and never counted twice. */
        for (uint32_t seen : c->getcs_batch_jobids) {
            if (seen == msg->job_id) {
                log_warning() << "scheduler_use_cs batch duplicate job " << msg->job_id
                              << "; ignoring" << endl;
                return 0;
            }
        }
        /* A genuinely new exact job id beyond the requested count is excess. */
        if (c->getcs_delivered >= c->getcs_expected) {
            log_warning() << "scheduler_use_cs batch excess job " << msg->job_id
                          << " client " << msg->client_id << "; terminalizing" << endl;
            return send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size(),
                                             msg->assignmentEpoch(), msg->assignmentNonce(),
                                             msg->cGuid(), msg->tuSeq())) ? 0 : 1;
        }
        c->getcs_batch_jobids.push_back(msg->job_id);   /* record BEFORE the write */
        /* S2 (BigOracle, 4th independent gap): a count>1 request bypassed
           the entire S2 block above/below this branch (both `return`
           early), so an accepted batch decision could relay a scheduler-
           sent cache-present tail with NO daemon-retained assignment-bound
           state at all, and never cleared a stale scalar cacheHandoff this
           (reused) Client might still carry.  The batch ledger
           (getcs_batch_jobids) records only exact job ids -- no epoch/
           nonce/endpoint per decision -- so a correct per-assignment cache
           binding is not even representable here.  BigOracle: hoisting the
           scalar cacheHandoff assignment above this branch would be WRONG
           (last-writer-wins across N decisions is nonsensical, not merely
           incomplete) -- doing so idiomatically means re-checking the
           SAME admissibility helper here too, and
           unittests/p50cacheadvertisement-source.sh already anchors that
           exact call's count, scoped to the scalar path below, so that
           specific hoist shape reddens an EXISTING source anchor, not
           just the behavioral daemonbatch-run.sh rows.  BOUNDED FIX
           instead: batch is
           cache-INELIGIBLE.  This is a FEATURE fallback, not a wire
           fallback -- batch compiles keep working via legacy (no-cache)
           transport; a future reviewed per-assignment ledger keyed by
           {job_id, epoch, nonce} may re-enable batch cache tails.  Every
           accepted batch decision therefore (a) clears any singular
           scalar cacheHandoff this Client might retain -- the SAME
           canonical clear used at the other three S2 "no real worker
           snapshot" sites (scheduler_no_cs, handle_old_request's stranded
           replay, handle_get_cs's scheduler-absent fallback), so it can
           never leak into a batch decision -- and (b) relays a COPY of
           *msg with its cache triple canonically absent, never the
           original frame, even when the scheduler sent a valid-present
           tail.  See test_poison_cache_handoff_if_armed's own comment
           for why the poison/record pair below brackets this real,
           unmodified clear. */
        const bool cache_handoff_test_poisoned_batch =
            test_poison_cache_handoff_if_armed(c, "batch");
        c->cacheHandoff = Client::CacheHandoff{};
        if (cache_handoff_test_poisoned_batch) {
            test_record_cache_handoff_clear(c, "batch");
        }
        UseCSMsg batch_reply = *msg;
        batch_reply.cache_endpoint_port = 0;
        batch_reply.cache_protocol = 0;
        batch_reply.cache_profile_mask = 0;
        /* Relay the ORIGINAL scheduler frame (bigoracle 00:35 #1) -- except
           for the cache triple canonicalized above -- preserving got_env,
           client_id, matched_job_id, and every other field exactly, as the
           scalar remote path does via send_msg(*msg).  Rebuilding with
           hardcoded got_env=true/client_id=1 could tell a real client to
           skip a required environment transfer and fail the build. */
        if (!c->channel->send_msg(batch_reply)) {
            ++usecs_exact_aborts;
            handle_end(c, 143);
            return 0;
        }
        ++c->getcs_delivered;
        c->job_id = msg->job_id;
        c->last_known_job_id = msg->job_id;
        if (c->status == Client::WAITFORCS) {
            c->set_status(Client::WAITCOMPILE, "scheduler_use_cs: batch decisions delivering");
        }
        return 0;
    }

    /* G4 (bigoracle 18:45 P0): SCALAR_ONE (count<=1).  The session is ACTIVE
       (checked above); authorize a reply only for a request PUBLISHED under the
       CURRENT active generation and still awaiting a decision (WAITFORCS).  A
       stale reply carrying a superseded generation, or one for an unpublished/
       wrong-phase client, is terminalized by its EXACT job id and never
       delivered. */
    if (!(c->getcs_published
            && c->getcs_generation == scheduler_session_generation
            && c->status == Client::WAITFORCS)) {
        log_warning() << "scheduler_use_cs unmatched job " << msg->job_id
                      << " client " << msg->client_id << "; terminalizing" << endl;
        return send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size(),
                                         msg->assignmentEpoch(), msg->assignmentNonce(),
                                         msg->cGuid(), msg->tuSeq())) ? 0 : 1;
    }

    if (c->status == Client::WAITFORCS) {
        c->last_waitforcs_msec = monotonic_msec() - c->status_since_msec;
        record_waitforcs_latency(true, c->last_waitforcs_msec);
    }

    /* S2: validate and retain the assignment-bound cache-endpoint handoff
       BEFORE either branch below constructs its own relay UseCS.  The
       admissibility check itself (usecs_cache_handoff_admissible,
       services/comm.h) is factored out as a pure, independently testable
       helper -- see its own comment for why: msg->valid_payload() already
       enforced the tail's own absent-or-present AND identity-binding law
       on receipt (see MsgChannel::get_msg), so this re-checks it
       explicitly rather than trusting that channel-layer gate implicitly.
       Every field is bound from this SAME msg, including the wire id/
       epoch/nonce, so a later consumer can refuse to reuse this endpoint
       under a different assignment.  No connection is made here (see M3,
       out of scope for this change): this only stores the endpoint
       alongside the job for a later milestone to consume. */
    c->cacheHandoff = Client::CacheHandoff{};
    // Only an accepted AF_UNIX wrapper with complete accept-time peer
    // credentials may retain a cache handoff.  TCP and failed-credential
    // wrappers still use the ordinary legacy compile path, but their client
    // projection is canonically cache-absent.
    const P50CacheClientCapability current_client_cache_capability =
        p50_cache_client_capability_from_env(
            c->channel != nullptr ? c->channel->protocol : 0);
    const bool cache_service_ready = cache_client_service_ready();
    // This offer was published before the scheduler chose an assignment.
    // A supervised C-sidecar replacement while that decision is in flight
    // does not revoke the scheduler's exact assignment or widen its offer.
    // Keep the original lease here: descriptor delivery below independently
    // waits/rebinds to authenticated READY before any source operation starts.
    const bool exact_offer_owned =
        c->cache_offer_generation == c->getcs_generation &&
        c->cache_offer_generation == scheduler_session_generation &&
        c->cache_offer.protocol != 0 && c->cache_offer.profile_mask != 0 &&
        c->cache_offer_lease.has_value() &&
        c->cache_offer_lease->valid();
    const bool wrapper_cache_eligible =
        c->connection_provenance.cache_eligible() &&
        exact_offer_owned &&
        (cache_service_ready || cache_sidecar_recovery_in_progress()) &&
        c->cache_offer.protocol == msg->cache_protocol &&
        (msg->cache_profile_mask & ~c->cache_offer.profile_mask) == 0 &&
        current_client_cache_capability.protocol == msg->cache_protocol &&
        (msg->cache_profile_mask &
         ~current_client_cache_capability.profile_mask) == 0 &&
        (msg->cache_profile_mask & cache_unavailable_profile_mask) == 0;
    if (wrapper_cache_eligible && usecs_cache_handoff_admissible(*msg)) {
        c->cacheHandoff = Client::CacheHandoff{
            true, msg->job_id, msg->assignmentEpoch(), msg->assignmentNonce(),
            msg->hostname, msg->port,
            msg->cache_endpoint_port, msg->cache_protocol,
            msg->cache_profile_mask, msg->cGuid(), msg->tuSeq(),
            cache_route_state_generation, *c->cache_offer_lease};
    }
    /* Both relay projections below carry this SAME validated triple (or
       canonical 0/0/0 when c->cacheHandoff.valid is false) -- otherwise a
       client that reaches this daemon via the remote-worker branch would
       silently see cache absence even though the scheduler->daemon hop
       just validated a real endpoint.  Only derived host reachability
       (127.0.0.1 vs the real worker address) differs between the two
       branches; port/protocol/mask and the assignment identity above do
       not change with it.

       BigOracle (d23d9c5d HOLD): the TWO branches below do not deliver to
       the client the same way, and c->usecsmsg is NOT the wire vehicle in
       both of them.  The local branch's c->usecsmsg (just below) IS what
       gets sent -- see the PENDING_USE_CS drain's
       client->channel->send_msg(*client->usecsmsg).  The remote branch
       ALSO constructs a c->usecsmsg carrying this same relay_cache_*
       triple, but that object is used only for introspection/diagnostics
       (dump_internals, the web JSON endpoints) -- its actual client
       delivery is c->channel->send_msg(*msg) below, relaying the
       scheduler's OWN frame directly.  msg already carries this same
       validated triple (that is exactly what usecs_cache_handoff_
       admissible(*msg) just confirmed above), so this is correct today --
       but a regression that stripped the cache triple from *msg alone,
       without touching relay_cache_port/protocol/mask or c->usecsmsg,
       would leave every existing anchor and the local-branch test green
       while silently breaking every remote dispatch; see
       unittests/cachehandoffdaemon.cpp's remote-selected-F scenario. */
    const CacheHandoffProjection relay_cache = project_cache_handoff(
        c->connection_provenance,
        c->cacheHandoff.valid ? c->cacheHandoff.cachePort : 0,
        c->cacheHandoff.valid ? c->cacheHandoff.cacheProtocol : 0,
        c->cacheHandoff.valid ? c->cacheHandoff.cacheProfileMask : 0);
    const uint32_t relay_cache_port = relay_cache.port;
    const uint32_t relay_cache_protocol = relay_cache.protocol;
    const uint32_t relay_cache_mask = relay_cache.profile_mask;

    if (msg->hostname == remote_name && int(msg->port) == daemon_port) {
        /* S2 (BigOracle, 5th independent gap -- a REAL pre-existing product
           bug, predating the cache work): this is the ACTUAL wire vehicle
           for the self-selected-F local rewrite (see the PENDING_USE_CS
           drain comment above), so hand-rebuilding it field-by-field, as
           the code used to, meant every field NOT explicitly threaded
           through was silently hardcoded instead of preserved -- got_env
           and client_id were both wrong (true/1 always, regardless of
           what the scheduler actually decided).  client/remote.cpp's
           build_remote_int reads usecs->got_env to decide whether to send
           EnvTransferMsg; a scheduler reply saying got_env=false (this F
           does not already have the environment cached) got silently
           overridden to true, so the client skipped a required
           environment transfer and the compile could fail against an env
           this daemon does not have.  Fix: copy *msg wholesale (every
           field preserved by construction, including any added later)
           and override ONLY the two things this branch actually decides
           -- derived host reachability, and the independently validated
           cache projection above. */
        std::unique_ptr<UseCSMsg> relay(new UseCSMsg(*msg));
        relay->hostname = "127.0.0.1";
        relay->port = daemon_port;
        relay->cache_endpoint_port = relay_cache_port;
        relay->cache_protocol = relay_cache_protocol;
        relay->cache_profile_mask = relay_cache_mask;
        install_pending_usecs(c, relay.release());
        c->set_status(Client::PENDING_USE_CS, "scheduler_use_cs: local compile");
    } else {
        // Preserve every non-cache UseCS field from the scheduler.  The old
        // reconstruction hardcoded got_env/client_id and silently changed
        // assignment semantics for remote wrappers.  Only the cache triple
        // is projected according to the immutable wrapper provenance.
        std::unique_ptr<UseCSMsg> relay(new UseCSMsg(*msg));
        relay->cache_endpoint_port = relay_cache_port;
        relay->cache_protocol = relay_cache_protocol;
        relay->cache_profile_mask = relay_cache_mask;
        install_pending_usecs(c, relay.release());

        /* EXACT identity is persisted BEFORE the framed write starts, and
           the client is moved to an explicit handoff phase.  If the write
           fails partway, handle_end() then settles with the exact
           scheduler job id (a plain FROM_SUBMITTER JobDone) -- the old
           order assigned job_id only after a successful send, so the
           teardown settled with unknown_job_client_id, which the
           scheduler's cancellation sweep deliberately does not apply to a
           DISPATCHED job: the assignment, its dispatch debit, and the
           worker reservation all stayed live (release blocker A).  */
        c->job_id = msg->job_id;
        c->last_known_job_id = msg->job_id;
        c->set_status(Client::FORWARDING_USE_CS, "scheduler_use_cs: forwarding UseCS");
        ++usecs_delivery_attempts;

        {
            /* Test-only (ICECC_TEST_USECS_CUT_AT): arm a one-shot mid-frame cut
               of THIS client UseCS frame at the configured byte offset, so the
               prerequisite usecs-cut-{zero,header,body,final-short} witnesses
               reproduce the exact injection position deterministically.  Never
               armed on the production path.  */
            static const char *const usecs_cut_env = getenv("ICECC_TEST_USECS_CUT_AT");
            if (usecs_cut_env) {
                c->channel->testCutNextFlushAfter((size_t)strtoul(usecs_cut_env, nullptr, 10));
            }
        }

        /* This is the remote branch's ACTUAL client wire vehicle -- *msg,
           the scheduler's own frame, relayed directly (not c->usecsmsg,
           see the comment above this branch). */
        UseCSMsg client_reply = *msg;
        client_reply.cache_endpoint_port = relay_cache_port;
        client_reply.cache_protocol = relay_cache_protocol;
        client_reply.cache_profile_mask = relay_cache_mask;
        if (!c->channel->send_msg(client_reply)) {
            ++usecs_exact_aborts;
            handle_end(c, 143);
            return 0;
        }

        ++usecs_frames_committed;
        /* Complete framed delivery: the assignment is now DELIVERY
           UNCERTAIN until the client claims it at the worker (JobBegin
           reaches the scheduler) or this connection ends.  */
        c->set_status(Client::WAITCOMPILE, "scheduler_use_cs: remote compile");
    }

    c->job_id = msg->job_id;
    c->last_known_job_id = msg->job_id;

    return 0;
}

int Daemon::scheduler_no_cs(NoCSMsg *msg)
{
    /* G4 (bigoracle 20:13:59): drop a pre-active reply before any lookup or
       terminalization (see scheduler_use_cs). */
    if (!scheduler_session_active) {
        log_warning() << "scheduler_no_cs before session active for client "
                      << msg->client_id << "; dropping attempt" << endl;
        return 1;
    }
    Client *c = clients.find_by_client_id(msg->client_id);
    trace() << "scheduler_no_cs " << msg->job_id << " " << msg->client_id
            << " " << c << " " <<  endl;

    if (!c) {
        if (send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size(),
                                      msg->assignmentEpoch(), msg->assignmentNonce(),
                                      msg->cGuid(), msg->tuSeq()))) {
            return 0;
        }

        return 1;
    }

    /* G4 (bigoracle 18:45 P0): session ACTIVE (above); a NoCS decision applies
       only to a request published under the CURRENT active generation and still
       awaiting one; otherwise terminalize by exact job id. */
    if (!(c->getcs_published
            && c->getcs_generation == scheduler_session_generation
            && c->status == Client::WAITFORCS)) {
        log_warning() << "scheduler_no_cs unmatched job " << msg->job_id
                      << " client " << msg->client_id << "; terminalizing" << endl;
        return send_scheduler(JobDoneMsg(msg->job_id, 107, JobDoneMsg::FROM_SUBMITTER, clients.size(),
                                         msg->assignmentEpoch(), msg->assignmentNonce(),
                                         msg->cGuid(), msg->tuSeq())) ? 0 : 1;
    }

    if (c->status == Client::WAITFORCS) {
        c->last_waitforcs_msec = monotonic_msec() - c->status_since_msec;
        record_waitforcs_latency(false, c->last_waitforcs_msec);
    }

    /* S2: NO_CS carries no worker snapshot at all -- always canonical
       cache-absent.  install_cache_absent_local_decision (BigOracle exact
       blueprint) clears any handoff retained from an earlier dispatch on
       this same (reused) Client, deletes any prior usecsmsg, and installs
       the replacement atomically, so it can never leak forward.  See
       test_poison_cache_handoff_if_armed's own comment for why the
       poison/record pair below brackets this real, unmodified call. */
    const bool cache_handoff_test_poisoned_no_cs =
        test_poison_cache_handoff_if_armed(c, "no_cs");
    install_cache_absent_local_decision(
        *c,
        std::unique_ptr<UseCSMsg>(new UseCSMsg(string(), "127.0.0.1", daemon_port,
                                                msg->job_id, true, 1, 0,
                                                msg->assignmentEpoch(), msg->assignmentNonce(),
                                                msg->cGuid(), msg->tuSeq())),
        "scheduler_no_cs: local compile");
    if (cache_handoff_test_poisoned_no_cs) {
        test_record_cache_handoff_clear(c, "no_cs");
    }

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
           client->status != Client::WAITP50INPUT &&
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
    register_child(pid, getpgid(pid) > 0 ? getpgid(pid) : pid,
                   ChildRecord::ENV_INSTALL, client->client_id);
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
    complete_child_registration(client->child_pid);
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
        unregister_child(client->child_pid);
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
                            || it2->second->status == Client::WAITP50INPUT
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
    /* A remote wrapper reports its cache-route result on its existing local
       protocol-50 connection.  The observation flags are local-only and are
       never forwarded to S.  A success remains telemetry only.  An exact
       failure also requests withdrawal of that assignment: emit a separate,
       clean FROM_SUBMITTER JobDone so S can order RevokeBeforeStart against F
       and decide whether the source arm was reserved or already claimed. */
    if (m->is_p50_cache_route_observation()) {
        reconcile_cache_route_state();
        /* Assignment identity outlives route affinity.  A different request's
           success or a sidecar replacement can advance the route generation/
           READY lease while this wrapper still owns its retained UseCS.  Such
           an observation is stale for route mutation, but an exact failure
           must still withdraw its own scheduler assignment. */
        const bool assignment_bound = cl->channel != nullptr &&
            IS_PROTOCOL_VERSION(PROTOCOL_VERSION_CACHE_ADVERTISEMENT,
                                cl->channel) &&
            cl->connection_provenance.cache_eligible() &&
            cl->cacheHandoff.valid &&
            p50_source_profile_selection_valid(
                cl->cacheHandoff.cacheProfileMask) &&
            cl->cacheHandoff.cGuid != 0 &&
            cl->cacheHandoff.readyLease.has_value() &&
            cl->cacheHandoff.ordinaryPort != 0 &&
            cl->cacheHandoff.ordinaryPort <= UINT16_MAX &&
            cl->cacheHandoff.host.size() <= P50_CACHE_AFFINITY_HOST_MAX &&
            cl->cacheHandoff.host.find('\0') == string::npos;
        const P50CacheRouteObservationKind assignment_observation =
            assignment_bound
            ? p50_cache_route_observation_kind(
                  *m, cl->cacheHandoff.wireJobId,
                  cl->cacheHandoff.assignmentEpoch,
                  cl->cacheHandoff.assignmentNonce,
                  cl->cacheHandoff.cGuid, cl->cacheHandoff.tuSeq,
                  cl->cacheHandoff.cacheProfileMask)
            : P50CacheRouteObservationKind::Invalid;
        const bool exact_handoff = assignment_bound &&
            cache_route_state_lease.has_value() &&
            icecc::p50::daemon::p50_ready_lease_observation_equal(
                *cl->cacheHandoff.readyLease, *cache_route_state_lease);
        const P50CacheRouteObservationKind observation = exact_handoff
            ? assignment_observation
            : P50CacheRouteObservationKind::Invalid;
        const bool generation_current =
            cl->cacheHandoff.routeStateGeneration ==
            cache_route_state_generation;
        const bool exact_current_affinity =
            generation_current &&
            cache_affinity_host == cl->cacheHandoff.host &&
            cache_affinity_port == cl->cacheHandoff.ordinaryPort &&
            cache_affinity_profile_mask ==
                cl->cacheHandoff.cacheProfileMask;
        if (observation == P50CacheRouteObservationKind::Success &&
            generation_current &&
            (cache_unavailable_profile_mask &
             cl->cacheHandoff.cacheProfileMask) == 0) {
            cache_affinity_host = cl->cacheHandoff.host;
            cache_affinity_profile_mask = cl->cacheHandoff.cacheProfileMask;
            cache_affinity_port = cl->cacheHandoff.ordinaryPort;
            if (cache_route_state_generation !=
                std::numeric_limits<uint64_t>::max())
                ++cache_route_state_generation;
        } else if (observation == P50CacheRouteObservationKind::Failure) {
            /* A late failed attempt must not erase a newer success.  Clear
               only the exact route whose generation was current when this
               assignment was dispatched. */
            if (exact_current_affinity) {
                cache_affinity_host.clear();
                cache_affinity_profile_mask = 0;
                cache_affinity_port = 0;
                if (cache_route_state_generation !=
                    std::numeric_limits<uint64_t>::max())
                    ++cache_route_state_generation;
            }
        } else if (observation ==
                   P50CacheRouteObservationKind::
                       PermanentLocalProfileFailure) {
            /* This typed fact is lease-wide, not one transport attempt.  It
               suppresses only P29V1.  Preserve a newer non-P29 affinity, but
               never retain a contradictory P29 hint. */
            bool changed =
                (cache_unavailable_profile_mask & CACHE_PROFILE_P29V1) == 0;
            cache_unavailable_profile_mask |= CACHE_PROFILE_P29V1;
            if (cache_affinity_profile_mask == CACHE_PROFILE_P29V1) {
                cache_affinity_host.clear();
                cache_affinity_profile_mask = 0;
                cache_affinity_port = 0;
                changed = true;
            }
            if (changed && cache_route_state_generation !=
                std::numeric_limits<uint64_t>::max())
                ++cache_route_state_generation;
        } else if (observation ==
                   P50CacheRouteObservationKind::
                       LocalSidecarReplacementRequired) {
            /* The sidecar sends this typed result only after its response was
               ACKed, so lifecycle retirement cannot cut off the reply in
               flight.  Replacement clears every possibly ambiguous route
               and bounded replay ledger under the exact current READY lease. */
            if (cache_adapter != nullptr) {
                cache_adapter->outer_request_replacement();
                reconcile_cache_route_state();
            }
        } else {
            log_warning()
                << "ignored invalid or stale P50 cache-route observation for job "
                << m->job_id << endl;
        }

        const bool failed_assignment =
            assignment_observation == P50CacheRouteObservationKind::Failure ||
            assignment_observation ==
                P50CacheRouteObservationKind::PermanentLocalProfileFailure ||
            assignment_observation ==
                P50CacheRouteObservationKind::LocalSidecarReplacementRequired;
        if (failed_assignment &&
            cl->job_id == cl->cacheHandoff.wireJobId &&
            scheduler_owns_getcs_assignment(cl)) {
            JobDoneMsg withdrawal(
                cl->cacheHandoff.wireJobId, m->exitcode,
                JobDoneMsg::FROM_SUBMITTER, clients.size(),
                cl->cacheHandoff.assignmentEpoch,
                cl->cacheHandoff.assignmentNonce,
                cl->cacheHandoff.cGuid, cl->cacheHandoff.tuSeq);
            if (!send_scheduler(withdrawal)) {
                return false;
            }
            /* last_known_job_id deliberately remains as the audit identity;
               zeroing the active id makes the later End/HUP idempotent. */
            cl->job_id = 0;
        }
        return true;
    }

    if (cl->getcs_expected > 1) {
        /* G4 BATCH: the client reports one of its N decisions done.  Match by
           the EXACT job id and drop that entry -- do NOT assert against the
           scalar cl->job_id (a batch request holds many).  Forward the exact
           JobDone to S only for an assignment owned by the current active
           session; the request is complete once every recorded entry settles.
           (Remote batch decisions hold no local compile slot, so the scalar
           CLIENTWORK capacity accounting below does not apply.) */
        for (std::vector<uint32_t>::iterator it = cl->getcs_batch_jobids.begin();
                it != cl->getcs_batch_jobids.end(); ++it) {
            if (*it == m->job_id) {
                cl->getcs_batch_jobids.erase(it);
                break;
            }
        }
        if (cl->getcs_batch_jobids.empty() && cl->getcs_delivered >= cl->getcs_expected) {
            cl->set_status(Client::JOBDONE, "handle_job_done: batch complete");
        }
        m->client_count = clients.size();
        if (!scheduler_owns_getcs_assignment(cl)) {
            return true;
        }
        return send_scheduler(*m);
    }
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

    /* G4 (20:13:59/20:32:36): the local daemon accounting above always runs;
       forward the JobDone to S only for a GetCS request owned by the current
       active session.  An unpublished schedulerless fallback settles locally
       and emits no scheduler frame. */
    if (!scheduler_owns_getcs_assignment(cl)) {
        return true;
    }
    return send_scheduler(*msg);
}

P50CacheClientCapability Daemon::project_getcs_cache_route(
    GetCSMsg *request, P50CacheClientCapability capability)
{
    assert(request != nullptr);

    capability = cache_capability_for_scheduler(capability);

    /* Cache revision 2 is carried inside the protocol-50 GetCS tail, but the
       scheduler's protocol-50 decoder only accepts revision 1. Keep the
       wrapper's local channel and sidecar opted into R2; project canonical
       cache absence only on this older scheduler hop. This preserves ordinary
       remote scheduling without sending an R2-shaped GetCS to a peer that
       cannot validate it. */
    /* The wrapper may contribute exactly one routing restriction: a failed
       ordinary F endpoint for its bounded strict retry.  C still owns the
       capability/kill-switch decision.  Preserve the restriction only when
       that decision remains P50-capable; unlike the daemon-wide warm hint it
       narrows selection and can never grant a profile or endpoint. */
    const uint32_t requested_avoid_port = request->cache_retry_avoid_port;
    const string requested_avoid_host = request->cache_retry_avoid_host;
    const bool preserve_retry_avoid = capability.protocol != 0 &&
        capability.profile_mask != 0 &&
        p50_cache_retry_avoid_is_present(
            requested_avoid_port, requested_avoid_host);

    request->cache_protocol = capability.protocol;
    request->cache_profile_mask = capability.profile_mask;
    request->cache_affinity_profile_mask = 0;
    request->cache_affinity_port = 0;
    request->cache_affinity_host.clear();
    request->cache_retry_avoid_port = 0;
    request->cache_retry_avoid_host.clear();

    if (preserve_retry_avoid) {
        request->cache_retry_avoid_port = requested_avoid_port;
        request->cache_retry_avoid_host = requested_avoid_host;
        trace() << "P50 retry exclusion forwarded endpoint="
                << requested_avoid_host << ":" << requested_avoid_port
                << endl;
        return capability;
    }

    if (capability.profile_mask != 0 &&
        cache_affinity_profile_mask != 0 &&
        (cache_affinity_profile_mask & ~capability.profile_mask) == 0 &&
        cache_affinity_port != 0 && cache_affinity_port <= UINT16_MAX &&
        !cache_affinity_host.empty()) {
        request->cache_affinity_profile_mask = cache_affinity_profile_mask;
        request->cache_affinity_port = cache_affinity_port;
        request->cache_affinity_host = cache_affinity_host;
    }
    return capability;
}

P50CacheClientCapability Daemon::cache_capability_for_scheduler(
    P50CacheClientCapability capability) const noexcept
{
    if (scheduler != nullptr &&
        capability.protocol == CACHE_WIRE_REVISION_R2 &&
        !protocol_supports_cache_r2(scheduler->protocol)) {
        trace() << "P51 cache request withheld from ordinary protocol-"
                << scheduler->protocol << " scheduler" << endl;
        return {};
    }
    return capability;
}

void Daemon::handle_old_request()
{
    if (session_quiescence_pending() || child_ownership_gate.admission_blocked())
        return;
    const unsigned int compile_limit = std::max((unsigned int)1, max_kids);
    const unsigned int preprocess_limit = std::max((unsigned int)1, max_preprocess_kids);

    /* G4 (18:21): a LOGIN_ATTEMPT no longer freezes the local lane.  The
       schedulerless local lanes (LINKJOB / PENDING_USE_CS below) run during the
       attempt; only SCHEDULER-owned emission is gated -- a held GetCS is
       re-driven only once ACTIVE (below), and a local job started before the
       session activates is tagged UNOWNED_LOCAL so it never emits JobLocalBegin/
       JobLocalDone to S (a later ConfCS must not retroactively announce it). */
    /* Re-drive any GetCS held during a prior LOGIN_ATTEMPT now that the session
       is active. */
    if (scheduler_session_active) {
        for (const auto &kv : clients) {
            Client *c = kv.second;
            if (!c->deferred_getcs) {
                continue;
            }
            GetCSMsg *g = c->deferred_getcs;
            const bool waiting_for_cache =
                c->deferred_getcs_waits_for_cache;
            const bool cache_ready =
                waiting_for_cache && cache_client_sidecar_ready();
            if (waiting_for_cache && !cache_ready &&
                cache_sidecar_recovery_in_progress()) {
                continue;
            }
            c->deferred_getcs = nullptr;
            /* The request may have arrived during a reconnecting S session.
               Re-admit only the capability that the wrapper originally
               offered, and only while its exact authenticated READY lease is
               still current.  This can narrow but never widen the held
               request.  Recompute affinity at the send boundary so a lease
               change cannot leak a stale route hint. */
            P50CacheClientCapability revalidated{};
            if (waiting_for_cache && cache_ready &&
                c->connection_provenance.cache_eligible()) {
                const P50CacheClientCapability current =
                    p50_cache_client_capability_from_env(
                        c->channel != nullptr ? c->channel->protocol : 0);
                if (g->cache_protocol == current.protocol) {
                    revalidated.protocol = current.protocol;
                    revalidated.profile_mask =
                        g->cache_profile_mask & current.profile_mask &
                        ~cache_unavailable_profile_mask;
                    if (revalidated.profile_mask == 0)
                        revalidated = {};
                }
                if (revalidated.profile_mask != 0 &&
                    cache_route_state_lease.has_value()) {
                    c->cache_offer_lease = *cache_route_state_lease;
                }
            } else if (!waiting_for_cache && c->cache_offer.protocol != 0 &&
                c->cache_offer.profile_mask != 0 &&
                c->cache_offer_lease.has_value() &&
                cache_client_service_ready() &&
                cache_route_state_lease.has_value() &&
                icecc::p50::daemon::p50_ready_lease_observation_equal(
                    *c->cache_offer_lease, *cache_route_state_lease)) {
                const P50CacheClientCapability current =
                    p50_cache_client_capability_from_env(
                        c->channel != nullptr ? c->channel->protocol : 0);
                if (current.protocol == c->cache_offer.protocol) {
                    revalidated.protocol = current.protocol;
                    revalidated.profile_mask =
                        c->cache_offer.profile_mask & current.profile_mask &
                        ~cache_unavailable_profile_mask;
                    if (revalidated.profile_mask == 0)
                        revalidated = {};
                }
            }
            revalidated = project_getcs_cache_route(g, revalidated);
            c->cache_offer = revalidated;
            if (revalidated.profile_mask == 0)
                c->cache_offer_lease.reset();
            c->deferred_getcs_waits_for_cache = false;
            g->client_count = clients.size();
            g->command_summary.clear();
            const bool sent = send_scheduler(*g);
            delete g;
            if (!sent) {
                return;   /* session lost; answer_client_requests runs the cleanup */
            }
            c->getcs_published = true;   /* G4 (17:20#1): re-driven held GetCS is now published to S */
            c->getcs_generation = scheduler_session_generation;   /* G4 (18:45 P0): under the current ACTIVE generation */
            c->cache_offer_generation = scheduler_session_generation;
            c->set_status(Client::WAITFORCS, "handle_old_request: re-driven held GetCS");
        }
    }

    /* G4 (18:19#3): a LOGIN_ATTEMPT that failed/expired leaves requests held for
       it stranded (deferred_getcs set, but now no active session and no pending
       attempt).  Resolve them instead of waiting forever: a single-reply request
       takes the existing schedulerless local fallback; a multi-reply request
       (which one local UseCS cannot satisfy) closes only that client so its own
       local fallback runs.  Snapshot first -- handle_end() erases from clients. */
    if (!scheduler_session_active && !scheduler_login_pending) {
        std::vector<Client *> stranded;
        for (const auto &kv : clients) {
            if (kv.second->deferred_getcs) {
                stranded.push_back(kv.second);
            }
        }
        for (Client *c : stranded) {
            GetCSMsg *g = c->deferred_getcs;
            if (g->remote_required == 1) {
                /* An explicit remote-only request remains private while no
                   scheduler session is active.  reconnect() keeps advancing;
                   the ACTIVE branch above revalidates and publishes it. */
                continue;
            }
            if (g->count <= 1) {
                /* S2: schedulerless local fallback, no worker snapshot --
                   canonical cache-absent (see scheduler_no_cs).
                   install_cache_absent_local_decision (BigOracle exact
                   blueprint) atomically clears any handoff retained from
                   an earlier dispatch on this same (reused) Client,
                   deletes any prior usecsmsg, and installs the
                   replacement.  See test_poison_cache_handoff_if_armed's
                   own comment for why the poison/record pair below
                   brackets this real, unmodified call. */
                const bool cache_handoff_test_poisoned_stranded =
                    test_poison_cache_handoff_if_armed(c, "old_request_stranded");
                install_cache_absent_local_decision(
                    *c,
                    std::unique_ptr<UseCSMsg>(new UseCSMsg(g->target, "127.0.0.1",
                                                            daemon_port, c->client_id,
                                                            true, 1, 0)),
                    "handle_old_request: held GetCS -> local fallback (attempt failed)");
                if (cache_handoff_test_poisoned_stranded) {
                    test_record_cache_handoff_clear(c, "old_request_stranded");
                }
                c->job_id = c->client_id;
                c->last_known_job_id = c->client_id;
                delete c->deferred_getcs;
                c->deferred_getcs = nullptr;
                c->deferred_getcs_waits_for_cache = false;
            } else {
                delete c->deferred_getcs;
                c->deferred_getcs = nullptr;
                c->deferred_getcs_waits_for_cache = false;
                handle_end(c, 111);   /* multi-reply: close this client so its own local fallback runs */
            }
        }
    }

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

           fulljob semantics: start when any compile slot is free, then
           reserve them all (clients.active_processes += compile_limit);
           the bounded preprocess lane keeps running alongside.  */
        /* Select the best ADMISSIBLE LINKJOB by (niceness, client_id).
           Admissibility must be part of the comparison, not a test applied
           to the global best: otherwise a blocked compile job hides an
           admissible preprocess job (and vice versa), leaving a whole lane
           idle.  */
        Client *client = nullptr;
        for (const auto &it : clients) {
            Client *candidate = it.second;
            if (candidate->status != Client::LINKJOB) {
                continue;
            }

            const bool preprocess_job = candidate->local_preprocess && !candidate->fulljob;
            bool admissible;
            if (candidate->fulljob) {
                admissible = compile_capacity && fulljob_active == 0;
            } else if (preprocess_job) {
                admissible = preprocess_capacity;
            } else {
                admissible = compile_capacity && fulljob_active == 0;
            }

            if (!admissible) {
                continue;
            }

            if (client == nullptr
                || candidate->niceness < client->niceness
                || (candidate->niceness == client->niceness
                    && candidate->client_id < client->client_id)) {
                client = candidate;
            }
        }

        if (client) {
            trace() << "send JobLocalBeginMsg to client" << endl;
            const bool preprocess_job = client->local_preprocess && !client->fulljob;

            if (!client->channel->send_msg(JobLocalBeginMsg())) {
                log_warning() << "can't send start message to client" << endl;
                handle_end(client, 112);
            } else {
                client->set_status(Client::CLIENTWORK, "handle_old_request: local job started");
                /* G4 (18:21): tag ownership at start -- ACTIVE -> owned by the
                   current generation (announced to S below); LOGIN_ATTEMPT or
                   offline -> UNOWNED_LOCAL (0), never announced to S. */
                client->local_owner_generation =
                    scheduler_session_active ? scheduler_session_generation : 0;
                if (preprocess_job) {
                    client->running_preprocess = true;
                    ++preprocess_active_processes;
                    trace() << "pushed local preprocess job " << client->client_id << endl;
                } else if (client->fulljob) { // reserve every compile slot
                    client->running_preprocess = false;
                    clients.active_processes += compile_limit;
                    ++fulljob_active;
                    trace() << "pushed full local job " << client->client_id << endl;
                } else {
                    client->running_preprocess = false;
                    clients.active_processes++;
                    trace() << "pushed local job " << client->client_id << endl;
                }
                /* G4 (18:21): announce to S only for an ACTIVE-owned local job;
                   an UNOWNED_LOCAL job runs locally but emits no scheduler frame. */
                if (scheduler_session_active
                        && client->local_owner_generation == scheduler_session_generation) {
                    if (!send_scheduler(JobLocalBeginMsg(client->client_id, client->outfile,
                            client->fulljob, client->local_reason, client->command_line,
                            preprocess_job ? JobLocalBeginMsg::LocalFlagPreprocessOnly
                                           : JobLocalBeginMsg::LocalFlagNone))) {
                        return;
                    }
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
            bool exact_p50_ready = false;
            for (const auto &entry : clients) {
                const Client *candidate = entry.second;
                if (candidate->status == Client::TOCOMPILE &&
                    candidate->job != nullptr &&
                    candidate->job->usesP50Input() &&
                    candidate->p50_input_lease_state ==
                        Client::P50InputLeaseState::Active) {
                    exact_p50_ready = true;
                    break;
                }
            }
            // A scheduler-assigned P50 job with an authenticated, exact input
            // lease already owns a compiler slot.  Do not deadlock it behind
            // the coarse load sentinel (1000); retain load shedding for all
            // unassigned/local work.
            if (!exact_p50_ready)
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
            const int compiler_input_fd = client->p50_input_fd;
            client->p50_input_fd = -1;
            std::optional<icecc::p50::forkfd::ForkSourceLease>
                compiler_input_source;
            if (job->usesP50Input() && compiler_input_fd >= 0 &&
                client->p50_input_lease.has_value()) {
                const auto& attachment = *client->p50_input_lease;
                const auto& input = job->compileInputIdentity();
                const icecc::p50::InputRecordKey expected_key{
                    icecc::p50::CStoreGuid{input.c_store_guid},
                    icecc::p50::TuSeq{input.tu_seq}};
                const icecc::p50::InputLeaseOwner expected_owner{
                    job->jobID(), job->assignmentEpoch(), job->assignmentNonce()};
                const bool attachment_matches =
                    attachment.key == expected_key &&
                    attachment.owner == expected_owner &&
                    attachment.request_id == input.request_id;
                if (attachment_matches) {
                    compiler_input_source =
                        icecc::p50::forkfd::mint_attached_input_source(
                            compiler_input_fd, attachment.request_id);
                } else {
                    log_warning() << "P50 compiler attachment binding mismatch for job "
                                  << job->jobID() << " request "
                                  << attachment.request_id << "/"
                                  << input.request_id << endl;
                }
            }
            if (job->usesP50Input() && !compiler_input_source.has_value()) {
                log_warning() << "P50 attached input failed compiler-fork validation for job "
                              << job->jobID() << endl;
                if (compiler_input_fd >= 0)
                    (void)::close(compiler_input_fd);
                handle_end(client, 146);
                continue;
            }
            pid = handle_connection(envbasedir, job, client->channel, sock,
                                    mem_limit, user_uid, user_gid,
                                    compiler_input_fd,
                                    std::move(compiler_input_source));
            trace() << "handle connection returned " << pid << endl;

            if (pid > 0) {
                /* Ownership is verified and registered BEFORE the client
                   is exposed as WAITFORCHILD.  The compile worker must
                   lead its own process group (set on both fork sides);
                   if neither side won the race, the subtree cannot be
                   targeted for exact termination -- an explicit fatal
                   ownership error, not a silent degradation.  */
                if (getpgid(pid) != pid && setpgid(pid, pid) != 0
                        && getpgid(pid) != pid) {
                    log_error() << "cannot establish process group for"
                                << " compile child " << pid
                                << ": fatal child-ownership error" << endl;
                    kill(pid, SIGKILL);
                    while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
                    handle_end(client, 144);
                    continue;
                }
                if (std::getenv("ICECC_P50_DEBUG_ATTACH") != nullptr
                        && job->usesP50Input()) {
                    trace() << "P50_TEST_COMPILER_CHILD job=" << job->jobID()
                            << " epoch=" << job->assignmentEpoch()
                            << " nonce=" << job->assignmentNonce()
                            << " pid=" << pid << " pgid=" << getpgid(pid)
                            << endl;
                }
                register_child(pid, pid, ChildRecord::COMPILER,
                               client->client_id);
                mark_assignment_claimed_or_later(job->jobID());
                current_kids++;
                client->set_status(Client::WAITFORCHILD, "handle_old_request: compiling locally (child running)");
                client->pipe_from_child = sock;
                client->child_pid = pid;

                if (!send_scheduler(JobBeginMsg(job->jobID(), clients.size()))) {
                    /* G4: the compensating send failed and closed the scheduler
                       session.  Bail immediately -- do NOT continue this loop
                       and start further clients against a dead session; the
                       caller (answer_client_requests) runs the single
                       finish_scheduler_loss() and leaves the turn. */
                    log_info() << "failed sending scheduler about " << job->jobID() << endl;
                    return;
                }
            } else {
                handle_end(client, 117);
            }

            continue;
        }

        break;
    }
}

static bool p50_completion_matches_retained_lease(
    const icecc::p50::P50CompletionRecord& record,
    const std::optional<icecc::p50::InputFdRequest>& retained) noexcept
{
    if (!retained.has_value())
        return false;
    const icecc::p50::InputFdRequest& lease = *retained;
    return lease.owner.logical_job == record.job_id &&
           lease.owner.assignment_epoch == record.assignment_epoch &&
           lease.owner.assignment_nonce == record.assignment_nonce &&
           lease.key.c_store_guid.bytes == record.c_store_guid &&
           lease.key.tu_seq.value == record.tu_seq &&
           lease.request_id == record.request_id &&
           record.attempt_id == record.assignment_nonce;
}

bool Daemon::handle_compile_done(Client *client)
{
    assert(client->status == Client::WAITFORCHILD);
    assert(client->child_pid > 0);
    assert(client->pipe_from_child >= 0);

    const bool p50_input = client->job->usesP50Input();
    icecc::p50::P50CompletionObservation p50_observation;
    if (p50_input) {
        if (!client->p50_completion_reader.has_value()) {
            client->p50_completion_reader.emplace(
                client->pipe_from_child, *client->job);
        }
        const icecc::p50::P50CompletionPumpResult completion =
            client->p50_completion_reader->pump();
        if (completion == icecc::p50::P50CompletionPumpResult::Pending)
            return true;
        p50_observation =
            client->p50_completion_reader->observation();
    }

    JobDoneMsg *msg = new JobDoneMsg(client->job->jobID(), -1, JobDoneMsg::FROM_SERVER,
                                     clients.size(), client->job->assignmentEpoch(),
                                     client->job->assignmentNonce(), client->job->cGuid(),
                                     client->job->tuSeq());
    assert(msg);
    const bool slot_released = complete_child_registration(client->child_pid);
    if (!slot_released || current_kids == 0) {
        log_error() << "compiler completion has no exactly-once capacity slot for pid "
                    << client->child_pid << "; failing closed" << endl;
        child_ownership_gate.mark_sticky_failure();
    } else {
        --current_kids;
    }

    unsigned int job_stat[8];
    int end_status = 151;

    if (!p50_input &&
        read(client->pipe_from_child, job_stat, sizeof(job_stat)) == sizeof(job_stat)) {
        msg->in_uncompressed = job_stat[JobStatistics::in_uncompressed];
        msg->in_compressed = job_stat[JobStatistics::in_compressed];
        msg->out_compressed = msg->out_uncompressed = job_stat[JobStatistics::out_uncompressed];
        end_status = msg->exitcode = job_stat[JobStatistics::exit_code];
        msg->real_msec = job_stat[JobStatistics::real_msec];
        msg->user_msec = job_stat[JobStatistics::user_msec];
        msg->sys_msec = job_stat[JobStatistics::sys_msec];
        msg->pfaults = job_stat[JobStatistics::sys_pfaults];
    } else if (p50_input && p50_observation.valid()) {
        const icecc::p50::P50CompletionRecord& record =
            p50_observation.record;
        msg->in_uncompressed = record.stats[JobStatistics::in_uncompressed];
        msg->in_compressed = record.stats[JobStatistics::in_compressed];
        msg->out_compressed = msg->out_uncompressed =
            record.stats[JobStatistics::out_uncompressed];
        end_status = msg->exitcode = record.result_status;
        msg->real_msec = record.stats[JobStatistics::real_msec];
        msg->user_msec = record.stats[JobStatistics::user_msec];
        msg->sys_msec = record.stats[JobStatistics::sys_msec];
        msg->pfaults = record.stats[JobStatistics::sys_pfaults];

        const bool exact_retained_lease =
            p50_completion_matches_retained_lease(
                record, client->p50_input_lease);
        if (!exact_retained_lease) {
            log_warning() << "P50 child completion does not match retained lease for job "
                          << client->job->jobID() << endl;
        } else if (p50_observation.disposition ==
                   icecc::p50::P50CompletionDisposition::Accepted) {
            settle_p50_input(
                client,
                icecc::p50::InputLifecycleAction::CloseAcceptedJob,
                "submitter accepted complete result");
        } else if (p50_observation.disposition ==
                   icecc::p50::P50CompletionDisposition::DefinitiveCancel) {
            settle_p50_input(
                client, icecc::p50::InputLifecycleAction::CancelJob,
                "submitter definitive cancellation");
        }
    }

    close(client->pipe_from_child);
    client->pipe_from_child = -1;
    client->p50_completion_reader.reset();
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

static int p50_attachment_failure_exit_code(
    icecc::p50::InputFdAttachmentStatus status) noexcept
{
    using Status = icecc::p50::InputFdAttachmentStatus;
    switch (status) {
    case Status::InvalidArgument:
    case Status::UnsupportedPlatform:
    case Status::MalformedRequest:
    case Status::UnknownRecord:
    case Status::StaleIdentity:
    case Status::MaterializationFailed:
        return 146; // authoritative record/identity refusal
    case Status::Accepted:
    case Status::PeerUnauthenticated:
    case Status::HandshakeFailed:
    case Status::Timeout:
    case Status::Disconnected:
    case Status::HandoffFailed:
        return 152; // sidecar transport or malformed accepted handoff
    }
    return 152;
}

bool Daemon::advance_p50_attachments(const std::vector<pollfd>& pollfds)
{
    for (const auto& entry : clients) {
        Client *client = entry.second;
        if (!client->p50_attachment) continue;
        short revents = 0;
        for (const auto& descriptor : pollfds)
            if (descriptor.fd == client->p50_attachment->poll_fd())
                revents |= descriptor.revents;
        client->p50_attachment->advance(revents);
        if (!client->p50_attachment->done()) continue;
        client->p50_attachment_result = client->p50_attachment->take_result();
        client->p50_attachment.reset();
        if (!client->p50_attachment_result || !client->p50_attachment_job) {
            handle_end(client, 146);
            return true;
        }
        CompileFileMsg resume(client->p50_attachment_job.release(), true);
        // Re-enter the exact ownership/ReadyLease checks before publishing
        // the returned descriptor. The message owns the job until takeJob().
        const bool alive = handle_compile_file(client, &resume);
        (void)alive;
        // Completion may erase a client or settle the scheduler; rebuild the
        // poll snapshot rather than reusing references from this turn.
        return true;
    }
    return false;
}

bool Daemon::handle_compile_file(Client *client, Msg *msg)
{
    CompileJob *job = dynamic_cast<CompileFileMsg *>(msg)->takeJob();
    assert(client);
    assert(job);

    // The source arm arrives on the short-lived cache wrapper, while this
    // CompileFile arrives on the original ordinary connection.  Once the
    // cache wrapper has detached its fd, join only that exact retained owner
    // and move the claim before admitting any compiler input.
    if (!client->p50_source_arm_fields.has_value()) {
        Client *detached_owner = nullptr;
        for (const auto &entry : clients) {
            Client *candidate = entry.second;
            if (!candidate->p50_cache_session_detached ||
                candidate->status != Client::WAITP50INPUT ||
                !candidate->p50_source_arm_fields.has_value() ||
                candidate->p50_source_compile_pending || candidate->job != nullptr)
                continue;
            const P50SourceArmFields &arm = *candidate->p50_source_arm_fields;
            if (!job->usesP50Input() || !job->hasAssignmentIdentity() ||
                arm.wire_job_id != job->jobID() ||
                arm.assignment_epoch != job->assignmentEpoch() ||
                arm.assignment_nonce != job->assignmentNonce())
                continue;
            const CompileInputIdentity &input = job->compileInputIdentity();
            if (!input.validPresent() ||
                !((input.profile == CompileInputIdentity::P29V1Profile &&
                   arm.cache_profile == CACHE_PROFILE_P29V1) ||
                  (input.profile == CompileInputIdentity::ZstdTuProfile &&
                   arm.cache_profile == CACHE_PROFILE_ZSTD_TU) ||
                  (input.profile == CompileInputIdentity::ZstdRouteProfile &&
                   arm.cache_profile == CACHE_PROFILE_ZSTD_ROUTE)) ||
                input.c_store_guid != arm.c_store_guid ||
                input.attempt_id != arm.compiler_attempt ||
                input.request_id != arm.source_request_id)
                continue;
            auto live = live_assignments.find(arm.wire_job_id);
            if (live == live_assignments.end() ||
                live->second.phase != WorkerAssignment::Claimed ||
                live->second.claimant != static_cast<uint32_t>(candidate->client_id) ||
                live->second.key.epoch != arm.assignment_epoch ||
                live->second.key.nonce != arm.assignment_nonce)
                continue;
            detached_owner = candidate;
            break;
        }

        if (detached_owner != nullptr) {
            const P50SourceArmFields retained_arm =
                *detached_owner->p50_source_arm_fields;
            auto live = live_assignments.find(retained_arm.wire_job_id);
            // The single-threaded daemon loop makes this check-and-transfer
            // atomic with respect to the exact owner found above.
            if (live != live_assignments.end() &&
                live->second.phase == WorkerAssignment::Claimed &&
                live->second.claimant ==
                    static_cast<uint32_t>(detached_owner->client_id)) {
                live->second.claimant = static_cast<uint32_t>(client->client_id);
                client->p50_source_arm_fields =
                    std::move(detached_owner->p50_source_arm_fields);
                client->p50_source_f_lease =
                    std::move(detached_owner->p50_source_f_lease);
                client->p50_source_f_store_generation =
                    detached_owner->p50_source_f_store_generation;
                client->p50_source_armed_ack =
                    std::move(detached_owner->p50_source_armed_ack);
                client->p50_source_deadline_msec =
                    detached_owner->p50_source_deadline_msec;
                client->p50_source_arm_provenance =
                    client->connection_provenance;
                client->p50_source_compile_pending = false;
                client->job_id = retained_arm.wire_job_id;
                client->last_known_job_id = retained_arm.wire_job_id;
                detached_owner->p50_input_wait.close();
                // Moving an engaged optional leaves it engaged (with a
                // moved-from value); clear every source identity before
                // handle_end so wrapper teardown cannot settle the claim we
                // just transferred to the ordinary connection.
                detached_owner->p50_source_arm_fields.reset();
                detached_owner->p50_source_f_lease.reset();
                detached_owner->p50_source_armed_ack.reset();
                detached_owner->p50_source_f_store_generation = 0;
                detached_owner->p50_source_arm_provenance =
                    ConnectionProvenance{};
                detached_owner->p50_source_deadline_msec = 0;
                detached_owner->p50_source_compile_pending = false;
                detached_owner->job_id = 0;
                detached_owner->last_known_job_id = 0;
                detached_owner->p50_cache_session_detached = false;
                handle_end(detached_owner, 121);
                if (!client->p50_input_wait.arm_input(retained_arm)) {
                    delete job;
                    (void)client->channel->send_msg(EndMsg());
                    handle_end(client, 150);
                    return false;
                }
                client->set_status(Client::WAITP50INPUT,
                                   "p50: detached source owner joined ordinary connection");
            }
        }
    }

    if (client->p50_source_arm_fields.has_value()) {
        // A later CompileFile is the same assignment claimant, not a second
        // source arm.  Revalidate the retained F lease at the admission
        // boundary because a replacement can be observed between poll turns.
        const bool current_lease =
            cache_adapter != nullptr &&
            cache_advertisement_snapshot().present() &&
            cache_adapter->outer_current_ready_lease().has_value() &&
            client->p50_source_f_lease.has_value() &&
            icecc::p50::daemon::p50_ready_lease_observation_equal(
                *client->p50_source_f_lease,
                *cache_adapter->outer_current_ready_lease());
        const bool current_store_generation = current_lease &&
            client->p50_source_f_store_generation != 0 &&
            client->p50_source_f_store_generation ==
                cache_adapter->outer_current_ready_lease()->store_generation;
        const bool exact_claim = current_lease &&
            current_store_generation &&
            client->source_arm_matches_compile_claim(*job) &&
            authorize_assignment_claim(
                *job, static_cast<uint32_t>(client->client_id),
                client->channel != nullptr ? client->channel->protocol : 0);
        if (!exact_claim || client->job != nullptr || client->p50_attachment ||
            client->p50_source_compile_pending) {
            log_warning() << "P50 CompileFile did not match one live source owner for job "
                          << job->jobID() << endl;
            delete job;
            (void)client->channel->send_msg(EndMsg());
            handle_end(client, 150);
            return false;
        }

        const CompileInputIdentity &input = job->compileInputIdentity();
        const icecc::p50::InputRecordKey input_key{
            icecc::p50::CStoreGuid{input.c_store_guid},
            icecc::p50::TuSeq{input.tu_seq}};
        const icecc::p50::InputLeaseOwner input_owner{
            job->jobID(), job->assignmentEpoch(), job->assignmentNonce()};
        if (!client->p50_attachment_result.has_value()) {
            client->p50_attachment = cache_adapter->begin_attach_input(
                input_key, input_owner, input.request_id);
            if (!client->p50_attachment) {
                delete job;
                (void)client->channel->send_msg(EndMsg());
                handle_end(client, 146);
                return false;
            }
            client->p50_attachment_lease = client->p50_attachment->request();
            client->p50_attachment_job.reset(job);
            client->p50_attachment_started_msec = monotonic_msec();
            trace() << "P50_INPUT_ATTACH_BEGIN job=" << job->jobID()
                    << " epoch=" << job->assignmentEpoch()
                    << " nonce=" << job->assignmentNonce()
                    << " request=" << input.request_id << endl;
            return true;
        }
        auto attached = std::move(*client->p50_attachment_result);
        client->p50_attachment_result.reset();
        trace() << "P50_INPUT_ATTACH_END job=" << job->jobID()
                << " epoch=" << job->assignmentEpoch()
                << " nonce=" << job->assignmentNonce()
                << " request=" << input.request_id
                << " elapsed_ms=" << monotonic_msec() - client->p50_attachment_started_msec
                << " status=" << static_cast<unsigned>(attached.status) << endl;
        if (attached.status != icecc::p50::InputFdAttachmentStatus::Accepted ||
            !attached.fd.valid() || !attached.lease.has_value()) {
            log_warning() << "P50 compiler input attachment failed for job "
                          << job->jobID() << " (status "
                          << static_cast<unsigned>(attached.status) << ")" << endl;
            delete job;
            (void)client->channel->send_msg(EndMsg());
            handle_end(client,
                       p50_attachment_failure_exit_code(attached.status));
            return false;
        }
        const P50SourceArmFields &arm = *client->p50_source_arm_fields;
        icecc::p50::P50SourceArm ready_arm;
        ready_arm.wire_job_id = arm.wire_job_id;
        ready_arm.assignment_epoch = arm.assignment_epoch;
        ready_arm.assignment_nonce = arm.assignment_nonce;
        ready_arm.selected_f_host = arm.selected_f_host;
        ready_arm.selected_f_ordinary_port = arm.selected_f_ordinary_port;
        ready_arm.selected_f_cache_port = arm.selected_f_cache_port;
        ready_arm.cache_protocol = arm.cache_protocol;
        ready_arm.cache_profile = arm.cache_profile;
        ready_arm.logical_job = arm.logical_job;
        ready_arm.attempt_id = arm.compiler_attempt;
        ready_arm.c_store_generation = arm.c_store_generation;
        ready_arm.c_store_guid = icecc::p50::CStoreGuid{arm.c_store_guid};
        ready_arm.source_request_id = arm.source_request_id;
        ready_arm.source_mode = arm.source_mode;
        icecc::p50::P50InputReady input_ready;
        input_ready.arm = std::move(ready_arm);
        input_ready.tu_seq = icecc::p50::TuSeq{input.tu_seq};
        input_ready.raw_bytes = input.raw_bytes;
        input_ready.raw_digest = icecc::Digest128{input.raw_digest};
        input_ready.f_store_guid = client->p50_source_f_lease.has_value()
                                       ? client->p50_source_f_lease->f_store_guid
                                       : icecc::p50::FStoreGuid{};
        input_ready.attachment_store_generation =
            client->p50_source_f_store_generation;
        input_ready.attachment_request_id = input.request_id;
        input_ready.ready_event_id = input.request_id;
        int attached_fd = attached.fd.get();
        if (!client->accept_p50_input(*client->p50_source_arm_fields,
                                      input_ready, attached_fd)) {
            (void)::close(attached.fd.release());
            delete job;
            (void)client->channel->send_msg(EndMsg());
            handle_end(client, 146);
            return false;
        }
        // accept_p50_input() moved the descriptor into the daemon's exact
        // compiler-attempt slot; do not let the temporary result close it.
        (void)attached.fd.release();
        client->p50_input_lease = attached.lease;
        client->p50_input_lease_state = Client::P50InputLeaseState::Active;
        client->p50_attachment_lease.reset();
        client->job = job;
        client->p50_source_compile_pending = true;
        if (client->command_line.empty()) {
            client->command_line = command_line_from_compile_job(job);
        }
        client->last_known_job_id = job->jobID();
        trace() << "P50 CompileFile attached exact "
                << (arm.cache_profile == CACHE_PROFILE_P29V1
                               ? "P29V1"
                        : (arm.cache_profile == CACHE_PROFILE_ZSTD_ROUTE
                        ? "ZSTD_ROUTE"
                        : "ZSTD_TU"))
                << " input for job "
                << job->jobID() << endl;
        // The exact input attachment is the admission boundary, not the
        // terminal state.  Queue the now-complete P50 CompileFile through the
        // ordinary bounded compiler scheduler; leaving this owner in
        // WAITP50INPUT strands it forever while the client waits for a result.
        client->set_status(Client::TOCOMPILE,
                           "P50 CompileFile attached: queued for compile");
        return true;
    }

    if (client->status != Client::CLIENTWORK
            && !authorize_assignment_claim(
                *job, client->client_id,
                client->channel != nullptr ? client->channel->protocol : 0)) {
        /* Authorization is resolved before the job is attached to a client,
           queued, touches an environment, or can start a compiler. */
        trace() << "rejecting unprepared/revoked assignment claim "
                << job->jobID() << endl;
        delete job;
        client->channel->send_msg(EndMsg());
        handle_end(client, 145);
        return false;
    }
    if (job->usesP50Input()) {
        const CompileInputIdentity &input = job->compileInputIdentity();
        const bool exact_assignment_input =
            job->hasAssignmentIdentity() &&
            input.attempt_id == job->assignmentNonce() &&
            input.request_id == job->assignmentNonce();
        if (!exact_assignment_input || client->p50_input_fd >= 0 ||
            client->p50_input_lease.has_value() ||
            client->p50_input_lease_state !=
                Client::P50InputLeaseState::None) {
            // CompileInputIdentity::validPresent() proves only syntactic
            // presence.  Compiler admission additionally binds its attempt
            // and replay request to this exact scheduler assignment.  Refuse
            // an impossible second CompileFile before it can reserve another
            // sidecar owner.
            log_warning() << "P50 compiler input owner mismatch/reuse for job "
                          << job->jobID() << endl;
            if (client->status != Client::CLIENTWORK)
                finish_assignment_claim(job->jobID());
            delete job;
            (void)client->channel->send_msg(EndMsg());
            handle_end(client, 146);
            return false;
        }
        // The compiler-attempt reducer has not yet joined the outer
        // InputRecord attachment/result owner.  Do not invoke the historical
        // whole-operation InputFdAttachmentClient here: that would perform a
        // hidden connect/receive/SCM_RIGHTS exchange from iceccd and could
        // bind A bytes after the lifecycle has begun withdrawing them.
        // Leave the exact source assignment unconsumed and fail closed until
        // the future reducer supplies its original deadline and routes the
        // typed outer operation through DaemonSidecarAdapter.
        log_warning() << "P50 compiler input attachment unavailable until outer reducer is installed for job "
                      << job->jobID() << endl;
        if (client->status != Client::CLIENTWORK)
            finish_assignment_claim(job->jobID());
        delete job;
        (void)client->channel->send_msg(EndMsg());
        handle_end(client, 146);
        return false;
    } else if (client->p50_input_fd >= 0 ||
               client->p50_input_lease.has_value() ||
               client->p50_input_lease_state !=
                   Client::P50InputLeaseState::None) {
        // Canonical legacy selection can never inherit a prior P50 cursor.
        if (client->status != Client::CLIENTWORK)
            finish_assignment_claim(job->jobID());
        delete job;
        (void)client->channel->send_msg(EndMsg());
        handle_end(client, 146);
        return false;
    }

    client->job = job;
    if (!job->usesP50Input() &&
        client->channel->protocol >= PROTOCOL_VERSION_ASSIGNMENT_IDENTITY) {
        trace() << "legacy CompileFile admitted canonical input for job "
                << job->jobID() << " epoch " << job->assignmentEpoch()
                << " nonce " << job->assignmentNonce() << " c_guid "
                << job->cGuid() << " tu_seq " << job->tuSeq() << endl;
        const P50LegacyWireIdentity identity{
            job->jobID(), job->assignmentEpoch(), job->assignmentNonce(),
            job->cGuid(), job->tuSeq()};
        if (!client->channel->set_p50_legacy_wire_identity(identity)) {
            log_warning() << "legacy wire identity could not be bound for job "
                          << job->jobID() << endl;
            client->job = nullptr;
            delete job;
            (void)client->channel->send_msg(EndMsg());
            handle_end(client, 146);
            return false;
        }
    }
    if (client->command_line.empty()) {
        client->command_line = command_line_from_compile_job(job);
    }
    client->last_known_job_id = job->jobID();

    if (client->status == Client::CLIENTWORK) {
        assert(job->environmentVersion() == "__client");

        /* G4 (20:13:59/20:32:36): announce JobBegin to S only for a GetCS request
           owned by the current active session.  An unpublished schedulerless
           fallback compiles locally but emits no scheduler frame. */
        if (scheduler_owns_getcs_assignment(client)) {
            if (!send_scheduler(JobBeginMsg(job->jobID(), clients.size()))) {
                trace() << "can't reach scheduler to tell him about compile file job "
                        << job->jobID() << endl;
                return false;
            }
        }

        // no scheduler is not an error case!
    } else {
        client->set_status(Client::TOCOMPILE, "handle_compile_file: queued for local compile");
    }

    return true;
}

void Daemon::settle_p50_input(
    Client *client, icecc::p50::InputLifecycleAction action,
    const char *reason) noexcept
{
    if (client == nullptr ||
        client->p50_input_lease_state !=
            Client::P50InputLeaseState::Active)
        return;

    const bool terminal =
        action == icecc::p50::InputLifecycleAction::CloseAcceptedJob ||
        action == icecc::p50::InputLifecycleAction::CancelJob;
    client->p50_input_lease_state = terminal
        ? Client::P50InputLeaseState::TerminalSettled
        : Client::P50InputLeaseState::AttemptSettled;

    if (!client->p50_input_lease.has_value()) {
        log_error() << "P50 input lease state lost before settlement for client "
                    << client->client_id << endl;
        return;
    }

    const icecc::p50::InputFdRequest lease =
        *client->p50_input_lease;
    client->p50_input_lease.reset();
    if (cache_adapter == nullptr) {
        // Adapter shutdown destroys its sidecar/store incarnation.  There is
        // no successor store to which this old observation may be rebound.
        log_info() << "P50 input settlement observed after sidecar shutdown for job "
                   << lease.owner.logical_job << " ("
                   << (reason ? reason : "unspecified") << ")" << endl;
        return;
    }

    const icecc::p50::InputLifecycleResult result =
        cache_adapter->apply_input_lifecycle(lease, action);
    if (result.status == icecc::p50::InputLifecycleStatus::Disconnected) {
        trace() << "P50 input settlement queued for job "
                << lease.owner.logical_job << " action "
                << static_cast<unsigned int>(action) << " reason "
                << (reason ? reason : "unspecified") << endl;
        return;
    }
    complete_p50_input_lifecycle(result, reason);
}

void Daemon::complete_p50_input_lifecycle(
    const icecc::p50::InputLifecycleResult& result,
    const char *reason) noexcept
{
    const icecc::p50::InputLifecycleRequest& request = result.request;
    const icecc::p50::InputFdRequest lease{
        request.identity, request.key, request.owner,
        request.owner.assignment_nonce};
    trace() << "P50 input settlement job " << request.owner.logical_job
            << " action " << static_cast<unsigned int>(request.action)
            << " status "
            << icecc::p50::input_lifecycle_status_name(result.status)
            << " reason " << (reason ? reason : "unspecified") << endl;

    /* The product harness retries the consumed lease after each lifecycle
       action.  Terminal close/cancel removes the record; attempt-only
       cancellation revokes this owner while preserving the bytes for a new
       assignment. */
    const char *required = getenv("ICECC_P50_C1F1_REQUIRED");
    const char *probe = getenv("ICECC_P50_TEST_POST_TERMINAL_ATTACH");
    if ((result.status == icecc::p50::InputLifecycleStatus::Applied ||
         result.status == icecc::p50::InputLifecycleStatus::AlreadyApplied) &&
        required != nullptr && string(required) == "1" &&
        probe != nullptr && string(probe) == "1") {
        icecc::p50::InputFdAttachmentResult attachment =
            cache_adapter->attach_input(
                lease.key, lease.owner, lease.request_id);
        trace() << "P50 terminal test post-settlement attach job "
                << lease.owner.logical_job << " action "
                << static_cast<unsigned int>(request.action) << " status "
                << icecc::p50::input_fd_attachment_status_name(
                       attachment.status)
                << " fd " << (attachment.fd.valid() ? "valid" : "invalid")
                << endl;
        if (attachment.status ==
                icecc::p50::InputFdAttachmentStatus::Accepted ||
            attachment.fd.valid()) {
            log_error() << "P50 terminal test unexpectedly reattached consumed lease for job "
                        << lease.owner.logical_job << endl;
        }
    }

    switch (result.status) {
    case icecc::p50::InputLifecycleStatus::Applied:
    case icecc::p50::InputLifecycleStatus::AlreadyApplied:
    case icecc::p50::InputLifecycleStatus::StoreReplaced:
    case icecc::p50::InputLifecycleStatus::UnknownRecord:
    case icecc::p50::InputLifecycleStatus::Timeout:
    case icecc::p50::InputLifecycleStatus::Disconnected:
    case icecc::p50::InputLifecycleStatus::HandshakeFailed:
        // The adapter retains incomplete operations for bounded retry.
        break;
    default:
        log_warning() << "P50 input settlement rejected for job "
                      << lease.owner.logical_job << " ("
                      << icecc::p50::input_lifecycle_status_name(result.status)
                      << ")" << endl;
        break;
    }
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
    trace() << "handle_end " << client->client_id << " " << client->channel->name
            << " exitcode=" << exitcode
            << " status=" << Client::status_str(client->status)
            << " fd=" << client->channel->fd
            << " detached=" << (client->p50_cache_session_detached ? 1 : 0)
            << endl;
#ifdef ICECC_DEBUG
    trace() << "handle_end " << client->dump() << endl;
    trace() << dump_internals() << endl;
#endif
    /* A source-arm owner exists before CompileFile creates a CompileJob and
       has no retained UseCS on the worker-side wrapper.  Snapshot its fenced
       scheduler identity before the teardown below clears the arm.  The
       pre-compile JobDone intentionally keeps C_GUID/TU_SEQ absent: F has not
       received the scheduler's compile identity at this boundary. */
    const std::optional<P50SourceArmFields> precompile_source_identity =
        client->status == Client::WAITP50INPUT &&
                client->p50_source_arm_fields.has_value()
            ? client->p50_source_arm_fields
            : std::nullopt;
    if (client->pending_p51_source_arm) {
        auto& pending = client->pending_p51_source_arm;
        const auto stage = static_cast<uint8_t>(pending->stage);
        if (stage >= static_cast<uint8_t>(
                         Client::PendingP51SourceArm::Stage::ReservationSend) &&
            p51_source_operation_capacity_available(
                orphaned_p51_source_arms.size())) {
            try {
                orphaned_p51_source_arms.push_back(std::move(pending));
            } catch (...) {
                log_warning() << "cannot retain bounded in-flight P51 ARM cleanup"
                              << endl;
                if (pending && pending->armed.has_value() &&
                    pending->reservation_error_code == 0)
                    queue_p51_source_cancel(
                        pending->arm, *pending->armed,
                        pending->absolute_deadline, pending->ready_lease);
                else if (pending &&
                         pending->reservation_error_code == 0)
                    withdraw_p51_source_incarnation(
                        pending->ready_lease,
                        "orphaned ARM cleanup retention failure");
                pending.reset();
            }
        } else if (stage >= static_cast<uint8_t>(
                                Client::PendingP51SourceArm::Stage::ReservationSend)) {
            if (pending->armed.has_value() &&
                pending->reservation_error_code == 0)
                queue_p51_source_cancel(pending->arm, *pending->armed,
                                        pending->absolute_deadline,
                                        pending->ready_lease);
            else
                withdraw_p51_source_incarnation(
                    pending->ready_lease, "orphaned ARM cleanup saturation");
        } else if (pending->armed.has_value() &&
                   pending->reservation_error_code == 0) {
            queue_p51_source_cancel(pending->arm, *pending->armed,
                                    pending->absolute_deadline,
                                    pending->ready_lease);
        }
    } else if (client->status == Client::WAITP50INPUT &&
        client->p51_source_arm_fields.has_value() &&
        client->p51_source_armed_fields.has_value() &&
        client->p51_source_absolute_deadline.has_value() &&
        client->p50_source_f_lease.has_value()) {
        queue_p51_source_cancel(
            *client->p51_source_arm_fields,
            *client->p51_source_armed_fields,
            *client->p51_source_absolute_deadline,
            *client->p50_source_f_lease);
    }
    // A normal disconnect, worker failure, scheduler loss, or retry says only
    // that this assignment attempt is over.  It must not terminally close the
    // logical job; an explicit result disposition does that before handle_end.
    // Attachment reserves the owner before ACK and before the validated FD
    // becomes Active. Close its transport first, then revoke that exact
    // pending incarnation even when completion never published an active lease.
    client->p50_attachment.reset();
    client->pending_p51_source_arm.reset();
    if (client->p50_attachment_lease.has_value()) {
        const auto lease = *client->p50_attachment_lease;
        client->p50_attachment_lease.reset();
        if (cache_adapter != nullptr) {
            const auto result = cache_adapter->apply_input_lifecycle(
                lease, icecc::p50::InputLifecycleAction::CancelAttempt);
            if (result.status != icecc::p50::InputLifecycleStatus::Disconnected)
                complete_p50_input_lifecycle(result, "pending attachment teardown");
        }
    }
    settle_p50_input(client, icecc::p50::InputLifecycleAction::CancelAttempt,
                     "handle_end");
    remember_finished_job(client, exitcode);
    if (client->job && (client->status == Client::TOCOMPILE
                        || client->status == Client::WAITP50INPUT
                        || client->status == Client::WAITFORCHILD)) {
        finish_assignment_claim(client->job->jobID());
    } else if (client->p50_source_arm_fields.has_value() &&
               (client->job_id != 0 ||
                client->p50_source_arm_fields->wire_job_id != 0)) {
        // Source-arm ownership precedes CompileFile, so the exact scheduler
        // wire id may be carried by Client rather than a CompileJob.  Close
        // that claim before deleting the ordinary wrapper.
        finish_assignment_claim(client->job_id != 0
                                    ? client->job_id
                                    : client->p50_source_arm_fields->wire_job_id);
    }
    if (client->status == Client::WAITP50INPUT) {
        client->p50_input_wait.close();
    }
    client->p50_source_arm_fields.reset();
    client->p51_source_arm_fields.reset();
    client->p51_source_armed_fields.reset();
    client->p51_source_absolute_deadline.reset();
    client->p50_attachment.reset();
    client->p50_attachment_job.reset();
    client->p50_attachment_result.reset();
    client->p50_source_f_lease.reset();
    client->p50_source_armed_ack.reset();
    client->p50_source_f_store_generation = 0;
    client->p50_source_arm_provenance = ConnectionProvenance{};
    client->p50_source_deadline_msec = 0;
    client->p50_source_compile_pending = false;
    client->p50_cache_session_detached = false;
    // Remove the value lease before erasing/deleting its Client and channel.
    // Delayed callbacks therefore cannot be rescued by fd or allocator
    // address reuse; repeated teardown is intentionally harmless.
    if (client->connection_provenance.lease.valid()) {
        connection_leases.cancel(client->connection_provenance.lease);
        client->connection_provenance = ConnectionProvenance{};
    }
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
        if (client->getcs_expected > 1) {
            /* G4 BATCH teardown (local-oracle 21:19): settle each recorded
               decision by its EXACT job id, plus one client-id cancellation for
               the undelivered tail (delivered < expected).  Only for an
               assignment owned by the current active generation (an unpublished/
               superseded batch emits nothing).  job_id is then cleared so the
               scalar settlement below is a no-op for this client. */
            if (scheduler_owns_getcs_assignment(client)) {
                for (uint32_t jid : client->getcs_batch_jobids) {
                    send_scheduler(JobDoneMsg(jid, exitcode, JobDoneMsg::FROM_SUBMITTER, clients.size()));
                }
                if (client->getcs_delivered < client->getcs_expected) {
                    JobDoneMsg cancel(0, exitcode, JobDoneMsg::FROM_SUBMITTER, clients.size());
                    cancel.set_unknown_job_client_id(client->client_id);
                    send_scheduler(cancel);
                }
            }
            client->job_id = 0;
            client->last_known_job_id = 0;
            client->getcs_batch_jobids.clear();
        }
        int job_id = client->job_id;
        bool use_client_id = false;

        if (client->status == Client::TOCOMPILE ||
            client->status == Client::WAITP50INPUT) {
            // A source arm owns the exact wire id before CompileFile creates a
            // CompileJob.  Never dereference a missing job during HUP,
            // deadline, or sidecar-replacement teardown.
            job_id = client->job ? client->job->jobID() : client->job_id;
        }

        if (client->status == Client::WAITFORCS && client->getcs_published) {
            // Published request: S accepted a GetCS for this client but has not
            // replied with a job id yet, so settle by client_id -- the scheduler
            // matches it.  A PRIVATE held request (getcs_published false: deferred
            // during LOGIN_ATTEMPT, never sent) is NOT settled here -- S has no
            // knowledge of it, so we publish no JobDone; the client is simply
            // destroyed with its held request (G4 17:20#1).
            use_client_id = true;
            assert( client->client_id > 0 );
        }

        if (job_id == 0 && !use_client_id && client->last_known_job_id > 0) {
            /* A settlement for this client's job was already issued (the
               id is zeroed at issue time): reject the duplicate instead of
               re-settling.  */
            ++duplicate_settlements_rejected;
        }
        if (job_id > 0 || use_client_id) {
            JobDoneMsg::from_type flag = JobDoneMsg::FROM_SUBMITTER;

            switch (client->status) {
            case Client::TOCOMPILE:
            case Client::WAITP50INPUT:
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
            case Client::FORWARDING_USE_CS:
                flag = JobDoneMsg::FROM_SUBMITTER;
                break;
            }

            trace() << "scheduler->send_msg( JobDoneMsg( " << client->dump() << ", " << exitcode << "))\n";

            /* A submitter-side remote wrapper never owns a CompileJob: its
               exact assignment lives in the retained UseCS copied by
               scheduler_use_cs().  If the client disconnects before sending
               its own JobDone, settling from client->job alone therefore
               emitted an all-zero identity.  Protocol-50 schedulers correctly
               reject that terminal frame and close the daemon connection.

               Use the retained frame only when it names this exact job and is
               still wire-valid.  The job object remains authoritative on the
               worker/local paths, while stale batch or replacement UseCS
               objects cannot redirect teardown because their job id differs. */
            const UseCSMsg *const retained_usecs =
                client->job == nullptr && job_id > 0 &&
                        client->usecsmsg != nullptr &&
                        client->usecsmsg->job_id ==
                            static_cast<uint32_t>(job_id) &&
                        client->usecsmsg->valid_payload()
                    ? client->usecsmsg
                    : nullptr;
            const uint64_t c_guid = client->job
                ? client->job->cGuid()
                : retained_usecs ? retained_usecs->cGuid() : 0;
            const uint64_t tu_seq = client->job
                ? client->job->tuSeq()
                : retained_usecs ? retained_usecs->tuSeq() : 0;
            const uint64_t assignment_epoch = client->job
                ? client->job->assignmentEpoch()
                : retained_usecs ? retained_usecs->assignmentEpoch()
                : precompile_source_identity
                    ? precompile_source_identity->assignment_epoch : 0;
            const uint64_t assignment_nonce = client->job
                ? client->job->assignmentNonce()
                : retained_usecs ? retained_usecs->assignmentNonce()
                : precompile_source_identity
                    ? precompile_source_identity->assignment_nonce : 0;
            JobDoneMsg msg(job_id, exitcode, flag, clients.size(),
                           assignment_epoch, assignment_nonce, c_guid, tu_seq);
            if( use_client_id ) {
                msg.set_unknown_job_client_id( client->client_id );
            }
            /* G4 (20:13:59/20:32:36): a FROM_SUBMITTER settlement is GetCS-derived
               -- emit it to S only for an assignment owned by the current active
               session (an unpublished schedulerless fallback or a superseded
               generation emits nothing).  A FROM_SERVER settlement (TOCOMPILE,
               worker-side) is governed by worker-assignment ownership, not this
               predicate, so it is left unchanged. */
            const bool emit_settlement = (flag == JobDoneMsg::FROM_SUBMITTER)
                                         ? scheduler_owns_getcs_assignment(client)
                                         : true;
            if (emit_settlement) {
                if (!send_scheduler(msg)) {
                    trace() << "failed to reach scheduler for remote job done msg!" << endl;
                }
            }
            /* The settlement for this job id has been issued exactly once;
               a repeated teardown path must not settle it again.  */
            client->job_id = 0;
        } else if (client->status == Client::CLIENTWORK) {
            // Clientwork && !job_id == LINK
            /* G4 (18:21): announce completion to S only if this local job was
               owned by the CURRENT active session.  An UNOWNED_LOCAL job (started
               during a LOGIN_ATTEMPT/offline) or one owned by a superseded
               generation emits no JobLocalDone -- S was never told it began. */
            if (scheduler_session_active
                    && client->local_owner_generation == scheduler_session_generation) {
                trace() << "scheduler->send_msg( JobLocalDoneMsg( " << client->client_id << ") );\n";
                if (!send_scheduler(JobLocalDoneMsg(client->client_id))) {
                    trace() << "failed to reach scheduler for local job done msg!" << endl;
                }
            }
        }
    }

    delete client;
}

void Daemon::clear_children()
{
    begin_session_quiescence();
}

bool Daemon::handle_get_cs(Client *client, Msg *msg)
{
    GetCSMsg *umsg = dynamic_cast<GetCSMsg *>(msg);
    assert(client);
    if (umsg->count == 0) {
        /* G4 (local-oracle 21:19): a zero-count GetCS is a no-op -- no UseCS
           reply, no scheduler frame, and NO request state (getcs_outstanding is
           left untouched), so the connection stays usable for a later ordinary
           request. */
        trace() << "handle_get_cs count=0 no-op for client " << client->client_id << endl;
        return true;
    }
    /* G4 (17:20#2 / bigoracle 18:45 P0): exactly one outstanding GetCS per
       client.  getcs_outstanding is set at accept and cleared only when the
       client is destroyed, so a second GetCS is rejected in EVERY non-terminal
       state -- not merely while WAITFORCS.  This closes the loophole where a
       request that had already advanced to PENDING_USE_CS/FORWARDING_USE_CS/
       WAITCOMPILE/CLIENTWORK (after UseCS/NoCS) could be silently overwritten,
       stranding the first scheduler job.  Reject rather than overwrite;
       handle_end() settles the outstanding request correctly.  Return FALSE so
       handle_activity()'s caller treats the client as gone (mirrors the END/
       default protocol-error cases); returning true would dereference a freed
       client. */
    if (client->getcs_outstanding) {
        log_error() << "client " << client->client_id
                    << " sent a second GetCS while one is outstanding; closing"
                    << endl;
        handle_end(client, 120);
        return false;
    }

    /* The local C daemon is the authority for cache capability and the C
       kill switch.  A new wrapper may only narrow that authority: canonical
       absence opts this job out (the bounded legacy retry), while a present
       request is intersected with the daemon-authorized profiles.  Thus an
       old/disabled wrapper cannot be upgraded by the daemon and a wrapper can
       never enable a profile the daemon disabled. */
    P50CacheClientCapability requested_cache_capability{};
    if (umsg->count == 1 && client->connection_provenance.cache_eligible()) {
        requested_cache_capability = p50_cache_client_capability_from_env(
            client->channel != nullptr ? client->channel->protocol : 0);
        if (umsg->cache_protocol != requested_cache_capability.protocol) {
            requested_cache_capability = {};
        } else {
            requested_cache_capability.profile_mask &= umsg->cache_profile_mask;
            if (requested_cache_capability.profile_mask == 0)
                requested_cache_capability = {};
        }
    }
    const P50CacheClientCapability scheduler_requested_capability =
        cache_capability_for_scheduler(requested_cache_capability);
    const bool sidecar_ready = cache_client_sidecar_ready();
    /* P50 strictness is intentionally not inferred from remote_required.
       ICECC_P50_C1F1_REQUIRED is a wrapper policy and its one fresh strict
       retry carries an explicit cache capability while remote_required stays
       zero.  During an already-supervised replacement, publishing that
       capability as canonical absence races the successor lease and produces
       Error 105.  Hold every one-job cache-capable request across this finite
       recovery; cache-absent/legacy requests remain immediately publishable. */
    P50CacheClientCapability cache_capability =
        sidecar_ready ? requested_cache_capability
                      : P50CacheClientCapability{};
    if (cache_capability.profile_mask != 0) {
        cache_capability.profile_mask &= ~cache_unavailable_profile_mask;
        if (cache_capability.profile_mask == 0)
            cache_capability = {};
    }
    const bool wait_for_cache_recovery =
        icecc::p50::daemon::should_defer_cache_capable_getcs(
            umsg->count, scheduler_requested_capability.profile_mask,
            sidecar_ready, cache_sidecar_recovery_in_progress());
    client->cache_offer = cache_capability;
    client->cache_offer_generation = 0;
    client->cache_offer_lease.reset();
    if (cache_capability.profile_mask != 0 &&
        cache_route_state_lease.has_value()) {
        client->cache_offer_lease = *cache_route_state_lease;
    }
    if (!umsg->command_summary.empty()) {
        client->command_line = umsg->command_summary;
    } else if (client->command_line.empty() && client->channel) {
        client->command_line = command_line_from_peer_socket(client->channel->fd);
    }
    client->niceness = umsg->niceness;
    client->last_waitforcs_msec = 0;
    client->set_status(Client::WAITFORCS, scheduler ? "handle_get_cs: sent GetCS to scheduler" : "handle_get_cs: scheduler missing");
    umsg->client_id = client->client_id;
    client->getcs_published = false;    /* G4 (17:20#1): unpublished until an active-session send succeeds */
    client->getcs_outstanding = true;   /* G4 (18:45 P0): request now occupies the client until destruction */
    client->getcs_generation = 0;
    client->getcs_expected = umsg->count;   /* G4 (local-oracle 21:19): >1 -> BATCH_LEDGER mode */
    client->getcs_delivered = 0;
    client->getcs_batch_jobids.clear();
    trace() << "handle_get_cs " << umsg->client_id << endl;

    if (wait_for_cache_recovery) {
        /* Keep the wrapper's original narrowing request private while the
           already-supervised sidecar replacement is making finite lifecycle
           progress.  handle_old_request revalidates it against the successor
           READY lease before publication.  A terminal recovery state falls
           through there as canonical absence; this is never an unbounded or
           authority-widening wait. */
        client->deferred_getcs = new GetCSMsg(*umsg);
        client->deferred_getcs_waits_for_cache = true;
        client->set_status(
            Client::WAITFORCS,
            "handle_get_cs: holding P50 cache-capable request for cache recovery");
        return true;
    }

    if (scheduler && !scheduler_session_active) {
        /* G4 LOGIN_ATTEMPT: the channel is up but the session is not committed
           (no ConfCS yet).  Hold this request -- do NOT forward it on the
           pending channel, and do NOT convert it to local work merely because
           activation is pending.  It stays PRIVATE (getcs_published false), so a
           disconnect before ConfCS publishes no JobDone.  handle_old_request
           re-drives it once the session becomes active.  (deferred_getcs is
           guaranteed null here: a client that still had one was rejected by the
           single-outstanding check above.) */
        client->deferred_getcs = new GetCSMsg(*umsg);
        client->deferred_getcs_waits_for_cache = false;
        client->set_status(Client::WAITFORCS, "handle_get_cs: holding for session activation");
        return true;
    }

    if (!scheduler) {
        if (umsg->remote_required == 1) {
            client->deferred_getcs = new GetCSMsg(*umsg);
            client->deferred_getcs_waits_for_cache = false;
            client->set_status(
                Client::WAITFORCS,
                "handle_get_cs: holding remote-required request for scheduler");
            return true;
        }
        /* No scheduler -> resolve locally, preserving reply cardinality
           (G4 20:13:59): count<=1 gets the one synthetic local UseCS; count>1
           (which one local UseCS cannot satisfy) closes only this client so its
           own local fallback runs.  Neither emits a scheduler frame. */
        if (umsg->count > 1) {
            log_warning() << "client " << client->client_id
                          << " GetCS count>1 with no scheduler; closing for local fallback"
                          << endl;
            handle_end(client, 111);
            return false;
        }
        /* S2: scheduler missing entirely, no worker snapshot -- canonical
           cache-absent (see scheduler_no_cs).
           install_cache_absent_local_decision (BigOracle exact blueprint)
           atomically clears any handoff retained from an earlier dispatch
           on this same (reused) Client, deletes any prior usecsmsg, and
           installs the replacement.  See
           test_poison_cache_handoff_if_armed's own comment for why the
           poison/record pair below brackets this real, unmodified call. */
        const bool cache_handoff_test_poisoned_no_scheduler =
            test_poison_cache_handoff_if_armed(client, "get_cs_no_scheduler");
        install_cache_absent_local_decision(
            *client,
            std::unique_ptr<UseCSMsg>(new UseCSMsg(umsg->target, "127.0.0.1",
                                                    daemon_port, umsg->client_id,
                                                    true, 1, 0)),
            "handle_get_cs: scheduler missing, local compile");
        if (cache_handoff_test_poisoned_no_scheduler) {
            test_record_cache_handoff_clear(client, "get_cs_no_scheduler");
        }
        client->job_id = umsg->client_id;
        client->last_known_job_id = umsg->client_id;
        return true;
    }

    cache_capability = project_getcs_cache_route(umsg, cache_capability);
    client->cache_offer = cache_capability;
    if (cache_capability.profile_mask == 0)
        client->cache_offer_lease.reset();

    umsg->client_count = clients.size();
    umsg->command_summary.clear();

    if (!send_scheduler(*umsg)) {
        return false;
    }
    /* G4 (17:20#1 / 18:45 P0): the GetCS reached S under the current ACTIVE
       generation, which now owns this request by client id.  A teardown before
       UseCS must settle it with a JobDone; and only a reply carrying this
       generation may later authorize the client (see scheduler_use_cs). */
    client->getcs_published = true;
    client->getcs_generation = scheduler_session_generation;
    client->cache_offer_generation = scheduler_session_generation;
    return true;
}

int Daemon::handle_assign_prepare(AssignPrepareMsg *msg)
{
    if (!scheduler_session_active
            || !assignment_mode_prepares(assignment_fence_mode)
            || assignment_table_exhausted
            || msg->epoch() == 0 || msg->wire_id == 0 || msg->nonce() == 0
            || msg->flags != 0 || msg->epoch() != assignment_scheduler_epoch) {
        ++assignment_stale_controls;
        return 1;
    }
    const AssignmentKey key { msg->epoch(), msg->wire_id, msg->nonce() };

    auto terminal = assignment_terminals.find(key);
    if (terminal != assignment_terminals.end()) {
        ++assignment_revoke_results;
        return send_scheduler(RevokeResultMsg(
            key.epoch, key.wire_id, key.nonce, terminal->second.result)) ? 0 : 1;
    }

    auto live = live_assignments.find(key.wire_id);
    if (live != live_assignments.end()) {
        if (live->second.key == key) {
            /* Exact duplicate: preserve its current phase and re-READY. */
        } else if (assignment_fence_mode == ConfCSMsg::Advisory
                && live->second.key.epoch == key.epoch
                && live->second.key.wire_id == key.wire_id
                && live->second.key.nonce == 0
                && live->second.phase != WorkerAssignment::Orphaned) {
            /* Claim won the cross-channel race.  Bind PREPARE's full identity
               into that same owner record; never regress to Reserved. */
            live->second.key = key;
            ++assignment_prepares;
            if (live->second.phase == WorkerAssignment::ClaimedOrLater) {
                if (!retain_assignment_terminal(
                        key, RevokeResultMsg::ClaimedOrLater, true)) {
                    live->second.phase = WorkerAssignment::Orphaned;
                    return 1;
                }
                live_assignments.erase(live);
            }
        } else {
            /* A wire id may be reused only after its earlier live record has
               reached a retained terminal.  Never replace live authority. */
            ++assignment_stale_controls;
            return 1;
        }
    } else {
        if (live_assignments.size() + assignment_terminals.size()
                >= assignment_terminal_limit()) {
            assignment_table_exhausted = true;
            return 1;
        }
        WorkerAssignment record;
        record.phase = WorkerAssignment::Reserved;
        record.key = key;
        record.claimant = 0;
        live_assignments.emplace(key.wire_id, record);
        auto closed = closed_wire_ids.find(key.wire_id);
        if (closed != closed_wire_ids.end() && !(closed->second == key)) {
            closed_wire_ids.erase(closed);
        }
        ++assignment_prepares;
    }

    /* In strict mode the selected F's cache advertisement is part of the
       assignment projection.  PREPARE can arrive during sidecar startup;
       announcing READY in that window makes S emit a valid identity-bearing
       UseCS with a permanently absent cache tail before the real READY
       advertisement exists.  Retain the bounded Reserved row and announce
       it from poll_cache_adapter once the sidecar reaches READY. */
    const bool wait_for_cache_ready =
        assignment_fence_mode == ConfCSMsg::StrictNonce &&
        cache_adapter != nullptr &&
        (cache_adapter->state() == icecc::p50::daemon::AdapterState::Starting ||
         (cache_adapter->state() == icecc::p50::daemon::AdapterState::Absent &&
          cache_adapter->outer_lifecycle_state() ==
              icecc::p50::sidecar::LifecycleState::Ready) ||
         cache_adapter->outer_launch_plan_active());
    if (wait_for_cache_ready)
        return 0;

    ++assignment_ready_replies;
    const bool sent = send_scheduler(AssignReadyMsg(key.epoch, key.wire_id,
                                                     key.nonce));
    if (sent) {
        auto ready = live_assignments.find(key.wire_id);
        if (ready != live_assignments.end() && ready->second.key == key)
            ready->second.ready_announced = true;
    }
    return sent ? 0 : 1;
}

int Daemon::handle_revoke_before_start(RevokeBeforeStartMsg *msg)
{
    if (!scheduler_session_active
            || !assignment_mode_prepares(assignment_fence_mode)
            || assignment_table_exhausted
            || msg->epoch() == 0 || msg->wire_id == 0 || msg->nonce() == 0
            || msg->epoch() != assignment_scheduler_epoch) {
        ++assignment_stale_controls;
        return 1;
    }
    const AssignmentKey key { msg->epoch(), msg->wire_id, msg->nonce() };

    auto terminal = assignment_terminals.find(key);
    if (terminal != assignment_terminals.end()) {
        ++assignment_revoke_results;
        return send_scheduler(RevokeResultMsg(
            key.epoch, key.wire_id, key.nonce, terminal->second.result)) ? 0 : 1;
    }

    RevokeResultMsg::Result result = RevokeResultMsg::Revoked;
    bool blocks_compat_claim = false;
    bool erase_live = false;
    auto live = live_assignments.find(key.wire_id);
    if (live != live_assignments.end() && live->second.key == key) {
        if (live->second.phase == WorkerAssignment::Reserved) {
            erase_live = true;
            blocks_compat_claim = true;
        } else {
            result = RevokeResultMsg::ClaimedOrLater;
            blocks_compat_claim = true;
        }
    } else if (live == live_assignments.end()) {
        /* REVOKE may win before a delayed PREPARE is consumed. */
        blocks_compat_claim = true;
    } else {
        /* A delayed old full-triple revoke after wire-id reuse receives its
           own deterministic outcome but cannot touch the new live record. */
        ++assignment_stale_controls;
    }

    if (!retain_assignment_terminal(key, result, blocks_compat_claim)) {
        return 1;
    }
    if (erase_live) {
        live_assignments.erase(live);
    }
    ++assignment_revoke_results;
    return send_scheduler(RevokeResultMsg(
        key.epoch, key.wire_id, key.nonce, result)) ? 0 : 1;
}

int Daemon::handle_cs_conf(ConfCSMsg *msg)
{
    max_scheduler_pong = msg->max_scheduler_pong;
    max_scheduler_ping = msg->max_scheduler_ping;
    /* G4 activation point: the first ConfCS for the pending Login commits the
       session generation and marks it ACTIVE -- exactly once.  A duplicate or
       unsolicited ConfCS (no pending Login, or already active) is ignored and
       creates no second generation. */
    if (scheduler_login_pending && !scheduler_session_active) {
        if (scheduler_session_generation == ~(uint64_t)0) {
            /* Generation space exhausted: refuse activation rather than wrap to
               a reused generation, and make exhaustion TERMINAL so reconnect()
               stops re-attempting the same impossible activation forever
               (17:20#3). */
            log_error() << "scheduler session generation exhausted; refusing activation" << endl;
            scheduler_generation_exhausted = true;
        } else {
            if (IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_FENCE, scheduler)) {
                if (msg->epoch() == 0
                        || (msg->fence_mode != ConfCSMsg::Legacy
                            && msg->fence_mode != ConfCSMsg::Advisory
                            && msg->fence_mode != ConfCSMsg::EnforcingCompat
                            && msg->fence_mode != ConfCSMsg::StrictNonce)) {
                    log_error() << "invalid protocol-49 assignment configuration" << endl;
                    return 1;
                }
                const ConfCSMsg::FenceMode requested =
                    static_cast<ConfCSMsg::FenceMode>(msg->fence_mode);
                if (requested == ConfCSMsg::StrictNonce
                        && !IS_PROTOCOL_VERSION(
                            PROTOCOL_VERSION_ASSIGNMENT_IDENTITY, scheduler)) {
                    log_error() << "strict-nonce requires protocol 50 nonce-bearing client claims" << endl;
                    return 1;
                }
                if (assignment_epoch_history_exhausted
                        || retired_assignment_epochs.find(msg->epoch())
                            != retired_assignment_epochs.end()) {
                    log_error() << "refusing retired protocol-49 scheduler epoch" << endl;
                    return 1;
                }
                if (assignment_scheduler_epoch != 0
                        && assignment_scheduler_epoch == msg->epoch()
                        && (assignment_fence_mode != requested
                            || assignment_table_exhausted)) {
                    log_error() << "refusing changed or exhausted protocol-49 epoch configuration" << endl;
                    return 1;
                }
                if (assignment_scheduler_epoch != msg->epoch()) {
                    if (!replace_assignment_epoch()) {
                        log_error() << "protocol-49 epoch history exhausted" << endl;
                        return 1;
                    }
                    assignment_scheduler_epoch = msg->epoch();
                    assignment_fence_mode = requested;
                }
            } else {
                if (!replace_assignment_epoch()) {
                    log_error() << "protocol-49 epoch history exhausted" << endl;
                    return 1;
                }
            }
            ++scheduler_session_generation;
            scheduler_session_active = true;
            scheduler_login_pending = false;
            scheduler_login_deadline_msec = 0;
        }
    }
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

bool Daemon::handle_p50_cache_session_fd_request(
    Client *client, P50CacheSessionFdRequestMsg *msg,
    P50CacheFdReplyTicket reply_ticket)
{
    auto refuse = [&](const char *reason) {
        log_warning() << "P50 C-cache control request refused: " << reason << endl;
        if (client != nullptr && client->channel != nullptr)
            (void)client->channel->send_msg(EndMsg());
        if (client != nullptr)
            handle_end(client, 151);
        return false;
    };

    if (client == nullptr || client->channel == nullptr || msg == nullptr ||
        !msg->valid_payload())
        return refuse("invalid request");

    const P50CacheSessionFdRequestFields& request = msg->request;
    const ConnectionProvenance& provenance = client->connection_provenance;
    const UseCSMsg *const assignment = client->usecsmsg;
    Client::CacheHandoff& handoff = client->cacheHandoff;
    const bool assignment_owner_status =
        client->status == Client::WAITCOMPILE ||
        client->status == Client::CLIENTWORK;
    if (!assignment_owner_status || !provenance.cache_eligible() ||
        !connection_leases.revalidate(provenance.lease, client, client->channel,
                                      provenance.peer).has_value())
        return refuse("wrapper connection is not the live local assignment owner");
    if (assignment == nullptr || !assignment->valid_payload() ||
        !handoff.valid || request.wire_job_id != assignment->job_id ||
        request.assignment_epoch != assignment->assignmentEpoch() ||
        request.assignment_nonce != assignment->assignmentNonce() ||
        request.wire_job_id != handoff.wireJobId ||
        request.assignment_epoch != handoff.assignmentEpoch ||
        request.assignment_nonce != handoff.assignmentNonce ||
        assignment->cache_endpoint_port != handoff.cachePort ||
        assignment->cache_protocol != handoff.cacheProtocol ||
        assignment->cache_profile_mask != handoff.cacheProfileMask ||
        (request.profile & handoff.cacheProfileMask) != request.profile)
        return refuse("request does not match the retained UseCS handoff");

    if (!reply_ticket.valid()) {
        reply_ticket =
            client->channel->take_p50_cache_fd_reply_ticket(*msg);
    }
    if (!reply_ticket.valid())
        return refuse("cache-control reply authority is unavailable");

    const P50CacheClientCapability current_capability =
        p50_cache_client_capability_from_env(
            client->channel != nullptr ? client->channel->protocol : 0);
    const bool assignment_rebind_eligible =
        scheduler_owns_getcs_assignment(client) &&
        handoff.readyLease.has_value() && handoff.readyLease->valid() &&
        current_capability.protocol == handoff.cacheProtocol &&
        (request.profile & current_capability.profile_mask) == request.profile &&
        (request.profile & cache_unavailable_profile_mask) == 0;

    if ((cache_adapter == nullptr || !cache_adapter->authenticated() ||
         !cache_adapter->outer_current_ready_lease().has_value()) &&
        assignment_rebind_eligible && cache_sidecar_recovery_in_progress()) {
        if (client->deferred_p50_cache_fd_request.has_value())
            return refuse("duplicate cache-control request during supervised replacement");
        client->deferred_p50_cache_fd_request.emplace(
            Client::DeferredP50CacheFdRequest{
                request, std::move(reply_ticket)});
        trace() << "deferred P50 C-cache control request across supervised replacement for assignment "
                << request.wire_job_id << endl;
        return true;
    }

    if (cache_adapter == nullptr || !cache_adapter->authenticated())
        return refuse("supervised cache service is unavailable");
    const auto ready_lease = cache_adapter->outer_current_ready_lease();
    if (!ready_lease.has_value() || !ready_lease->valid())
        return refuse("supervised cache service has no current READY lease");
    if (!handoff.readyLease.has_value())
        return refuse("retained UseCS has no READY lease");
    if (!icecc::p50::daemon::p50_ready_lease_observation_equal(
            *handoff.readyLease, *ready_lease)) {
        if (!assignment_rebind_eligible)
            return refuse("retained UseCS belongs to another READY lease");
        /* No source operation began under the retired C lease: the wrapper is
           still blocked waiting for this descriptor.  Rebind only the C-local
           lease/generation of the exact live scheduler assignment.  Its F
           endpoint, assignment identity, profile, and retry budget are
           unchanged. */
        handoff.readyLease = *ready_lease;
        handoff.routeStateGeneration = cache_route_state_generation;
        trace() << "rebound retained P50 assignment " << request.wire_job_id
                << " to successor C-cache READY lease" << endl;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    icecc::p50::local::Status connect_status = icecc::p50::local::Status::Ok;
    icecc::p50::local::Connection control =
        icecc::p50::local::connect_unix_until(ready_lease->socket_path,
                                              deadline, &connect_status);
    if (!control.valid())
        return refuse("cannot connect to the current cache service");

    const icecc::p50::local::Identity identity{
        ready_lease->identity.generation, ready_lease->identity.attempt};
    icecc::p50::local::CredentialExpectation expected_peer;
    expected_peer.uid = ::geteuid();
    expected_peer.gid = ::getegid();
    if (control.verify_peer_credentials(expected_peer) !=
            icecc::p50::local::Status::Ok ||
        control.send_until(
            icecc::p50::local::make_hello(
                icecc::p50::local::PeerRole::Daemon, identity),
            deadline) != icecc::p50::local::Status::Ok)
        return refuse("cache-service HELLO failed");

    icecc::p50::local::Frame acknowledgement;
    if (control.receive_until(acknowledgement, deadline) !=
            icecc::p50::local::Status::Ok ||
        icecc::p50::local::validate_handshake(
            acknowledgement, icecc::p50::local::MessageType::HelloAck,
            icecc::p50::local::PeerRole::Sidecar, identity) !=
            icecc::p50::local::Status::Ok)
        return refuse("cache-service HELLO acknowledgement failed");

    const int transfer_fd = ::dup(control.native_handle());
    if (transfer_fd < 0)
        return refuse("cannot duplicate cache-service control descriptor");
    const int descriptor_flags = ::fcntl(transfer_fd, F_GETFD);
    if (descriptor_flags < 0 ||
        ::fcntl(transfer_fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        (void)::close(transfer_fd);
        return refuse("cannot mark cache-service control descriptor close-on-exec");
    }

    const P50CacheControlIdentity control_identity{
        identity.generation, identity.attempt,
        static_cast<uint64_t>(::geteuid()),
        static_cast<uint64_t>(::getegid())};
    if (!client->channel->send_p50_cache_fd_reply(
            std::move(reply_ticket), control_identity, transfer_fd, deadline))
        return refuse("cannot deliver cache-service control descriptor");

    trace() << "P50 C-cache control descriptor delivered for assignment "
            << request.wire_job_id << " profile " << request.profile << endl;
    return true;
}

bool Daemon::handle_p51_source_lease_request(
    Client *client, P51SourceLeaseRequestMsg *msg,
    P51CacheFdReplyTicket reply_ticket)
{
    auto refuse = [&](const char *reason) {
        log_warning() << "P51 C-cache source lease refused: " << reason << endl;
        if (client != nullptr && client->channel != nullptr)
            (void)client->channel->send_msg(EndMsg());
        if (client != nullptr)
            handle_end(client, 151);
        return false;
    };
    if (client == nullptr || client->channel == nullptr || msg == nullptr ||
        !msg->valid_payload() ||
        !protocol_supports_cache_r2(client->channel->protocol))
        return refuse("invalid request or protocol");

    const P51SourceLeaseRequestFields &request = msg->request;
    const ConnectionProvenance &provenance = client->connection_provenance;
    const UseCSMsg *const assignment = client->usecsmsg;
    Client::CacheHandoff &handoff = client->cacheHandoff;
    const bool assignment_owner_status =
        client->status == Client::WAITCOMPILE ||
        client->status == Client::CLIENTWORK;
    if (!assignment_owner_status || !provenance.cache_eligible() ||
        !connection_leases.revalidate(provenance.lease, client, client->channel,
                                      provenance.peer).has_value())
        return refuse("wrapper is not the live assignment owner");
    if (assignment == nullptr || !assignment->valid_payload() ||
        !handoff.valid || request.wire_job_id != assignment->job_id ||
        request.assignment_epoch != assignment->assignmentEpoch() ||
        request.assignment_nonce != assignment->assignmentNonce() ||
        request.wire_job_id != handoff.wireJobId ||
        request.assignment_epoch != handoff.assignmentEpoch ||
        request.assignment_nonce != handoff.assignmentNonce ||
        assignment->cache_endpoint_port != handoff.cachePort ||
        assignment->cache_protocol != request.requested_cache_revision ||
        handoff.cacheProtocol != request.requested_cache_revision ||
        assignment->cache_profile_mask != handoff.cacheProfileMask ||
        (request.profile & handoff.cacheProfileMask) != request.profile)
        return refuse("request does not match the retained R2 assignment");

    if (!reply_ticket.valid())
        reply_ticket = client->channel->take_p51_cache_fd_reply_ticket(*msg);
    if (!reply_ticket.valid())
        return refuse("source-lease reply authority is unavailable");
    if (cache_adapter == nullptr || !cache_adapter->authenticated())
        return refuse("supervised cache service is unavailable");
    const auto ready_lease = cache_adapter->outer_current_ready_lease();
    if (!ready_lease.has_value() || !ready_lease->valid())
        return refuse("supervised cache service has no current READY lease");
#ifdef ICECC_P50_ENDPOINT_TEST_HOOKS
    // The expiry regression withholds exactly one repeated lease reply while
    // leaving the daemon event loop live.  The wrapper must time out under its
    // original deadline; unrelated clients and the retained assignment remain
    // serviceable.  This is private-fixture-only and defaults off.
    ++client->test_p51_source_lease_requests;
    static bool test_withheld_retry_lease = false;
    const char *withhold_retry =
        ::getenv("ICECC_TEST_P51_WITHHOLD_RETRY_LEASE");
    const char *expiry_marker =
        ::getenv("ICECC_TEST_P51_CAPACITY_EXPIRY_MARKER");
    const bool expiry_gate_armed = expiry_marker == nullptr ||
        ::access(expiry_marker, F_OK) == 0;
    if (!test_withheld_retry_lease && withhold_retry != nullptr &&
        std::strcmp(withhold_retry, "1") == 0 &&
        expiry_gate_armed &&
        client->test_p51_source_lease_requests == 2) {
        test_withheld_retry_lease = true;
        std::fprintf(stderr,
            "P51_CAPACITY_TEST_WITHHELD_RETRY_LEASE job=%u epoch=%llu nonce=%llu profile=%u window=%u\n",
            request.wire_job_id,
            static_cast<unsigned long long>(request.assignment_epoch),
            static_cast<unsigned long long>(request.assignment_nonce),
            request.profile, request.requested_window);
        std::fflush(stderr);
        return true;
    }
#endif
    if (client->pending_p51_source_lease)
        return refuse("a source lease is already pending on this wrapper");
    size_t pending_count = 0;
    for (const auto &entry : clients)
        pending_count += entry.second->pending_p51_source_lease ? 1u : 0u;
    if (!p51_source_operation_capacity_available(pending_count))
        return refuse("bounded source-lease setup capacity is full");
    if (!handoff.readyLease.has_value() || !handoff.readyLease->valid())
        return refuse("assignment has no retained READY lease");
    if (!icecc::p50::daemon::p50_ready_lease_observation_equal(
            *handoff.readyLease, *ready_lease)) {
        const bool eligible_successor =
            scheduler_owns_getcs_assignment(client) &&
            assignment->cache_protocol == handoff.cacheProtocol &&
            (request.profile & handoff.cacheProfileMask) == request.profile &&
            (request.profile & cache_unavailable_profile_mask) == 0;
        if (!eligible_successor)
            return refuse("assignment READY lease changed before source setup");
        handoff.readyLease = *ready_lease;
        handoff.routeStateGeneration = cache_route_state_generation;
    }

    try {
        auto pending = std::make_unique<Client::PendingP51SourceLease>();
        pending->request = request;
        pending->reply_ticket = std::move(reply_ticket);
        pending->ready_lease = *ready_lease;
        pending->deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(1);
        pending->connect = std::make_unique<
            icecc::p50::local::UnixConnectOperation>(
                ready_lease->socket_path, pending->deadline);
        pending->connect->advance();
        client->pending_p51_source_lease = std::move(pending);
    } catch (...) {
        return refuse("cannot allocate bounded source-lease setup state");
    }
    return true;
}

bool Daemon::handle_p51_source_arm(Client *client, P51SourceArmMsg *msg)
{
    auto refuse = [&](const char *reason) {
        log_warning() << "P51 source arm refused: " << reason << endl;
        if (client != nullptr && client->channel != nullptr)
            (void)client->channel->send_msg(EndMsg());
        if (client != nullptr)
            handle_end(client, 153);
        return false;
    };
    if (client == nullptr || client->channel == nullptr || msg == nullptr ||
        !msg->valid_payload() ||
        !protocol_supports_cache_r2(client->channel->protocol))
        return refuse("invalid request or protocol");

    const P51SourceArmFields arm = msg->arm;
    const P50SourceArmFields &source = arm.source;
    const auto &provenance = client->connection_provenance;
    if (client->status != Client::UNKNOWN || client->p50_source_arm_fields ||
        client->p51_source_arm_fields || client->pending_p51_source_lease ||
        client->pending_p51_source_arm || client->job != nullptr ||
        !client->source_wrapper_provenance_valid() ||
        cache_adapter == nullptr || !cache_adapter->authenticated() ||
        !scheduler_session_active || !cache_advertisement_snapshot().present() ||
        !connection_leases.revalidate_live(
            provenance.lease, client, client->channel, provenance.peer))
        return refuse("daemon state or assignment owner is not eligible");

    const auto &current_lease = cache_adapter->outer_current_ready_lease();
    if (!current_lease.has_value() || !current_lease->valid())
        return refuse("sidecar has no READY lease");
    const auto snapshot = cache_advertisement_snapshot();
    if (source.selected_f_host != remote_name ||
        source.selected_f_ordinary_port != static_cast<uint32_t>(daemon_port) ||
        source.selected_f_cache_port != snapshot.endpoint_port ||
        source.cache_protocol != snapshot.protocol ||
        source.cache_protocol != CACHE_WIRE_REVISION_R2 ||
        !p50_source_profile_selection_valid(source.cache_profile) ||
        !p50_source_profile_mode_valid(source.cache_profile, source.source_mode) ||
        (snapshot.profile_mask & source.cache_profile) != source.cache_profile ||
        source.c_store_derivation_version != current_lease->store_derivation_version ||
        icecc::p50::store_identity_file_guid_matches_client(
            source.c_store_guid, current_lease->f_store_guid.bytes))
        return refuse("source request differs from the selected R2 endpoint");
    if (!authorize_source_arm_claim(
            source, static_cast<uint32_t>(client->client_id)))
        return refuse("assignment claim was refused");
    client->job_id = source.wire_job_id;
    client->last_known_job_id = source.wire_job_id;

    const uint64_t budget_msec = p50_source_arm_budget_msec();
    const uint64_t now_msec = monotonic_msec();
    if (budget_msec == 0 ||
        budget_msec > P50SourceArmedFields::MaxSourceBudgetMsec ||
        now_msec > UINT64_MAX - budget_msec) {
        finish_assignment_claim(source.wire_job_id);
        return refuse("source deadline cannot be represented");
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(budget_msec);
    try {
        size_t pending_count = 0;
        for (const auto &entry : clients)
            pending_count += entry.second->pending_p51_source_arm ? 1u : 0u;
        if (!p51_source_operation_capacity_available(pending_count)) {
            finish_assignment_claim(source.wire_job_id);
            return refuse("bounded reservation setup capacity is full");
        }
        auto pending = std::make_unique<Client::PendingP51SourceArm>();
        pending->arm = arm;
        pending->ready_lease = *current_lease;
        pending->deadline = deadline;
        pending->deadline_msec = now_msec + budget_msec;
        const auto clock =
            icecc::p50::sidecar::process_monotonic_clock_identity();
        pending->absolute_deadline =
            icecc::p50::sidecar::AbsoluteMonotonicDeadline::from_steady_time_point(
                deadline, clock.clock_domain_id, clock.time_namespace_id);
        if (!pending->absolute_deadline.valid())
            throw std::invalid_argument("invalid monotonic deadline");
        pending->connect = std::make_unique<icecc::p50::local::UnixConnectOperation>(
            current_lease->socket_path, deadline);
        pending->connect->advance();
        client->pending_p51_source_arm = std::move(pending);
    } catch (...) {
        finish_assignment_claim(source.wire_job_id);
        return refuse("cannot allocate bounded sidecar reservation state");
    }
    return true;
}

void Daemon::withdraw_p51_source_incarnation(
    const icecc::p50::sidecar::ReadyLease& ready_lease,
    const char* reason) noexcept
{
    if (cache_adapter == nullptr || !ready_lease.valid())
        return;
    const auto current = cache_adapter->outer_current_ready_lease();
    if (!current.has_value() ||
        !icecc::p50::daemon::p50_ready_lease_observation_equal(
            ready_lease, *current))
        return;
    log_error() << "withdrawing exact P51 sidecar incarnation after "
                << reason << endl;
    // This is a recoverable exact-incarnation withdrawal, not final daemon
    // shutdown. The main lifecycle reducer will publish the advertisement
    // transition on its next owner turn and may mint a successor only after
    // the fenced teardown completes.
    cache_adapter->outer_request_replacement();
}

void Daemon::queue_p51_source_cancel(
    const P51SourceArmFields& arm,
    const P51SourceArmedFields& armed,
    const icecc::p50::sidecar::AbsoluteMonotonicDeadline& absolute_deadline,
    const icecc::p50::sidecar::ReadyLease& ready_lease) noexcept
{
    using namespace icecc::p50;
    using namespace icecc::p50::local;
    if (!arm.valid() || !armed.valid() ||
        !armed.acknowledges(P51SourceArmMsg{arm}) ||
        !absolute_deadline.valid() || !ready_lease.valid() ||
        !p51_source_operation_capacity_available(
            pending_p51_source_cancels.size())) {
        if (!p51_source_operation_capacity_available(
                pending_p51_source_cancels.size()))
            withdraw_p51_source_incarnation(
                ready_lease, "reservation-cancel queue saturation");
        return;
    }
    try {
        auto pending = std::make_unique<Client::PendingP51SourceCancel>();
        pending->request.arm = arm;
        pending->request.armed = armed;
        pending->request.absolute_deadline = absolute_deadline;
        pending->ready_lease = ready_lease;
        pending->deadline = absolute_deadline.as_steady_time_point();
        if (pending->deadline <= std::chrono::steady_clock::now())
            return;
        pending->connect = std::make_unique<UnixConnectOperation>(
            ready_lease.socket_path, pending->deadline);
        pending->connect->advance();
        pending_p51_source_cancels.push_back(std::move(pending));
    } catch (...) {
        log_warning() << "cannot queue bounded P51 source-reservation cancellation"
                      << endl;
        withdraw_p51_source_incarnation(
            ready_lease, "reservation-cancel queue allocation failure");
    }
}

bool Daemon::advance_p51_source_cancels(const std::vector<pollfd> &pollfds)
{
    using namespace icecc::p50;
    using namespace icecc::p50::local;
    bool progressed = false;
    for (auto it = pending_p51_source_cancels.begin();
         it != pending_p51_source_cancels.end();) {
        auto& pending = **it;
        if (std::chrono::steady_clock::now() >= pending.deadline) {
            log_warning() << "P51 source-reservation cancellation expired"
                          << endl;
            it = pending_p51_source_cancels.erase(it);
            progressed = true;
            continue;
        }
        short revents = 0;
        const int operation_fd = pending.connect
            ? pending.connect->poll_fd()
            : pending.control ? pending.control->native_handle() : -1;
        for (const pollfd& descriptor : pollfds)
            if (descriptor.fd == operation_fd)
                revents |= descriptor.revents;

        auto fail = [&](const char* reason) {
            log_warning() << "P51 source-reservation cancellation failed: "
                          << reason << endl;
            if (std::chrono::steady_clock::now() < pending.deadline)
                withdraw_p51_source_incarnation(pending.ready_lease, reason);
            it = pending_p51_source_cancels.erase(it);
            progressed = true;
        };
        if (pending.connect) {
            pending.connect->advance(revents);
            if (!pending.connect->done()) {
                ++it;
                continue;
            }
            if (pending.connect->status() != Status::Ok) {
                fail("sidecar connect");
                continue;
            }
            try {
                pending.control = std::make_unique<Connection>(
                    pending.connect->take_connection());
                pending.connect.reset();
                CredentialExpectation expected_peer;
                expected_peer.uid = ::geteuid();
                expected_peer.gid = ::getegid();
                if (!pending.control->valid() ||
                    pending.control->verify_peer_credentials(expected_peer) !=
                        Status::Ok) {
                    fail("sidecar credentials");
                    continue;
                }
                const Identity identity{
                    pending.ready_lease.identity.generation,
                    pending.ready_lease.identity.attempt};
                pending.frame = std::make_unique<FrameOperation>(
                    *pending.control,
                    make_hello(PeerRole::Daemon, identity), pending.deadline);
                pending.stage = Client::PendingP51SourceCancel::Stage::HelloSend;
                progressed = true;
            } catch (...) {
                fail("hello setup");
                continue;
            }
        }
        if (!pending.frame) {
            ++it;
            continue;
        }
        pending.frame->advance();
        if (!pending.frame->done()) {
            ++it;
            continue;
        }
        if (pending.frame->status() != Status::Ok) {
            fail("control frame I/O");
            continue;
        }
        const Identity identity{
            pending.ready_lease.identity.generation,
            pending.ready_lease.identity.attempt};
        try {
            switch (pending.stage) {
            case Client::PendingP51SourceCancel::Stage::HelloSend:
                pending.frame = std::make_unique<FrameOperation>(
                    *pending.control, pending.deadline);
                pending.stage =
                    Client::PendingP51SourceCancel::Stage::HelloAckRead;
                break;
            case Client::PendingP51SourceCancel::Stage::HelloAckRead:
                if (validate_handshake(
                        pending.frame->frame(), local::MessageType::HelloAck,
                        PeerRole::Sidecar, identity) != Status::Ok) {
                    fail("hello acknowledgement identity");
                    continue;
                }
                {
                    ControlOperation operation;
                    operation.kind = ControlOperationKind::SourceReservationCancel;
                    operation.identity = identity;
                    operation.request_id =
                        pending.request.arm.source.source_request_id;
                    operation.sender_role = ControlOperationRole::Daemon;
                    operation.p51_reservation_cancel = pending.request;
                    const std::vector<uint8_t> payload =
                        encode_control_operation(operation);
                    if (payload.empty()) {
                        fail("cancel request encoding");
                        continue;
                    }
                    const local::Frame frame{
                        local::kProtocolVersion, local::MessageType::Data,
                        identity, payload};
                    pending.frame = std::make_unique<FrameOperation>(
                        *pending.control, frame, pending.deadline);
                    pending.stage =
                        Client::PendingP51SourceCancel::Stage::CancelSend;
                }
                break;
            case Client::PendingP51SourceCancel::Stage::CancelSend:
                pending.frame = std::make_unique<FrameOperation>(
                    *pending.control, pending.deadline);
                pending.stage =
                    Client::PendingP51SourceCancel::Stage::CancelReplyRead;
                break;
            case Client::PendingP51SourceCancel::Stage::CancelReplyRead: {
                ControlOperation observed;
                const local::Frame& frame = pending.frame->frame();
                if (frame.type != local::MessageType::Data ||
                    validate_identity(frame, identity) != Status::Ok ||
                    !decode_control_operation(frame.payload, observed) ||
                    observed.kind !=
                        ControlOperationKind::SourceReservationCancel ||
                    observed.identity != identity ||
                    observed.request_id !=
                        pending.request.arm.source.source_request_id ||
                    !observed.p51_reservation_cancel.has_value() ||
                    [&] {
                        auto expected = pending.request;
                        expected.cancelled =
                            observed.p51_reservation_cancel->cancelled;
                        return *observed.p51_reservation_cancel != expected;
                    }() ||
                    !observed.p51_reservation_cancel->cancelled.has_value() ||
                    observed.p51_reservation_cancel_result !=
                        observed.p51_reservation_cancel->cancelled) {
                    fail("cancel reply binding");
                    continue;
                }
                if (std::getenv("ICECC_P50_DEBUG_ATTACH") != nullptr) {
                    static constexpr char hex_digits[] =
                        "0123456789abcdef";
                    std::string reservation_hex;
                    reservation_hex.reserve(
                        pending.request.armed.reservation_id.size() * 2);
                    for (const uint8_t byte :
                         pending.request.armed.reservation_id) {
                        reservation_hex.push_back(hex_digits[byte >> 4]);
                        reservation_hex.push_back(hex_digits[byte & 0x0f]);
                    }
                    const auto& source = pending.request.arm.source;
                    trace() << "P51_SOURCE_CANCEL_RESULT job="
                            << source.wire_job_id
                            << " epoch=" << source.assignment_epoch
                            << " nonce=" << source.assignment_nonce
                            << " request=" << source.source_request_id
                            << " reservation=" << reservation_hex
                            << " cancelled="
                            << (*observed.p51_reservation_cancel_result ? 1 : 0)
                            << endl;
                } else if (*observed.p51_reservation_cancel_result) {
                    trace() << "P51 exact source reservation cancelled" << endl;
                } else {
                    trace() << "P51 source reservation was already settled"
                            << endl;
                }
                const local::Frame goodbye{
                    local::kProtocolVersion, local::MessageType::Goodbye,
                    identity, {}};
                pending.frame = std::make_unique<FrameOperation>(
                    *pending.control, goodbye, pending.deadline);
                pending.stage =
                    Client::PendingP51SourceCancel::Stage::GoodbyeSend;
                break;
            }
            case Client::PendingP51SourceCancel::Stage::GoodbyeSend:
                log_info() << "cancelled exact P51 source reservation"
                           << endl;
                it = pending_p51_source_cancels.erase(it);
                progressed = true;
                continue;
            }
            progressed = true;
        } catch (...) {
            fail("control stage setup");
            continue;
        }
        if (it != pending_p51_source_cancels.end())
            ++it;
    }
    return progressed;
}

bool Daemon::advance_p51_source_leases(const std::vector<pollfd> &pollfds)
{
    for (const auto &entry : clients) {
        Client *client = entry.second;
        auto &pending = client->pending_p51_source_lease;
        if (!pending)
            continue;
        short revents = 0;
        const int operation_fd = pending->connect
            ? pending->connect->poll_fd()
            : pending->control && pending->frame
                ? pending->control->native_handle() : -1;
        for (const pollfd &descriptor : pollfds)
            if (descriptor.fd == operation_fd)
                revents |= descriptor.revents;

        auto fail = [&](const char *reason) {
            log_warning() << "P51 source lease setup failed: " << reason << endl;
            pending.reset();
            (void)client->channel->send_msg(EndMsg());
            handle_end(client, 151);
        };
        if (std::chrono::steady_clock::now() >= pending->deadline) {
            fail("deadline");
            return true;
        }
        if (pending->connect) {
            pending->connect->advance(revents);
            if (!pending->connect->done())
                continue;
            if (pending->connect->status() !=
                icecc::p50::local::Status::Ok) {
                fail("connect");
                return true;
            }
            try {
                pending->control = std::make_unique<
                    icecc::p50::local::Connection>(
                        pending->connect->take_connection());
                pending->connect.reset();
                icecc::p50::local::CredentialExpectation expected_peer;
                expected_peer.uid = ::geteuid();
                expected_peer.gid = ::getegid();
                if (!pending->control->valid() ||
                    pending->control->verify_peer_credentials(expected_peer) !=
                        icecc::p50::local::Status::Ok) {
                    fail("peer credentials");
                    return true;
                }
                const icecc::p50::local::Identity identity{
                    pending->ready_lease.identity.generation,
                    pending->ready_lease.identity.attempt};
                pending->frame = std::make_unique<
                    icecc::p50::local::FrameOperation>(
                        *pending->control,
                        icecc::p50::local::make_hello(
                            icecc::p50::local::PeerRole::Daemon, identity),
                        pending->deadline);
            } catch (...) {
                fail("hello setup");
                return true;
            }
        }
        if (!pending->frame)
            continue;
        pending->frame->advance();
        if (!pending->frame->done())
            continue;
        if (pending->frame->status() != icecc::p50::local::Status::Ok) {
            fail(pending->hello_sent ? "hello acknowledgement receive"
                                     : "hello send");
            return true;
        }
        if (!pending->hello_sent) {
            pending->hello_sent = true;
            const icecc::p50::local::Identity identity{
                pending->ready_lease.identity.generation,
                pending->ready_lease.identity.attempt};
            try {
                pending->frame = std::make_unique<
                    icecc::p50::local::FrameOperation>(
                        *pending->control, pending->deadline);
            } catch (...) {
                fail("acknowledgement setup");
                return true;
            }
            (void)identity;
            continue;
        }
        const icecc::p50::local::Identity identity{
            pending->ready_lease.identity.generation,
            pending->ready_lease.identity.attempt};
        if (icecc::p50::local::validate_handshake(
                pending->frame->frame(),
                icecc::p50::local::MessageType::HelloAck,
                icecc::p50::local::PeerRole::Sidecar, identity) !=
            icecc::p50::local::Status::Ok) {
            fail("acknowledgement identity");
            return true;
        }

        const int client_fd = client->channel->fd;
        const auto live = fd2client.find(client_fd);
        const auto current_lease = cache_adapter != nullptr
            ? cache_adapter->outer_current_ready_lease()
            : std::optional<icecc::p50::sidecar::ReadyLease>{};
        const auto revalidated = connection_leases.revalidate_live(
            client->connection_provenance.lease, client, client->channel,
            client->connection_provenance.peer);
        const bool exact_assignment = client->usecsmsg != nullptr &&
            client->cacheHandoff.valid &&
            pending->request.wire_job_id == client->usecsmsg->job_id &&
            pending->request.assignment_epoch ==
                client->usecsmsg->assignmentEpoch() &&
            pending->request.assignment_nonce ==
                client->usecsmsg->assignmentNonce() &&
            pending->request.requested_cache_revision ==
                client->usecsmsg->cache_protocol &&
            pending->request.requested_cache_revision ==
                client->cacheHandoff.cacheProtocol &&
            client->usecsmsg->cache_endpoint_port ==
                client->cacheHandoff.cachePort &&
            (pending->request.profile &
             client->cacheHandoff.cacheProfileMask) ==
                pending->request.profile;
        if (live == fd2client.end() || live->second != client ||
            !revalidated || !exact_assignment || !current_lease ||
            !current_lease->valid() ||
            !client->cacheHandoff.readyLease.has_value() ||
            !client->cacheHandoff.readyLease->valid()) {
            fail("stale Client, assignment, or READY lease");
            return true;
        }
        if (!icecc::p50::daemon::p50_ready_lease_observation_equal(
                pending->ready_lease, *current_lease) ||
            !icecc::p50::daemon::p50_ready_lease_observation_equal(
                *client->cacheHandoff.readyLease, *current_lease)) {
            const bool eligible_successor =
                scheduler_owns_getcs_assignment(client) &&
                client->usecsmsg->cache_protocol ==
                    client->cacheHandoff.cacheProtocol &&
                (pending->request.profile &
                 client->cacheHandoff.cacheProfileMask) ==
                    pending->request.profile &&
                (pending->request.profile &
                 cache_unavailable_profile_mask) == 0;
            if (!eligible_successor) {
                fail("READY replacement is not assignment-eligible");
                return true;
            }
            pending->frame.reset();
            pending->control.reset();
            pending->connect.reset();
            pending->hello_sent = false;
            pending->ready_lease = *current_lease;
            client->cacheHandoff.readyLease = *current_lease;
            client->cacheHandoff.routeStateGeneration =
                cache_route_state_generation;
            try {
                pending->connect = std::make_unique<
                    icecc::p50::local::UnixConnectOperation>(
                        current_lease->socket_path, pending->deadline);
                pending->connect->advance();
            } catch (...) {
                fail("successor READY connect allocation");
            }
            return true;
        }
        const int transfer_fd = ::dup(pending->control->native_handle());
        if (transfer_fd < 0) {
            fail("descriptor duplication");
            return true;
        }
        const int flags = ::fcntl(transfer_fd, F_GETFD);
        if (flags < 0 ||
            ::fcntl(transfer_fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
            (void)::close(transfer_fd);
            fail("descriptor CLOEXEC");
            return true;
        }
        const P51CacheControlIdentity control_identity{
            identity.generation, identity.attempt,
            static_cast<uint64_t>(::geteuid()),
            static_cast<uint64_t>(::getegid()),
            pending->ready_lease.store_generation,
            pending->ready_lease.store_derivation_version,
            pending->ready_lease.c_store_guid.bytes};
        const P51SourceLeaseRequestFields request = pending->request;
        const auto reply_deadline = pending->deadline;
        P51CacheFdReplyTicket ticket = std::move(pending->reply_ticket);
        pending.reset();
        if (!client->channel->send_p51_cache_fd_reply(
                std::move(ticket), control_identity, transfer_fd,
                reply_deadline)) {
            (void)client->channel->send_msg(EndMsg());
            handle_end(client, 151);
            return true;
        }
        trace() << "P51 C-cache source-control lease delivered for assignment "
                << request.wire_job_id << " profile " << request.profile
                << " window " << request.requested_window << endl;
        return true;
    }
    return false;
}

bool Daemon::advance_p51_source_arms(const std::vector<pollfd> &pollfds)
{
    static bool test_pause_after_goodbye_used = false;
    for (const auto &entry : clients) {
        Client *client = entry.second;
        auto &pending = client->pending_p51_source_arm;
        if (!pending)
            continue;
        short revents = 0;
        const int operation_fd = pending->connect
            ? pending->connect->poll_fd()
            : pending->control ? pending->control->native_handle() : -1;
        for (const pollfd &descriptor : pollfds)
            if (descriptor.fd == operation_fd)
                revents |= descriptor.revents;

        auto fail = [&](const char *reason) {
            log_warning() << "P51 source arm setup failed: " << reason
                          << " stage=" << static_cast<unsigned>(pending->stage)
                          << " reservation_error_code="
                          << pending->reservation_error_code
                          << " fd=" << operation_fd
                          << " frame_status="
                          << (pending->frame
                                  ? static_cast<unsigned>(pending->frame->status())
                                  : 255u)
                          << endl;
            const uint32_t job_id = pending->arm.source.wire_job_id;
            if (pending->armed.has_value() &&
                pending->reservation_error_code == 0)
                queue_p51_source_cancel(pending->arm, *pending->armed,
                                        pending->absolute_deadline,
                                        pending->ready_lease);
            pending.reset();
            if (job_id != 0)
                finish_assignment_claim(job_id);
            if (client->channel != nullptr)
                (void)client->channel->send_msg(EndMsg());
            handle_end(client, 153);
        };
        if (std::chrono::steady_clock::now() >= pending->deadline) {
            fail("absolute source deadline");
            return true;
        }
        if (pending->connect) {
            pending->connect->advance(revents);
            if (!pending->connect->done())
                continue;
            if (pending->connect->status() !=
                icecc::p50::local::Status::Ok) {
                fail("sidecar connect");
                return true;
            }
            try {
                pending->control = std::make_unique<icecc::p50::local::Connection>(
                    pending->connect->take_connection());
                pending->connect.reset();
                icecc::p50::local::CredentialExpectation expected_peer;
                expected_peer.uid = ::geteuid();
                expected_peer.gid = ::getegid();
                if (!pending->control->valid() ||
                    pending->control->verify_peer_credentials(expected_peer) !=
                        icecc::p50::local::Status::Ok) {
                    fail("sidecar peer credentials");
                    return true;
                }
                const icecc::p50::local::Identity identity{
                    pending->ready_lease.identity.generation,
                    pending->ready_lease.identity.attempt};
                pending->frame = std::make_unique<icecc::p50::local::FrameOperation>(
                    *pending->control,
                    icecc::p50::local::make_hello(
                        icecc::p50::local::PeerRole::Daemon, identity),
                    pending->deadline);
                pending->stage = Client::PendingP51SourceArm::Stage::HelloSend;
            } catch (...) {
                fail("sidecar hello setup");
                return true;
            }
        }
        if (!pending->frame)
            continue;
        pending->frame->advance();
        if (!pending->frame->done())
            continue;
        if (pending->frame->status() != icecc::p50::local::Status::Ok) {
            fail("sidecar control frame I/O");
            return true;
        }

        const icecc::p50::local::Identity identity{
            pending->ready_lease.identity.generation,
            pending->ready_lease.identity.attempt};
        try {
            switch (pending->stage) {
            case Client::PendingP51SourceArm::Stage::Connecting:
                fail("invalid connect state");
                return true;
            case Client::PendingP51SourceArm::Stage::HelloSend:
                pending->frame = std::make_unique<icecc::p50::local::FrameOperation>(
                    *pending->control, pending->deadline);
                pending->stage =
                    Client::PendingP51SourceArm::Stage::HelloAckRead;
                break;
            case Client::PendingP51SourceArm::Stage::HelloAckRead:
                if (icecc::p50::local::validate_handshake(
                        pending->frame->frame(),
                        icecc::p50::local::MessageType::HelloAck,
                        icecc::p50::local::PeerRole::Sidecar, identity) !=
                    icecc::p50::local::Status::Ok) {
                    fail("sidecar hello acknowledgement identity");
                    return true;
                }
                {
                    const icecc::p50::local::P51SourceReservationRequest request{
                        pending->arm, pending->absolute_deadline};
                    auto operation =
                        icecc::p50::local::make_p51_source_reservation_operation(
                            identity, request);
                    const std::vector<uint8_t> payload =
                        icecc::p50::local::encode_control_operation(operation);
                    if (payload.empty()) {
                        fail("reservation request encoding");
                        return true;
                    }
                    const icecc::p50::local::Frame frame{
                        icecc::p50::local::kProtocolVersion,
                        icecc::p50::local::MessageType::Data, identity, payload};
                    pending->frame =
                        std::make_unique<icecc::p50::local::FrameOperation>(
                            *pending->control, frame, pending->deadline);
                    pending->stage =
                        Client::PendingP51SourceArm::Stage::ReservationSend;
                }
                break;
            case Client::PendingP51SourceArm::Stage::ReservationSend:
                pending->frame = std::make_unique<icecc::p50::local::FrameOperation>(
                    *pending->control, pending->deadline);
                pending->stage =
                    Client::PendingP51SourceArm::Stage::ReservationReplyRead;
                break;
            case Client::PendingP51SourceArm::Stage::ReservationReplyRead: {
                icecc::p50::local::ControlOperation observed;
                const auto &frame = pending->frame->frame();
                if (frame.type != icecc::p50::local::MessageType::Data ||
                    icecc::p50::local::validate_identity(frame, identity) !=
                        icecc::p50::local::Status::Ok ||
                    !icecc::p50::local::decode_control_operation(
                        frame.payload, observed) ||
                    observed.kind !=
                        icecc::p50::local::ControlOperationKind::SourceReservation ||
                    observed.identity != identity ||
                    observed.request_id != pending->arm.source.source_request_id ||
                    !observed.p51_reservation.has_value() ||
                    *observed.p51_reservation !=
                        icecc::p50::local::P51SourceReservationRequest{
                            pending->arm, pending->absolute_deadline} ||
                    !observed.p51_reservation_result.has_value() ||
                    !observed.p51_reservation_result->valid()) {
                    fail("reservation response binding");
                    return true;
                }
                pending->reservation_error_code =
                    observed.p51_reservation_result->error_code;
                pending->armed = observed.p51_reservation_result->armed;
                const auto goodbye = icecc::p50::local::Frame{
                    icecc::p50::local::kProtocolVersion,
                    icecc::p50::local::MessageType::Goodbye, identity, {}};
                pending->frame =
                    std::make_unique<icecc::p50::local::FrameOperation>(
                        *pending->control, goodbye, pending->deadline);
                pending->stage = Client::PendingP51SourceArm::Stage::GoodbyeSend;
                break;
            }
            case Client::PendingP51SourceArm::Stage::GoodbyeSend: {
                /*
                 * A process-level test can stop one specifically identified,
                 * successfully reserved ARM after the daemon has written
                 * Goodbye to the sidecar but before final deadline/READY/assignment
                 * checks below.  This makes the otherwise narrow deadline
                 * race deterministic without changing the request deadline.
                 */
                const char *pause_request = std::getenv(
                    "ICECC_TEST_P51_PAUSE_AFTER_GOODBYE_REQUEST");
                if (!test_pause_after_goodbye_used &&
                    std::getenv("ICECC_TESTS") != nullptr &&
                    pause_request != nullptr && *pause_request != '\0' &&
                    pending->armed.has_value() &&
                    pending->reservation_error_code == 0 &&
                    pending->armed->acknowledges(P51SourceArmMsg{pending->arm})) {
                    char *end = nullptr;
                    errno = 0;
                    const unsigned long long request_id =
                        std::strtoull(pause_request, &end, 10);
                    if (errno == 0 && end != pause_request && *end == '\0' &&
                        request_id == pending->arm.source.source_request_id) {
                        test_pause_after_goodbye_used = true;
                        log_warning() << "test-only P51 ARM pause after Goodbye"
                                      << " request_id=" << request_id << endl;
                        (void)::raise(SIGSTOP);
                    }
                }
                const auto current_lease = cache_adapter != nullptr
                    ? cache_adapter->outer_current_ready_lease()
                    : std::optional<icecc::p50::sidecar::ReadyLease>{};
                const int client_fd = client->channel != nullptr
                    ? client->channel->fd : -1;
                const auto live_fd = fd2client.find(client_fd);
                const auto provenance = connection_leases.revalidate_live(
                    client->connection_provenance.lease, client, client->channel,
                    client->connection_provenance.peer);
                const auto assignment = live_assignments.find(
                    pending->arm.source.wire_job_id);
                const bool exact_assignment =
                    assignment != live_assignments.end() &&
                    assignment->second.phase == WorkerAssignment::Claimed &&
                    assignment->second.claimant ==
                        static_cast<uint32_t>(client->client_id) &&
                    assignment->second.key.epoch ==
                        pending->arm.source.assignment_epoch &&
                    assignment->second.key.nonce ==
                        pending->arm.source.assignment_nonce;
                if (live_fd == fd2client.end() || live_fd->second != client ||
                    !provenance || !exact_assignment || !current_lease ||
                    !current_lease->valid()) {
                    fail("stale Client, assignment, or READY completion");
                    return true;
                }
                if (!icecc::p50::daemon::p50_ready_lease_observation_equal(
                        pending->ready_lease, *current_lease)) {
                    if (pending->armed.has_value() &&
                        pending->reservation_error_code == 0)
                        queue_p51_source_cancel(pending->arm, *pending->armed,
                                                pending->absolute_deadline,
                                                pending->ready_lease);
                    pending->frame.reset();
                    pending->control.reset();
                    pending->connect.reset();
                    pending->armed.reset();
                    pending->ready_lease = *current_lease;
                    pending->connect =
                        std::make_unique<icecc::p50::local::UnixConnectOperation>(
                            current_lease->socket_path, pending->deadline);
                    pending->connect->advance();
                    pending->stage =
                        Client::PendingP51SourceArm::Stage::Connecting;
                    return true;
                }
                if (pending->reservation_error_code != 0 ||
                    !pending->armed.has_value()) {
                    fail("F rejected source reservation");
                    return true;
                }
                if (pending->deadline <= std::chrono::steady_clock::now() ||
                    !pending->armed->acknowledges(
                        P51SourceArmMsg{pending->arm})) {
                    fail("expired or mismatched ARMED result");
                    return true;
                }
                const uint64_t remaining = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        pending->deadline - std::chrono::steady_clock::now())
                        .count());
                if (remaining == 0 || remaining >
                        P50SourceArmedFields::MaxSourceBudgetMsec) {
                    fail("source deadline expired before ARMED");
                    return true;
                }
                client->p50_source_arm_fields = pending->arm.source;
                client->p51_source_arm_fields = pending->arm;
                client->p51_source_armed_fields = *pending->armed;
                if (!client->p50_input_wait.arm_input(pending->arm.source)) {
                    fail("cannot install R2 input wait");
                    return true;
                }
                client->p50_source_f_lease = *current_lease;
                client->p50_source_f_store_generation =
                    current_lease->store_generation;
                client->p50_source_arm_provenance =
                    client->connection_provenance;
                client->p50_source_deadline_msec =
                    monotonic_msec() + remaining;
                client->p51_source_absolute_deadline =
                    pending->absolute_deadline;
                client->p50_source_compile_pending = false;
                client->set_status(
                    Client::WAITP50INPUT,
                    "p51: exact source reservation armed; waiting for CompileFile");
                const P51SourceArmedMsg acknowledgement{*pending->armed};
                if (!acknowledgement.valid_payload()) {
                    fail("invalid final ARMED witness");
                    return true;
                }
                pending.reset();
                if (!client->channel->send_msg(acknowledgement)) {
                    finish_assignment_claim(client->job_id);
                    (void)client->channel->send_msg(EndMsg());
                    handle_end(client, 153);
                    return true;
                }
                return true;
            }
            }
        } catch (...) {
            fail("reservation state transition exception");
            return true;
        }
    }
    return false;
}

bool Daemon::advance_orphaned_p51_source_arms(
    const std::vector<pollfd>& pollfds)
{
    using namespace icecc::p50;
    using namespace icecc::p50::local;
    (void)pollfds;
    bool progressed = false;
    for (auto it = orphaned_p51_source_arms.begin();
         it != orphaned_p51_source_arms.end();) {
        auto& pending = **it;
        if (std::chrono::steady_clock::now() >= pending.deadline) {
            log_warning() << "orphaned P51 ARM expired before reservation cancel"
                          << endl;
            it = orphaned_p51_source_arms.erase(it);
            progressed = true;
            continue;
        }
        if (!pending.control || !pending.frame || pending.connect) {
            log_warning() << "orphaned P51 ARM lost its in-flight control state"
                          << endl;
            const bool cancel = pending.armed.has_value() &&
                                pending.reservation_error_code == 0;
            const auto arm = pending.arm;
            const auto armed = pending.armed;
            const auto deadline = pending.absolute_deadline;
            const auto ready_lease = pending.ready_lease;
            it = orphaned_p51_source_arms.erase(it);
            if (cancel)
                queue_p51_source_cancel(arm, *armed, deadline, ready_lease);
            else
                withdraw_p51_source_incarnation(
                    ready_lease, "orphaned ARM control state loss");
            progressed = true;
            continue;
        }
        pending.frame->advance();
        if (!pending.frame->done()) {
            ++it;
            continue;
        }
        if (pending.frame->status() != Status::Ok) {
            const bool cancel = pending.armed.has_value() &&
                                pending.reservation_error_code == 0;
            const auto arm = pending.arm;
            const auto armed = pending.armed;
            const auto deadline = pending.absolute_deadline;
            const auto steady_deadline = pending.deadline;
            const auto ready_lease = pending.ready_lease;
            log_warning() << "orphaned P51 ARM control exchange failed" << endl;
            it = orphaned_p51_source_arms.erase(it);
            if (cancel)
                queue_p51_source_cancel(arm, *armed, deadline, ready_lease);
            else if (std::chrono::steady_clock::now() < steady_deadline)
                withdraw_p51_source_incarnation(
                    ready_lease, "orphaned ARM response loss");
            progressed = true;
            continue;
        }
        try {
            switch (pending.stage) {
            case Client::PendingP51SourceArm::Stage::ReservationSend:
                pending.frame = std::make_unique<FrameOperation>(
                    *pending.control, pending.deadline);
                pending.stage =
                    Client::PendingP51SourceArm::Stage::ReservationReplyRead;
                break;
            case Client::PendingP51SourceArm::Stage::ReservationReplyRead: {
                ControlOperation observed;
                const local::Frame& frame = pending.frame->frame();
                const Identity identity{
                    pending.ready_lease.identity.generation,
                    pending.ready_lease.identity.attempt};
                const P51SourceReservationRequest request{
                    pending.arm, pending.absolute_deadline};
                if (frame.type != local::MessageType::Data ||
                    validate_identity(frame, identity) != Status::Ok ||
                    !decode_control_operation(frame.payload, observed) ||
                    observed.kind != ControlOperationKind::SourceReservation ||
                    observed.identity != identity ||
                    observed.request_id != pending.arm.source.source_request_id ||
                    observed.p51_reservation != request ||
                    !observed.p51_reservation_result.has_value() ||
                    !observed.p51_reservation_result->valid())
                    throw std::invalid_argument(
                        "late reservation response identity mismatch");
                pending.reservation_error_code =
                    observed.p51_reservation_result->error_code;
                pending.armed = observed.p51_reservation_result->armed;
                {
                    const local::Frame goodbye{
                        local::kProtocolVersion, local::MessageType::Goodbye,
                        identity, {}};
                    pending.frame = std::make_unique<FrameOperation>(
                        *pending.control, goodbye, pending.deadline);
                }
                pending.stage = Client::PendingP51SourceArm::Stage::GoodbyeSend;
                break;
            }
            case Client::PendingP51SourceArm::Stage::GoodbyeSend:
                {
                    const bool cancel = pending.armed.has_value() &&
                                        pending.reservation_error_code == 0;
                    const auto arm = pending.arm;
                    const auto armed = pending.armed;
                    const auto deadline = pending.absolute_deadline;
                    const auto ready_lease = pending.ready_lease;
                    it = orphaned_p51_source_arms.erase(it);
                    if (cancel)
                        queue_p51_source_cancel(arm, *armed, deadline,
                                                ready_lease);
                }
                progressed = true;
                continue;
            default:
                throw std::logic_error(
                    "orphaned P51 ARM reached invalid cleanup stage");
            }
            progressed = true;
        } catch (const std::exception& error) {
            log_warning() << "orphaned P51 ARM cleanup failed: "
                          << error.what() << endl;
            const bool cancel = pending.armed.has_value() &&
                                pending.reservation_error_code == 0;
            const auto arm = pending.arm;
            const auto armed = pending.armed;
            const auto deadline = pending.absolute_deadline;
            const auto steady_deadline = pending.deadline;
            const auto ready_lease = pending.ready_lease;
            it = orphaned_p51_source_arms.erase(it);
            if (cancel)
                queue_p51_source_cancel(arm, *armed, deadline, ready_lease);
            else if (std::chrono::steady_clock::now() < steady_deadline)
                withdraw_p51_source_incarnation(
                    ready_lease, "orphaned ARM validation failure");
            progressed = true;
            continue;
        }
        ++it;
    }
    return progressed;
}

bool Daemon::handle_cache_session(Client *client, Msg *msg)
{
    if (!client || !client->channel || !msg || *msg != Msg::CACHE_SESSION) {
        return false;
    }

    const int old_fd = client->channel->fd;
    // The descriptor handoff is asynchronous from the wrapper's point of
    // view. Re-enter only through the immutable value lease and the exact
    // live object identities captured at accept time. An fd or Client address
    // reused after teardown must never authorize a CACHE_SESSION dispatch.
    const auto lease = client->connection_provenance.lease;
    const bool public_tcp_cache_session =
        client->connection_provenance.listener == ListenerKind::TcpLoopback ||
        client->connection_provenance.listener == ListenerKind::TcpRemote;
    if (!public_tcp_cache_session ||
        !connection_leases.revalidate_live(
            lease, client, client->channel, client->connection_provenance.peer)) {
        log_warning() << "CACHE_SESSION rejected stale wrapper connection lease" << endl;
        handle_end(client, 121);
        return false;
    }

    /* Dispatch the exact clean-boundary descriptor to the authenticated
       sidecar under the authoritative CacheSession handoff.  That handoff
       starts the endpoint itself; no second shadow P5FS relationship is
       created for the same operation. */
    if (client->status != Client::WAITP50INPUT ||
        !client->p50_source_arm_fields.has_value() || cache_adapter == nullptr ||
        !cache_adapter->authenticated() ||
        cache_adapter->dispatcher() == nullptr) {
        log_warning() << "CACHE_SESSION refused: no authenticated cache sidecar"
                      << endl;
        handle_end(client, 121);
        return false;
    }
    const auto ready_lease = cache_adapter->outer_current_ready_lease();
    if (!ready_lease.has_value() || !ready_lease->valid()) {
        log_warning() << "CACHE_SESSION refused: no READY sidecar lease" << endl;
        handle_end(client, 121);
        return false;
    }
    const auto outcome = cache_adapter->dispatcher()->dispatch(
        *client->channel, client->channel->protocol,
        static_cast<uint32_t>(Msg::CACHE_SESSION));
    if (outcome.result != icecc::p50::daemon::CacheDispatchResult::Accepted ||
        !outcome.handoff_acknowledged || !outcome.trailing_byte_barrier) {
        log_warning() << "CACHE_SESSION handoff failed (result="
                      << int(outcome.result) << ", handoff="
                      << icecc::p50::local::fd_handoff_status_name(
                             outcome.handoff_status)
                      << ", acknowledged="
                      << (outcome.handoff_acknowledged ? 1 : 0)
                      << ", boundary="
                      << (outcome.trailing_byte_barrier ? 1 : 0) << ")"
                      << endl;
        handle_end(client, 121);
        return false;
    }

    // The handoff consumed the public descriptor. Remove only its stale
    // poll-map entry: the Client and its exact source-arm claim remain in
    // `clients` for the later ordinary CompileFile connection and for
    // deadline/lease withdrawal. In particular, do not call handle_end()
    // here; that would settle the retained scheduler assignment.
    client->p50_cache_session_detached = true;
    const auto fd_entry = fd2client.find(old_fd);
    if (fd_entry != fd2client.end() && fd_entry->second == client)
        fd2client.erase(fd_entry);
    trace() << "CACHE_SESSION dispatched to authoritative sidecar endpoint (gen "
            << ready_lease->identity.generation << "/"
            << ready_lease->identity.attempt << ")" << endl;
    return true;
}

bool Daemon::handle_p51_cache_link_session(
    Client *client, P51CacheLinkSessionMsg *msg)
{
    auto refuse = [&](const char *reason) {
        log_warning() << "P51 cache-link setup refused: " << reason << endl;
        if (client != nullptr && client->channel != nullptr)
            (void)client->channel->send_msg(EndMsg());
        if (client != nullptr)
            handle_end(client, 152);
        return false;
    };
    if (client == nullptr || client->channel == nullptr || msg == nullptr ||
        !protocol_supports_cache_r2(client->channel->protocol))
        return refuse("invalid request or protocol");

    const int old_fd = client->channel->fd;
    const auto &provenance = client->connection_provenance;
    const bool public_tcp_cache_link =
        provenance.listener == ListenerKind::TcpLoopback ||
        provenance.listener == ListenerKind::TcpRemote;
    if (client->status != Client::UNKNOWN)
        return refuse("link channel is not in UNKNOWN state");
    if (client->job != nullptr || client->usecsmsg != nullptr)
        return refuse("link channel already owns a compiler assignment");
    if (client->p50_source_arm_fields.has_value() ||
        client->p51_source_arm_fields.has_value() ||
        client->p51_source_armed_fields.has_value() ||
        client->pending_p51_source_lease || client->pending_p51_source_arm)
        return refuse("link channel already owns source-arm state");
    if (client->p50_attachment || client->p50_attachment_lease.has_value())
        return refuse("link channel already owns input attachment state");
    if (!public_tcp_cache_link)
        return refuse("link channel did not arrive on a public TCP listener");
    // Public TCP sockets have no Unix peer credentials by design. The
    // immutable accepted-peer tuple is still part of the lease comparison;
    // peer.complete() is required only for Unix-local provenance.
    if (!connection_leases.revalidate_live(
            provenance.lease, client, client->channel, provenance.peer))
        return refuse("link channel connection lease is stale");
    if (cache_adapter == nullptr || !cache_adapter->authenticated() ||
        cache_adapter->dispatcher() == nullptr)
        return refuse("authenticated cache sidecar is unavailable");
    const auto ready_lease = cache_adapter->outer_current_ready_lease();
    if (!ready_lease.has_value() || !ready_lease->valid())
        return refuse("sidecar has no current READY endpoint");

    const auto outcome = cache_adapter->dispatcher()->dispatch(
        *client->channel, client->channel->protocol,
        static_cast<uint32_t>(Msg::P51_CACHE_LINK_SESSION));
    if (outcome.result != icecc::p50::daemon::CacheDispatchResult::Accepted ||
        !outcome.detached || !outcome.handoff_acknowledged ||
        !outcome.trailing_byte_barrier) {
        log_warning() << "P51 cache-link handoff failed (result="
                      << int(outcome.result) << ", handoff="
                      << icecc::p50::local::fd_handoff_status_name(
                             outcome.handoff_status)
                      << ", detached=" << (outcome.detached ? 1 : 0)
                      << ", acknowledged="
                      << (outcome.handoff_acknowledged ? 1 : 0)
                      << ", boundary="
                      << (outcome.trailing_byte_barrier ? 1 : 0) << ")"
                      << endl;
        handle_end(client, 152);
        return false;
    }

    // This is an auxiliary persistent-data-link setup socket, not a compiler
    // assignment owner.  The clean descriptor is now owned by the sidecar;
    // remove only this temporary ordinary client without settling any job.
    const auto fd_entry = fd2client.find(old_fd);
    if (fd_entry != fd2client.end() && fd_entry->second == client)
        fd2client.erase(fd_entry);
    trace() << "P51 cache-link descriptor adopted by sidecar (generation "
            << ready_lease->identity.generation << "/"
            << ready_lease->identity.attempt << ")" << endl;
    handle_end(client, 0);
    return false;
}

bool Daemon::handle_p50_source_arm(Client *client, P50SourceArmMsg *msg)
{
    if (client == nullptr) {
        return false;
    }

    /* A valid exact triple is the only identity on which a rejection may
       settle a scheduler assignment.  A malformed frame is otherwise
       untrusted input; if this is a later malformed/replayed frame, the
       already-retained owner supplies the exact token. */
    auto reject = [&](const P50SourceArmFields *identity, int exitcode) {
        const P50SourceArmFields *settlement_identity = identity;
        // Once this wrapper owns a live arm, every later rejection belongs to
        // that immutable token.  Do not let a malformed/replayed frame name a
        // second live PREPARE and strand the retained owner when this client
        // is torn down.
        if (client->p50_source_arm_fields) {
            settlement_identity = &*client->p50_source_arm_fields;
        }
        if (settlement_identity != nullptr &&
            bind_source_assignment_for_settlement(
                *settlement_identity, static_cast<uint32_t>(client->client_id))) {
            client->job_id = settlement_identity->wire_job_id;
            client->last_known_job_id = settlement_identity->wire_job_id;
            client->set_status(Client::WAITP50INPUT,
                               "p50: rejected source arm settled exactly");
            finish_assignment_claim(settlement_identity->wire_job_id);
        }
        if (client->channel) {
            (void)client->channel->send_msg(EndMsg());
        }
        handle_end(client, exitcode);
        return false;
    };

    if (msg == nullptr || !msg->valid_payload()) {
        log_warning() << "P50 source arm rejected before admission: invalid payload"
                      << endl;
        return reject(nullptr, 147);
    }

    const P50SourceArmFields& arm = msg->arm;
    if (client->status != Client::UNKNOWN ||
        client->p50_source_arm_fields.has_value() ||
        !client->source_wrapper_provenance_valid() ||
        cache_adapter == nullptr || !scheduler_session_active ||
        !cache_advertisement_snapshot().present()) {
        log_warning() << "P50 source arm rejected before admission: daemon state"
                      << " status=" << static_cast<unsigned>(client->status)
                      << " retained=" << client->p50_source_arm_fields.has_value()
                      << " provenance=" << client->source_wrapper_provenance_valid()
                      << " adapter=" << (cache_adapter != nullptr)
                      << " scheduler=" << scheduler_session_active
                      << " advertisement="
                      << cache_advertisement_snapshot().present() << endl;
        return reject(&arm, 147);
    }

    const auto& current_lease = cache_adapter->outer_current_ready_lease();
    if (!current_lease.has_value() || !current_lease->valid()) {
        log_warning() << "P50 source arm rejected before admission: no current ready lease"
                      << endl;
        return reject(&arm, 147);
    }
    const auto& lease = *current_lease;
    const auto snapshot = cache_advertisement_snapshot();

    // This ordinary connection is the selected F wrapper link.  Its host,
    // ordinary listener, advertised endpoint, protocol, and selected profile
    // must all agree with the one current F advertisement.  In particular,
    // profile membership is a mask test; a profile value is never compared
    // to a capability mask as if the latter were scalar identity.
    if (arm.selected_f_host != remote_name ||
        arm.selected_f_ordinary_port != static_cast<uint32_t>(daemon_port) ||
        arm.selected_f_cache_port != snapshot.endpoint_port ||
        arm.cache_protocol != snapshot.protocol ||
        !p50_source_profile_selection_valid(arm.cache_profile) ||
        !p50_source_profile_mode_valid(arm.cache_profile, arm.source_mode) ||
        (snapshot.profile_mask & arm.cache_profile) == 0 ||
        arm.c_store_derivation_version != lease.store_derivation_version ||
        icecc::p50::store_identity_file_guid_matches_client(
            arm.c_store_guid, lease.f_store_guid.bytes)) {
        log_warning() << "P50 source arm rejected before admission: binding mismatch"
                      << " host=" << (arm.selected_f_host == remote_name)
                      << " ordinary_port="
                      << (arm.selected_f_ordinary_port ==
                          static_cast<uint32_t>(daemon_port))
                      << " endpoint_port="
                      << (arm.selected_f_cache_port == snapshot.endpoint_port)
                      << " protocol=" << (arm.cache_protocol == snapshot.protocol)
                      << " profile="
                      << p50_source_profile_selection_valid(arm.cache_profile)
                      << " mode="
                      << p50_source_profile_mode_valid(arm.cache_profile,
                                                       arm.source_mode)
                      << " advertised="
                      << ((snapshot.profile_mask & arm.cache_profile) != 0)
                      << " derivation="
                      << (arm.c_store_derivation_version ==
                          lease.store_derivation_version)
                      << " distinct_store="
                      << !icecc::p50::store_identity_file_guid_matches_client(
                             arm.c_store_guid, lease.f_store_guid.bytes)
                      << " selected_profile=" << arm.cache_profile
                      << " source_mode=" << arm.source_mode
                      << " profile_mask=" << snapshot.profile_mask << endl;
        return reject(&arm, 147);
    }

    // Claim PREPARE before installing and acknowledging WAIT.  The token is
    // bound before any ACK bytes leave the ordinary wrapper connection, so a
    // disconnect/HUP/expiry can use the same exact scheduler JobDone path.
    if (!authorize_source_arm_claim(
            arm, static_cast<uint32_t>(client->client_id))) {
        log_warning() << "P50 source arm rejected before admission: assignment claim"
                      << endl;
        return reject(&arm, 147);
    }
    client->job_id = arm.wire_job_id;
    client->last_known_job_id = arm.wire_job_id;

    // authorize_source_arm_claim() retains the scheduler owner during this
    // synchronous turn.  Keep Client UNKNOWN until arm_p50_source installs
    // the complete arm/lease/deadline and performs the sole WAIT transition;
    // pre-setting WAIT makes that atomic installer reject every real arm.

    if (next_p50_arm_observation_id == 0 ||
        next_p50_arm_observation_id == UINT64_MAX) {
        log_warning() << "P50 source arm rejected before admission: observation id exhausted"
                      << endl;
        finish_assignment_claim(arm.wire_job_id);
        return reject(&arm, 147);
    }
    const uint64_t observation = next_p50_arm_observation_id++;
    const uint64_t now = monotonic_msec();
    const uint64_t budget_msec = p50_source_arm_budget_msec();
    if (budget_msec == 0 || now > UINT64_MAX - budget_msec ||
        !client->arm_p50_source(
            arm, lease, observation, now + budget_msec)) {
        log_warning() << "P50 source arm rejected before admission: arm installation"
                      << " budget_msec=" << budget_msec << endl;
        finish_assignment_claim(arm.wire_job_id);
        return reject(&arm, 147);
    }
    return true;
}

bool Daemon::handle_activity(Client *client)
{
    assert(client->status != Client::TOCOMPILE && client->status != Client::WAITINSTALL);

    Msg *msg = client->channel->get_msg(0, true);

    if (!msg) {
        // get_msg() rejects malformed P50_SOURCE_ARM frames before exposing a
        // Msg, but preserves a complete assignment triple when the frame
        // carried one.  Settle that exact owner before closing; a later
        // malformed/replayed frame on an already-armed client falls back to
        // its retained owner token.  Partial/random identities remain unable
        // to settle anything.
        uint32_t invalid_wire_id = 0;
        uint64_t invalid_epoch = 0;
        uint64_t invalid_nonce = 0;
        bool exact_invalid_owner = false;
        const bool retained_source_owner = client->p50_source_arm_fields.has_value();
        if (retained_source_owner) {
            // A malformed/replayed frame on an armed wrapper cannot redirect
            // teardown to another assignment.  The retained full owner is
            // the only scheduler token this connection may settle.
            const P50SourceArmFields& retained = *client->p50_source_arm_fields;
            exact_invalid_owner = bind_source_assignment_for_settlement(
                retained, static_cast<uint32_t>(client->client_id));
            if (exact_invalid_owner) {
                client->job_id = retained.wire_job_id;
                client->last_known_job_id = retained.wire_job_id;
            }
            (void)client->channel->take_invalid_p50_source_arm_identity(
                &invalid_wire_id, &invalid_epoch, &invalid_nonce);
        } else if (client->channel->take_invalid_p50_source_arm_identity(
                       &invalid_wire_id, &invalid_epoch, &invalid_nonce)) {
            P50SourceArmFields identity;
            identity.wire_job_id = invalid_wire_id;
            identity.assignment_epoch = invalid_epoch;
            identity.assignment_nonce = invalid_nonce;
            exact_invalid_owner = bind_source_assignment_for_settlement(
                identity, static_cast<uint32_t>(client->client_id));
            if (exact_invalid_owner) {
                client->job_id = invalid_wire_id;
                client->last_known_job_id = invalid_wire_id;
            }
        }
        if (exact_invalid_owner) {
            client->set_status(Client::WAITP50INPUT,
                               "p50: malformed frame settled exact owner");
            finish_assignment_claim(client->job_id);
        }
        handle_end(client, 118);
        return false;
    }

    if (client->pending_p51_source_lease) {
        delete msg;
        log_warning() << "P51 wrapper sent another frame before its source lease completed"
                      << endl;
        handle_end(client, 151);
        return false;
    }

    // Recheck the one absolute F-local deadline before every later ordinary
    // message.  No CompileFile/arm replay can renew this nonrenewable lease.
    if (client->p50_source_arm_fields.has_value() &&
        !client->source_budget_live()) {
        delete msg;
        handle_end(client, 149);
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
    case Msg::CACHE_SESSION:
        ret = handle_cache_session(client, msg);
        break;
    case Msg::P50_CACHE_SESSION_FD_REQUEST:
        ret = handle_p50_cache_session_fd_request(
            client, dynamic_cast<P50CacheSessionFdRequestMsg *>(msg));
        break;
    case Msg::P51_SOURCE_LEASE_REQUEST:
        ret = handle_p51_source_lease_request(
            client, dynamic_cast<P51SourceLeaseRequestMsg *>(msg));
        break;
    case Msg::P51_CACHE_LINK_SESSION:
        ret = handle_p51_cache_link_session(
            client, dynamic_cast<P51CacheLinkSessionMsg *>(msg));
        break;
    case Msg::P50_SOURCE_ARM:
        ret = handle_p50_source_arm(client,
                                    dynamic_cast<P50SourceArmMsg *>(msg));
        break;
    case Msg::P51_SOURCE_ARM:
        ret = handle_p51_source_arm(
            client, dynamic_cast<P51SourceArmMsg *>(msg));
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

void Daemon::clear_pending_client_admissions() noexcept
{
    pending_client_admissions.clear();
}

uint64_t Daemon::next_pending_client_admission_deadline_msec() const noexcept
{
    uint64_t earliest = 0;
    for (const auto &entry : pending_client_admissions) {
        const uint64_t deadline = entry.second.deadline_msec;
        if (deadline != 0 && (earliest == 0 || deadline < earliest))
            earliest = deadline;
    }
    return earliest;
}

bool Daemon::expire_pending_client_admissions() noexcept
{
    const uint64_t now_msec = monotonic_msec();
    bool changed = false;
    for (auto it = pending_client_admissions.begin();
         it != pending_client_admissions.end();) {
        if (it->second.deadline_msec != 0 &&
            now_msec >= it->second.deadline_msec) {
            trace() << "protocol admission timeout on fd " << it->first << endl;
            it = pending_client_admissions.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

void Daemon::service_pending_client_admissions(
    const vector<pollfd> &pollfds)
{
    for (auto it = pending_client_admissions.begin();
         it != pending_client_admissions.end();) {
        auto current = it++;
        const int fd = current->first;
        PendingClientAdmission &admission = current->second;
        MsgChannel *channel = admission.channel.get();
        short revents = 0;
        for (const pollfd &descriptor : pollfds) {
            if (descriptor.fd == fd) {
                revents = descriptor.revents;
                break;
            }
        }
        if (revents == 0)
            continue;

        bool alive = (revents & POLLNVAL) == 0;
        if (alive && (revents & POLLOUT) != 0 &&
            channel->has_pending_write()) {
            alive = channel->flush_pending();
        }
        if (alive &&
            (revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            alive = channel->read_a_bit();
        }

        MsgChannel::ProtocolAdmissionState state =
            channel->protocol_admission_state();
        if (!alive)
            state = MsgChannel::ProtocolAdmissionState::Failed;
        if (state == MsgChannel::ProtocolAdmissionState::Pending)
            continue;
        if (state == MsgChannel::ProtocolAdmissionState::Failed) {
            trace() << "protocol admission failed on fd " << fd << endl;
            pending_client_admissions.erase(current);
            continue;
        }

        // Remove pending ownership before exposing the channel.  No ordinary
        // handler can observe NEED_PROTO or a queued handshake reply.
        PendingClientAdmission completed = std::move(current->second);
        pending_client_admissions.erase(current);
        channel = completed.channel.get();
        if (!channel->finish_protocol_admission())
            continue;

        channel->set_p50_legacy_wire_role(P50LegacyWireRole::F);
        Client *client = new Client;
        client->client_id = ++new_client_id;
        client->channel = completed.channel.release();
        if (auto provenance = connection_leases.allocate(
                completed.listener_kind, completed.peer_credentials)) {
            client->connection_provenance = *provenance;
            if (!connection_leases.bind(
                    provenance->lease, client, client->channel)) {
                connection_leases.cancel(provenance->lease);
                client->connection_provenance = ConnectionProvenance{};
            }
        }
        clients[client->channel] = client;
        fd2client[client->channel->fd] = client;
        trace() << "accepted " << client->channel->fd << " "
                << client->channel->name << " as " << client->client_id
                << endl;
    }
}

void Daemon::service_pending_client_admissions_now()
{
    if (pending_client_admissions.empty())
        return;

    vector<pollfd> pollfds;
    pollfds.reserve(pending_client_admissions.size());
    for (const auto &entry : pending_client_admissions) {
        pollfd pfd{};
        pfd.fd = entry.first;
        pfd.events = POLLIN | POLLHUP | POLLERR;
        if (entry.second.channel->has_pending_write())
            pfd.events |= POLLOUT;
        pollfds.push_back(pfd);
    }

    const int result = poll(pollfds.data(), pollfds.size(), 0);
    if (result < 0) {
        if (errno != EINTR)
            log_perror("poll pending client admissions");
        return;
    }
    if (result != 0)
        service_pending_client_admissions(pollfds);
}

void Daemon::answer_client_requests()
{
    if (expire_scheduler_output())
        return;
#ifdef ICECC_DEBUG

    if (clients.size() + current_kids) {
        log_info() << dump_internals() << endl;
    }

    log_info() << "clients " << clients.dump_per_status() << " " << current_kids
               << " (" << max_kids << ")" << endl;

#endif

    // Deadlines are owned by this event loop, not by fd readiness.  Sweep
    // before constructing pollfds so a silent peer is closed even when no
    // descriptor event is pending.
    if (expire_pending_client_admissions())
        return;
    if (expire_p50_source_waiters()) {
        return;
    }

    /* Sidecar wait status is consumed only by cache_child_reaper after its
       exact pidfd has become readable below.  There is deliberately no
       anonymous child sweep: an anonymous reap could steal a direct worker and
       misattribute PID reuse to the lifecycle.  Generic daemon children are
       also probed by their registered PID, one fair record per turn. */
    {
        static size_t child_reap_cursor = 0;
        if (!child_registry.empty()) {
            if (child_reap_cursor >= child_registry.size())
                child_reap_cursor = 0;
            auto iterator = child_registry.begin();
            std::advance(iterator, static_cast<long>(child_reap_cursor));
            const pid_t registered_pid = iterator->first;
            const bool lifecycle_complete =
                iterator->second.completion_observed;
            bool erased = false;
            if (iterator->second.session_quiescence_target) {
                // The persistent session barrier is the sole exact-reaper
                // owner for this snapshot record.
            } else if (iterator->second.kind == ChildRecord::COMPILER) {
                ChildRecord &record = iterator->second;
                if (!lifecycle_complete) {
                    /* Preserve the exact leader waitable.  Scheduler-loss
                       quiescence may still need this anchor.  If the leader
                       has already crashed, use that retained anchor now to
                       kill any descendant that could otherwise keep the
                       completion pipe open forever. */
                    const icecc::daemon_child::CleanupAdvance cleanup =
                        icecc::daemon_child::advance_exited_group_cleanup(
                            record.signal, record.pid, record.pgid,
                            child_signal_operations);
                    if (cleanup.final_signal.invoked) {
                        record.state = ChildRecord::KILL_SENT;
                        log_info()
                            << "orphaned compiler cleanup KILL pid="
                            << record.pid << " pgid=" << record.pgid
                            << " generation=" << record.session_generation
                            << " errno=" << cleanup.final_signal.error << endl;
                    }
                } else if (record.slot.active) {
                    /* Completion and slot release must be one transaction.
                       Never erase cleanup ownership for an unaccounted slot. */
                    log_error() << "completed compiler pid " << record.pid
                                << " retains a capacity slot; failing closed"
                                << endl;
                    child_ownership_gate.mark_sticky_failure();
                } else {
                    const icecc::daemon_child::CleanupAdvance cleanup =
                        icecc::daemon_child::advance_exited_group_cleanup(
                            record.signal, record.pid, record.pgid,
                            child_signal_operations);
                    if (cleanup.final_signal.invoked) {
                        record.state = ChildRecord::KILL_SENT;
                        log_info()
                            << "completed compiler cleanup KILL pid="
                            << record.pid << " pgid=" << record.pgid
                            << " generation=" << record.session_generation
                            << " errno=" << cleanup.final_signal.error << endl;
                    }
                    if (cleanup.settled) {
                        log_info() << "completed compiler cleanup settled pid="
                                   << record.pid << " pgid=" << record.pgid
                                   << " generation=" << record.session_generation
                                   << endl;
                        child_registry.erase(iterator);
                        erased = true;
                    }
                }
            } else {
                int status = 0;
                const pid_t result = waitpid(registered_pid, &status, WNOHANG);
                if (result == registered_pid ||
                        (result < 0 && errno == ECHILD && lifecycle_complete)) {
                    child_registry.erase(iterator);
                    erased = true;
                } else if (result < 0 && errno == ECHILD) {
                    iterator->second.signal.active = false;
                    iterator->second.signal.leader_consumed = true;
                    iterator->second.state = ChildRecord::REAPED;
                }
            }
            if (!child_registry.empty()) {
                child_reap_cursor = erased
                    ? child_reap_cursor % child_registry.size()
                    : (child_reap_cursor + 1) % child_registry.size();
            }
        }
    }

    /* Push queued state records into the writer pipe (nonblocking; no-op
       when nothing is queued or the pipe is full).  */
    /* Poll the writer's liveness beside the pump: the generic child sweep
       sweep above reaps a dead writer like any other child but cannot
       reset the StateWriter's pid, so without this call started() stayed
       true, telemetry reported writer_alive:true, and records were
       accepted only to be dropped -- a silent telemetry blackout until
       restart (issue #3).  alive() is one WNOHANG waitpid and tolerates
       the sweep having reaped first (ECHILD).  */
    state_writer.alive();
    state_writer.pump();

    /* G4: any boundary this turn that loses the session funnels through the
       single exactly-once finish_scheduler_loss_if_needed(), which consumes the
       pending-loss token close_scheduler() records at loss time. */
    handle_old_request();
    if (finish_scheduler_loss_if_needed()) {
        return;
    }

    /* collect the stats after the children exited icecream_load; only on an
       ACTIVE session -- do not send stats on a pending (LOGIN_ATTEMPT) channel. */
    if (scheduler_session_active) {
        maybe_stats();
    }
    if (finish_scheduler_loss_if_needed()) {
        return;    /* a STATS send can close the established session */
    }

    vector< pollfd > pollfds;
    pollfds.reserve(fd2client.size() + pending_client_admissions.size() + 6);
    pollfd pfd; // tmp varible

    const size_t pending_admission_limit = pending_client_admission_limit();
    const size_t pending_remote_limit =
        pending_remote_admission_limit(pending_admission_limit);
    size_t pending_remote_count = 0;
    for (const auto &entry : pending_client_admissions) {
        if (entry.second.listener_kind == ListenerKind::TcpRemote)
            ++pending_remote_count;
    }
    const bool admission_enabled =
        !session_quiescence_pending() &&
        !child_ownership_gate.admission_blocked();
    const bool any_client_admission_capacity = admission_enabled &&
        pending_client_admissions.size() < pending_admission_limit;
    const bool remote_client_admission_capacity =
        any_client_admission_capacity &&
        pending_remote_count < pending_remote_limit;
    if (remote_client_admission_capacity && tcp_listen_fd != -1) {
        pfd.fd = tcp_listen_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }
    if (any_client_admission_capacity && tcp_listen_local_fd != -1) {
        pfd.fd = tcp_listen_local_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

    if (any_client_admission_capacity && unix_listen_fd != -1) {
        pfd.fd = unix_listen_fd;
        pfd.events = POLLIN;
        pollfds.push_back(pfd);
    }

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

    for (const auto &it : pending_client_admissions) {
        pfd.fd = it.first;
        pfd.events = POLLIN | POLLHUP | POLLERR;
        if (it.second.channel->has_pending_write())
            pfd.events |= POLLOUT;
        pollfds.push_back(pfd);
    }

    bool p51_connect_completion_pending = false;
    for (const auto &entry : clients) {
        const Client *client = entry.second;
        if (!client->pending_p51_source_lease)
            continue;
        const auto &pending = *client->pending_p51_source_lease;
        p51_connect_completion_pending |=
            pending.connect && pending.connect->done();
        if (pending.connect && pending.connect->poll_fd() >= 0)
            pollfds.push_back(pollfd{pending.connect->poll_fd(),
                                     pending.connect->poll_events(), 0});
        else if (pending.control && pending.frame)
            pollfds.push_back(pollfd{pending.control->native_handle(),
                                     pending.frame->poll_events(), 0});
    }
    for (const auto &entry : clients) {
        const Client *client = entry.second;
        if (!client->pending_p51_source_arm)
            continue;
        const auto &pending = *client->pending_p51_source_arm;
        p51_connect_completion_pending |=
            pending.connect && pending.connect->done();
        if (pending.connect && pending.connect->poll_fd() >= 0)
            pollfds.push_back(pollfd{pending.connect->poll_fd(),
                                     pending.connect->poll_events(), 0});
        else if (pending.control && pending.frame)
            pollfds.push_back(pollfd{pending.control->native_handle(),
                                     pending.frame->poll_events(), 0});
    }
    for (const auto& pending : pending_p51_source_cancels) {
        p51_connect_completion_pending |=
            pending->connect && pending->connect->done();
        if (pending->connect && pending->connect->poll_fd() >= 0)
            pollfds.push_back(pollfd{pending->connect->poll_fd(),
                                     pending->connect->poll_events(), 0});
        else if (pending->control && pending->frame)
            pollfds.push_back(pollfd{pending->control->native_handle(),
                                     pending->frame->poll_events(), 0});
    }
    for (const auto& pending : orphaned_p51_source_arms) {
        if (pending->control && pending->frame)
            pollfds.push_back(pollfd{pending->control->native_handle(),
                                     pending->frame->poll_events(), 0});
    }

    /* G4 (16:47#1): set when a client had more than one complete message already
       parsed into its channel's userspace buffer; fd readiness will not re-fire
       for bytes drained out of the kernel, so we force a zero-timeout poll below
       to process the remainder promptly. */
    bool buffered_client_pending = false;
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
        bool select_channel = (current_status == Client::TOCOMPILE);
        if (!select_channel && !ignore_channel) {
            if (!c->has_msg()) {
                select_channel = true;
            } else {
                /* Process the buffered message now.  G4: that handler can send
                   to S and close the session -- check loss before any later
                   client or poll-set work, and never dereference a client the
                   handler deleted. */
                const bool alive = handle_activity(client);
                if (finish_scheduler_loss_if_needed()) {
                    return;
                }
                if (!alive) {
                    continue;   /* client was deleted by the handler */
                }
                /* G4 (16:47#1): recompute eligibility from the NEW state.  The
                   handler may have moved the client to WAITFORCHILD/WAITINSTALL
                   -- the two states this loop deliberately ignores -- and a
                   readable WAITINSTALL client whose fd we register can reach the
                   post-poll assertion that forbids it.  A blind
                   select_channel = true is wrong. */
                current_status = client->status;
                const bool now_ignore = current_status == Client::WAITFORCHILD
                                        || current_status == Client::WAITINSTALL;
                select_channel = (current_status == Client::TOCOMPILE) || !now_ignore;
                if (c->has_msg()) {
                    buffered_client_pending = true;   /* more parsed bytes remain */
                }
            }
        }

        if (select_channel) {
            pfd.fd = i;
            pfd.events = current_status == Client::WAITP50INPUT
                ? (POLLIN | POLLHUP | POLLERR)
                : POLLIN;
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
        if (scheduler->has_pending_write())
            pfd.events |= POLLOUT;
        pollfds.push_back(pfd);
    } else if (discover && discover->connect_fd() >= 0) {
        // A direct -s scheduler endpoint uses a nonblocking TCP connect.  Its
        // socket is writable when connect completion (success or failure) is
        // ready; polling only listen_fd() skips this socket entirely and can
        // strand scheduler rediscovery until its timeout.
        pfd.fd = discover->connect_fd();
        pfd.events = POLLOUT;
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

    // The sidecar lifecycle contributes its private pipes, exact pidfd, and
    // any authenticated control socket to this same poll inventory.  No
    // nested poll or synchronous sidecar helper is allowed to hide here.
    if (cache_adapter != nullptr)
        cache_adapter->outer_append_pollfds(pollfds);

    int poll_timeout_msec = max_scheduler_pong * 1000;
    const uint64_t quiescence_wakeup =
        next_session_quiescence_wakeup_msec();
    if (quiescence_wakeup != 0) {
        const uint64_t now = monotonic_msec();
        const int timeout = quiescence_wakeup > now
            ? static_cast<int>(std::min<uint64_t>(quiescence_wakeup - now,
                  std::numeric_limits<int>::max())) : 0;
        if (poll_timeout_msec < 0 || timeout < poll_timeout_msec)
            poll_timeout_msec = timeout;
    }
    if (scheduler && scheduler->deferred_output_armed()) {
        const uint64_t now = monotonic_msec();
        const uint64_t deadline = scheduler->deferred_output_deadline_msec();
        const int remaining = deadline > now
            ? static_cast<int>(std::min<uint64_t>(deadline - now,
                  std::numeric_limits<int>::max())) : 0;
        if (poll_timeout_msec < 0 || remaining < poll_timeout_msec)
            poll_timeout_msec = remaining;
    }
    for (const auto& entry : clients) {
        const auto &lease = entry.second->pending_p51_source_lease;
        if (lease) {
            auto wakeup = lease->deadline;
            if (lease->connect)
                wakeup = std::min(wakeup, lease->connect->next_wakeup());
            const auto remaining = std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    wakeup - std::chrono::steady_clock::now()).count();
            const int timeout = static_cast<int>(std::clamp<int64_t>(
                remaining, 0, std::numeric_limits<int>::max()));
            if (poll_timeout_msec < 0 || timeout < poll_timeout_msec)
                poll_timeout_msec = timeout;
        }
        const auto& attachment = entry.second->p50_attachment;
        if (!attachment) continue;
        if (attachment->poll_fd() >= 0)
            pollfds.push_back(pollfd{attachment->poll_fd(), attachment->poll_events(), 0});
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            attachment->next_wakeup() - std::chrono::steady_clock::now()).count();
        const int timeout = static_cast<int>(std::clamp<int64_t>(
            remaining, 0, std::numeric_limits<int>::max()));
        if (poll_timeout_msec < 0 || timeout < poll_timeout_msec)
            poll_timeout_msec = timeout;
    }
    for (const auto& pending : pending_p51_source_cancels) {
        auto wakeup = pending->deadline;
        if (pending->connect)
            wakeup = std::min(wakeup, pending->connect->next_wakeup());
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                wakeup - std::chrono::steady_clock::now()).count();
        const int timeout = static_cast<int>(std::clamp<int64_t>(
            remaining, 0, std::numeric_limits<int>::max()));
        if (poll_timeout_msec < 0 || timeout < poll_timeout_msec)
            poll_timeout_msec = timeout;
    }
    for (const auto& pending : orphaned_p51_source_arms) {
        auto wakeup = pending->deadline;
        if (pending->connect)
            wakeup = std::min(wakeup, pending->connect->next_wakeup());
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                wakeup - std::chrono::steady_clock::now()).count();
        const int timeout = static_cast<int>(std::clamp<int64_t>(
            remaining, 0, std::numeric_limits<int>::max()));
        if (poll_timeout_msec < 0 || timeout < poll_timeout_msec)
            poll_timeout_msec = timeout;
    }
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

    /* G4 (17:20#3): while a LOGIN_ATTEMPT is pending on a modern (>=24) channel,
       never sleep past its activation deadline; otherwise a scheduler that
       accepts Login and then goes silent (no ConfCS, no other daemon activity)
       would block poll for up to max_scheduler_pong seconds and reconnect()
       could never enforce the deadline.  Cap at the time remaining (>= 0). */
    if (scheduler_login_pending && !scheduler_session_active
            && scheduler_login_deadline_msec != 0
            && scheduler && scheduler->protocol >= 24) {
        const uint64_t now = monotonic_msec();
        const int to_deadline = (scheduler_login_deadline_msec > now)
                                ? int(scheduler_login_deadline_msec - now) : 0;
        if (poll_timeout_msec < 0 || to_deadline < poll_timeout_msec) {
            poll_timeout_msec = to_deadline;
        }
    }

    // Pending protocol handshakes retain the historical 15-second per-peer
    // lifetime, but the shared loop sleeps only until the earliest deadline.
    const uint64_t protocol_deadline_msec =
        next_pending_client_admission_deadline_msec();
    if (protocol_deadline_msec != 0) {
        const uint64_t now = monotonic_msec();
        const int to_protocol_deadline = protocol_deadline_msec > now
            ? static_cast<int>(std::min<uint64_t>(
                  protocol_deadline_msec - now,
                  std::numeric_limits<int>::max()))
            : 0;
        if (poll_timeout_msec < 0 ||
            to_protocol_deadline < poll_timeout_msec) {
            poll_timeout_msec = to_protocol_deadline;
        }
    }

    // A live source owner has one nonrenewable absolute deadline.  Cap poll
    // by the earliest such owner so traffic cannot make the loop sleep past
    // expiry; the explicit sweep above/after poll performs the actual close.
    const uint64_t source_deadline_msec = next_p50_source_deadline_msec();
    if (source_deadline_msec != 0) {
        const uint64_t now = monotonic_msec();
        const int to_source_deadline = source_deadline_msec > now
            ? static_cast<int>(std::min<uint64_t>(
                  source_deadline_msec - now, std::numeric_limits<int>::max()))
            : 0;
        if (poll_timeout_msec < 0 || to_source_deadline < poll_timeout_msec) {
            poll_timeout_msec = to_source_deadline;
        }
    }

    if (cache_adapter != nullptr) {
        const auto lifecycle_deadline = cache_adapter->outer_next_deadline();
        if (lifecycle_deadline != std::chrono::steady_clock::time_point{}) {
            const auto now = std::chrono::steady_clock::now();
            const auto remaining = lifecycle_deadline > now
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      lifecycle_deadline - now).count()
                : 0;
            const int lifecycle_timeout = static_cast<int>(std::min<long long>(
                std::max<long long>(remaining, 0), std::numeric_limits<int>::max()));
            if (poll_timeout_msec < 0 || lifecycle_timeout < poll_timeout_msec)
                poll_timeout_msec = lifecycle_timeout;
        }
    }

    /* G4 (16:47#1): bytes already parsed into a channel buffer won't re-trigger
       fd readiness -- process the remainder on a zero-timeout pass. */
    if (buffered_client_pending) {
        poll_timeout_msec = 0;
    }
    // UnixConnectOperation may complete synchronously in its initial
    // advance(). A completed operation has no poll fd; give its owner state
    // machine another turn before a blocking poll can outlive the sidecar's
    // bounded handshake budget.
    if (p51_connect_completion_pending)
        poll_timeout_msec = 0;
    /* An armed sidecar launch/cleanup plan advances exactly one bounded
       action per turn. Grant zero-timeout turns while already-admitted work
       can progress, so the finite plan reaches fork or exact teardown before
       its absolute deadline instead of expiring between quiet polls. */
    if (cache_adapter != nullptr && cache_adapter_start_attempted &&
        cache_adapter->outer_immediate_turn_required()) {
        poll_timeout_msec = 0;
    }

    int ret = poll(pollfds.data(), pollfds.size(), poll_timeout_msec);

    if (expire_scheduler_output())
        return;
    if (ret < 0 && errno != EINTR) {
        log_perror("poll");
        close_scheduler();
        finish_scheduler_loss_if_needed();
        return;
    }
    if (expire_pending_client_admissions())
        return;
    // Poll may return zero for the capped deadline, or may return another fd
    // at the same instant.  Re-run the exact owner sweep before touching any
    // revents so an expired owner cannot consume a late frame.
    if (expire_p50_source_waiters()) {
        return;
    }
    if (advance_orphaned_p51_source_arms(pollfds)) {
        finish_scheduler_loss_if_needed();
        return;
    }
    if (advance_p51_source_cancels(pollfds)) {
        finish_scheduler_loss_if_needed();
        return;
    }
    if (advance_p51_source_leases(pollfds)) {
        finish_scheduler_loss_if_needed();
        return;
    }
    if (advance_p51_source_arms(pollfds)) {
        finish_scheduler_loss_if_needed();
        return;
    }
    if (cache_adapter != nullptr && cache_adapter_start_attempted) {
        static unsigned advance_trace_budget = 24;
        if (advance_trace_budget > 0) {
            --advance_trace_budget;
            trace() << "cache lifecycle turn: action_taken="
                    << cache_adapter->outer_action_taken() << " lifecycle="
                    << int(static_cast<uint8_t>(
                           cache_adapter->outer_lifecycle_state()))
                    << " launch_phase=" << cache_adapter->outer_launch_phase_diag()
                    << " launch_failed=" << cache_adapter->outer_launch_failed_diag()
                    << endl;
        }
    }
    if (cache_adapter != nullptr && cache_adapter_start_attempted &&
        !cache_adapter->outer_action_taken()) {
        icecc::p50::advertisement::Update lifecycle_update;
        (void)cache_adapter->outer_advance_turn(
            std::chrono::steady_clock::now(), pollfds, &lifecycle_update);
        if (const auto result =
                cache_adapter->take_outer_input_lifecycle_result();
            result.has_value()) {
            const char *reason = "outer input lifecycle";
            switch (result->request.action) {
            case icecc::p50::InputLifecycleAction::CancelAttempt:
                reason = "handle_end";
                break;
            case icecc::p50::InputLifecycleAction::CloseAcceptedJob:
                reason = "submitter accepted complete result";
                break;
            case icecc::p50::InputLifecycleAction::CancelJob:
                reason = "submitter definitive cancellation";
                break;
            default:
                break;
            }
            complete_p50_input_lifecycle(*result, reason);
        }
        for (size_t index = 0; index < lifecycle_update.count; ++index) {
            if (!scheduler_session_active || scheduler == nullptr ||
                !reannounce_environments(&lifecycle_update.transitions[index]))
                return;
        }
        if (invalidate_p50_source_waiters_for_lease())
            return;
        reconcile_cache_route_state();
    }
    if (advance_p50_attachments(pollfds)) {
        finish_scheduler_loss_if_needed();
        return;
    }
    // Keep exact child status delivery after the lifecycle turn.  The central
    // registry remains the only wait-status consumer; this ordering prevents
    // one daemon turn from combining WNOWAIT/consume with another fallible
    // lifecycle action.
    if (cache_adapter != nullptr && cache_adapter_start_attempted &&
        cache_adapter->outer_pidfd() >= 0 && cache_adapter->outer_child_pid() > 1) {
        const int sidecar_pidfd = cache_adapter->outer_pidfd();
        bool pidfd_ready = false;
        for (const pollfd& descriptor : pollfds) {
            if (descriptor.fd == sidecar_pidfd &&
                (descriptor.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
                pidfd_ready = true;
                break;
            }
        }
        bool delivered = false;
        if (pidfd_ready) {
            const std::optional<icecc::p50::sidecar::ReapEvent> event =
                cache_child_reaper.reap_one(cache_adapter->outer_child_pid(),
                                             sidecar_pidfd);
            if (event.has_value()) {
                (void)cache_adapter->outer_observe_child_reaped(*event);
                delivered = true;
            }
        }
        // If exact consumption happened while the lifecycle mailbox was full,
        // pidfd readiness need not recur.  The registry owns this pure,
        // bounded publication retry and remains the only status authority.
        if (!delivered) {
            const auto event = cache_child_reaper.publish_pending();
            if (event.has_value())
                (void)cache_adapter->outer_observe_child_reaped(*event);
        }
        reconcile_cache_route_state();
    }
    // Reset debug if needed, but only if we aren't waiting for any child processes to finish,
    // otherwise their debug output could end up reset in the middle (and flush log marks used
    // by tests could be written out before debug output from children).
    if( current_kids == 0 ) {
        reset_debug_if_needed();
    }

    if (ret > 0) {
        if (scheduler && pollfd_is_set(pollfds, scheduler->fd, POLLOUT) &&
            !scheduler->flush_pending()) {
            close_scheduler();
            (void)finish_scheduler_loss_if_needed();
            return;
        }
        if (scheduler && pollfd_is_set(pollfds, scheduler->fd, POLLIN)) {
            /* A handler in this loop (e.g. scheduler_use_cs -> handle_end ->
               a failed compensating send_scheduler) can call close_scheduler()
               and null `scheduler` mid-iteration while still returning 0.  The
               loop condition must therefore re-check liveness, or the next
               `scheduler->read_a_bit()` dereferences a freed channel.  When it
               is lost, fall through to the single had_scheduler && !scheduler
               cleanup below (exactly once for the lost generation). */
            while (scheduler && (!scheduler->read_a_bit() || scheduler->has_msg())) {
                Msg *msg = scheduler->get_msg(0, true);

                if (!msg) {
                    log_warning() << "scheduler closed connection" << endl;
                    close_scheduler();
                    finish_scheduler_loss_if_needed();
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
                case Msg::ASSIGN_PREPARE:
                    ret = handle_assign_prepare(static_cast<AssignPrepareMsg *>(msg));
                    break;
                case Msg::REVOKE_BEFORE_START:
                    ret = handle_revoke_before_start(
                        static_cast<RevokeBeforeStartMsg *>(msg));
                    break;
                default:
                    log_error() << "unknown scheduler type " << msg->to_string() << endl;
                    ret = 1;
                }

                delete msg;

                if (ret) {
                    close_scheduler();
                    finish_scheduler_loss_if_needed();
                    return;
                }
            }
        }

        /* G4: the drain (a failed compensating send inside scheduler_use_cs)
           may have lost the session.  Do not process web/listener/client/
           child/env readiness from this same poll snapshot against a dead
           session -- funnel to the single cleanup and leave the turn. */
        if (finish_scheduler_loss_if_needed()) {
            return;
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

        // Progress every previously accepted protocol peer before admitting
        // another batch. This phase performs bounded nonblocking handshake IO
        // and promotion only; ordinary messages remain below the listener
        // phase, preserving admission-before-activity ordering.
        service_pending_client_admissions(pollfds);

        auto accept_client_admissions_now = [&]() {
            /* Pending handshakes above can free admission capacity after the outer
               poll snapshot was built.  A connection beyond the prior accept
               quantum may already be waiting in the kernel queue even though that
               snapshot did not arm or report its listener.  The ordinary
               listeners are nonblocking, so probe every currently admissible
               listener once; EAGAIN is the bounded no-work result. */
            pending_remote_count = 0;
            for (const auto &entry : pending_client_admissions) {
                if (entry.second.listener_kind == ListenerKind::TcpRemote)
                    ++pending_remote_count;
            }
            const bool current_any_client_admission_capacity =
                !session_quiescence_pending() &&
                !child_ownership_gate.admission_blocked() &&
                pending_client_admissions.size() < pending_admission_limit;
            const bool current_remote_client_admission_capacity =
                current_any_client_admission_capacity &&
                pending_remote_count < pending_remote_limit;

            int ready_listeners[3];
            size_t ready_listener_count = 0;
            if (tcp_listen_fd != -1 &&
                current_remote_client_admission_capacity) {
                ready_listeners[ready_listener_count++] = tcp_listen_fd;
            }
            if (tcp_listen_local_fd != -1 &&
                current_any_client_admission_capacity) {
                ready_listeners[ready_listener_count++] = tcp_listen_local_fd;
            }
            if (unix_listen_fd != -1 &&
                current_any_client_admission_capacity) {
                ready_listeners[ready_listener_count++] = unix_listen_fd;
            }

            /* One accept per outer turn lets a full compile burst spend seconds
               in the kernel listen queue while this same thread performs input
               settlement.  CacheWire source arms inherit the ordinary socket's
               transport deadline, so that queueing can destroy a valid assignment
               before application admission.  All ordinary listeners are
               nonblocking; drain a bounded batch, round-robin across listeners,
               then always service established clients/children below.

               This phase is admission-only: capture immutable accept provenance
               and register each socket for asynchronous protocol negotiation, but
               never wait for a peer or handle an ordinary client message between
               accepts. In particular P50_SOURCE_ARM,
               CACHE_SESSION, and COMPILE_FILE can all perform substantially more
               work than admission.  Interleaving that work here turns the nominal
               batch into one-at-a-time admission under load and can strand a
               valid socket in the kernel queue past its caller's absolute
               connection deadline. A newly accepted fd was not in this poll
               snapshot, so the dedicated zero-time admission pass below services
               any already-queued greeting before ordinary activity. Only after the
               complete two-way protocol exchange is it promoted into fd2client and
               exposed to the ordinary state machine. */
            bool listener_exhausted[3] = { false, false, false };
            size_t exhausted_count = 0;
            size_t accepted_count = 0;
            size_t accept_attempt_count = 0;
            while (ready_listener_count != 0 &&
                   exhausted_count < ready_listener_count &&
                   accepted_count < client_accept_batch_limit &&
                   pending_client_admissions.size() < pending_admission_limit &&
                   accept_attempt_count <
                       client_accept_batch_limit + ready_listener_count) {
                const size_t listener_index =
                    client_accept_cursor % ready_listener_count;
                client_accept_cursor =
                    (client_accept_cursor + 1) % ready_listener_count;
                if (listener_exhausted[listener_index])
                    continue;
                const int listen_fd = ready_listeners[listener_index];
                if (listen_fd == tcp_listen_fd &&
                    pending_remote_count >= pending_remote_limit) {
                    listener_exhausted[listener_index] = true;
                    ++exhausted_count;
                    continue;
                }
                struct sockaddr cli_addr;
                socklen_t cli_len = sizeof cli_addr;
                ++accept_attempt_count;
                int acc_fd = accept(listen_fd, &cli_addr, &cli_len);

                if (acc_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        listener_exhausted[listener_index] = true;
                        ++exhausted_count;
                    } else if (errno != EINTR) {
                        note_accept_error("client", errno);
                        listener_exhausted[listener_index] = true;
                        ++exhausted_count;
                    }
                } else {
                    ++accepted_count;
                    // Capture AF_UNIX credentials before any wrapper/channel
                    // setup can run.  TCP and a credential failure remain valid
                    // legacy clients; their provenance simply cannot authorize a
                    // cache handoff.
                    const ListenerKind listener_kind = classify_listener(
                        listen_fd, unix_listen_fd, tcp_listen_local_fd, tcp_listen_fd);
                    if (listener_kind == ListenerKind::TcpRemote)
                        ++pending_remote_count;
                    PeerCredentials peer_credentials;
                    if (listener_kind == ListenerKind::UnixLocal)
                        (void)capture_unix_peer_credentials(acc_fd, peer_credentials);
                    MsgChannel *channel = Service::createChannelAccepted(
                        acc_fd, &cli_addr, cli_len);
                    if (channel) {
                        PendingClientAdmission admission;
                        admission.channel.reset(channel);
                        admission.listener_kind = listener_kind;
                        admission.peer_credentials = peer_credentials;
                        admission.deadline_msec = monotonic_msec() +
                            ICECC_PROTOCOL_HANDSHAKE_TIMEOUT_MSEC;
                        pending_client_admissions.emplace(
                            channel->fd, std::move(admission));
                    }
                }
            }

            /* A socket accepted above was absent from the outer poll snapshot.
               Its peer may already have queued the complete protocol greeting,
               especially when this batch crossed client_accept_batch_limit.  Give
               those admission-only channels one zero-time readiness pass before
               established client activity.  Otherwise the first socket in the
               next accept quantum can spend a whole busy turn behind ordinary
               compile settlement and exceed a legacy peer's handshake deadline. */
            // A previously accepted peer may finish its greeting while ordinary
            // work runs. Progress it even when this probe accepted no new socket.
            service_pending_client_admissions_now();
        };

        accept_client_admissions_now();

        /* Accept readiness never suppresses already-established client or
           child readiness from the same poll snapshot. */
        {
            for (auto it = fd2client.begin(); it != fd2client.end();)  {
                /* A connection can cross into the nonblocking listen queue
                   after the stale outer snapshot.  Re-probe between ordinary
                   clients so no full ready set can postpone its protocol
                   greeting behind an unbounded number of settlements. */
                accept_client_admissions_now();
                int i = it->first;
                Client *client = it->second;
                MsgChannel *c = client->channel;
                assert(client);
                ++it;

                if (client->status == Client::WAITFORCHILD
                        && client->pipe_from_child >= 0
                        && pollfd_is_set(pollfds, client->pipe_from_child,
                                         POLLIN | POLLHUP | POLLERR)) {
                    if (!handle_compile_done(client)) {
                        finish_scheduler_loss_if_needed();
                        return;
                    }
                }
                if ((client->status == Client::TOINSTALL || client->status == Client::WAITINSTALL)
                        && client->pipe_from_child >= 0
                        && pollfd_is_set(pollfds, client->pipe_from_child, POLLIN)) {
                    if (!handle_env_install_child_done(client)) {
                        finish_scheduler_loss_if_needed();
                        return;
                    }
                }

                const bool client_event = client->status == Client::WAITP50INPUT
                    ? pollfd_is_set(pollfds, i, POLLIN | POLLHUP | POLLERR)
                    : pollfd_is_set(pollfds, i, POLLIN);
                if (client_event) {
                    if( client->status == Client::TOCOMPILE )
                    {
                        /* read as the preprocessed input is ready but don't process it and leave it to the child
                           if we didn't read it now, the client would be blocked and timed out */
                        c->read_a_bit();
                    }
                    else if (client->status == Client::WAITP50INPUT)
                    {
                        // WAIT remains registered for POLLIN/HUP/ERR.  Consume
                        // framed ordinary messages only through handle_activity:
                        // exact later CompileFile is retained pending, while
                        // legacy FileChunk, repeated/wrong arms, and EOF/HUP
                        // settle the exact owner and close.  No bytes are
                        // reinterpreted as compiler input and no fork occurs.
                        while (!c->read_a_bit() || c->has_msg()) {
                            const bool alive = handle_activity(client);
                            if (finish_scheduler_loss_if_needed()) {
                                return;
                            }
                            if (!alive)
                                break;
                            // CACHE_SESSION can transfer the channel fd while
                            // intentionally retaining this WAIT owner outside
                            // fd2client for the later ordinary CompileFile.
                            // Do not start a second read on the now-detached
                            // MsgChannel; that would turn its expected fd=-1
                            // state into a malformed-frame teardown.
                            if (client->p50_cache_session_detached || c->fd < 0)
                                break;
                            if (client->status != Client::WAITP50INPUT)
                                break;
                        }
                    }
                    else
                    {
                        assert(client->status != Client::TOCOMPILE &&
                               client->status != Client::WAITP50INPUT &&
                               client->status != Client::WAITINSTALL);

                        while (!c->read_a_bit() || c->has_msg()) {
                            const bool alive = handle_activity(client);
                            /* G4 (16:47#2): a scheduler-bound message can close S
                               mid-drain; funnel to the single cleanup before
                               draining the next already-buffered message against
                               a dead session -- the per-client check below is
                               too coarse for a multi-message drain. */
                            if (finish_scheduler_loss_if_needed()) {
                                return;
                            }
                            if (!alive) {
                                break;
                            }

                            if (client->status == Client::TOCOMPILE
                                || client->status == Client::WAITP50INPUT
                                || client->status == Client::WAITFORCHILD
                                || client->status == Client::WAITINSTALL) {
                                break;
                            }
                        }
                    }
                }
                if (finish_scheduler_loss_if_needed()) {
                    return;    /* a per-client handler closed the established session */
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

        if (finish_scheduler_loss_if_needed()) {
            return;
        }

    }
}

bool Daemon::reconnect()
{
    if (session_quiescence_pending() ||
            child_ownership_gate.admission_blocked()) {
        return false;
    }
    if (scheduler_generation_exhausted) {
        /* G4 (17:20#3): the 64-bit session-generation space is exhausted.
           Refuse to (re)establish any session rather than reuse a generation;
           close any pending attempt.  Terminal by design. */
        if (scheduler) {
            close_scheduler();
        }
        return false;
    }
    if (scheduler) {
        if (scheduler_login_pending && !scheduler_session_active
                && scheduler_login_deadline_msec != 0
                && scheduler->protocol >= 24
                && monotonic_msec() >= scheduler_login_deadline_msec) {
            /* G4 (16:45/17:20#3): the LOGIN_ATTEMPT activation lease expired --
               Login was accepted but no ConfCS committed the session.  Gated on
               protocol >= 24, where ConfCS is the activation signal; legacy
               schedulers (< 24) send no ConfCS, so the deadline never fires
               against them (no wrongful drop -- `protocol` is only >= 24 once
               the version handshake has finalized, which is well within the
               lease).  Drop only the attempt (not an established-session loss)
               and reschedule on a later loop; schedulerless local work is
               preserved. */
            log_warning() << "scheduler login activation deadline expired;"
                          << " dropping attempt" << endl;
            close_scheduler();
            return false;
        }
        return true;
    }

    if (!discover && next_scheduler_connect > time(nullptr)) {
        trace() << "Delaying reconnect." << endl;
        return false;
    }

#ifdef ICECC_DEBUG
    trace() << "reconn " << dump_internals() << endl;
#endif

    if (!discover) {
        discover = new DiscoverSched(netname, max_scheduler_pong, schedname, scheduler_port);
    } else {
        scheduler = discover->try_get_scheduler();
        if (discover->connection_failed()) {
            delete discover;
            discover = nullptr;
            schedule_scheduler_reconnect();
            log_info() << "scheduler connect failed; retry scheduled in "
                       << (next_scheduler_connect - time(nullptr))
                       << " seconds" << endl;
            return false;
        }
        if (scheduler == nullptr && discover->timed_out()) {
            delete discover;
            discover = new DiscoverSched(netname, max_scheduler_pong, schedname, scheduler_port);
        }
    }

    if (!scheduler) {
        log_warning() << "scheduler not yet found/selected." << endl;
        return false;
    }

    delete discover;
    discover = nullptr;
    /* G4: channel acquired -> LOGIN_ATTEMPT.  Do NOT commit the session
       generation or ownership here; that happens on the first ConfCS. */
    scheduler_login_pending = true;
    scheduler_session_active = false;
    scheduler_login_deadline_msec = monotonic_msec() + kLoginActivationDeadlineMsec;
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
    const icecc::p50::advertisement::Snapshot absent{};
    apply_cache_advertisement(lmsg, absent);
    lmsg.envs = available_environments(envbasedir);
    lmsg.max_kids = max_kids;
    lmsg.noremote = noremote;
    if (!send_scheduler(lmsg))
        return false;
    scheduler_cache_snapshot = absent;
    scheduler_cache_snapshot_valid = true;
    return true;
}

int Daemon::working_loop()
{
    bool cache_shutdown_started = false;
    bool shutdown_children_started = false;
    for (;;) {
        // Shutdown remains in this same daemon outer loop.  Once requested,
        // stop reconnect/advertisement work but keep answer_client_requests()
        // as the sole owner of the shared poll inventory, lifecycle deadline,
        // and exact central-reaper delivery.  No nested poll/drain loop may
        // outlive the normal scheduler/client fairness boundary.
        if (!cache_shutdown_started) {
            reconnect();
            poll_cache_adapter();
        } else if (cache_adapter != nullptr) {
            // Shutdown still owns one serialized adapter action per daemon
            // turn.  poll_cache_adapter() normally opens that quota, but it
            // is deliberately disabled once shutdown starts so it cannot
            // reconnect or republish.  Keep only the begin-turn reset here;
            // answer_client_requests() performs the one poll/advance below.
            (void)cache_adapter->outer_begin_turn(
                std::chrono::steady_clock::now(), nullptr);
        }
        answer_client_requests();
        // This remains in the ordinary loop even when answer_client_requests
        // returns early after a lifecycle boundary.
        advance_session_quiescence();
        maybe_dump_state();

        if (!cache_shutdown_started && exit_main_loop) {
            cache_shutdown_started = true;
            shutdown_cache_adapter();
        }
        if (cache_shutdown_started &&
            !shutdown_children_started &&
            (cache_adapter == nullptr || cache_adapter->outer_shutdown_complete())) {
            close_scheduler(true);   /* orderly shutdown: no established-loss token */
            clear_children();
            shutdown_children_started = true;
        }
        if (shutdown_children_started
                && session_quiescence.phase == SessionQuiescence::SETTLED
                && session_quiescence.targets.empty()
                && current_kids == 0 &&
                !child_ownership_gate.admission_blocked()) {
            // Even if cleanup exceeded its reap deadline, orderly process exit
            // waits for exact proof that every target group is settled.
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
            { "cache-service", 1, nullptr, 0},
            { "cache-runtime-dir", 1, nullptr, 0},
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
            } else if (optname == "cache-service") {
                if (optarg && *optarg && optarg[0] == '/') {
                    d.cache_service_executable = optarg;
                } else {
                    usage("Error: --cache-service requires an absolute path");
                }
            } else if (optname == "cache-runtime-dir") {
                if (optarg && *optarg && optarg[0] == '/') {
                    d.cache_runtime_directory = optarg;
                } else {
                    usage("Error: --cache-runtime-dir requires an absolute path");
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

    if (d.cache_service_executable.empty()
            != d.cache_runtime_directory.empty()) {
        usage("Error: --cache-service and --cache-runtime-dir must be supplied together");
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

    /* Start the state-writer PROCESS now: after daemonize()'s forks, before
       any job handling, while the daemon is still single-threaded.  The
       state-log sink needs a file path; when the daemon logs to stderr (no
       -l), state-log records keep the historical direct stream write, which
       is a tty/pipe in that configuration, not storage.  */
    if (!d.state_jsonl_path.empty() || (d.state_dump_log && !logfile.empty())) {
        if (!d.state_writer.start(d.state_jsonl_path,
                                  d.state_dump_log ? logfile : std::string())) {
            log_error() << "failed to start state writer process; state output disabled" << endl;
        } else {
            register_child(d.state_writer.pid(), d.state_writer.pid(),
                           ChildRecord::STATE_WRITER, 0);
        }
    }

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

    if (!d.configure_cache_adapter()) {
        return 1;
    }

    return d.working_loop();
}
