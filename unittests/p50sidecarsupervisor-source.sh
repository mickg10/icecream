#!/bin/sh
# Deletion-sensitive gates for the lifecycle-only sidecar supervisor.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_sidecar_supervisor.cpp"
header="$src/cache/p50_sidecar_supervisor.h"

grep -F 'kReadyFdEnvironment' "$impl" >/dev/null
grep -F 'READY\n' "$impl" >/dev/null
grep -F 'State::DegradedLegacy' "$impl" >/dev/null
grep -F 'SIGTERM' "$impl" >/dev/null
grep -F 'SIGKILL' "$impl" >/dev/null
grep -F 'FD_CLOEXEC' "$impl" >/dev/null
grep -F 'std::chrono::steady_clock' "$impl" >/dev/null
grep -F 'access(config.executable.c_str(), X_OK)' "$impl" >/dev/null
grep -F 'execve' "$impl" >/dev/null

# The component must remain detached from advertisement and daemon ownership.
if grep -E 'daemon/main|apply_inert_cache_advertisement|LoginMsg|port' "$impl" "$header" >/dev/null; then
    echo 'FAIL: supervisor gained daemon or advertisement integration' >&2
    exit 1
fi

mutant=$(mktemp "${TMPDIR:-/tmp}/p50sidecarsupervisor-mutant.XXXXXX")
trap 'rm -f "$mutant"' EXIT HUP INT TERM
sed 's/::kill(child_pid_, SIGKILL)/::kill(child_pid_, SIGTERM)/' "$impl" >"$mutant"
if grep -F 'SIGKILL' "$mutant" >/dev/null; then
    echo 'FAIL: shutdown escalation deletion mutant was accepted' >&2
    exit 1
fi
echo 'ok - sidecar supervisor source gates hold'
