#!/bin/sh
set -eu

root=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
work=$(mktemp -d "${TMPDIR:-/tmp}/p50-input-lifecycle-sanitize.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

"$cxx" "$standard" -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread \
    -fsanitize=address,undefined,leak -fno-omit-frame-pointer \
    -DHAVE_CONFIG_H -I"$build" -I"$root" -I"$root/cache" -I"$root/services" \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_LIBZSTD_CFLAGS:-} \
    "$root/unittests/p50_input_lifecycle_test.cpp" \
    "$root/cache/p50_input_lifecycle.cpp" \
    "$root/cache/p50_control_operation.cpp" \
    "$root/cache/p50_local_transport.cpp" \
    "$build/cache/libprotocol50.a" \
    "$build/services/.libs/libicecc.a" \
    ${ICECC_TEST_LIBCAP_NG_LIBS:-} -llzo2 \
    ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
    ${ICECC_TEST_XXHASH_LIBS:--lxxhash} ${ICECC_TEST_LDFLAGS:-} \
    -o "$work/p50inputlifecycle"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
    "$work/p50inputlifecycle"
echo 'PASS: p50 input lifecycle ASan/UBSan/LSan runtime'
