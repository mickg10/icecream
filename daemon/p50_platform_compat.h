#pragma once

// Startup-time platform capability decision for the three kernel-3.10-class
// compatibility mitigations described in doc/p50-platform-compat.md:
//
//   M1 child handle   -- pidfd_open()/waitid(P_PIDFD)      vs (pid, starttime)
//   M2 input artifact -- sealed memfd                       vs daemon-private
//                                                               unlinked file
//   M3 random         -- getrandom()                        vs /dev/urandom
//
// A fourth mitigation (kcmp-based fd proof, "M4") was dropped from scope
// entirely: the production entry point for P50 compiler-fork input never
// calls kcmp, so there is nothing on the real path for a 3.10 fallback to
// replace. See doc/p50-platform-compat.md, "Why kcmp (M4) was dropped, not
// fallback-ed", for the finding. The seccomp 3.10-surface emulation profile
// must still deny kcmp (EPERM) as proof of that independence; this header
// has no kcmp content at all.
//
// This module decides once, at daemon startup, before any sidecar is
// launched. Every probe here is failure-based: it exercises the exact
// syscall shape the production code uses and classifies the result from the
// syscall's own return value and errno, never from uname()/kernel-version
// parsing. Decisions are not re-probed later and are not re-read from the
// environment anywhere except here.

#include "p50_compat_child_handle.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace icecc::p50::platform {

// Per-mitigation outcome. `FailClosed` means: the probe failed in a way that
// is not the clean "primitive unsupported" signal (ENOSYS for M1/M3,
// memfd_create ENOSYS or a seal that does not round-trip for M2), or the
// mode was `native` and the native probe failed, or the mode string was
// invalid. In every FailClosed case the caller must behave exactly like
// today's FailedClosed/cache=off path for that mitigation; this module does
// not silently substitute compat for an unrelated error.
enum class Backend : uint8_t {
    Native,
    Compat,
    FailClosed,
};

// Parsed form of an `ICECC_P50_PLATFORM*` value.
enum class Mode : uint8_t {
    Auto,
    Native,
    Compat,
    Invalid,
};

[[nodiscard]] Mode parse_mode(const char* value) noexcept;

struct MitigationDecision {
    Backend backend = Backend::FailClosed;
    // Short, human-readable reason, e.g. "pidfd ok", "ENOSYS", "invalid
    // value 'frobnicate'". Built once at startup decision time; never on a
    // signal-handling or other latency-sensitive path.
    std::string reason = "unprobed";
};

struct PlatformDecision {
    MitigationDecision child;
    MitigationDecision input;
    MitigationDecision random;
};

// Reads ICECC_P50_PLATFORM plus the three per-mitigation overrides
// (ICECC_P50_PLATFORM_CHILD / _INPUT / _RANDOM), runs whichever probes the
// resulting modes require, and returns the three independent decisions.
// Safe to call more than once (e.g. from tests); the daemon itself must call
// it exactly once at startup and hold the result for the process lifetime.
[[nodiscard]] PlatformDecision decide_platform() noexcept;

// True if any mitigation resolved to FailClosed. The daemon's existing
// FailedClosed/cache=off behavior is unchanged by this module: this helper
// only tells the caller whether to take that existing path.
[[nodiscard]] bool any_fail_closed(const PlatformDecision& decision) noexcept;

// Formats the single startup log line, e.g.:
//   p50 platform: child=native(pidfd ok) input=native(seal round-trip ok) random=native(getrandom ok)
[[nodiscard]] std::string format_platform_log_line(const PlatformDecision& decision) noexcept;

// ---- Individual probes (failure-based; exposed for unit testing) ----

// M1: fork() a short-lived no-op child, pidfd_open() it, waitid(P_PIDFD).
// Returns true if the primitive works. On a clean ENOSYS, sets
// *out_unsupported = true and returns false; any other failure leaves
// *out_unsupported = false (a probe error, not a platform signal).
[[nodiscard]] bool probe_pidfd_child(bool* out_unsupported) noexcept;

