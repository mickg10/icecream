#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
header="$src/unittests/support/p50_cache_session_join.h"
impl="$src/unittests/support/p50_cache_session_join.cpp"
test_file="$src/unittests/p50_cache_session_join_test.cpp"
wire_header="$src/services/p50_cache_session_wire.h"
wire_file="$src/services/p50_cache_session_wire.cpp"
unittest_makefile="$src/unittests/Makefile.am"
cache_makefile="$src/cache/Makefile.am"

for file in "$header" "$impl" "$test_file" "$wire_header" "$wire_file" \
    "$unittest_makefile" "$cache_makefile"; do
    test -f "$file"
    if grep -E '#include <(thread|future|mutex)|std::(thread|future|async|mutex)|pthread_' "$file" >/dev/null; then
        echo "FAIL: daemon join reducer acquired a thread/future primitive: $file" >&2
        exit 1
    fi
done

grep -F 'p50sessionjoin_SOURCES = p50_cache_session_join_test.cpp' \
    "$unittest_makefile" >/dev/null
grep -F '../daemon/connection_provenance.cpp' "$unittest_makefile" >/dev/null
grep -F 'support/p50_cache_session_join.cpp support/p50_cache_session_join.h' \
    "$unittest_makefile" >/dev/null
if grep -F 'p50_cache_session_join' "$cache_makefile" >/dev/null; then
    echo 'FAIL: reference join reducer returned to the product build' >&2
    exit 1
fi
grep -F 'P50_PROTOCOL.md' "$cache_makefile" >/dev/null

wire_block=$(sed -n '/struct P50CacheSessionWireClaim {/,/^};/p' "$wire_header")
if printf '%s\n' "$wire_block" | grep -E 'ConnectionLease|client_id|OwnerContext' >/dev/null; then
    echo 'FAIL: F-local owner identity leaked into the public wire claim' >&2
    exit 1
fi

reserve_block=$(sed -n '/P50CacheSessionJoinTable::reserve_claim(/,/^}/p' "$impl")
if printf '%s\n' "$reserve_block" | grep -E 'attempt_high_water|registered_observation_high_water_ *=' >/dev/null; then
    echo 'FAIL: network arrival order became global freshness authority' >&2
    exit 1
fi

terminal_block=$(sed -n '/P50CacheSessionJoinTable::terminal(/,/^}/p' "$impl")
if printf '%s\n' "$terminal_block" | grep 'ReconcileRequired' >/dev/null; then
    echo 'FAIL: unresolved reconciliation was made terminal' >&2
    exit 1
fi

for pattern in \
    'register_wait' \
    'P50SourceArmedFields' \
    'cache_session_binding_from_armed' \
    'message.valid_payload()' \
    'P50CacheSessionAttemptProof' \
    'attempt_authority_->consume' \
    'retire_unseen_owner' \
    'retirement_fence_capacity_' \
    'RetirementFenceCapacity' \
    'InvalidSettlement' \
    'source_budget_msec' \
    'P50CacheSessionOwnerAuthorityAdapter' \
    'registered_observation_high_water_' \
    'row->binding != claim.binding || row->owner != owner' \
    'revalidate_before_private_connect' \
    'revalidate_before_fd_release' \
    'mark_public_fd_detached' \
    'mark_endpoint_inflight' \
    'release_pre_detach_for_retry' \
    'P50EndpointSettlement::Unresolved' \
    'P50EndpointSettlement::CommittedInput' \
    'P50CacheSessionJoinState::CommittedSuppressed' \
    'current_f_.store_generation' \
    'current_f_.daemon_generation' \
    'owner_authority_->current(current_f_, binding, owner)' \
    'store_identity_file_guid_matches_client' \
    'put_string32_nul' \
    'take_string32_nul' \
    'reader.remaining() != 0' \
    'ready_lease_observation_id'; do
    grep -F "$pattern" "$header" "$impl" "$test_file" \
        "$src/services/comm.h" "$wire_header" "$wire_file" >/dev/null
done

grep -F 'struct P50CacheSessionArmBinding {' "$wire_header" >/dev/null
if grep -F 'struct P50CacheSessionArmBinding : P50SourceArmedFields' \
        "$wire_header" >/dev/null; then
    echo 'FAIL: cache-session binding regained projected-arm inheritance' >&2
    exit 1
fi
grep -F 'cache_session_binding_from_armed(const P50SourceArmedMsg &message)' \
    "$header" "$impl" >/dev/null
if grep -E 'cache_session_binding_from_armed\([^)]*P50SourceArmFields|independent_store_roots' \
    "$header" "$impl" >/dev/null; then
    echo 'FAIL: cache-session join retained a projected-arm conversion/helper' >&2
    exit 1
fi

grep -F 'store_identity_guid_valid_for_role' \
    "$src/services/p50_store_identity_wire.h" "$header" "$impl" >/dev/null
grep -F 'reclaim_expired_pre_detach' "$header" "$impl" >/dev/null

for witness in \
    'claim binding is extracted losslessly from the actual full ACK' \
    'canonical full-ACK claim has one stable byte representation' \
    'every truncated public-session claim is rejected' \
    'trailing bytes cannot create an alternate claim encoding' \
    'nonzero reserved full-ACK word is rejected' \
    'older live claim still reserves after reverse-order arrival' \
    'uncertain result still cannot retire' \
    'late canonical committed_input remains authoritative' \
    'WAIT registration refuses a non-current F StoreIdentity' \
    'unseen retired owner cannot register a future WAIT' \
    'owner retirement before release prevents descriptor detach' \
    'F listener replacement invalidates the old READY-bound owner' \
    'C/F role-tagged GUIDs must still have independent 127-bit roots' \
    'same connection sequence with a different client cannot alias' \
    'failed attempt remains burned'; do
    grep -F "$witness" "$test_file" >/dev/null
done

for witness in \
    'forged non-burned capability cannot reserve a live WAIT' \
    'exact unseen fence does not reject a lower still-live owner' \
    'fence capacity exhaustion fails closed' \
    'source budget mechanically expires and reclaims pre-detach row' \
    'deadline addition overflow fails closed' \
    'invalid future endpoint settlement cannot mutate reducer state' \
    'production-shaped adapter consumes registry and READY authority'; do
    grep -F "$witness" "$test_file" >/dev/null
done

for witness in \
    'role-tag-only F StoreIdentity root is rejected' \
    'role-tag-only C StoreIdentity root is rejected' \
    'StoreIdentity factories reject a zero 127-bit root' \
    'expired pre-detach WAIT is swept before capacity admission' \
    'expired pre-detach AttemptReserved WAIT is swept before admission' \
    'detached/in-flight ownership is never swept by pre-detach expiry'; do
    grep -F "$witness" "$test_file" >/dev/null
done

echo 'ok - authoritative cache-session join source shape holds'
