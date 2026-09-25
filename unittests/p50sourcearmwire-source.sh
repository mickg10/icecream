#!/bin/sh
set -eu
src=${ICECC_TEST_TOP_SRCDIR:?}
header="$src/services/comm.h"
impl="$src/services/comm.cpp"
identity="$src/services/p50_store_identity_wire.h"
makefile="$src/services/Makefile.am"
test="$src/unittests/p50_source_arm_wire_test.cpp"
lease_test="$src/unittests/p50_cache_fd_seam_test.cpp"

for file in "$header" "$impl" "$identity" "$makefile" "$test" "$lease_test"; do
    test -f "$file"
done

for needle in \
    '#define PROTOCOL_VERSION 51' \
    '#define PROTOCOL_VERSION_CACHE_R2_NEGOTIATION 51' \
    'inline constexpr uint32_t CACHE_WIRE_REVISION_R1 = 1;' \
    '#define PROTOCOL_VERSION_P50_SOURCE_ARM_R1 50' \
    '#define PROTOCOL_VERSION_P50_CACHE_SESSION_R1 50' \
    'P50_CACHE_FD_LEASE_V3_VERSION = 3' \
    'P50_CACHE_FD_LEASE_V3_BYTES = 64' \
    'P51_CACHE_FD_LEASE_V4_VERSION = 4' \
    'P51_CACHE_FD_LEASE_V4_BYTES = 96' \
    'P50_SOURCE_ARM = 0x50f00010' \
    'P50_SOURCE_ARMED = 0x50f00011' \
    'P51_SOURCE_LEASE_REQUEST = 0x51f00000' \
    'P51_SOURCE_ARM = 0x51f00010' \
    'P51_SOURCE_ARMED = 0x51f00011' \
    'wire_job_id' 'assignment_epoch' 'assignment_nonce' \
    'selected_f_host' 'selected_f_ordinary_port' 'selected_f_cache_port' \
    'cache_protocol' 'cache_profile' 'logical_job' 'compiler_attempt' \
    'c_store_generation' 'c_store_derivation_version' 'c_store_guid' \
    'source_request_id' 'source_mode' \
    'c_control_generation' 'c_control_attempt' \
    'P50SourceArmedFields' 'semantic_valid()' \
    'f_control_generation' 'f_control_attempt' 'f_store_generation' \
    'f_store_guid' 'f_store_derivation_version' 'arm_observation_id' \
    'ClaimAttemptCapability128' 'attempt_capability_1' \
    'attempt_capability_2' \
    'source_budget_msec' 'MaxSourceBudgetMsec' \
    'P50_SOURCE_MODE_ZSTD_TU' 'P50_SOURCE_MODE_ZSTD_ROUTE' \
    'CACHE_PROFILE_ZSTD_TU' 'CACHE_PROFILE_ZSTD_ROUTE' \
    'kStoreIdentityDerivationVersion' 'kStoreIdentityRoleByte' \
    'kStoreIdentityRoleMask' 'kStoreIdentityClientRole' \
    'kStoreIdentityFileRole' 'store_identity_guid_valid_for_role' \
    'store_identity_file_guid_matches_client' \
    'read_bounded_string' 'current_message_bytes_remaining() != 0' \
    'protocol_supports_p50_r1_bridge(negotiated_protocol)' \
    'if (!valid_payload())'; do
    grep -F "$needle" "$header" "$impl" "$identity" >/dev/null
done

grep -F 'P50SourceArmMsg' "$test" >/dev/null
grep -F 'P50SourceArmedMsg' "$test" >/dev/null
grep -F 'trailing payload bytes' "$test" >/dev/null
grep -F 'truncated capability tail' "$test" >/dev/null
grep -F 'zero attempt capability is refused before framing' "$test" >/dev/null
grep -F 'equal attempt capabilities are refused before framing' "$test" >/dev/null
grep -F 'Protocol 49' "$test" >/dev/null
grep -F 'protocol 51' "$test" >/dev/null
grep -F 'protocol-51 decoder accepts the explicitly selected R1 SOURCE_ARM' "$test" >/dev/null
grep -F 'P51 lease request is the exact 32-byte revision/window fixture' "$test" >/dev/null
grep -F 'P51 ARM is a distinct exact frame' "$test" >/dev/null
grep -F 'P51 ARMED exact echo includes reservation' "$test" >/dev/null
grep -F 'legacy_v3_lease_fixture_is_byte_exact' "$lease_test" >/dev/null
grep -F 'v4_lease_fixture_and_scm_rights_roundtrip' "$lease_test" >/dev/null
grep -F 'v4_lease_receiver_binds_full_request_and_exact_shape' "$lease_test" >/dev/null
grep -F 'WIRE-AUDIT three-bucket classification' "$test" >/dev/null
grep -F 'stable Protocol-50 fixture bytes' "$test" >/dev/null
grep -F 'independent F sidecar StoreIdentity root is accepted' "$test" >/dev/null
grep -F 'malformed source-arm preserves only its exact assignment triple' "$test" >/dev/null
grep -F 'malformed source-arm assignment triple is one-shot' "$test" >/dev/null
grep -F 'take_invalid_p50_source_arm_identity' "$header" "$impl" >/dev/null
test "$(grep -c '^[[:space:]]*p50_store_identity_wire\.h' "$makefile")" -eq 1
if sed -n '/^ice_HEADERS =/,/^$/p' "$makefile" | grep -F 'p50_store_identity_wire.h' >/dev/null && \
   sed -n '/^noinst_HEADERS =/,/^$/p' "$makefile" | grep -F 'p50_store_identity_wire.h' >/dev/null; then
    echo 'FAIL: shared StoreIdentity header is duplicated in public and noinst headers' >&2
    exit 1
