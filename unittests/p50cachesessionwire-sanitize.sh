#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
archive="$top_build/services/.libs/libicecc.a"
build=$(mktemp -d "${TMPDIR:-/tmp}/p50-claim-wire-sanitize.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

test -f "$archive"

# Copy the archive and remove the members we compile instrumented, so the
# linker sees no duplicate definitions whether or not the archive is LTO.
cp "$archive" "$build/libicecc.a"
ar d "$build/libicecc.a" libicecc_la-comm.o libicecc_la-p50_cache_session_wire.o

# shellcheck disable=SC2086
"$cxx" "$standard" -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_LIBCAP_NG_CFLAGS:-} \
    -I"$src" -I"$src/services" -I"$top_build" \
    "$src/unittests/p50_cache_session_wire_test.cpp" \
    "$src/services/comm.cpp" "$src/services/p50_cache_session_wire.cpp" \
    "$build/libicecc.a" ${ICECC_TEST_LDFLAGS:-} \
    ${ICECC_TEST_LIBCAP_NG_LIBS:-} \
    ${ICECC_TEST_LIBS:--llzo2} \
    ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
    ${ICECC_TEST_XXHASH_LIBS:--lxxhash} -ldl -pthread \
    -fsanitize=address,undefined -o "$build/p50cachesessionwire-sanitize"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$build/p50cachesessionwire-sanitize"

echo 'ok - P50 claim/outcome foundation ASan/UBSan/LSan clean'
