#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_BUILDDIR:-${TMPDIR:-/tmp}}
cxx=${ICECC_TEST_CXX:-c++}
mkdir -p "$build"
sanitize_dir=$(mktemp -d "$build/p50-adopted-writer-sanitize.XXXXXX")
trap 'rm -rf "$sanitize_dir"' EXIT HUP INT TERM

"$cxx" -std=c++20 -Wall -Wextra -Werror -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined \
    -I"$src/cache" -I"$src/services" -I"$src" \
    "$src/cache/p50_adopted_outcome_writer.cpp" \
    "$src/services/p50_cache_session_wire.cpp" \
    "$src/unittests/p50_adopted_outcome_writer_test.cpp" \
    -o "$sanitize_dir/p50-adopted-outcome-writer"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    "$sanitize_dir/p50-adopted-outcome-writer"
echo 'ok - P5CO adopted outcome reducer ASan/UBSan executable rows pass'
