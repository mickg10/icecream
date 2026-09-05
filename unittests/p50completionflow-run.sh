#!/bin/sh
# Real all-P50 terminal-disposition and InputRecord reclamation matrix.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_TOP_BUILDDIR:-$(CDPATH= cd -- "$src" && pwd)}
timeout_s=${ICECC_P50_C1F1_TIMEOUT:-180}
worker_scheduler_host=${ICECC_P50_C1F1_WORKER_SCHEDULER_HOST:-}

# 127.0.0.1 is the protocol sentinel for a scheduler-selected local fallback.
# The real F in this single-network-namespace gate must register through an
# explicit ordinary address so its legitimate UseCS cannot alias that sentinel.
case "$worker_scheduler_host" in
""|localhost|127.*|0.0.0.0|::1)
    echo "SKIP: ICECC_P50_C1F1_WORKER_SCHEDULER_HOST must be an explicit non-loopback IPv4 address" >&2
    exit 77
    ;;
esac

"$src/unittests/p50compilee2e-source.sh"
"$src/unittests/p50completionflow-source.py"

for binary in \
    "$build/daemon/iceccd" \
    "$build/scheduler/icecc-scheduler" \
    "$build/client/icecc" \
    "$build/client/icecc-p50-completion-test" \
    "$build/cache/icecc-cache-service"; do
    test -x "$binary" || {
        echo "SKIP: missing built executable $binary" >&2
        exit 77
    }
done

for command in timeout g++ bash python3 strings readlink; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "SKIP: $command is required for the P50 completion-flow gate" >&2
        exit 77
    }
done

if ! python3 - "$worker_scheduler_host" <<'PY'
import ipaddress
import sys

try:
    address = ipaddress.IPv4Address(sys.argv[1])
except ipaddress.AddressValueError:
    raise SystemExit(1)
if (address.is_loopback or address.is_unspecified or address.is_multicast or
        address.is_reserved or address.is_link_local):
    raise SystemExit(1)
PY
then
    echo "SKIP: ICECC_P50_C1F1_WORKER_SCHEDULER_HOST is not an ordinary IPv4 address" >&2
    exit 77
fi

