#!/bin/sh
set -eu
src=${ICECC_TEST_TOP_SRCDIR:?}
identity="$src/cache/p50_source_identity.cpp"
header="$src/cache/p50_source_identity.h"
wait_cpp="$src/cache/p50_input_wait.cpp"
wait_header="$src/cache/p50_input_wait.h"
test="$src/unittests/p50_source_arm_wait_test.cpp"
daemon="$src/daemon/main.cpp"

for file in "$identity" "$header" "$wait_cpp" "$wait_header" "$test"; do
    test -f "$file"
done
grep -F "cache_transfer_permitted() const noexcept" "$wait_header" >/dev/null
grep -F "state_ != State::ArmSent || acknowledged != arm_" "$wait_cpp" >/dev/null
grep -F "state_ != State::WaitP50Input" "$wait_cpp" >/dev/null
grep -F "arm_input(const P50SourceArmFields& arm)" "$wait_header" >/dev/null
if grep -F "legacy_arm_projection" "$wait_cpp" "$wait_header" "$test" >/dev/null; then
    echo 'FAIL: lossy legacy arm projection remains in canonical WAIT path' >&2
    exit 1
fi
grep -F "same_complete_arm" "$wait_cpp" >/dev/null
grep -F "c_control_generation == right.c_control_generation" "$wait_cpp" >/dev/null
grep -F "c_control_attempt == right.c_control_attempt" "$wait_cpp" >/dev/null
grep -F "accept_ready(const P50SourceArmFields& arm" "$wait_header" "$wait_cpp" >/dev/null
grep -F "ready_matches_complete_arm" "$wait_cpp" >/dev/null
grep -F "!ready.matches_arm(arm_)" "$wait_cpp" >/dev/null
grep -F "sealed_fd < 0" "$wait_cpp" >/dev/null
grep -F "state_ != State::Ready || sealed_fd_ < 0" "$wait_cpp" >/dev/null
grep -F "P50InputWaitState::State::WaitP50Input" "$test" >/dev/null
grep -F "WAITP50INPUT" "$daemon" >/dev/null
grep -F "arm_p50_source" "$daemon" >/dev/null
grep -F "accept_p50_input" "$daemon" >/dev/null
grep -F "client->p50_input_wait.close()" "$daemon" >/dev/null
grep -F "status != Client::WAITP50INPUT" "$daemon" >/dev/null
grep -F "p50_input_wait.arm_input(fields)" "$daemon" >/dev/null
if grep -F "P50SourceArm projection" "$daemon" >/dev/null; then
    echo 'FAIL: Client::arm_p50_source regressed to lossy legacy projection' >&2
    exit 1
fi

mutant=$(mktemp "${TMPDIR:-/tmp}/p50sourcearm-mutant.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed 's/state_ != State::ArmSent || acknowledged != arm_/state_ != State::ArmSent/' "$wait_cpp" > "$mutant"
if grep -F "state_ != State::ArmSent || acknowledged != arm_" "$mutant" >/dev/null; then
    echo 'FAIL: arm-ACK deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - source-arm ACK and WAITP50INPUT deletion contract holds'
