/* sys_close_range.h -- one shared SYS_close_range fallback definition.
 *
 * close_range(2) is Linux kernel 5.9+, exposed via glibc only on recent
 * headers.  Old-glibc builds (Fedora 28-era headers, used by the build0xx
 * node image and the VFOS5 rpm) do not define SYS_close_range, which used
 * to make every caller that gated a fast path on `defined(SYS_close_range)`
 * compile that fast path OUT and run its fallback unconditionally, even on
 * a kernel new enough to support the real syscall.  This header is the ONE
 * place that supplies the syscall number for x86_64 when the system headers
 * do not, so every caller (daemon/p50_fork_fd_hygiene.cpp,
 * cache/p50_daemon_sidecar_adapter.cpp) takes the SAME fast path on a modern
 * kernel and makes the SAME fallback decision on an old one -- a build with
 * old headers but a new kernel still gets the fast path; only a build (or a
 * kernel, or a seccomp profile) that actually cannot do close_range falls
 * back.
 *
 * Defining SYS_close_range here does not make the syscall available at
 * runtime.  A pre-5.9 kernel still returns ENOSYS.  A container under an
 * old seccomp profile that does not recognize syscall number 436 returns
 * EPERM instead of ENOSYS -- Docker's default seccomp profile denies an
 * unrecognized syscall with EPERM, not ENOSYS, which is exactly the build0xx
 * node image's shape.  Every caller of this syscall number MUST treat
 * ENOSYS, EINVAL, and EPERM alike as "backend unavailable, fall back" --
 * never fail closed on EPERM alone.
 */
#ifndef ICECC_SYS_CLOSE_RANGE_H
#define ICECC_SYS_CLOSE_RANGE_H

#if defined(__linux__)
#  include <sys/syscall.h>
#  include <sys/types.h>
#  if defined(SYS_close_range)
#    include <linux/close_range.h>
#  elif defined(__x86_64__)
/* close_range(2) is x86_64 syscall 436.  Supply the number the way
   pidfd-uapi-style shims do; see the file comment above for the ENOSYS vs
   EPERM distinction every caller must honor. */
#    define SYS_close_range 436
#    define ICECC_SUPPLIED_CLOSE_RANGE_NR 1
#  endif
#endif

#if defined(SYS_close_range) && !defined(CLOSE_RANGE_CLOEXEC)
/* linux/close_range.h was not available (old headers); supply the one flag
   bit callers need here.  Matches the UAPI value exactly. */
#  define CLOSE_RANGE_CLOEXEC (1U << 2)
#endif

#endif /* ICECC_SYS_CLOSE_RANGE_H */
