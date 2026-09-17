#!/bin/sh
# Deletion-sensitive contract for the production iceccd/sidecar integration.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:?}
daemon="$src/daemon/main.cpp"
makefile="$src/daemon/Makefile.am"
runtime_test="$src/unittests/p50daemonpositive.cpp"

require() {
    grep -F -- "$2" "$1" >/dev/null
}

contract() {
    candidate=$1
    require "$candidate" 'exact_public_tcp_listener' &&
    require "$candidate" 'SO_ACCEPTCONN' &&
    require "$candidate" 'DaemonSidecarAdapter::valid_config' &&
    require "$candidate" 'if (!scheduler_cache_owner || scheduler == nullptr)' &&
    require "$candidate" 'cache_adapter->outer_begin_turn' &&
    require "$candidate" 'cache_adapter->outer_append_pollfds(pollfds)' &&
    require "$candidate" 'cache_adapter->outer_advance_turn' &&
    require "$candidate" 'reannounce_environments(&update.transitions[index])' &&
    require "$candidate" 'scheduler_cache_snapshot != current' &&
    require "$candidate" 'scheduler_session_active && cache_adapter != nullptr' &&
    require "$candidate" 'cache_advertisement_snapshot().present()' &&
    require "$candidate" 'cache_adapter->outer_request_shutdown(&update)' &&
    require "$candidate" 'cache_child_reaper.reap_one' &&
    require "$candidate" 'cache_adapter->outer_observe_child_reaped' &&
    require "$candidate" 'apply_cache_advertisement(lmsg, absent)' &&
    require "$candidate" 'scheduler_cache_snapshot_valid = false' &&
    require "$candidate" 'client_accept_batch_limit' &&
    require "$candidate" 'accepted_count < client_accept_batch_limit' &&
    require "$candidate" 'pending_client_admission_limit' &&
    require "$candidate" 'pending_remote_admission_limit' &&
    require "$candidate" 'RLIMIT_NOFILE' &&
    require "$candidate" 'Service::createChannelAccepted' &&
    require "$candidate" 'service_pending_client_admissions(pollfds)' &&
    require "$candidate" 'service_pending_client_admissions_now()' &&
    require "$candidate" 'current_remote_client_admission_capacity' &&
    require "$candidate" 'finish_protocol_admission()' &&
    require "$candidate" 'ICECC_PROTOCOL_HANDSHAKE_TIMEOUT_MSEC' &&
    require "$candidate" 'This phase is admission-only' &&
    require "$candidate" 'O_NONBLOCK' &&
    require "$candidate" 'Accept readiness never suppresses' &&
    require "$candidate" 'cache_sidecar_recovery_in_progress' &&
    require "$candidate" 'deferred_getcs_waits_for_cache' &&
    require "$candidate" 'Hold every one-job cache-capable request' &&
    require "$candidate" 'holding P50 cache-capable request for cache recovery'
}

contract "$daemon" || {
    echo 'FAIL: production positive sidecar wiring contract is incomplete' >&2
    exit 1
}

# The bounded listener loop must only admit/authenticate/register channels.
# Running the ordinary client state machine inside this region serializes
# acceptance behind source/session/compile work and recreates the exact queue
# starvation this batch is meant to prevent.  Existing real-daemon source-arm
# and CacheSession coverage exercises the deferred path; this gate makes the
# scheduling boundary deterministic, deletion-sensitive, and fails on the
# predecessor's inline handler.
accept_block=$(sed -n \
    '/This phase is admission-only/,/Accept readiness never suppresses/p' \
    "$daemon")
if printf '%s\n' "$accept_block" | grep -F 'handle_activity(' >/dev/null; then
    echo 'FAIL: bounded accept phase runs ordinary client activity inline' >&2
    exit 1
fi

# Strict P50 and remote-required are independent wrapper policies.  The farm's
# strict S70 workload sets ICECC_P50_C1F1_REQUIRED without setting
# ICECC_REMOTE_REQUIRED, so the bounded replacement hold must be gated by the
# explicit cache capability rather than the unrelated remote_required bit.
recovery_hold_block=$(sed -n \
    '/const bool wait_for_cache_recovery =/,/sidecar_recovery_in_progress());/p' \
    "$daemon")
if ! printf '%s\n' "$recovery_hold_block" | \
        grep -F 'should_defer_cache_capable_getcs' >/dev/null; then
    echo 'FAIL: sidecar recovery hold is not gated by explicit P50 capability' >&2
    exit 1
fi
if printf '%s\n' "$recovery_hold_block" | grep -F 'remote_required' >/dev/null; then
    echo 'FAIL: strict P50 recovery hold incorrectly depends on remote_required' >&2
    exit 1
fi
if printf '%s\n' "$accept_block" | grep -F 'Service::createChannel(acc_fd' >/dev/null; then
    echo 'FAIL: bounded accept phase synchronously waits for peer protocol' >&2
    exit 1
