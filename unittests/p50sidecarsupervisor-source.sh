#!/bin/sh
# Deletion-sensitive gates for the lifecycle-only sidecar supervisor.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
impl="$src/cache/p50_sidecar_supervisor.cpp"
header="$src/cache/p50_sidecar_supervisor.h"

lease_gate() {
    candidate_impl=$1
    candidate_header=$2
    grep -F 'class LaunchIdentityAllocator' "$candidate_header" >/dev/null &&
        grep -F 'std::shared_ptr<LaunchIdentityAllocator> launch_identities' \
            "$candidate_header" >/dev/null &&
        grep -F 'LaunchIdentityAllocator::allocate()' "$candidate_impl" >/dev/null &&
        grep -F 'config_.launch_identities->allocate()' "$candidate_impl" >/dev/null &&
        grep -F 'std::string_view(ready).substr(0, prefix_bytes)' \
            "$candidate_impl" >/dev/null &&
        grep -F 'prefix.substr(0, prefix_bytes)' "$candidate_impl" >/dev/null &&
        grep -F 'kReadyLeaseLivenessBarrierMilliseconds' \
            "$candidate_impl" >/dev/null &&
        grep -F 'child_has_exited_exact(expected_child)' "$candidate_impl" >/dev/null &&
        grep -F 'SYS_pidfd_open' "$candidate_impl" >/dev/null &&
        grep -F 'SYS_pidfd_send_signal' "$candidate_impl" >/dev/null &&
        grep -F 'int child_pidfd_ = -1;' "$candidate_header" >/dev/null &&
        grep -F 'exact_child_handles_supported()' \
            "$candidate_impl" >/dev/null &&
        grep -F 'read_launch_permission(launch_gate[0])' \
            "$candidate_impl" >/dev/null &&
        grep -F 'make_launch_gate(launch_gate)' \
            "$candidate_impl" >/dev/null &&
        grep -F 'send_launch_permission(launch_gate[1])' \
            "$candidate_impl" >/dev/null &&
        grep -F 'MSG_NOSIGNAL' "$candidate_impl" >/dev/null &&
        grep -F 'if (pidfd < 0)' "$candidate_impl" >/dev/null &&
        grep -F '::setsid()' "$candidate_impl" >/dev/null &&
        grep -F 'write_session_marker(exec_pipe[1])' \
            "$candidate_impl" >/dev/null &&
        grep -F 'marker != kExecSessionMarker' \
            "$candidate_impl" >/dev/null &&
        grep -F 'child_owns_session(expected_child)' \
            "$candidate_impl" >/dev/null &&
        grep -F 'stop_child_handle(' "$candidate_impl" >/dev/null &&
        grep -F 'signal_child_handle(pidfd, SIGSTOP)' \
            "$candidate_impl" >/dev/null &&
        grep -F 'constexpr idtype_t kPidfdIdType' \
            "$candidate_impl" >/dev/null &&
        grep -F 'WSTOPPED | WEXITED | WNOHANG | WNOWAIT' \
            "$candidate_impl" >/dev/null &&
        grep -F 'anchor == ChildAnchorResult::Stopped' \
            "$candidate_impl" >/dev/null &&
        grep -F 'child_is_in_group(expected_child, expected_group)' \
            "$candidate_impl" >/dev/null &&
        grep -F 'signal_child_handle(child_pidfd_, SIGKILL)' \
            "$candidate_impl" >/dev/null &&
        grep -F 'const bool proven_dead = terminate_child();' \
            "$candidate_impl" >/dev/null &&
        grep -F 'if (proven_dead) {' "$candidate_impl" >/dev/null &&
        grep -F 'SYS_renameat2' "$candidate_impl" >/dev/null &&
        grep -F 'RENAME_NOREPLACE' "$candidate_impl" >/dev/null &&
        grep -F 'AT_SYMLINK_NOFOLLOW' "$candidate_impl" >/dev/null &&
        grep -F 'capture_and_remove_at' "$candidate_impl" >/dev/null &&
        grep -F 'return direct_dead && group_dead;' \
            "$candidate_impl" >/dev/null
}

lease_gate "$impl" "$header"

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
grep -F 'write_session_marker' "$impl" >/dev/null
grep -F 'mark_child_fds_cloexec' "$impl" >/dev/null
grep -F 'close_range' "$impl" >/dev/null
grep -F 'ICECC_P50_FORCE_FD_FALLBACK' "$impl" >/dev/null
grep -F 'SYS_getdents64' "$impl" >/dev/null
grep -F 'SYS_openat' "$impl" >/dev/null
grep -F 'SYS_kill' "$impl" >/dev/null
grep -F 'errno != EPERM' "$impl" >/dev/null
grep -F 'WNOWAIT' "$impl" >/dev/null
grep -F 'waitid(kPidfdIdType' "$impl" >/dev/null
grep -F 'child_has_exited' "$impl" >/dev/null
grep -F 'getpgid' "$impl" >/dev/null
grep -F 'move-group' "$src/unittests/p50_sidecar_supervisor_test.cpp" >/dev/null
grep -F 'reaped/reused numeric PID' \
    "$src/unittests/p50_sidecar_supervisor_test.cpp" >/dev/null
