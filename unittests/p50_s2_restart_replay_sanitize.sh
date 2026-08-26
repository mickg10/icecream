#!/bin/sh
# Optional ASan/UBSan/LSan gate for the bounded S2 restart/replay harness.
set -eu

if [ -n "${ICECC_TEST_TOP_SRCDIR:-}" ]; then
    test_srcdir="$ICECC_TEST_TOP_SRCDIR/unittests"
else
    test_srcdir=${srcdir:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
fi
build_dir=${ICECC_TEST_BUILDDIR:-${builddir:-$test_srcdir}}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-${top_builddir:-}}
if [ -z "$top_build_dir" ] && [ -d "$build_dir/.." ]; then
    top_build_dir=$(CDPATH= cd -- "$build_dir/.." && pwd)
fi
services_lib="$top_build_dir/services/.libs/libicecc.a"
if [ -z "$top_build_dir" ] || [ ! -f "$services_lib" ]; then
    echo 'skip - S2 sanitizer requires a configured libicecc build'
    exit 0
fi

cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:-${ICE_CXX_STANDARD_FLAG:--std=c++23}}
cxxflags=${ICECC_TEST_CXXFLAGS:-${CXXFLAGS:-}}
cppflags=${ICECC_TEST_CPPFLAGS:-${CPPFLAGS:-}}
ldflags=${ICECC_TEST_LDFLAGS:-${LDFLAGS:-}}
libs=${ICECC_TEST_LIBS:-${LIBS:-}}
sanitize_flags=${ICECC_TEST_SANITIZE_FLAGS:--fsanitize=address,undefined,leak}
mkdir -p "$build_dir"
binary=$(mktemp "$build_dir/p50s2restartreplay-sanitize.XXXXXX")
cleanup() { rm -f -- "$binary"; }
trap cleanup EXIT HUP INT TERM

"$cxx" "$standard" $cxxflags $cppflags -Wall -Wextra -Wpedantic -Werror \
    -pthread $sanitize_flags -fno-omit-frame-pointer \
    -I"$test_srcdir/.." -I"$test_srcdir/../cache" -I"$test_srcdir/../services" \
    "$test_srcdir/p50_s2_restart_replay_test.cpp" \
    "$test_srcdir/../cache/p50_sidecar_supervisor.cpp" \
    "$test_srcdir/../cache/p50_reverse_fd_retry.cpp" \
    "$test_srcdir/../cache/p50_fd_handoff.cpp" \
    "$test_srcdir/../cache/p50_local_transport.cpp" \
    "$test_srcdir/../cache/p50_phase_open.cpp" \
    "$test_srcdir/../cache/p50_source_identity.cpp" \
    $ldflags "$services_lib" $libs -o "$binary"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$binary"
echo 'PASS: bounded S2 restart/replay ASan/UBSan/LSan gate passed'
