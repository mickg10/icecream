/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2004 Michael Matz <matz@suse.de>
                  2004 Stephan Kulow <coolo@suse.de>

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

#ifndef _GNU_SOURCE
// getopt_long
#define _GNU_SOURCE 1
#endif

#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <time.h>
#include <getopt.h>
#include <string>
#include <list>
#include <map>
#include <queue>
#include <algorithm>
#include <tuple>
#include <cassert>
#include <fstream>
#include <string>
#include <limits>
#include <random>
#include <set>
#include <unordered_map>
#include <stdio.h>
#include <pwd.h>
#include "../services/comm.h"
#include "../services/getifaddrs.h"
#include "../services/logging.h"
#include "../services/job.h"
#include "../services/util.h"

#include "compileserver.h"
#include "selection.h"
#include "job.h"
#include "scheduler.h"
#include "fastest.h"
#include "jobidallocator.h"

/* TODO:
   * leak check
   * are all filedescs closed when done?
   * simplify lifetime of the various structures (Jobs/Channels/CompileServers know
     of each other and sometimes take over ownership)
 */

/* TODO:
  - iron out differences in code size between architectures
   + ia64/i686: 1.63
   + x86_64/i686: 1.48
   + ppc/i686: 1.22
   + ppc64/i686: 1.59
  (missing data for others atm)
*/

/* The typical flow of messages for a remote job should be like this:
     prereq: daemon is connected to scheduler
     * client does GET_CS
     * request gets queued
     * request gets handled
     * scheduler sends USE_CS
     * client asks remote daemon
     * daemon sends JOB_BEGIN
     * client sends END + closes connection
     * daemon sends JOB_DONE (this can be swapped with the above one)
   This means, that iff the client somehow closes the connection we can and
   must remove all traces of jobs resulting from that client in all lists.
 */

using namespace std;

static string pidFilePath;

static map<int, CompileServer *> fd2cs;
static volatile sig_atomic_t exit_main_loop = false;

time_t starttime;
time_t last_announce;
static string scheduler_interface = "";
static unsigned int scheduler_port = 8765;

// A subset of connected_hosts representing the compiler servers
static list<CompileServer *> css;
static list<CompileServer *> monitors;
static list<CompileServer *> controls;
static list<string> block_css;
static map<unsigned int, Job *> jobs;

struct SchedulerAssignmentKey {
    uint64_t epoch;
    uint32_t wire_id;
    uint64_t nonce;

    bool operator==(const SchedulerAssignmentKey &other) const
    {
        return epoch == other.epoch && wire_id == other.wire_id
            && nonce == other.nonce;
    }
};

