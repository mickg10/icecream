#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_BUILDDIR:-${TMPDIR:-/tmp}}
cxx=${ICECC_TEST_CXX:-c++}
out="$build/p50-adopted-outcome-writer.$$"
trap 'rm -f "$out"' EXIT HUP INT TERM

"$cxx" -std=c++20 -Wall -Wextra -Werror \
    -I"$src/cache" -I"$src/services" -I"$src" \
    "$src/cache/p50_adopted_outcome_writer.cpp" \
    "$src/services/p50_cache_session_wire.cpp" \
    "$src/unittests/p50_adopted_outcome_writer_test.cpp" \
    -o "$out"
"$out"
echo 'ok - P5CO adopted outcome reducer executable rows pass'
