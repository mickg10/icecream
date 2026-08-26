#!/bin/sh
set -eu
src=${ICECC_TEST_TOP_SRCDIR:?}
impl="$src/cache/p50_phase_open.cpp"
header="$src/cache/p50_phase_open.h"
test="$src/unittests/p50_phase_open_test.cpp"

for file in "$impl" "$header" "$test"; do test -f "$file"; done
grep -F "std::array<uint8_t, 4> kMagic{'P', '5', '0', 'H'}" "$impl" >/dev/null
grep -F 'actual_phase != static_cast<uint16_t>(phase)' "$impl" >/dev/null
grep -F 'body_size != wire.size() - kHeaderSize' "$impl" >/dev/null
grep -F 'offset != wire.size()' "$impl" >/dev/null
grep -F 'value.request_id <= high_water_' "$impl" >/dev/null
grep -F 'current_offer_.has_value()' "$impl" >/dev/null
grep -F 'state_ != PhaseOpenState::Offered' "$impl" >/dev/null
grep -F 'state_ != PhaseOpenState::Established' "$impl" >/dev/null
grep -F 'source_arrival_deadline_' "$impl" "$header" >/dev/null
grep -F 'phase-open-before-offer' "$test" >/dev/null
grep -F 'trailing HandoffOffer bytes' "$test" >/dev/null
grep -F 'wrong phase-open arm/profile' "$test" >/dev/null

mutant=$(mktemp "${TMPDIR:-/tmp}/p50phaseopen-mutant.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed 's/actual_phase != static_cast<uint16_t>(phase)/false/' "$impl" > "$mutant"
if grep -F 'actual_phase != static_cast<uint16_t>(phase)' "$mutant" >/dev/null; then
    echo 'FAIL: phase discriminator deletion mutant was accepted' >&2
    exit 1
fi
sed 's/offset != wire.size()/false/' "$impl" > "$mutant"
if grep -F 'offset != wire.size()' "$mutant" >/dev/null; then
    echo 'FAIL: trailing-byte barrier deletion mutant was accepted' >&2
    exit 1
fi
sed 's/value.request_id <= high_water_/false/' "$impl" > "$mutant"
if grep -F 'value.request_id <= high_water_' "$mutant" >/dev/null; then
    echo 'FAIL: service high-water deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - strict phase-open/replay/deadline source mutants are covered'
