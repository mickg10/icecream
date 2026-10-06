/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2026 the Icecream maintainers

    Opt-in, zero-cost-when-off per-job stage trace.

    Every component on the P50 remote-compile path (scheduler, client
    daemon, icecc client process, C/F cache sidecar, F daemon) can call
    stage_trace() at a hand-off point to record a single timestamped
    event.  When ICECC_STAGE_TRACE_DIR is not set in the environment the
    call costs one cached branch and nothing else: no syscalls, no
    allocation, no formatting.

    When the directory *is* set, each process appends newline-delimited
    JSON records to "<dir>/<component>-<host>-<pid>.jsonl". Writes are
    buffered in-process and flushed at a size/line threshold and at exit;
    fsync is never called. The writer is thread-safe (one mutex guards the
    shared buffer); it is NOT async-signal-safe, and must never be called
    from a fork()ed child before it execs (the child should either avoid
    tracing entirely or call stage_trace::reset_after_fork() first, which
    just forgets the parent's buffered-but-unflushed bytes rather than
    writing them twice).

    This header intentionally only accepts integer kv extras (bytes, fd
    counts, queue depths, slot counts, ...): everything on the documented
    stage list is a count, a size, or an id, and sticking to one value
    type keeps every call site uniform.
*/

#ifndef ICECREAM_STAGE_TRACE_H
#define ICECREAM_STAGE_TRACE_H

#include <cstdint>
#include <initializer_list>

namespace icecc::stagetrace {

// One small integer extra, e.g. {"bytes", 4096}.
struct KV {
    const char *key;
    int64_t value;
};

// Sentinel for "id not known/applicable at this stamp".
constexpr int64_t kUnknown = -1;

// True once ICECC_STAGE_TRACE_DIR has been resolved to a non-empty, usable
// directory. Resolution happens at most once per process, on the first
// call to this function (lazily, so processes that never trace never pay
// even the getenv()). Safe to call from any thread.
bool enabled();

// Appends one record. Only called when enabled() is true; use the
// stage_trace() wrapper below from call sites instead of calling this
// directly, so the common (disabled) path never reaches here.
void record(const char *component, const char *stage,
            int64_t epoch, int64_t wire_id,
            int64_t logical_job, int64_t attempt,
            std::initializer_list<KV> extras);

// Drops any buffered-but-unflushed bytes without writing them. Call this
// in a fork() child, before doing anything else that might trace, if the
// child intends to trace independently (it will reopen its own
// "<component>-<host>-<childpid>.jsonl" on its first record()). Never
// call stage_trace() between fork() and this reset, or between fork() and
// exec() at all if the child isn't resetting.
void reset_after_fork();

// Flushes buffered records to disk now. Normally unnecessary (an atexit
// handler flushes), but useful right before a process replaces itself
// with exec() without going through atexit, or in tests.
void flush();

// The single call site API. Component names are short lowercase tokens
// such as "scheduler", "cdaemon", "iceccclient", "csidecar", "fdaemon",
// "fsidecar". Stage names are short imperative/past-participle tokens
// such as "getcs_recv", "assign_prepare_sent", "transfer_lock_acquired".
inline void stage_trace(const char *component, const char *stage,
                         int64_t epoch = kUnknown, int64_t wire_id = kUnknown,
                         int64_t logical_job = kUnknown, int64_t attempt = kUnknown,
                         std::initializer_list<KV> extras = {})
{
    if (!enabled()) {
        return;
    }
    record(component, stage, epoch, wire_id, logical_job, attempt, extras);
}

} // namespace icecc::stagetrace

#endif
