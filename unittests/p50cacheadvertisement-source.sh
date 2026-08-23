#!/bin/sh
# Deletion-sensitive scope gate for the inert Login-only advertisement.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

require_count() {
    expected=$1
    pattern=$2
    file=$3
    label=$4
    actual=$(grep -F -c "$pattern" "$src/$file" || true)
    if [ "$actual" -ne "$expected" ]; then
        echo "FAIL: $label (expected $expected anchor(s), found $actual)" >&2
        exit 1
    fi
    echo "ok - $label"
}

require_absent() {
    pattern=$1
    shift
    label=$1
    shift
    if grep -E -n "$pattern" "$@" >/dev/null 2>&1; then
        echo "FAIL: $label" >&2
        grep -E -n "$pattern" "$@" >&2 || true
        exit 1
    fi
    echo "ok - $label"
}

require_count 4 'IS_PROTOCOL_VERSION(PROTOCOL_VERSION_CACHE_ADVERTISEMENT, c)' \
    services/comm.cpp \
    'Login and UseCS codecs (S2 handoff) each gate both read and write at protocol 50'
require_count 1 'current_message_end = intogo_old + inmsglen' \
    services/comm.cpp 'decoder records the current frame boundary'
require_count 1 'if (c->current_message_bytes_remaining() < 3 * sizeof(uint32_t)) {' \
    services/comm.cpp 'Login refuses a shortened three-word tail'
# UseCS's decode is a tri-state, not Login's binary short-tail check (BigOracle
# steer: protocol 50 already carries an earlier feature's four-word identity
# tail, so a peer built before this cache-handoff tail existed sends nothing
# more at protocol 50 -- exactly zero remaining bytes -- which must decode as
# canonical absence, not be refused as short).
require_count 1 'const size_t remaining = c->current_message_bytes_remaining();' \
    services/comm.cpp 'UseCS decode captures the remaining-bytes tri-state input'
require_count 1 'if (remaining == 0) {' services/comm.cpp \
    'UseCS decode treats a frozen pre-handoff (zero remaining bytes) frame as absence'
require_count 1 '} else if (remaining < 3 * sizeof(uint32_t)) {' services/comm.cpp \
    'UseCS decode refuses only a genuinely short (1..11 byte) tail'
require_count 1 'const bool absent = cache_endpoint_port == 0' \
    services/comm.cpp 'Login payload has a canonical whole-absence branch'
require_count 1 '&& (cache_profile_mask & ~CACHE_ADVERTISABLE_PROFILE_MASK) == 0' \
    services/comm.cpp 'Login rejects every non-runnable or unknown profile bit'
require_count 3 'apply_inert_cache_advertisement' daemon/main.cpp \
    'real daemon applies canonical absence at definition, login, and reannouncement'
require_count 2 'cs->setCacheAdvertisement(m->cache_endpoint_port, m->cache_protocol,' \
    scheduler/scheduler.cpp 'scheduler retains initial and replacement Login snapshots'
require_count 2 'it->cacheEndpointPort()' scheduler/scheduler.cpp \
    'scheduler reads endpoint port only for listcs visibility'
require_count 1 'it->cacheProtocol()' scheduler/scheduler.cpp \
    'scheduler reads cache protocol only for listcs visibility'
require_count 1 'it->cacheProfileMask()' scheduler/scheduler.cpp \
    'scheduler reads cache profiles only for listcs visibility'
require_count 1 'Z3_LONG = 4' cache/protocol50.h \
    'z3_long has one stable protocol profile ID'
require_count 1 'Z3_SHARED_LONG = 5' cache/protocol50.h \
    'z3_shared_long has one stable protocol profile ID'

# The server-selection half of scheduler.cpp ends at handle_login.  New cache
# metadata must not become eligibility, scoring, or assignment input.  S2's
# post-selection UseCS cache-handoff fill (project_cache_handoff) is defined
# right after handle_login, so this boundary also proves that fill is
# textually outside the selection/scoring code: send_remote_dispatch_reply
# (inside the slice) only ever CALLS project_cache_handoff by name -- the
# getters themselves are read nowhere before the cutoff.
selection_slice=$(sed -n '1,2841p' "$src/scheduler/scheduler.cpp")
if printf '%s\n' "$selection_slice" \
        | grep -E 'cacheEndpointPort|cacheProtocol\(|cacheProfileMask' >/dev/null; then
    echo 'FAIL: cache advertisement leaked into scheduler selection' >&2
    exit 1
