#!/bin/sh
set -eu

test_srcdir=${srcdir:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
build_dir=${builddir:-$test_srcdir}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:-${ICE_CXX_STANDARD_FLAG:--std=c++23}}
sanitize_flags=${ICECC_TEST_SANITIZE_FLAGS:--fsanitize=address,undefined,leak}
mkdir -p "$build_dir"
binary=$(mktemp "$build_dir/p50localtransport-sanitize.XXXXXX")
cleanup() {
    rm -f -- "$binary"
}
trap cleanup EXIT HUP INT TERM

"$cxx" "$standard" -Wall -Wextra -Wpedantic -Werror -pthread \
    -DICECC_P50_LOCAL_TRANSPORT_TEST_HOOKS $sanitize_flags -fno-omit-frame-pointer \
    -I"$test_srcdir/.." \
    "$test_srcdir/p50_local_transport_test.cpp" \
    "$test_srcdir/../cache/p50_local_transport.cpp" \
    -o "$binary"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 "$binary"
