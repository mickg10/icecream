#!/bin/sh
# Real all-P50 terminal-disposition and InputRecord reclamation matrix.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_TOP_BUILDDIR:-$(CDPATH= cd -- "$src" && pwd)}
timeout_s=${ICECC_P50_C1F1_TIMEOUT:-180}

"$src/unittests/p50compilee2e-source.sh"
"$src/unittests/p50completionflow-source.py"

for binary in \
    "$build/daemon/iceccd" \
    "$build/scheduler/icecc-scheduler" \
    "$build/client/icecc" \
    "$build/cache/icecc-cache-service"; do
    test -x "$binary" || {
        echo "SKIP: missing built executable $binary" >&2
        exit 77
    }
done

for command in timeout g++ bash python3; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "SKIP: $command is required for the P50 completion-flow gate" >&2
        exit 77
    }
done

# The sidecar appends an identity-rich attempt leaf below this directory.
# Keep the socket namespace below sockaddr_un.sun_path even when the caller's
# TMPDIR is a long out-of-tree build path.
work=$(mktemp -d /tmp/p5c.XXXXXX)
cleanup() {
    for pid in "${service_pid:-}" "${client_service_pid:-}" "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
        test -n "$pid" && kill "$pid" 2>/dev/null || :
    done
    for _ in $(seq 1 50); do
        live=0
        for pid in "${service_pid:-}" "${client_service_pid:-}" "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
            if test -n "$pid" && kill -0 "$pid" 2>/dev/null; then
                live=1
            fi
        done
        test "$live" -eq 0 && break
        sleep 0.1
    done
    for pid in "${service_pid:-}" "${client_service_pid:-}" "${client_pid:-}" "${worker_pid:-}" "${sched_pid:-}"; do
        test -n "$pid" && kill -9 "$pid" 2>/dev/null || :
    done
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
    "$work/out" "$work/cache-runtime-f" "$work/cache-runtime-c" "$work/home" "$work/evidence"
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
    -s "127.0.0.1:$port_sched" -n "$network" -N p50-f \
    -b "$work/envs-f" -l "$work/f.log" -vvv \
    --cache-service "$build/cache/icecc-cache-service" \
    --cache-runtime-dir "$work/cache-runtime-f" &
worker_pid=$!

ICECC_TEST_SOCKET="$work/client.sock" ICECC_P50_C1F1_REQUIRED=1 \
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
            timeout "$timeout_s" "$build/client/icecc" g++ -std=c++17 -O2 -Wall -c \
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

echo "PASS: real P50 Accepted/DefinitiveCancel/malformed/disconnect lifecycle matrix"