struct SchedulerAssignmentKeyHash {
    size_t operator()(const SchedulerAssignmentKey &key) const
    {
        size_t h = std::hash<uint64_t>()(key.epoch);
        h ^= std::hash<uint64_t>()(key.nonce) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>()(key.wire_id) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

/* Control replies use the full identity as their expected-O(1) lookup key;
   the historical jobs map remains the wire-id index for legacy traffic. */
static std::unordered_map<SchedulerAssignmentKey, Job *,
                          SchedulerAssignmentKeyHash> fenced_assignments;

/* Assignment fencing is an explicit scheduler policy and stays LEGACY by
   default.  Link versions constrain eligibility but never select a mode. */
static ConfCSMsg::FenceMode assignment_fence_mode = ConfCSMsg::Legacy;
static uint64_t scheduler_assignment_epoch = 0;
static uint64_t next_tu_seq = 0;

static bool assignment_mode_prepares()
{
    return assignment_fence_mode == ConfCSMsg::Advisory
        || assignment_fence_mode == ConfCSMsg::EnforcingCompat
        || assignment_fence_mode == ConfCSMsg::StrictNonce;
}

static uint64_t fresh_assignment_word()
{
    static std::mt19937_64 generator([] {
        std::random_device rd;
        std::seed_seq seed { rd(), rd(), rd(), rd(),
                             uint32_t(getpid()), uint32_t(time(nullptr)) };
        return std::mt19937_64(seed);
    }());
    uint64_t value = 0;
    while (value == 0) {
        value = generator();
    }
    return value;
}

static uint64_t fresh_assignment_nonce()
{
    /* One scheduler thread owns this cursor.  The randomized starting point
       avoids a fixed wire pattern; monotonic allocation guarantees no reuse
       within an epoch.  Exhaustion is terminal rather than wrapping onto a
       previously issued identity. */
    static uint64_t cursor = fresh_assignment_word();
    static uint64_t issued = 0;
    if (issued == std::numeric_limits<uint64_t>::max()) {
        log_error() << "protocol-49 assignment nonce domain exhausted" << endl;
        abort();
    }
    do {
        ++cursor;
    } while (cursor == 0);
    ++issued;
    return cursor;
}

static ConfCSMsg scheduler_conf_message(const CompileServer *peer)
{
    /* A mixed fleet remains connectable in explicit STRICT_NONCE mode, but
       protocol-49 daemons cannot accept that configuration and are never
       eligible for its remote assignments.  Project LEGACY to those peers;
       this is compatibility, not mode inference. */
    const ConfCSMsg::FenceMode projected =
        assignment_fence_mode == ConfCSMsg::StrictNonce
            && !IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_IDENTITY, peer)
        ? ConfCSMsg::Legacy
        : assignment_fence_mode;
    return ConfCSMsg(scheduler_assignment_epoch, projected);
}

static const char *assignment_mode_name(ConfCSMsg::FenceMode mode)
{
    switch (mode) {
    case ConfCSMsg::Legacy: return "legacy";
    case ConfCSMsg::Advisory: return "advisory";
    case ConfCSMsg::EnforcingCompat: return "enforcing-compat";
    case ConfCSMsg::StrictNonce: return "strict-nonce";
    }
    return "invalid";
}

/* XXX Uah.  Don't use a queue for the job requests.  It's a hell
   to delete anything out of them (for clean up).  */
// Job requests from one submitter.
/* Static selection key.  score(t) = estimate + (t - enqueue)/2, and the
   difference between any two jobs' scores has no time term, so the order is
   fully decided by K = estimate - enqueue/2.  Signed 64-bit: the monotonic
   clock and any plausible estimate are far below 2^62, so the subtraction
   cannot wrap (the review's correction to an unsigned formulation).  */
static int64_t job_static_score_key(const Job *job)
{
    return selection_static_key(job->estimateSnapshotMsec(),
                                job->enqueueMonoMsec());
}

struct JobRequestsGroup {
    list<Job *> l;
    CompileServer *submitter;
    // Priority as unix nice values 0 (highest) to 20 (lowest).
    // Values <0 are mapped to 0 (otherwise somebody could use this to starve
    // the whole cluster).
    int niceness;
    /* Score index over l: {static key, -(int64)id, job}, so *rbegin() is the
       selection winner (max key, then smallest id) in O(1).  l keeps FIFO
       order for the 60s promotion deadline -- its front is always the
       group's oldest request.  Both structures hold exactly the same jobs;
       all mutation goes through add_job()/remove_job().  */
    std::multiset<std::tuple<int64_t, int64_t, Job *>> byScore;
    void add_job(Job *);
    bool remove_job(Job *);
};
// All pending job requests, grouped by the same submitter and niceness value,
// and sorted with higher priority first.
static list<JobRequestsGroup *> job_requests;

static list<JobStat> all_job_stats;
static JobStat cum_job_stats;
/* Estimate identity: a filename suffix alone collides across repositories
   and build modes ("src/foo.cpp" is not unique), and it ignores what makes
   compile time differ -- target, environment/toolchain, language and the
   material optimization/debug flags.  Key on all of them.  */
struct RuntimeEstimate {
    uint64_t ewma_real_msec;
    uint32_t samples;
    /* icecream_monotonic_seconds(): freshness must not move with wall-clock
       steps.  */
    time_t last_update;
    RuntimeEstimate()
        : ewma_real_msec(0)
        , samples(0)
        , last_update(0) {}
};
static map<string, RuntimeEstimate> file_runtime_estimates;
static const size_t max_file_runtime_estimates = 50000;
/* Estimates older than this are treated as unusable (see the expiry note in
   estimate_job_real_msec).  */
static const time_t max_runtime_estimate_age_s = 24 * 60 * 60;
/* LRU recency list + index into it, so eviction does not scan the map.  */
static list<string> runtime_estimate_lru;
static map<string, list<string>::iterator> runtime_estimate_lru_pos;
static uint64_t runtime_estimate_hits = 0;
static uint64_t runtime_estimate_misses = 0;
static uint64_t runtime_estimate_stale = 0;
static uint64_t runtime_estimate_evictions = 0;

static float server_speed(CompileServer *cs, Job *job = nullptr, bool blockDebug = false);

/* Searches the queue for JOB and removes it.
   Returns true if something was deleted.  */
void JobRequestsGroup::add_job(Job *job)
{
    l.push_back(job);
    job->setQueueIt(--l.end());
    job->setQueued(true);
    byScore.insert(std::make_tuple(job_static_score_key(job),
                                   -(int64_t)job->id(), job));
}

bool JobRequestsGroup::remove_job(Job *job)
{
    /* Membership without scanning: a job lives in at most one group, and
       (submitter, niceness) pairs are unique across groups, so the caller's
       submitter check plus this niceness check plus the queued flag prove
       the stored iterator belongs to this->l.  */
    if (!job->queued() || niceness != job->niceness()) {
        return false;
    }
    l.erase(job->queueIt());
    byScore.erase(std::make_tuple(job_static_score_key(job),
                                  -(int64_t)job->id(), job));
    job->setQueued(false);
    return true;
}

static uint64_t default_estimated_real_msec(const Job *job)
{
    uint64_t estimate_msec = 1000;
    if (!all_job_stats.empty()) {
        estimate_msec = std::max<uint64_t>(1, uint64_t(cum_job_stats.compileTimeReal() / all_job_stats.size()));
    }

    if (job) {
        const unsigned int flags = job->argFlags();
        if (flags & CompileJob::Flag_O2 || flags & CompileJob::Flag_Ol2) {
            estimate_msec = estimate_msec * 7 / 4;
        } else if (flags & CompileJob::Flag_O) {
            estimate_msec = estimate_msec * 6 / 4;
        }
        if (flags & CompileJob::Flag_g3) {
            estimate_msec = estimate_msec * 13 / 10;
        } else if (flags & CompileJob::Flag_g) {
            estimate_msec = estimate_msec * 11 / 10;
        }
    }

    return std::max<uint64_t>(1, estimate_msec);
}

static string runtime_estimate_key(const Job *job)
{
    if (!job || job->fileName().empty()) {
        return string();
    }
    /* arg_flags carries the material optimization/debug/language bits the
       client negotiated, so the key separates -O0 from -O2 -g builds of the
       same path, and one toolchain/target from another.  */
    /* Prefer the toolchain identity persisted at dispatch; before dispatch
       (scoring time) fall back to the offer matching the target platform.
       When the two differ the estimate is recorded under the environment
       that actually ran, and scoring for such jobs falls back to the
       default estimate -- a different toolchain's runtime is not evidence
       about this one.  */
    string env = job->selectedEnvironment();
    if (env.empty()) {
        for (const auto &e : job->environments()) {
            if (e.first == job->targetPlatform()) {
                env = e.second;
                break;
            }
        }
    }
    /* Length-prefixed components: no in-band separator can be forged by a
       component that happens to contain the separator byte.  */
    ostringstream key;
    const string parts[] = { job->fileName(), job->targetPlatform(), env,
                             job->language(), toString(job->argFlags()) };
    for (const string &part : parts) {
        key << part.size() << ':' << part;
    }
    return key.str();
}

static uint64_t estimate_job_real_msec(const Job *job)
{
    const uint64_t fallback = default_estimated_real_msec(job);
    const string key = runtime_estimate_key(job);
    if (key.empty()) {
        return fallback;
    }

    const auto it = file_runtime_estimates.find(key);
    if (it == file_runtime_estimates.end() || !it->second.ewma_real_msec) {
        ++runtime_estimate_misses;
        return fallback;
    }

    /* Semantic expiry: an estimate that has not been refreshed within the
       retention window describes a build configuration that may no longer
       exist (toolchain change, different branch).  Fall back rather than
       let stale data order today's queue indefinitely.  */
    if (icecream_monotonic_seconds() - it->second.last_update > max_runtime_estimate_age_s) {
        ++runtime_estimate_stale;
        return fallback;
    }
    ++runtime_estimate_hits;

    const RuntimeEstimate &estimate = it->second;
    if (estimate.samples >= 3) {
        return std::max<uint64_t>(1, estimate.ewma_real_msec);
    }

    const uint64_t mixed = (estimate.ewma_real_msec * estimate.samples
                            + fallback * (4 - estimate.samples)) / 4;
    return std::max<uint64_t>(1, mixed);
}


static int64_t estimate_job_queue_score(const Job *job, uint64_t now_mono_msec)
{
    if (!job) {
        return 0;
    }

    /* The estimate was frozen at enqueue (see Job::estimateSnapshotMsec):
       the rank of a queued job never drifts, each decision is reproducible,
       and the scan does not touch the estimates map at all.  */
    uint64_t estimate_msec = job->estimateSnapshotMsec();
    if (!estimate_msec) {
        estimate_msec = estimate_job_real_msec(job);
    }
    return selection_score(estimate_msec, job->enqueueMonoMsec(), now_mono_msec);
}

static void add_runtime_estimate(const Job *job, unsigned long real_msec)
{
    const string key = runtime_estimate_key(job);
    if (key.empty() || !real_msec) {
        return;
    }

    RuntimeEstimate &estimate = file_runtime_estimates[key];
    if (!estimate.ewma_real_msec) {
        estimate.ewma_real_msec = real_msec;
    } else {
        estimate.ewma_real_msec = (estimate.ewma_real_msec * 7 + real_msec) / 8;
    }
    if (estimate.samples < std::numeric_limits<uint32_t>::max()) {
        ++estimate.samples;
    }
    estimate.last_update = icecream_monotonic_seconds();

    /* Touch: move (or insert) this key at the back of the recency list and
       remember where it sits, so eviction is O(1) instead of a linear scan
       of all 50,000 entries on every completed job.  */
    auto pos_it = runtime_estimate_lru_pos.find(key);
    if (pos_it != runtime_estimate_lru_pos.end()) {
        runtime_estimate_lru.erase(pos_it->second);
        pos_it->second = runtime_estimate_lru.insert(runtime_estimate_lru.end(), key);
    } else {
        runtime_estimate_lru_pos[key] =
            runtime_estimate_lru.insert(runtime_estimate_lru.end(), key);
    }

    while (file_runtime_estimates.size() > max_file_runtime_estimates
           && !runtime_estimate_lru.empty()) {
        const string evicted = runtime_estimate_lru.front();
        runtime_estimate_lru.pop_front();
        runtime_estimate_lru_pos.erase(evicted);
        file_runtime_estimates.erase(evicted);
        ++runtime_estimate_evictions;
    }
}

static void add_job_stats(Job *job, JobDoneMsg *msg)
{
    if (msg && msg->exitcode == 0 && msg->real_msec > 0) {
        add_runtime_estimate(job, msg->real_msec);
    }

    JobStat st;

    /* We don't want to base our timings on failed or too small jobs.  */
    if (msg->out_uncompressed < 4096
            || msg->exitcode != 0) {
        return;
    }

    st.setOutputSize(msg->out_uncompressed);
    st.setCompileTimeReal(msg->real_msec);
    st.setCompileTimeUser(msg->user_msec);
    st.setCompileTimeSys(msg->sys_msec);
    st.setJobId(job->id());

    if (job->argFlags() & CompileJob::Flag_g) {
        st.setOutputSize(st.outputSize() * 10 / 36);    // average over 1900 jobs: faktor 3.6 in osize
    } else if (job->argFlags() & CompileJob::Flag_g3) {
        st.setOutputSize(st.outputSize() * 10 / 45);    // average over way less jobs: factor 1.25 over -g
    }

    // the difference between the -O flags isn't as big as the one between -O0 and -O>=1
    // the numbers are actually for gcc 3.3 - but they are _very_ rough heurstics anyway)
    if (job->argFlags() & CompileJob::Flag_O
            || job->argFlags() & CompileJob::Flag_O2
            || job->argFlags() & CompileJob::Flag_Ol2) {
        st.setOutputSize(st.outputSize() * 58 / 35);
    }

    if (job->server()->lastCompiledJobs().size() >= 7) {
        /* Smooth out spikes by not allowing one job to add more than
           20% of the current speed.  */
        float this_speed = (float) st.outputSize() / (float) st.compileTimeUser();
        /* The current speed of the server, but without adjusting to the current
           job, hence no second argument.  */
        float cur_speed = server_speed(job->server());

        if ((this_speed / 1.2) > cur_speed) {
            st.setOutputSize((long unsigned) (cur_speed * 1.2 * st.compileTimeUser()));
        } else if ((this_speed * 1.2) < cur_speed) {
            st.setOutputSize((long unsigned)(cur_speed / 1.2 * st.compileTimeUser()));
        }
    }

    job->server()->appendCompiledJob(st);
    job->server()->setCumCompiled(job->server()->cumCompiled() + st);

    if (job->server()->lastCompiledJobs().size() > 200) {
        job->server()->setCumCompiled(job->server()->cumCompiled() - *job->server()->lastCompiledJobs().begin());
        job->server()->popCompiledJob();
    }

    /* The submitter can be gone when the worker reports: a client daemon
       that disconnected mid-compile has its submitter pointer detached
       (handle_end retains the started job for the worker to finish), so
       there is nothing to attribute the request stats to.  The worker-side
       stats above are what scheduling needs.  */
    if (job->submitter()) {
        job->submitter()->appendRequestedJobs(st);
        job->submitter()->setCumRequested(job->submitter()->cumRequested() + st);

        if (job->submitter()->lastRequestedJobs().size() > 200) {
            job->submitter()->setCumRequested(job->submitter()->cumRequested() - *job->submitter()->lastRequestedJobs().begin());
            job->submitter()->popRequestedJobs();
        }
    }

    all_job_stats.push_back(st);
    cum_job_stats += st;

    if (all_job_stats.size() > 2000) {
        cum_job_stats -= *all_job_stats.begin();
        all_job_stats.pop_front();
    }

#if DEBUG_SCHEDULER > 1
    if (job->argFlags() < 7000) {
        trace() << "add_job_stats " << job->language() << " "
                << (time(0) - starttime) << " "
                << st.compileTimeUser() << " "
                << (job->argFlags() & CompileJob::Flag_g ? '1' : '0')
                << (job->argFlags() & CompileJob::Flag_g3 ? '1' : '0')
                << (job->argFlags() & CompileJob::Flag_O ? '1' : '0')
                << (job->argFlags() & CompileJob::Flag_O2 ? '1' : '0')
                << (job->argFlags() & CompileJob::Flag_Ol2 ? '1' : '0')
                << " " << st.outputSize() << " " << msg->out_uncompressed << " "
                << job->server()->nodeName() << " "
                << float(msg->out_uncompressed) / st.compileTimeUser() << " "
                << server_speed(job->server(), NULL, true) << endl;
    }
#endif
}

static bool handle_end(CompileServer *cs, Msg *);

/* Semantic per-submitter dispatch credit.
   A successful send() means "accepted by the local kernel", NOT "consumed by
   the submitter daemon" -- so counting bytes cannot bound how many farm
   slots one unresponsive submitter reserves.  Instead bound the assignments
   that have been dispatched but not yet confirmed by observable progress
   (JobBeginMsg from the compile server, i.e. the client really did receive
   its UseCS and contacted that server).  A submitter at its limit is passed
   over for selection until its clients make progress; healthy submitters
   are unaffected because JobBegin normally follows dispatch within
   milliseconds.  */
static unsigned int max_outstanding_dispatches = 32;   // --max-outstanding-dispatches
/* Stale non-worker completions ignored for detached jobs -- observable
   via the internals text command so a lifecycle gate can assert the
   rejection actually happened rather than inferring it from silence.  */
static unsigned long detached_terminal_rejects = 0;
/* Begin messages ignored because the job was not awaiting one (duplicate
   or out-of-order JobBegin from the assigned worker).  */
static unsigned long nonwaiting_begin_rejects = 0;
static unsigned long duplicate_local_begin_ignored = 0;
static unsigned long id_release_violations = 0;
static unsigned long assignment_ready_accepted = 0;
static unsigned long assignment_ready_ignored = 0;
static unsigned long assignment_revoke_requested = 0;
static unsigned long assignment_terminal_accepted = 0;
static unsigned long assignment_terminal_ignored = 0;
/* Pre-login lease, population bound, and accept quantum (issue #4 P1).
   Named conservative defaults; test overrides via environment.  */
static uint64_t prelogin_lease_msec = 15000;        // T_login
static unsigned int prelogin_max_peers = 256;       // L
static unsigned int accept_quantum = 64;            // Q
static unsigned int prelogin_current = 0;
static unsigned int prelogin_max_observed = 0;
static unsigned long prelogin_expired_total = 0;
static unsigned long prelogin_rejected_total = 0;
static unsigned long prelogin_completed_total = 0;
static unsigned long accepts_deferred_total = 0;
/* G4 teardown race (bigoracle 21:47 / local-oracle 21:50): exact JobDones that
   arrive after the job was already removed by the winning side of a worker/
   submitter completion race.  Counted and ignored (return true) so the drain
   keeps consuming buffered frames; an absent job's state cannot change. */
static unsigned long late_submitter_terminal_ignores = 0;
static unsigned long late_worker_terminal_ignores = 0;
/* Set when an accept quantum was exhausted with connections still
   pending: the next poll uses a zero timeout so the listener is
   re-serviced after one full scheduler turn.  */
static bool accept_repoll = false;

/* ONE scheduler-wide legacy internals fan-out at a time (issue #4 P1,
   corrected per the round-2 re-review): the legacy STATUS_TEXT reply
   carries no request id, so concurrent fan-outs are not correlatable.
   The transaction retains ONLY stable values -- (fd, connection
   generation) keys and node-name snapshots, never a CompileServer
   pointer (a disconnected target's pointer is freed while the
   transaction lives; the first version dereferenced it when reporting).
   Every channel operation re-resolves fd2cs[fd] and requires generation
   equality.  A request is WAITING_REPLY only after its exact frame has
   FULLY FLUSHED (framesFlushed >= the recorded frame sequence) -- a
   deferrable send returning true means queued, not delivered, and an
   unsolicited status must not satisfy a request that never left the
   scheduler.  Control output is nonblocking/deferrable and bounded
   (per-target forward cap, whole-transaction retained cap, control
   pending-byte cap), and the transaction completes only when the final
   frame is known flushed (FINAL_PENDING) or the control is explicitly
   failed at the output deadline.  */
struct InternalsTarget {
    int fd;
    unsigned int generation;
    std::string node_name;
    uint64_t request_frame_seq;
    bool held = false;   /* test seam: request genuinely deferred (not sent) */
    enum State { SEND_PENDING, WAITING_REPLY, REPLIED, DISCONNECTED, TIMED_OUT } state;
};
struct InternalsTxn {
    bool active = false;
    bool final_pending = false;
    int control_fd = -1;
    unsigned int control_generation = 0;
    uint64_t deadline_mono = 0;          // reply-collection deadline
    uint64_t output_deadline_mono = 0;   // absolute control-output deadline
    uint64_t final_frame_seq = 0;
    size_t retained_bytes = 0;   // whole-transaction wire bytes emitted
    size_t peak_pending = 0;        // max control pending bytes observed (active control only)
    unsigned marker_queued = 0;     // omission markers actually QUEUED ({0,1})
    unsigned terminal_queued = 0;   // terminals actually QUEUED ({0,1})
    bool payload_omitted = false;   // any worker payload dropped/truncated
    std::vector<InternalsTarget> targets;
};
static InternalsTxn internals_txn;

/* Last-settled diagnostic snapshot: the active counters reset on clear, so
   the A.3 gate can read the settled outcome here after the transaction
   ends (peak pending, cumulative wire, {0,1} marker/terminal counts, the
   control generation, and a settlement reason).  */
struct InternalsSnapshot {
    bool valid = false;
    size_t peak_pending = 0;
    size_t retained_bytes = 0;
    unsigned marker_queued = 0;
    unsigned terminal_queued = 0;
    unsigned control_generation = 0;
    uint64_t output_deadline = 0;
    bool entered_output_phase = false;
    const char *reason = "";
};
static InternalsSnapshot internals_last;

static void internals_snapshot(const char *reason)
{
    internals_last.valid = true;
    internals_last.peak_pending = internals_txn.peak_pending;
    internals_last.retained_bytes = internals_txn.retained_bytes;
    internals_last.marker_queued = internals_txn.marker_queued;
    internals_last.terminal_queued = internals_txn.terminal_queued;
    internals_last.control_generation = internals_txn.control_generation;
    internals_last.output_deadline = internals_txn.output_deadline_mono;
    internals_last.entered_output_phase = internals_txn.final_pending;
    internals_last.reason = reason;
}
static uint64_t internals_deadline_msec = 10000;        // whole command
static bool internals_hold_armed = false;               // test-only pre-delivery seam
static unsigned long internals_output_dropped = 0;      // rows dropped by the exact bound
static const size_t kInternalsPerTargetCap = 64 * 1024;
static size_t kInternalsRetainedCap = 4 * 1024 * 1024;   /* test-overridable */
static size_t kInternalsControlPendingCap = 1024 * 1024;   /* test-overridable */

static CompileServer *internals_resolve(int fd, unsigned int generation)
{
    map<int, CompileServer *>::iterator it = fd2cs.find(fd);
    if (it == fd2cs.end()) {
        return nullptr;
    }
    if (it->second->connectionGeneration() != generation) {
        return nullptr;   /* fd reuse: not the recorded peer */
    }
    return it->second;
}

/* Exact bounded control output.  QUEUED / DROPPED / ERROR are DISTINCT:
   the earlier boolean conflated "queued" with "dropped by the bound", so a
   dropped terminal could be recorded as successfully queued.  Callers must
   treat the terminal as never-droppable (fail the control if it cannot be
   queued) and settle the control on ERROR.  Byte accounting is the EXACT
   text-channel wire length: write_line() appends the payload plus a single
   newline only if the text does not already end in one -- no length
   prefix on a text channel.  */
enum InternalsSend { ISEND_QUEUED, ISEND_DROPPED, ISEND_ERROR };

static size_t internals_wire_len(const std::string &text)
{
    return text.size() + ((text.empty() || text[text.size() - 1] != '\n') ? 1 : 0);
}


/* Overflow-safe cumulative bound: true iff have + sum(adds) would exceed
   cap, computed by reducing the remaining room one addend at a time so no
   intermediate sum can wrap.  Every internals output-bound check uses it.  */
static bool internals_exceeds(size_t have, size_t cap,
                              std::initializer_list<size_t> adds)
{
    if (have > cap) {
        return true;
    }
    size_t room = cap - have;
    for (const size_t a : adds) {
        if (a > room) {
            return true;
        }
        room -= a;
    }
    return false;
}

static InternalsSend internals_emit(CompileServer *control, const std::string &text,
                                    size_t cap)
{
    if (internals_exceeds(control->pending_bytes(), cap, { internals_wire_len(text) })) {
        ++internals_output_dropped;
        return ISEND_DROPPED;
    }
    if (!control->send_msg(TextMsg(text),
                           MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable)) {
        return ISEND_ERROR;
    }
    if (internals_txn.active
            && control->fd == internals_txn.control_fd
            && control->connectionGeneration() == internals_txn.control_generation
            && control->pending_bytes() > internals_txn.peak_pending) {
        internals_txn.peak_pending = control->pending_bytes();
    }
    return ISEND_QUEUED;
}

/* Exact still-outstanding REQUIRED output for the transaction: the
   terminal, one possible omission marker, and a fallback semantic row for
   every target not yet REPLIED (the longer "disconnected" phrasing is
   reserved so either actual phrasing fits).  Reserving this before any
   OPTIONAL worker payload is admitted makes the whole-transaction bound an
   invariant -- pending/retained + payload + required_tail <= cap -- so a
   required row in finalize can never be squeezed out by payload.  */
static const char kInternalsMarker[] =
    "[some worker output omitted: control output bound]\n";
static const char kInternalsTerm[] = "200 done";

static size_t internals_required_tail_wire()
{
    size_t need = internals_wire_len(kInternalsTerm)
                + internals_wire_len(kInternalsMarker);
    for (const InternalsTarget &t : internals_txn.targets) {
        if (t.state == InternalsTarget::REPLIED) {
            continue;
        }
        const size_t w = internals_wire_len(t.node_name + " disconnected before reporting\n");
        /* Saturating: a required tail larger than any real cap is simply
           "too big" and the caller rejects it; never wrap.  */
        need = (need > (size_t)-1 - w) ? (size_t)-1 : need + w;
    }
    return need;
}

static void internals_txn_clear(const char *reason = "cleared")
{
    if (internals_txn.active) {
        internals_snapshot(reason);
    }
    internals_txn.active = false;
    internals_txn.final_pending = false;
    internals_txn.control_fd = -1;
    internals_txn.targets.clear();
    internals_txn.retained_bytes = 0;
    internals_txn.peak_pending = 0;
    internals_txn.marker_queued = 0;
    internals_txn.terminal_queued = 0;
    internals_txn.payload_omitted = false;
}

static bool internals_txn_all_terminal()
{
    for (const InternalsTarget &t : internals_txn.targets) {
        if (t.state == InternalsTarget::SEND_PENDING
                || t.state == InternalsTarget::WAITING_REPLY) {
            return false;
        }
    }
    return true;
}

/* Queue the per-target result rows and the terminal frame to the control
   (nonblocking; bounded), then wait for the final frame to flush.  */
static void internals_txn_finalize()
{
    InternalsTxn &txn = internals_txn;
    if (!txn.active || txn.final_pending) {
        return;
    }
    CompileServer *control = internals_resolve(txn.control_fd, txn.control_generation);
    if (!control) {
        internals_txn_clear("control-lost");
        return;
    }
    /* Every required row here was reserved while optional payload was
       admitted (internals_required_tail_wire), so emitting against the
       full cap fits; a DROPPED required row would be an invariant
       violation and is still failed defensively.  */
    for (const InternalsTarget &t : txn.targets) {
        const char *why = nullptr;
        switch (t.state) {
        case InternalsTarget::REPLIED:
            continue;
        case InternalsTarget::SEND_PENDING:
        case InternalsTarget::WAITING_REPLY:
        case InternalsTarget::TIMED_OUT:
            why = " not reporting (timeout)\n";
            break;
        case InternalsTarget::DISCONNECTED:
            why = " disconnected before reporting\n";
            break;
        }
        /* SEMANTIC result rows, not optional payload: a DROP means the
           control cannot receive a complete response, so fail its exact
           generation -- never silently omit a required row.  */
        const std::string srow = t.node_name + why;
        if (internals_emit(control, srow, kInternalsControlPendingCap) != ISEND_QUEUED) {
            internals_txn_clear("required-row-drop");
            handle_end(control, nullptr);
            return;
        }
        txn.retained_bytes += internals_wire_len(srow);
    }
    /* Exactly ONE deterministic omission marker for the whole transaction
       if any worker payload was dropped or truncated -- required (not
       droppable), and the terminal reserve guarantees room.  */
    if (txn.payload_omitted) {
        if (internals_emit(control, string(kInternalsMarker),
                           kInternalsControlPendingCap) != ISEND_QUEUED) {
            internals_txn_clear("marker-drop");
            handle_end(control, nullptr);
            return;
        }
        txn.retained_bytes += internals_wire_len(kInternalsMarker);
        txn.marker_queued = 1;   /* emitted-marker count, not the condition flag */
    }
    /* The terminal is NEVER a droppable row.  If it cannot be queued the
       control cannot receive a complete response: fail its exact
       generation instead of clearing the transaction as if done.  */
    if (internals_emit(control, string(kInternalsTerm),
                       kInternalsControlPendingCap) != ISEND_QUEUED) {
        internals_txn_clear("terminal-drop");
        handle_end(control, nullptr);
        return;
    }
    txn.retained_bytes += internals_wire_len(kInternalsTerm);
    txn.terminal_queued = 1;   /* exactly-one-terminal evidence */
    txn.final_frame_seq = control->framesQueued();
    txn.final_pending = true;
}

/* Per-turn transaction service: promote flushed requests, enforce the
   collection deadline, and complete once the final frame is flushed or
   the control is failed at the output deadline.  */
static void internals_txn_tick()
{
    InternalsTxn &txn = internals_txn;
    if (!txn.active) {
        return;
    }
    const uint64_t now = icecream_monotonic_msec();
    if (txn.final_pending) {
        CompileServer *control = internals_resolve(txn.control_fd, txn.control_generation);
        if (!control) {
            internals_txn_clear("control-lost");
            return;
        }
        if (control->framesFlushed() >= txn.final_frame_seq) {
            internals_txn_clear("complete");   /* final frame delivered */
            return;
        }
        if (now >= txn.output_deadline_mono) {
            /* A control that will not read is explicitly failed.  Record
               the exact settling event (output-deadline) BEFORE handle_end
               clears the txn as control-disconnected, so the last-settled
               snapshot preserves why it ended.  */
            trace() << "internals: control output deadline expired" << endl;
            internals_txn_clear("output-deadline");
            handle_end(control, nullptr);
            return;
        }
        return;
    }
    for (InternalsTarget &t : txn.targets) {
        if (t.state != InternalsTarget::SEND_PENDING) {
            continue;
        }
        CompileServer *target = internals_resolve(t.fd, t.generation);
        if (!target) {
            t.state = InternalsTarget::DISCONNECTED;
            continue;
        }
        static const bool no_tick_promote =
            getenv("ICECC_TEST_INTERNALS_NO_TICK_PROMOTE") != nullptr;
        if (!no_tick_promote && !t.held && target->framesFlushed() >= t.request_frame_seq) {
            /* The test knob suppresses this promotion to isolate the
               STATUS_TEXT handler's same-turn recheck as the ONLY path
               that can accept a reply -- a deterministic stand-in for the
               ready-fd loop flushing the request and draining the reply in
               one turn before this tick runs.  */
            t.state = InternalsTarget::WAITING_REPLY;
        }
    }
    if (now >= txn.deadline_mono) {
        for (InternalsTarget &t : txn.targets) {
            if (t.state == InternalsTarget::SEND_PENDING
                    || t.state == InternalsTarget::WAITING_REPLY) {
                t.state = InternalsTarget::TIMED_OUT;
            }
        }
    }
    if (internals_txn_all_terminal()) {
        internals_txn_finalize();
    }
}

static void prelogin_read_overrides()
{
    /* Overrides are STRICTLY validated and clamped by explicit rule: a
       zero accept quantum would spin the loop at zero timeout forever, a
       zero peer cap would refuse every daemon, and a zero lease would
       expire handshakes instantly.  Out-of-range values are clamped to
       the nearest legal bound and reported.  */
    const char *e;
    if ((e = getenv("ICECC_TEST_PRELOGIN_LEASE_MSEC"))) {
        const long long v = atoll(e);
        prelogin_lease_msec = v < 100 ? 100 : (uint64_t)v;
        if (v < 100) {
            log_error() << "prelogin lease override " << v
                        << " clamped to 100 ms" << endl;
        }
    }
    if ((e = getenv("ICECC_TEST_PRELOGIN_MAX_PEERS"))) {
        const int v = atoi(e);
        prelogin_max_peers = v < 1 ? 1 : (unsigned int)v;
        if (v < 1) {
            log_error() << "prelogin peer-cap override " << v
                        << " clamped to 1" << endl;
        }
    }
    if ((e = getenv("ICECC_TEST_INTERNALS_PENDING_CAP"))) {
        const long long v = atoll(e);
        /* Floor at the required-tail minimum (terminal + marker) so a
           zero-target fan-out can still complete; smaller values only
           shrink the room for target rows -- exactly what the preflight
           gate needs.  */
        const size_t floor_bytes =
            internals_wire_len(kInternalsTerm) + internals_wire_len(kInternalsMarker);
        kInternalsControlPendingCap = (v > (long long)floor_bytes) ? (size_t)v : floor_bytes;
    }
    if (getenv("ICECC_TEST_INTERNALS_HOLD_FRAME")) {
        internals_hold_armed = true;
    }
    if ((e = getenv("ICECC_TEST_INTERNALS_RETAINED_CAP"))) {
        /* Independent cumulative-cap override so the A.3 stopped-reader
           gate can reach the retained bound without the lower pending cap
           masking it.  */
        const long long v = atoll(e);
        const size_t floor_bytes =
            internals_wire_len(kInternalsTerm) + internals_wire_len(kInternalsMarker);
        kInternalsRetainedCap = (v > (long long)floor_bytes) ? (size_t)v : floor_bytes;
    }
    if ((e = getenv("ICECC_TEST_ACCEPT_QUANTUM"))) {
        const int v = atoi(e);
        accept_quantum = v < 1 ? 1 : (unsigned int)v;
        if (v < 1) {
            log_error() << "accept quantum override " << v
                        << " clamped to 1 (zero would spin the loop)" << endl;
        }
    }
}

/* Exact-once release of a pre-login accounting slot.  */
static unsigned long prelogin_underflow_violations = 0;

static void prelogin_release(CompileServer *cs)
{
    if (cs->preloginAccounted()) {
        cs->setPreloginAccounted(false);
        if (prelogin_current > 0) {
            --prelogin_current;
        } else {
            /* An accounted flag with a zero population is a conservation
               violation: counted and reported, never silently floored.  */
            ++prelogin_underflow_violations;
            log_error() << "prelogin accounting underflow" << endl;
        }
    }
}

/* How long an assignment may go unconfirmed before the scheduler REPORTS
   that one of this submitter's clients is not progressing.  Reporting is
   all it does: the assignment and its worker reservation are retained,
   because only that client or its worker can truthfully end them.

   Deliberately generous: a legitimate client can wait for an environment
   install on the chosen compile server (MAX_BUSY_INSTALLING, 120s) before
   its JobBegin appears, so anything tighter would report healthy
   submitters on a cold farm.  A peer that is not draining its socket is a
   different, sharper case and keeps the 30s deferred-output bound.  */
static uint64_t max_outstanding_stall_msec = 180 * 1000;   // --dispatch-stall-report-after (seconds)

/* Remote-capable compile slots across the farm, recomputed once per main
   loop (prune_servers).  Used to clamp the per-submitter dispatch credit:
   on a farm with fewer slots than the configured credit, one submitter
   could otherwise reserve every slot before a second submitter is served.
   The clamp always leaves one slot's worth of credit for someone else.  */
static unsigned int cached_remote_farm_slots = 0;

/* Ingress cap: a GLOBAL work budget per main-loop iteration, spent across
   connections from a rotating cursor.  A per-connection cap alone still
   lets N connections do N*cap units of work before poll() runs again, and
   one GetCS message can create m->count jobs in a single handler call --
   so the budget is charged in JOBS ADMITTED (minimum one unit per
   message), and the cursor rotates so the same early connections cannot
   monopolize successive loops.  Leftovers are already parsed in userspace
   where poll() cannot see them; the caller re-polls with a zero timeout.  */
static const int max_inbound_units_per_loop = 128;
/* Per-loop-iteration ingress budget, spent at the post-poll read paths.
   Charged one unit per handled message and one per admitted job, so a
   multi-job GetCS cannot buy an unbounded amount of work for one unit.  */
static int inbound_budget_remaining = max_inbound_units_per_loop;
/* Set when a read path stops with data still buffered in userspace, where
   poll() cannot see it; consumed by the next iteration's timeout choice.  */
static bool has_buffered_inbound = false;
/* Jobs materialised from one GetCS message per loop iteration.  The rest of
   the request is NOT discarded -- discarding would silently break the wire
   contract (count is the number of UseCS replies the caller waits for).  A
   pending-expansion record carries the decoded request and a cursor, and the
   remainder is materialised on later iterations until the original count is
   fulfilled.  */
static const unsigned int max_jobs_per_expansion_step = 64;

struct PendingExpansion {
    GetCSMsg msg;                 // decoded request, retained verbatim
    unsigned int next_index;      // how many jobs have been created so far
    /* Jobs created so far, in order, NOT yet eligible for dispatch.  The
       whole sibling set is enqueued at once when the request completes: the
       master's environment narrowing at dispatch covers exactly its
       then-current sibling list, so a member admitted after the master
       dispatched -- or worse, after it completed -- would never be pinned.
       Staging recreates the released scheduler's ordering, where all N jobs
       exist before any can be selected.  Front is the master; it cannot be
       dispatched (or freed) while the record lives, so the anchor needs no
       id-based re-lookup.  On teardown the submitter sweep in handle_end
       frees these via the jobs map; the record is erased in the same call.  */
    list<Job *> staged;
    PendingExpansion(const GetCSMsg &m)
        : msg(m), next_index(0) {}
};
/* Per-connection FIFO of unfinished requests, keyed by the submitter's
   channel fd; erased on teardown (handle_end).  A deque, not a single slot:
   a daemon may send another request while one is still expanding, and each
   must be admitted in arrival order with its own full count.  */
static map<int, deque<PendingExpansion> > pending_expansions;
/* Round-robin position for resuming pending requests, so the lowest fd does
   not monopolise every leftover budget.  */
static int pending_service_cursor = -1;
/* Set when pending work found the loop budget already spent; the next
   iteration then serves pending requests FIRST, before new ingress.  Strict
   alternation under contention: new traffic cannot starve half-finished
   requests, and pending backlogs cannot starve new traffic.  */
static bool pending_expansions_starved = false;
static uint64_t jobs_admitted_total = 0;   // lifetime; exposed via 'estimates'

static bool handle_activity(CompileServer *cs);

/* Read and dispatch from one connection until it is quiet or the budget is
   spent.  Returns false when the connection is gone (caller must not touch
   it again); sets *more when work remains for the next iteration.  */
static bool drain_connection(CompileServer *cs, bool *more)
{
    while (inbound_budget_remaining > 0) {
        const int budget_before = inbound_budget_remaining;
        const bool quiet = cs->read_a_bit() && !cs->has_msg();
        if (quiet) {
            return true;
        }
        if (!handle_activity(cs)) {
            return false;          // connection deleted by the handler
        }
        /* Job admission charges the budget at the admission site itself
           (admit_request_jobs), which the resume path shares.  A message
           that admitted nothing still costs one unit -- the budget bounds
           work per loop iteration, and zero-cost messages would let a chatty
           peer hold the loop indefinitely.  */
        if (inbound_budget_remaining == budget_before) {
            --inbound_budget_remaining;
        }
    }
    /* Budget exhausted with data still pending: poll() cannot report bytes
       already sitting in userspace, so the caller re-polls with a zero
       timeout and this connection is served again next iteration.  */
    *more = true;
    return true;
}

static unsigned int effective_dispatch_credit()
{
    unsigned int credit = max_outstanding_dispatches;
    const unsigned int slots = cached_remote_farm_slots;
    if (slots > 0 && credit >= slots) {
        credit = slots > 1 ? slots - 1 : 1;
    }
    return credit;
}

static void notify_monitors(Msg *m)
{
    list<CompileServer *>::iterator it;
    list<CompileServer *>::iterator it_old;

    for (it = monitors.begin(); it != monitors.end();) {
        it_old = it++;

        /* If we can't send it, don't be clever, simply close this monitor.  */
        if (!(*it_old)->send_msg(*m, MsgChannel::SendNonBlocking /*| MsgChannel::SendBulkOnly*/)) {
            trace() << "monitor is blocking... removing" << endl;
            handle_end(*it_old, nullptr);
        }
    }

    delete m;
}

static float server_speed(CompileServer *cs, Job *job, bool blockDebug)
{
#if DEBUG_SCHEDULER <= 2
    (void)blockDebug;
#endif
    if (cs->lastCompiledJobs().size() == 0 || cs->cumCompiled().compileTimeUser() == 0) {
        return 0;
    } else {
        float f = (float)cs->cumCompiled().outputSize()
                  / (float) cs->cumCompiled().compileTimeUser();

        // we only care for the load if we're about to add a job to it
        if (job) {
            if (job->submitter() == cs) {
                int clientCount = cs->clientCount();
                if( clientCount == 0 ) {
                    // Older client/daemon that doesn't send client count. Use the number of jobs
                    // that we've already been told about as the fallback value (it will sometimes
                    // be an underestimate).
                    clientCount = cs->submittedJobsCount();
                }
                if (clientCount > cs->maxJobs()) {
                    // The submitter would be overloaded by building all its jobs locally,
                    // so penalize it heavily in order to send jobs preferably to other nodes,
                    // so that the submitter should preferably do tasks that cannot be distributed,
                    // such as linking or preparing jobs for remote nodes.
                    f *= 0.1;
#if DEBUG_SCHEDULER > 2
                    if(!blockDebug)
                        log_info() << "penalizing local build for job " << job->id() << endl;
#endif
                } else if (clientCount == cs->maxJobs()) {
                    // This means the submitter would be fully loaded by its jobs. It is still
                    // preferable to distribute the job, unless the submitter is noticeably faster.
                    f *= 0.8;
#if DEBUG_SCHEDULER > 2
                    if(!blockDebug)
                        log_info() << "slightly penalizing local build for job " << job->id() << endl;
#endif
                }
                else if (clientCount <= cs->maxJobs() / 2) {
                    // The submitter has only few jobs, slightly prefer building the job locally
                    // in order to save the overhead of distributing.
                    // Note that this is unreliable, the submitter may be in fact running a large
                    // parallel build but this is just the first of the jobs and other icecc instances
                    // haven't been launched yet. There's probably no good way to detect this reliably.
                    f *= 1.1;
#if DEBUG_SCHEDULER > 2
                    if(!blockDebug)
                        log_info() << "slightly preferring local build for job " << job->id() << endl;
#endif
                } else {
                    // the remaining case, don't adjust
                    f *= 1;
                }
                // ignoring load for submitter - assuming the load is our own
            } else {
                f *= float(1000 - cs->load()) / 1000;
            }

            /* Gradually throttle with the number of assigned jobs. This
             * takes care of the fact that not all slots are equally fast on
             * CPUs with SMT and dynamic clock ramping.
             */
            f *= (1.0f - (0.5f * cs->currentJobCount() / cs->maxJobs()));
        }

        // below we add a pessimism factor - assuming the first job a computer got is not representative
        if (cs->lastCompiledJobs().size() < 7) {
            f *= (-0.5 * cs->lastCompiledJobs().size() + 4.5);
        }

        return f;
    }
}

static void handle_monitor_stats(CompileServer *cs, StatsMsg *m = nullptr)
{
    if (monitors.empty()) {
        return;
    }

    string msg;
    char buffer[1000];
    sprintf(buffer, "Name:%s\n", cs->nodeName().c_str());
    msg += buffer;
    sprintf(buffer, "IP:%s\n", cs->name.c_str());
    msg += buffer;
    sprintf(buffer, "MaxJobs:%d\n", cs->maxJobs());
    msg += buffer;
    sprintf(buffer, "NoRemote:%s\n", cs->noRemote() ? "true" : "false");
    msg += buffer;
    sprintf(buffer, "Platform:%s\n", cs->hostPlatform().c_str());
    msg += buffer;
    sprintf(buffer, "Version:%d\n", cs->maximum_remote_protocol);
    msg += buffer;
    sprintf(buffer, "Features:%s\n", supported_features_to_string(cs->supportedFeatures()).c_str());
    msg += buffer;
    sprintf(buffer, "Speed:%f\n", server_speed(cs));
    msg += buffer;

    if (m) {
        sprintf(buffer, "Load:%d\n", m->load);
        msg += buffer;
        sprintf(buffer, "LoadAvg1:%u\n", m->loadAvg1);
        msg += buffer;
        sprintf(buffer, "LoadAvg5:%u\n", m->loadAvg5);
        msg += buffer;
        sprintf(buffer, "LoadAvg10:%u\n", m->loadAvg10);
        msg += buffer;
        sprintf(buffer, "FreeMem:%u\n", m->freeMem);
        msg += buffer;
    } else {
        sprintf(buffer, "Load:%u\n", cs->load());
        msg += buffer;
    }

    notify_monitors(new MonStatsMsg(cs->hostId(), msg));
}

/* One JobIdAllocator instance serves every scheduler-visible id --
   remote jobs AND local monitor records -- so neither class can be
   issued an id the other still holds (the old probe checked only the
   remote jobs map).  The test domain seam (ICECC_TEST_JOB_ID_DOMAIN=<n>)
   shrinks the id space for the exhaustion/reuse gates.  */
static JobIdAllocator &job_id_allocator()
{
    static JobIdAllocator alloc([]() -> uint32_t {
        const char *e = getenv("ICECC_TEST_JOB_ID_DOMAIN");
        return e ? (uint32_t)atoi(e) : 0xffffffffu;
    }());
    return alloc;
}

/* Exact removal: erase from the jobs map and release the id exactly
   once, or report the accounting violation loudly.  */
/* Checked release for LOCAL monitor ids: an ignored false return was
   an uncounted invariant failure.  */
static void release_local_id(uint32_t id)
{
    if (!job_id_allocator().release(id)) {
        ++id_release_violations;
        log_error() << "local id " << id << " released without a live"
                    << " allocation -- id accounting violated" << endl;
    }
}

static SchedulerAssignmentKey assignment_key(const Job *job)
{
    return SchedulerAssignmentKey {
        job->assignmentEpoch(), job->id(), job->assignmentNonce()
    };
}

static void index_fenced_assignment(Job *job)
{
    assert(job && job->assignmentFenced());
    const auto inserted = fenced_assignments.emplace(assignment_key(job), job);
    if (!inserted.second || inserted.first->second != job) {
        log_error() << "duplicate full P49 assignment identity for job "
                    << job->id() << endl;
        abort();
    }
}

static void unindex_fenced_assignment(Job *job)
{
    if (!job || !job->assignmentFenced()) {
        return;
    }
    auto indexed = fenced_assignments.find(assignment_key(job));
    if (indexed != fenced_assignments.end() && indexed->second == job) {
        fenced_assignments.erase(indexed);
    } else {
        log_error() << "missing full P49 assignment index for job "
                    << job->id() << endl;
    }
}

static void remove_job_entry(map<unsigned int, Job *>::iterator it)
{
    const unsigned int id = it->first;
    unindex_fenced_assignment(it->second);
    jobs.erase(it);
    if (!job_id_allocator().release(id)) {
        ++id_release_violations;
        log_error() << "job id " << id << " released without a live"
                    << " allocation -- id accounting violated" << endl;
    }
}

static Job *create_new_job(CompileServer *submitter)
{
    const unsigned int id = job_id_allocator().allocate();
    if (id == 0) {
        return nullptr;   /* explicit domain exhaustion */
    }
    Job *job = new Job(id, submitter);
    const std::pair<map<unsigned int, Job *>::iterator, bool> ins =
        jobs.insert(std::make_pair(id, job));
    if (!ins.second) {
        /* The allocator reserved it, so the map cannot hold it -- a
           collision here is an invariant failure, never an overwrite.  */
        log_error() << "job id " << id << " collided with a live job;"
                    << " refusing the allocation" << endl;
        job_id_allocator().release(id);
        delete job;
        return nullptr;
    }
    return job;
}

static void enqueue_job_request(Job *job)
{
    if (!job->estimateSnapshotMsec()) {
        job->setEstimateSnapshotMsec(estimate_job_real_msec(job));
    }
    for( list<JobRequestsGroup*>::iterator it = job_requests.begin(); it != job_requests.end(); ++it ) {
        if( (*it)->submitter == job->submitter() && (*it)->niceness == job->niceness()) {
            (*it)->add_job(job);
            return;
        }
        if( (*it)->niceness > job->niceness()) { // lower priority starts here, insert group
            JobRequestsGroup *newone = new JobRequestsGroup();
            newone->submitter = job->submitter();
            newone->niceness = job->niceness();
            newone->add_job(job);
            job_requests.insert(it, newone);
            return;
        }
    }
    JobRequestsGroup *newone = new JobRequestsGroup();
    newone->submitter = job->submitter();
    newone->niceness = job->niceness();
    newone->add_job(job);
    job_requests.push_back(newone);
}

static void enqueue_job_requests_group(JobRequestsGroup* group) {
    for( list<JobRequestsGroup*>::iterator it = job_requests.begin(); it != job_requests.end(); ++it ) {
        if( (*it)->niceness > group->niceness) { // lower priority starts here, insert group
            job_requests.insert(it, group);
            return;
        }
    }
    job_requests.push_back(group);
}

// Gives a position in job_requests, used to iterate items.
struct JobRequestPosition
{
    JobRequestPosition() : group( nullptr ), job( nullptr ) {}
    JobRequestPosition(JobRequestsGroup* g, Job* j) : group( g ), job( j ) {}
    bool isValid() const { return group != nullptr; }
    JobRequestsGroup* group;
    Job* job;
};

/* Debit/credit helpers.  Job::dispatchOutstanding() makes every release
   path idempotent, so a job that is confirmed AND later erased releases
   exactly one credit.  */
static void debit_dispatch_credit(Job *job)
{
    if (job && !job->dispatchOutstanding()) {
        job->setDispatchOutstanding(true);
        const uint64_t now_msec = icecream_monotonic_msec();
        job->setDispatchDebitMsec(now_msec);
        job->submitter()->addOutstandingDispatch(now_msec);
    }
}

static void credit_dispatch_credit(Job *job)
{
    if (job && job->dispatchOutstanding() && job->submitter()) {
        job->setDispatchOutstanding(false);
        if (!job->submitter()->removeOutstandingDispatch(job->dispatchDebitMsec())) {
            log_error() << "dispatch-credit invariant failure: job " << job->id()
                        << " debit " << job->dispatchDebitMsec()
                        << " not found on " << job->submitter()->nodeName()
                        << " (outstanding=" << job->submitter()->outstandingDispatches()
                        << ")" << endl;
        }
    }
}

/* S2: fills the outgoing UseCS cache-handoff tail from the already-selected
   CS's retained Login snapshot.  Defined after handle_login (see
   project_cache_handoff below) so its body -- the only code that reads the
   CS's retained cache-advertisement getters -- stays outside the
   scoring/selection slice that unittests/p50cacheadvertisement-source.sh
   greps (lines 1..~2841).  job is already dispatched to job->server() by
   every caller of this function. */
static void project_cache_handoff(const Job *job,
                                  uint32_t &out_port, uint32_t &out_protocol,
                                  uint32_t &out_mask);

static bool send_remote_dispatch_reply(Job *job)
{
    assert(job);
    assert(job->submitter());
    assert(job->server());
    uint32_t cache_port = 0, cache_protocol = 0, cache_mask = 0;
    project_cache_handoff(job, cache_port, cache_protocol, cache_mask);
    UseCSMsg reply(job->dispatchPlatform(), job->server()->name,
                   job->server()->remotePort(), job->id(),
                   job->dispatchGotEnv(), job->localClientId(),
                   job->dispatchMatchedJobId(), job->assignmentEpoch(),
                   job->assignmentNonce(), cache_port, cache_protocol, cache_mask);
    reply.setCompileIdentity(job->cGuid(), job->tuSeq());
    return job->submitter()->send_msg(
        reply, MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable);
}

/* Queue the revoke on the same ordered S->F stream as PREPARE.  A successful
   return means only that ownership is retained in REVOKE_PENDING; release is
   exclusively the matching REVOKE_RESULT transition. */
static bool request_assignment_revoke(Job *job)
{
    if (!job || !job->assignmentFenced() || !job->server()
            || job->assignmentPhase() == Job::ASSIGNMENT_REVOKE_PENDING
            || job->assignmentPhase() == Job::ASSIGNMENT_CLAIMED_OR_LATER
            || job->assignmentPhase() == Job::ASSIGNMENT_TERMINAL) {
        return true;
    }
    if (job->assignmentPhase() != Job::ASSIGNMENT_PREPARED
            && job->assignmentPhase() != Job::ASSIGNMENT_READY) {
        return false;
    }
    RevokeBeforeStartMsg revoke(job->assignmentEpoch(), job->id(),
                                job->assignmentNonce());
    if (!job->server()->send_msg(
            revoke, MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable)) {
        return false;
    }
    job->setAssignmentPhase(Job::ASSIGNMENT_REVOKE_PENDING);
    ++assignment_revoke_requested;
    return true;
}

/* A submitter is eligible for new assignments only when it is neither
   mid-backlog nor holding its full quota of unconfirmed dispatches.  The
   deferral half is episode-scoped on purpose: it stays closed until the
   backlog fully drains, rather than reopening because userspace bytes moved
   into the kernel.  */
static bool submitter_accepts_dispatch(CompileServer *submitter)
{
    return submitter
        && !submitter->has_pending_write()
        && submitter->outstandingDispatches() < effective_dispatch_credit();
}

enum AdmitResult { ADMIT_COMPLETE, ADMIT_YIELDED, ADMIT_FATAL_EXHAUSTION };
static AdmitResult admit_request_jobs(CompileServer *submitter, PendingExpansion &req);

/* Resume requests whose expansion was cut short by the loop budget.  A
   caller that asked for N replies still gets N; the work is merely spread
   across iterations.  Connections are served round-robin from a persistent
   cursor -- restarting from begin() every turn would let the lowest fd
   monopolise whatever budget is left -- and each serviced connection
   advances only the FRONT of its FIFO, preserving per-connection request
   order.  Returns true if anything remains for a later turn.  */
static bool expand_pending_requests(bool single_quantum = false)
{
    if (pending_expansions.empty()) {
        return false;
    }

    map<int, deque<PendingExpansion> >::iterator it =
        pending_expansions.upper_bound(pending_service_cursor);
    const size_t rounds = single_quantum ? 1 : pending_expansions.size();
    bool more = false;
    for (size_t k = 0; k < rounds; ++k) {
        if (it == pending_expansions.end()) {
            it = pending_expansions.begin();
        }
        const int fd = it->first;
        map<int, CompileServer *>::const_iterator cit = fd2cs.find(fd);
        if (cit == fd2cs.end()) {
            it = pending_expansions.erase(it);   // submitter gone
            continue;
        }
        if (inbound_budget_remaining <= 0) {
            /* Ask for the pending-first turn: without it, iterations whose
               budget is always spent by new ingress would never get here
               with anything left.  */
            pending_expansions_starved = true;
            return true;
        }
        deque<PendingExpansion> &q = it->second;
        if (!q.empty()) {
            const AdmitResult r = admit_request_jobs(cit->second, q.front());
            if (r == ADMIT_COMPLETE) {
                q.pop_front();
            } else if (r == ADMIT_FATAL_EXHAUSTION) {
                /* Close this submitter generation: its teardown sweep
                   terminalizes and releases every staged member exactly
                   once and erases its records; unrelated peers stay
                   live.  The iterator over pending_expansions is
                   invalidated by that teardown -- stop this pass.  */
                pending_service_cursor = fd;
                handle_end(cit->second, nullptr);
                return true;
            }
        }
        pending_service_cursor = fd;
        if (q.empty()) {
            it = pending_expansions.erase(it);
        } else {
            more = true;
            ++it;
        }
    }
    if (single_quantum && !pending_expansions.empty()) {
        more = true;
    }
    return more;
}

static JobRequestPosition get_first_job_request()
{
    if (job_requests.empty()) {
        return JobRequestPosition();
    }

    const uint64_t now_mono_msec = icecream_monotonic_msec();

    /* Active band: the first niceness level that has a dispatchable
       submitter.  Groups are sorted by niceness, so this also lets a lower
       band be served with full scoring/promotion when every group above it
       is gated -- previously a gated top band degraded selection to a bare
       FIFO fallback with neither scores nor deadlines applied.  */
    int best_niceness = -1;
    for (JobRequestsGroup *group : job_requests) {
        if (submitter_accepts_dispatch(group->submitter)) {
            best_niceness = group->niceness;
            break;
        }
    }
    if (best_niceness < 0) {
        return JobRequestPosition();   // every submitter is gated: dispatch pauses
    }

    /* Deadline-first LPT within the active band, O(groups) total: each
       group's FIFO front is its oldest request (promotion candidate), and
       *byScore.rbegin() is its score winner -- the static key means neither
       needs a walk over the group's jobs.  */
    JobRequestPosition overdue;
    uint64_t overdue_enqueue_mono = 0;
    JobRequestPosition best;
    int64_t best_key = 0;

    for (JobRequestsGroup *group : job_requests) {
        if (group->niceness < best_niceness) {
            continue;
        }
        if (group->niceness > best_niceness) {
            break;
        }
        /* A submitter whose channel still holds deferred (undelivered)
           dispatch replies -- or its full credit of unconfirmed dispatches --
           must not be granted further assignments; see
           submitter_accepts_dispatch().  Gated groups are skipped whole, so
           an ineligible submitter's jobs are never popped and restored.  */
        if (!submitter_accepts_dispatch(group->submitter)) {
            continue;
        }
        assert(!group->l.empty());
        Job *oldest = group->l.front();
        if (selection_overdue(oldest->enqueueMonoMsec(), now_mono_msec)) {
            if (!overdue.isValid() || oldest->enqueueMonoMsec() < overdue_enqueue_mono
                    || (oldest->enqueueMonoMsec() == overdue_enqueue_mono
                        && oldest->id() < overdue.job->id())) {
                overdue = JobRequestPosition(group, oldest);
                overdue_enqueue_mono = oldest->enqueueMonoMsec();
            }
            continue;
        }
        auto top = group->byScore.rbegin();
        Job *candidate = std::get<2>(*top);
        const int64_t key = std::get<0>(*top);
        if (!best.isValid()
                || selection_prefers(key, candidate->id(), best_key, best.job->id())) {
            best = JobRequestPosition(group, candidate);
            best_key = key;
        }
    }

    if (overdue.isValid()) {
        return overdue;   // hard promotion wins over any score
    }
    return best;
}

/* Circular successor of `pos`, or an invalid position once the walk has
   returned to `start`.  The walk must be circular: the scored head can sit
   in a later group, and a forward-only walk would never test the groups and
   jobs BEFORE it -- compatible work would be skipped and the caller would
   report "no suitable host" while schedulable requests were queued.  */
static JobRequestPosition get_next_job_request(const JobRequestPosition& pos,
                                               const JobRequestPosition& start)
{
    assert(!job_requests.empty());
    assert(pos.group != nullptr && pos.job != nullptr);

    JobRequestsGroup* group = pos.group;
    JobRequestPosition next;
    // Get next job in the same group -- O(1) via the job's stored position.
    list<Job*>::iterator jobIt = pos.job->queueIt();
    assert(*jobIt == pos.job);
    ++jobIt;
    if( jobIt != group->l.end()) {
        next = JobRequestPosition( group, *jobIt );
    } else {
        // Get next group, wrapping to the first.
        list<JobRequestsGroup*>::iterator groupIt = std::find(job_requests.begin(), job_requests.end(), group);
        assert(groupIt != job_requests.end());
        ++groupIt;
        if( groupIt == job_requests.end()) {
            groupIt = job_requests.begin();
        }
        group = *groupIt;
        assert(!group->l.empty());
        next = JobRequestPosition( group, group->l.front());
    }

    if (start.isValid() && next.group == start.group && next.job == start.job) {
        return JobRequestPosition();   // full circle: every candidate seen
    }
    return next;
}

// Removes the given job request.
// Also tries to rotate submitters in a round-robin fashion to try to serve
// them all fairly.
static void remove_job_request(const JobRequestPosition& pos)
{
    assert(!job_requests.empty());
    assert(pos.group != nullptr && pos.job != nullptr);

    JobRequestsGroup* group = pos.group;
    /* No membership asserts here: they were linear scans, which turned
       assertion-enabled builds back into the quadratic selector and made
       them useless as performance diagnostics.  remove_job() itself
       verifies membership through the queued flag.  */
    job_requests.remove(group);
    group->remove_job(pos.job);

    if (group->l.empty()) {
        delete group;
    } else {
        enqueue_job_requests_group(group);
    }
}

static string dump_job(Job *job, bool verbose);

/* Materialise jobs for one request, resuming at req.next_index, until the
   request is complete or the per-step / per-loop budget stops it.  Shared by
   direct ingress (handle_cs_request) and the pending-expansion resume path,
   so both charge the same budget and build the same sibling chain.  Advances
   req in place; returns true when the request is fully admitted.

   At least one job is always made per call: a request must progress every
   time it is serviced, or an exhausted budget could park it forever.  */
static AdmitResult admit_request_jobs(CompileServer *submitter, PendingExpansion &req)
{
    const GetCSMsg &m = req.msg;

    /* Exhaustion preflight: an expansion whose remaining members exceed
       the free id domain can NEVER complete -- parking it retried the
       same member forever while its staged members sat published but
       undispatchable.  Fail it fatally instead; the caller closes this
       submitter generation, which terminalizes and releases every staged
       member through the audited teardown sweep, exactly once.  */
    if ((uint64_t)(m.count - req.next_index) > (uint64_t)job_id_allocator().freeCount()) {
        log_error() << "expansion for client " << m.client_id << " needs "
                    << (m.count - req.next_index) << " ids but only "
                    << job_id_allocator().freeCount() << " remain;"
                    << " failing the batch" << endl;
        return ADMIT_FATAL_EXHAUSTION;
    }

    /* The chain anchor is the front of the staged list: staged jobs are not
       dispatchable, so the master cannot run -- let alone complete -- before
       the last sibling is chained onto it.  (An id-based re-lookup was tried
       first and reproducibly lost the chain for count=2000: the master
       dispatched and finished between admission steps, and every later
       sibling escaped its environment pinning.)  */
    Job *master_job = req.staged.empty() ? nullptr : req.staged.front();

    unsigned int made = 0;
    for (unsigned int i = req.next_index; i < m.count; ++i) {
        if (made > 0
                && (made >= max_jobs_per_expansion_step
                    || inbound_budget_remaining <= 0)) {
            req.next_index = i;
            return ADMIT_YIELDED;
        }
        Job *job = create_new_job(submitter);
        if (!job) {
            /* The preflight covers this, but a race with local-id
               allocation is still possible: same fatal outcome.  */
            log_error() << "job id domain exhausted at member " << i
                        << " of " << m.count << "; failing the batch" << endl;
            req.next_index = i;
            return ADMIT_FATAL_EXHAUSTION;
        }
        ++made;
        --inbound_budget_remaining;
        ++jobs_admitted_total;
        submitter->admittedJobsIncrement();
        job->setEnvironments(m.versions);
        job->setTargetPlatform(m.target);
        job->setArgFlags(m.arg_flags);
        switch(m.lang) {
            case CompileJob::Lang_C:
                job->setLanguage("C");
                break;
            case CompileJob::Lang_CXX:
                job->setLanguage("C++");
                break;
            case CompileJob::Lang_OBJC:
                job->setLanguage("ObjC");
                break;
            case CompileJob::Lang_OBJCXX:
                job->setLanguage("ObjC++");
                break;
            case CompileJob::Lang_Custom:
                job->setLanguage("<custom>");
                break;
            default:
                job->setLanguage("???"); // presumably newer client?
                break;
        }
        job->setFileName(m.filename);
        job->setLocalClientId(m.client_id);
        job->setPreferredHost(m.preferred_host);
        job->setMinimalHostVersion(m.minimal_host_version);
        job->setRequiredFeatures(m.required_features);
        job->setNiceness(max(0, min(20,int(m.niceness))));
        job->setCacheRequest(m.cache_protocol, m.cache_profile_mask,
                             m.cache_affinity_profile_mask,
                             m.cache_affinity_port,
                             m.cache_affinity_host);
        req.staged.push_back(job);
        std::ostream &dbg = log_info();
        dbg << "NEW " << job->id() << " client="
            << submitter->nodeName() << " versions=[";

        Environments envs = job->environments();

        for (Environments::const_iterator it = envs.begin();
                it != envs.end();) {
            dbg << it->second << "(" << it->first << ")";

            if (++it != envs.end()) {
                dbg << ", ";
            }
        }

        dbg << "] " << m.filename << " " << job->language() << " " << job->niceness();

        if (!master_job) {
            master_job = job;
        } else {
            master_job->appendJob(job);
            /* Chain membership in the admission record: multi-count siblings
               share the master's environment pinning, and a resumed request
               must land on the SAME master -- this line is what a test can
               hold against that.  */
            dbg << " master=" << master_job->id();
        }
        dbg << endl;
        notify_monitors(new MonGetCSMsg(job->id(), submitter->hostId(), &m));
    }

    req.next_index = m.count;
    /* Complete: the sibling set exists in full.  Only now do the jobs become
       eligible for dispatch, in creation order.  Activation is charged to
       the loop budget (one unit per job, like admission): the enqueue burst
       itself is atomic -- the released scheduler admitted AND enqueued all
       N in one synchronous call, so this is no worse than the baseline --
       but charging it stops the same turn from doing another full quantum
       of anything else on top of it.  */
    /* Saturating debit: a well-defined "budget exhausted", not amortisation
       -- the enqueue burst itself is still atomic (equal to the released
       scheduler's synchronous behavior for the same request).  A plain
       subtraction narrows size_t to int and can wrap for absurd counts.  */
    if (req.staged.size() >= (size_t)inbound_budget_remaining) {
        inbound_budget_remaining = 0;
    } else {
        inbound_budget_remaining -= (int)req.staged.size();
    }
    for (Job * const j : req.staged) {
        enqueue_job_request(j);
    }
    req.staged.clear();
    return ADMIT_COMPLETE;
}

static bool handle_cs_request(MsgChannel *cs, Msg *_m)
{
    GetCSMsg *m = dynamic_cast<GetCSMsg *>(_m);

    if (!m) {
        return false;
    }

    CompileServer *submitter = static_cast<CompileServer *>(cs);

    submitter->setClientCount(m->client_count);

    /* A count of zero asks for zero replies: admit nothing, exactly as the
       released scheduler's `for (i < count)` loop did.  Coercing it to one
       would manufacture a reply the caller never waits for.  */
    if (m->count == 0) {
        return true;
    }

    map<int, deque<PendingExpansion> >::iterator pit =
        pending_expansions.find(submitter->fd);
    if (pit != pending_expansions.end() && !pit->second.empty()) {
        /* An earlier request from this daemon is still expanding.  Queue
           this one behind it whole: requests are admitted in arrival order,
           so a newcomer can neither displace the older request's remaining
           jobs nor have its own admitted ahead of them.  */
        pit->second.push_back(PendingExpansion(*m));
        return true;
    }

    PendingExpansion req(*m);
    switch (admit_request_jobs(submitter, req)) {
    case ADMIT_COMPLETE:
        break;
    case ADMIT_YIELDED:
        pending_expansions[submitter->fd].push_back(req);
        break;
    case ADMIT_FATAL_EXHAUSTION:
        /* The teardown sweep owns the staged members' terminals and id
           releases; the connection object is gone after this call.  */
        handle_end(submitter, nullptr);
        return false;
    }

    return true;
}

static bool handle_local_job(CompileServer *cs, Msg *_m)
{
    JobLocalBeginMsg *m = dynamic_cast<JobLocalBeginMsg *>(_m);

    if (!m) {
        return false;
    }

    /* Duplicate local Begin for an already-mapped client-local id is an
       IDEMPOTENT no-op: counted, no allocation, no displaced terminal,
       no second monitor Begin (the ruled trace: Begin,Begin,Done,Done =>
       one allocation, one Begin, one terminal).  The mapping is checked
       BEFORE any allocation.  */
    if (cs->getClientLocalJobId(m->id) != 0) {
        ++duplicate_local_begin_ignored;
        trace() << "handle_local_job: duplicate local Begin for client id "
                << m->id << " ignored" << endl;
        return true;
    }
    const unsigned int local_id = job_id_allocator().allocate();
    if (local_id == 0) {
        trace() << "handle_local_job: id domain exhausted; dropping monitor"
                << " record for " << m->outfile << endl;
        return true;   /* monitoring-only record; the local build proceeds */
    }
    trace() << "handle_local_job " << (m->fulljob ? "(full) " : "") << m->outfile
        << " " << m->id << endl;
    cs->insertClientLocalJobId(m->id, local_id, m->fulljob);
    notify_monitors(new MonLocalJobBeginMsg(local_id, m->outfile, m->stime, cs->hostId()));
    return true;
}

static bool handle_local_job_done(CompileServer *cs, Msg *_m)
{
    JobLocalDoneMsg *m = dynamic_cast<JobLocalDoneMsg *>(_m);

    if (!m) {
        return false;
    }

    trace() << "handle_local_job_done " << m->job_id << endl;
    const int global_id = cs->getClientLocalJobId(m->job_id);
    if (global_id == 0) {
        /* Unknown or duplicate local Done: no record, no terminal for
           global id 0 (the old lookup default-inserted one), nothing to
           release.  */
        trace() << "no local record for client-local id " << m->job_id << endl;
        return true;
    }
    notify_monitors(new JobLocalDoneMsg(global_id));
    cs->eraseClientLocalJobId(m->job_id);
    release_local_id((uint32_t)global_id);
    return true;
}

/* Given a candidate CS and a JOB, check all installed environments
   on the CS for a match.  Return an empty string if none of the required
   environments for this job is installed.  Otherwise return the
   host platform of the first found installed environment which is among
   the requested.  That can be send to the client, which then completely
   specifies which environment to use (name, host platform and target
   platform).  */
static string envs_match(CompileServer *cs, const Job *job)
{
    if (job->submitter() == cs) {
        return cs->hostPlatform();    // it will compile itself
    }

    Environments compilerVersions = cs->compilerVersions();

    /* Check all installed envs on the candidate CS ...  */
    for (Environments::const_iterator it = compilerVersions.begin();
            it != compilerVersions.end(); ++it) {
        if (it->first == job->targetPlatform()) {
            /* ... IT now is an installed environment which produces code for
               the requested target platform.  Now look at each env which
               could be installed from the client (i.e. those coming with the
               job) if it matches in name and additionally could be run
               by the candidate CS.  */
            Environments environments = job->environments();
            for (Environments::const_iterator it2 = environments.begin();
                    it2 != environments.end(); ++it2) {
                if (it->second == it2->second && cs->platforms_compatible(it2->first)) {
                    return it2->first;
                }
            }
        }
    }

    return string();
}

static list<CompileServer *> filter_ineligible_servers(Job *job)
{
    list<CompileServer *> eligible;
    std::copy_if(
        css.begin(),
        css.end(),
        std::back_inserter(eligible),
        [=](CompileServer* cs) {
            if (cs != job->submitter()
                    && assignment_fence_mode == ConfCSMsg::StrictNonce
                    && (!IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_IDENTITY, cs)
                        || !IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_IDENTITY,
                                                job->submitter()))) {
                return false;
            }
            /* PREPARE is admitted only onto an empty S->F userspace queue.
               Appending another assignment behind deferred output would make
               an already bounded backlog the admission path for new work. */
            if (cs != job->submitter() && assignment_mode_prepares()
                    && IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_FENCE, cs)
                    && cs->has_pending_write()) {
                trace() << cs->nodeName()
                        << " has deferred assignment-control output" << endl;
                return false;
            }
            if (!cs->is_eligible_now(job)) {
#if DEBUG_SCHEDULER > 1
                if ((cs->currentJobCount() >= cs->maxJobs() + cs->maxPreloadCount()) || (cs->load() >= 1000)) {
                    trace() << "overloaded " << cs->nodeName() << " " << cs->currentJobCount() << "/"
                            <<  cs->maxJobs() << " jobs, load:" << cs->load() << endl;
                } else
                    trace() << cs->nodeName() << " not eligible" << endl;
#endif
                return false;
            }

