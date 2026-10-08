#!/bin/sh
# Deletion-sensitive gate: every translation unit that tests or uses
# SYS_pidfd_open / SYS_close_range must include the one shared fallback
# header, services/sys_syscall_numbers.h. A caller that omits it (or rolls
# its own ad hoc shim instead) can compile to a permanent ENOSYS fallback on
# a build with old headers (Fedora 28 / kernel-headers-4.16, the VFOS5 rpm
# leg and the build0xx node image) even on a runtime kernel new enough to
# run the real syscall -- the production bug this header exists to prevent.
# See services/sys_syscall_numbers.h for the full rationale.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
header="$src/services/sys_syscall_numbers.h"

test -s "$header"
grep -F 'SYS_pidfd_open' "$header" >/dev/null
grep -F 'SYS_close_range' "$header" >/dev/null
grep -F '#error' "$header" >/dev/null

fail=0
# Every .cpp/.h in the tree (excluding the shared header itself) that
# actually references either syscall as code -- `defined(SYS_...)` or
# `::syscall(SYS_...` -- must include the shared header. Narrowing to these
# two patterns (rather than a bare name grep) deliberately excludes prose
# comments that merely discuss the syscall (e.g. unittests/p50_fork_fd_hygiene_test.cpp's
# "glibc headers that predate SYS_close_range" note) without using it, so
# this gate stays a precise anchor on real callers, not vacuous noise.
for candidate in $(grep -lE 'defined\(SYS_pidfd_open\)|defined\(SYS_close_range\)|::syscall\(SYS_pidfd_open|::syscall\(SYS_close_range' \
        -r "$src" --include='*.cpp' --include='*.h' 2>/dev/null); do
    case "$candidate" in
        */services/sys_syscall_numbers.h) continue ;;
    esac
    if ! grep -F '#include "services/sys_syscall_numbers.h"' "$candidate" >/dev/null; then
        echo "FAIL: $candidate references SYS_pidfd_open/SYS_close_range without including services/sys_syscall_numbers.h" >&2
        fail=1
    fi
done

# Known callers must stay on the list; a caller silently dropping its
# syscall-number reference entirely (e.g. reverting to a hand-rolled shim
# that doesn't name SYS_pidfd_open/SYS_close_range at all) would make the
# scan above vacuous for that file.
for known in \
    "$src/daemon/p50_platform_compat.cpp" \
    "$src/cache/p50_daemon_sidecar_adapter.cpp" \
    "$src/daemon/p50_fork_fd_hygiene.cpp" \
    "$src/unittests/support/p50_sidecar_supervisor.cpp"; do
    if ! grep -E 'defined\(SYS_pidfd_open\)|defined\(SYS_close_range\)|::syscall\(SYS_pidfd_open|::syscall\(SYS_close_range' \
            "$known" >/dev/null 2>&1; then
        echo "FAIL: $known no longer references SYS_pidfd_open/SYS_close_range -- update this gate's known-caller list" >&2
        fail=1
    fi
    if ! grep -F '#include "services/sys_syscall_numbers.h"' "$known" >/dev/null 2>&1; then
        echo "FAIL: $known does not include services/sys_syscall_numbers.h" >&2
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo 'PASS: every SYS_pidfd_open/SYS_close_range caller includes the shared syscall-number shim'