# The sidecar appends an identity-rich attempt leaf below this directory.
# Keep the socket namespace below sockaddr_un.sun_path even when the caller's
# TMPDIR is a long out-of-tree build path.
work=$(mktemp -d /tmp/p5c.XXXXXX)
cleanup() {
    for pid in "${retry_wrapper_pid:-}" "${service_pid:-}" "${client_service_pid:-}" "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
        test -n "$pid" && kill "$pid" 2>/dev/null || :
    done
    for _ in $(seq 1 50); do
        live=0
        for pid in "${retry_wrapper_pid:-}" "${service_pid:-}" "${client_service_pid:-}" "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
            if test -n "$pid" && kill -0 "$pid" 2>/dev/null; then
                live=1
            fi
        done
        test "$live" -eq 0 && break
        sleep 0.1
    done
    for pid in "${retry_wrapper_pid:-}" "${service_pid:-}" "${client_service_pid:-}" "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
        test -n "$pid" && kill -9 "$pid" 2>/dev/null || :
    done
    wait "${retry_wrapper_pid:-}" 2>/dev/null || :
    wait "${client_pid:-}" 2>/dev/null || :
    wait "${worker_pid:-}" 2>/dev/null || :
    wait "${sched_pid:-}" 2>/dev/null || :
    if test "${ICECC_P50_C1F1_KEEP_WORK:-0}" = 1; then
        echo "INFO: preserving P50 completion-flow workdir $work" >&2
    else
        rm -rf "$work"
    fi
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$work/envs-f" "$work/envs-c" "$work/toolchain" "$work/src" \
    "$work/out" "$work/cache-runtime-f" "$work/cache-runtime-c" "$work/home" \
    "$work/evidence" "$work/bin"
ln -s "$build/client/icecc-p50-completion-test" "$work/bin/icecc"
test "$(readlink -f "$work/bin/icecc")" = \
    "$(readlink -f "$build/client/icecc-p50-completion-test")" || {
    echo "FAIL: check-only icecc symlink does not resolve to the isolated test wrapper" >&2
    exit 1
}
chmod 1777 "$work/envs-f" "$work/envs-c"
chmod 0700 "$work/cache-runtime-f" "$work/cache-runtime-c" "$work/home"
HOME="$work/home"
export HOME
port_sched=$((24000 + ($$ % 1000)))
port_worker=$((25000 + ($$ % 1000)))
network="p50completion-$$"
experiment_id=${ICECC_P50_EXPERIMENT_ID:-p50completionflow}

printf '%s\n' \
    '#include <cstdint>' \
    'int p50_accepted_translation_unit() {' \
    '    return static_cast<int>(UINT32_C(50));' \
    '}' >"$work/src/accepted.cpp"
printf '%s\n' \
    '#include <cstdint>' \
    '[[deprecated("P50 definitive cancellation gate")]]' \
    'static int p50_deprecated_remote_warning() { return 51; }' \
    'int p50_definitive_translation_unit() {' \
    '    return p50_deprecated_remote_warning();' \
    '}' >"$work/src/definitive.cpp"
printf '%s\n' \
    '#include <cstdint>' \
    'int p50_malformed_translation_unit() {' \
    '    return static_cast<int>(UINT32_C(52));' \
    '}' >"$work/src/malformed.cpp"
printf '%s\n' \
    '#include <cstdint>' \
    'int p50_disconnect_translation_unit() {' \
    '    return static_cast<int>(UINT32_C(53));' \
    '}' >"$work/src/disconnect.cpp"
printf '%s\n' \
    '#include <cstdint>' \
    'int p50_fresh_local_retry_translation_unit() {' \
    '    return static_cast<int>(UINT32_C(54));' \
    '}' >"$work/src/fresh-local-retry.cpp"

(cd "$work/toolchain" && timeout "$timeout_s" \
    bash "$build/client/icecc-create-env" "$(command -v g++)" \
    >"$work/create-env.log" 2>&1)
envtar=$(find "$work/toolchain" -maxdepth 1 -type f -name '*.tar.gz' -print -quit)
test -n "$envtar" || {
    echo "FAIL: real icecc-create-env produced no compiler environment" >&2
    exit 1
}

set --
if test -n "${ICECC_TEST_DAEMON_UID:-}"; then
    daemon_gid=${ICECC_TEST_DAEMON_GID:-$ICECC_TEST_DAEMON_UID}
    chown -R "$ICECC_TEST_DAEMON_UID:$daemon_gid" "$work"
    set -- -u "$ICECC_TEST_DAEMON_UID"
fi

"$build/scheduler/icecc-scheduler" -p "$port_sched" -n "$network" \
    --assignment-fence-mode strict-nonce -l "$work/scheduler.log" -vvv &
sched_pid=$!
sleep 1
kill -0 "$sched_pid" 2>/dev/null || {
    echo "FAIL: real scheduler exited during startup" >&2
    exit 1
}

ICECC_TEST_SOCKET="$work/worker.sock" ICECC_P50_C1F1_REQUIRED=1 \
    ICECC_P50_TEST_LIFECYCLE_TRACE="$work/lifecycle.trace" \
    ICECC_P50_TEST_READY_TRACE="$work/ready.trace" \
    ICECC_P50_TEST_POST_TERMINAL_ATTACH=1 \
    "$build/daemon/iceccd" "$@" -p "$port_worker" -m 1 \
    -s "$worker_scheduler_host:$port_sched" -n "$network" -N p50-f \
    -b "$work/envs-f" -l "$work/f.log" -vvv \
    --cache-service "$build/cache/icecc-cache-service" \
    --cache-runtime-dir "$work/cache-runtime-f" &
worker_pid=$!

ICECC_TEST_SOCKET="$work/client.sock" \
    ICECC_P50_SOURCE_RESULT_TRACE="$work/source-result.jsonl" \
    "$build/daemon/iceccd" "$@" --no-remote -m 0 \
    -s "127.0.0.1:$port_sched" -n "$network" -N p50-c \
    -b "$work/envs-c" -l "$work/c.log" -vvv \
    --cache-service "$build/cache/icecc-cache-service" \
    --cache-runtime-dir "$work/cache-runtime-c" &
client_pid=$!

logins=0
for _ in $(seq 1 30); do
    logins=$(grep -c login "$work/scheduler.log" 2>/dev/null || true)
    test "${logins:-0}" -ge 2 && break
    sleep 1
done
test "${logins:-0}" -ge 2 || {
    echo "FAIL: real completion-flow daemons did not register" >&2
    exit 1
}

find_service_pid() {
    ps -eo pid=,ppid=,args= | \
        awk -v parent="$worker_pid" -v exe="$build/cache/icecc-cache-service" \
        '$2 == parent && index($0, exe) > 0 { print $1; exit }'
}

find_client_service_pid() {
    ps -eo pid=,ppid=,args= | \
        awk -v parent="$client_pid" -v exe="$build/cache/icecc-cache-service" \
        '$2 == parent && index($0, exe) > 0 { print $1; exit }'
}

service_pid=
for _ in $(seq 1 30); do
    service_pid=$(find_service_pid)
    test -n "$service_pid" && break
    sleep 1
done
test -n "$service_pid" || {
    echo "FAIL: production daemon did not start its cache sidecar" >&2
    exit 1
}

client_service_pid=
for _ in $(seq 1 30); do
    client_service_pid=$(find_client_service_pid)
    test -n "$client_service_pid" && break
    sleep 1
done
test -n "$client_service_pid" || {
    echo "FAIL: C daemon did not start its authenticated local cache sidecar" >&2
    exit 1
}

# The process can be alive before F has advertised its usable cache endpoint.
# Wait for the scheduler's cache-bearing relogin before submitting the first
# job so startup timing cannot strand an otherwise valid assignment.
cache_ready=0
for _ in $(seq 1 30); do
    if grep -E 'RELOGIN p50-f.*cache=.*cache_profiles=.*zstd_tu' \
        "$work/scheduler.log" >/dev/null 2>&1; then
        cache_ready=1
        break
    fi
    sleep 1
done
test "$cache_ready" -eq 1 || {
    echo "FAIL: production F cache endpoint was not advertised READY" >&2
    exit 1
}
grep -F "accepted $worker_scheduler_host" "$work/scheduler.log" >/dev/null || {
    echo "FAIL: scheduler did not accept F through the required ordinary address" >&2
    exit 1
}
grep -F "I am known as $worker_scheduler_host" "$work/f.log" >/dev/null || {
    echo "FAIL: F did not receive the required ordinary address from S" >&2
    exit 1
}

wait_for_count() {
    expected=$1
    pattern=$2
    file=$3
    observed=0
    for _ in $(seq 1 100); do
        observed=$(grep -E -c "$pattern" "$file" 2>/dev/null || true)
        test "${observed:-0}" -eq "$expected" && return 0
        sleep 0.1
    done
    echo "FAIL: expected $expected matches for $pattern in $file, got ${observed:-0}" >&2
    return 1
}

run_remote_cell() {
    cell=$1
    mode=$2
    remote_obj="$work/out/$cell-remote.o"
    local_obj="$work/out/$cell-local.o"
    client_log="$work/$cell-client.log"

    case "$mode" in
    accepted)
        ICECC_TEST_SOCKET="$work/client.sock" ICECC_TEST_REMOTEBUILD=1 \
            ICECC_VERSION="$envtar" ICECC_P50_C1F1_REQUIRED=1 \
            ICECC_P50_COMPILE_IDENTITY_TRACE="$work/compile-identity.jsonl" \
            ICECC_PREFERRED_HOST=p50-f ICECC_CARET_WORKAROUND=0 \
            ICECC_DEBUG=debug ICECC_LOGFILE="$client_log" \
            timeout "$timeout_s" "$build/client/icecc" g++ -std=c++17 -O2 -Wall -c \
            "$work/src/$cell.cpp" -o "$remote_obj"
        ;;
    definitive)
        ICECC_TEST_SOCKET="$work/client.sock" ICECC_TEST_REMOTEBUILD=1 \
            ICECC_VERSION="$envtar" ICECC_PREFERRED_HOST=p50-f \
            ICECC_P50_COMPILE_IDENTITY_TRACE="$work/compile-identity.jsonl" \
            ICECC_CARET_WORKAROUND=1 ICECC_DEBUG=debug \
            ICECC_LOGFILE="$client_log" \
            timeout "$timeout_s" "$build/client/icecc" g++ -std=c++17 -O2 -Wall -c \
            "$work/src/$cell.cpp" -o "$remote_obj"
        ;;
    malformed|disconnect)
        ICECC_TEST_SOCKET="$work/client.sock" ICECC_TEST_REMOTEBUILD=1 \
            ICECC_VERSION="$envtar" ICECC_P50_C1F1_REQUIRED=1 \
            ICECC_P50_COMPILE_IDENTITY_TRACE="$work/compile-identity.jsonl" \
            ICECC_PREFERRED_HOST=p50-f ICECC_CARET_WORKAROUND=0 \
            ICECC_P50_TEST_DISPOSITION="$mode" \
            ICECC_DEBUG=debug ICECC_LOGFILE="$client_log" \
            timeout "$timeout_s" "$work/bin/icecc" g++ -std=c++17 -O2 -Wall -c \
            "$work/src/$cell.cpp" -o "$remote_obj"
        ;;
    *)
        echo "FAIL: unknown completion-flow cell $mode" >&2
        return 1
        ;;
    esac

    g++ -std=c++17 -O2 -Wall -c "$work/src/$cell.cpp" -o "$local_obj" \
        2>"$work/$cell-local.err"
    cmp -s "$remote_obj" "$local_obj" || {
        echo "FAIL: $cell result differs from the exact local reference" >&2
        return 1
    }
    grep -E 'ZSTD_TU|CACHE_SESSION' "$client_log" "$work/c.log" \
        "$work/f.log" >/dev/null || {
        echo "FAIL: $cell has no positive P50 cache-path evidence" >&2
        return 1
    }
}