// M2: memfd_create(MFD_ALLOW_SEALING), add the three immutable seals, read
// them back. Returns true only if memfd_create succeeded and all three
// seals round-tripped.
[[nodiscard]] bool probe_sealed_memfd(bool* out_unsupported) noexcept;

// M3: getrandom(buf, 32, 0).
[[nodiscard]] bool probe_getrandom(bool* out_unsupported) noexcept;

// ---- M1 compat backend: reusable child-handle abstraction ----
//
// CompatChildHandle and compat_wait_bounded now live in
// cache/p50_compat_child_handle.h (included above), not here, so that
// cache/-level consumers (e.g. the OuterKillDomainVerifier M1 compat path
// in cache/p50_daemon_sidecar_adapter.cpp) can use them without creating a
// cache/-depends-on-daemon/ build edge. They remain in the
// icecc::p50::platform namespace, so every existing daemon/-level use
// (including decide_platform()'s own M1 probe) is unaffected.

// ---- M3 compat backend ----

// Matches cache/p50_incarnation_identity.h's StoreIdentityEntropyProvider
// signature exactly, so it is selectable at that call site (and the
// equivalent one in services/comm.cpp) without an adapter. Opens
// /dev/urandom once per call, O_CLOEXEC, and loops tolerating partial reads
// and EINTR. Returns -1/errno on failure, matching the native provider's
// failure contract.
ssize_t compat_store_identity_entropy(void* buffer, size_t length,
                                      unsigned flags) noexcept;

// ---- M2 compat backend: daemon-side consumer copy ----

// Result of copy_compat_input_from_sidecar(): a daemon-owned, unlinked,
// mode-0400 regular file holding an independently-verified copy of the
// bytes the sidecar handed off. `fd` is O_RDONLY and ready to pass straight
// into forkfd::mint_attached_input_source() under IdentityPolicy::
// CompatPrivateFile; the caller owns it (close it like any other fd).
struct CompatInputCopy {
    int fd = -1;
    bool ok = false;
};

// M2 (input) compat consumer step. Per doc/p50-platform-compat.md, the
// CompatPrivateFile admission identity must be the daemon's own copy, never
// the sidecar's file directly: the sidecar process (and whatever handed it
// that fd over SCM_RIGHTS) is outside this daemon's trust/ownership
// boundary, even though the content already passed the sidecar-side
// materialize() digest check once.
//
// Copies every byte of `source_fd` (must be a regular file) into a new file
// created the same way the sidecar creates its own compat private file:
// mkstemp() + immediate unlink() under a 0700 owner-only directory in
// /dev/shm, hashing while copying. After the copy:
//   - the number of bytes copied must equal source_fd's size observed by
//     fstat() before the copy began (fails closed on a short read or on a
//     source that changed size mid-copy);
//   - the new file is fchmod()ed to 0400 and reopened O_RDONLY via
//     /proc/self/fd (closing the writable descriptor), then its content is
//     read back end-to-end and re-hashed; that second digest must equal the
//     first (fails closed on any mismatch between what was written and what
//     the new file now serves back).
// Does not close `source_fd`; the caller decides when that is no longer
// needed (today: once this call returns, since mint_attached_input_source()
// must bind to the returned copy, not to source_fd).
[[nodiscard]] CompatInputCopy copy_compat_input_from_sidecar(int source_fd) noexcept;

#if defined(ICECC_P50_PLATFORM_COMPAT_TEST_HOOKS)
// Test-only fault injection, following the existing
// ICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS / TestHooks convention. Every probe
// consults these before touching the real syscall, so every errno and every
// partial-support combination is unit-testable on any kernel.
struct TestHooks {
    // 0 = do not force. Otherwise, the probe below behaves as if its first
    // syscall failed with this errno.
    int force_pidfd_open_errno = 0;
    int force_waitid_pidfd_errno = 0;
    int force_memfd_create_errno = 0;
    bool force_seal_roundtrip_failure = false;
    int force_getrandom_errno = 0;
};

void set_test_hooks(TestHooks hooks) noexcept;
void reset_test_hooks() noexcept;
#endif

} // namespace icecc::p50::platform
