#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
cxx=${ICECC_TEST_CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/p50forkfdhygiene-oot.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/daemon" "$tmp/unittests"
cp "$src/daemon/p50_fork_fd_hygiene.cpp" "$tmp/daemon/"
cp "$src/daemon/p50_fork_fd_hygiene.h" "$tmp/daemon/"
cp "$src/unittests/p50_fork_fd_hygiene_test.cpp" "$tmp/unittests/"
"$cxx" "$standard" -Wall -Wextra -Werror \
    -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS -I"$tmp" \
    "$tmp/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$tmp/daemon/p50_fork_fd_hygiene.cpp" -o "$tmp/test"
"$tmp/test"
echo 'PASS: exact fork descriptor hygiene out-of-tree compile/run passed'