fi
sed -n '/^ice_HEADERS =/,/^$/p' "$makefile" | grep -F 'p50_store_identity_wire.h' >/dev/null

# C and F operations run under independent supervised sidecar incarnations,
# but a role-tagged alias of the same 127-bit root is forbidden.  The complete
# canonical ACK validator must enforce both individual GUID validity and this
# pairwise non-alias law.
grep -F 'store_identity_guid_valid_for_role' "$header" >/dev/null
grep -F '!icecc::p50::store_identity_file_guid_matches_client' "$header" >/dev/null

# Keep each R1 source-arm message class bound to the bridge predicate; a call
# elsewhere in comm.h must not mask a deleted guard on either actual message.
check_source_arm_protocol_guards() {
    candidate_header=$1
    arm_msg=$(sed -n '/^class P50SourceArmMsg/,/^};/p' "$candidate_header")
    armed_msg=$(sed -n '/^class P50SourceArmedMsg/,/^};/p' "$candidate_header")
    bridge=$(sed -n \
        '/^inline constexpr bool protocol_supports_p50_r1_bridge/,/^}/p' \
        "$candidate_header")
    test -n "$arm_msg" && test -n "$armed_msg" && test -n "$bridge" || return 1
    printf '%s\n' "$arm_msg" | \
        grep -F 'protocol_supports_p50_r1_bridge(negotiated_protocol)' \
        >/dev/null || return 1
    printf '%s\n' "$armed_msg" | \
        grep -F 'protocol_supports_p50_r1_bridge(negotiated_protocol)' \
        >/dev/null || return 1
    printf '%s\n' "$bridge" | \
        grep -F 'protocol >= PROTOCOL_VERSION_P50_CACHE_SESSION_R1' \
        >/dev/null || return 1
    printf '%s\n' "$bridge" | \
        grep -F 'protocol <= PROTOCOL_VERSION_SUPPORTED_MAX' >/dev/null
}
check_source_arm_protocol_guards "$header"

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
identity_mutant=$(mktemp "${TMPDIR:-/tmp}/p50sourcearmwire-identity-mutant.XXXXXX")
protocol_mutant=$(mktemp "${TMPDIR:-/tmp}/p50sourcearmwire-protocol-mutant.XXXXXX")
armed_protocol_mutant=$(mktemp "${TMPDIR:-/tmp}/p50sourcearmwire-armed-protocol-mutant.XXXXXX")
trap 'rm -f "$mutant" "$identity_mutant" "$protocol_mutant" "$armed_protocol_mutant"' EXIT HUP INT TERM

sed '/^class P50SourceArmMsg/,/^};/s/protocol_supports_p50_r1_bridge(negotiated_protocol)/true/' \
    "$header" >"$protocol_mutant"
if check_source_arm_protocol_guards "$protocol_mutant"; then
    echo 'FAIL: R1 bridge protocol-guard deletion mutant survived' >&2
    exit 1
fi
sed '/^class P50SourceArmedMsg/,/^};/s/protocol_supports_p50_r1_bridge(negotiated_protocol)/true/' \
    "$header" >"$armed_protocol_mutant"
if check_source_arm_protocol_guards "$armed_protocol_mutant"; then
    echo 'FAIL: R1 ARMED bridge protocol-guard deletion mutant survived' >&2
    exit 1
fi
sed 's/channel->current_message_bytes_remaining() != 0/false/' "$impl" > "$mutant"
if grep -F 'channel->current_message_bytes_remaining() != 0' "$mutant" >/dev/null; then
    echo 'FAIL: trailing-byte deletion mutant was accepted' >&2
    exit 1
fi

sed 's/type == Msg::P50_SOURCE_ARM/false/' "$impl" > "$identity_mutant"
if grep -F 'type == Msg::P50_SOURCE_ARM' "$identity_mutant" >/dev/null; then
    echo 'FAIL: malformed-arm assignment capture deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - source-arm ordinary-wire deletion contract holds'
