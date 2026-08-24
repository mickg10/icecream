#!/bin/sh
set -eu

root=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
work=$(mktemp -d "${TMPDIR:-/tmp}/p50-completion-record-sanitize.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

"$cxx" "$standard" -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread \
    -fsanitize=address,undefined,leak -fno-omit-frame-pointer \
    -DHAVE_CONFIG_H -I"$build" -I"$root" -I"$root/daemon" \
    -I"$root/client" -I"$root/services" \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_LIBZSTD_CFLAGS:-} \
    "$root/unittests/p50_completion_record_test.cpp" \
    "$root/daemon/p50_completion_record.cpp" \
    "$build/services/.libs/libicecc.a" -llzo2 \
    ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
    ${ICECC_TEST_XXHASH_LIBS:--lxxhash} ${ICECC_TEST_LDFLAGS:-} \
    -o "$work/p50completionrecord"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
    "$work/p50completionrecord"
echo 'PASS: P50 completion record/disposition ASan/UBSan/LSan runtime'
