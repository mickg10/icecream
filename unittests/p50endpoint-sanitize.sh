#!/bin/sh
# ASan/UBSan/LSan gate for the typed endpoint and two-phase InputRecord seam.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
libtool=${ICECC_TEST_LIBTOOL:-$top_build/libtool}
work=$(mktemp -d "${TMPDIR:-/tmp}/p50-endpoint-sanitize.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

for required in "$libtool" "$top_build/cache/libp50localtransport.a" \
    "$top_build/cache/libprotocol50.a" "$top_build/services/libicecc.la"; do
    test -f "$required"
done

compile() {
    output=$1
    shift
    # shellcheck disable=SC2086
    "$libtool" --tag=CXX --mode=link "$cxx" "$standard" -O1 -g \
        -Wall -Wextra -Wpedantic -Wno-mismatched-new-delete \
        -fsanitize=address,undefined -fno-omit-frame-pointer -DHAVE_CONFIG_H \
        -DICECC_P50_ENDPOINT_TEST_HOOKS \
        ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
        ${ICECC_TEST_LIBZSTD_CFLAGS:-} \
        ${ICECC_TEST_XXHASH_CFLAGS:-} \
        -I"$top_build" -I"$src" -I"$src/cache" -I"$src/services" \
        "$@" "$top_build/cache/libp50localtransport.a" \
        "$top_build/cache/libprotocol50.a" \
        "$top_build/services/libicecc.la" \
        ${ICECC_TEST_LDFLAGS:-} ${ICECC_TEST_BOOST_LDFLAGS:-} \
        ${ICECC_TEST_LIBZSTD_LIBS:-} \
        ${ICECC_TEST_XXHASH_LIBS:-} ${ICECC_TEST_LIBCAP_NG_LIBS:-} \
        ${ICECC_TEST_BOOST_LIBS:-} ${ICECC_TEST_LIBS:-} \
        -fsanitize=address,undefined -pthread -o "$output" >/dev/null
}

compile "$work/p50endpoint" \
    "$src/unittests/p50_endpoint_test.cpp" \
    "$src/cache/p50_endpoint.cpp" \
    "$src/cache/p50_endpoint_run_cancel.cpp" \
    "$src/cache/p50_input_record.cpp" \
    "$src/cache/p50_adopted_outcome_writer.cpp" \
    "$src/cache/p50_adopted_socket_lease.cpp"

compile "$work/p50inputrecord" \
    "$src/unittests/p50_input_record_test.cpp" \
    "$src/cache/p50_input_record.cpp"

sanitizer_options='detect_leaks=1:halt_on_error=1:abort_on_error=1:strict_string_checks=1'
# Repeat the complete endpoint process so worker-last-owner destruction and
# execution-context teardown are exercised across multiple scheduler orders.
for iteration in 1 2 3; do
    ASAN_OPTIONS=$sanitizer_options UBSAN_OPTIONS=halt_on_error=1 \
        timeout 360s "$work/p50endpoint"
done
ASAN_OPTIONS=$sanitizer_options UBSAN_OPTIONS=halt_on_error=1 \
    timeout 120s "$work/p50inputrecord"

echo 'PASS: typed endpoint and two-phase InputRecord sanitizer gates'
