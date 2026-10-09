/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2026 the Icecream maintainers

    See p50_owner_watchdog.h for the contract. Implementation notes:

    - Everything that can run off the signal handler (threshold parsing,
      the stall/rate-limit state machine, reading /proc/self/task/<tid>/*,
      formatting the P50_OWNER_STALL_PROC and P50_OWNER_STALL_END lines) is
      ordinary C++: fprintf/std::string/exceptions are all fine there.
    - Only owner_stall_signal_handler() runs on a signal stack, on the
      owner thread, asynchronously with respect to whatever that thread
      was doing. It is restricted to: format_u64_decimal() (pure, no
      syscalls), write(2), clock_gettime(2) (vDSO, documented safe to call
      from a signal handler), backtrace() (safe here specifically because
      install_owner_sigprof_handler() pre-warms its lazy unwinder-init
      exactly so that first call does not happen inside a handler), and
      backtrace_symbols_fd() (glibc: unlike backtrace_symbols(), does not
      call malloc).
*/

#include "p50_owner_watchdog.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>

namespace icecc::p50::diag {

size_t format_u64_decimal(uint64_t value, char* buf, size_t buf_size) noexcept {
    char tmp[20]; // 2^64-1 has 20 decimal digits
    size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + static_cast<int>(value % 10));
        value /= 10;
    } while (value != 0 && n < sizeof(tmp));

    const size_t out = n < buf_size ? n : buf_size;
    for (size_t i = 0; i < out; ++i)
        buf[i] = tmp[n - 1 - i];
    return out;
}

OwnerWatchdogThresholdResult resolve_owner_watchdog_threshold(const char* env_value) noexcept {
    OwnerWatchdogThresholdResult result;

    if (env_value == nullptr || *env_value == '\0')
        return result;

    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(env_value, &end, 10);
    const bool full_match = (end != env_value) && (*end == '\0');
    const bool in_range = parsed >= 100 && parsed <= 10000;

    if (full_match && errno == 0 && in_range) {
        result.enabled = true;
        result.threshold_ms = static_cast<int>(parsed);
        return result;
    }

    result.rejected = true;
    result.rejection_reason =
        std::string("malformed/out-of-range ICECC_P50_OWNER_WATCHDOG_MS='") + env_value +
        "' (must be an integer in [100, 10000]); owner watchdog stays disabled";
    return result;
}

int64_t steady_now_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

OwnerStallDumpGate::OwnerStallDumpGate(int64_t threshold_ns, int64_t dump_interval_ns,
                                       int max_dumps_per_stall) noexcept
    : threshold_ns_(threshold_ns),
      dump_interval_ns_(dump_interval_ns),
      max_dumps_per_stall_(max_dumps_per_stall) {}

OwnerStallDumpGate::Action OwnerStallDumpGate::tick(int64_t now_ns, int64_t heartbeat_ns) noexcept {
    const int64_t lag_ns = now_ns - heartbeat_ns;

    if (lag_ns <= threshold_ns_) {
        if (stalled_) {
            stalled_ = false;
            last_stalled_ms_ = (heartbeat_ns - stall_started_ns_) / 1'000'000LL;
            return Action::StallEnded;
        }
        return Action::None;
    }

    if (!stalled_) {
        stalled_ = true;
        stall_started_ns_ = heartbeat_ns;
        dumps_this_stall_ = 0;
        last_dump_ns_ = INT64_MIN;
    }

    if (dumps_this_stall_ >= max_dumps_per_stall_)
        return Action::None;
    if (dumps_this_stall_ > 0 && (now_ns - last_dump_ns_) < dump_interval_ns_)
        return Action::None;

    last_dump_ns_ = now_ns;
    ++dumps_this_stall_;
    last_stalled_ms_ = (now_ns - stall_started_ns_) / 1'000'000LL;
    return Action::Dump;
}

namespace {

// Set by the watchdog thread immediately before tgkill(), read by
// owner_stall_signal_handler() on the owner thread right after it is
// signaled. The watchdog never fires two dumps closer together than
// dump_interval_ns_ (production: 1s), so there is no realistic risk of the
// handler reading a value meant for a different dump.
std::atomic<int64_t> g_pending_stalled_ms{0};
std::atomic<int> g_pending_dump_index{0};

void write_all(int fd, const char* data, size_t len) noexcept {
    while (len > 0) {
        const ssize_t n = ::write(fd, data, len);
        if (n <= 0)
            return; // best-effort; never retry-loop inside a signal handler
        data += n;
        len -= static_cast<size_t>(n);
    }
}

void write_literal(int fd, const char* s) noexcept {
    write_all(fd, s, std::strlen(s));
}

void write_u64(int fd, uint64_t value) noexcept {
    char buf[20];
    const size_t n = format_u64_decimal(value, buf, sizeof(buf));
    write_all(fd, buf, n);
}

// The actual SIGPROF handler. Runs on the owner thread, on its dedicated
// sigaltstack (see install_owner_sigaltstack()), asynchronously -- see the
// file header comment above for exactly which calls are safe here and why.
extern "C" void owner_stall_signal_handler(int /*sig*/) {
    static void* frames[64];

    const long tid = syscall(SYS_gettid);
    const int64_t stalled_ms = g_pending_stalled_ms.load(std::memory_order_relaxed);
    const int dump_index = g_pending_dump_index.load(std::memory_order_relaxed);
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    const int64_t wall_clock_ns =
        static_cast<int64_t>(rt.tv_sec) * 1'000'000'000LL + rt.tv_nsec;

    write_literal(2, "P50_OWNER_STALL tid=");
    write_u64(2, static_cast<uint64_t>(tid));
    write_literal(2, " stalled_ms=");
    write_u64(2, static_cast<uint64_t>(stalled_ms));
    write_literal(2, " dump=");
    write_u64(2, static_cast<uint64_t>(dump_index));
    write_literal(2, " wall_clock_ns=");
    write_u64(2, static_cast<uint64_t>(wall_clock_ns));
    write_literal(2, "\n");

    const int n = backtrace(frames, static_cast<int>(sizeof(frames) / sizeof(frames[0])));
    if (n > 0)
        backtrace_symbols_fd(frames, n, 2);
}

// Reads one single-line-ish /proc/self/task/<tid>/<leaf> pseudo-file into a
// bounded local buffer and prints it as
// "P50_OWNER_STALL_PROC tid=<tid> <leaf>=<contents>". Ordinary (non-signal)
// code: called from the watchdog thread, before signaling the owner (see
// the header comment for why "before" matters -- SIGPROF delivery wakes the
// thread and would make wchan/syscall stale if read afterward).
void print_proc_file(int tid, const char* leaf) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/%s", tid, leaf);

