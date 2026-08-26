#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?ICECC_TEST_TOP_SRCDIR is required}
top_build=${ICECC_TEST_TOP_BUILDDIR:?ICECC_TEST_TOP_BUILDDIR is required}
cxx=${ICECC_TEST_CXX:-${CXX:-c++}}
standard=${ICECC_TEST_CXX_STANDARD_FLAG:--std=c++23}
libtool="$top_build/libtool"
build=$(mktemp -d "${TMPDIR:-/tmp}/p50-session-join-mutants.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

for file in "$libtool" "$top_build/cache/libprotocol50.a" \
    "$top_build/services/libicecc.la"; do
    test -f "$file"
done

compile_mutant() {
    mutant=$1
    output=$2
    # shellcheck disable=SC2086
    "$libtool" --tag=CXX --mode=link "$cxx" "$standard" -O0 -g \
        -Wall -Wextra -Wpedantic -Werror ${ICECC_TEST_CPPFLAGS:-} \
        -I"$src" -I"$src/cache" -I"$src/services" -I"$top_build" \
        "$src/unittests/p50_cache_session_join_test.cpp" "$mutant" \
        "$src/daemon/connection_provenance.cpp" \
        "$top_build/cache/libprotocol50.a" \
        "$top_build/services/libicecc.la" \
        ${ICECC_TEST_LDFLAGS:-} ${ICECC_TEST_LIBZSTD_LIBS:-} \
        ${ICECC_TEST_XXHASH_LIBS:-} -pthread -o "$output" >/dev/null
}

compile_and_require_red() {
    name=$1
    expression=$2
    mutant="$build/$name.cpp"
    sed "$expression" "$src/cache/p50_cache_session_join.cpp" >"$mutant"
    if cmp -s "$src/cache/p50_cache_session_join.cpp" "$mutant"; then
        echo "FAIL: $name mutation did not apply" >&2
        exit 1
    fi
    if ! compile_mutant "$mutant" "$build/$name"; then
        echo "FAIL: $name did not compile; this is not a semantic red witness" >&2
        exit 1
    fi
    if "$build/$name" >"$build/$name.out" 2>&1; then
        echo "FAIL: semantic deletion mutant survived: $name" >&2
        exit 1
    fi
}

compile_and_require_red observation_registration \
    '/registered_observation_high_water_ = binding.arm_observation_id;/d'
compile_and_require_red reconcile_terminal \
    's/state == P50CacheSessionJoinState::CommittedInput ||/state == P50CacheSessionJoinState::ReconcileRequired || state == P50CacheSessionJoinState::CommittedInput ||/'
compile_and_require_red owner_equality \
    's/row->binding != claim.binding || row->owner != owner/false/'
compile_and_require_red owner_authority \
    's/owner_authority_->current(current_f_, binding, owner)/(owner_authority_->current(current_f_, binding, owner) || owner.valid())/'
compile_and_require_red f_store_generation \
    's/binding.f_store_generation == current_f_.store_generation/true/'
compile_and_require_red canonical_f_store_generation_wire \
    's/put_u64(out, armed.f_store_generation);/put_u64(out, 0);/'
compile_and_require_red duplicate_attempt \
    's/return P50CacheSessionJoinDecision::DuplicateAttempt;/return P50CacheSessionJoinDecision::Busy;/'
compile_and_require_red suppress_late_commit \
    's/row->state = row->owner_closed/row->state = false/'
compile_and_require_red attempt_capability \
    's/!attempt_authority_->consume(claim.attempt, claim.binding)/false/'
compile_and_require_red exact_owner_fence \
    's/if (owner_fenced(owner))/if (false)/g'
compile_and_require_red expiry_reclaim \
    's/erase_row(row);/row->state = P50CacheSessionJoinState::ClosedPrecommit;/'
compile_and_require_red automatic_expiry_sweep \
    's/rows_.erase(rows_.begin() + static_cast<ptrdiff_t>(i));/++i;/'
compile_and_require_red invalid_settlement \
    '/if (settlement != P50EndpointSettlement::Unresolved &&/,+3d'
compile_and_require_red invalid_claim_boundary \
    '/if (!claim.valid())/,+1d'
compile_and_require_red role_root_validation \
    's/return store_identity_guid_valid_for_role(guid.bytes, expected_role);/return guid != Id128{} \&\& (guid.bytes[kStoreIdentityRoleByte] \& kStoreIdentityRoleMask) == expected_role;/'

ICECC_TEST_TOP_SRCDIR="$src" \
    "$src/unittests/p50storeidentitywire-mutants.sh" >/dev/null

echo 'ok - authoritative cache-session join semantic mutants red'