            // incompatible architecture or busy installing
            if (!cs->can_install(job).size()) {
#if DEBUG_SCHEDULER > 2
                trace() << cs->nodeName() << " can't install " << job->id() << endl;
#endif
                return false;
            }

            /* Don't use non-chroot-able daemons for remote jobs.  XXX */
            if (!cs->chrootPossible() && cs != job->submitter()) {
                trace() << cs->nodeName() << " can't use chroot\n";
                return false;
            }

            // Check if remote & if remote allowed
            if (!cs->check_remote(job)) {
                trace() << cs->nodeName() << " fails remote job check\n";
                return false;
            }

            return true;
        });
    return eligible;
}

static uint32_t selected_cache_profile(const Job *job,
                                       const CompileServer *cs,
                                       P50CacheProfileRequest request)
{
    if (job == nullptr || cs == nullptr || cs == job->submitter() ||
        !IS_PROTOCOL_VERSION(PROTOCOL_VERSION_CACHE_ADVERTISEMENT, cs) ||
        !cache_advertisement_is_valid_present(
            cs->cacheEndpointPort(), cs->cacheProtocol(),
            cs->cacheProfileMask()))
        return 0;
    return p50_select_pair_cache_profile(
        job->cacheProtocol(), job->cacheProfileMask(),
        cs->cacheProtocol(), cs->cacheProfileMask(), request);
}