restart_cache_sidecar() {
    old_pid=$service_pid
    ready_before=$(grep -E -c \
        'RELOGIN p50-f.*cache=.*cache_profiles=.*zstd_tu' \
        "$work/scheduler.log" 2>/dev/null || true)
    kill -9 "$old_pid"
    replacement=
    for _ in $(seq 1 300); do
        replacement=$(find_service_pid)
        if test -n "$replacement" && test "$replacement" != "$old_pid"; then
            break
        fi
        sleep 0.1
    done
    test -n "$replacement" && test "$replacement" != "$old_pid" || {
        echo "FAIL: cache sidecar did not restart after ambiguous disposition" >&2
        return 1
    }
    if kill -0 "$old_pid" 2>/dev/null; then
        echo "FAIL: killed cache sidecar incarnation remains live" >&2
        return 1
    fi
    service_pid=$replacement

    replacement_ready=0
    for _ in $(seq 1 300); do
        ready_now=$(grep -E -c \
            'RELOGIN p50-f.*cache=.*cache_profiles=.*zstd_tu' \
            "$work/scheduler.log" 2>/dev/null || true)
        if test "${ready_now:-0}" -gt "${ready_before:-0}"; then
            replacement_ready=1
            break
        fi
        sleep 0.1
    done
    test "$replacement_ready" -eq 1 || {
        echo "FAIL: replacement cache sidecar did not advertise READY" >&2
        return 1
    }
}

