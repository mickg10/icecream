#!/bin/sh
set -eu

src="${srcdir:-$(dirname "$0")}/../cache/p50_daemon_sidecar_adapter.cpp"
header="${srcdir:-$(dirname "$0")}/../cache/p50_daemon_sidecar_adapter.h"

grep -F 'connect_unix_until' "$src" >/dev/null
grep -F 'cumulative_post_ready_exits_' "$src" >/dev/null
grep -F 'prior_supervisor_post_ready_exits_' "$src" >/dev/null
grep -F 'disable_relationship();' "$src" >/dev/null
grep -F 'cleanup_attempt_node();' "$src" >/dev/null
grep -F 'max_restarts = 0' "$src" >/dev/null
grep -F 'observe_public_listener' "$header" >/dev/null

if grep -E 'daemon/main\.cpp|signal\(|sigaction\(|listen_unix\(' "$src" "$header" >/dev/null; then
    echo 'FAIL: adapter acquired daemon-main, signal-handler, or public-listener ownership' >&2
    exit 1
fi
if grep -E 'unlink\(.*socket|unlink\(socket_path_' "$src" >/dev/null; then
    echo 'FAIL: adapter pathname-unlinked a service-owned socket' >&2
    exit 1
fi
echo 'p50 daemon sidecar adapter source: ok'