    char contents[256];
    size_t len = 0;
    const int fd = ::open(path, O_RDONLY);
    if (fd >= 0) {
        const ssize_t n = ::read(fd, contents, sizeof(contents) - 1);
        ::close(fd);
        if (n > 0)
            len = static_cast<size_t>(n);
    }
    contents[len] = '\0';
    // wchan has no trailing newline; syscall's does -- strip either way.
    while (len > 0 && (contents[len - 1] == '\n' || contents[len - 1] == '\0'))
        contents[--len] = '\0';
    if (len == 0) {
        std::snprintf(contents, sizeof(contents), "<unavailable>");
    }

    std::fprintf(stderr, "P50_OWNER_STALL_PROC tid=%d %s=%s\n", tid, leaf, contents);
}

} // namespace

bool install_owner_sigprof_handler() noexcept {
    struct sigaction old_action;
    std::memset(&old_action, 0, sizeof(old_action));
    if (sigaction(SIGPROF, nullptr, &old_action) != 0)
        return false;

    const bool someone_else_owns_it =
        (old_action.sa_flags & SA_SIGINFO) != 0 ||
        !(old_action.sa_handler == SIG_DFL || old_action.sa_handler == SIG_IGN);
    if (someone_else_owns_it)
        return false;

    // Pre-warm backtrace()'s lazy one-time unwinder init off any signal
    // handler (see file header comment).
    void* warmup[4];
    (void)backtrace(warmup, 4);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &owner_stall_signal_handler;
    sa.sa_flags = SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, nullptr) != 0)
        return false;

    return true;
}

void install_owner_sigaltstack() noexcept {
    static thread_local unsigned char stack[65536];
    stack_t ss;
    ss.ss_sp = stack;
    ss.ss_size = sizeof(stack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);
}

OwnerWatchdog::OwnerWatchdog(int threshold_ms, std::atomic<int64_t>& heartbeat_ns,
                             const std::atomic<int>& owner_tid) noexcept
    : heartbeat_ns_(heartbeat_ns),
      owner_tid_(owner_tid),
      gate_(static_cast<int64_t>(threshold_ms) * 1'000'000LL),
      handler_installed_by_us_(install_owner_sigprof_handler()) {
    if (!handler_installed_by_us_) {
        std::fprintf(stderr,
                     "P50_OWNER_WATCHDOG: SIGPROF already has a handler installed by another "
                     "component; running in heartbeat-only mode (no backtraces)\n");
    }
    thread_ = std::thread([this] { run(); });
}

OwnerWatchdog::~OwnerWatchdog() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable())
        thread_.join();
}

void OwnerWatchdog::run() {
    ::pthread_setname_np(::pthread_self(), "p50-ownerwd");

    for (;;) {
        for (int waited_ms = 0; waited_ms < 50 && !stop_.load(std::memory_order_acquire);
             waited_ms += 10) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Always tick once more here, even if stop_ is now set: this is the
        // only chance to observe a heartbeat that resumed (StallEnded) or a
        // stall that just started (Dump) during the final 50ms poll window
        // before the destructor tears this thread down. Skipping this tick
        // on the stopping path would silently lose that last transition --
        // e.g. a caller that constructs+immediately-destroys a watchdog
        // right as the owner recovers (exactly what the unit test does, and
        // plausible in production if SidecarRuntime is shutting down right
        // as the owner thread comes back).
        const bool stopping = stop_.load(std::memory_order_acquire);

        const int64_t now = steady_now_ns();
        const int64_t hb = heartbeat_ns_.load(std::memory_order_acquire);
        const int tid = owner_tid_.load(std::memory_order_acquire);
        if (tid > 0) {
            const OwnerStallDumpGate::Action action = gate_.tick(now, hb);
            if (action == OwnerStallDumpGate::Action::Dump) {
                // Kernel-side diagnostics first -- see header comment: SIGPROF
                // delivery wakes the thread and would make these stale.
                print_proc_file(tid, "wchan");
                print_proc_file(tid, "syscall");

                if (handler_installed_by_us_) {
                    g_pending_stalled_ms.store(gate_.stalled_ms(), std::memory_order_relaxed);
                    g_pending_dump_index.store(gate_.dump_count(), std::memory_order_relaxed);
                    syscall(SYS_tgkill, getpid(), tid, SIGPROF);
                }
            } else if (action == OwnerStallDumpGate::Action::StallEnded) {
                std::fprintf(stderr, "P50_OWNER_STALL_END tid=%d stalled_ms=%lld\n", tid,
                             static_cast<long long>(gate_.stalled_ms()));
            }
        }

        if (stopping)
            break;
    }
}

} // namespace icecc::p50::diag