fi
echo 'ok - cache advertisement is absent from scheduler selection'

# S2: the UseCS tail encode/decode are gated at PROTOCOL_VERSION_CACHE_ADVERTISEMENT,
# exactly like LoginMsg's, but scoped to UseCSMsg's own methods so this cannot
# be satisfied by LoginMsg's separate occurrences of the same gate literal.
usecs_fill_slice=$(sed -n '/^void UseCSMsg::fill_from_channel/,/^}/p' "$src/services/comm.cpp")
usecs_fill_gate_count=$(printf '%s\n' "$usecs_fill_slice" \
    | grep -F -c 'IS_PROTOCOL_VERSION(PROTOCOL_VERSION_CACHE_ADVERTISEMENT, c)' || true)
if [ "$usecs_fill_gate_count" -ne 1 ]; then
    echo "FAIL: UseCS decode cache tail must be gated at PROTOCOL_VERSION_CACHE_ADVERTISEMENT exactly once (found $usecs_fill_gate_count)" >&2
    exit 1
fi
echo 'ok - UseCS decode cache tail is gated at protocol 50'

usecs_send_slice=$(sed -n '/^void UseCSMsg::send_to_channel/,/^}/p' "$src/services/comm.cpp")
usecs_send_gate_count=$(printf '%s\n' "$usecs_send_slice" \
    | grep -F -c 'IS_PROTOCOL_VERSION(PROTOCOL_VERSION_CACHE_ADVERTISEMENT, c)' || true)
if [ "$usecs_send_gate_count" -ne 1 ]; then
    echo "FAIL: UseCS encode cache tail must be gated at PROTOCOL_VERSION_CACHE_ADVERTISEMENT exactly once (found $usecs_send_gate_count)" >&2
    exit 1
fi
echo 'ok - UseCS encode cache tail is gated at protocol 50'

# S2 (BigOracle steer): daemon/main.cpp derives its LOCAL relay's cache
# triple from ONE validated source (relay_cache_port/protocol/mask, each
# computed once from c->cacheHandoff), and BOTH scheduler_use_cs projection
# branches (self-selected-F 127.0.0.1 rewrite, and ordinary remote worker)
# consume that SAME source -- so neither branch can silently drop it or
# drift from the other.
require_count 1 'const uint32_t relay_cache_port = c->cacheHandoff.valid' \
    daemon/main.cpp 'the relay cache triple has exactly one validated source'
require_count 2 'relay_cache_mask);' daemon/main.cpp \
    'both scheduler_use_cs relay projections consume that same source'

require_absent \
    'cache_endpoint_port|cacheProtocol\(|cacheProfileMask|CACHE_PROFILE_Z3_(LONG|SHARED_LONG)' \
    'advertisement did not enter assignment, client attachment, or compiler input' \
    "$src/services/job.h" "$src/client/remote.cpp" \
    "$src/daemon/compiler_input.cpp" "$src/daemon/compiler_input.h" \
    "$src/daemon/workit.cpp" "$src/scheduler/job.cpp" "$src/scheduler/job.h"

require_absent 'z3_long|z3_shared_long|Z3_LONG|Z3_SHARED_LONG' \
    'declared streaming labels have no codec implementation' \
    "$src/cache/p50_zstd.cpp" "$src/cache/p50_zstd.h" \
    "$src/cache/p50_endpoint.cpp" "$src/cache/p50_endpoint.h" \
    "$src/cache/p50_slice0.cpp" "$src/cache/p50_slice0.h"

if grep -R -n 'z3_shared_long_b1' "$src/services" "$src/cache" \
        "$src/daemon" "$src/scheduler" "$src/client" >/dev/null 2>&1; then
    echo 'FAIL: experiment-only z3_shared_long_b1 entered product vocabulary' >&2
    exit 1
fi
echo 'ok - z3_shared_long_b1 remains experiment-only'

echo 'PASS: inert cache advertisement remains Login-only and non-selecting'
