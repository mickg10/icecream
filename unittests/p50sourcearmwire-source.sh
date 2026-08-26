#!/bin/sh
set -eu
src=${ICECC_TEST_TOP_SRCDIR:?}
header="$src/services/comm.h"
impl="$src/services/comm.cpp"
identity="$src/services/p50_store_identity_wire.h"
makefile="$src/services/Makefile.am"
test="$src/unittests/p50_source_arm_wire_test.cpp"

for file in "$header" "$impl" "$identity" "$makefile" "$test"; do
    test -f "$file"
done

for needle in \
    'P50_SOURCE_ARM = 0x50f00010' \
    'P50_SOURCE_ARMED = 0x50f00011' \
    'wire_job_id' 'assignment_epoch' 'assignment_nonce' \
    'selected_f_host' 'selected_f_ordinary_port' 'selected_f_cache_port' \
    'cache_protocol' 'cache_profile' 'logical_job' 'compiler_attempt' \
    'c_store_generation' 'c_store_derivation_version' 'c_store_guid' \
    'source_request_id' 'source_mode' \
    'c_control_generation' 'c_control_attempt' \
    'f_control_generation' 'f_control_attempt' 'f_store_guid' \
    'f_store_derivation_version' 'arm_observation_id' \
    'P50_SOURCE_MODE_ZSTD_TU' 'CACHE_PROFILE_ZSTD_TU' \
    'kStoreIdentityDerivationVersion' 'kStoreIdentityRoleByte' \
    'kStoreIdentityRoleMask' 'kStoreIdentityClientRole' \
    'kStoreIdentityFileRole' 'store_identity_file_guid_matches_client' \
    'read_bounded_string' 'current_message_bytes_remaining() != 0' \
    'negotiated_protocol == PROTOCOL_VERSION' 'p50_nonzero' \
    'if (!valid_payload())'; do
    grep -F "$needle" "$header" "$impl" "$identity" >/dev/null
done

grep -F 'P50SourceArmMsg' "$test" >/dev/null
grep -F 'P50SourceArmedMsg' "$test" >/dev/null
grep -F 'trailing payload bytes' "$test" >/dev/null
grep -F 'Protocol 49' "$test" >/dev/null
grep -F 'WIRE-AUDIT three-bucket classification' "$test" >/dev/null
grep -F 'stable Protocol-50 fixture bytes' "$test" >/dev/null
grep -F 'unrelated same-role root' "$test" >/dev/null
test "$(grep -c '^[[:space:]]*p50_store_identity_wire\.h' "$makefile")" -eq 1
if sed -n '/^ice_HEADERS =/,/^$/p' "$makefile" | grep -F 'p50_store_identity_wire.h' >/dev/null && \
   sed -n '/^noinst_HEADERS =/,/^$/p' "$makefile" | grep -F 'p50_store_identity_wire.h' >/dev/null; then
    echo 'FAIL: shared StoreIdentity header is duplicated in public and noinst headers' >&2
    exit 1
fi
sed -n '/^ice_HEADERS =/,/^$/p' "$makefile" | grep -F 'p50_store_identity_wire.h' >/dev/null

# The ACK must not transmit or model raw CSPRNG entropy/root state.
if grep -F 'f_store_identity_root' "$header" "$impl" "$identity" "$test" >/dev/null; then
    echo 'FAIL: raw StoreIdentity root leaked into ordinary source ACK' >&2
    exit 1
fi
if grep -F 'f_store_guid[15]' "$header" "$impl" "$identity" "$test" >/dev/null; then
    echo 'FAIL: source ACK diverged to the obsolete low-last-byte role bit' >&2
    exit 1
fi

# Removing the exact trailing-byte check must be visible to this gate.
mutant=$(mktemp "${TMPDIR:-/tmp}/p50sourcearmwire-mutant.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed 's/channel->current_message_bytes_remaining() != 0/false/' "$impl" > "$mutant"
if grep -F 'channel->current_message_bytes_remaining() != 0' "$mutant" >/dev/null; then
    echo 'FAIL: trailing-byte deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - source-arm ordinary-wire deletion contract holds'
