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
        return 0;
    }
    return clamp_u32(now - invocation_timing.submit_msec);
}

void invocation_timing_reset()
{
    invocation_timing.submit_ts = uint32_t(time(nullptr));
    invocation_timing.submit_msec = invocation_now_msec();
    invocation_timing.enqueue_msec = 0;
    invocation_timing.start_msec = 0;
    invocation_timing.finish_msec = 0;
    invocation_timing.waitforcs_msec = 0;
    invocation_timing.local_queue_msec = 0;
    invocation_timing.exec_msec = 0;
    invocation_timing.scheduler_job_id = 0;
    invocation_timing.compile_job_id = 0;
    invocation_timing.exitcode = 0;
    invocation_timing.mode.clear();
    invocation_timing.sent = false;
}

void invocation_timing_mark_enqueue(const std::string &mode)
{
    if (!invocation_timing.enqueue_msec) {
        invocation_timing.enqueue_msec = invocation_elapsed_msec();
    }
    if (!mode.empty()) {
        invocation_timing.mode = mode;
    }
}

void invocation_timing_mark_start(const std::string &mode)
{
    if (!invocation_timing.start_msec) {
        invocation_timing.start_msec = invocation_elapsed_msec();
    }
    if (!mode.empty()) {
        invocation_timing.mode = mode;
    }
    if (invocation_timing.start_msec >= invocation_timing.enqueue_msec) {
        invocation_timing.local_queue_msec = invocation_timing.start_msec - invocation_timing.enqueue_msec;
    }
}

void invocation_timing_mark_finish(int exitcode)
{
    invocation_timing.finish_msec = invocation_elapsed_msec();
    invocation_timing.exitcode = exitcode;
    if (invocation_timing.finish_msec >= invocation_timing.start_msec) {
        invocation_timing.exec_msec = invocation_timing.finish_msec - invocation_timing.start_msec;
    }
    if (!invocation_timing.waitforcs_msec && invocation_timing.start_msec >= invocation_timing.enqueue_msec
            && invocation_timing.mode.find("remote") != std::string::npos) {
        invocation_timing.waitforcs_msec = invocation_timing.start_msec - invocation_timing.enqueue_msec;
    }
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

bool invocation_timing_send(MsgChannel *local_daemon)
{
    if (!local_daemon || invocation_timing.sent || !invocation_timing.finish_msec
            || !IS_PROTOCOL_VERSION(PROTOCOL_VERSION_JOB_TIMING, local_daemon)) {
        return false;
    }

    const JobTimingMsg msg(invocation_timing.submit_ts,
                           invocation_timing.enqueue_msec,
                           invocation_timing.start_msec,
                           invocation_timing.finish_msec,
                           invocation_timing.waitforcs_msec,
                           invocation_timing.local_queue_msec,
                           invocation_timing.exec_msec,
                           invocation_timing.scheduler_job_id,
                           invocation_timing.compile_job_id,
                           invocation_timing.exitcode,
                           invocation_timing.mode.empty() ? string("unknown") : invocation_timing.mode);
    const bool ok = local_daemon->send_msg(msg);
    if (ok) {
        invocation_timing.sent = true;
    }
    return ok;
}

