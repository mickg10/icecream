#!/bin/sh
set -eu
root="${ICECC_TEST_TOP_SRCDIR:-$(pwd)/..}"
build="${ICECC_TEST_BUILDDIR:-$(pwd)}"
cxx="${ICECC_TEST_CXX:-c++}"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/p50-sidecar-lifecycle-san.XXXXXX")"
trap 'rm -r -- "$tmp"' EXIT HUP INT TERM
"$cxx" -std=c++20 -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
    -I"$root" -I"$root/cache" -I"$root/services" \
    "$root/unittests/p50_sidecar_lifecycle_test.cpp" \
    "$root/cache/p50_sidecar_lifecycle.cpp" \
    "$build/../cache/libp50sidecar.a" "$build/../cache/libp50localtransport.a" \
    "$build/../services/.libs/libicecc.a" \
    -L"${P50_R2_DEPS:-/tanksmall/MICKG2/mickg/src/mickg10/icecream-worktrees/.p50-r2-deps/root/usr/lib/x86_64-linux-gnu}" \
    -lxxhash ${ICECC_TEST_LIBS:-} -o "$tmp/test" >/dev/null 2>&1 || {
        echo 'sanitizer compile unavailable; held' >&2
        exit 77
    }
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/test" >/dev/null
