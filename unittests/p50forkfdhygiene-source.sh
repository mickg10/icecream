#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
source=$src/daemon/p50_fork_fd_hygiene.cpp
header=$src/daemon/p50_fork_fd_hygiene.h
serve=$src/daemon/serve.cpp
main=$src/daemon/main.cpp
doc=$src/daemon/P50_FORK_FD_HYGIENE.md
makefile=$src/daemon/Makefile.am
test -f "$source" -a -f "$header"
grep -F 'SYS_close_range' "$source" >/dev/null
grep -F 'close_range' "$source" >/dev/null
if grep -F 'CLOSE_RANGE_CLOEXEC' "$source" >/dev/null; then
    echo 'FAIL: first-fork hygiene must use real close_range, not CLOEXEC' >&2
    exit 1
fi
grep -F 'force_proc_failure' "$source" >/dev/null
grep -F 'force_proc_close_ebadf' "$source" >/dev/null
grep -F 'Failure::EnumerationFailure' "$source" >/dev/null
grep -F 'Failure::ParseFailure' "$source" >/dev/null
grep -F 'Failure::CloseFailure' "$source" >/dev/null
grep -F 'ForkSourceLease' "$header" >/dev/null
grep -F 'mint_fork_source_lease' "$header" >/dev/null
grep -F 'source_required' "$header" >/dev/null
grep -F 'FD_CLOEXEC' "$source" >/dev/null
grep -F 'O_WRONLY' "$source" >/dev/null
grep -F 'O_RDWR' "$source" >/dev/null
grep -F 'SO_TYPE' "$source" >/dev/null
grep -F 'getpeername' "$source" >/dev/null
if grep -F 'errno != EBADF' "$source" >/dev/null; then
    echo 'FAIL: /proc close EBADF is still ignored' >&2
    exit 1
fi
if grep -F 'close_unneeded_fds_in_child' "$serve" >/dev/null; then
    echo 'FAIL: best-effort close helper survived in production serve path' >&2
    exit 1
fi
grep -F 'forkfd::sweep' "$serve" >/dev/null
grep -F 'before reset_debug/work_it' "$serve" >/dev/null
grep -F 'compiler_input_source' "$serve" >/dev/null
grep -F 'ForkSourceLease' "$main" "$serve" >/dev/null
grep -F 'it MUST erase it before this TOCOMPILE/fork' "$main" >/dev/null
grep -F 'explicitly HOLD' "$doc" >/dev/null
grep -F 'p50_fork_fd_hygiene.cpp' "$makefile" >/dev/null
echo 'PASS: exact fork descriptor hygiene source anchors'
