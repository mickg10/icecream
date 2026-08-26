#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
cxx=${CXX:-c++}
build=$(mktemp -d "${TMPDIR:-/tmp}/p50-store-wire-mutants.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
mkdir "$build/services"

# This mutation recreates the rejected bug: the fixed F role bit alone makes
# the tagged GUID appear nonzero even though all 127 root bits are zero.
sed 's/if (root_byte != 0)/if (root_byte != 0 || guid[i] != 0)/' \
    "$src/services/p50_store_identity_wire.h" \
    >"$build/services/p50_store_identity_wire.h"
if cmp -s "$src/services/p50_store_identity_wire.h" \
          "$build/services/p50_store_identity_wire.h"; then
    echo 'FAIL: zero-root mutation was not applied' >&2
    exit 1
fi

"$cxx" -std=c++20 -O0 -g -Wall -Wextra -Wpedantic -Werror \
    -I"$build" -I"$src" "$src/unittests/p50_store_identity_wire_test.cpp" \
    -o "$build/role-only-root-mutant"
if "$build/role-only-root-mutant" >"$build/mutant.out" 2>&1; then
    echo 'FAIL: role-tag-only zero-root semantic mutant survived' >&2
    exit 1
fi

# Delete pairwise same-root recognition.  The runtime test must distinguish
# valid independent roots from a forbidden role-tagged alias of one root.
sed '/inline bool store_identity_file_guid_matches_client/,/^}/ s/    return true;/    return false;/' \
    "$src/services/p50_store_identity_wire.h" \
    >"$build/services/p50_store_identity_wire.h"
if cmp -s "$src/services/p50_store_identity_wire.h" \
          "$build/services/p50_store_identity_wire.h"; then
    echo 'FAIL: same-root pair mutation was not applied' >&2
    exit 1
fi
"$cxx" -std=c++20 -O0 -g -Wall -Wextra -Wpedantic -Werror \
    -I"$build" -I"$src" "$src/unittests/p50_store_identity_wire_test.cpp" \
    -o "$build/same-root-pair-mutant"
if "$build/same-root-pair-mutant" >"$build/pair-mutant.out" 2>&1; then
    echo 'FAIL: same-root pair semantic mutant survived' >&2
    exit 1
fi

echo 'ok - StoreIdentity zero-root and same-root semantic mutants red'
