#!/bin/sh
# Deletion-sensitive StoreIdentity/legacy READY source gate.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_cache_service.cpp"
header="$src/cache/p50_cache_service.h"
identity="$src/cache/p50_incarnation_identity.h"
wire="$src/services/p50_store_identity_wire.h"
doc="$src/cache/P50_CACHE_SERVICE.md"
test_file="$src/unittests/p50cacheservice.cpp"

require() { grep -F "$2" "$1" >/dev/null; }
for pair in \
    "$identity|StoreIdentityEntropyProvider" \
    "$wire|kStoreIdentityRoleMask" \
    "$wire|kStoreIdentityClientRole" \
    "$wire|kStoreIdentityFileRole" \
    "$identity|fresh_store_identity_root_with_provider" \
    "$identity|result > 0" \
    "$identity|root = {}" \
    "$impl|read_structured_launch" \
    "$impl|present != names.size()" \
    "$impl|format != \"2\"" \
    "$impl|have_c_store_guid" \
    "$impl|have_f_store_guid" \
    "$impl|structured_launch.c_store_guid" \
    "$impl|structured_launch.f_store_guid" \
    "$impl|store_identity_root_from_f_guid" \
    "$impl|c_guid != c_store_guid_for_root(root)" \
    "$impl|guid != f_store_guid_for_root(root)" \
    "$impl|fresh_store_identity_root(legacy_root)" \
    "$impl|runtime_config.f_store_guid = structured_launch.active" \
    "$impl|ICECC_CACHE_SERVICE_EXPECTED_DERIVATION_VERSION" \
    "$header|RuntimeConfig" \
    "$test_file|legacy_store_identity_launches" \
    "$test_file|test_runtime_store_identity_is_explicit_and_role_tagged" \
    "$test_file|READY v2 generation=91 attempt=7 DERIVATION_VERSION=1" \
    "$doc|CSPRNG"; do
    file=${pair%%|*}; pattern=${pair#*|}
    require "$file" "$pattern"
done

# Structured GUIDs must not have an identity-derived fallback, and the legacy
# launch must remain a separate, fresh-root path.
if grep -E 'f_store_guid_for_incarnation|c_store_guid_for_incarnation|monotonic_msec|local::Identity.*store' \
    "$impl" "$header" "$identity" >/dev/null; then
    echo 'FAIL: identity-derived StoreIdentity path remains' >&2
    exit 1
fi

# Source deletion mutants: each authentication/entropy anchor is required.
mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50cacheservice-source.XXXXXX")
trap 'rmdir "$mutant_dir" 2>/dev/null || true' EXIT HUP INT TERM
for pattern in \
    'read_structured_launch' \
    'have_c_store_guid' \
    'have_f_store_guid' \
    'fresh_store_identity_root(legacy_root)' \
    'structured_launch.c_store_guid'; do
    mutant="$mutant_dir/mutant"
    sed "/$(printf '%s' "$pattern" | sed 's/[.[\*^$\\/]/\\&/g')/d" "$impl" >"$mutant"
    if grep -F "$pattern" "$mutant" >/dev/null; then
        echo "deletion mutant survived: $pattern" >&2
        exit 1
    fi
done

echo 'ok - cache service StoreIdentity/legacy source gates hold'