run_remote_cell accepted accepted
wait_for_count 1 'action 2 status applied reason submitter accepted complete result' \
    "$work/f.log"

run_remote_cell definitive definitive
wait_for_count 1 'action 3 status applied reason submitter definitive cancellation' \
    "$work/f.log"
grep -E 'Error 102|stdout/stderr workaround|local build forced by remote exception' \
    "$work/definitive-client.log" >/dev/null || {
    echo "FAIL: warning cell did not exercise the caret definitive-cancel path" >&2
    exit 1
}

run_remote_cell malformed malformed
wait_for_count 1 'missing/mismatched P50 result disposition' "$work/f.log"
wait_for_count 1 'action 1 status applied reason handle_end' "$work/f.log"
grep -F 'P50 terminal test sending malformed disposition' \
    "$work/malformed-client.log" >/dev/null
restart_cache_sidecar

run_remote_cell disconnect disconnect
wait_for_count 2 'missing/mismatched P50 result disposition' "$work/f.log"
wait_for_count 2 'action 1 status applied reason handle_end' "$work/f.log"
grep -F 'P50 terminal test disconnecting before disposition' \
    "$work/disconnect-client.log" >/dev/null
restart_cache_sidecar

wait_for_count 4 \
    'P50 terminal test post-settlement attach job .* fd invalid' "$work/f.log"
if grep -E 'P50 terminal test post-settlement attach job .* status accepted|unexpectedly reattached consumed lease' \
    "$work/f.log" >/dev/null 2>&1; then
    echo "FAIL: a consumed terminal/attempt lease was reattached" >&2
    exit 1
fi

