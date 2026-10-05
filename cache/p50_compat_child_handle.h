#pragma once

#include <cstdint>

// M1 child-handle compat primitive: identifies a child process by
// (pid, start-time ticks) instead of a pidfd, for kernel-3.10-class hosts
// where pidfd_open()/waitid(P_PIDFD) is ENOSYS. See
// doc/p50-platform-compat.md.
//
// This lives in cache/, not daemon/, even though the platform *decision*
// that selects it (icecc::p50::platform::decide_platform(), in
// daemon/p50_platform_compat.h) is a daemon-level concept. The reason is
// build-system layering: daemon/ depends on cache/'s static libraries
// (daemon/Makefile.am adds -I$(top_srcdir)/cache and links iceccd against
// several ../cache/lib*.a), but there is no reverse path, and at least one
// consumer of cache/'s libp50sidecar.a -- the p50daemonsidecaradapter unit
// test in unittests/Makefile.am -- links libp50daemonsidecaradapter.a and
// libp50sidecar.a without compiling or linking anything from daemon/ at
// all. A type used from cache/p50_daemon_sidecar_adapter.cpp (as this one
// is, for the OuterKillDomainVerifier M1 compat path) must therefore be
// defined somewhere cache/'s own libraries already reach, or every such
// consumer breaks with undefined references. daemon/p50_platform_compat.h
// includes this header and keeps re-exporting these names unchanged from
// icecc::p50::platform, so decide_platform()'s own M1 probe, and any other
// daemon/-level caller, see no difference.

namespace icecc::p50 {

// Which child-identity primitive a given KillDomainVerifier (or other M1
// consumer) should use. Declared here, directly in icecc::p50, rather than
// in icecc::p50::platform below, so it is usable unqualified from
// icecc::p50::daemon (cache/p50_daemon_sidecar_adapter.h's Config struct) --
// the same reason InputMaterializationBackend (cache/p50_input_fd_attachment.h)
// and StoreIdentityEntropyProvider (cache/p50_incarnation_identity.h) also
// live directly in icecc::p50 rather than in a sub-namespace. Deliberately
// independent of icecc::p50::platform::Backend (the three-way
// Native/Compat/FailClosed platform *decision* type declared in
// daemon/p50_platform_compat.h): by the time any cache/-level code runs,
// any_fail_closed() has already sent a FailClosed decision down the
// existing FailedClosed/cache=off path, so a cache/-level consumer only
// ever needs to choose between these two.
enum class ChildHandleBackend : uint8_t {
    Native,
    Compat,
};

} // namespace icecc::p50

namespace icecc::p50::platform {

// Captures (pid, starttime ticks) from /proc/<pid>/stat. The residual risk
// this accepts, relative to a kernel-held pidfd, is a pid_max wrap between
// the initial capture and a later re-check; see doc/p50-platform-compat.md,
// "What each compat backend weakens".
class CompatChildHandle {
public:
    CompatChildHandle() noexcept = default;

    // False if the process cannot be read (already gone, or /proc is
    // unavailable).
    [[nodiscard]] bool arm(long pid) noexcept;

    // Re-reads /proc/<pid>/stat and returns whether it is still the same
    // (pid, starttime) pair this handle was armed with. False once the pid
    // has exited and been reaped past where /proc can see it, and false
    // (deliberately, fail-closed) if a *different* process now holds pid.
    [[nodiscard]] bool still_same_process() const noexcept;

    [[nodiscard]] bool armed() const noexcept { return pid_ > 0; }
    [[nodiscard]] long pid() const noexcept { return pid_; }

private:
    long pid_ = -1;
    unsigned long long starttime_ticks_ = 0;
};

// Bounded compat wait: polls waitid(..., WNOWAIT) for `pid` without reaping
// it, re-checking `handle` between polls, until the child exits, the handle
// no longer matches (fail closed), or `timeout_ms` elapses (fail closed).
// On success, reaps the child (WNOWAIT cleared) and stores its exit status.
[[nodiscard]] bool compat_wait_bounded(long pid, const CompatChildHandle& handle,
                                       int timeout_ms, int* exit_status) noexcept;

} // namespace icecc::p50::platform
