#!/bin/sh
# Real executable sanitizer gate for the sidecar runtime and its live tests.
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
boost_cppflags=${ICECC_TEST_BOOST_CPPFLAGS:-${BOOST_CPPFLAGS:-}}
binary=$(mktemp "$build_dir/p50cacheservice-sanitize.XXXXXX")
cleanup() {
    rm -f -- "$binary"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
dep_libdir=${ICECC_TEST_DEP_LIBDIR:-}
if test -z "$dep_libdir" && test -n "${ICECC_TEST_PKG_CONFIG_PATH:-}"; then
    dep_libdir=${ICECC_TEST_PKG_CONFIG_PATH%/pkgconfig}
fi
dep_ldflags=
if test -n "$dep_libdir"; then
    dep_ldflags="-L$dep_libdir"
fi
lzo_lib=${ICECC_TEST_LZO_LIBS:-}
if test -z "$lzo_lib" && test -f /lib/x86_64-linux-gnu/liblzo2.so.2; then
    lzo_lib=/lib/x86_64-linux-gnu/liblzo2.so.2
fi
if test -z "$lzo_lib"; then
    lzo_lib=-llzo2
fi

"$cxx" "$standard" $cxxflags $cppflags \
    $boost_cppflags ${ICECC_TEST_LIBZSTD_CFLAGS:-} \
    ${ICECC_TEST_XXHASH_CFLAGS:-} \
    -Wall -Wextra -Wpedantic -Werror -pthread \
    -DICECC_P50_CACHE_SERVICE_NO_MAIN -fsanitize=address,undefined,leak \
    -fno-omit-frame-pointer -I"$test_srcdir/../cache" \
    -I"$test_srcdir/../services" -I"$test_srcdir/.." \
    $ldflags \
    "$test_srcdir/p50cacheservice.cpp" \
    "$test_srcdir/../cache/p50_cache_service.cpp" \
    "$build_dir/../cache/libp50endpoint.a" \
    "$build_dir/../cache/libp50inputfd.a" \
    "$build_dir/../cache/libp50inputlifecycle.a" \
    "$build_dir/../cache/libprotocol50.a" \
    "$build_dir/../cache/libp50localtransport.a" \
    "$build_dir/../services/.libs/libicecc.a" \
    $dep_ldflags ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
    ${ICECC_TEST_XXHASH_LIBS:--lxxhash} $lzo_lib -ldl $libs -o "$binary"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
    ICECC_TEST_CACHE_SERVICE=${ICECC_TEST_CACHE_SERVICE:-$top_build_dir/cache/icecc-cache-service} \
    ICECC_TEST_READY_CLOSE_SHIM=${ICECC_TEST_READY_CLOSE_SHIM:-$build_dir/readyclose_shim.so} \
    "$binary"
