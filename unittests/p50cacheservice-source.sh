#!/bin/sh
# Deletion-sensitive mechanism gate for the sidecar handoff bridge.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_cache_service.cpp"
header="$src/cache/p50_cache_service.h"
doc="$src/cache/P50_CACHE_SERVICE.md"

for pattern in \
    'SidecarRuntime::run_one' \
    'FdHandoffReceiver receiver' \
    'receiver.receive_and_ack' \
    'receiver.take_adopted_fd' \
    'P50ServerEndpoint::adopt_connected_fd' \
    'endpoint_->run_adopted' \
    'busy_.test_and_set' \
    'cancel_active_socket()' \
    'RuntimeConfig' \
    'max_live_handoffs = 1'; do
    grep -F "$pattern" "$impl" "$header" >/dev/null
done

# The bridge must remain mechanism-only: no daemon listener or advertisement.
if grep -E 'tcp::acceptor|listen\(|LoginMsg|cache_port|apply_inert_cache_advertisement' \
    "$impl" "$header" >/dev/null; then
    echo 'FAIL: service bridge gained public listener or advertisement ownership' >&2
    exit 1
fi
grep -F '0/0/0' "$doc" >/dev/null
grep -F 'run_adopted' "$doc" >/dev/null
grep -F 'test_runtime_zstd_tu_af_unix_loopback' "$src/unittests/p50cacheservice.cpp" >/dev/null
grep -F 'observed == input' "$src/unittests/p50cacheservice.cpp" >/dev/null
grep -F 'test_runtime_identity_disconnect_and_endpoint_failure' "$src/unittests/p50cacheservice.cpp" >/dev/null

mutant=$(mktemp "${TMPDIR:-/tmp}/p50cacheservice-source.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed '/receiver.receive_and_ack/d' "$impl" >"$mutant"
if grep -F 'receiver.receive_and_ack' "$mutant" >/dev/null; then
    echo 'FAIL: handoff-receive deletion mutant was not formed' >&2
    exit 1
fi
sed '/P50ServerEndpoint::adopt_connected_fd/d' "$impl" >"$mutant"
if grep -F 'P50ServerEndpoint::adopt_connected_fd' "$mutant" >/dev/null; then
    echo 'FAIL: endpoint-adoption deletion mutant was not formed' >&2
    exit 1
fi
sed '/endpoint_->run_adopted/d' "$impl" >"$mutant"
if grep -F 'endpoint_->run_adopted' "$mutant" >/dev/null; then
    echo 'FAIL: shared-reducer deletion mutant was not formed' >&2
    exit 1
fi
echo 'ok - cache service bridge source gates hold'
