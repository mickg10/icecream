#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_BUILDDIR:-${TMPDIR:-/tmp}}
cxx=${ICECC_TEST_CXX:-c++}
out="$build/p50-adopted-socket-lease.$$"
trap 'rm -f "$out"' EXIT HUP INT TERM

"$cxx" -std=c++20 -Wall -Wextra -Werror \
    -DICECC_P50_ADOPTED_SOCKET_LEASE_TEST_HOOKS \
    -I"$src/cache" -I"$src/services" -I"$src" \
    "$src/cache/p50_adopted_socket_lease.cpp" \
    "$src/cache/p50_adopted_outcome_writer.cpp" \
    "$src/cache/p50_fd_handoff.cpp" \
    "$src/cache/p50_local_transport.cpp" \
    "$src/services/p50_cache_session_wire.cpp" \
    "$src/unittests/p50_adopted_socket_lease_test.cpp" \
    -pthread -o "$out"
"$out"
echo 'ok - retained P5CO socket lease is bounded and endpoint-only'
