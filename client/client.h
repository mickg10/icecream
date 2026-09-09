/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of icecc.

    Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
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

#ifndef _CLIENT_H_
#define _CLIENT_H_

#include <job.h>
#include <comm.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <stdint.h>

#include <stdexcept>

#include "exitcode.h"
#include "logging.h"
#include "util.h"

class MsgChannel;

extern std::string remote_daemon;
extern std::string invocation_cmdline;

/* Explicit phase records replace zero-sentinel timestamps: zero is both a
   valid sub-millisecond offset and "unset", which made a fast invocation
   indistinguishable from an unfinished one and let a remote-then-local
   fallback overwrite its own history.  Each phase carries a validity flag,
   and the fallback path records its own boundaries instead of mutating the
   remote ones.  */
struct InvocationPhase
{
    uint32_t msec = 0;
    bool valid = false;

    void mark(uint32_t at)
    {
        if (!valid) {
            msec = at;
            valid = true;
        }
    }
};

/* How the invocation actually ended, kept separate from timing so failure
   accounting cannot be inferred from durations.  */
enum InvocationOutcome
{
    InvocationCompilerCompleted = 0,   // a compiler ran and returned a status
    InvocationTransportLost = 1,       // remote attempt lost before a result
    InvocationCancelled = 2
};

struct InvocationTiming
{
    uint32_t submit_ts;
    uint64_t submit_msec;
    // phase boundaries, all relative to submit
    InvocationPhase remote_enqueue;      // GetCS sent to the local daemon
    InvocationPhase assignment;          // UseCS received
    InvocationPhase remote_attempt_end;  // remote path gave up (fallback only)
    InvocationPhase fallback_enqueue;    // local fallback queued
    InvocationPhase local_start;         // local compiler started
    InvocationPhase finish;              // invocation finished
    uint32_t scheduler_job_id;
    uint32_t compile_job_id;
    int exitcode;
    InvocationOutcome outcome;
    std::string mode;       // first mode entered, never overwritten
    bool fell_back;         // remote attempted, then local
    bool sent;
};

extern InvocationTiming invocation_timing;
extern uint64_t invocation_now_msec();
extern void invocation_timing_reset();
extern void invocation_timing_mark_enqueue(const std::string &mode = std::string());
extern void invocation_timing_mark_start(const std::string &mode = std::string());
extern void invocation_timing_mark_fallback();
extern void invocation_timing_mark_finish(int exitcode,
        InvocationOutcome outcome = InvocationCompilerCompleted);
extern void invocation_timing_set_scheduler_job_id(uint32_t job_id);
extern void invocation_timing_set_compile_job_id(uint32_t job_id);
extern void invocation_timing_replace_assignment_ids(
    uint32_t scheduler_job_id, uint32_t compile_job_id);
extern bool invocation_timing_send(MsgChannel *local_daemon);

/* in remote.cpp */
extern std::string get_absfilename(const std::string &_file);

enum RunFlags
{
    None           = 0,
    AlwaysLocal    = 1 << 0,  // The job should be built locally.
    FullJob        = 1 << 1,  // The job should reserve all slots (if AlwaysLocal).
    PreprocessOnly = 1 << 2   // Preprocessor-only invocation (-E / -M*).
};
/* In arg.cpp.  */
// Returns RunFlags or-ed.
extern int analyse_argv(const char * const *argv, CompileJob &job, bool icerun,
                        std::list<std::string> *extrafiles);

/* In cpp.cpp.  */
extern pid_t call_cpp(CompileJob &job, int fdwrite, int fdread = -1);

/* In local.cpp.  */
extern int build_local(CompileJob &job, MsgChannel *daemon, struct rusage *usage = 0);
extern std::string find_compiler(const CompileJob &job);
extern bool compiler_is_clang(const CompileJob &job);
extern bool compiler_only_rewrite_includes(const CompileJob &job);
extern std::string compiler_path_lookup(const std::string &compiler);
extern std::string clang_get_default_target(const CompileJob &job);
extern bool compiler_get_arch_flags(const CompileJob& job, bool march, bool mcpu, bool mtune,
    std::list<std::string>& args);

/* In remote.cpp - permill is the probability it will be compiled three times */
extern int build_remote(CompileJob &job, MsgChannel *scheduler,
                        const Environments &envs, int permill,
                        bool request_p50 = true,
                        const std::string &retry_avoid_host = std::string(),
                        uint32_t retry_avoid_port = 0);

/* safeguard.cpp */
// We allow several recursions if icerun is involved, just in case icerun is e.g. used to invoke a script
// that calls make that invokes compilations. In this case, it is allowed to have icerun->icecc->compiler.
// However, icecc->icecc recursion is a problem, so just one recursion exceeds the limit.
// Also note that if the total number of such recursive invocations exceedds the number of allowed local
// jobs, iceccd will not assign another local job and the whole build will get stuck.
static const int SafeguardMaxLevel = 2;
enum SafeguardStep
{
    SafeguardStepCompiler = SafeguardMaxLevel,
    SafeguardStepCustom = 1
};
extern void dcc_increment_safeguard(SafeguardStep step);
extern int dcc_recursion_safeguard(void);

extern Environments parse_icecc_version(const std::string &target, const std::string &prefix);

class client_error :  public std::runtime_error
{
    public:
    client_error(int code, const std::string& what)
    : std::runtime_error(what)
    , errorCode(code)
    {}

    const int errorCode;
};

class remote_error : public client_error
{
    public:
    remote_error(int code, const std::string& what)
    : client_error(code, what)
    , retryAvoidPort(0)
    {}

    remote_error(int code, const std::string& what,
                 std::string retry_avoid_host,
                 uint32_t retry_avoid_port)
    : client_error(code, what)
    , retryAvoidHost(std::move(retry_avoid_host))
    , retryAvoidPort(retry_avoid_port)
    {}

    [[nodiscard]] bool hasRetryAvoidEndpoint() const noexcept
    {
        return p50_cache_retry_avoid_is_present(
            retryAvoidPort, retryAvoidHost);
    }

    const std::string retryAvoidHost;
    const uint32_t retryAvoidPort;
};


#endif
