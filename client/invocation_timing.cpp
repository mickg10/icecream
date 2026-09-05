/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
 * icecc -- A simple distributed compiler system
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
 */

#include <chrono>

#include "client.h"

using namespace std;

std::string invocation_cmdline;
InvocationTiming invocation_timing;

uint64_t invocation_now_msec()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static uint32_t clamp_u32(uint64_t value)
{
    return value > 0xffffffffULL ? 0xffffffffU : uint32_t(value);
}

static uint32_t invocation_elapsed_msec()
{
    if (!invocation_timing.submit_ts) {
        return 0;
    }
    const uint64_t now = invocation_now_msec();
    if (now <= invocation_timing.submit_msec) {
        return 0;   // a genuine zero offset; phases carry their own validity
    }
    return clamp_u32(now - invocation_timing.submit_msec);
}

void invocation_timing_reset()
{
    invocation_timing.submit_ts = uint32_t(time(nullptr));
    invocation_timing.submit_msec = invocation_now_msec();
    invocation_timing.remote_enqueue = InvocationPhase();
    invocation_timing.assignment = InvocationPhase();
    invocation_timing.remote_attempt_end = InvocationPhase();
    invocation_timing.fallback_enqueue = InvocationPhase();
    invocation_timing.local_start = InvocationPhase();
    invocation_timing.finish = InvocationPhase();
    invocation_timing.scheduler_job_id = 0;
    invocation_timing.compile_job_id = 0;
    invocation_timing.exitcode = 0;
    invocation_timing.outcome = InvocationCompilerCompleted;
    invocation_timing.mode.clear();
    invocation_timing.fell_back = false;
    invocation_timing.sent = false;
}

void invocation_timing_mark_enqueue(const std::string &mode)
{
    const uint32_t at = invocation_elapsed_msec();
    if (invocation_timing.fell_back) {
        invocation_timing.fallback_enqueue.mark(at);
    } else {
        invocation_timing.remote_enqueue.mark(at);
    }
    /* The mode records how the invocation STARTED and is never overwritten;
       a later fallback is reported by fell_back, so a remote attempt that
       ended locally is no longer indistinguishable from a plain local
       build.  */
    if (!mode.empty() && invocation_timing.mode.empty()) {
        invocation_timing.mode = mode;
    }
}

void invocation_timing_mark_fallback()
{
    /* The remote attempt has failed; close it out and start the local
       phases rather than mutating the remote timestamps.  */
    invocation_timing.remote_attempt_end.mark(invocation_elapsed_msec());
    invocation_timing.fell_back = true;
}

void invocation_timing_mark_start(const std::string &mode)
{
    const uint32_t at = invocation_elapsed_msec();
    if (invocation_timing.fell_back) {
        invocation_timing.local_start.mark(at);
    } else {
        /* For remote work this is the moment the assignment (UseCS) arrived,
           not the moment a compiler began -- named accordingly.  */
        invocation_timing.assignment.mark(at);
    }
    if (!mode.empty() && invocation_timing.mode.empty()) {
        invocation_timing.mode = mode;
    }
}

void invocation_timing_mark_finish(int exitcode, InvocationOutcome outcome)
{
    invocation_timing.finish.mark(invocation_elapsed_msec());
    invocation_timing.exitcode = exitcode;
    invocation_timing.outcome = outcome;
}

void invocation_timing_set_scheduler_job_id(uint32_t job_id)
{
    if (job_id && !invocation_timing.scheduler_job_id) {
        invocation_timing.scheduler_job_id = job_id;
    }
}

void invocation_timing_set_compile_job_id(uint32_t job_id)
{
    if (job_id && !invocation_timing.compile_job_id) {
        invocation_timing.compile_job_id = job_id;
    }
}

void invocation_timing_replace_assignment_ids(
    uint32_t scheduler_job_id, uint32_t compile_job_id)
{
    /* A bounded P50 failure obtains a genuinely fresh legacy assignment.
       Replace only the assignment identities so the one invocation's phase
       timestamps and total wall interval remain continuous across attempts. */
    if (scheduler_job_id != 0)
        invocation_timing.scheduler_job_id = scheduler_job_id;
    if (compile_job_id != 0)
        invocation_timing.compile_job_id = compile_job_id;
}

bool invocation_timing_send(MsgChannel *local_daemon)
{
    /* Validity comes from the phase flag, not from a nonzero value: a
       sub-millisecond invocation finishes at offset 0 and must still be
       reported.  */
    if (!local_daemon || invocation_timing.sent || !invocation_timing.finish.valid
            || !IS_PROTOCOL_VERSION(PROTOCOL_VERSION_JOB_TIMING, local_daemon)) {
        return false;
    }

    /* Derive the wire fields from the phase records (protocol layout
       unchanged).  Each duration now spans exactly the phases it names:
        - waitforcs: request enqueued -> assignment received
        - local_queue: fallback queued -> local compiler started
        - post_dispatch (wire field 'exec'): assignment/local start -> finish;
          for remote work this covers connect, env transfer, preprocess,
          upload, compile and download, which is why it is not reported as
          compiler execution time.  */
    const InvocationTiming &t = invocation_timing;
    const uint32_t enqueue_msec = t.remote_enqueue.valid ? t.remote_enqueue.msec
                                : (t.fallback_enqueue.valid ? t.fallback_enqueue.msec : 0);
    const uint32_t start_msec = t.fell_back
        ? (t.local_start.valid ? t.local_start.msec : enqueue_msec)
        : (t.assignment.valid ? t.assignment.msec : enqueue_msec);
    const uint32_t finish_msec = t.finish.msec;
    const uint32_t waitforcs_msec =
        (t.remote_enqueue.valid && t.assignment.valid && t.assignment.msec >= t.remote_enqueue.msec)
        ? t.assignment.msec - t.remote_enqueue.msec : 0;
    const uint32_t local_queue_msec =
        (t.fallback_enqueue.valid && t.local_start.valid && t.local_start.msec >= t.fallback_enqueue.msec)
        ? t.local_start.msec - t.fallback_enqueue.msec : 0;
    const uint32_t post_dispatch_msec =
        finish_msec >= start_msec ? finish_msec - start_msec : 0;
    string mode = t.mode.empty() ? string("unknown") : t.mode;
    if (t.fell_back) {
        mode += "+fallback_local";
    }

    const JobTimingMsg msg(t.submit_ts,
                           enqueue_msec,
                           start_msec,
                           finish_msec,
                           waitforcs_msec,
                           local_queue_msec,
                           post_dispatch_msec,
                           t.scheduler_job_id,
                           t.compile_job_id,
                           t.exitcode,
                           mode);
    const bool ok = local_daemon->send_msg(msg);
    if (ok) {
        invocation_timing.sent = true;
    }
    return ok;
}
