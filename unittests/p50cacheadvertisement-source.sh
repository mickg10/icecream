#!/bin/sh
# Deletion-sensitive gate for protocol-50 capability negotiation and routing.
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

require_text() {
    file=$1
    pattern=$2
    label=$3
    if ! grep -F "$pattern" "$file" >/dev/null; then
        echo "FAIL: $label" >&2
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

require_count 8 'IS_PROTOCOL_VERSION(PROTOCOL_VERSION_CACHE_ADVERTISEMENT, c)' \
    services/comm.cpp \
    'Login, GetCS, UseCS, and CompileFile P50 tails each gate read and write at protocol 50'
require_count 1 'current_message_end = intogo_old + inmsglen' \
    services/comm.cpp 'decoder records the current frame boundary'
require_count 1 'if (c->current_message_bytes_remaining() < 3 * sizeof(uint32_t)) {' \
    services/comm.cpp 'Login refuses a shortened three-word tail'
# UseCS's decode is now mandatory and exact, matching Login's own binary
# short-tail check in shape (owner ruling on the d23d9c5d HOLD: protocol 50
# is an in-development draft with no deployed base and no intra-50
# compatibility obligation, so the earlier rolling-upgrade tri-state that
# treated a wholly-omitted tail as absence for a hypothetical
# pre-cache-handoff peer is superseded).  Absence is
# value-encoded (0/0/0) only; a wholly-omitted, partial, or over-length
# tail is refused identically to a malformed one.
require_count 1 'const size_t remaining = c->current_message_bytes_remaining();' \
    services/comm.cpp 'UseCS decode captures the remaining-bytes mandatory-tail input'
require_count 1 'if (remaining != 3 * sizeof(uint32_t)) {' services/comm.cpp \
    'UseCS decode requires exactly the three-word tail, matching Login'
require_count 1 'cache_request_tail_valid = c->read_bounded_string(' \
    services/comm.cpp 'GetCS decodes its warm host through the bounded string reader'
require_count 1 '8 * sizeof(uint32_t) + 2' services/comm.cpp \
    'GetCS requires the complete six-field/two-string P50 request tail'
require_count 1 'cache_retry_avoid_host, P50_CACHE_AFFINITY_HOST_MAX);' \
    services/comm.cpp 'GetCS decodes the retry endpoint host with the same bound'
require_count 1 'p50_cache_retry_avoid_is_valid(' services/comm.cpp \
    'GetCS validates canonical absent-or-exact retry endpoint fields'
require_count 1 'p50_cache_client_request_is_valid(' services/comm.cpp \
    'GetCS payload validation uses the canonical client-request law'
require_count 1 'const bool absent = cache_endpoint_port == 0' \
    services/comm.cpp 'Login payload has a canonical whole-absence branch'
require_count 1 'const bool present = cache_advertisement_is_well_formed_present(' \
    services/comm.cpp 'Login uses the canonical present-advertisement validator'
require_count 1 '&& (profile_mask & ~CACHE_ADVERTISABLE_PROFILE_MASK) == 0' \
    services/comm.h 'the canonical validator rejects every unknown profile bit'
require_count 3 'apply_cache_advertisement' daemon/main.cpp \
    'real daemon applies the canonical sidecar snapshot at definition, login, and shutdown reannouncement'
require_count 2 'cs->setCacheAdvertisement(m->cache_endpoint_port, m->cache_protocol,' \
    scheduler/scheduler.cpp 'scheduler retains initial and replacement Login snapshots'
require_count 1 'P29V1 = 1' cache/protocol50.h \
    'P29V1 has revision-1 profile ID 1'
require_count 1 'ZSTD_TU = 2' cache/protocol50.h \
    'ZSTD_TU has revision-1 profile ID 2'
require_count 1 'ZSTD_ROUTE = 3' cache/protocol50.h \
    'ZSTD_ROUTE has revision-1 profile ID 3'

# Stage 4: C/F capability compatibility is now a deliberate selection input,
# but only through one bounded preference layer.  A genuinely-free compatible
# worker is preferred; when none exists the original eligible set is retained.
require_count 1 'static uint32_t selected_cache_profile(' scheduler/scheduler.cpp \
    'scheduler has one canonical C/F profile-intersection helper'
require_count 1 'static bool prefer_cache_compatible_servers(' scheduler/scheduler.cpp \
    'scheduler has one bounded cache preference layer'
require_count 1 'if (!assignment_mode_prepares())' scheduler/scheduler.cpp \
    'legacy assignment mode bypasses cache-aware selection completely'
require_count 1 'compatible_free.push_back(cs);' scheduler/scheduler.cpp \
    'cache preference builds one genuine-free compatible selection set'
require_count 1 'static bool cache_retry_has_compatible_alternative(' scheduler/scheduler.cpp \
    'retry routing has one static-compatible alternative predicate'
require_count 1 '!cs->is_eligible_ever(job)' scheduler/scheduler.cpp \
    'alternative existence ignores transient load and capacity while retaining authorization'
require_count 1 'if (retry_alternative_exists) {' scheduler/scheduler.cpp \
    'exact endpoint exclusion is conditional on a genuine compatible alternative'
require_count 1 'return is_cache_retry_avoided_endpoint(job, cs);' scheduler/scheduler.cpp \
    'currently admissible failed endpoint is removed from retry selection'
require_count 1 'P50_RETRY_AVOID_WAIT job=' scheduler/scheduler.cpp \
    'transient alternative pressure emits an explicit wait disposition'
require_count 1 '!job->preferredHost().empty() && !retry_alternative_exists' \
    scheduler/scheduler.cpp \
    'retry exclusion outranks the ordinary preferred-host shortcut'
require_count 1 'cache_retry_wait = prefer_cache_compatible_servers(' \
    scheduler/scheduler.cpp \
    'server selection applies the bounded preference exactly once'
require_count 1 'if (!cache_retry_wait && !job->preExposureRedispatch() &&' \
    scheduler/scheduler.cpp \
    'retry wait, unexposed redispatch, and remote-required policy suppress submitter-local fallback'
require_count 1 'if (job->preExposureRedispatch() && cs == job->submitter()) {' \
    scheduler/scheduler.cpp \
    'ordinary selection excludes the local submitter during unexposed redispatch'
require_count 1 'const bool redispatch_local =' scheduler/scheduler.cpp \
    'preferred-host selection independently excludes the redispatch submitter'
require_count 1 '*c << remote_required;' services/comm.cpp \
    'GetCS writes the remote-required policy exactly once'
require_count 1 '*c >> remote_required;' services/comm.cpp \
    'GetCS reads the remote-required policy exactly once'
require_count 1 'remote_required <= 1' services/comm.cpp \
    'GetCS rejects a non-boolean remote-required policy'
require_count 1 'holding remote-required request for scheduler' daemon/main.cpp \
    'C holds a remote-required request instead of synthesizing localhost'
require_count 1 'No suitable remote host found for remote-required job' \
    scheduler/scheduler.cpp \
    'S queues a remote-required request until a remote worker is eligible'
require_count 1 'job->remoteRequired() && cs == job->submitter()' \
    scheduler/scheduler.cpp \
    'ordinary S selection excludes the submitter for remote-required work'
require_count 3 'remote-only policy refuses' client/main.cpp \
    'wrapper fallback exits are independently fail-closed for remote-only work'
require_count 1 'remote-required run has no local daemon transport' \
    client/main.cpp \
    'wrapper refuses remote-required work before a daemon transport exists'
require_count 1 'remote-required run refuses scheduler-selected localhost' \
    client/remote.cpp \
    'wrapper refuses the scheduler-local fast path for remote-required work'
require_count 1 'remote-required assignment needs protocol 50' \
    client/remote.cpp \
    'wrapper refuses policy loss through an older daemon protocol'
require_count 1 'if (g->remote_required == 1) {' daemon/main.cpp \
    'C preserves the remote-required hold after a failed login attempt'
require_count 1 'job->setRemoteRequired(m.remote_required == 1);' \
    scheduler/scheduler.cpp \
    'S retains the decoded policy on the admitted job'
require_count 1 'job->setCacheRequest(m.cache_protocol, m.cache_profile_mask,' \
    scheduler/scheduler.cpp 'decoded C capabilities are retained on every admitted job'
require_count 1 'cs->remotePort() == job->cacheAffinityPort() &&' \
    scheduler/scheduler.cpp \
    'warm preference binds the hinted host to the selected F ordinary port'
require_count 1 'p50_select_pair_cache_profile(' services/comm.h \
    'the canonical pair law selects only from the exact C/F intersection'

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

# daemon/main.cpp derives its LOCAL relay's cache
# triple from ONE validated source (the pure project_cache_handoff call), and
# BOTH scheduler_use_cs projection branches (self-selected-F 127.0.0.1
# rewrite, and ordinary remote worker) consume that same source -- so neither
# branch can silently drop it or drift from the other.  The local branch and
# its remote-branch introspection copy use relay-> assignments, while the
# remote branch's actual wire vehicle is a client_reply copy.
require_count 1 'const CacheHandoffProjection relay_cache = project_cache_handoff(' \
    daemon/main.cpp 'the relay cache triple has exactly one validated source'
require_count 2 'relay->cache_profile_mask = relay_cache_mask;' daemon/main.cpp \
    'both relay projections consume the validated cache source'
require_count 1 'client_reply.cache_profile_mask = relay_cache_mask;' daemon/main.cpp \
    'the remote-worker wire projection consumes the validated cache source'

# c->usecsmsg is NOT the wire
# vehicle for the remote-worker projection above -- that branch's actual
# client delivery is *msg, the scheduler's own frame, relayed directly;
# c->usecsmsg there exists only for introspection (dump_internals, the web
# JSON endpoints), so a source count on its constructor is bookkeeping, not
# proof of bytes sent.  This anchors the real send vehicle text itself; the
# actual proof that the right bytes reach the client on BOTH branches is
# unittests/cachehandoffdaemon.cpp's behavioral rows (Client A for the
# local rewrite, Client C for the remote worker), not a source count.
require_count 1 "This is the remote branch's ACTUAL client wire vehicle" \
    daemon/main.cpp 'the remote-worker branch is documented at its real send site'

# A pre-existing product bug
# predating the cache work): the local-rewrite branch used to hand-rebuild
# its relay from individual fields, hardcoding got_env=true and
# client_id=1 regardless of what the scheduler actually decided --
# client/remote.cpp's build_remote_int reads got_env to decide whether to
# send EnvTransferMsg, so a real got_env=false reply got silently
# overridden and a required environment transfer could be skipped.  Fix:
# copy *msg wholesale (every field preserved by construction, including
# any added later) and override only host/port + the cache projection
# above.  This anchors that copy-construction text itself; the actual
# proof that every other field survives exactly is
# unittests/cachehandoffdaemon.cpp's usecs_matches_except_host_and_cache
# behavioral row on Client A, not a source count.
require_count 2 'std::unique_ptr<UseCSMsg> relay(new UseCSMsg(*msg));' \
    daemon/main.cpp \
    'the local-rewrite relay is built by copying the scheduler frame, not hand-rebuilt field-by-field'

# The daemon's defensive re-check is factored
# into a small, pure, independently testable helper -- see its own comment
# in services/comm.h for why (UseCSMsg::valid_payload's identity-binding
# law means no live wire path can hand scheduler_use_cs the one input this
# helper exists to catch, so it is unit-tested directly with a hand-
# constructed object instead).
require_count 1 'inline bool usecs_cache_handoff_admissible' services/comm.h \
    'the daemon defensive re-check is a pure, independently testable helper'
require_count 1 'if (wrapper_cache_eligible && usecs_cache_handoff_admissible(*msg)) {' daemon/main.cpp \
    'scheduler_use_cs retains the cache handoff only via that helper'
require_count 1 'bool Daemon::cache_client_service_ready() noexcept' daemon/main.cpp \
    'daemon has one authenticated C-side cache readiness predicate'
require_count 5 'cache_client_service_ready()' daemon/main.cpp \
    'one declaration/definition, both wire boundaries, and deferred descriptor resumption use current C-side readiness'
require_count 1 'bool Daemon::cache_client_sidecar_ready() noexcept' daemon/main.cpp \
    'daemon separates C-sidecar lease health from scheduler publication readiness'
require_count 5 'cache_client_sidecar_ready()' daemon/main.cpp \
    'sidecar lease health is used by its declaration, definition, scheduler gate, initial admission, and recovery re-admission'
reconcile_slice=$(sed -n '/^void Daemon::reconcile_cache_route_state()/,/^}/p' \
    "$src/daemon/main.cpp")
if printf '%s\n' "$reconcile_slice" | grep -E \
        'scheduler_session_active|scheduler != nullptr' >/dev/null; then
    echo 'FAIL: C route ownership must not depend on scheduler connectivity' >&2
    exit 1
fi
echo 'ok - C route ownership is bound only to the authenticated sidecar ReadyLease'
poll_inactive_slice=$(sed -n '/if (!scheduler_cache_owner || scheduler == nullptr) {/,/^    }/p' \
    "$src/daemon/main.cpp")
if printf '%s\n' "$poll_inactive_slice" | grep -F \
        'outer_request_replacement' >/dev/null; then
    echo 'FAIL: scheduler loss must not replace a healthy C sidecar' >&2
    exit 1
fi
echo 'ok - scheduler loss suppresses publication without replacing the C sidecar'
waiter_lease_slice=$(sed -n '/^bool Daemon::invalidate_p50_source_waiters_for_lease()/,/^}/p' \
    "$src/daemon/main.cpp")
if printf '%s\n' "$waiter_lease_slice" | grep -E \
        'scheduler_session_active|scheduler != nullptr|cache_advertisement_snapshot' >/dev/null; then
    echo 'FAIL: armed input ownership must survive scheduler loss on the same ReadyLease' >&2
    exit 1
fi
echo 'ok - armed input ownership is invalidated only by C-sidecar ReadyLease change'
require_count 1 'if (umsg->count == 1 && client->connection_provenance.cache_eligible()) {' daemon/main.cpp \
    'only a provenance-authenticated singleton request may publish C capability'
require_count 1 'client_cache_capability.protocol == msg->cache_protocol' daemon/main.cpp \
    'the C kill switch also gates a scheduler-supplied handoff at relay time'
require_count 1 'requested_cache_capability.profile_mask &= umsg->cache_profile_mask;' daemon/main.cpp \
    'the C daemon authorizes only the wrapper-requested capability intersection'
require_count 4 'project_getcs_cache_route(' daemon/main.cpp \
    'one projection helper serves direct and deferred GetCS boundaries'
require_count 1 'const bool preserve_retry_avoid = capability.protocol != 0 &&' \
    daemon/main.cpp 'C preserves retry exclusion only under live P50 capability'
require_count 1 'request->cache_retry_avoid_port = 0;' daemon/main.cpp \
    'C canonicalizes retry endpoint state before each projection'
require_count 1 'request->cache_retry_avoid_port = requested_avoid_port;' \
    daemon/main.cpp 'C restores only the exact validated request-local exclusion'
require_count 1 'getcs.cache_profile_mask = CACHE_ADVERTISABLE_PROFILE_MASK;' client/remote.cpp \
    'a new wrapper explicitly opts an ordinary scalar request into retained profiles'
require_count 1 'getcs.cache_retry_avoid_port = retry_avoid_port;' client/remote.cpp \
    'strict retry serializes its exact failed ordinary F port on fresh GetCS'
require_count 1 'getcs.cache_retry_avoid_host = retry_avoid_host;' client/remote.cpp \
    'strict retry serializes its exact failed F host on fresh GetCS'
require_count 1 'ret = build_remote(job, local_daemon, envs, rate,' client/main.cpp \
    'the wrapper owns one bounded P50 reassignment loop'
require_count 1 'bool p50_retry_attempted = false;' client/main.cpp \
    'the fresh P50 reassignment has one wrapper-global retry budget'
require_count 1 'error.errorCode != 106 || p50_retry_attempted' client/main.cpp \
    'only the first authenticated P50 loss can request reassignment'
require_count 1 '!p50_retry_attempted || strict_p50,' client/main.cpp \
    'a normal retry sends canonical absence while a strict retry requests P50 again'
require_count 1 'strict_p50 && !error.hasRetryAvoidEndpoint()' client/main.cpp \
    'strict Error106 without an exact failed endpoint fails closed'
require_count 1 'p50_retry_avoid_host = error.retryAvoidHost;' client/main.cpp \
    'wrapper carries the failed host only into its one fresh assignment'
require_count 1 'p50_retry_avoid_port = error.retryAvoidPort;' client/main.cpp \
    'wrapper carries the failed port only into its one fresh assignment'
require_count 1 'p50_retry_attempted = true;' client/main.cpp \
    'the retry budget is consumed before the fresh GetCS'
require_count 1 'Each build_remote() call owns one fresh scheduler assignment attempt.' \
    client/remote.cpp \
    'every fresh assignment attempt canonicalizes reused CompileJob input state'
fresh_attempt_clear_line=$(grep -n -F \
    'job.clearCompileInputIdentity();' "$src/client/remote.cpp" | head -n 1 | cut -d: -f1)
first_local_branch_line=$(grep -n -F \
    'if (!maybe_build_local(local_daemon, usecs, job, ret)) {' \
    "$src/client/remote.cpp" | head -n 1 | cut -d: -f1)
if test -z "$fresh_attempt_clear_line" || test -z "$first_local_branch_line" || \
        test "$fresh_attempt_clear_line" -ge "$first_local_branch_line"; then
    echo 'FAIL: fresh attempt must clear CompileInputIdentity before the local/remote branch' >&2
    exit 1
fi
echo 'ok - fresh retry clears stale P50 input before scheduler-local admission'
require_count 1 'cache_affinity_profile_mask = cl->cacheHandoff.cacheProfileMask;' daemon/main.cpp \
    'successful cache-capable completion retains one soft warm-route hint'
require_count 1 'cache_affinity_port = cl->cacheHandoff.ordinaryPort;' daemon/main.cpp \
    'the retained warm-route hint includes the exact selected F ordinary port'
require_count 1 'p50_cache_route_observation_kind(' daemon/main.cpp \
    'warm-hint and typed failure retention are bound to the exact submitter observation'

# M3 now consumes the post-selection UseCS projection.  The exact selected
# assignment is the sole client authority: remote.cpp admits it once, connects
# to that same selected hostname, commits ZSTD_TU before CompileFile, and binds
# the resulting immutable selector.  The endpoint triple still may not leak
# backwards into scheduler scoring or the generic compiler-input reader.
require_count 1 'kP50CompilerConnectBudget = std::chrono::seconds(20)' client/remote.cpp \
    'a P50 compiler connection has one explicit bounded deadline policy'
require_count 1 'kP50CompilerConnectAttemptBudget = std::chrono::seconds(5)' client/remote.cpp \
    'one blackholed compiler socket cannot consume the complete assignment deadline'
require_count 1 'const bool cache_advertised_assignment =' client/remote.cpp \
    'the ordinary compiler connection selects P50 policy from the exact UseCS'
require_count 1 'Service::createChannelRetryUntil(' client/remote.cpp \
    'cache-advertised compiler connections retry only the same selected endpoint'
require_count 1 'Service::createChannel(hostname, port, 10)' client/remote.cpp \
    'legacy compiler connections retain the historical ten-second connector'
require_count 1 'p50_zstd_selected_profile(' client/remote.cpp \
    'the production client has one exact P50 profile selection/admission site'
require_count 1 'assignment.cache_endpoint_port' client/remote.cpp \
    'the cache connection reads the selected UseCS endpoint exactly once'
require_count 1 'job.setCompileInputIdentity(*identity);' client/remote.cpp \
    'a validated committed InputRecord binds CompileFile before it is sent'
require_absent \
    'cache_endpoint_port|cacheProtocol\(|cacheProfileMask|CACHE_PROFILE_Z3_(LONG|SHARED_LONG)' \
    'cache endpoint metadata remains absent from generic compiler-input readers' \
    "$src/daemon/compiler_input.cpp" "$src/daemon/compiler_input.h"

require_text "$src/cache/p50_zstd.cpp" 'ZstdRouteCodec' \
    'ZSTD_ROUTE codec implementation remains in the product path'
require_text "$src/cache/p50_endpoint.cpp" 'ProfileId::ZSTD_ROUTE' \
    'ZSTD_ROUTE endpoint profile remains in the product path'
require_absent 'z3_shared_long_b1' \
    'unimplemented route labels remain absent from the product path' \
    "$src/cache/p50_zstd.cpp" "$src/cache/p50_zstd.h" \
    "$src/cache/p50_endpoint.cpp" "$src/cache/p50_endpoint.h" \
    "$src/cache/p50_slice0.cpp" "$src/cache/p50_slice0.h"

if grep -R -n 'z3_shared_long_b1' "$src/services" "$src/cache" \
        "$src/daemon" "$src/scheduler" "$src/client" >/dev/null 2>&1; then
    echo 'FAIL: experiment-only z3_shared_long_b1 entered product vocabulary' >&2
    exit 1
fi
echo 'ok - z3_shared_long_b1 remains experiment-only'

echo 'PASS: protocol-50 routing uses bounded C/F capability preference with legacy escape'