grep -F 'shutdown_owns_process_group_against_external_reaper' \
    "$src/unittests/p50_sidecar_supervisor_test.cpp" >/dev/null
grep -F 'pre_ready_exited_leader_refuses_unanchored_group_signal' \
    "$src/unittests/p50_sidecar_supervisor_test.cpp" >/dev/null
grep -F 'session_leader_refuses_group_escape' \
    "$src/unittests/p50_sidecar_supervisor_test.cpp" >/dev/null
grep -F 'setsid' "$impl" >/dev/null
grep -F 'process_group_owned_' "$impl" "$header" >/dev/null
grep -F 'process_group_ = -1' "$impl" >/dev/null
grep -F 'increment_saturating' "$impl" >/dev/null
grep -F 'max_attempts_per_recovery' "$impl" "$header" >/dev/null
grep -F 'mkdtemp' "$impl" >/dev/null
grep -F 'parse_ready_lease' "$impl" >/dev/null
grep -F 'F_STORE_GUID' "$impl" "$header" >/dev/null
grep -F 'listener_device' "$impl" "$header" >/dev/null
grep -F 'pathname_info' "$impl" >/dev/null
grep -F 'lstat(lease.socket_path.c_str()' "$impl" >/dev/null
grep -F 'socket_path_digest' "$impl" "$header" >/dev/null
grep -F 'cleanup_lease' "$impl" >/dev/null
grep -F 'rmdir' "$impl" >/dev/null
grep -F 'SYS_renameat2' "$impl" >/dev/null
grep -F 'RENAME_NOREPLACE' "$impl" >/dev/null
grep -F 'fstatat' "$impl" >/dev/null
grep -F 'AT_SYMLINK_NOFOLLOW' "$impl" >/dev/null
grep -F 'capture_and_remove_at' "$impl" >/dev/null
grep -F 'canonical_absolute_lease_path' "$header" "$impl" >/dev/null
grep -F 'directory_device == 0' "$header" >/dev/null
grep -F 'socket_path_digest != digest128(socket_path)' "$header" >/dev/null
grep -F 'cleanup_never_deletes_replaced_socket' \
    "$src/unittests/p50_sidecar_supervisor_test.cpp" >/dev/null
grep -F 'cleanup_never_deletes_replaced_directory' \
    "$src/unittests/p50_sidecar_supervisor_test.cpp" >/dev/null
grep -F 'structured_actual_service_publishes_prebound_ready' \
    "$src/unittests/p50_sidecar_supervisor_test.cpp" >/dev/null
grep -F 'structured-trailing-space' \
    "$src/unittests/p50_sidecar_supervisor_test.cpp" >/dev/null
if grep -E 'lstat\(lease->|unlink\(lease->|rmdir\(lease->' "$impl" >/dev/null; then
    echo 'FAIL: lease cleanup regressed to pathname lstat/unlink/rmdir' >&2
    exit 1
fi

# A stale numeric PID must never be a direct signal target.  Group signalling
# remains separately guarded by the proven PGID ownership fence.
if grep -E 'signal_target\(child_pid_|kill\(child_pid_' "$impl" >/dev/null; then
    echo 'FAIL: supervisor directly signals a reusable numeric child PID' >&2
    exit 1
fi
if grep -F 'signal_child_handle(child_pidfd_, SIGTERM)' "$impl" >/dev/null; then
    echo 'FAIL: exact leader must remain STOP-anchored until numeric group use ends' >&2
    exit 1
fi
grep -F 'if (child_pidfd_ >= 0)' "$impl" >/dev/null
grep -F 'static_cast<id_t>(child_pidfd_)' "$impl" >/dev/null
if grep -F 'setpgid' "$impl" >/dev/null; then
    echo 'FAIL: supervisor regressed from a private session to a joinable daemon-session group' >&2
    exit 1
fi

# The component must remain detached from advertisement and daemon ownership.
if grep -E 'daemon/main|apply_inert_cache_advertisement|LoginMsg|endpoint_port' "$impl" "$header" >/dev/null; then
    echo 'FAIL: supervisor gained daemon or advertisement integration' >&2
    exit 1
fi

mutant=$(mktemp "${TMPDIR:-/tmp}/p50sidecarsupervisor-mutant.XXXXXX")
mutant_fds=$(mktemp "${TMPDIR:-/tmp}/p50sidecarsupervisor-fd-mutant.XXXXXX")
mutant_ready=$(mktemp "${TMPDIR:-/tmp}/p50sidecarsupervisor-ready-mutant.XXXXXX")
mutant_dir=$(mktemp -d "${TMPDIR:-/tmp}/p50sidecarsupervisor-lease-mutants.XXXXXX")
trap 'rm -f "$mutant" "$mutant_fds" "$mutant_ready"; rm -rf "$mutant_dir"' \
    EXIT HUP INT TERM