test -f "$work/lifecycle.trace" || {
    echo "FAIL: sidecar emitted no authoritative lifecycle trace" >&2
    exit 1
}
test "$(grep -c '^P50_LIFECYCLE ' "$work/lifecycle.trace")" -eq 4
test "$(grep -E -c ' action=2 status=0 before_records=1 before_bytes=[1-9][0-9]* after_records=0 after_bytes=0$' "$work/lifecycle.trace")" -eq 1
test "$(grep -E -c ' action=3 status=0 before_records=1 before_bytes=[1-9][0-9]* after_records=0 after_bytes=0$' "$work/lifecycle.trace")" -eq 1
test "$(grep -E -c ' action=1 status=0 before_records=1 before_bytes=[1-9][0-9]* after_records=1 after_bytes=[1-9][0-9]*$' "$work/lifecycle.trace")" -eq 2

attempt_only_pids=$(awk '$0 ~ / action=1 / { for (i=1; i<=NF; ++i) if ($i ~ /^pid=/) print $i }' \
    "$work/lifecycle.trace" | sort -u | wc -l)
attempt_only_attempts=$(awk '$0 ~ / action=1 / { for (i=1; i<=NF; ++i) if ($i ~ /^attempt=/) print $i }' \
    "$work/lifecycle.trace" | sort -u | wc -l)
test "$attempt_only_pids" -eq 2 || {
    echo "FAIL: malformed/disconnect did not run against distinct sidecar processes" >&2
    exit 1
}
test "$attempt_only_attempts" -eq 2 || {
    echo "FAIL: sidecar restart did not change the private store attempt" >&2
    exit 1
}

test "$(grep -E -c 'P50 input settlement job .* action 2 ' "$work/f.log")" -eq 1
test "$(grep -E -c 'P50 input settlement job .* action 3 ' "$work/f.log")" -eq 1
test "$(grep -E -c 'P50 input settlement job .* action 1 ' "$work/f.log")" -eq 2

python3 "$src/unittests/p50_runtime_evidence.py" \
    --experiment-id "$experiment_id" --run-id "$(basename "$work")" \
    --ready "$work/ready.trace" --lifecycle "$work/lifecycle.trace" \
    --worker-log "$work/f.log" \
    --identity-trace "$work/compile-identity.jsonl" \
    --output "$work/evidence/runtime.json" \
    >"$work/evidence/verification.json"
grep -F '"status":"HOLD"' "$work/evidence/verification.json" >/dev/null || {
    echo "FAIL: runtime evidence verifier did not retain incomplete rows as HOLD" >&2
    exit 1
}
test -s "$work/evidence/runtime.json"
python3 - "$work/evidence/runtime.json" <<'PY'
import json
import sys

document = json.load(open(sys.argv[1], encoding="utf-8"))
statuses = {row["field"]: row["status"] for row in document["identity_status"]}
if statuses != {"c_guid": "PASS", "tu_seq": "PASS"}:
    raise SystemExit(f"FAIL: compile identities are not PASS: {statuses}")
identities = document["runtime"]["compile_identity"]
if len(identities) != 4:
    raise SystemExit(f"FAIL: expected four compile identity records, got {len(identities)}")
verification = document["verification"]
if verification.get("status") != "HOLD" or verification.get("issues") != [
        "statistics_document_missing"]:
    raise SystemExit(f"FAIL: runtime evidence has unexpected remaining issues: {verification}")
PY

kill -0 "$service_pid" 2>/dev/null || {
    echo "FAIL: final replacement sidecar is not live" >&2
    exit 1
}

# Exercise the production retry loop through its real error boundary.  The
# check-only wrapper closes the completed result channel before Accepted and
# pauses.  That gives F time to publish its exact attempt-only terminal.  Only
# after S has consumed that terminal do we remove the sole F and release the
# wrapper, making its fresh canonical-absent request deterministically NoCS.
retry_marker="$work/fresh-retry.barrier"
retry_release="$retry_marker.release"
retry_trace="$work/fresh-retry-local.jsonl"
retry_identity="$work/fresh-retry-compile-identity.jsonl"
retry_client_log="$work/fresh-retry-client.log"
retry_remote_obj="$work/out/fresh-local-retry-remote.o"
retry_local_obj="$work/out/fresh-local-retry-local.o"

