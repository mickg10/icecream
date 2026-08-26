#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_BUILDDIR:-.}
cxx=${ICECC_TEST_CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
flags=${ICECC_TEST_CXXFLAGS:-}
cppflags=${ICECC_TEST_CPPFLAGS:-}
bin=${build}/p50forkfdhygiene-sanitize

# Keep sanitizer coverage independently runnable even when automake has not
# been regenerated after this bounded slice.
$cxx $standard -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined,leak \
    $flags $cppflags -DICECC_P50_FORK_FD_HYGIENE_TEST_HOOKS \
    -I"$src" "$src/unittests/p50_fork_fd_hygiene_test.cpp" \
    "$src/daemon/p50_fork_fd_hygiene.cpp" -o "$bin"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$bin"
rm -f "$bin"
echo 'PASS: exact fork descriptor hygiene ASan/UBSan/LSan gate passed'