/* Soft rollout preference with a hard liveness escape.  Restrict selection
   to cache-compatible new workers only while at least one such worker has a
   real compile slot free (preload capacity does not count).  Once all are
   full, retain the entire legacy eligible set, so an old/incompatible worker
   can make progress rather than a request waiting for cache capacity.  A
   compatible warm-host hint is applied only inside that same free set. */
static void prefer_cache_compatible_servers(
    const Job *job, list<CompileServer *> &eligible)
{
    // Cache routing is an assignment-bound P50 policy.  Legacy dispatch has
    // no epoch/nonce with which to bind a handoff, so it must retain both the
    // cost and the ordering semantics of the historical selector.
    if (!assignment_mode_prepares())
        return;

    const P50CacheProfileRequest request =
        p50_cache_profile_request_from_env();
    list<CompileServer *> compatible_free;
    for (CompileServer *const cs : eligible) {
        if (cs->currentJobCount() < cs->maxJobs() &&
            selected_cache_profile(job, cs, request) != 0)
            compatible_free.push_back(cs);
    }
    if (compatible_free.empty())
        return;

    const string &affinity_host = job->cacheAffinityHost();
    list<CompileServer *> warm;
    size_t idle_compatible_excluded = 0;
    if (!affinity_host.empty()) {
        for (CompileServer *const cs : compatible_free) {
            const uint32_t selected =
                selected_cache_profile(job, cs, request);
            const bool matches_affinity =
                // UseCS serializes CompileServer::name as the canonical warm
                // host key.  CompileServer::matches() also accepts the
                // unauthenticated Login nodeName and would let a colliding
                // worker impersonate that retained endpoint.
                cs->name == affinity_host &&
                cs->remotePort() == job->cacheAffinityPort() &&
                (job->cacheAffinityProfileMask() & selected) != 0;
            if (matches_affinity) {
                warm.push_back(cs);
            } else if (cs->currentJobCount() == 0) {
                ++idle_compatible_excluded;
            }
        }
    }
    if (!warm.empty() && idle_compatible_excluded != 0) {
        trace() << "P50_WARM_HINT_OVERRIDE job=" << job->id()
                << " warm=" << warm.size()
                << " compatible_free=" << compatible_free.size()
                << " idle_excluded=" << idle_compatible_excluded << endl;
    }
    eligible.swap(warm.empty() ? compatible_free : warm);
}

static CompileServer *pick_server_random(list<CompileServer *> &eligible)
{
    auto iter = eligible.cbegin();
    std::advance(iter, random() % eligible.size());
    return *iter;
}

static CompileServer *pick_server_round_robin(list<CompileServer *> &eligible)
{
    uint64_t oldest_pick = 0;
    CompileServer *selected = nullptr;

    // Each recorded assignment stamps the server with a monotonic 64-bit
    // pick sequence (wire job ids may wrap, so they are not a clock);
    // round robin selects the server whose stamp is oldest.
    for (CompileServer * const cs: eligible) {
#if DEBUG_SCHEDULER > 1
        trace()
            << "considering server " << cs->nodeName() << " with last pick seq "
            << cs->lastPickSeq() << " and oldest known pick seq " << oldest_pick
            << endl;
#endif
        if (!selected || cs->lastPickSeq() < oldest_pick) {
            selected = cs;
            oldest_pick = cs->lastPickSeq();
        }
    }
    return selected;
}

static CompileServer *pick_server_least_busy(list<CompileServer *> &eligible)
{
    /* Pick the lowest OCCUPANCY (currentJobCount / maxJobs), comparing the
       fractions exactly by 64-bit cross-multiplication -- one pass, no
       division, no bucketing.  The previous two-pass form initialised its
       minimum to zero (unsigned, so it could never rise), used a different
       formula in each pass (ceiling vs floor), and bucketed occupancy into
       integer quotients; consequences: once every eligible host sat in its
       preload zone (count >= maxJobs) the filter selected an EMPTY set and
       the scheduler answered "no suitable host" while capacity existed, and
       below that a 75%-full host tied a 3%-full one.  Exact ties fall
       through to round-robin, which is the distribution property this mode
       exists for.  */
    list<CompileServer *> selected;
    uint64_t best_num = 0;
    uint64_t best_den = 1;
    for (CompileServer * const cs : eligible) {
#if DEBUG_SCHEDULER > 1
        trace()
            << "considering server " << cs->nodeName() << " with "
            << cs->currentJobCount() << " of " << cs->maxJobs() << " maximum jobs"
            << endl;
#endif
        if (!cs->maxJobs()) {
            continue;
        }
        const uint64_t num = cs->currentJobCount();
        const uint64_t den = cs->maxJobs();
        if (selected.empty() || num * best_den < best_num * den) {
            selected.clear();
            selected.push_back(cs);
            best_num = num;
            best_den = den;
        } else if (num * best_den == best_num * den) {
            selected.push_back(cs);
        }
    }

#if DEBUG_SCHEDULER > 1
    trace()
        << "servers to consider further: " << selected.size()
        << ", using ROUND_ROBIN for final selection" << endl;
#endif
    return pick_server_round_robin(selected);
}

static CompileServer *pick_server_new(Job *job, list<CompileServer *> &eligible)
{
    CompileServer *selected = nullptr;

    for (CompileServer * const cs: eligible) {
        if ((cs->lastCompiledJobs().size() == 0) && (cs->currentJobCount() == 0) && cs->maxJobs()) {
            if (!selected) {
                selected = cs;
            } else if (!envs_match(cs, job).empty()) {
                // if there is one server that already got the environment and one that
                // hasn't compiled at all, pick the one with environment first
                selected = cs;
            }
        }
    }
    return selected;
}

static CompileServer *pick_server_fastest(Job *job, list<CompileServer *> &eligible)
{
    // If we have no statistics simply use any server which is usable
    if (!all_job_stats.size()) {
        CompileServer *selected = pick_server_random(eligible);
        trace()
            << "no job stats - returning randomly selected "
            << selected->nodeName()
            << " load: "
            << selected->load()
            << " can install: "
            << selected->can_install(job)
            << endl;
        return selected;
    }

    CompileServer *best = nullptr;
    // best uninstalled
    CompileServer *bestui = nullptr;
    // best preloadable host
    CompileServer *bestpre = nullptr;

    // Any "new" servers with no stats should be selected first so we can get the stats we need.
    best = pick_server_new(job, eligible);
    if (best) {
        return best;
    }

    for (CompileServer * const cs : eligible) {

#if DEBUG_SCHEDULER > 1
        trace() << cs->nodeName() << " compiled " << cs->lastCompiledJobs().size() << " got now: " <<
                cs->currentJobCount() << " speed: " << server_speed(cs, job, true) << " compile time " <<
                cs->cumCompiled().compileTimeUser() << " produced code " << cs->cumCompiled().outputSize() <<
                " client count: " << cs->clientCount() << endl;
#endif

        // Some portion of the selection goes to a host that has not been
        // picked in a while, so its statistics stay reasonably current;
        // the greater STATS_UPDATE_WEIGHT, the rarer that refresh.  The
        // policy is the pure inequality in fastest.h (the previous
        // expression computed its threshold fraction in integer uint8_t
        // arithmetic, evaluated to zero, and never executed -- a cold or
        // formerly slow host stayed under-sampled forever).  The distance
        // clock is the dedicated 64-bit pick sequence, not the wire job
        // id, which may wrap.
        if (fastest_should_refresh(CompileServer::pickSequence() - cs->lastPickSeq(),
                                   STATS_UPDATE_WEIGHT, eligible.size())) {
            best = cs;
            break;
        }

        if (!envs_match(cs, job).empty()) {
            if (!best) {
                best = cs;
            }
            // Search the server with the earliest projected time to compile
            // the job.  (XXX currently this is equivalent to the fastest one)
            else if ((best->lastCompiledJobs().size() != 0)
                     && (server_speed(best, job) < server_speed(cs, job))) {
                if (cs->currentJobCount() < cs->maxJobs()) {
                    best = cs;
                } else {
                    bestpre = cs;
                }
            }

        } else {
            if (!bestui) {
                bestui = cs;
            }
            // Search the server with the earliest projected time to compile
            // the job.  (XXX currently this is equivalent to the fastest one)
            else if ((bestui->lastCompiledJobs().size() != 0)
                     && (server_speed(bestui, job) < server_speed(cs, job))) {
                if (cs->currentJobCount() < cs->maxJobs()) {
                    bestui = cs;
                } else {
                    bestpre = cs;
                }
            }
        }
    }

    if (best) {
#if DEBUG_SCHEDULER > 1
        trace() << "taking best installed " << best->nodeName() << " " <<  server_speed(best, job, true) << endl;
#endif
        return best;
    }

    if (bestui) {
#if DEBUG_SCHEDULER > 1
        trace() << "taking best uninstalled " << bestui->nodeName() << " " <<  server_speed(bestui, job, true) << endl;
#endif
        return bestui;
    }

    if (bestpre) {
#if DEBUG_SCHEDULER > 1
        trace() << "taking best preload " << bestpre->nodeName() << " " <<  server_speed(bestpre, job, true) << endl;
#endif
    }

    return bestpre;
}

static CompileServer *pick_server(Job *job, SchedulerAlgorithmName schedulerAlgorithm)
{
#if DEBUG_SCHEDULER > 0
    /* consistency checking for now */
    for (list<CompileServer *>::iterator it = css.begin(); it != css.end(); ++it) {
        CompileServer *cs = *it;

        const list<Job *>& jobList = cs->jobList();
        for (list<Job *>::const_iterator it2 = jobList.begin(); it2 != jobList.end(); ++it2) {
            assert(jobs.find((*it2)->id()) != jobs.end());
        }
    }

    for (map<unsigned int, Job *>::const_iterator it = jobs.begin();
            it != jobs.end(); ++it) {
        Job *j = it->second;

        if (j->state() == Job::COMPILING) {
            CompileServer *cs = j->server();
            const list<Job *>& jobList = cs->jobList();
            assert(find(jobList.begin(), jobList.end(), j) != jobList.end());
        }
    }
#endif

    // Ignore ineligible servers
    list<CompileServer *> eligible = filter_ineligible_servers(job);

#if DEBUG_SCHEDULER > 1
    trace() << "pick_server " << job->id() << " " << job->targetPlatform() << endl;
#endif

    /* if the user wants to test/prefer one specific daemon, we return it if available */
    if (!job->preferredHost().empty()) {
        for (CompileServer* const cs : css) {
            const bool prepare_backlogged = cs != job->submitter()
                && assignment_mode_prepares()
                && IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_FENCE, cs)
                && cs->has_pending_write();
            const bool strict_incompatible = cs != job->submitter()
                && assignment_fence_mode == ConfCSMsg::StrictNonce
                && (!IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_IDENTITY, cs)
                    || !IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_IDENTITY,
                                            job->submitter()));
            if (cs->matches(job->preferredHost()) && cs->is_eligible_now(job)
                    && !prepare_backlogged && !strict_incompatible) {
#if DEBUG_SCHEDULER > 1
                trace() << "taking preferred " << cs->nodeName() << " " <<  server_speed(cs, job, true) << endl;
#endif
                return cs;
            }
        }

        return nullptr;
    }

    prefer_cache_compatible_servers(job, eligible);

    // Don't bother running an algorithm if we don't need to.
    if ( eligible.size() == 0 ) {
        trace() << "no eligible servers" << endl;
        return nullptr;
    } else if (eligible.size() == 1) {
        CompileServer *selected = eligible.front();
        trace() << "returning only available server "
            << selected->nodeName()
            << " load: "
            << selected->load()
            << " can install: "
            << selected->can_install(job)
            << endl;
        return selected;
    }

    CompileServer *selected;
    switch (schedulerAlgorithm) {
        case SchedulerAlgorithmName::NONE:
        case SchedulerAlgorithmName::UNDEFINED:
            [[fallthrough]];
        default:
            trace()
                << "unknown scheduler algorithm " << schedulerAlgorithm
                << ", using " << SchedulerAlgorithmName::RANDOM << endl;
            [[fallthrough]];
        case SchedulerAlgorithmName::RANDOM:
            selected = pick_server_random(eligible);
            break;
        case SchedulerAlgorithmName::ROUND_ROBIN:
            selected = pick_server_round_robin(eligible);
            break;
        case SchedulerAlgorithmName::LEAST_BUSY:
            selected = pick_server_least_busy(eligible);
            break;
        case SchedulerAlgorithmName::FASTEST:
            selected = pick_server_fastest(job, eligible);
            break;
    }

    if (selected) {
        trace()
            << "selected " << selected->nodeName()
            << " using " << schedulerAlgorithm << " algorithm" << endl;
    } else {
        trace()
            << "failed to select a server using "
            << schedulerAlgorithm << " algorithm" << endl;
    }
    return selected;
}

/* Prunes the list of connected servers by those which haven't
   answered for a long time. Return the number of seconds when
   we have to cleanup next time. */
static time_t prune_servers()
{
    list<CompileServer *>::iterator it;

    time_t now = time(nullptr);
    time_t min_time = MAX_SCHEDULER_PING;

    /* Pre-login peers live only in fd2cs -- the old prune walked controls
       and logged-in css, so a peer that never completed its handshake was
       never visited and held its slot forever.  Expire every accounted
       peer past its whole-handshake lease.  */
    {
        const uint64_t now_mono = icecream_monotonic_msec();
        uint64_t earliest_budget_ms = 0;
        bool have_budget = false;
        map<int, CompileServer *>::iterator fit = fd2cs.begin();
        while (fit != fd2cs.end()) {
            CompileServer *pcs = fit->second;
            ++fit;   /* handle_end erases from fd2cs */
            if (!pcs->preloginAccounted()) {
                continue;
            }
            if (now_mono >= pcs->preloginDeadline()) {
                trace() << "pre-login lease expired for " << pcs->name << endl;
                ++prelogin_expired_total;
                handle_end(pcs, nullptr);
                continue;
            }
            const uint64_t left = pcs->preloginDeadline() - now_mono;
            if (!have_budget || left < earliest_budget_ms) {
                earliest_budget_ms = left;
                have_budget = true;
            }
        }
        /* The earliest live lease must GOVERN the poll sleep: an idle
           scheduler otherwise dozes past the nominal expiry until an
           unrelated timer wakes it.  Rounded UP, never to zero before
           expiry.  */
        if (have_budget) {
            const time_t secs = (time_t)((earliest_budget_ms + 999) / 1000);
            if (secs < min_time) {
                min_time = secs > 0 ? secs : 1;
            }
        }
    }

    /* Refresh the farm-slot aggregate for the dispatch-credit clamp.  Once
       per loop over all daemons is cheap and cannot go stale across the
       login/logout/relogin paths that change it.  */
    {
        unsigned int slots = 0;
        for (CompileServer * const cs : css) {
            if (cs->state() == CompileServer::LOGGEDIN && !cs->noRemote()
                && cs->maxJobs() > 0) {
                slots += (unsigned int)cs->maxJobs();
            }
        }
        cached_remote_farm_slots = slots;
    }

    for (it = controls.begin(); it != controls.end();) {
        if ((now - (*it)->last_talk) >= MAX_SCHEDULER_PING) {
            CompileServer *old = *it;
            ++it;
            handle_end(old, nullptr);
            continue;
        }

        min_time = min(min_time, MAX_SCHEDULER_PING - now + (*it)->last_talk);
        ++it;
    }

    for (it = css.begin(); it != css.end();) {
        (*it)->startInConnectionTest();
        time_t cs_in_conn_timeout = (*it)->getNextTimeout();
        if(cs_in_conn_timeout != -1)
        {
            min_time = min(min_time, cs_in_conn_timeout);
        }

        if ((*it)->busyInstalling() && ((now - (*it)->busyInstalling()) >= MAX_BUSY_INSTALLING)) {
            trace() << "busy installing for a long time - removing " << (*it)->nodeName() << endl;
            CompileServer *old = *it;
            ++it;
            handle_end(old, nullptr);
            continue;
        }

        /* Deferred dispatch replies (SendDeferrable) must not linger without
           bound if the daemon stays alive at the TCP level but never drains
           its socket: TCP keepalive does not cover that case, and
           TCP_USER_TIMEOUT is #ifdef'd and platform-dependent.  Give the
           daemon the same 30 seconds the old blocking send used to allow,
           then treat it as dead -- this is the application-level bound that
           keeps a stalled submitter's WAITINGFORCS jobs from pinning remote
           slots forever.  */
        if ((*it)->outstandingDispatches() == 0) {
            (*it)->setStallReported(false);
        }
        if ((*it)->outstandingDispatches() > 0 && !(*it)->deferred_output_armed()) {
            const uint64_t stall_msec =
                (*it)->oldestOutstandingDispatchMsec(icecream_monotonic_msec());
            if (stall_msec >= max_outstanding_stall_msec) {
                /* RETAIN everything and report it.  Nothing else here is
                   safe.

                   A submitting daemon proxies every compiler wrapper on its
                   host.  One wrapper frozen after its UseCS but before the
                   worker sees CompileFile holds a dispatch debit that
                   JobBegin never credits.  Three reactions were tried and
                   rejected: removing the daemon takes every healthy
                   sibling's work with it; deleting the assignment releases
                   a reservation NOBODY cancelled, so a late thaw can still
                   compile against a job the scheduler wrote off; and
                   quarantining the submitter from new work is re-armed by
                   the same stale debit on the next poll, cutting the host
                   off from remote builds entirely.

                   What bounds this is the per-submitter dispatch credit.
                   Be precise about what that does and does not provide:

                   - it caps how many UNCONFIRMED assignments ONE submitter
                     may hold, so a submitter whose wrappers freeze stops
                     receiving work once its credit is consumed;
                   - it is NOT a farm-progress guarantee.  The clamp is
                     computed from aggregate advertised slots, so it knows
                     nothing about platform, environment or eligibility;
                     several stalled submitters can between them retain
                     every slot; a one-slot farm gives credit 1, so a single
                     stalled submitter can hold that slot; and healthy
                     wrappers behind the same daemon are unaffected only
                     while that submitter has credit left.

                   Reclaiming a retained reservation in bounded time needs
                   either targeted cancellation with the daemon or
                   eligibility-aware global fairness; neither exists yet, so
                   the honest contract here is bounded admission with
                   retained ownership, and this bound is an OBSERVABILITY
                   point that names the stuck submitter for an operator.  */
                if (!(*it)->stallReported()) {
                    (*it)->setStallReported(true);
                    log_warning() << (*it)->nodeName() << " holds "
                                  << (*it)->outstandingDispatches()
                                  << " unconfirmed dispatches, oldest for "
                                  << (stall_msec / 1000) << "s - a client of"
                                     " this daemon is not progressing.  Its"
                                     " assignment and worker reservation are"
                                     " retained (only that client or its"
                                     " worker can end them); this submitter"
                                     " may hold up to its dispatch credit of "
                                  << effective_dispatch_credit()
                                  << " unconfirmed assignments" << endl;
                }
                ++it;
                continue;
            }
            /* Below the threshold: the reported episode is over, so a
               LATER stall on this submitter is reported as a new incident.
               Deliberately not cleared from the Begin/Done handlers --
               unrelated sibling progress does not resolve the stale
               assignment, and clearing there turned one frozen job into 61
               warnings in twelve seconds.  */
            (*it)->setStallReported(false);
            const uint64_t remaining = max_outstanding_stall_msec - stall_msec;
            min_time = min(min_time, (time_t)((remaining + 999) / 1000));
        }

        if ((*it)->deferred_output_armed()) {
            const uint64_t now_msec = icecream_monotonic_msec();
            const uint64_t deadline = (*it)->deferred_output_deadline_msec();

            if (now_msec >= deadline) {
                log_warning() << (*it)->nodeName()
                              << " has not accepted dispatch data within "
                              << (ICECC_DEFERRED_SEND_TIMEOUT_MSEC / 1000)
                              << "s - removing" << endl;
                CompileServer *old = *it;
                ++it;
                handle_end(old, nullptr);
                continue;
            }
            /* Cap the poll timeout by the remaining budget (rounded up, at
               least one second granularity) so a peer that never produces
               an event is still reaped on time.  */
            const uint64_t remaining_msec = deadline - now_msec;
            min_time = min(min_time, (time_t)((remaining_msec + 999) / 1000));
        }

        /* protocol version 27 and newer use TCP keepalive */
        if (IS_PROTOCOL_VERSION(27, *it)) {
            ++it;
            continue;
        }

        if ((now - (*it)->last_talk) >= MAX_SCHEDULER_PING) {
            if ((*it)->maxJobs() >= 0) {
                trace() << "send ping " << (*it)->nodeName() << endl;
                (*it)->setMaxJobs((*it)->maxJobs() * -1);   // better not give it away

                if ((*it)->send_msg(PingMsg())) {
                    // give it MAX_SCHEDULER_PONG to answer a ping
                    (*it)->last_talk = time(nullptr) - MAX_SCHEDULER_PING
                                       + 2 * MAX_SCHEDULER_PONG;
                    min_time = min(min_time, (time_t) 2 * MAX_SCHEDULER_PONG);
                    ++it;
                    continue;
                }
            }

            // R.I.P.
            trace() << "removing " << (*it)->nodeName() << endl;
            CompileServer *old = *it;
            ++it;
            handle_end(old, nullptr);
            continue;
        } else {
            min_time = min(min_time, MAX_SCHEDULER_PING - now + (*it)->last_talk);
        }

#if DEBUG_SCHEDULER > 1
        if ((random() % 400) < 0) {
            // R.I.P.
            trace() << "FORCED removing " << (*it)->nodeName() << endl;
            CompileServer *old = *it;
            ++it;
            handle_end(old, 0);
            continue;
        }
#endif

        ++it;
    }

    return min_time;
}