ICECC_TEST_SOCKET="$work/client.sock" \
    ICECC_VERSION="$envtar" ICECC_PREFERRED_HOST=p50-f \
    ICECC_CARET_WORKAROUND=0 ICECC_P50_TEST_FRESH_LEGACY_RETRY=1 \
    ICECC_P50_TEST_DISPOSITION=accepted-send-fail \
    ICECC_P50_TEST_FRESH_LEGACY_RETRY_BARRIER="$retry_marker" \
    ICECC_P50_TEST_FRESH_LEGACY_RETRY_TRACE="$retry_trace" \
    ICECC_P50_COMPILE_IDENTITY_TRACE="$retry_identity" \
    ICECC_DEBUG=debug ICECC_LOGFILE="$retry_client_log" \
    timeout "$timeout_s" "$work/bin/icecc" \
    g++ -std=c++17 -O2 -Wall -c "$work/src/fresh-local-retry.cpp" \
    -o "$retry_remote_obj" >"$work/fresh-retry.out" 2>&1 &
retry_wrapper_pid=$!

retry_armed=0
for _ in $(seq 1 300); do
    if test -s "$retry_marker"; then
        retry_armed=1
        break
    fi
    kill -0 "$retry_wrapper_pid" 2>/dev/null || break
    sleep 0.1
done
test "$retry_armed" -eq 1 || {
    echo "FAIL: fresh retry did not reach the post-output Accepted-send barrier" >&2
    exit 1
}

first_retry_job=$(python3 - "$retry_marker" <<'PY'
import json
import sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["job_id"])
PY
)
test -n "$first_retry_job"

# The newly received remote object exists now; remove it while the wrapper is
# paused so only the subsequent scheduler-local compiler can recreate the
# final witness.
test -s "$retry_remote_obj" || {
    echo "FAIL: first P50 attempt had not received the complete object" >&2
    exit 1
}
rm -f -- "$retry_remote_obj"

wait_for_count 3 'action 1 status applied reason handle_end' "$work/f.log"
wait_for_count 1 "END $first_retry_job status=0 .* server=p50-f" \
    "$work/scheduler.log"

# END is emitted only after the scheduler validates the complete terminal
# identity; its same handler then removes the exact job.  Remove F afterwards,
# wait until S has consumed the disconnect, and only then allow the wrapper's
# production 107 -> 106 path to issue its fresh GetCS.
old_worker_pid=$worker_pid
old_service_pid=$service_pid
kill -TERM "$old_worker_pid" 2>/dev/null || :
for _ in $(seq 1 100); do
    kill -0 "$old_worker_pid" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$old_worker_pid" 2>/dev/null; then
    kill -KILL "$old_worker_pid" 2>/dev/null || :
fi
wait "$old_worker_pid" 2>/dev/null || :
wait_for_count 1 'remove daemon p50-f' "$work/scheduler.log"

: >"$retry_release"
set +e
wait "$retry_wrapper_pid"
retry_rc=$?
set -e
retry_wrapper_pid=
test "$retry_rc" -eq 0 || {
    echo "FAIL: fresh retry wrapper exited $retry_rc" >&2
    exit 1
}

# The first object was unlinked at the barrier.  A nonempty exact result now
# therefore proves the decoded canonical-legacy CompileFile actually entered
# and completed the daemon-local compiler, not merely that stale bytes survived.
test -s "$retry_remote_obj" || {
    echo "FAIL: scheduler-local retry did not recreate the object" >&2
    exit 1
}
g++ -std=c++17 -O2 -Wall -c "$work/src/fresh-local-retry.cpp" \
    -o "$retry_local_obj" 2>"$work/fresh-retry-local.err"
cmp -s "$retry_remote_obj" "$retry_local_obj" || {
    echo "FAIL: fresh scheduler-local retry differs from exact local reference" >&2
    exit 1
}

grep -F 'P50 terminal test forcing Accepted send failure' \
    "$retry_client_log" >/dev/null
grep -F 'normalizing P50 client error 107 to Error 106 for a fresh assignment' \
    "$retry_client_log" >/dev/null
grep -F 'P50 assignment failed; requesting one fresh legacy remote assignment' \
    "$retry_client_log" >/dev/null
grep -F 'building myself, but telling localhost' "$retry_client_log" >/dev/null
if grep -F 'local build forced by remote exception' "$retry_client_log" >/dev/null; then
    echo "FAIL: fresh NoCS assignment escaped into the outer local fallback" >&2
    exit 1
fi

python3 - "$retry_marker" "$retry_trace" "$retry_identity" \
    "$work/source-result.jsonl" "$work/lifecycle.trace" \
    "$work/scheduler.log" "$work/c.log" <<'PY'
import json
import re
import sys

