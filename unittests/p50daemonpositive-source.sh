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
    require "$candidate" 'if (!scheduler_session_active || scheduler == nullptr)' &&
    require "$candidate" 'cache_adapter->start(&update)' &&
    require "$candidate" 'reannounce_environments(&update.transitions[index])' &&
    require "$candidate" 'scheduler_cache_snapshot != current' &&
    require "$candidate" 'scheduler_session_active && cache_adapter != nullptr' &&
    require "$candidate" 'cache_advertisement_snapshot().present()' &&
    require "$candidate" 'cache_adapter->shutdown(&update)' &&
    require "$candidate" 'apply_cache_advertisement(lmsg, absent)' &&
    require "$candidate" 'scheduler_cache_snapshot_valid = false'
}

contract "$daemon" || {
    echo 'FAIL: production positive sidecar wiring contract is incomplete' >&2
    exit 1
}
require "$makefile" 'libp50daemonsidecaradapter.a'
require "$makefile" 'libp50readyadvertisement.a'
require "$makefile" 'libp50sidecarsupervisor.a'
require "$runtime_test" 'initial Login is canonical cache absence before ConfCS/READY'
require "$runtime_test" 'LOGIN_ATTEMPT cannot dispatch cache while scheduler is inactive'
require "$runtime_test" 'one-shot handoff publishes withdrawal first'
require "$runtime_test" 'fresh authenticated relationship republishes presence second'
require "$runtime_test" 'orderly shutdown withdraws before scheduler teardown'

if grep -F 'apply_inert_cache_advertisement' "$daemon" >/dev/null \
        || grep -F 'cache_dispatcher->dispatch' "$daemon" >/dev/null; then
    echo 'FAIL: obsolete mechanism-only daemon wiring survived' >&2
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
    'if (!scheduler_session_active || scheduler == nullptr)' \
    'cache_adapter->start(&update)' \
    'reannounce_environments(&update.transitions[index])' \
    'scheduler_cache_snapshot != current' \
    'scheduler_session_active && cache_adapter != nullptr' \
    'cache_advertisement_snapshot().present()' \
    'cache_adapter->shutdown(&update)' \
    'apply_cache_advertisement(lmsg, absent)' \
    'scheduler_cache_snapshot_valid = false'; do
    mutant="$mutant_dir/main.cpp"
    awk -v removed="$needle" 'index($0, removed) == 0' "$daemon" >"$mutant"
    if contract "$mutant"; then
        echo "FAIL: positive daemon deletion mutant survived: $needle" >&2
        exit 1
    fi
done

echo 'ok - positive daemon production wiring and deletion mutants hold'
