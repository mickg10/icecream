#!/bin/sh
# Deletion-sensitive source/distribution gate for the pure READY projection.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_ready_advertisement.cpp"
header="$src/cache/p50_ready_advertisement.h"
test="$src/unittests/p50_ready_advertisement_test.cpp"

require() {
    pattern=$1
    file=$2
    label=$3
    if ! grep -F "$pattern" "$file" >/dev/null; then
        echo "FAIL: $label" >&2
        exit 1
    fi
    echo "ok - $label"
}

require 'observation.supervisor_state == sidecar::State::Ready' "$impl" \
    'presence is gated on exact supervisor READY'
require 'observation.current_lease_matches' "$impl" \
    'presence is gated on the current immutable READY lease'
if grep -F '&& observation.private_relationship_authenticated' "$impl" >/dev/null; then
    echo 'FAIL: transient per-TU authentication became advertisement authority' >&2
    exit 1
fi
echo 'ok - transient per-TU authentication is not advertisement authority'
require 'crashed && current_.present()' "$impl" \
    'a post-READY crash forces withdrawal before recovery'
require 'CACHE_WIRE_PROTOCOL_V1, CACHE_PROFILE_ZSTD_TU' "$impl" \
    'presence projects only the exact runnable CacheWire profile'
require 'Error::CounterRegression' "$impl" \
    'counter rollback fails closed'
require 'Error::CounterSaturated' "$impl" \
    'saturating supervisor counters fail closed before ambiguity'
require 'cumulative_post_ready_exits' "$header" \
    'adapter contract preserves the exit edge across supervisor recreation'
require 'std::array<Snapshot, 2>' "$header" \
    'one observation has a statically bounded transition batch'
require 'compressed crash and recovery withdraws before republishing' "$test" \
    'behavioral suite covers the two-transition crash edge'
require 'READY current lease advertises while no transient TU relationship exists' "$test" \
    'behavioral suite covers idle current-lease advertisement'
require 'same-counter READY recovery at saturation remains failed closed' "$test" \
    'behavioral suite covers the saturated same-counter crash ambiguity'

require 'libp50readyadvertisement.a' "$src/cache/Makefile.am" \
    'controller library is registered in the cache build'
require 'P50_READY_ADVERTISEMENT.md' "$src/cache/Makefile.am" \
    'policy contract is distributed'
require 'p50readyadvertisement-source.sh' "$src/unittests/Makefile.am" \
    'source gate is registered and distributed'
require 'p50readyadvertisement-mutants.sh' "$src/unittests/Makefile.am" \
    'behavioral mutant gate is registered and distributed'

# Mechanism-only checkpoint: daemon Login must remain canonical absence until
# the supervised private relationship and transition application are wired.
count=$(grep -F -c 'apply_inert_cache_advertisement' "$src/daemon/main.cpp" || true)
if [ "$count" -ne 3 ]; then
    echo "FAIL: READY mechanism checkpoint changed inert daemon Login ($count anchors)" >&2
    exit 1
fi
echo 'ok - daemon Login remains inert at the mechanism-only checkpoint'

echo 'PASS: pure READY advertisement source gates hold'
