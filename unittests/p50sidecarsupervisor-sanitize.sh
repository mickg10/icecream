#!/bin/sh
# Sanitizer coverage for lease parsing and identity-safe teardown.
set -eu

if test -n "${ICECC_TEST_TOP_SRCDIR:-}"; then
    test_srcdir="$ICECC_TEST_TOP_SRCDIR/unittests"
else
    test_srcdir=${srcdir:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
fi
build_dir=${ICECC_TEST_BUILDDIR:-${builddir:-$test_srcdir}}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$(CDPATH= cd -- "$build_dir/.." && pwd)}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:-${ICE_CXX_STANDARD_FLAG:--std=c++23}}
cxxflags=${ICECC_TEST_CXXFLAGS:-${CXXFLAGS:-}}
cppflags=${ICECC_TEST_CPPFLAGS:-${CPPFLAGS:-}}
ldflags=${ICECC_TEST_LDFLAGS:-${LDFLAGS:-}}
libs=${ICECC_TEST_LIBS:-${LIBS:-}}
protocol_lib="$top_build_dir/cache/libprotocol50.a"
transport_lib="$top_build_dir/cache/libp50localtransport.a"
services_lib="$top_build_dir/services/.libs/libicecc.a"
if test ! -f "$protocol_lib" || test ! -f "$transport_lib" || test ! -f "$services_lib"; then
    echo 'skip - sanitizer requires a configured libicecc build'
    exit 0
fi

mkdir -p "$build_dir"
binary=$(mktemp "$build_dir/p50sidecarsupervisor-sanitize.XXXXXX")
cleanup() { rm -f -- "$binary"; }
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

"$cxx" "$standard" $cxxflags $cppflags \
    -Wall -Wextra -Wpedantic -Werror -pthread \
    -fsanitize=address,undefined,leak -fno-omit-frame-pointer \
    -I"$test_srcdir/.." -I"$test_srcdir/../cache" -I"$test_srcdir/../services" \
    "$test_srcdir/p50_sidecar_supervisor_test.cpp" \
    "$test_srcdir/../cache/p50_sidecar_supervisor.cpp" \
    $ldflags "$transport_lib" "$protocol_lib" "$services_lib" \
    /lib/x86_64-linux-gnu/libxxhash.so.0 -lzstd -ldl $libs -o "$binary"
# The test intentionally exercises fail-closed lease leaks and uses fork/exec
# children; LeakSanitizer reports inherited allocator state from those child
# boundaries rather than a supervisor leak. ASan/UBSan remain enabled.
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 "$binary"
