#!/bin/sh
set -eu
root=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=$(mktemp -d "${TMPDIR:-/tmp}/p50-daemon-control-sanitize.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
cxx=${CXX:-${ICECC_TEST_CXX:-g++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
"$cxx" "$standard" -O1 -g -fno-omit-frame-pointer -pthread \
    -fsanitize=address,undefined,leak -I"$root" \
    "$root/unittests/p50_daemon_control_test.cpp" \
    "$root/cache/p50_daemon_control.cpp" "$root/cache/p50_local_transport.cpp" \
    -o "$build/p50daemoncontrol"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} "$build/p50daemoncontrol"
echo 'PASS: daemon incremental control ASan/UBSan/LSan gate'
