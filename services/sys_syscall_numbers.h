/* sys_syscall_numbers.h -- shared build-header syscall-number fallbacks.
 *
 * Two syscalls used on the P50 fast paths are newer than the build headers
 * on one real target: the VFOS5 rpm leg and the build0xx node image are
 * built against Fedora 28 / kernel-headers-4.16, which predates both
 * pidfd_open(2) (5.3) and close_range(2) (5.4) and therefore defines neither
 * SYS_pidfd_open nor SYS_close_range. The RUNTIME kernels this package
 * targets (5.15+) have both syscalls; only the BUILD headers are old. This
 * is the ONE place that supplies each syscall number when the build headers
 * do not, so every caller takes the SAME fast path on a modern kernel and
 * makes the SAME fallback decision on an old one -- a build with old headers
 * but a new kernel still gets the fast path; only a build (or a kernel, or a
 * seccomp profile) that actually cannot do the syscall falls back.
 *
 * Defining the number here does NOT make the syscall available at runtime.
 * A pre-5.9/5.3 kernel still returns ENOSYS. A container under an old
 * seccomp profile that does not recognize the syscall number returns EPERM
 * instead of ENOSYS -- Docker's default seccomp profile denies an
 * unrecognized syscall with EPERM, not ENOSYS, which is exactly the
 * build0xx node image's shape. Every caller of these syscall numbers MUST
 * treat ENOSYS, EINVAL, and EPERM alike as "backend unavailable, fall back"
 * -- never fail closed on EPERM alone.
 *
 * Every translation unit that references SYS_pidfd_open or SYS_close_range
 * must include this header (see unittests/p50syscallshim-source.sh, which
 * enforces that mechanically); a caller that rolls its own
 * `#if !defined(SYS_pidfd_open) ... #define SYS_pidfd_open 434` instead of
 * including this header can silently drift from the number used everywhere
 * else, or -- as happened in production -- simply omit the shim and compile
 * to a permanent ENOSYS fallback on an old-header build (daemon selects the
 * compat child backend, the sidecar never arms, cache=off forever even on a
 * runtime kernel new enough to run the real syscall).
 */
#ifndef ICECC_SYS_SYSCALL_NUMBERS_H
#define ICECC_SYS_SYSCALL_NUMBERS_H

#if defined(__linux__)
#  include <sys/syscall.h>
#  include <sys/types.h>
#  if defined(SYS_close_range)
/* glibc's generated syscall-number table can define SYS_close_range (it is
   just a numeric ABI slot) well before the matching UAPI header exists: the
   VFOS5 rpm's build image (Fedora 28, kernel-headers-4.16.13) defines the
   number but ships no linux/close_range.h (that header postdates the
   4.16-era headers; close_range(2) itself is 5.9+). Only include it when it
   is actually there -- the CLOSE_RANGE_CLOEXEC fallback below supplies the
   one flag bit callers need when it is not. */
#    if __has_include(<linux/close_range.h>)
#      include <linux/close_range.h>
#    endif
#  elif defined(__x86_64__)
/* close_range(2) is x86_64 syscall 436. Left undefined (deliberately, no
   #error) on other architectures: every caller already gates its fast path
   on `defined(SYS_close_range)` and falls back cleanly when it is absent, so
   an unrecognized architecture here is "take the fallback," not a build
   failure. */
#    define SYS_close_range 436
#    define ICECC_SUPPLIED_CLOSE_RANGE_NR 1
#  endif

#  if !defined(SYS_pidfd_open)
/* Unlike SYS_close_range, every caller of SYS_pidfd_open in this tree
   unconditionally selects the compat backend (ENOSYS) when the number is
   absent -- there is no "stay on the fast path anyway" option, so an
   unrecognized architecture here would silently and permanently disable the
   native pidfd backend on a runtime kernel that actually supports it. Fail
   the build instead so a new architecture gets its number added here, not a
   silent downgrade. pidfd_open(2) is 5.3+; numbers from the kernel uapi
   generic syscall table (x86_64/aarch64): 434 pidfd_open. */
#    if defined(__x86_64__) || defined(__aarch64__)
#      define SYS_pidfd_open 434
#      define ICECC_SUPPLIED_PIDFD_OPEN_NR 1
#    else
#      error "SYS_pidfd_open missing from build headers and no number for this arch -- update kernel-headers or add the number here"
#    endif
#  endif
#endif

#if defined(SYS_close_range) && !defined(CLOSE_RANGE_CLOEXEC)
/* linux/close_range.h was not available (old headers); supply the one flag
   bit callers need here. Matches the UAPI value exactly. */
#  define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

#endif /* ICECC_SYS_SYSCALL_NUMBERS_H */
