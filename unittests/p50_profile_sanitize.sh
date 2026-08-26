#!/bin/sh
# Address/undefined/leak gate for the type-erased profile boundary.
set -eu

if [ -n "${ICECC_TEST_TOP_SRCDIR:-}" ]; then
    test_srcdir="$ICECC_TEST_TOP_SRCDIR/unittests"
else
    test_srcdir=${srcdir:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
fi
build_dir=${ICECC_TEST_BUILDDIR:-${builddir:-$test_srcdir}}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-}
services_lib="$top_build_dir/services/.libs/libicecc.a"
if [ -z "$top_build_dir" ] || [ ! -f "$services_lib" ]; then
    echo 'skip - profile sanitizer requires a configured libicecc build'
    exit 0
fi

cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:-${ICE_CXX_STANDARD_FLAG:--std=c++23}}
cxxflags=${ICECC_TEST_CXXFLAGS:-${CXXFLAGS:-}}
cppflags=${ICECC_TEST_CPPFLAGS:-${CPPFLAGS:-}}
ldflags=${ICECC_TEST_LDFLAGS:-${LDFLAGS:-}}
libs=${ICECC_TEST_LIBS:-${LIBS:-}}
xxhash_cflags=${ICECC_TEST_XXHASH_CFLAGS:-}
xxhash_libs=${ICECC_TEST_XXHASH_LIBS:-}
zstd_cflags=${ICECC_TEST_LIBZSTD_CFLAGS:-}
zstd_libs=${ICECC_TEST_LIBZSTD_LIBS:-}
sanitize_flags=${ICECC_TEST_SANITIZE_FLAGS:--fsanitize=address,undefined,leak}
mkdir -p "$build_dir"
binary=$(mktemp "$build_dir/p50profile-sanitize.XXXXXX")
cleanup() { rm -f -- "$binary"; }
trap cleanup EXIT HUP INT TERM

"$cxx" "$standard" $cxxflags $cppflags $xxhash_cflags $zstd_cflags \
    -Wall -Wextra -Wpedantic -Werror -pthread $sanitize_flags \
    -fno-omit-frame-pointer -I"$test_srcdir/.." \
    "$test_srcdir/p50_profile_test.cpp" \
    "$test_srcdir/../cache/p50_profile.cpp" \
    "$test_srcdir/../cache/p50_zstd.cpp" \
    "$test_srcdir/../cache/protocol50.cpp" \
    "$test_srcdir/../cache/p50_actions.cpp" \
    "$test_srcdir/../cache/p50_slice0.cpp" \
    $ldflags "$services_lib" $zstd_libs $xxhash_libs $libs -o "$binary"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$binary"
echo 'PASS: profile vtable ASan/UBSan/LSan gate passed'
