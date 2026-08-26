#!/bin/sh
set -eu
src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/p50sourceingress-sanitize.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"$build/libtool" --mode=link "${CXX:-g++}" -std=c++23 -Wall -Wextra -Werror \
  -fsanitize=address,undefined,leak -fno-omit-frame-pointer \
  -I"$src" -I"$build" "$src/cache/p50_source_ingress.cpp" \
  "$src/unittests/p50_source_ingress_test.cpp" "$build/cache/libprotocol50.a" \
  "$build/services/libicecc.la" -pthread -o "$tmp/p50sourceingress-sanitize"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  "$tmp/p50sourceingress-sanitize"
echo 'ok - source ingress successor ASan/UBSan/LSan gate'