static bool empty_queue(SchedulerAlgorithmName schedulerAlgorithm)
{
    JobRequestPosition jobPosition = get_first_job_request();
    if (!jobPosition.isValid()) {
        return false;   // empty, or every submitter is gated (backlog/credit)
    }
    const JobRequestPosition walkStart = jobPosition;

    assert(!css.empty());

    CompileServer *use_cs = nullptr;
    Job* job = jobPosition.job;

    while (true) {
        use_cs = pick_server(job, schedulerAlgorithm);

        if (use_cs) {
            break;
        }

        /* Ignore the load on the submitter itself if no other host could
           be found.  We only obey to its max job number.  */
        use_cs = job->submitter();
        if ((use_cs->currentJobCount() < use_cs->maxJobs())
                && job->preferredHost().empty()
                /* This should be trivially true.  */
                && use_cs->can_install(job).size()) {
            break;
        }

        jobPosition = get_next_job_request( jobPosition, walkStart );
        /* Skip positions whose submitter is gated (backlog or credit limit)
           -- their assignments could not be delivered/confirmed anyway.  */
        while (jobPosition.isValid()
               && !submitter_accepts_dispatch(jobPosition.job->submitter())) {
            jobPosition = get_next_job_request( jobPosition, walkStart );
        }
        if (jobPosition.isValid()) {
            /* Retarget the job under test.  Without this the loop kept
               re-evaluating the ORIGINAL job at every later position, so a
               temporarily unschedulable queue head hid every schedulable
               request behind it (inherited defect, amplified by score-based
               head selection).  */
            job = jobPosition.job;
        }
        if (!jobPosition.isValid()) { // every live candidate was tested once
            jobPosition = walkStart;
            job = jobPosition.job;
            for (CompileServer * const cs : css) {
                if(!job->preferredHost().empty() && !cs->matches(job->preferredHost()))
                    continue;
                if(cs->is_eligible_ever(job)) {
                    trace() << "No suitable host found, delaying" << endl;
                    return false;
                }
            }
            // This means that there's nobody who could possibly handle the job,
            // so there's no point in delaying.
            log_info() << "No suitable host found, assigning submitter" << endl;
            use_cs = job->submitter();
            break;
        }
    }

    remove_job_request( jobPosition );

    job->setState(Job::WAITINGFORCS);
    job->setServer(use_cs);

    string host_platform = envs_match(use_cs, job);
    bool gotit = true;

    if (host_platform.empty()) {
        gotit = false;
        host_platform = use_cs->can_install(job);
    }

    /* Record which toolchain actually runs this job: the offer matching the
       chosen host platform.  Runtime estimates are keyed on it.  */
    for (const auto &e : job->environments()) {
        if (e.first == host_platform) {
            job->setSelectedEnvironment(e.second);
            break;
        }
    }

    // mix and match between job ids
    unsigned matched_job_id = 0;
    unsigned count = 0;

    list<JobStat> lastRequestedJobs = job->submitter()->lastRequestedJobs();
    for (list<JobStat>::const_iterator l = lastRequestedJobs.begin();
            l != lastRequestedJobs.end(); ++l) {
        unsigned rcount = 0;

        list<JobStat> lastCompiledJobs = use_cs->lastCompiledJobs();
        for (list<JobStat>::const_iterator r = lastCompiledJobs.begin();
                r != lastCompiledJobs.end(); ++r) {
            if (l->jobId() == r->jobId()) {
                matched_job_id = l->jobId();
            }

            if (++rcount > 16) {
                break;
            }
        }

        if (matched_job_id || (++count > 16)) {
            break;
        }
    }
    /* The dispatch reply is sent deferrable: if the submitter daemon is slow
       to drain its socket (its receive buffer is full because the machine is
       busy under a highly parallel build), the message stays queued in the
       channel's write buffer and is flushed from the main loop once poll()
       reports the socket writable again (see has_pending_write() there).
       This must not block, and transient backpressure must not tear down the
       submitter with all its in-flight jobs -- send_msg() only returns false
       here if the connection is genuinely dead.  */
    /* Is this a LOCAL decision -- the job placed back on the host that asked
       for it?  Semantic test, deliberately not keyed on the message class:
       protocol 37+ encodes it as NoCS, older peers as a UseCS naming the
       submitter itself, and both reserve zero remote farm capacity.  */
    const bool local_decision = (use_cs == job->submitter());

    job->setDispatchProjection(host_platform, gotit, matched_job_id);

    /* The policy is frozen per assignment before either peer can observe it.
       STRICT_NONCE additionally requires the complete P50 path, S->C and
       S->F; local decisions use neither a worker assignment nor this carrier. */
    const bool strict_path_eligible = assignment_fence_mode != ConfCSMsg::StrictNonce
        || (IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_IDENTITY, use_cs)
            && IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_IDENTITY,
                                   job->submitter()));
    if (!local_decision && !strict_path_eligible) {
        /* Selection filters make this unreachable in normal operation.  If a
           future selector violates that invariant, close the request instead
           of silently exposing a legacy remote assignment in strict mode. */
        log_error() << "refusing incompatible remote strict assignment "
                    << job->id() << endl;
        handle_end(job->submitter(), nullptr);
        return true;
    }
    const bool prepared_assignment = !local_decision
        && assignment_mode_prepares()
        && IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_FENCE, use_cs)
        && strict_path_eligible;
    if (prepared_assignment) {
        switch (assignment_fence_mode) {
        case ConfCSMsg::Advisory:
            job->setAssignmentPolicy(Job::ASSIGNMENT_ADVISORY);
            break;
        case ConfCSMsg::EnforcingCompat:
            job->setAssignmentPolicy(Job::ASSIGNMENT_ENFORCING_COMPAT);
            break;
        case ConfCSMsg::StrictNonce:
            job->setAssignmentPolicy(Job::ASSIGNMENT_STRICT_NONCE);
            break;
        case ConfCSMsg::Legacy:
            assert(false);
            break;
        }
        job->setAssignmentIdentity(scheduler_assignment_epoch,
                                   fresh_assignment_nonce());
        job->setAssignmentPhase(Job::ASSIGNMENT_PREPARED);
        index_fenced_assignment(job);
        AssignPrepareMsg prepare(job->assignmentEpoch(), job->id(),
                                 job->assignmentNonce(),
                                 job->submitter()->hostId());
        if (!use_cs->send_msg(
                prepare, MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable)) {
            trace() << "failed to prepare assignment " << job->id() << endl;
            handle_end(use_cs, nullptr);
            return true;
        }
        /* Admission is the accepted PREPARE enqueue, not later READY or
           client exposure.  Charge exactly here for both prepared modes. */
        debit_dispatch_credit(job);
    } else {
        job->setAssignmentPolicy(Job::ASSIGNMENT_LEGACY);
        job->setAssignmentPhase(Job::ASSIGNMENT_NONE);
    }

    const bool p50_assignment =
        IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_IDENTITY, job->submitter())
        && IS_PROTOCOL_VERSION(PROTOCOL_VERSION_ASSIGNMENT_IDENTITY, use_cs);
    if (p50_assignment) {
        assert(next_tu_seq != UINT64_MAX);
        job->setCompileIdentity(scheduler_assignment_epoch, next_tu_seq++);
    } else {
        job->setCompileIdentity(0, 0);
    }

    if (IS_PROTOCOL_VERSION(37, job->submitter()) && local_decision)
    {
        NoCSMsg m2(job->id(), job->localClientId(),
                   job->assignmentEpoch(), job->assignmentNonce(),
                   job->cGuid(), job->tuSeq());
        if (!job->submitter()->send_msg(m2, MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable)) {
            trace() << "failed to deliver job " << job->id() << endl;
            handle_end(job->submitter(), nullptr);   // will care for the rest
            return true;
        }
    }
    else if (!prepared_assignment || !job->assignmentReadyGated())
    {
        if (!send_remote_dispatch_reply(job)) {
            trace() << "failed to deliver job " << job->id() << endl;
            handle_end(job->submitter(), nullptr);   // will care for the rest
            return true;
        }
    }

    /* Charge remote-dispatch credit ONLY for work that actually reserves a
       farm slot.  Charging local decisions let a busy mixed-role daemon --
       i.e. any ordinary developer machine, which both submits and compiles --
       gate itself out of a live farm and then lose its queued and in-flight
       jobs to the liveness bound.  Reproduced: 0 of 3000 jobs served while
       the farm served 189 to another submitter.  */
    if (!local_decision && !job->dispatchOutstanding()) {
        debit_dispatch_credit(job);
    }


#if DEBUG_SCHEDULER >= 0
    if (!gotit) {
        trace() << "put " << job->id() << " in joblist of " << use_cs->nodeName() << " (will install now)" << endl;
    } else {
        trace() << "put " << job->id() << " in joblist of " << use_cs->nodeName() << endl;
    }
#endif
    use_cs->appendJob(job);

    /* if it doesn't have the environment, it will get it. */
    if (!gotit) {
        use_cs->setBusyInstalling(time(nullptr));
    }

    string env;

    if (!job->masterJobFor().empty()) {
        Environments environments = job->environments();
        for (Environments::const_iterator it = environments.begin(); it != environments.end(); ++it) {
            if (it->first == use_cs->hostPlatform()) {
                env = it->second;
                break;
            }
        }
    }

    if (!env.empty()) {
        list<Job *> masterJobFor = job->masterJobFor();
        for (Job * const jobTmp : masterJobFor) {
            // remove all other environments
            jobTmp->clearEnvironments();
            jobTmp->appendEnvironment(make_pair(use_cs->hostPlatform(), env));
        }
    }

    return true;
}

static bool handle_login(CompileServer *cs, Msg *_m)
{
    LoginMsg *m = dynamic_cast<LoginMsg *>(_m);

    if (!m) {
        return false;
    }

    std::ostream &dbg = trace();

    cs->setRemotePort(m->port);
    cs->setCompilerVersions(m->envs);
    cs->setMaxJobs(m->max_kids);
    cs->setNoRemote(m->noremote);

    if (m->nodename.length()) {
        cs->setNodeName(m->nodename);
    } else {
        cs->setNodeName(cs->name);
    }

    cs->setHostPlatform(m->host_platform);
    cs->setChrootPossible(m->chroot_possible);
    cs->setSupportedFeatures(m->supported_features);
    cs->setCacheAdvertisement(m->cache_endpoint_port, m->cache_protocol,
                              m->cache_profile_mask);
    cs->pick_new_id();

    for (list<string>::const_iterator it = block_css.begin(); it != block_css.end(); ++it)
        if (cs->matches(*it)) {
            return false;
        }

    dbg << "login " << m->nodename << " protocol version: " << cs->protocol
        << " features: " << supported_features_to_string(m->supported_features)
        << " cache=";
    if (m->hasCacheAdvertisement()) {
        dbg << cs->name << ":" << m->cache_endpoint_port
            << " cache_wire=v1 cache_protocol=" << m->cache_protocol
            << " cache_profiles="
            << cache_profiles_to_string(m->cache_profile_mask);
    } else {
        dbg << "off";
    }
    dbg << " [";
    for (Environments::const_iterator it = m->envs.begin(); it != m->envs.end(); ++it) {
        dbg << it->second << "(" << it->first << "), ";
    }
    dbg << "]" << endl;

    handle_monitor_stats(cs);

    /* remove any other clients with the same IP and name, they must be stale */
    for (list<CompileServer *>::iterator it = css.begin(); it != css.end();) {
        if (cs->eq_ip(*(*it)) && cs->nodeName() == (*it)->nodeName()) {
            CompileServer *old = *it;
            ++it;
            handle_end(old, nullptr);
            continue;
        }

        ++it;
    }

    if (cs->preloginAccounted()) {
        ++prelogin_completed_total;
    }
    prelogin_release(cs);
    css.push_back(cs);

    /* Configure the daemon */
    if (IS_PROTOCOL_VERSION(24, cs)) {
        cs->send_msg(scheduler_conf_message(cs));
    }

    return true;
}

/* S2: project a CS's retained Login cache-advertisement snapshot into the
   three-word UseCS cache-handoff tail (see UseCSMsg's cache_endpoint_port /
   cache_protocol / cache_profile_mask).  Applies exactly the same
   absent-or-present law LoginMsg::valid_payload enforces on the way in
   (cache_advertisement_is_well_formed_present): a snapshot that is not fully
   valid projects as wholly absent, never partially or incorrectly
   advertised.

   A present triple additionally REQUIRES a complete nonzero assignment
   identity {wire_job_id, assignment_epoch, assignment_nonce} -- the exact
   identity the outgoing UseCS itself carries (see send_remote_dispatch_reply)
   -- so the handoff is always bound to a specific, provable assignment.  A
   job dispatched under ASSIGNMENT_LEGACY (assignment_epoch/nonce left at 0)
   projects wholly absent even when the selected CS's own snapshot is
   perfectly valid: there is no wire identity to bind it to.

   Called only by send_remote_dispatch_reply, itself called only once
   ordinary worker selection has already picked `cs` for the job being
   dispatched (see the two call sites of send_remote_dispatch_reply).  This
   must never become scoring/selection input; that invariant is enforced by
   unittests/p50cacheadvertisement-source.sh, which greps the file's
   selection slice (up to roughly here) for this function's getters and
   requires none be found there. */
static void project_cache_handoff(const Job *job,
                                  uint32_t &out_port, uint32_t &out_protocol,
                                  uint32_t &out_mask)
{
    assert(job != nullptr);
    const CompileServer *const cs = job->server();
    assert(cs != nullptr);
    const bool identity_complete = job->id() != 0 &&
        job->assignmentEpoch() != 0 && job->assignmentNonce() != 0;
    const uint32_t port = cs->cacheEndpointPort();
    const uint32_t protocol = cs->cacheProtocol();
    const uint32_t mask = cs->cacheProfileMask();
    // Login advertises runnable capabilities, while the assignment tail must
    // carry one negotiated profile.  An explicit scheduler request is exact;
    // an absent request retains the P29V1-first default.  Unknown bits and
    // unavailable requests remain absent rather than silently switching.
    const auto request = p50_cache_profile_request_from_env();
    const uint32_t selected_mask = selected_cache_profile(job, cs, request);
    if (identity_complete &&
        cache_advertisement_is_valid_present(port, protocol, mask) &&
        selected_mask != 0) {
        out_port = port;
        out_protocol = protocol;
        out_mask = selected_mask;
    } else {
        out_port = 0;
        out_protocol = 0;
        out_mask = 0;
    }
}

static bool handle_relogin(MsgChannel *mc, Msg *_m)
{
    LoginMsg *m = dynamic_cast<LoginMsg *>(_m);

    if (!m) {
        return false;
    }

    CompileServer *cs = static_cast<CompileServer *>(mc);
    cs->setCompilerVersions(m->envs);
    cs->setCacheAdvertisement(m->cache_endpoint_port, m->cache_protocol,
                              m->cache_profile_mask);
    cs->setBusyInstalling(0);

    std::ostream &dbg = trace();
    dbg << "RELOGIN " << cs->nodeName() << "(" << cs->hostPlatform() << "): [";

    for (Environments::const_iterator it = m->envs.begin(); it != m->envs.end(); ++it) {
        dbg << it->second << "(" << it->first << "), ";
    }

    dbg << "] cache=";
    if (m->hasCacheAdvertisement()) {
        dbg << cs->name << ":" << m->cache_endpoint_port
            << " cache_wire=v1 cache_protocol=" << m->cache_protocol
            << " cache_profiles="
            << cache_profiles_to_string(m->cache_profile_mask);
    } else {
        dbg << "off";
    }
    dbg << endl;

    /* Configure the daemon */
    if (IS_PROTOCOL_VERSION(24, cs)) {
        cs->send_msg(scheduler_conf_message(cs));
    }

    /* A strict assignment may have crossed READY before this re-login was
       processed by S.  Its first projection was intentionally held when the
       retained worker cache tuple was absent; expose those exact jobs now,
       after installing the positive cache snapshot above. */
    for (map<unsigned int, Job *>::const_iterator it = jobs.begin();
         it != jobs.end(); ++it) {
        Job *job = it->second;
        if (job == nullptr || job->server() != cs ||
            job->assignmentPolicy() != Job::ASSIGNMENT_STRICT_NONCE ||
            job->assignmentPhase() != Job::ASSIGNMENT_READY ||
            job->assignmentReplySent())
            continue;
        if (!cache_advertisement_is_well_formed_present(
                cs->cacheEndpointPort(), cs->cacheProtocol(),
                cs->cacheProfileMask()))
            continue;
        if (!send_remote_dispatch_reply(job))
            return false;
        job->setAssignmentReplySent(true);
    }

    return false;
}

static bool handle_mon_login(CompileServer *cs, Msg *_m)
{
    if (cs->preloginAccounted()) {
        ++prelogin_completed_total;
    }
    prelogin_release(cs);

    MonLoginMsg *m = dynamic_cast<MonLoginMsg *>(_m);

    if (!m) {
        return false;
    }

    monitors.push_back(cs);
    // monitors really want to be fed lazily
    cs->setBulkTransfer();

    for (list<CompileServer *>::const_iterator it = css.begin(); it != css.end(); ++it) {
        handle_monitor_stats(*it);
    }

    fd2cs.erase(cs->fd);   // no expected data from them
    return true;
}

static bool handle_assignment_ready(CompileServer *cs, Msg *_m)
{
    AssignReadyMsg *m = dynamic_cast<AssignReadyMsg *>(_m);
    if (!m || m->epoch() == 0 || m->wire_id == 0 || m->nonce() == 0) {
        ++assignment_ready_ignored;
        return true;
    }
    const SchedulerAssignmentKey key { m->epoch(), m->wire_id, m->nonce() };
    auto indexed = fenced_assignments.find(key);
    if (indexed == fenced_assignments.end()) {
        ++assignment_ready_ignored;
        return true;
    }
    Job *job = indexed->second;
    if (job->server() != cs
            || job->assignmentPhase() != Job::ASSIGNMENT_PREPARED) {
        /* Duplicate, stale, post-revoke, and wrong-worker READY messages are
           bounded no-ops.  In particular they never resurrect exposure. */
        ++assignment_ready_ignored;
        return true;
    }

    job->setAssignmentPhase(Job::ASSIGNMENT_READY);
    ++assignment_ready_accepted;
    if (job->assignmentReadyGated()
            && getenv("ICECC_TESTS")
            && getenv("ICECC_TEST_P49_READY_NESTED_TEARDOWN")) {
        /* Deterministic integration seam for the double-failure boundary:
           UseCS fails, then the ordered REVOKE fails on this worker.  Both
           cuts are one-shot and reachable only in an explicitly armed test
           scheduler. */
        job->submitter()->testCutNextFlushAfter(0);
        job->server()->testCutNextFlushAfter(0);
    }
    const bool cache_ready = cache_advertisement_is_well_formed_present(
        cs->cacheEndpointPort(), cs->cacheProtocol(), cs->cacheProfileMask());
    if (job->assignmentPolicy() == Job::ASSIGNMENT_STRICT_NONCE && !cache_ready) {
        trace() << "holding strict READY job " << job->id()
                << " until worker cache re-login" << endl;
        return true;
    }
    if (job->assignmentReadyGated() && !send_remote_dispatch_reply(job)) {
        trace() << "failed to expose ready assignment " << job->id() << endl;
        const int worker_fd = cs->fd;
        const unsigned int worker_generation = cs->connectionGeneration();
        handle_end(job->submitter(), nullptr);
        /* Submitter teardown requests an ordered revoke.  If that send also
           fails, handle_end() recursively destroys this exact worker.  Tell
           drain_connection() that its pointer is gone; returning true here
           would make it dereference freed `cs` on the next drain iteration. */
        const auto live = fd2cs.find(worker_fd);
        const bool worker_still_live = live != fd2cs.end()
            && live->second == cs
            && live->second->connectionGeneration() == worker_generation;
        if (!worker_still_live) {
            trace() << "READY teardown deleted current worker; stopping drain"
                    << endl;
        }
        return worker_still_live;
    }
    if (job->assignmentReadyGated())
        job->setAssignmentReplySent(true);
    return true;
}

static bool handle_assignment_terminal(CompileServer *cs, Msg *_m)
{
    RevokeResultMsg *m = dynamic_cast<RevokeResultMsg *>(_m);
    if (!m || m->epoch() == 0 || m->wire_id == 0 || m->nonce() == 0
            || !m->validResult()) {
        ++assignment_terminal_ignored;
        return true;
    }
    const SchedulerAssignmentKey key { m->epoch(), m->wire_id, m->nonce() };
    auto indexed = fenced_assignments.find(key);
    if (indexed == fenced_assignments.end()) {
        ++assignment_terminal_ignored;
        return true;
    }
    Job *job = indexed->second;
    if (job->server() != cs
            || job->assignmentPhase() != Job::ASSIGNMENT_REVOKE_PENDING) {
        ++assignment_terminal_ignored;
        return true;
    }

    if (m->result == RevokeResultMsg::ClaimedOrLater) {
        /* The claim won at F.  JobBegin may already have advanced the product
           state, or may follow once a claimed request reaches the load gate.
           Either way ownership remains until the worker's ordinary JobDone. */
        ++assignment_terminal_accepted;
        job->setAssignmentPhase(Job::ASSIGNMENT_CLAIMED_OR_LATER);
        return true;
    }

    /* Revoked is a release witness only while the assignment still has no
       scheduler-side claim evidence.  A JobBegin that crossed the ordered
       revoke is stronger evidence than a contradictory delayed result and
       leaves ownership to the ordinary worker completion boundary. */
    if (job->state() != Job::WAITINGFORCS) {
        ++assignment_terminal_ignored;
        return true;
    }

    ++assignment_terminal_accepted;
    job->setAssignmentPhase(Job::ASSIGNMENT_TERMINAL);
    /* This accepted Revoked result is the scheduler's authoritative terminal
       boundary for a reservation that never started.  Keep it in the same
       textual END grammar as ordinary JobDone so strict evidence collectors
       can prove every dispatch has exactly one terminal. */
    trace() << "END " << job->id() << " status=255" << endl;
    if (job->server()) {
        job->server()->removeJob(job);
    }
    credit_dispatch_credit(job);
    notify_monitors(new MonJobDoneMsg(JobDoneMsg(job->id(), 255)));
    map<unsigned int, Job *>::iterator it = jobs.find(job->id());
    assert(it != jobs.end() && it->second == job);
    remove_job_entry(it);
    delete job;
    return true;
}

static bool handle_job_begin(CompileServer *cs, Msg *_m)
{
    JobBeginMsg *m = dynamic_cast<JobBeginMsg *>(_m);

    if (!m) {
        return false;
    }

    if (jobs.find(m->job_id) == jobs.end()) {
        trace() << "handle_job_begin: no valid job id " << m->job_id << endl;
        return false;
    }

    Job *job = jobs[m->job_id];

    if (job->server() != cs) {
        trace() << "that job isn't handled by " << cs->name << endl;
        return false;
    }

    cs->setClientCount(m->client_count);

    /* Explicit begin transition -- one linearization point per job.
         WAITINGFORCS -> COMPILING   the valid begin;
         COMPILING    -> COMPILING   a duplicate begin from the assigned
                                     worker: counted and ignored, so it
                                     cannot reset timestamps or emit a
                                     second monitor begin event;
         anything else               stale/invalid: counted and ignored
                                     (the sender is the recorded worker --
                                     already checked above -- so this is
                                     not an authority violation, just an
                                     out-of-order message).  */
    if (job->state() != Job::WAITINGFORCS) {
        trace() << "handle_job_begin: job " << m->job_id << " is not"
                << " awaiting a begin (state " << (int)job->state()
                << "); duplicate/stale begin ignored" << endl;
        ++nonwaiting_begin_rejects;
        return true;
    }

    /* Observable progress: the client received its UseCS and reached the
       compile server, so this assignment no longer occupies a dispatch
       credit on its submitter, and whatever was stuck is moving again.  */
    credit_dispatch_credit(job);

    job->setState(Job::COMPILING);
    if (job->assignmentFenced()
            && job->assignmentPhase() != Job::ASSIGNMENT_REVOKE_PENDING) {
        job->setAssignmentPhase(Job::ASSIGNMENT_CLAIMED_OR_LATER);
    }
    job->setStartTime(m->stime);
    job->setStartOnScheduler(time(nullptr));
    notify_monitors(new MonJobBeginMsg(m->job_id, m->stime, cs->hostId()));
#if DEBUG_SCHEDULER >= 0
    trace() << "BEGIN: " << m->job_id << " client="
            << (job->submitter() ? job->submitter()->nodeName() : job->submitterName() + "<detached>")
            << "(" << job->targetPlatform() << ")" << " server="
            << job->server()->nodeName() << "(" << job->server()->hostPlatform()
            << ")" << endl;
#endif

    return true;
}


