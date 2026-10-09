/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
    This file is part of Icecream.

    Copyright (c) 2026 the Icecream maintainers

    Unit tests for cache/p50_owner_watchdog.{h,cpp}: the diagnostic-only
    owner-stall watchdog gated by ICECC_P50_OWNER_WATCHDOG_MS.

    Three groups, cheapest/most-isolated first:
      1. format_u64_decimal() -- pure integer formatter.
      2. resolve_owner_watchdog_threshold() -- pure env-var parse/clamp.
      3. OwnerStallDumpGate -- pure stall-detection/rate-limit state machine,
         driven with synthetic timestamps (no real sleeping).
      4. An end-to-end test that runs a real OwnerWatchdog against a fake
         owner thread (not a SidecarRuntime), blocks that thread's
         "handler" for 400ms with a 150ms threshold, and asserts that a
         P50_OWNER_STALL line with at least one backtrace frame line
         appears on stderr (redirected to a pipe for the duration of the
         test).
*/

#include "../cache/p50_owner_watchdog.h"

#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace diag = icecc::p50::diag;

namespace {

int failures = 0;

#define CHECK(expr, text)                                                                        \
    do {                                                                                          \
        if (!(expr)) {                                                                            \
            std::fprintf(stderr, "FAILED - %s\n", text);                                          \
            ++failures;                                                                           \
        } else {                                                                                  \
            std::fprintf(stderr, "ok - %s\n", text);                                              \
        }                                                                                          \
    } while (0)

std::string format_to_string(uint64_t value, size_t buf_size = 32) {
    std::string buf(buf_size, '\0');
    const size_t n = diag::format_u64_decimal(value, buf.data(), buf.size());
    return buf.substr(0, n);
}

void test_format_u64_decimal() {
    CHECK(format_to_string(0) == "0", "format_u64_decimal(0) == \"0\"");
    CHECK(format_to_string(7) == "7", "format_u64_decimal(7) == \"7\"");
    CHECK(format_to_string(12345) == "12345", "format_u64_decimal(12345) == \"12345\"");
    CHECK(format_to_string(UINT64_MAX) == "18446744073709551615",
          "format_u64_decimal(UINT64_MAX) matches the known decimal expansion");
    // Truncation path: a buffer smaller than the full representation keeps
    // the most-significant digits and reports exactly how many bytes it
    // wrote (see header comment -- this path is never exercised by any
    // caller sizing a real buffer, but must still behave predictably).
    CHECK(format_to_string(12345, 3) == "123",
          "format_u64_decimal truncates to the leftmost digits when buf_size is too small");
}

void test_resolve_owner_watchdog_threshold() {
    {
        const auto r = diag::resolve_owner_watchdog_threshold(nullptr);
        CHECK(!r.enabled && !r.rejected, "unset (nullptr) ICECC_P50_OWNER_WATCHDOG_MS is silent");
    }
    {
        const auto r = diag::resolve_owner_watchdog_threshold("");
        CHECK(!r.enabled && !r.rejected, "empty ICECC_P50_OWNER_WATCHDOG_MS is silent");
    }
    {
        const auto r = diag::resolve_owner_watchdog_threshold("150");
        CHECK(r.enabled && !r.rejected && r.threshold_ms == 150,
              "ICECC_P50_OWNER_WATCHDOG_MS=150 enables the watchdog at 150ms");
    }
    {
        const auto r = diag::resolve_owner_watchdog_threshold("100");
        CHECK(r.enabled && r.threshold_ms == 100, "the lower boundary 100 is inclusive");
    }
    {
        const auto r = diag::resolve_owner_watchdog_threshold("10000");
        CHECK(r.enabled && r.threshold_ms == 10000, "the upper boundary 10000 is inclusive");
    }
    {
        const auto r = diag::resolve_owner_watchdog_threshold("99");
        CHECK(!r.enabled && r.rejected, "99 (below the lower boundary) is rejected");
    }
    {
        const auto r = diag::resolve_owner_watchdog_threshold("10001");
        CHECK(!r.enabled && r.rejected, "10001 (above the upper boundary) is rejected");
    }
    {
        const auto r = diag::resolve_owner_watchdog_threshold("not-a-number");
        CHECK(!r.enabled && r.rejected, "non-numeric values are rejected");
    }
    {
        const auto r = diag::resolve_owner_watchdog_threshold("150abc");
        CHECK(!r.enabled && r.rejected, "trailing garbage after a valid number is rejected");
    }
    {
        const auto r = diag::resolve_owner_watchdog_threshold("  150");
        CHECK(r.enabled && r.threshold_ms == 150, "leading whitespace is tolerated (strtoll)");
    }
    {
        const auto r = diag::resolve_owner_watchdog_threshold("abc");
        CHECK(r.rejection_reason.find("ICECC_P50_OWNER_WATCHDOG_MS") != std::string::npos &&
                  r.rejection_reason.find("abc") != std::string::npos,
              "the rejection reason names both the env var and the offending value");
    }
}

void test_owner_stall_dump_gate() {
    constexpr int64_t kThresholdNs = 150'000'000LL;
    constexpr int64_t kDumpIntervalNs = 100'000'000LL;
    constexpr int kMaxDumps = 3;
    diag::OwnerStallDumpGate gate(kThresholdNs, kDumpIntervalNs, kMaxDumps);

    constexpr int64_t kBase = 1000;

    CHECK(gate.tick(kBase, kBase) == diag::OwnerStallDumpGate::Action::None,
          "a fresh heartbeat produces no action");
    CHECK(gate.tick(kBase + 140'000'000LL, kBase) == diag::OwnerStallDumpGate::Action::None,
          "lag just under the threshold produces no action");

    const auto a1 = gate.tick(kBase + 151'000'000LL, kBase);
    CHECK(a1 == diag::OwnerStallDumpGate::Action::Dump, "crossing the threshold fires a dump");
    CHECK(gate.dump_count() == 1, "the first dump of a stall is dump #1");
    CHECK(gate.stalled_ms() == 151, "stalled_ms reflects the lag at dump time");

    const auto a2 = gate.tick(kBase + 201'000'000LL, kBase);
    CHECK(a2 == diag::OwnerStallDumpGate::Action::None,
          "a second check inside the dump interval is rate-limited");

    const auto a3 = gate.tick(kBase + 251'000'000LL, kBase);
    CHECK(a3 == diag::OwnerStallDumpGate::Action::Dump,
          "a check at the next dump interval boundary fires dump #2");
    CHECK(gate.dump_count() == 2, "dump count increments across dumps in the same stall");

    const auto a4 = gate.tick(kBase + 351'000'000LL, kBase);
    CHECK(a4 == diag::OwnerStallDumpGate::Action::Dump, "dump #3 fires at the next interval");
    CHECK(gate.dump_count() == 3, "dump count reaches the configured max");

    const auto a5 = gate.tick(kBase + 451'000'000LL, kBase);
    CHECK(a5 == diag::OwnerStallDumpGate::Action::None,
          "a 4th would-be dump is suppressed once max_dumps_per_stall is reached");

    const int64_t resume_ns = kBase + 500'000'000LL;
    const auto a6 = gate.tick(resume_ns, resume_ns);
    CHECK(a6 == diag::OwnerStallDumpGate::Action::StallEnded,
          "the heartbeat catching back up to now ends the stall");
    CHECK(gate.stalled_ms() == 500, "StallEnded reports the total duration of the stall");

    CHECK(gate.tick(resume_ns + 10'000'000LL, resume_ns + 10'000'000LL) ==
              diag::OwnerStallDumpGate::Action::None,
          "a healthy heartbeat after StallEnded produces no further action");
}

std::string drain_captured_stderr(int saved_stderr_fd, int pipe_read_fd) {
    // Restoring real stderr onto fd 2 drops the pipe-write-end reference
    // that fd 2 held; combined with the explicit close(pipe_write) the
    // caller already did, this is the last reference, so the read loop
    // below reliably reaches EOF instead of blocking forever.
    dup2(saved_stderr_fd, 2);
    close(saved_stderr_fd);

    std::string captured;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(pipe_read_fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        captured.append(buf, static_cast<size_t>(n));
    }
    close(pipe_read_fd);
    return captured;
}

void test_owner_watchdog_end_to_end_stall_capture() {
    int pipefd[2];
    CHECK(pipe(pipefd) == 0, "pipe() for capturing stderr succeeds");
    const int saved_stderr = dup(2);
    CHECK(saved_stderr >= 0, "dup() of the real stderr fd succeeds");
    dup2(pipefd[1], 2);
    close(pipefd[1]);

    std::atomic<int64_t> heartbeat_ns{0};
    std::atomic<int> owner_tid{0};
    std::atomic<bool> owner_ready{false};

    std::thread fake_owner([&] {
        // Mirrors exactly what SidecarRuntime::endpoint_owner_loop() does
        // at the top of the real owner thread: install a dedicated signal
        // stack on this thread, then publish this thread's tid.
        diag::install_owner_sigaltstack();
        owner_tid.store(static_cast<int>(syscall(SYS_gettid)), std::memory_order_release);
        heartbeat_ns.store(diag::steady_now_ns(), std::memory_order_release);
        owner_ready.store(true, std::memory_order_release);

        // Simulate the owner stuck inside one handler for 400ms: no
        // heartbeat update during this window, exactly like the real
        // owner blocked in a syscall during a freeze.
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        heartbeat_ns.store(diag::steady_now_ns(), std::memory_order_release);
    });

    while (!owner_ready.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    {
        // threshold_ms=150 < the 400ms stall above, so this must fire at
        // least one dump before the fake owner thread resumes.
        diag::OwnerWatchdog watchdog(150, heartbeat_ns, owner_tid);
        fake_owner.join();
        // watchdog destructor (end of scope) stops+joins the "p50-ownerwd"
        // thread before we restore/drain stderr below.
    }

    const std::string captured = drain_captured_stderr(saved_stderr, pipefd[0]);

    const size_t stall_pos = captured.find("P50_OWNER_STALL tid=");
    CHECK(stall_pos != std::string::npos,
          "a P50_OWNER_STALL line appears in stderr during the simulated stall");

    const size_t header_end = (stall_pos == std::string::npos)
                                   ? std::string::npos
                                   : captured.find('\n', stall_pos);
    CHECK(header_end != std::string::npos && header_end + 1 < captured.size(),
          "at least one line follows the P50_OWNER_STALL header (the backtrace)");

    CHECK(captured.find("P50_OWNER_STALL_PROC tid=") != std::string::npos,
          "the kernel-side wchan/syscall diagnostics are printed");
    CHECK(captured.find("P50_OWNER_STALL_END tid=") != std::string::npos,
          "a P50_OWNER_STALL_END line appears once the fake owner's heartbeat resumes");
}

} // namespace

int main() {
    test_format_u64_decimal();
    test_resolve_owner_watchdog_threshold();
    test_owner_stall_dump_gate();
    test_owner_watchdog_end_to_end_stall_capture();
    return failures == 0 ? 0 : 1;
}
