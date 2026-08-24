#!/bin/sh
# Deletion-sensitive source/mechanism gate for the installed sidecar bridge.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_cache_service.cpp"
header="$src/cache/p50_cache_service.h"
endpoint="$src/cache/p50_endpoint.cpp"
doc="$src/cache/P50_CACHE_SERVICE.md"
test_file="$src/unittests/p50cacheservice.cpp"

gate() {
    file=$1
    grep -F 'f_store_guid_for_identity' "$file" >/dev/null &&
        grep -F 'result.bytes[sizeof(identity.generation) + index]' "$file" >/dev/null &&
        grep -F 'g_signal_wake_fd = wake_fd' "$file" >/dev/null &&
        grep -F 'const ssize_t ignored = ::write' "$file" >/dev/null &&
        grep -F 'active_control_cancel_fd_.store' "$file" >/dev/null &&
        grep -F 'context_.post([this]' "$file" >/dev/null &&
        grep -F 'runtime_config.f_store_guid = f_store_guid_for_identity' "$file" >/dev/null &&
        grep -F 'SidecarRuntime::run_one' "$file" >/dev/null &&
        grep -F 'FdHandoffReceiver receiver' "$file" >/dev/null &&
        grep -F 'receiver.receive_and_ack' "$file" >/dev/null &&
        grep -F 'receiver.take_adopted_fd' "$file" >/dev/null &&
        grep -F 'P50ServerEndpoint::adopt_connected_fd' "$file" >/dev/null &&
        grep -F 'endpoint_->run_adopted' "$file" >/dev/null &&
        grep -F 'busy_.test_and_set' "$file" >/dev/null &&
        grep -F 'cancel_active_socket()' "$file" >/dev/null &&
        grep -F 'RuntimeConfig' "$file" >/dev/null
}

gate "$impl"
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

grep -F 'native_handle()' "$src/cache/p50_local_transport.h" >/dev/null
grep -F 'active_control_cancel_fd_' "$header" >/dev/null
grep -F 'cancel_active_io' "$endpoint" "$src/cache/p50_endpoint.h" >/dev/null
grep -F 'test_runtime_store_identity_fences_attempt' "$test_file" >/dev/null
grep -F 'test_runtime_stop_interrupts_control_wait' "$test_file" >/dev/null
grep -F 'test_runtime_stop_interrupts_active_endpoint' "$test_file" >/dev/null
grep -F 'signal_interrupts_control_wait(SIGTERM)' "$test_file" >/dev/null
grep -F 'signal_interrupts_control_wait(SIGINT)' "$test_file" >/dev/null
grep -F 'second_control.sender' "$test_file" >/dev/null
grep -F 'READY\n' "$doc" >/dev/null
grep -F 'generation and attempt' "$doc" >/dev/null
grep -F '0/0/0' "$doc" >/dev/null
grep -F 'run_adopted' "$doc" >/dev/null

# Mechanism boundary: this file must not acquire a public listener or daemon
# advertisement ownership.
if grep -E 'tcp::acceptor|LoginMsg|cache_port|apply_inert_cache_advertisement' \
    "$impl" "$header" >/dev/null; then
    echo 'FAIL: service bridge gained public listener or advertisement ownership' >&2
    exit 1
fi

mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50cacheservice-source.XXXXXX")
trap 'rm -rf "$mutant_dir"' EXIT HUP INT TERM

for pattern in \
    'result.bytes[sizeof(identity.generation) + index]' \
    'g_signal_wake_fd = wake_fd' \
    'const ssize_t ignored = ::write' \
    'active_control_cancel_fd_.store' \
    'context_.post([this]' \
    'runtime_config.f_store_guid = f_store_guid_for_identity'; do
    mutant="$mutant_dir/mutant.cpp"
    awk -v needle="$pattern" 'index($0, needle) == 0' "$impl" >"$mutant"
    if gate "$mutant"; then
        echo "deletion mutant survived: $pattern" >&2
        exit 1
    fi
done

echo 'ok - cache service bridge source gates hold'
