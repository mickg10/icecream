#!/bin/sh
set -eu

test_srcdir=${srcdir:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
build_dir=${builddir:-$test_srcdir}
cxx=${CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:-${ICE_CXX_STANDARD_FLAG:--std=c++23}}
binary="$build_dir/p50daemoncachedispatch-sanitize"
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-}
services_lib="$top_build_dir/services/.libs/libicecc.a"

# MsgChannel lives in libicecc; if this optional sanitizer is invoked from a
# source-only checkout, leave the existing focused sanitizer gate to cover the
# transport primitives rather than inventing a parallel comm build.
if [ -z "$top_build_dir" ] || [ ! -f "$services_lib" ]; then
    echo 'skip - sanitizer requires a configured libicecc build'
    exit 0
fi

lzo_lib=${ICECC_TEST_LZO_LIB:-}
if [ -z "$lzo_lib" ] && [ -f /lib/x86_64-linux-gnu/liblzo2.so.2 ]; then
    lzo_lib=/lib/x86_64-linux-gnu/liblzo2.so.2
fi
extra_libs="-lzstd"
if [ -n "$lzo_lib" ]; then
    extra_libs="$extra_libs $lzo_lib"
fi

"$cxx" "$standard" -Wall -Wextra -Wpedantic -Werror -pthread \
    -fsanitize=address,undefined,leak -fno-omit-frame-pointer \
    -I"$test_srcdir/.." -I"$test_srcdir/../cache" -I"$test_srcdir/../services" \
    "$test_srcdir/p50_daemon_cache_dispatch_test.cpp" \
    "$test_srcdir/../cache/p50_daemon_cache_dispatch.cpp" \
    "$test_srcdir/../cache/p50_fd_handoff.cpp" \
    "$test_srcdir/../cache/p50_local_transport.cpp" \
    "$services_lib" $extra_libs \
    -o "$binary"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$binary"
rm -f "$binary"
