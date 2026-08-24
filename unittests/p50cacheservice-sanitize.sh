#!/bin/sh
# Real executable sanitizer gate for the sidecar runtime and its live tests.
set -eu

test_srcdir=${srcdir:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
build_dir=${builddir:-$test_srcdir}
cxx=${CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:-${ICE_CXX_STANDARD_FLAG:--std=c++23}}
binary="$build_dir/p50cacheservice-sanitize"
dep_libdir=${ICECC_TEST_DEP_LIBDIR:-}
if test -z "$dep_libdir" && test -n "${ICECC_TEST_PKG_CONFIG_PATH:-}"; then
    dep_libdir=${ICECC_TEST_PKG_CONFIG_PATH%/pkgconfig}
fi
dep_ldflags=
if test -n "$dep_libdir"; then
    dep_ldflags="-L$dep_libdir"
fi

"$cxx" "$standard" ${CXXFLAGS:-} ${CPPFLAGS:-} \
    ${ICECC_TEST_BOOST_CPPFLAGS:-} ${ICECC_TEST_CPPFLAGS:-} \
    -Wall -Wextra -Wpedantic -Werror -pthread \
    -DICECC_P50_CACHE_SERVICE_NO_MAIN -fsanitize=address,undefined,leak \
    -fno-omit-frame-pointer -I"$test_srcdir/../cache" \
    -I"$test_srcdir/../services" -I"$test_srcdir/.." \
    "$test_srcdir/p50cacheservice.cpp" \
    "$test_srcdir/../cache/p50_cache_service.cpp" \
    "$build_dir/../cache/libp50endpoint.a" \
    "$build_dir/../cache/libprotocol50.a" \
    "$build_dir/../cache/libp50localtransport.a" \
    "$build_dir/../services/.libs/libicecc.a" \
    $dep_ldflags ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
    ${ICECC_TEST_XXHASH_LIBS:--lxxhash} -ldl -o "$binary"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
    ICECC_TEST_CACHE_SERVICE=${ICECC_TEST_CACHE_SERVICE:?missing service executable} \
    ICECC_TEST_READY_CLOSE_SHIM=${ICECC_TEST_READY_CLOSE_SHIM:?missing ready shim} \
    "$binary"

rm -f "$binary"