static bool handle_job_done(CompileServer *cs, Msg *_m)
{
    JobDoneMsg *m = dynamic_cast<JobDoneMsg *>(_m);

    if (!m) {
        return false;
    }

    Job *j = nullptr;

    if (uint32_t clientId = m->unknown_job_client_id()) {
        /* Pre-reply cancellation: the client exited while its request was
           still waiting for a host, so the daemon can name it only by its
           local client id.  A count>1 request gives every sibling that ONE
           client id, so the cancellation names the whole batch and must
           remove every member atomically.  The previous one-at-a-time form
           had two failure modes: it deleted only the LAST match while
           dequeuing all of them (the rest lived on in the jobs map as
           undispatchable ghosts), and a STAGED job -- published in the jobs
           map but owned by its expansion record -- could be deleted out
           from under the record, leaving dangling pointers in the staged
           list and in the master's sibling list (use-after-free on the
           next admission step).  */
        cs->setClientCount(m->client_count);

        /* One unified sweep, in BOTH domains, never an early return.  A
           client id can have members in the staging records AND in the live
           jobs map at the same moment: an earlier request with the same id
           may be fully admitted (its record gone, its jobs queued) while a
           later one is still staged -- daemons reuse client ids -- and the
           request and its cancel can be split across TCP segments, so
           admission and even dispatch can run between them.  The previous
           form returned as soon as it had destroyed a staged record,
           leaving every queued member of the same client id behind as
           undispatchable ghosts that pinned the submitter's dispatch credit
           until the scheduler looked wedged (issue #2).  */
        unsigned int cancelled = 0;

        /* 1. Staging records: destroy EVERY matching record and its staged
           members (a fd can hold several queued requests for one id).  */
        map<int, deque<PendingExpansion> >::iterator pit =
            pending_expansions.find(cs->fd);
        if (pit != pending_expansions.end()) {
            deque<PendingExpansion> &q = pit->second;
            for (deque<PendingExpansion>::iterator rit = q.begin();
                    rit != q.end();) {
                if (rit->msg.client_id != clientId) {
                    ++rit;
                    continue;
                }
                trace() << "STOP (STAGED) FOR client " << clientId << ": "
                        << rit->staged.size() << " of " << rit->msg.count
                        << " staged jobs cancelled before activation" << endl;
                for (Job * const sj : rit->staged) {
                    notify_monitors(new MonJobDoneMsg(JobDoneMsg(sj->id(), 255)));
                    {
                        map<unsigned int, Job *>::iterator sit = jobs.find(sj->id());
                        if (sit != jobs.end()) {
                            remove_job_entry(sit);
                        }
                    }
                    delete sj;
                    ++cancelled;
                }
                rit = q.erase(rit);
            }
            if (q.empty()) {
                pending_expansions.erase(pit);
            }
        }

        /* 2. Activated members: cancel EVERY queued sibling.  Members
           already dispatched (server set) are left alone deliberately: the
           submitting daemon receives their UseCS, finds the client gone,
           and answers each with JobDone(107, FROM_SUBMITTER) -- that bounce
           credits their dispatch debits and releases their worker
           reservations, so cancelling them here would race it.  */
        for (map<unsigned int, Job *>::iterator mit = jobs.begin();
                mit != jobs.end();) {
            Job *job = mit->second;
            if (!(job->server() == nullptr && job->submitter() == cs
                  && job->localClientId() == clientId)) {
                ++mit;
                continue;
            }
            trace() << "STOP (WAITFORCS) FOR " << mit->first << endl;
            for (list<JobRequestsGroup *>::iterator it = job_requests.begin();
                    it != job_requests.end(); ++it) {
                if ((*it)->submitter == cs && (*it)->remove_job(job)) {
                    if ((*it)->l.empty()) {
                        delete *it;
                        job_requests.erase(it);
                    }
                    break;
                }
            }
            notify_monitors(new MonJobDoneMsg(JobDoneMsg(job->id(), 255)));
            credit_dispatch_credit(job);
            {
                map<unsigned int, Job *>::iterator next = mit;
                ++next;
                remove_job_entry(mit);
                mit = next;
            }
            delete job;
            ++cancelled;
        }
        if (cancelled == 0) {
            /* Every member had already dispatched (or completed): nothing
               is scheduler-owned any more, and the daemon's own
               JobDone(107) bounces will settle the dispatched ones.  A
               well-formed cancellation on the submitter's own connection
               is idempotently successful -- returning false here signalled
               "connection deleted" to the drain loop while the connection
               was in fact alive, stopping the drain for one iteration at
               exactly the moment the daemon may be sending those 107
               completions.  */
            trace() << "no scheduler-owned members remain for client "
                    << clientId << endl;
        }
        return true;
    } else if (jobs.find(m->job_id) != jobs.end()) {
        j = jobs[m->job_id];
    }

    if (!j) {
        /* G4 teardown race (bigoracle 21:47 / local-oracle 21:50): an exact
           JobDone can arrive after the job was already removed by the winning
           side of a worker/submitter completion race.  drain_connection()
           treats a false return as "connection deleted" and stops draining,
           stranding later frames already buffered on a live channel with no new
           readiness edge.  The job is absent, so neither late origin can change
           job state -- count it and return true so the drain continues.
           (Unknown-client cancellation kept its own separate sweep above; a
           delayed 32-bit terminal after id reuse is a p50 assignment-key
           concern, orthogonal to absent-job idempotence.) */
        if (m->is_from_server()) {
            ++late_worker_terminal_ignores;
        } else {
            ++late_submitter_terminal_ignores;
        }
        trace() << "late terminal for absent job " << m->job_id
                << " ignored (from_server=" << (m->is_from_server() ? 1 : 0) << ")" << endl;
        return true;
    }

    const bool assignment_identity_matches =
        m->assignmentEpoch() == j->assignmentEpoch() &&
        m->assignmentNonce() == j->assignmentNonce();
    const bool compile_identity_matches =
        m->cGuid() == j->cGuid() && m->tuSeq() == j->tuSeq();
    /* A selected protocol-50 F can fail after claiming the exact fenced
       assignment but before CompileFile reaches it (for example, a bounded
       cache-session refusal or source-arm deadline).  At that boundary F
       owns {job, epoch, nonce}, but has never received {C_GUID, TU_SEQ}.
       Accept a wholly-absent compile identity only for that non-success
       worker terminal while the scheduler still has no JobBegin.  The
       selected-worker authority check immediately below remains mandatory;
       successes, submitter terminals, begun compiles, partial identities,
       and every assignment mismatch still fail closed. */
    const bool exact_precompile_worker_failure =
        assignment_identity_matches && m->is_from_server() &&
        m->exitcode != 0 && j->assignmentFenced() &&
        j->state() == Job::WAITINGFORCS && j->cGuid() != 0 &&
        m->cGuid() == 0 && m->tuSeq() == 0;
    if (!assignment_identity_matches ||
        (!compile_identity_matches && !exact_precompile_worker_failure)) {
        log_info() << "terminal assignment/compile identity mismatch for job "
                   << m->job_id << endl;
        handle_end(cs, nullptr);
        return false;
    }

    /* Centralized, phase- and origin-sensitive terminal authority.  The
       decision comes from the job (pointer AND generation must match; a
       detached job accepts no submitter-origin terminal at all); this
       handler only maps the decision to the wire policy.  All mismatch
       diagnostics are null-safe.  */
    switch (j->authorizeTerminal(cs, cs->connectionGeneration(), m->is_from_server())) {
    case Job::ACCEPT_WORKER:
    case Job::ACCEPT_SUBMITTER:
        break;
    case Job::REJECT_WRONG_WORKER:
        log_info() << "the server isn't the same for job " << m->job_id << endl;
        log_info() << "server: "
                   << (j->server() ? j->server()->nodeName() : string("<none>"))
                   << endl;
        log_info() << "msg came from: " << cs->nodeName() << endl;
        // the daemon is not following matz's rules: kick him
        handle_end(cs, nullptr);
        return false;
    case Job::REJECT_DETACHED_SUBMITTER:
        trace() << "ignoring non-worker completion for detached job "
                << m->job_id << " from " << cs->nodeName()
                << " (only worker " << (j->server() ? j->server()->nodeName()
                                                    : string("<none>"))
                << " may terminate it)" << endl;
        ++detached_terminal_rejects;
        return true;
    case Job::REJECT_WRONG_SUBMITTER:
        log_info() << "the submitter isn't the same for job " << m->job_id << endl;
        log_info() << "submitter: "
                   << (j->submitter() ? j->submitter()->nodeName()
                                      : j->submitterName() + "<detached>") << endl;
        log_info() << "msg came from: " << cs->nodeName() << endl;
        // the daemon is not following matz's rules: kick him
        handle_end(cs, nullptr);
        return false;
    }



    cs->setClientCount(m->client_count);

    /* A P49 submitter terminal before Begin requests ordered withdrawal; it
       is not itself permission to release the worker reservation.  Sever the
       submitter authority now (duplicate bounces become harmless), retain the
       job, and wait for the exact F terminal. */
    if (!m->is_from_server() && j->assignmentFenced()
            && j->state() == Job::WAITINGFORCS
            && (j->assignmentPhase() == Job::ASSIGNMENT_PREPARED
                || j->assignmentPhase() == Job::ASSIGNMENT_READY)) {
        CompileServer *worker = j->server();
        j->detachSubmitter();
        if (!request_assignment_revoke(j)) {
            /* A failed ordered send means this worker session is unusable;
               its teardown is the terminal boundary for all its jobs. */
            handle_end(worker, nullptr);
        }
        return true;
    }

    /* If JobBegin won before the submitter's exact withdrawal, the selected
       remote worker is already the only terminal authority.  Detach C just
       as the submitter-disconnect path does, but retain the job, worker slot,
       and wire id until that exact F reports completion (or its session is
       torn down).  Falling through would treat C's failure as completion,
       free live F capacity, and strand F's later authoritative JobDone. */
    if (!m->is_from_server() && j->assignmentFenced()
            && j->state() == Job::COMPILING
            && j->server() && j->server() != cs) {
        trace() << "submitter withdrew after fenced begin for job "
                << j->id() << "; retaining for worker completion" << endl;
        j->detachSubmitter();
        return true;
    }

    if (m->exitcode == 0) {
        std::ostream &dbg = trace();
        dbg << "END " << m->job_id
            << " status=" << m->exitcode;

        if (m->in_uncompressed)
            dbg << " in=" << m->in_uncompressed
                << "(" << int(m->in_compressed * 100 / m->in_uncompressed) << "%)";
        else {
            dbg << " in=0(0%)";
        }

        if (m->out_uncompressed)
            dbg << " out=" << m->out_uncompressed
                << "(" << int(m->out_compressed * 100 / m->out_uncompressed) << "%)";
        else {
            dbg << " out=0(0%)";
        }

        dbg << " real=" << m->real_msec
            << " user=" << m->user_msec
            << " sys=" << m->sys_msec
            << " pfaults=" << m->pfaults
            << " server=" << j->server()->nodeName()
            << endl;
    } else {
        trace() << "END " << m->job_id
                << " status=" << m->exitcode << endl;
    }

    if (j->server()) {
        j->server()->removeJob(j);
    }

    add_job_stats(j, m);
    notify_monitors(new MonJobDoneMsg(*m));
    credit_dispatch_credit(j);
    {
        map<unsigned int, Job *>::iterator dit = jobs.find(m->job_id);
        if (dit != jobs.end()) {
            remove_job_entry(dit);
        }
    }
    delete j;

    return true;
}

static bool handle_ping(CompileServer *cs, Msg * /*_m*/)
{
    cs->last_talk = time(nullptr);

    if (cs->maxJobs() < 0) {
        cs->setMaxJobs(cs->maxJobs() * -1);
    }

    return true;
}

static bool handle_stats(CompileServer *cs, Msg *_m)
{
    StatsMsg *m = dynamic_cast<StatsMsg *>(_m);

    if (!m) {
        return false;
    }

    /* Before protocol 25, ping and stat handling was
       clutched together.  */
    if (!IS_PROTOCOL_VERSION(25, cs)) {
        cs->last_talk = time(nullptr);

        if (cs->maxJobs() < 0) {
            cs->setMaxJobs(cs->maxJobs() * -1);
        }
    }

    for (CompileServer * const c : css)
        if (c == cs) {
            c->setLoad(m->load);
            /* StatsMsg never serializes client_count (the codec reads and
               writes only load and memory fields), so the member here is
               always its default zero.  Assigning it overwrote the fresher
               counts learned from GetCS/JobBegin/JobDone -- which DO carry
               the field on the wire -- and periodically zeroed a
               mixed-role host's ranking input.  Those message handlers
               keep updating it; the periodic stats must not.  */
            handle_monitor_stats(c, m);
            return true;
        }

    return false;
}

static bool handle_blacklist_host_env(CompileServer *cs, Msg *_m)
{
    BlacklistHostEnvMsg *m = dynamic_cast<BlacklistHostEnvMsg *>(_m);

    if (!m) {
        return false;
    }

    for (list<CompileServer *>::const_iterator it = css.begin(); it != css.end(); ++it)
        if ((*it)->name == m->hostname) {
            trace() << "Blacklisting host " << m->hostname << " for environment " << m->environment
                    << " (" << m->target << ")" << endl;
            cs->blacklistCompileServer(*it, make_pair(m->target, m->environment));
        }

    return true;
}

static string dump_job(Job *job, bool verbose)
{
    char buffer[1000];
    string line;

    /* A detached job keeps its submitter-name snapshot observable: the
       lifecycle gates assert post-detach identity from this dump, and a
       bare "<>" would erase exactly the information they check.  */
    const string subName = job->submitter()
        ? job->submitter()->nodeName()
        : (job->submitterDetached() ? job->submitterName() + "<detached>"
                                    : string("<>"));

    string jobState;
    switch(job->state()) {
    case Job::PENDING:
        jobState = "PEND";
        break;
    case Job::WAITINGFORCS:
        jobState = "WAIT";
        break;
    case Job::COMPILING:
        jobState = "COMP";
        break;
    default:
        jobState = "Huh?";
    }
    if (verbose) {
        time_t now = time(nullptr);
        time_t queue_age_s = now - job->enqueueTime();
        if (queue_age_s < 0) {
            queue_age_s = 0;
        }
        time_t state_age_s = now - job->stateChangeTime();
        if (state_age_s < 0) {
            state_age_s = 0;
        }
        snprintf(buffer, sizeof(buffer), "%u %s q:%lds st:%lds sub:%s on:%s ",
                 job->id(),
                 jobState.c_str(),
                 (long)queue_age_s,
                 (long)state_age_s,
                 subName.c_str(),
                 job->server() ? job->server()->nodeName().c_str() : "<unknown>");
    } else {
        snprintf(buffer, sizeof(buffer), "%u %s sub:%s on:%s ",
                 job->id(),
                 jobState.c_str(),
                 subName.c_str(),
                 job->server() ? job->server()->nodeName().c_str() : "<unknown>");
    }
    buffer[sizeof(buffer) - 1] = 0;
    line = buffer;
    line = line + job->fileName();
    return line;
}

/* Splits the string S between characters in SET and add them to list L.  */
static void split_string(const string &s, const char *set, list<string> &l)
{
    string::size_type end = 0;

    while (end != string::npos) {
        string::size_type start = s.find_first_not_of(set, end);

        if (start == string::npos) {
            break;
        }

        end = s.find_first_of(set, start);

        /* Do we really need to check end here or is the subtraction
           defined on every platform correctly (with GCC it's ensured,
        that (npos - start) is the rest of the string).  */
        if (end != string::npos) {
            l.push_back(s.substr(start, end - start));
        } else {
            l.push_back(s.substr(start));
        }
    }
}

static bool handle_control_login(CompileServer *cs)
{
    cs->setType(CompileServer::LINE);
    cs->last_talk = time(nullptr);
    cs->setBulkTransfer();
    cs->setState(CompileServer::LOGGEDIN);
    assert(find(controls.begin(), controls.end(), cs) == controls.end());
    controls.push_back(cs);

    std::ostringstream o;
    o << "200-ICECC " VERSION ": "
      << time(nullptr) - starttime << "s uptime, "
      << css.size() << " hosts, "
      << jobs.size() << " jobs in queue "
      << "(" << job_id_allocator().issuedTotal() << " total)." << endl;
    o << "200 Use 'help' for help and 'quit' to quit." << endl;
    return cs->send_msg(TextMsg(o.str()));
}

static bool handle_line(CompileServer *cs, Msg *_m)
{
    /* The control that owns an active internals transaction is streaming
       its response and must not issue another command on the same
       channel: that reply would bypass internals_emit and the reserved
       tail accounting.  Treat it as a protocol violation and fail the
       exact control generation.  (Concurrent internals from a DIFFERENT
       control still takes the bounded busy path below.)  */
    if (internals_txn.active
            && cs->fd == internals_txn.control_fd
            && cs->connectionGeneration() == internals_txn.control_generation) {
        /* Record the reason and clear the txn FIRST (so handle_end's own
           internals settlement sees it inactive and does not overwrite the
           reason), then fail the exact control channel: returning false to
           the drain loop means "the handler already deleted it", so we must
           actually delete it via handle_end.  */
        internals_txn_clear("active-control-reissued");
        handle_end(cs, nullptr);
        return false;
    }
    TextMsg *m = dynamic_cast<TextMsg *>(_m);

    if (!m) {
        return false;
    }

    string line;
    list<string> l;
    split_string(m->text, " \t\n", l);
    string cmd;

    cs->last_talk = time(nullptr);

    if (l.empty()) {
        cmd = "";
    } else {
        cmd = l.front();
        l.pop_front();
        transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
    }

    if (cmd == "listcs") {
        for (CompileServer * const it : css) {
            char buffer[1000];
            sprintf(buffer, " (%s:%u) ", it->name.c_str(), it->remotePort());
            line = " " + it->nodeName() + buffer;
            line += "[" + it->hostPlatform() + "] speed=";
            /* admitted_total, NOT "submitted": the object also carries a
               live submittedJobsCount, and this is the cumulative admission
               count.  gen stamps the connection: the counter restarts with
               each connection object, so a baseline/delta pair is only
               valid while gen is unchanged.  */
            sprintf(buffer, "%.2f jobs=%d/%d load=%u admitted_total=%llu gen=%u outstanding=%u",
                    server_speed(it),
                    it->currentJobCount(), it->maxJobs(), it->load(),
                    (unsigned long long)it->admittedJobsTotal(),
                    it->connectionGeneration(),
                    it->outstandingDispatches());
            line += buffer;

            if (it->cacheEndpointPort() != 0) {
                sprintf(buffer,
                        " cache=%s:%u cache_wire=v1 cache_protocol=%u cache_profiles=%s",
                        it->name.c_str(), it->cacheEndpointPort(),
                        it->cacheProtocol(),
                        cache_profiles_to_string(it->cacheProfileMask()).c_str());
                line += buffer;
            } else {
                line += " cache=off";
            }

            if (it->busyInstalling()) {
                sprintf(buffer, " busy installing since %ld s",  time(nullptr) - it->busyInstalling());
                line += buffer;
            }

            if (!cs->send_msg(TextMsg(line))) {
                return false;
            }

            const list<Job *>& jobList = it->jobList();
            for (list<Job *>::const_iterator it2 = jobList.begin(); it2 != jobList.end(); ++it2) {
                if (!cs->send_msg(TextMsg("   " + dump_job(*it2, false)))) {
                    return false;
                }
            }
        }
    } else if (cmd == "listblocks") {
        for (list<string>::const_iterator it = block_css.begin(); it != block_css.end(); ++it) {
            if (!cs->send_msg(TextMsg("   " + (*it)))) {
                return false;
            }
        }
    } else if (cmd == "listjobs") {
        const bool verbose = !l.empty() && (l.front() == "v" || l.front() == "verbose");
        {
            char summary[1792];
            snprintf(summary, sizeof(summary),
                     " detached_terminal_rejects=%lu nonwaiting_begin_rejects=%lu"
                     " late_submitter_terminal_ignores=%lu late_worker_terminal_ignores=%lu"
                     " dup_local_begin=%lu id_release_violations=%lu"
                     " internals_output_dropped=%lu"
                     " prelogin_current=%u prelogin_max=%u prelogin_expired=%lu"
                     " prelogin_rejected=%lu prelogin_completed=%lu accepts_deferred=%lu"
                     " prelogin_underflow=%lu"
                     " alloc_live=%llu alloc_issued=%llu"
                     " internals_active=%d internals_peak_pending=%llu"
                     " internals_retained=%llu internals_omitted=%d internals_final_pending=%d"
                     " internals_marker_queued=%u internals_terminal_queued=%u"
                     " internals_sendpending=%u internals_waiting=%u internals_replied=%u"
                     " internals_t0_fd=%d internals_t0_gen=%u"
                     " internals_control_generation=%u internals_phase=%s"
                     " internals_collection_deadline=%llu internals_output_deadline=%llu"
                     " internals_last_valid=%d internals_last_peak=%llu"
                     " internals_last_retained=%llu internals_last_marker=%u"
                     " internals_last_terminal=%u internals_last_generation=%u"
                     " internals_last_output_deadline=%llu internals_last_entered_output_phase=%d"
                     " internals_last_reason=%s",
                     detached_terminal_rejects, nonwaiting_begin_rejects,
                     late_submitter_terminal_ignores, late_worker_terminal_ignores,
                     duplicate_local_begin_ignored, id_release_violations,
                     internals_output_dropped,
                     prelogin_current, prelogin_max_observed,
                     prelogin_expired_total, prelogin_rejected_total,
                     prelogin_completed_total, accepts_deferred_total,
                     prelogin_underflow_violations,
                     (unsigned long long)job_id_allocator().liveCount(),
                     (unsigned long long)job_id_allocator().issuedTotal(),
                     internals_txn.active ? 1 : 0,
                     (unsigned long long)internals_txn.peak_pending,
                     (unsigned long long)internals_txn.retained_bytes,
                     internals_txn.payload_omitted ? 1 : 0,
                     internals_txn.final_pending ? 1 : 0,
                     internals_txn.marker_queued, internals_txn.terminal_queued,
                     [](){ unsigned c=0; for (const InternalsTarget &t : internals_txn.targets) if (t.state==InternalsTarget::SEND_PENDING) ++c; return c; }(),
                     [](){ unsigned c=0; for (const InternalsTarget &t : internals_txn.targets) if (t.state==InternalsTarget::WAITING_REPLY) ++c; return c; }(),
                     [](){ unsigned c=0; for (const InternalsTarget &t : internals_txn.targets) if (t.state==InternalsTarget::REPLIED) ++c; return c; }(),
                     internals_txn.targets.empty() ? -1 : internals_txn.targets.front().fd,
                     internals_txn.targets.empty() ? 0u : internals_txn.targets.front().generation,
                     internals_txn.control_generation,
                     internals_txn.final_pending ? "output" : "collection",
                     (unsigned long long)internals_txn.deadline_mono,
                     (unsigned long long)internals_txn.output_deadline_mono,
                     internals_last.valid ? 1 : 0,
                     (unsigned long long)internals_last.peak_pending,
                     (unsigned long long)internals_last.retained_bytes,
                     internals_last.marker_queued, internals_last.terminal_queued,
                     internals_last.control_generation,
                     (unsigned long long)internals_last.output_deadline,
                     internals_last.entered_output_phase ? 1 : 0,
                     internals_last.reason);
            if (!cs->send_msg(TextMsg(summary))) {
                return false;
            }
        }
        for (map<unsigned int, Job *>::const_iterator it = jobs.begin();
                it != jobs.end(); ++it)
            if (!cs->send_msg(TextMsg(" " + dump_job(it->second, verbose)))) {
                return false;
            }
    } else if (cmd == "listrequests") {
        time_t now = time(nullptr);
        for (JobRequestsGroup * const group : job_requests) {
            if (group->l.empty()) {
                continue;
            }
            Job *oldest = group->l.front();
            time_t oldest_age_s = now - oldest->enqueueTime();
            if (oldest_age_s < 0) {
                oldest_age_s = 0;
            }
            /* Environment-narrowing observable: a multi-count request's
               siblings are pinned to the master's environment when the
               master is dispatched, so a queued sibling still carrying
               SEVERAL environment choices after that is a job the pinning
               missed.  multi_env exposes exactly that.  */
            size_t single_env = 0;
            size_t multi_env = 0;
            for (Job * const j : group->l) {
                if (j->environments().size() > 1) {
                    ++multi_env;
                } else {
                    ++single_env;
                }
            }
            const string msg = " submitter=" + group->submitter->nodeName()
                + " niceness=" + toString(group->niceness)
                + " count=" + toString(group->l.size())
                + " single_env=" + toString(single_env)
                + " multi_env=" + toString(multi_env)
                + " oldest_queue_age_s=" + toString((long)oldest_age_s);
            if (!cs->send_msg(TextMsg(msg))) {
                return false;
            }
        }
    } else if (cmd == "quit" || cmd == "exit") {
        handle_end(cs, nullptr);
        return false;
    } else if (cmd == "removecs" || cmd == "blockcs") {
        if (l.empty()) {
            if (!cs->send_msg(TextMsg(string("401 Sure. But which hosts?")))) {
                return false;
            }
        } else {
            for (list<string>::const_iterator si = l.begin(); si != l.end(); ++si) {
                if (cmd == "blockcs")
                    block_css.push_back(*si);
                for (CompileServer * const it : css) {
                    if (it->matches(*si)) {
                        if (cs->send_msg(TextMsg(string("removing host ") + *si))) {
                            handle_end(it, nullptr);
                        }
                        break;
                    }
                }
            }
        }
    } else if (cmd == "unblockcs") {
        if (l.empty()) {
            if(!cs->send_msg (TextMsg (string ("401 Sure. But which host?"))))
                return false;
        } else {
            for (list<string>::const_iterator si = l.begin(); si != l.end(); ++si) {
                for (list<string>::iterator it = block_css.begin(); it != block_css.end(); ++it) {
                    if (*si == *it) {
                        block_css.erase(it);
                        break;
                    }
                }
            }
        }
    } else if (cmd == "estimates") {
        /* Visibility for the estimate cache and the dispatch credit, so the
           cliffs these guard against are observable in production.  */
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "estimates: entries=%zu/%zu hits=%llu misses=%llu stale=%llu evictions=%llu",
                 file_runtime_estimates.size(), max_file_runtime_estimates,
                 (unsigned long long)runtime_estimate_hits,
                 (unsigned long long)runtime_estimate_misses,
                 (unsigned long long)runtime_estimate_stale,
                 (unsigned long long)runtime_estimate_evictions);
        if (!cs->send_msg(TextMsg(buf))) {
            return false;
        }
        snprintf(buf, sizeof(buf),
                 "dispatch_credit=%u effective=%u farm_slots=%u stall_report_after=%llus"
                 " jobs_admitted=%llu",
                 max_outstanding_dispatches, effective_dispatch_credit(),
                 cached_remote_farm_slots,
                 (unsigned long long)(max_outstanding_stall_msec / 1000),
                 (unsigned long long)jobs_admitted_total);
        if (!cs->send_msg(TextMsg(buf))) {
            return false;
        }
        for (CompileServer * const it : css) {
            if (it->outstandingDispatches() == 0) {
                continue;
            }
            snprintf(buf, sizeof(buf), " %s: outstanding_dispatches=%u oldest_unconfirmed=%llus",
                     it->nodeName().c_str(), it->outstandingDispatches(),
                     (unsigned long long)(it->oldestOutstandingDispatchMsec(icecream_monotonic_msec()) / 1000));
            if (!cs->send_msg(TextMsg(buf))) {
                return false;
            }
        }
    } else if (cmd == "internals") {
        if (internals_txn.active) {
            /* The legacy STATUS_TEXT reply has no request id: one fan-out
               at a time, and a concurrent request is told so with bounded
               deferrable output (a non-reading concurrent control must not
               block the loop either).  A hard transport failure returns
               false so the drain loop deletes this channel -- the existing
               control contract; we must NOT delete it here and then return
               to a caller that still holds it.  */
            {
                /* Atomic: reserve BOTH lines' exact wire bytes before
                   emitting either, so the concurrent control never gets a
                   busy row without its terminal.  */
                const std::string busy = "500 internals busy\n";
                const std::string term = "200 done";
                if (internals_exceeds(cs->pending_bytes(), kInternalsControlPendingCap,
                                      { internals_wire_len(busy), internals_wire_len(term) })) {
                    return false;   /* cannot fit the whole response: drop the connection */
                }
                if (internals_emit(cs, busy, kInternalsControlPendingCap) != ISEND_QUEUED
                        || internals_emit(cs, term, kInternalsControlPendingCap) != ISEND_QUEUED) {
                    return false;
                }
            }
            return true;
        }
        internals_txn.active = true;
        internals_txn.final_pending = false;
        internals_txn.control_fd = cs->fd;
        internals_txn.control_generation = cs->connectionGeneration();
        internals_txn.deadline_mono = icecream_monotonic_msec()
                                      + internals_deadline_msec;
        internals_txn.output_deadline_mono = internals_txn.deadline_mono
                                             + internals_deadline_msec;
        internals_txn.retained_bytes = 0;
        internals_txn.peak_pending = cs->pending_bytes();   /* baseline, not 0 */
        internals_txn.targets.clear();
        /* PASS 1: build value-only target snapshots (fd, generation,
           node name, initial state) WITHOUT sending anything.  */
        for (CompileServer * const it : css) {
            if (!l.empty()) {
                list<string>::const_iterator si;
                for (si = l.begin(); si != l.end(); ++si) {
                    if (it->matches(*si)) {
                        break;
                    }
                }
                if (si == l.end()) {
                    continue;
                }
            }
            InternalsTarget t;
            t.fd = it->fd;
            t.generation = it->connectionGeneration();
            t.node_name = it->nodeName();
            t.request_frame_seq = 0;
            t.state = InternalsTarget::SEND_PENDING;
            internals_txn.targets.push_back(t);
        }
        /* GLOBAL INVARIANT preflight, computed from the COMPLETE target
           snapshot and BEFORE any worker side effect: the whole required
           tail (a semantic fallback row for every target + one omission
           marker + terminal) must fit under BOTH the control pending cap
           and the cumulative transaction budget.  If a pre-filled control
           cannot hold the complete response, fail its exact generation now
           -- no GetInternalStatus has been queued to any worker yet.  */
        {
            const size_t tail = internals_required_tail_wire();
            if (internals_exceeds(cs->pending_bytes(), kInternalsControlPendingCap, { tail })
                    || tail > kInternalsRetainedCap) {
                internals_txn_clear("preflight-overflow");
                handle_end(cs, nullptr);
                return false;
            }
        }
        /* PASS 2: only now fan out.  Re-resolve each snapshot's exact
           fd/generation (a target may have disappeared between passes ->
           settle it disconnected); record the request frame sequence only
           after a successful nonblocking deferrable queue.  */
        for (InternalsTarget &t : internals_txn.targets) {
            CompileServer *target = internals_resolve(t.fd, t.generation);
            if (!target) {
                t.state = InternalsTarget::DISCONNECTED;
                continue;
            }
            if (internals_hold_armed) {
                /* Test seam (A.2 pre-delivery): GENUINELY defer the
                   request -- no send_msg, no queued frame.  The target is
                   held SEND_PENDING with nothing on the wire; tick
                   promotion and the STATUS_TEXT recheck both skip held
                   targets, so an early reply stays unsolicited.  The gated
                   internals-release later performs the real send.  */
                t.held = true;
                t.request_frame_seq = 0;
                t.state = InternalsTarget::SEND_PENDING;
                continue;
            }
            if (target->send_msg(GetInternalStatus(),
                                 MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable)) {
                t.request_frame_seq = target->framesQueued();
                t.state = InternalsTarget::SEND_PENDING;
            } else {
                t.state = InternalsTarget::DISCONNECTED;
            }
        }
        internals_txn_tick();
        {
            /* Id-accounting conservation, checked once per turn: every
               live id is either a remote job or a live local-monitor
               mapping.  A divergence is counted and reported, never
               silently tolerated.  */
            size_t local_live = 0;
            for (CompileServer * const c : css) {
                local_live += c->liveClientLocalJobIds().size();
            }
            if (job_id_allocator().liveCount() != jobs.size() + local_live) {
                ++id_release_violations;
                log_error() << "id accounting divergence: live="
                            << job_id_allocator().liveCount() << " jobs="
                            << jobs.size() << " local=" << local_live << endl;
            }
        }   /* instant completion for an empty set or
                                   already-flushed requests */

        /* No trailing 200 done here: the transaction emits it when every
           target is terminal or the whole-command deadline expires.  */
        return true;
    } else if (internals_hold_armed && cmd == "internals-release") {
        /* Test-only (A.2), recognized ONLY when the hold seam was armed at
           startup.  PHASE/DEADLINE gating (bigoracle 16:18, local-oracle
           16:26): settle the deadline first, then perform the real deferred
           send ONLY while the transaction is in its active COLLECTION phase
           (not yet final-pending output) and strictly before the collection
           deadline, and only for targets still genuinely held
           (held && SEND_PENDING).  A held target that already TIMED_OUT, or a
           transaction that has entered output, must never receive a late,
           uncorrelated GetInternalStatus.

           A SAME-owner release needs no explicit rejection here: the
           universal active-owner guard at the top of handle_line() tears down
           any command issued on the connection that owns the active response
           (before dispatch), so a same (fd,generation) release never reaches
           this branch and the A.2 gate necessarily drives release from a
           separate control connection.  */
        internals_txn_tick();   /* settle deadline / final-pending first */
        if (internals_txn.active && !internals_txn.final_pending
                && icecream_monotonic_msec() < internals_txn.deadline_mono) {
            for (InternalsTarget &t : internals_txn.targets) {
                if (!t.held || t.state != InternalsTarget::SEND_PENDING) {
                    continue;
                }
                CompileServer *tgt = internals_resolve(t.fd, t.generation);
                if (tgt && tgt->send_msg(GetInternalStatus(),
                                         MsgChannel::SendNonBlocking | MsgChannel::SendDeferrable)) {
                    /* real deferred send: record the actual queued sequence,
                       never lower it to the current flushed count.  */
                    t.request_frame_seq = tgt->framesQueued();
                    t.held = false;
                } else {
                    t.held = false;
                    t.state = InternalsTarget::DISCONNECTED;
                }
            }
        }
        return cs->send_msg(TextMsg(string("200 done")));
    } else if (cmd == "help") {
        if (!cs->send_msg(TextMsg(
                             "listcs\nlistblocks\nlistjobs [v|verbose]\nlistrequests\nestimates\nremovecs\nblockcs\nunblockcs\ninternals\nhelp\nquit"))) {
            return false;
        }
    } else {
        string txt = "Invalid command '";
        txt += m->text;
        txt += "'";

        if (!cs->send_msg(TextMsg(txt))) {
            return false;
        }
    }

    return cs->send_msg(TextMsg(string("200 done")));
}