sed 's/const bool claimed_group = process_group_owned_ && expected_group > 1;/const bool claimed_group = false;/' \
    "$impl" >"$mutant"
if grep -F 'process_group_owned_ && expected_group > 1' "$mutant" >/dev/null; then
    echo 'FAIL: process-group ownership guard mutant was not formed' >&2
    exit 1
fi
sed 's/if (!mark_child_fds_cloexec(ambient_fd_limit))/if (true)/' "$impl" >"$mutant_fds"
if grep -F 'if (!mark_child_fds_cloexec(ambient_fd_limit))' "$mutant_fds" >/dev/null; then
    echo 'FAIL: ambient-FD deletion mutant was not formed' >&2
    exit 1
fi
sed 's/ready.size() != kReadyMessageSize/false/g' "$impl" >"$mutant_ready"
if grep -F 'ready.size() != kReadyMessageSize' "$mutant_ready" >/dev/null; then
    echo 'FAIL: exact-READY deletion mutant was not formed' >&2
    exit 1
fi

# Every new incarnation/lease fence is load-bearing. Deleting any one exact
# source anchor must make the focused source gate red.
for pattern in \
    'LaunchIdentityAllocator::allocate()' \
    'config_.launch_identities->allocate()' \
    'std::string_view(ready).substr(0, prefix_bytes)' \
    'prefix.substr(0, prefix_bytes)' \
    'kReadyLeaseLivenessBarrierMilliseconds' \
    'child_has_exited_exact(expected_child)' \
    'SYS_pidfd_open' \
    'SYS_pidfd_send_signal' \
    'exact_child_handles_supported()' \
    'read_launch_permission(launch_gate[0])' \
    'make_launch_gate(launch_gate)' \
    'send_launch_permission(launch_gate[1])' \
    'MSG_NOSIGNAL' \
    'if (pidfd < 0)' \
    '::setsid()' \
    'write_session_marker(exec_pipe[1])' \
    'marker != kExecSessionMarker' \
    'child_owns_session(expected_child)' \
    'stop_child_handle(' \
    'signal_child_handle(pidfd, SIGSTOP)' \
    'constexpr idtype_t kPidfdIdType' \
    'WSTOPPED | WEXITED | WNOHANG | WNOWAIT' \
    'anchor == ChildAnchorResult::Stopped' \
    'child_is_in_group(expected_child, expected_group)' \
    'signal_child_handle(child_pidfd_, SIGKILL)' \
    'const bool proven_dead = terminate_child();' \
    'if (proven_dead) {' \
    'return direct_dead && group_dead;'; do
    lease_mutant="$mutant_dir/impl.cpp"
    awk -v needle="$pattern" 'index($0, needle) == 0' "$impl" >"$lease_mutant"
    if lease_gate "$lease_mutant" "$header"; then
        echo "FAIL: supervisor lease deletion mutant survived: $pattern" >&2
        exit 1
    fi
done

# Deletion-sensitive source mutants must not remove the atomic capture fence.
for pattern in \
    'SYS_renameat2' \
    'RENAME_NOREPLACE' \
    'AT_SYMLINK_NOFOLLOW' \
    'capture_and_remove_at'; do
    atomic_mutant="$mutant_dir/atomic-${pattern##*/}.cpp"
    grep -vF "$pattern" "$impl" >"$atomic_mutant"
    if lease_gate "$atomic_mutant" "$header"; then
        echo "FAIL: atomic lease deletion mutant survived: $pattern" >&2
        exit 1
    fi
done

for pattern in \
    'directory_device == 0' \
    'socket_path_digest != digest128(socket_path)'; do
    atomic_header_mutant="$mutant_dir/atomic-${pattern##*/}.h"
    grep -vF "$pattern" "$header" >"$atomic_header_mutant"
    if grep -F "$pattern" "$atomic_header_mutant" >/dev/null; then
        echo "FAIL: canonical lease deletion mutant was not formed: $pattern" >&2
        exit 1
    fi
done

for pattern in \
    'class LaunchIdentityAllocator' \
    'std::shared_ptr<LaunchIdentityAllocator> launch_identities' \
    'int child_pidfd_ = -1;'; do
    lease_mutant="$mutant_dir/header.h"
    awk -v needle="$pattern" 'index($0, needle) == 0' "$header" >"$lease_mutant"
    if lease_gate "$impl" "$lease_mutant"; then
        echo "FAIL: supervisor allocator deletion mutant survived: $pattern" >&2
        exit 1
    fi
done
echo 'ok - sidecar supervisor source gates hold'
