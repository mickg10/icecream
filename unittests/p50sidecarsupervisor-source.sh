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
grep -F 'write_errno_record' "$impl" >/dev/null
grep -F 'mark_child_fds_cloexec' "$impl" >/dev/null
grep -F 'close_range' "$impl" >/dev/null
grep -F 'setpgid' "$impl" >/dev/null
grep -F 'max_attempts_per_recovery' "$impl" "$header" >/dev/null

# The component must remain detached from advertisement and daemon ownership.
if grep -E 'daemon/main|apply_inert_cache_advertisement|LoginMsg|port' "$impl" "$header" >/dev/null; then
    echo 'FAIL: supervisor gained daemon or advertisement integration' >&2
    exit 1
fi

mutant=$(mktemp "${TMPDIR:-/tmp}/p50sidecarsupervisor-mutant.XXXXXX")
mutant_fds=$(mktemp "${TMPDIR:-/tmp}/p50sidecarsupervisor-fd-mutant.XXXXXX")
mutant_ready=$(mktemp "${TMPDIR:-/tmp}/p50sidecarsupervisor-ready-mutant.XXXXXX")
trap 'rm -f "$mutant" "$mutant_fds" "$mutant_ready"' EXIT HUP INT TERM
sed 's/const bool group_alive = grouped && group_exists(process_group_);/const bool group_alive = false;/' \
    "$impl" >"$mutant"
if grep -F 'group_exists(process_group_)' "$mutant" >/dev/null; then
    echo 'FAIL: process-group shutdown deletion mutant was accepted' >&2
    exit 1
fi
sed 's/if (!mark_child_fds_cloexec(ambient_fd_limit))/if (true)/' "$impl" >"$mutant_fds"
if grep -F 'if (!mark_child_fds_cloexec(ambient_fd_limit))' "$mutant_fds" >/dev/null; then
    echo 'FAIL: ambient-FD deletion mutant was not formed' >&2
    exit 1
fi
sed 's/if (ready.size() == kReadyMessageSize)/if (false)/' "$impl" >"$mutant_ready"
if grep -F 'if (ready.size() == kReadyMessageSize)' "$mutant_ready" >/dev/null; then
    echo 'FAIL: exact-READY deletion mutant was not formed' >&2
    exit 1
fi
echo 'ok - sidecar supervisor source gates hold'
