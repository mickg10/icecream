#!/bin/sh
set -eu

if [ -n "${ICECC_TEST_TOP_SRCDIR:-}" ]; then
    test_srcdir="$ICECC_TEST_TOP_SRCDIR/unittests"
else
    test_srcdir=${srcdir:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
fi
build_dir=${ICECC_TEST_BUILDDIR:-${builddir:-$test_srcdir}}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:-${ICE_CXX_STANDARD_FLAG:--std=c++23}}
cxxflags=${ICECC_TEST_CXXFLAGS:-${CXXFLAGS:-}}
cppflags=${ICECC_TEST_CPPFLAGS:-${CPPFLAGS:-}}
ldflags=${ICECC_TEST_LDFLAGS:-${LDFLAGS:-}}
libs=${ICECC_TEST_LIBS:-${LIBS:-}}
sanitize_flags=${ICECC_TEST_SANITIZE_FLAGS:--fsanitize=address,undefined,leak}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-${top_builddir:-}}
if [ -z "$top_build_dir" ] && [ -d "$build_dir/.." ]; then
    top_build_dir=$(CDPATH= cd -- "$build_dir/.." && pwd)
fi
services_lib="$top_build_dir/services/.libs/libicecc.a"

# MsgChannel lives in libicecc; if this optional sanitizer is invoked from a
# source-only checkout, leave the existing focused sanitizer gate to cover the
# transport primitives rather than inventing a parallel comm build.
if [ -z "$top_build_dir" ] || [ ! -f "$services_lib" ]; then
    echo 'skip - sanitizer requires a configured libicecc build'
    exit 0
fi

mkdir -p "$build_dir"
binary=$(mktemp "$build_dir/p50daemoncachedispatch-sanitize.XXXXXX")
cleanup() {
    rm -f -- "$binary"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

lzo_lib=${ICECC_TEST_LZO_LIB:-}
if [ -z "$lzo_lib" ] && [ -f /lib/x86_64-linux-gnu/liblzo2.so.2 ]; then
    lzo_lib=/lib/x86_64-linux-gnu/liblzo2.so.2
fi
extra_cflags="${ICECC_TEST_LIBZSTD_CFLAGS:-} ${ICECC_TEST_LIBCAP_NG_CFLAGS:-}"
extra_libs="${ICECC_TEST_LIBZSTD_LIBS:--lzstd} ${ICECC_TEST_XXHASH_LIBS:--lxxhash}"
if [ -n "$lzo_lib" ]; then
    extra_libs="$extra_libs $lzo_lib"
else
    extra_libs="$extra_libs -llzo2"
fi
extra_libs="$extra_libs ${ICECC_TEST_LIBCAP_NG_LIBS:-} -ldl"

"$cxx" "$standard" $cxxflags $cppflags $extra_cflags \
    -Wall -Wextra -Wpedantic -Werror -pthread \
    $sanitize_flags -fno-omit-frame-pointer \
    -I"$test_srcdir/.." -I"$test_srcdir/../cache" -I"$test_srcdir/../services" \
    "$test_srcdir/p50_daemon_cache_dispatch_test.cpp" \
    "$test_srcdir/../cache/p50_daemon_cache_dispatch.cpp" \
    "$test_srcdir/../cache/p50_fd_handoff.cpp" \
    "$test_srcdir/../cache/p50_local_transport.cpp" \
    "$test_srcdir/../cache/p50_control_operation.cpp" \
    "$test_srcdir/../cache/p50_phase_open.cpp" \
    "$test_srcdir/../cache/p50_source_identity.cpp" \
    "$test_srcdir/../cache/protocol50.cpp" \
    $ldflags "$services_lib" $extra_libs $libs \
    -o "$binary"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$binary"
