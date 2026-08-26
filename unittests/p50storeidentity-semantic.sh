#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?}
cxx=${ICECC_TEST_CXX:-c++}
tmp=$(mktemp -d "${TMPDIR:-/tmp}/p50storeidentity-semantic.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

cp "$src/services/p50_store_identity_wire.h" "$tmp/p50_store_identity_wire.h"
"$cxx" -std=c++23 ${ICECC_TEST_CXXFLAGS:-} \
    -I"$tmp" -I"$src/services" \
    "$src/unittests/p50_store_identity_validator_semantic_test.cpp" \
    -o "$tmp/original"
"$tmp/original"

# Delete the production root-nonzero decision, rebuild, and require the
# executable behavior to expose the invalid role-tag-only identity.
sed -i 's/return false; \/\/ P50_ROOT_NONZERO_CHECK/return true; \/\/ semantic deletion mutant/' \
    "$tmp/p50_store_identity_wire.h"
"$cxx" -std=c++23 ${ICECC_TEST_CXXFLAGS:-} \
    -I"$tmp" -I"$src/services" \
    "$src/unittests/p50_store_identity_validator_semantic_test.cpp" \
    -o "$tmp/mutant"
if "$tmp/mutant"; then
    echo 'FAIL: deleted root-nonzero production validator survived' >&2
    exit 1
fi
echo 'ok - StoreIdentity root validator semantic deletion is detected'