// return false if some error occurred, leaves C open.  */
static bool try_login(CompileServer *cs, Msg *m)
{
    bool ret = true;

    switch (*m) {
    case Msg::LOGIN:
        cs->setType(CompileServer::DAEMON);
        ret = handle_login(cs, m);
        break;
    case Msg::MON_LOGIN:
        cs->setType(CompileServer::MONITOR);
        ret = handle_mon_login(cs, m);
        break;
    default:
        log_info() << "Invalid first message " << m->to_string() << endl;
        ret = false;
        break;
    }

    if (ret) {
        cs->setState(CompileServer::LOGGEDIN);
    } else {
        handle_end(cs, m);
    }

    delete m;
    return ret;
}

static bool handle_end(CompileServer *toremove, Msg *m)
{
    prelogin_release(toremove);
    std::set<CompileServer *> failed_fence_workers;

    /* Settle any internals-transaction involvement exactly once: a dead
       control cancels the whole fan-out (late replies revert to the
       unsolicited path); a dead target settles as DISCONNECTED for ITS
       exact generation -- the record is a value object and never holds
       the pointer being destroyed here.  */
    if (internals_txn.active) {
        if (toremove->fd == internals_txn.control_fd
                && toremove->connectionGeneration() == internals_txn.control_generation) {
            internals_txn_clear("control-disconnected");
        } else {
            for (InternalsTarget &t : internals_txn.targets) {
                if (t.fd == toremove->fd
                        && t.generation == toremove->connectionGeneration()
                        && (t.state == InternalsTarget::SEND_PENDING
                            || t.state == InternalsTarget::WAITING_REPLY)) {
                    t.state = InternalsTarget::DISCONNECTED;
                    break;
                }
            }
            if (internals_txn_all_terminal()) {
                internals_txn_finalize();
            }
        }
    }

#if DEBUG_SCHEDULER > 1
    trace() << "Handle_end " << toremove << " " << m << endl;
#else
    (void)m;
#endif

    switch (toremove->type()) {
    case CompileServer::MONITOR:
        assert(find(monitors.begin(), monitors.end(), toremove) != monitors.end());
        monitors.remove(toremove);
#if DEBUG_SCHEDULER > 1
        trace() << "handle_end(moni) " << monitors.size() << endl;
#endif
        break;
    case CompileServer::DAEMON:
        log_info() << "remove daemon " << toremove->nodeName() << endl;

        /* Reconcile the daemon's LOCAL monitor records: each live record
           gets its terminal and releases its scheduler-visible id -- the
           old teardown destroyed the map silently, leaking both.  */
        for (const int local_global_id : toremove->liveClientLocalJobIds()) {
            notify_monitors(new JobLocalDoneMsg(local_global_id));
            release_local_id((uint32_t)local_global_id);
        }

        notify_monitors(new MonStatsMsg(toremove->hostId(), "State:Offline\n"));

        /* A daemon disconnected.  We must remove it from the css list,
           and we have to delete all jobs scheduled on that daemon.
        There might be still clients connected running on the machine on which
         the daemon died.  We expect that the daemon dying makes the client
         disconnect soon too.  */
        css.remove(toremove);

        /* Unfortunately the job_requests queues are also tagged based on the daemon,
           so we need to clean them up also.  */

        for (list<JobRequestsGroup *>::iterator it = job_requests.begin(); it != job_requests.end();) {
            if ((*it)->submitter == toremove) {
                JobRequestsGroup *l = *it;
                list<Job *>::iterator jit;

                for (jit = l->l.begin(); jit != l->l.end(); ++jit) {
                    trace() << "STOP (DAEMON) FOR " << (*jit)->id() << endl;
                    notify_monitors(new MonJobDoneMsg(JobDoneMsg((*jit)->id(),  255)));

                    if ((*jit)->server()) {
                        (*jit)->server()->setBusyInstalling(0);
                    }

                    credit_dispatch_credit(*jit);
                    {
                        map<unsigned int, Job *>::iterator qit = jobs.find((*jit)->id());
                        if (qit != jobs.end()) {
                            remove_job_entry(qit);
                        }
                    }
                    delete(*jit);
                }

                delete l;
                it = job_requests.erase(it);
            } else {
                ++it;
            }
        }

        for (map<unsigned int, Job *>::iterator mit = jobs.begin(); mit != jobs.end();) {
            Job *job = mit->second;

            if (job->server() == toremove || job->submitter() == toremove) {
                /* Prepared P49 closes the dispatched-not-claimed gap: when the
                   submitter disappears, keep the assignment owned, queue an
                   ordered revoke, and release only on its matching result.
                   The job is detached before `toremove` can be destroyed. */
                if (job->submitter() == toremove
                        && job->server() && job->server() != toremove
                        && job->state() == Job::WAITINGFORCS
                        && job->assignmentFenced()
                        && (job->assignmentPhase() == Job::ASSIGNMENT_PREPARED
                            || job->assignmentPhase() == Job::ASSIGNMENT_READY
                            || job->assignmentPhase() == Job::ASSIGNMENT_REVOKE_PENDING)) {
                    job->detachSubmitter();
                    if (request_assignment_revoke(job)) {
                        trace() << "submitter gone; retaining fenced assignment "
                                << job->id() << " until worker terminal" << endl;
                        ++mit;
                        continue;
                    }
                    /* The worker channel rejected the ordered control send.
                       Do not infer release from the failed send alone.  Retain
                       the assignment until this turn tears down that exact
                       worker session, which is the established terminal
                       boundary for every job it owns. */
                    failed_fence_workers.insert(job->server());
                    ++mit;
                    continue;
                }

                if (job->submitter() == toremove
                        && job->server() && job->server() != toremove
                        && job->assignmentFenced()
                        && job->assignmentPhase() == Job::ASSIGNMENT_CLAIMED_OR_LATER) {
                    trace() << "submitter gone after fenced claim for job "
                            << job->id() << "; retaining for worker completion" << endl;
                    job->detachSubmitter();
                    ++mit;
                    continue;
                }

                /* A job already COMPILING on a DIFFERENT, live worker when
                   its submitter disconnects must NOT be deleted here: the
                   worker is physically running the compiler and its real
                   JobDone is still coming.  Deleting it frees the worker's
                   slot while the compile runs on (S could overcommit that
                   worker) and strands the eventual JobDone as an unknown
                   id.  Detach the dead submitter and retain the job --
                   add_job_stats and the FROM_SERVER JobDone path tolerate a
                   null submitter -- so the worker's completion reconciles
                   it exactly once.  (A dispatched-but-NOT-started job is a
                   separate case a future worker-side cancel exchange will
                   cover; for now it is left to the worker's own input-wait
                   timeout.)  */
                if (job->submitter() == toremove
                        && job->server() && job->server() != toremove
                        && job->state() == Job::COMPILING) {
                    /* COMPILING means JobBegin already credited the
                       dispatch debit; a retained job must never carry one
                       into detached life, where nothing could return it.  */
                    assert(!job->dispatchOutstanding());
                    trace() << "submitter gone but job " << job->id()
                            << " is COMPILING on " << job->server()->nodeName()
                            << "; retaining until the worker completes it" << endl;
                    job->detachSubmitter();
                    ++mit;
                    continue;
                }

                trace() << "STOP (DAEMON2) FOR " << mit->first << endl;
                notify_monitors(new MonJobDoneMsg(JobDoneMsg(job->id(),  255)));

                /* If this job is removed because the submitter is removed
                also remove the job from the servers joblist.  */
                if (job->server() && job->server() != toremove) {
                    job->server()->removeJob(job);
                }

                if (job->server()) {
                    job->server()->setBusyInstalling(0);
                }

                credit_dispatch_credit(job);
                {
                    map<unsigned int, Job *>::iterator next = mit;
                    ++next;
                    remove_job_entry(mit);
                    mit = next;
                }
                delete job;
            } else {
                ++mit;
            }
        }

        for (CompileServer *failed_worker : failed_fence_workers) {
            trace() << "tearing down worker after failed ordered revoke: "
                    << failed_worker->nodeName() << endl;
            handle_end(failed_worker, nullptr);
        }

        for (CompileServer * const cs : css) {
            cs->eraseCSFromBlacklist(toremove);
        }

        break;
    case CompileServer::LINE:
        /* Forced control teardown MUST NOT block.  A stopped or slow reader
           can already hold a large pending backlog -- e.g. an internals
           response that hit its output deadline (~155 KiB in the A.3 gate) --
           and the default send_msg flag is SendBlocking, whose flush_writebuf
           waits in a 30s POLLOUT poll on the full channel.  That would let a
           transaction the output deadline just cleared go on blocking the
           whole scheduler well past its declared deadline.  So: skip the
           farewell entirely when any output is still pending, and otherwise
           attempt it strictly nonblocking.  Either way the exact control
           channel is deleted in this same bounded turn (below). */
        if (toremove->pending_bytes() == 0) {
            toremove->send_msg(TextMsg("200 Good Bye!"), MsgChannel::SendNonBlocking);
        }
        controls.remove(toremove);

        break;
    default:
        trace() << "remote end had UNKNOWN type?" << endl;
        break;
    }

    /* Drop any half-finished multi-job expansions belonging to this peer.
       The map is keyed by the channel fd, and the kernel reuses a closed fd
       for the next accept(): without this erase a record outlives its owner
       and the next connection to land on the same number inherits it, so a
       request nobody made is expanded against an unrelated -- possibly still
       mid-handshake -- peer.  The lazy sweep in expand_pending_requests()
       cannot cover that case: it only drops records whose fd is ABSENT from
       fd2cs, and a reused fd is present again.  */
    pending_expansions.erase(toremove->fd);

    fd2cs.erase(toremove->fd);
    delete toremove;
    return true;
}

/* Returns TRUE if C was not closed.  */
static bool handle_activity(CompileServer *cs)
{
    Msg *m;
    bool ret = true;
    m = cs->get_msg(0, true);

    if (!m) {
        handle_end(cs, m);
        return false;
    }

    /* First we need to login.  */
    if (cs->state() == CompileServer::CONNECTED) {
        return try_login(cs, m);
    }

    switch (*m) {
    case Msg::JOB_BEGIN:
        ret = handle_job_begin(cs, m);
        break;
    case Msg::JOB_DONE:
        ret = handle_job_done(cs, m);
        break;
    case Msg::ASSIGN_READY:
        ret = handle_assignment_ready(cs, m);
        break;
    case Msg::REVOKE_RESULT:
        ret = handle_assignment_terminal(cs, m);
        break;
    case Msg::PING:
        ret = handle_ping(cs, m);
        break;
    case Msg::STATS:
        ret = handle_stats(cs, m);
        break;
    case Msg::END:
        handle_end(cs, m);
        ret = false;
        break;
    case Msg::JOB_LOCAL_BEGIN:
        ret = handle_local_job(cs, m);
        break;
    case Msg::JOB_LOCAL_DONE:
        ret = handle_local_job_done(cs, m);
        break;
    case Msg::LOGIN:
        ret = handle_relogin(cs, m);
        break;
    case Msg::TEXT:
        ret = handle_line(cs, m);
        break;
    case Msg::GET_CS:
        ret = handle_cs_request(cs, m);
        break;
    case Msg::BLACKLIST_HOST_ENV:
        ret = handle_blacklist_host_env(cs, m);
        break;
    case Msg::STATUS_TEXT: {
        InternalsTarget *hit = nullptr;
        if (internals_txn.active && !internals_txn.final_pending) {
            for (InternalsTarget &t : internals_txn.targets) {
                if (t.fd != cs->fd || t.generation != cs->connectionGeneration()) {
                    continue;
                }
                /* Same-turn flush/reply race: the ready-fd loop flushes this
                   channel's queued request and then drains its inbound in the
                   SAME turn, before internals_txn_tick() promotes the target
                   next turn.  Recheck the exact frame delivery here, so a
                   reply whose request frame has already left is accepted
                   rather than mis-classified as unsolicited and timed out.  */
                if (t.state == InternalsTarget::WAITING_REPLY
                        || (t.state == InternalsTarget::SEND_PENDING
                            && !t.held
                            && cs->framesFlushed() >= t.request_frame_seq)) {
                    t.state = InternalsTarget::WAITING_REPLY;
                    hit = &t;
                    break;
                }
            }
        }
        if (hit) {
            hit->state = InternalsTarget::REPLIED;
            CompileServer *control = internals_resolve(internals_txn.control_fd,
                                                       internals_txn.control_generation);
            if (control) {
                /* Forward the worker row through the SAME bounded, checked
                   path as every other row: per-target cap, whole-txn
                   retained cap (exact wire bytes), and the control cap
                   minus the terminal reserve.  A DROP emits one
                   deterministic omission marker; a hard ERROR settles the
                   control's exact generation.  */
                std::string text = static_cast<StatusTextMsg*>(m)->text;
                if (text.size() > kInternalsPerTargetCap) {
                    text.resize(kInternalsPerTargetCap);
                    internals_txn.payload_omitted = true;   /* truncated payload */
                }
                std::string row = hit->node_name + ": " + text;
                if (row.empty() || row[row.size() - 1] != '\n') { row += '\n'; }
                bool fail = false;
                /* Optional worker PAYLOAD: admitted only if it fits
                   alongside the EXACT still-outstanding required tail
                   (terminal + marker + a fallback row for every remaining
                   non-REPLIED target -- hit is already REPLIED, so it is
                   excluded).  Enforced against BOTH the live pending bytes
                   and the cumulative transaction budget, so the required
                   tail can never be squeezed out.  A drop only sets the
                   one transaction-level omission flag.  */
                const size_t row_wire = internals_wire_len(row);
                const size_t tail = internals_required_tail_wire();
                if (internals_exceeds(control->pending_bytes(), kInternalsControlPendingCap,
                                      { row_wire, tail })
                        || internals_exceeds(internals_txn.retained_bytes, kInternalsRetainedCap,
                                             { row_wire, tail })) {
                    internals_txn.payload_omitted = true;
                } else {
                    switch (internals_emit(control, row, kInternalsControlPendingCap)) {
                    case ISEND_QUEUED:
                        internals_txn.retained_bytes += row_wire;
                        break;
                    case ISEND_DROPPED:
                        internals_txn.payload_omitted = true;
                        break;
                    case ISEND_ERROR:
                        fail = true;
                        break;
                    }
                }
                if (fail) {
                    handle_end(control, nullptr);
                    break;   /* out of the STATUS_TEXT case */
                }
            }
            if (internals_txn.active && internals_txn_all_terminal()) {
                internals_txn_finalize();
            }
        } else {
            /* Unsolicited, duplicate, or pre-delivery status: legacy
               logging; transaction accounting untouched.  */
            log_info() << "StatusTextMsg from " << cs->nodeName() << ": " << static_cast<StatusTextMsg*>(m)->text << endl;
        }
        ret = true;
        break;
    }
    default:
        log_info() << "Invalid message type arrived " << m->to_string() << endl;
        handle_end(cs, m);
        ret = false;
        break;
    }

    delete m;
    return ret;
}

static int open_broad_listener(int port, const string &interface)
{
    int listen_fd;
    struct sockaddr_in myaddr;

    if ((listen_fd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
        log_perror("socket()");
        return -1;
    }

    int optval = 1;

    if (setsockopt(listen_fd, SOL_SOCKET, SO_BROADCAST, &optval, sizeof(optval)) < 0) {
        log_perror("setsockopt()");
        return -1;
    }

    if (!build_address_for_interface(myaddr, interface, port)) {
        return -1;
    }

    if (::bind(listen_fd, (struct sockaddr *) &myaddr, sizeof(myaddr)) < 0) {
        log_perror("bind()");
        return -1;
    }

    return listen_fd;
}

static int open_tcp_listener(short port, const string &interface)
{
    int fd;
    struct sockaddr_in myaddr;

    if ((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        log_perror("socket()");
        return -1;
    }

    int optval = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        log_perror("setsockopt()");
        return -1;
    }

    /* Although we poll() on fd we need O_NONBLOCK, due to
       possible network errors making accept() block although poll() said
       there was some activity.  */
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
        log_perror("fcntl()");
        return -1;
    }

    if (!build_address_for_interface(myaddr, interface, port)) {
        return -1;
    }

    if (::bind(fd, (struct sockaddr *) &myaddr, sizeof(myaddr)) < 0) {
        log_perror("bind()");
        return -1;
    }

    if (listen(fd, 1024) < 0) {
        log_perror("listen()");
        return -1;
    }

    return fd;
}

static void usage(const std::string reason = "")
{
    if (! reason.empty()) {
        cerr << reason << endl;
    }

    cerr << "ICECREAM scheduler " VERSION "\n";
    cerr << "usage: icecc-scheduler [options] \n"
         << "Options:\n"
         << "  -n, --netname <name>\n"
         << "  -i, --interface <net_interface>\n"
         << "  -p, --port <port>\n"
         << "  -h, --help\n"
         << "  -l, --log-file <file>\n"
         << "  -d, --daemonize\n"
         << "  -u, --user-uid\n"
         << "  -v[v[v]]]\n"
         << "  -r, --persistent-client-connection\n"
         << "  -a, --algorithm <name>\n"
         << "  --max-outstanding-dispatches <n>   per-submitter unconfirmed dispatch credit (1-1024, default 32)\n"
         << "  --dispatch-stall-report-after <sec>  report (do not remove) a submitter whose oldest unconfirmed dispatch exceeds this; its assignment and worker reservation are retained (10-3600, default 180)\n"
         << "  --assignment-fence-mode <legacy|advisory|enforcing-compat|strict-nonce>\n"
         << "                                      P49 assignment policy (default legacy; strict-nonce requires P50)\n"
         << "  --assignment-fence-strict          compatibility alias for enforcing-compat\n"
         << endl;

    exit(1);
}

static void trigger_exit(int signum)
{
    if (!exit_main_loop) {
        exit_main_loop = true;
    } else {
        // hmm, we got killed already. try better
        static const char msg[] = "forced exit.\n";
        ignore_result(write(STDERR_FILENO, msg, strlen( msg )));
        _exit(1);
    }

    // make BSD happy
    signal(signum, trigger_exit);
}

static void handle_scheduler_announce(const char* buf, const char* netname, bool persistent_clients, struct sockaddr_in broad_addr)
{
    /* Another scheduler is announcing it's running, disconnect daemons if it has a better version
       or the same version but was started earlier. */
    time_t other_time;
    int other_protocol_version;
    string other_netname;
    Broadcasts::getSchedulerVersionData(buf, &other_protocol_version, &other_time, &other_netname);
    trace() << "Received scheduler announcement from " << inet_ntoa(broad_addr.sin_addr)
            << ":" << ntohs(broad_addr.sin_port)
            << " (version " << int(other_protocol_version) << ", netname " << other_netname << ")" << endl;
    if (other_protocol_version >= 36)
    {
        if (other_netname == netname)
        {
            if (other_protocol_version > PROTOCOL_VERSION || (other_protocol_version == PROTOCOL_VERSION && other_time < starttime))
            {
                if (!persistent_clients){
                    log_info() << "Scheduler from " << inet_ntoa(broad_addr.sin_addr)
                        << ":" << ntohs(broad_addr.sin_port)
                        << " (version " << int(other_protocol_version) << ") has announced itself as a preferred"
                        " scheduler, disconnecting all connections." << endl;
                    if (!css.empty() || !monitors.empty())
                    {
                        while (!css.empty())
                        {
                            handle_end(css.front(), nullptr);
                        }
                        while (!monitors.empty())
                        {
                            handle_end(monitors.front(), nullptr);
                        }
                    }
                }
            }
        }
    }
}

