#pragma once

// Diagnostic-only owner-stall watchdog (ICECC_P50_OWNER_WATCHDOG_MS).
//
// Context: F-side sidecars occasionally show a 3-4s window where the
// endpoint owner thread stops making progress (no owner-lag trace lines,
// and a daemon lifecycle RPC to the sidecar gets no reply and is treated as
// Disconnected, causing the daemon to retire the sidecar) on otherwise idle
// routes with no kernel compaction/GC activity. An external sampler that
// picked the process's "highest-utime thread" during one such freeze saw it
// idle in poll(2) -- almost certainly the wrong thread, since no thread in
// this process is named today (ps/top cannot distinguish the owner from a
// control worker or a codec-pool thread). This module exists to capture the
// *actual* owner thread's user-space stack while it is stuck, with zero
// behavior or wire/protocol change on the production path.
//
// Disabled unless ICECC_P50_OWNER_WATCHDOG_MS names an integer in
// [100, 10000]; any other set-but-invalid value is rejected (default kept
// off) with one stderr line. Unset/empty is silent, exactly like the
// pre-existing ICECC_P50_OWNER_LAG_TRACE and
// ICECC_P50_INPUT_LIFECYCLE_TIMEOUT_MS probes (see owner_lag_trace_enabled()
// in p50_cache_service.cpp and daemon/p50_lifecycle_timeout_override.h).
//
// Pieces, in order of how directly they are unit-testable:
//
//   format_u64_decimal()            -- pure, no I/O. Manual decimal integer
//                                       formatter for the async-signal-safe
//                                       handler below (no snprintf/malloc
//                                       allowed inside a signal handler).
//   resolve_owner_watchdog_threshold() -- pure env-var parse/clamp, mirrors
//                                       daemon/p50_lifecycle_timeout_override.h's
//                                       resolve_input_lifecycle_timeout_override()
//                                       exactly, for the same reason: directly
//                                       testable without constructing a
//                                       SidecarRuntime.
//   OwnerStallDumpGate               -- pure state machine (no syscalls, no
//                                       real clock): decides, from a stream
//                                       of (now_ns, heartbeat_ns) samples,
//                                       when to fire a dump (rate-limited to
//                                       at most once per dump_interval_ns_
//                                       while a stall persists, capped at
//                                       max_dumps_per_stall_) and when to
//                                       report the stall as over. Directly
//                                       unit-testable with synthetic
//                                       timestamps.
//   install_owner_sigprof_handler()  -- installs the process-wide SIGPROF
//                                       disposition exactly once. Peeks the
//                                       existing disposition first and
//                                       refuses to clobber a handler some
//                                       other library already installed
//                                       (returns false; the caller logs
//                                       this and the watchdog falls back to
//                                       heartbeat-only detection with no
//                                       backtrace capture).
//   install_owner_sigaltstack()      -- must be called ON the owner thread
//                                       itself (sigaltstack is per-thread);
//                                       see SidecarRuntime::endpoint_owner_loop().
//   OwnerWatchdog                    -- the background "p50-ownerwd" thread.
//                                       Parameterized over references to the
//                                       owner's heartbeat/tid atomics (not a
//                                       SidecarRuntime pointer), so a test
//                                       can drive it against a fake owner
//                                       context without constructing the real
//                                       sidecar runtime.
//
// What actually fires on a detected stall:
//   1. (watchdog thread, ordinary code) read and print
//      /proc/self/task/<owner_tid>/{wchan,syscall} as
//      "P50_OWNER_STALL_PROC ..." lines -- *before* signaling the owner, since
//      SIGPROF delivery wakes the thread out of whatever syscall it was
//      blocked in and would otherwise make wchan/syscall stale by the time
//      they are read.
//   2. (watchdog thread) tgkill(SIGPROF) to the owner tid, iff our handler is
//      the one installed (see install_owner_sigprof_handler()).
//   3. (owner thread, inside the SIGPROF handler, async-signal-safe only)
//      "P50_OWNER_STALL tid=<n> stalled_ms=<n> dump=<k> wall_clock_ns=<n>",
//      then backtrace_symbols_fd() of the owner's current user-space stack.
//   4. (watchdog thread, once the heartbeat resumes) "P50_OWNER_STALL_END
//      tid=<n> stalled_ms=<total>".

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

namespace icecc::p50::diag {

// Writes the base-10 decimal representation of `value` into buf[0..); does
// NOT null-terminate (callers track length explicitly and use write(2), not
// a C-string API). Returns the number of bytes written, which is <= the
// number of decimal digits in `value` if buf_size is too small (output is
// simply truncated from the left -- callers size buf generously (>=20
// bytes) so this truncation path is never actually exercised in practice).
// Async-signal-safe: no syscalls, no library calls other than this function
// itself, no allocation.
[[nodiscard]] size_t format_u64_decimal(uint64_t value, char* buf, size_t buf_size) noexcept;

struct OwnerWatchdogThresholdResult {
    // True iff env_value named a valid threshold and the watchdog should run.
    bool enabled = false;
    // Valid iff enabled. Milliseconds of heartbeat silence that counts as a
    // stall.
    int threshold_ms = 0;
    // True iff env_value was set but did not parse as an integer in
    // [100, 10000]. `enabled` is false in this case too (fail off, never
    // fail closed on a malformed override) -- the caller must log
    // rejection_reason once and otherwise behave exactly as if the env var
    // had been unset.
    bool rejected = false;
    std::string rejection_reason;
};

// env_value is the raw ICECC_P50_OWNER_WATCHDOG_MS value (nullptr or ""
// both mean "unset": enabled=false, rejected=false, silent). A value that
// parses as a plain base-10 integer in [100, 10000] enables the watchdog at
// that threshold. Anything else (non-numeric, trailing garbage, out of
// range) is rejected: enabled stays false, rejected becomes true, and
// rejection_reason explains why -- the caller logs this exactly once and
// otherwise proceeds as if unset.
[[nodiscard]] OwnerWatchdogThresholdResult resolve_owner_watchdog_threshold(
    const char* env_value) noexcept;

// Pure stall-detection/rate-limiting state machine. One instance per
// watchdog; driven by repeated tick() calls (production: every ~50ms, from
// the watchdog thread; tests: synthetic timestamps, no sleeping required).
class OwnerStallDumpGate {
public:
    // threshold_ns: heartbeat silence beyond this counts as a stall.
    // dump_interval_ns: minimum gap between dumps while a single stall
    //   persists (production: 1s).
    // max_dumps_per_stall: hard cap on dumps for one contiguous stall
    //   (production: 10), so a watchdog cannot itself become a
    //   signal-storm if the owner is stuck for a very long time.
    explicit OwnerStallDumpGate(int64_t threshold_ns,
                                 int64_t dump_interval_ns = 1'000'000'000LL,
                                 int max_dumps_per_stall = 10) noexcept;

