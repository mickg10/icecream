#!/bin/sh
# Standalone ASan/UBSan/LSan gate for the transport/reducer slice.
set -eu
root=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=$(mktemp -d "${TMPDIR:-/tmp}/icecc-p50-reverse-fd.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
cppflags=${ICECC_TEST_CPPFLAGS:-}
ldflags=${ICECC_TEST_LDFLAGS:-}
"$cxx" "$standard" -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined,leak -Wall -Wextra -Wpedantic -Werror \
    $cppflags -I"$root" -I"$root/cache" -I"$root/services" \
    "$root/unittests/p50_reverse_fd_retry_test.cpp" \
    "$root/cache/p50_reverse_fd_retry.cpp" \
    "$root/cache/p50_fd_handoff.cpp" "$root/cache/p50_local_transport.cpp" \
    "$root/cache/p50_source_identity.cpp" $ldflags -pthread -o "$build/test"
ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1} \
UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1} "$build/test"
echo 'PASS: reverse sealed-FD ASan/UBSan/LSan gate passed'