marker_path, local_path, identity_path, source_path, lifecycle_path, scheduler_path, daemon_path = sys.argv[1:]
first = json.load(open(marker_path, encoding="utf-8"))
local_rows = [json.loads(line) for line in open(local_path, encoding="utf-8") if line.strip()]
identity_rows = [json.loads(line) for line in open(identity_path, encoding="utf-8") if line.strip()]
if len(local_rows) != 1 or len(identity_rows) != 1:
    raise SystemExit(f"FAIL: retry traces are not singleton: local={len(local_rows)} identity={len(identity_rows)}")
second = local_rows[0]
remote_result = identity_rows[0]

if first.get("record") != "p50-accepted-send-failure" or first.get("input_present") != 1:
    raise SystemExit(f"FAIL: first marker is not a committed present input: {first}")
for field in ("job_id", "assignment_epoch", "assignment_nonce", "c_guid", "tu_seq"):
    if remote_result.get(field) != first.get(field):
        raise SystemExit(f"FAIL: first result/marker mismatch for {field}: {remote_result} vs {first}")
for field in ("job_id", "assignment_epoch", "assignment_nonce", "c_guid"):
    if not isinstance(first.get(field), int) or first[field] == 0:
        raise SystemExit(f"FAIL: first prepared identity has invalid {field}: {first}")
if first.get("input_profile", 0) == 0 or not isinstance(first.get("input_tu_seq"), int) or first["input_tu_seq"] < 0:
    raise SystemExit(f"FAIL: first CompileInputIdentity lacks its own TU identity: {first}")
if first.get("attempt_id") != first.get("assignment_nonce") or first.get("request_id") != first.get("assignment_nonce"):
    raise SystemExit(f"FAIL: first input is not bound to its prepared assignment: {first}")
if first.get("raw_bytes", 0) <= 0 or not re.fullmatch(r"[0-9a-f]{32}", first.get("raw_digest", "")):
    raise SystemExit(f"FAIL: first committed input lacks exact raw witness: {first}")
if not re.fullmatch(r"[0-9a-f]{32}", first.get("input_c_store_guid", "")):
    raise SystemExit(f"FAIL: first committed input lacks C-store identity: {first}")

source_rows = [json.loads(line) for line in open(source_path, encoding="utf-8") if line.strip()]
source_match = [row for row in source_rows
                if row.get("logical_job") == first["job_id"]
                and row.get("assignment_epoch") == first["assignment_epoch"]
                and row.get("assignment_nonce") == first["assignment_nonce"]]
if len(source_match) != 1:
    raise SystemExit(f"FAIL: first assignment has {len(source_match)} committed source witnesses")
source = source_match[0]
profile_names = {1: "P29V1", 2: "ZSTD_TU", 3: "ZSTD_ROUTE"}
if (source.get("wire_job_id") != first["job_id"] or source.get("status") != 0 or
        source.get("attempts", 0) < 1 or source.get("profile") != profile_names.get(first["input_profile"]) or
        source.get("c_store_guid") != first["input_c_store_guid"] or
        source.get("tu_seq") != first["input_tu_seq"] or
        source.get("raw_bytes") != first["raw_bytes"] or
        source.get("raw_digest") != first["raw_digest"]):
    raise SystemExit(f"FAIL: C-sidecar committed-source witness does not join the first marker: source={source} first={first}")
for field in ("source_mutex_wait_ns", "source_mutex_service_ns"):
    if type(source.get(field)) is not int or source[field] < 0:
        raise SystemExit(f"FAIL: committed-source mutex timing is invalid for {field}: {source}")
if source["source_mutex_service_ns"] == 0:
    raise SystemExit(f"FAIL: committed-source mutex service timing is empty: {source}")

if second.get("record") != "fresh-legacy-local-assignment":
    raise SystemExit(f"FAIL: missing scheduler-local retry record: {second}")
if second.get("job_id", 0) == 0 or second.get("job_id") == first.get("job_id"):
    raise SystemExit(f"FAIL: retry did not receive a fresh nonzero job id: first={first} second={second}")
if second.get("assignment_epoch") != 0 or second.get("assignment_nonce") != 0:
    raise SystemExit(f"FAIL: NoCS assignment fence is not canonical absence: {second}")
if second.get("c_guid", 0) == 0 or not isinstance(second.get("tu_seq"), int):
    raise SystemExit(f"FAIL: NoCS compile identity is incomplete: {second}")
