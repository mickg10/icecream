#!/bin/sh
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
comm_h="$src/services/comm.h"
comm_cpp="$src/services/comm.cpp"
wire_h="$src/services/p50_cache_session_wire.h"
wire_cpp="$src/services/p50_cache_session_wire.cpp"
test_cpp="$src/unittests/p50_cache_session_wire_test.cpp"
services_makefile="$src/services/Makefile.am"
tests_makefile="$src/unittests/Makefile.am"

for file in "$comm_h" "$comm_cpp" "$wire_h" "$wire_cpp" "$test_cpp" \
    "$services_makefile" "$tests_makefile"; do
    test -f "$file"
done

# The public codec is a services dependency.  Moving it upward into cache or
# letting services include cache would create a circular authority boundary.
grep -F 'libicecc_la_SOURCES = job.cpp comm.cpp p50_cache_session_wire.cpp' \
    "$services_makefile" >/dev/null
grep -F 'p50_cache_session_wire.h' "$services_makefile" >/dev/null
if grep -E '#include[[:space:]]+[<"](\.\./)?cache/' \
    "$comm_h" "$comm_cpp" "$wire_h" "$wire_cpp" >/dev/null; then
    echo 'FAIL: services-owned claim/outcome foundation depends upward on cache' >&2
    exit 1
fi

for pattern in \
    'P50_CACHE_SESSION_CLAIM = 0x50f00012' \
    'P50_CACHE_SESSION_OUTCOME = 0x50f00013' \
    'inline constexpr std::array<uint32_t, 6> values' \
    'Msg::P50_CACHE_SESSION_CLAIM' \
    'Msg::P50_CACHE_SESSION_OUTCOME' \
    'static_assert(unique(), "Protocol-50 private ordinary message collision")' \
    'struct ClaimAttemptCapability128' \
    'std::array<uint8_t, 16> bytes{}' \
    'fresh_claim_attempt_capabilities_with_provider' \
    'fresh_claim_attempt_capabilities' \
    'P50ServerClaimReleaseTicket' \
    'P50ClientAdoptedReleaseTicket' \
    'send_p50_cache_session_outcome' \
    'take_p50_decoded_claim_stamp' \
    'take_p50_decoded_outcome_stamp' \
    'release_fd_after_p50_server_claim' \
    'release_fd_after_p50_client_adopted'; do
    grep -F "$pattern" "$comm_h" "$comm_cpp" >/dev/null
done

grep -F '::getrandom(buffer, size, flags)' "$comm_cpp" >/dev/null
grep -F 'result == static_cast<ssize_t>(capability.bytes.size())' \
    "$comm_cpp" >/dev/null
grep -F 'capability_1 != capability_2' "$comm_cpp" >/dev/null

binding_block=$(sed -n '/struct P50CacheSessionArmBinding {/,/^};/p' "$wire_h")
attempt_block=$(sed -n '/struct P50CacheSessionAttemptProof {/,/^};/p' "$wire_h")
test -n "$binding_block"
test -n "$attempt_block"
if printf '%s\n' "$binding_block" | \
    grep -E ': P50SourceArmedFields|attempt_capability|capability_[12]|authority_nonce' \
        >/dev/null; then
    echo 'FAIL: P5CL binding inherited or exposed unused capability authority' >&2
    exit 1
fi
printf '%s\n' "$attempt_block" | grep -F 'uint8_t ordinal = 0' >/dev/null
printf '%s\n' "$attempt_block" | \
    grep -F 'ClaimAttemptCapability128 selected_capability{}' >/dev/null
if printf '%s\n' "$attempt_block" | \
    grep -E 'authority_nonce|arm_observation_id|capability_[12]|c_control|f_control' \
        >/dev/null; then
    echo 'FAIL: P5CL attempt proof acquired redundant grant identity' >&2
    exit 1
fi

for pattern in \
    'kP50CacheSessionWireVersion = 1' \
    'kP50CacheSessionClaimKind = 1' \
    'kP50CacheSessionClaimMaxWireBytes = 1024' \
    'kP50CacheSessionOutcomeMaxWireBytes = 1152' \
    'Adopted = 1' \
    'RefusedPreDetach = 2' \
    'P50SessionOperationRole::FSession' \
    'decode_cache_session_wire_claim' \
    'encode_cache_session_wire_claim' \
    'decode_cache_session_outcome' \
    'encode_cache_session_outcome'; do
    grep -F "$pattern" "$wire_h" "$wire_cpp" >/dev/null
