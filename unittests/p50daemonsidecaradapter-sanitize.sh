#!/bin/sh
set -eu

cxx=${ICECC_TEST_CXX:-c++}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++20}
top_src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
service=${ICECC_TEST_CACHE_SERVICE:?ICECC_TEST_CACHE_SERVICE is required}
test -x "$service" || {
    echo "FAIL: executable cache service is required for sanitizer runtime" >&2
    exit 1
}
out=${TMPDIR:-/tmp}/p50-daemon-sidecar-adapter-sanitize.$$
trap 'rm -f "$out"' EXIT HUP INT TERM

"$cxx" "$standard" -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined,leak -fno-omit-frame-pointer \
    -DHAVE_CONFIG_H -DICECC_P50_DAEMON_SIDECAR_ADAPTER_TEST_HOOKS \
    -I"$top_build" -I"$top_src" -I"$top_src/client" -I"$top_src/services" \
    ${ICECC_TEST_CPPFLAGS:-} ${ICECC_TEST_BOOST_CPPFLAGS:-} \
    "$top_src/unittests/p50_daemon_sidecar_adapter_test.cpp" \
    "$top_src/cache/p50_daemon_sidecar_adapter.cpp" \
    "$top_src/cache/p50_daemon_cache_dispatch.cpp" \
    "$top_src/cache/p50_input_fd_attachment.cpp" \
    "$top_src/cache/p50_input_lifecycle.cpp" \
    "$top_src/cache/p50_daemon_control.cpp" \
    "$top_src/cache/p50_fd_handoff.cpp" \
    "$top_src/cache/p50_local_transport.cpp" \
    "$top_src/cache/p50_sidecar_lifecycle.cpp" \
    "$top_src/cache/p50_sidecar_supervisor.cpp" \
    "$top_src/cache/p50_ready_advertisement.cpp" \
    "$top_build/cache/libprotocol50.a" \
    "$top_build/services/.libs/libicecc.a" -llzo2 \
    ${ICECC_TEST_LIBZSTD_LIBS:--lzstd} \
    ${ICECC_TEST_XXHASH_LIBS:--lxxhash} ${ICECC_TEST_LDFLAGS:-} \
    -o "$out"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
ICECC_TEST_CACHE_SERVICE="$service" "$out"
echo 'p50 daemon sidecar adapter sanitizer runtime: ok'