if (second.get("c_guid"), second.get("tu_seq")) == (first.get("c_guid"), first.get("tu_seq")):
    raise SystemExit(f"FAIL: retry reused the first compile identity: first={first} second={second}")
if second.get("input_present") != 0 or second.get("input_profile") != 0:
    raise SystemExit(f"FAIL: retry CompileFile did not serialize canonical input absence: {second}")
if second.get("cache_protocol") != 0 or second.get("cache_profile_mask") != 0:
    raise SystemExit(f"FAIL: NoCS relay retained a cache tail: {second}")
if (second.get("hostname") != "127.0.0.1" or
        type(second.get("port")) is not int or second["port"] != 0):
    raise SystemExit(f"FAIL: retry was not daemon-created localhost UseCS: {second}")

lifecycle = open(lifecycle_path, encoding="utf-8").read()
life = re.findall(
    r"^P50_LIFECYCLE .* job=(\d+) epoch=(\d+) nonce=(\d+) c_store_guid=([0-9a-f]{32}) input_tu=(\d+) .* action=1 status=0 ",
    lifecycle, re.M)
expected_first = (str(first["job_id"]), str(first["assignment_epoch"]),
                  str(first["assignment_nonce"]), first["input_c_store_guid"],
                  str(first["input_tu_seq"]))
if expected_first not in life:
    raise SystemExit(f"FAIL: F did not settle the first exact assignment attempt-only: expected={expected_first} rows={life}")

scheduler = open(scheduler_path, encoding="utf-8").read()
scheduler_payload = "\n".join(
    re.sub(r"^\[\d+\] \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}: ", "", line)
    for line in scheduler.splitlines())
new_ids = [int(value) for value in re.findall(
    r"^NEW (\d+) .*fresh-local-retry\.cpp", scheduler_payload, re.M)]
if new_ids != [first["job_id"], second["job_id"]]:
    raise SystemExit(f"FAIL: scheduler did not mint exactly the traced fresh pair: {new_ids}")
if not re.search(rf"^END {first['job_id']} status=0 .* server=p50-f$", scheduler_payload, re.M):
    raise SystemExit("FAIL: scheduler did not consume F's exact first terminal")
if not re.search(rf"^END {second['job_id']} status=0 .* server=p50-c$", scheduler_payload, re.M):
    raise SystemExit("FAIL: scheduler did not consume the local retry JobDone")

daemon = open(daemon_path, encoding="utf-8").read()
decoded = re.search(
    rf"legacy CompileFile admitted canonical input for job {second['job_id']} epoch (\d+) nonce (\d+) c_guid (\d+) tu_seq (\d+)",
    daemon)
if not decoded:
    raise SystemExit("FAIL: C daemon did not decode/admit the retry CompileFile as canonical legacy")
decoded_tuple = tuple(map(int, decoded.groups()))
expected_tuple = (second["assignment_epoch"], second["assignment_nonce"], second["c_guid"], second["tu_seq"])
if decoded_tuple != expected_tuple:
    raise SystemExit(f"FAIL: decoded local identity mismatch: {decoded_tuple} != {expected_tuple}")
PY

# The production wrapper must contain none of the disposition mutation seams
# or their trace controls.  Only the check-only wrapper may expose them.
for selector in \
    ICECC_P50_TEST_DISPOSITION \
    ICECC_P50_TEST_FRESH_LEGACY_RETRY \
    'P50 terminal test sending malformed disposition' \
    'P50 terminal test disconnecting before disposition'
do
    if strings "$build/client/icecc" | grep -F "$selector" >/dev/null; then
        echo "FAIL: completion fault seam leaked into production client: $selector" >&2
        exit 1
    fi
done
for selector in \
    ICECC_P50_TEST_DISPOSITION \
    ICECC_P50_TEST_FRESH_LEGACY_RETRY_BARRIER
do
    strings "$build/client/icecc-p50-completion-test" | \
        grep -F "$selector" >/dev/null || {
        echo "FAIL: check-only client lacks completion fault seam: $selector" >&2
        exit 1
    }
done

# The F was intentionally removed to force NoCS; its sidecar must not survive
# as an unowned process after the daemon exits.
for _ in $(seq 1 100); do
    kill -0 "$old_service_pid" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$old_service_pid" 2>/dev/null; then
    echo "FAIL: removed F left its cache sidecar live" >&2
    exit 1
fi
worker_pid=
service_pid=

echo "PASS: real P50 terminal lifecycle plus 107/106 fresh scheduler-local retry"