int main(int argc, char *argv[])
{
    int listen_fd, remote_fd, broad_fd, text_fd;
    struct sockaddr_in remote_addr;
    socklen_t remote_len;
    const char *netname = "ICECREAM";
    bool detach = false;
    bool persistent_clients = false;
    int debug_level = Error;
    string logfile;
    uid_t user_uid;
    gid_t user_gid;
    int warn_icecc_user_errno = 0;
    SchedulerAlgorithmName scheduler_algo = SchedulerAlgorithmName::FASTEST;

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

    while (true) {
        int option_index = 0;
        static const struct option long_options[] = {
            { "netname", 1, nullptr, 'n' },
            { "help", 0, nullptr, 'h' },
            { "persistent-client-connection", 0, nullptr, 'r' },
            { "interface", 1, nullptr, 'i' },
            { "port", 1, nullptr, 'p' },
            { "daemonize", 0, nullptr, 'd'},
            { "log-file", 1, nullptr, 'l'},
            { "user-uid", 1, nullptr, 'u'},
            { "algorithm", 1, nullptr, 'a' },
            { "max-outstanding-dispatches", 1, nullptr, 1001 },
            { "dispatch-stall-report-after", 1, nullptr, 1002 },
            /* Compatibility alias for the previous spelling, from when this
               bound removed the submitter instead of reporting it.  */
            { "dispatch-stall-timeout", 1, nullptr, 1002 },
            { "assignment-fence-strict", 0, nullptr, 1003 },
            { "assignment-fence-mode", 1, nullptr, 1004 },
            { nullptr, 0, nullptr, 0 }
        };

        const int c = getopt_long(argc, argv, "n:i:p:hl:vdru:a:", long_options, &option_index);

        if (c == -1) {
            break;    // eoo
        }

        switch (c) {
        case 0:
            (void) long_options[option_index].name;
            break;
        case 'd':
            detach = true;
            break;
        case 'r':
            persistent_clients= true;
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
                netname = optarg;
            } else {
                usage("Error: -n requires argument");
            }

            break;
        case 'i':

            if (optarg && *optarg) {
                string interface = optarg;
                if (interface.empty()) {
                    usage("Error: Invalid network interface specified");
                }

                scheduler_interface = interface;
            } else {
                usage("Error: -i requires argument");
            }

            break;
        case 'p':

            if (optarg && *optarg) {
                scheduler_port = atoi(optarg);

                if (0 == scheduler_port) {
                    usage("Error: Invalid port specified");
                }
            } else {
                usage("Error: -p requires argument");
            }

            break;
        case 'u':

            if (optarg && *optarg) {
                struct passwd *pw = getpwnam(optarg);

                if (!pw) {
                    usage("Error: -u requires a valid username");
                } else {
                    user_uid = pw->pw_uid;
                    user_gid = pw->pw_gid;
                    warn_icecc_user_errno = 0;

                    if (!user_gid || !user_uid) {
                        usage("Error: -u <username> must not be root");
                    }
                }
            } else {
                usage("Error: -u requires a valid username");
            }

            break;
        case 'a':

            if (optarg && *optarg) {
                string algorithm_name = optarg;
                std::transform(
                        algorithm_name.begin(),
                        algorithm_name.end(),
                        algorithm_name.begin(),
                        ::tolower);

                if (algorithm_name == "random") {
                    scheduler_algo = SchedulerAlgorithmName::RANDOM;
                } else if (algorithm_name == "round_robin") {
                    scheduler_algo = SchedulerAlgorithmName::ROUND_ROBIN;
                } else if (algorithm_name == "least_busy") {
                    scheduler_algo = SchedulerAlgorithmName::LEAST_BUSY;
                } else if (algorithm_name == "fastest") {
                    scheduler_algo = SchedulerAlgorithmName::FASTEST;
                } else {
                    usage("Error: " + algorithm_name + " is an unknown scheduler algorithm.");
                }

            } else {
                usage("Error: -s requires a valid scheduler name");
            }

            break;

        case 1001:
        case 1002: {
            /* Strict integer parsing: '32junk' must be rejected, not read
               as 32.  errno, at-least-one-digit and full consumption are
               all checked.  */
            const char *name = (c == 1001) ? "--max-outstanding-dispatches"
                                           : "--dispatch-stall-report-after";
            if (!optarg || !*optarg) {
                usage(string("Error: ") + name + " requires argument");
            }
            errno = 0;
            char *end = nullptr;
            const long v = strtol(optarg, &end, 10);
            if (errno != 0 || end == optarg || *end != '\0') {
                usage(string("Error: ") + name + " requires a plain integer");
            }
            if (c == 1001) {
                if (v < 1 || v > 1024) {
                    usage("Error: --max-outstanding-dispatches must be 1..1024");
                }
                max_outstanding_dispatches = (unsigned int)v;
            } else {
                if (v < 10 || v > 3600) {
                    usage("Error: --dispatch-stall-report-after must be 10..3600 seconds");
                }
                max_outstanding_stall_msec = (uint64_t)v * 1000;
            }
            break;
        }

        case 1003:
            assignment_fence_mode = ConfCSMsg::EnforcingCompat;
            break;

        case 1004: {
            const string mode = optarg ? optarg : "";
            if (mode == "legacy") {
                assignment_fence_mode = ConfCSMsg::Legacy;
            } else if (mode == "advisory") {
                assignment_fence_mode = ConfCSMsg::Advisory;
            } else if (mode == "enforcing-compat") {
                assignment_fence_mode = ConfCSMsg::EnforcingCompat;
            } else if (mode == "strict-nonce") {
                assignment_fence_mode = ConfCSMsg::StrictNonce;
            } else {
                usage("Error: unknown --assignment-fence-mode: " + mode);
            }
            break;
        }

        default:
            usage();
        }
    }

    if (warn_icecc_user_errno != 0) {
        log_errno("No icecc user on system. Falling back to nobody.", errno);
    }

    if (getuid() == 0) {
        if (!logfile.size() && detach) {
            if (mkdir("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)) {
                if (errno == EEXIST) {
                    if (-1 == chmod("/var/log/icecc", S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)){
                        log_perror("chmod() failure");
                    }

                    if (-1 == chown("/var/log/icecc", user_uid, user_gid)){
                        log_perror("chown() failure");
                    }
                }
            }

            logfile = "/var/log/icecc/scheduler.log";
        }

        if (setgroups(0, nullptr) < 0) {
            log_perror("setgroups() failed");
            return 1;
        }

        if (setgid(user_gid) < 0) {
            log_perror("setgid() failed");
            return 1;
        }

        if (setuid(user_uid) < 0) {
            log_perror("setuid() failed");
            return 1;
        }
    }

    setup_debug(debug_level, logfile);

    scheduler_assignment_epoch = fresh_assignment_word();

    log_info() << "ICECREAM scheduler " VERSION " starting up, port " << scheduler_port << endl;
    log_info() << "dispatch credit: " << max_outstanding_dispatches
               << " unconfirmed per submitter (farm-clamped at runtime), stall reported after "
               << (max_outstanding_stall_msec / 1000) << "s" << endl;
    log_info() << "assignment fence: "
               << assignment_mode_name(assignment_fence_mode)
               << " epoch=" << scheduler_assignment_epoch << endl;
    log_info() << "Debug level: " << debug_level << endl;

    if (detach) {
        if (daemon(0, 0) != 0) {
            log_errno("Failed to detach.", errno);
            exit(1);
        }
    }

    listen_fd = open_tcp_listener(scheduler_port, scheduler_interface);

    if (listen_fd < 0) {
        return 1;
    }

    text_fd = open_tcp_listener(scheduler_port + 1, scheduler_interface);

    if (text_fd < 0) {
        return 1;
    }

    broad_fd = open_broad_listener(scheduler_port, scheduler_interface);

    if (broad_fd < 0) {
        return 1;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        log_warning() << "signal(SIGPIPE, ignore) failed: " << strerror(errno) << endl;
        return 1;
    }

    starttime = time(nullptr);
    if( getenv( "ICECC_FAKE_STARTTIME" ) != nullptr )
        starttime -= 1000;

    ofstream pidFile;
    string progName = argv[0];
    progName = find_basename(progName);
    pidFilePath = string(RUNDIR) + string("/") + progName + string(".pid");
    pidFile.open(pidFilePath.c_str());
    pidFile << getpid() << endl;
    pidFile.close();

    signal(SIGTERM, trigger_exit);
    signal(SIGINT, trigger_exit);
    signal(SIGALRM, trigger_exit);

    log_info() << "scheduler ready, algorithm: " <<  scheduler_algo << endl;

    time_t next_listen = 0;
    prelogin_read_overrides();

    Broadcasts::broadcastSchedulerVersion(scheduler_port, netname, starttime);
    last_announce = starttime;

    while (!exit_main_loop) {
        int timeout = prune_servers();

        /* Dispatch in bounded batches: draining an arbitrarily deep request
           queue before returning to poll() starves every other scheduler
           duty (control connections, daemon traffic, monitor feeds) for the
           whole drain -- measured at 10+ seconds for a 20k-request flood.
           After a full batch, poll with a zero timeout so pending fds are
           serviced and dispatching resumes immediately.  */
        int dispatch_batch = 128;
        bool more_dispatch = false;
        while (empty_queue(scheduler_algo)) {
            if (--dispatch_batch <= 0) {
                more_dispatch = true;
                break;
            }
        }
        if (more_dispatch) {
            timeout = 0;
        } else {
            /* The deferred-send deadline must be able to shorten a poll
               timeout computed BEFORE dispatch created new backlog, or the
               30s bound can silently stretch to prune_servers()'s full
               36s ceiling.  Keyed on the armed FLAG, not on an age value:
               an age of zero is ambiguous during the deadline's first
               second and would leave the stale timeout in place.  */
            const uint64_t now_msec = icecream_monotonic_msec();
            for (CompileServer * const cs : css) {
                if (!cs->deferred_output_armed()) {
                    continue;
                }
                const uint64_t deadline = cs->deferred_output_deadline_msec();
                const int remaining = deadline <= now_msec
                    ? 0 : (int)((deadline - now_msec + 999) / 1000);
                timeout = std::min(timeout, remaining);
            }
        }

        /* Announce ourselves from time to time, to make other possible schedulers disconnect
           their daemons if we are the preferred scheduler (daemons with version new enough
           should automatically select the best scheduler, but old daemons connect randomly). */
        if (last_announce + 120 < time(nullptr)) {
            Broadcasts::broadcastSchedulerVersion(scheduler_port, netname, starttime);
            last_announce = time(nullptr);
        }

        vector< pollfd > pollfds;
        pollfds.reserve( fd2cs.size() + css.size() + 5 );
        pollfd pfd; // tmp variable

        /* The control (text) listener is polled CONTINUOUSLY; only the
           daemon listener is throttled to one accept per second.  Gating
           text_fd behind next_listen made rapid control connections (the
           test gates poll listcs/listjobs) unacceptable during the re-arm
           interval, and nothing woke poll to re-arm on time.  */
        pfd.fd = text_fd;
        pfd.events = POLLIN;
        pollfds.push_back( pfd );

        const bool daemon_listener_armed = time(nullptr) >= next_listen;
        if (daemon_listener_armed) {
            pfd.fd = listen_fd;
            pfd.events = POLLIN;
            pollfds.push_back( pfd );
        }

        pfd.fd = broad_fd;
        pfd.events = POLLIN;
        pollfds.push_back( pfd );

        /* Ingress budget for THIS loop iteration.  It is spent where reads
           actually happen -- the post-poll read_a_bit() paths below -- not
           before poll(), where has_msg() only sees userspace buffers and a
           budget governs nothing.  */
        inbound_budget_remaining = max_inbound_units_per_loop;

        /* Pending-first turn: last iteration's new traffic spent the whole
           budget before half-finished requests could resume.  Serve them
           now, from the fresh budget, before this iteration's ingress --
           strict alternation, so neither side can starve the other.  */
        if (pending_expansions_starved) {
            pending_expansions_starved = false;
            /* ONE quantum only (<= max_jobs_per_expansion_step of the fresh
               max_inbound_units_per_loop budget): guaranteed pending
               progress, with at least half the budget left for ingress and
               the control plane.  Serving every pending connection here let
               two long expansions consume the entire budget turn after
               turn -- rotation among pending fds is not alternation between
               pending and fresh work.  */
            if (expand_pending_requests(true)) {
                has_buffered_inbound = true;
            }
        }
        /* NOT a per-iteration local: the post-poll reads below set it, while
           the timeout decision that consumes it runs BEFORE poll() -- so it
           must carry across the iteration boundary or the zero-timeout
           re-poll never happens and each iteration stalls in poll() after
           spending its budget.  */
        for (map<int, CompileServer *>::const_iterator it = fd2cs.begin(); it != fd2cs.end();) {
            int i = it->first;
            CompileServer *cs = it->second;
            bool ok = true;
            ++it;

            if (cs->has_msg()) {
                has_buffered_inbound = true;
            }

            if (ok) {
                pfd.fd = i;
                pfd.events = POLLIN;

                /* Dispatch replies queued by a deferrable send while the
                   daemon's receive buffer was full; ask poll() to tell us when
                   they can be flushed.  */
                if (cs->has_pending_write()) {
                    pfd.events |= POLLOUT;
                }

                pollfds.push_back( pfd );
            }
        }

        list<CompileServer *> cs_in_tsts;
        for (CompileServer * const cs : css)
        {
            if (cs->getConnectionInProgress())
            {
                int csInFd = cs->getInFd();
                cs_in_tsts.push_back(cs);
                pfd.fd = csInFd;
                pfd.events = POLLIN | POLLOUT;
                pollfds.push_back( pfd );
            }
        }

        if (has_buffered_inbound) {
            /* Parsed messages are waiting in userspace buffers; poll() knows
               nothing about them.  Service fds without sleeping.  */
            timeout = 0;
        }
        const bool service_buffered = has_buffered_inbound;
        has_buffered_inbound = false;   // the post-poll reads below re-arm it

        if (!daemon_listener_armed) {
            const time_t remaining = next_listen - time(nullptr);
            const time_t secs = remaining > 0 ? remaining : 0;
            if (timeout < 0 || secs < timeout) {
                timeout = secs;
            }
        }
        internals_txn_tick();
        {
            /* Id-accounting conservation, checked once per turn: every
               live id is either a remote job or a live local-monitor
               mapping.  A divergence is counted and reported, never
               silently tolerated.  */
            size_t local_live = 0;
            for (CompileServer * const c : css) {
                local_live += c->liveClientLocalJobIds().size();
            }
            if (job_id_allocator().liveCount() != jobs.size() + local_live) {
                ++id_release_violations;
                log_error() << "id accounting divergence: live="
                            << job_id_allocator().liveCount() << " jobs="
                            << jobs.size() << " local=" << local_live << endl;
            }
        }
        if (internals_txn.active) {
            const uint64_t now_mono = icecream_monotonic_msec();
            /* Phase-relevant deadline: the collection deadline governs
               while waiting for replies, the OUTPUT deadline once the
               terminal frame is queued.  Capping by the (already-elapsed)
               collection deadline in FINAL_PENDING selected a zero timeout
               and spun the loop until the output drained -- exactly the
               stalled-control case.  */
            const uint64_t phase_deadline = internals_txn.final_pending
                ? internals_txn.output_deadline_mono
                : internals_txn.deadline_mono;
            const uint64_t left = phase_deadline > now_mono
                ? phase_deadline - now_mono : 0;
            const time_t secs = (time_t)((left + 999) / 1000);
            if (timeout < 0 || secs < timeout) {
                timeout = secs;
            }
        }
        if (accept_repoll) {
            /* A quantum-limited accept pass left connections pending: run
               exactly one full turn of other work, then re-poll without
               sleeping.  */
            accept_repoll = false;
            timeout = 0;
        }
        int active_fds = poll(pollfds.data(), pollfds.size(), timeout * 1000);
        int poll_errno = errno;

        if (active_fds < 0 && errno == EINTR) {
            reset_debug_if_needed(); // we possibly got SIGHUP
            continue;
        }
        reset_debug_if_needed();

        if (active_fds < 0) {
            errno = poll_errno;
            log_perror("poll()");
            return 1;
        }

        if (pollfd_is_set(pollfds, listen_fd, POLLIN)) {
            active_fds--;
            bool pending_connections = true;
            unsigned int accepted_this_turn = 0;

            while (pending_connections) {
                /* Accept QUANTUM: the old loop drained the whole backlog
                   in one scheduler turn -- accepts, channel construction,
                   and per-connection bookkeeping unbounded by any turn
                   budget, starving control queries and existing daemons
                   under a connection flood.  After Q accepts the listener
                   yields; the next poll runs with a zero timeout so one
                   full turn of other work happens before it is serviced
                   again.  */
                if (accepted_this_turn >= accept_quantum) {
                    ++accepts_deferred_total;
                    accept_repoll = true;
                    break;
                }
                remote_len = sizeof(remote_addr);
                remote_fd = accept(listen_fd,
                                   (struct sockaddr *) &remote_addr,
                                   &remote_len);

                if (remote_fd < 0) {
                    pending_connections = false;
                }

                if (remote_fd < 0 && errno != EAGAIN && errno != EINTR
                        && errno != EWOULDBLOCK) {
                    log_perror("accept()");
                    /* don't quit because of ECONNABORTED, this can happen during
                     * floods  */
                }

                if (remote_fd >= 0) {
                    ++accepted_this_turn;
                    /* Population bound: at the cap, the socket is refused
                       outright -- no CompileServer, no protocol buffers,
                       no map entry.  */
                    if (prelogin_current >= prelogin_max_peers) {
                        ++prelogin_rejected_total;
                        close(remote_fd);
                        continue;
                    }
                    CompileServer *cs = new CompileServer(remote_fd, (struct sockaddr *) &remote_addr, remote_len, false);
                    trace() << "accepted " << cs->name << endl;
                    cs->last_talk = time(nullptr);

                    if (!cs->protocol) { // protocol mismatch
                        delete cs;
                        continue;
                    }

                    /* Whole-handshake lease: negotiation AND the first
                       valid login must complete before this absolute
                       monotonic deadline; partial bytes do not refresh
                       it.  last_talk stays telemetry only.  */
                    cs->setPreloginDeadline(icecream_monotonic_msec()
                                            + prelogin_lease_msec);
                    cs->setPreloginAccounted(true);
                    ++prelogin_current;
                    if (prelogin_current > prelogin_max_observed) {
                        prelogin_max_observed = prelogin_current;
                    }

                    fd2cs[cs->fd] = cs;

                    drain_connection(cs, &has_buffered_inbound);
                }
            }

            next_listen = time(nullptr) + 1;
        }

        if (active_fds && pollfd_is_set(pollfds, text_fd, POLLIN)) {
            active_fds--;
            remote_len = sizeof(remote_addr);
            remote_fd = accept(text_fd,
                               (struct sockaddr *) &remote_addr,
                               &remote_len);

            if (remote_fd < 0 && errno != EAGAIN && errno != EINTR) {
                log_perror("accept()");
                /* Don't quit the scheduler just because a debugger couldn't
                   connect.  */
            }

            if (remote_fd >= 0) {
                CompileServer *cs = new CompileServer(remote_fd, (struct sockaddr *) &remote_addr, remote_len, true);
                fd2cs[cs->fd] = cs;

                if (!handle_control_login(cs)) {
                    handle_end(cs, nullptr);
                    continue;
                }

                drain_connection(cs, &has_buffered_inbound);
            }
        }

        if (active_fds && pollfd_is_set(pollfds, broad_fd, POLLIN)) {
            active_fds--;
            char buf[Broadcasts::BROAD_BUFLEN + 1];
            struct sockaddr_in broad_addr;
            socklen_t broad_len = sizeof(broad_addr);
            /* We can get either a daemon request for a scheduler (1 byte) or another scheduler
               announcing itself (4 bytes + time). */

            int buflen = recvfrom(broad_fd, buf, Broadcasts::BROAD_BUFLEN, 0, (struct sockaddr *) &broad_addr,
                    &broad_len);
            if (buflen < 0 || buflen > Broadcasts::BROAD_BUFLEN){
                int err = errno;
                log_perror("recvfrom()");

                /* Some linux 2.6 kernels can return from select with
                   data available, and then return from read() with EAGAIN
                   even on a blocking socket (breaking POSIX).  Happens
                   when the arriving packet has a wrong checksum.  So
                   we ignore EAGAIN here, but still abort for all other errors. */
                if (err != EAGAIN && err != EWOULDBLOCK) {
                    return -1;
                }
            }
            int daemon_version;
            if (DiscoverSched::isSchedulerDiscovery(buf, buflen, &daemon_version)) {
                /* Daemon is searching for a scheduler, only answer if daemon would be able to talk to us. */
                if ( daemon_version >= MIN_PROTOCOL_VERSION){
                    log_info() << "broadcast from " << inet_ntoa(broad_addr.sin_addr)
                        << ":" << ntohs(broad_addr.sin_port)
                        << " (version " << daemon_version << ")\n";
                    int reply_len = DiscoverSched::prepareBroadcastReply(buf, netname, starttime);
                    if (sendto(broad_fd, buf, reply_len, 0,
                                (struct sockaddr *) &broad_addr, broad_len) != reply_len) {
                        log_perror("sendto()");
                    }
                }
            }
            else if(Broadcasts::isSchedulerVersion(buf, buflen)) {
                handle_scheduler_announce(buf, netname, persistent_clients, broad_addr);
            }
        }

        /* Snapshot the fds and re-look-up each one: a handler can erase ANY
           entry (duplicate-login eviction, removecs, monitor teardown), not
           just the one being served, so a retained iterator -- even an
           already-advanced one -- can be invalidated under us.  The rotating
           start keeps low fds from spending the whole budget every time.  */
        static int inbound_cursor_fd = -1;
        std::vector<int> ready_fds;
        ready_fds.reserve(fd2cs.size());
        for (map<int, CompileServer *>::const_iterator sit = fd2cs.upper_bound(inbound_cursor_fd);
                sit != fd2cs.end(); ++sit) {
            ready_fds.push_back(sit->first);
        }
        for (map<int, CompileServer *>::const_iterator sit = fd2cs.begin();
                sit != fd2cs.end() && sit->first <= inbound_cursor_fd; ++sit) {
            ready_fds.push_back(sit->first);
        }
        /* active_fds alone is not a sufficient guard once ingress is
           budgeted: a budget-stopped drain leaves messages in USERSPACE,
           which poll() cannot report, so a zero-timeout poll returns 0 and
           those messages would never be serviced.  */
        for (size_t ri = 0; ri < ready_fds.size() && (active_fds > 0 || service_buffered); ++ri) {
            if (inbound_budget_remaining <= 0 && service_buffered) {
                /* Budget spent: stop walking.  Continuing would assign the
                   cursor for every remaining fd and land it back where it
                   started, so a persistently buffered early connection would
                   spend the whole budget again next iteration while later
                   ones never got a turn.  */
                has_buffered_inbound = true;
                break;
            }
            const int i = ready_fds[ri];
            map<int, CompileServer *>::const_iterator live = fd2cs.find(i);
            if (live == fd2cs.end()) {
                continue;    // erased since the snapshot
            }
            CompileServer *cs = live->second;
            const int budget_before_fd = inbound_budget_remaining;

            /* pollfd_is_set() also reports POLLERR/POLLHUP as "set" (its
               check_errors default) -- deliberate here: an errored channel
               takes the flush_pending() path below even if it never asked
               for POLLOUT, which converges to handle_end() without waiting
               for the read side to notice the EOF.  */
            const bool can_write = pollfd_is_set(pollfds, i, POLLOUT);
            const bool can_read = pollfd_is_set(pollfds, i, POLLIN) || cs->has_msg();

            if (can_write || can_read) {
                /* poll() counts a file descriptor once, however many of its
                   events fired.  */
                active_fds--;
            }

            if (can_write) {
                /* Flush dispatch replies that were queued while the daemon's
                   receive buffer was full.  If this fails the connection is
                   genuinely dead, so tear the daemon down like any other dead
                   channel.  */
                if (!cs->flush_pending()) {
                    handle_end(cs, nullptr);
                    continue;    // cs is deleted now
                }
            }

            const bool alive_after = !can_read
                                     || drain_connection(cs, &has_buffered_inbound);
            if (inbound_budget_remaining < budget_before_fd) {
                /* Only a connection that actually consumed work advances the
                   cursor, so the next iteration resumes after it rather than
                   restarting the same cycle.  */
                inbound_cursor_fd = i;
            }
            if (!alive_after) {
                continue;    // connection deleted by a handler
            }
        }

        /* Finish any request whose expansion the budget cut short, so the
           caller receives its full reply count.  */
        if (expand_pending_requests()) {
            has_buffered_inbound = true;   // keep the loop awake for the rest
        }

        for (list<CompileServer *>::const_iterator it = cs_in_tsts.begin();
                it != cs_in_tsts.end(); ++it) {
            if(find(css.begin(), css.end(), *it) == css.end()) {
                continue; // deleted meanwhile
            }
            if((*it)->getConnectionInProgress())
            {
                /* A probe transitions on exactly two of ITS OWN facts: its
                   fd became ready, or its own monotonic deadline expired.
                   Unrelated ready descriptors (active_fds) must not gate
                   it.  The SO_ERROR verdict is clear-on-read and is read
                   exactly once, only on readiness.  */
                if (pollfd_is_set(pollfds, (*it)->getInFd(), POLLIN | POLLOUT)) {
                    (*it)->updateInConnectivity((*it)->probeCompletionOk());
                } else if ((*it)->probeDeadlineExpired()) {
                    (*it)->updateInConnectivity(false);
                }
            }
        }
    }

    shutdown(broad_fd, SHUT_RDWR);
    while (!css.empty())
        handle_end(css.front(), nullptr);
    while (!monitors.empty())
        handle_end(monitors.front(), nullptr);
    if ((-1 == close(broad_fd)) && (errno != EBADF)){
        log_perror("close failed");
    }
    if (-1 == unlink(pidFilePath.c_str()) && errno != ENOENT){
        log_perror("unlink failed") << "\t" << pidFilePath << endl;
    }
    return 0;
}
