#!/bin/sh
set -eu

test_srcdir=${srcdir:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
build_dir=${builddir:-$test_srcdir}
cxx=${CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:-${ICE_CXX_STANDARD_FLAG:--std=c++23}}
binary="$build_dir/p50fdhandoff-sanitize"

"$cxx" "$standard" -Wall -Wextra -Wpedantic -Werror -pthread \
    -DICECC_P50_FD_HANDOFF_TEST_HOOKS \
    -fsanitize=address,undefined,leak -fno-omit-frame-pointer \
    -I"$test_srcdir/../cache" \
    "$test_srcdir/p50fdhandoff.cpp" \
    "$test_srcdir/../cache/p50_fd_handoff.cpp" \
    "$test_srcdir/../cache/p50_local_transport.cpp" \
    -o "$binary"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$binary"
rm -f "$binary"
