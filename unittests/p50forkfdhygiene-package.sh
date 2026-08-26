#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_TOP_BUILDDIR:-${ICECC_TEST_BUILDDIR:-$src}}
make_cmd=${ICECC_TEST_MAKE:-make}
archive=
entries=
trap 'rm -f "$entries"' EXIT HUP INT TERM

# Exercise the release path itself.  `git archive` is deliberately not used:
# it bypasses Automake EXTRA_DIST and would let a broken `make dist` pass this
# gate while omitting the fork-hygiene witness scripts.
"$make_cmd" -C "$build" dist >/dev/null
archive=$(find "$build" -maxdepth 1 -type f \( -name '*.tar' -o -name '*.tar.gz' -o -name '*.tar.xz' -o -name '*.tar.bz2' \) \
    -printf '%T@ %p\n' | sort -nr | sed -n '1s/^[^ ]* //p')
test -n "$archive" || {
    echo 'FAIL: make dist produced no release archive' >&2
    exit 1
}
entries=$(mktemp "${TMPDIR:-/tmp}/icecc-fork-fd-hygiene-entries.XXXXXX")
tar -tf "$archive" >"$entries"
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
    grep -E "(^|/)${path}$" "$entries" >/dev/null || {
        echo "FAIL: package archive omits $path" >&2
        exit 1
    }
done
echo "package sha256: $(sha256sum "$archive")"
echo 'PASS: exact fork descriptor hygiene package inventory/hash gate passed'
