#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
archive=$(mktemp "${TMPDIR:-/tmp}/icecc-fork-fd-hygiene.XXXXXX.tar")
trap 'rm -f "$archive"' EXIT HUP INT TERM

# The package check is intentionally archive-based and does not depend on a
# worktree checkout.  A release archive must carry production, test, mutant,
# sanitizer, OOT, and package witnesses together.
git -C "$src" archive --format=tar HEAD >"$archive"
for path in \
    daemon/p50_fork_fd_hygiene.cpp \
    daemon/p50_fork_fd_hygiene.h \
    daemon/P50_FORK_FD_HYGIENE.md \
    unittests/p50_fork_fd_hygiene_test.cpp \
    unittests/p50forkfdhygiene-source.sh \
    unittests/p50forkfdhygiene-mutants.sh \
    unittests/p50_fork_fd_hygiene_sanitize.sh \
    unittests/p50forkfdhygiene-oot.sh \
    unittests/p50forkfdhygiene-package.sh; do
    tar -tf "$archive" | grep -Fx "$path" >/dev/null || {
        echo "FAIL: package archive omits $path" >&2
        exit 1
    }
done
echo "package sha256: $(sha256sum "$archive")"
echo 'PASS: exact fork descriptor hygiene package inventory/hash gate passed'