    enum class Action {
        None,       // no new stall, or stall ongoing but rate-limited/capped
        Dump,       // fire a dump now; see stalled_ms()/dump_count()
        StallEnded, // heartbeat resumed; see stalled_ms() for total duration
    };

    // now_ns and heartbeat_ns must be the same monotonic clock (production:
    // CLOCK_MONOTONIC via steady_now_ns(), matching std::chrono::steady_clock
    // on Linux). Pure: no I/O, no global state.
    [[nodiscard]] Action tick(int64_t now_ns, int64_t heartbeat_ns) noexcept;

    // Valid immediately after tick() returns Dump (current lag in this
    // ongoing stall) or StallEnded (total duration of the stall that just
    // ended). Meaningless after None.
    [[nodiscard]] int64_t stalled_ms() const noexcept { return last_stalled_ms_; }
    // Valid immediately after tick() returns Dump: 1-based count of dumps
    // fired so far for the current stall (resets to 0 at the start of each
    // new stall).
    [[nodiscard]] int dump_count() const noexcept { return dumps_this_stall_; }

private:
    int64_t threshold_ns_;
    int64_t dump_interval_ns_;
    int max_dumps_per_stall_;

    bool stalled_ = false;
    int64_t stall_started_ns_ = 0;
    int64_t last_dump_ns_ = 0;
    int dumps_this_stall_ = 0;
    int64_t last_stalled_ms_ = 0;
};

// Returns the current CLOCK_MONOTONIC time in nanoseconds. Matches
// std::chrono::steady_clock on Linux; used instead of std::chrono so the
// SIGPROF-handler-adjacent code in the .cpp has one, obviously
// signal-safe-friendly time source instead of two.
[[nodiscard]] int64_t steady_now_ns() noexcept;

// Installs the process-wide SIGPROF handler used to capture the owner's
// backtrace, exactly once. First peeks the existing disposition
// (sigaction(SIGPROF, nullptr, &old)); if something other than the default
// (SIG_DFL) or ignore (SIG_IGN) is already installed -- i.e. some other
// library in this process (profiler, sanitizer, ...) owns SIGPROF -- this
// does NOT install over it and returns false. The caller (OwnerWatchdog)
// logs that once and runs in heartbeat-only mode (still detects/reports
// stalls and reads /proc wchan+syscall, just never sends SIGPROF or expects
// a backtrace). Returns true iff this call installed our handler.
//
// Also pre-warms backtrace()'s lazy one-time unwinder init (which can touch
// malloc internally in glibc) by calling it once right here, off any signal
// handler, so the first *real* invocation inside the signal handler never
// does that lazy init itself.
[[nodiscard]] bool install_owner_sigprof_handler() noexcept;

// Installs a dedicated signal stack (SA_ONSTACK target) for the calling
// thread. Must be called ON the owner thread itself (sigaltstack(2) is a
// per-thread property) -- see SidecarRuntime::endpoint_owner_loop(). No-op
// (but still safe to call) if the watchdog ends up disabled; callers only
// call this when it is enabled, but the function itself does not assume
// that so tests can call it directly.
void install_owner_sigaltstack() noexcept;

// The "p50-ownerwd" background thread. Reads heartbeat_ns/owner_tid (owned
// by the caller -- production: SidecarRuntime; tests: a fake owner context)
// and, on a detected stall, drives the proc-diagnostics + SIGPROF dump
// sequence described above. Construction installs the SIGPROF handler (see
// install_owner_sigprof_handler()); destruction stops and joins the
// background thread. Owns no io_context, no socket, no sidecar state.
class OwnerWatchdog {
public:
    // threshold_ms must be in [100, 10000] (validate via
    // resolve_owner_watchdog_threshold() before constructing). heartbeat_ns
    // and owner_tid are read-only from this class's point of view and must
    // outlive it.
    OwnerWatchdog(int threshold_ms, std::atomic<int64_t>& heartbeat_ns,
                  const std::atomic<int>& owner_tid) noexcept;
    ~OwnerWatchdog();

    OwnerWatchdog(const OwnerWatchdog&) = delete;
    OwnerWatchdog& operator=(const OwnerWatchdog&) = delete;

    // True iff this instance's install_owner_sigprof_handler() call
    // actually installed our handler (false => heartbeat-only mode, see
    // above). Diagnostic/test use only.
    [[nodiscard]] bool handler_installed_by_us() const noexcept {
        return handler_installed_by_us_;
    }

private:
    void run();

    std::atomic<int64_t>& heartbeat_ns_;
    const std::atomic<int>& owner_tid_;
    OwnerStallDumpGate gate_;
    const bool handler_installed_by_us_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace icecc::p50::diag
