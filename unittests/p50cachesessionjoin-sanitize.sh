#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
libtool="$top_build/libtool"
build=$(mktemp -d "${TMPDIR:-/tmp}/p50-session-join-sanitize.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

for file in "$libtool" "$top_build/cache/libprotocol50.a" \
    "$top_build/services/libicecc.la"; do
    test -f "$file"
done

# shellcheck disable=SC2086
"$libtool" --tag=CXX --mode=link "$cxx" "$standard" -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined -Wall -Wextra -Wpedantic -Werror \
    ${ICECC_TEST_CPPFLAGS:-} -I"$src" -I"$src/cache" \
    -I"$src/services" -I"$top_build" \
    "$src/unittests/p50_cache_session_join_test.cpp" \
    "$src/cache/p50_cache_session_join.cpp" \
    "$src/daemon/connection_provenance.cpp" \
    "$top_build/cache/libprotocol50.a" "$top_build/services/libicecc.la" \
    ${ICECC_TEST_LDFLAGS:-} ${ICECC_TEST_LIBZSTD_LIBS:-} \
    ${ICECC_TEST_XXHASH_LIBS:-} -pthread \
    -o "$build/p50sessionjoin-sanitize" >/dev/null

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
    "$build/p50sessionjoin-sanitize"

echo 'ok - authoritative cache-session join ASan/UBSan/LSan gate holds'