done
grep -F "kClaimMagic{'P', '5', 'C', 'L'}" "$wire_cpp" >/dev/null
grep -F "kOutcomeMagic{'P', '5', 'C', 'O'}" "$wire_cpp" >/dev/null
grep -F 'reader.take_zeroes(7)' "$wire_cpp" >/dev/null
grep -F 'reader.remaining() != 0' "$wire_cpp" >/dev/null
grep -F '!std::equal(canonical.begin(), canonical.end(), wire.begin())' \
    "$wire_cpp" >/dev/null
grep -F 'outcome->canonical_claim == ticket.canonical_claim_' \
    "$comm_cpp" >/dev/null
grep -F 'outcome->canonical_claim != p50_outbound_claim' \
    "$comm_cpp" >/dev/null
grep -F 'f_sidecar_launch == claim->binding.f_control_identity()' \
    "$wire_cpp" >/dev/null
grep -F 'f_store_guid == claim->binding.f_store_guid' "$wire_cpp" >/dev/null
grep -F 'f_sidecar_launch == P50WireLaunchIdentity{}' "$wire_cpp" >/dev/null
grep -F 'operation == P50FSessionOperationId{}' "$wire_cpp" >/dev/null

for pattern in \
    'ticket.outcome_frame_sequence_ == 0' \
    'framesFlushed() != framesQueued()' \
    'ticket.outcome_frame_sequence_ != 0' \
    'ticket.outcome_frame_sequence_ == framesFlushed()' \
    'MSG_PEEK | MSG_DONTWAIT' \
    'client_claim_pending' \
    'server_claim_pending' \
    'client_ticket_pending' \
    'set_error(true)'; do
    grep -F "$pattern" "$comm_cpp" >/dev/null
done

# Capabilities are opaque bearer material, never diagnostics.  Reject obvious
# rendering surfaces in production foundation code.
if grep -E '(log_error|log_warning|trace\(\)|dump\(|to_string).*(capability|canonical_claim)' \
    "$comm_cpp" "$wire_cpp" >/dev/null; then
    echo 'FAIL: claim capability or canonical claim reached a diagnostic sink' >&2
    exit 1
fi

for witness in \
    'OS-provider seam creates two distinct typed 128-bit capabilities' \
    'zero, repeated-equal, and short entropy fail closed and clear' \
    'one P5CL exposes only its selected 128-bit capability' \
    'selected capability bytes are semantic canonical claim material' \
    'every truncated P5CL prefix rejects' \
    'oversized P5CL rejects before allocation-driven parsing' \
    'embedded NUL in bounded P5CL host rejects' \
    'every truncated P5CO prefix rejects' \
    'oversized P5CO rejects before embedded claim parsing' \
    'out-of-range refusal reason cannot encode' \
    'ADOPTED launch must equal the launch bound by echoed P5CL' \
    'ADOPTED F StoreIdentity must equal the GUID bound by echoed P5CL' \
    'REFUSED rejects a partially populated invalid launch identity' \
    'REFUSED rejects a partially populated invalid operation identity' \
    'server reservation ticket cannot release before P5CO flush' \
    'server barrier rejects a different canonical P5CL echo' \
    'exact ADOPTED P5CO barrier fully flushes before server release' \
    'kernel-queued byte refuses server release without consuming it' \
    'queued P5CL refuses a second ordinary frame' \
    'flushed P5CL awaiting P5CO refuses a second ordinary frame' \
    'different but canonical P5CL echo cannot release the client fd' \
    'wrong F-session operation sequence refuses client release' \
    'kernel-queued byte refuses client ADOPTED release' \
    'empty CACHE_SESSION creates zero positive claim/outcome stamp' \
    'unissued P5CO cannot send on a clean Protocol-50 channel' \
    'claim message is valid for exactly Protocol 50' \
    'outcome message is valid for exactly Protocol 50'; do
    grep -F "$witness" "$test_cpp" >/dev/null
done

for script in p50cachesessionwire-source.sh p50cachesessionwire-mutants.sh \
    p50cachesessionwire-sanitize.sh; do
    grep -F "$script" "$tests_makefile" >/dev/null
done

echo 'ok - P50 claim/outcome foundation source shape holds'