fi
require "$makefile" 'libp50daemonsidecaradapter.a'
require "$makefile" 'libp50sidecarlifecycle.a'
require "$makefile" 'libp50readyadvertisement.a'
require "$makefile" 'libp50sidecarsupervisor.a'
require "$runtime_test" 'initial Login is canonical cache absence before ConfCS/READY'
require "$runtime_test" 'LOGIN_ATTEMPT cannot dispatch cache while scheduler is inactive'
require "$runtime_test" 'kAdmissionBurstCount = 65'
require "$runtime_test" 'turn-boundary admission waits behind at most one client activity'
require "$runtime_test" 'remote handshake saturation preserves prompt Unix-client admission'
require "$runtime_test" 'source-arm owner is acknowledged before CACHE_SESSION'
require "$runtime_test" 'source-arm acknowledgement bypasses a silent accepted handshake'
require "$runtime_test" 'silent accepted handshake expires on the preserved bounded lifetime'
require "$runtime_test" 'orderly shutdown closes a still-pending protocol admission'
require "$runtime_test" 'authenticated one-shot handoff keeps the adopted session live'
require "$runtime_test" 'kAuthoritativeSessionCount = 65'
require "$runtime_test" 'more than 64 sequential authoritative CacheSessions remain accepted'
require "$runtime_test" 'authoritative CacheSessions leave no retained P5FS descriptors'
require "$runtime_test" 'accepted handoff keeps the READY advertisement stable'
require "$runtime_test" 'orderly shutdown withdraws before scheduler teardown'

if grep -F 'apply_inert_cache_advertisement' "$daemon" >/dev/null \
        || grep -F 'cache_dispatcher->dispatch' "$daemon" >/dev/null; then
    echo 'FAIL: obsolete mechanism-only daemon wiring survived' >&2
    exit 1
fi
if grep -F 'cache_adapter->start(' "$daemon" >/dev/null \
        || grep -F 'cache_adapter->poll(' "$daemon" >/dev/null \
        || grep -F 'cache_adapter->shutdown(' "$daemon" >/dev/null \
        || grep -F 'advance_cache_adapter_shutdown_turn' "$daemon" >/dev/null; then
    echo 'FAIL: synchronous/secondary sidecar adapter path survived production wiring' >&2
    exit 1
fi

# handle_cache_session has one authoritative fd handoff.  It must not mint or
# retain the removed second P5FS operation/control relationship per session.
cache_session_block=$(sed -n \
    '/^bool Daemon::handle_cache_session/,/^bool Daemon::handle_p50_source_arm/p' \
    "$daemon")
if printf '%s\n' "$cache_session_block" | \
        grep -E 'DaemonFSessionOperation|fsession_op|fsession_control_fd|connect_unix_until' \
        >/dev/null; then
    echo 'FAIL: CACHE_SESSION production path recreated a shadow P5FS owner' >&2
    exit 1
fi

mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50daemonpositive-source.XXXXXX")
trap 'rm -rf "$mutant_dir"' EXIT HUP INT TERM

# Every integration edge is independently observable by this contract.  The
# real process test supplies the behavioral proof; these deletion mutants keep
# a future edit from silently removing one owner while leaving that test text
# in the archive.
for needle in \
    'exact_public_tcp_listener' \
    'SO_ACCEPTCONN' \
    'DaemonSidecarAdapter::valid_config' \
    'if (!scheduler_cache_owner || scheduler == nullptr)' \
    'cache_adapter->outer_begin_turn' \
    'cache_adapter->outer_append_pollfds(pollfds)' \
    'cache_adapter->outer_advance_turn' \
    'reannounce_environments(&update.transitions[index])' \
    'scheduler_cache_snapshot != current' \
    'scheduler_session_active && cache_adapter != nullptr' \
    'cache_advertisement_snapshot().present()' \
    'cache_adapter->outer_request_shutdown(&update)' \
    'cache_child_reaper.reap_one' \
    'cache_adapter->outer_observe_child_reaped' \
    'apply_cache_advertisement(lmsg, absent)' \
    'scheduler_cache_snapshot_valid = false' \
    'client_accept_batch_limit' \
    'accepted_count < client_accept_batch_limit' \
    'pending_client_admission_limit' \
    'pending_remote_admission_limit' \
    'RLIMIT_NOFILE' \
    'Service::createChannelAccepted' \
    'service_pending_client_admissions(pollfds)' \
    'service_pending_client_admissions_now()' \
    'current_remote_client_admission_capacity' \
    'finish_protocol_admission()' \
    'ICECC_PROTOCOL_HANDSHAKE_TIMEOUT_MSEC' \
    'This phase is admission-only' \
    'O_NONBLOCK' \
    'Accept readiness never suppresses' \
    'cache_sidecar_recovery_in_progress' \
    'deferred_getcs_waits_for_cache' \
    'Hold every one-job cache-capable request' \
    'holding P50 cache-capable request for cache recovery'; do
    mutant="$mutant_dir/main.cpp"
    awk -v removed="$needle" 'index($0, removed) == 0' "$daemon" >"$mutant"
    if contract "$mutant"; then
        echo "FAIL: positive daemon deletion mutant survived: $needle" >&2
        exit 1
    fi
done

echo 'ok - positive daemon production wiring and deletion mutants hold'
