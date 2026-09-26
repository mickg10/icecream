#!/bin/sh
# Real all-P50 C1F1 networked profile compile gate.
#
# Once p50compilee2e-source.sh is green this starts the actual built
# scheduler, one actual iceccd F, one actual iceccd C, and the actual
# icecc-cache-service owned by the daemon's sidecar adapter. The compiler
# invocation is required to produce positive selected-profile evidence and a
# byte-identical local reference. No fake peer or legacy FileChunk fallback
# is accepted.
set -eu
unset ICECC_TEST_P51_RESTART_W30_TOPOLOGY

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_TOP_BUILDDIR:-$(CDPATH= cd -- "$src" && pwd)}
timeout_s=${ICECC_P50_C1F1_TIMEOUT:-180}
profile_marker=${ICECC_P50_PROFILE:-ZSTD_ROUTE}
suite=${ICECC_P50_SUITE:-C1F1/100000}
warm=${ICECC_P50_C1F1_WARM:-0}
passes=${ICECC_P50_C1F1_PASSES:-2}
topology=${ICECC_P50_TOPOLOGY:-}
external_mode=${ICECC_P50_EXTERNAL_FARM:-0}
worker_scheduler_host=${ICECC_P50_C1F1_WORKER_SCHEDULER_HOST:-}
reference_reuse=0
reference_witness=${ICECC_P50_REFERENCE_WITNESS:-}
reference_authority=${ICECC_P50_REFERENCE_AUTHORITY:-}
predictive_plan=${ICECC_P50_PREDICTIVE_PLAN:-}
s2_process_loss=${ICECC_P50_S2_PROCESS_LOSS:-0}
real_scheduler_restart_w30=${ICECC_P50_C1F1_REAL_SCHEDULER_RESTART_W30:-0}
real_scheduler_f_restart_w30=${ICECC_P50_C1F1_REAL_SCHEDULER_F_RESTART_W30:-0}
worker_session_loss=${ICECC_P50_C1F1_TEST_WORKER_SESSION_LOSS:-0}
expect_stable_f=${ICECC_P50_C1F1_EXPECT_STABLE_F:-0}
case "$external_mode" in
    0|1) ;;
    *) echo "FAIL: ICECC_P50_EXTERNAL_FARM must be 0 or 1" >&2; exit 1 ;;
esac
if test "$external_mode" = 0; then
    # 127.0.0.1 is the protocol sentinel for scheduler-selected local
    # fallback.  A real F sharing this gate's network namespace must register
    # through an explicit ordinary address so its UseCS cannot alias it.
    case "$worker_scheduler_host" in
        ""|localhost|127.*|0.0.0.0|::1)
            echo "SKIP: ICECC_P50_C1F1_WORKER_SCHEDULER_HOST must be an explicit non-loopback IPv4 address" >&2
            exit 77
            ;;
    esac
fi
case "$s2_process_loss" in
    0|1) ;;
    *) echo "FAIL: ICECC_P50_S2_PROCESS_LOSS must be 0 or 1" >&2; exit 1 ;;
esac
case "$real_scheduler_restart_w30" in
    0|1) ;;
    *) echo "FAIL: ICECC_P50_C1F1_REAL_SCHEDULER_RESTART_W30 must be 0 or 1" >&2; exit 1 ;;
esac
case "$real_scheduler_f_restart_w30" in
    0|1) ;;
    *) echo "FAIL: ICECC_P50_C1F1_REAL_SCHEDULER_F_RESTART_W30 must be 0 or 1" >&2; exit 1 ;;
esac
case "$worker_session_loss" in
    0|1) ;;
    *) echo "FAIL: ICECC_P50_C1F1_TEST_WORKER_SESSION_LOSS must be 0 or 1" >&2; exit 1 ;;
esac
case "$expect_stable_f" in
    0|1) ;;
    *) echo "FAIL: ICECC_P50_C1F1_EXPECT_STABLE_F must be 0 or 1" >&2; exit 1 ;;
esac
if test "$expect_stable_f" = 1 && test "$worker_session_loss" != 1; then
    echo "FAIL: stable-F expectation requires the worker-session-loss scenario" >&2
    exit 1
fi
if test "$worker_session_loss" = 1; then
    if test "$real_scheduler_restart_w30" = 1 || \
            test "$real_scheduler_f_restart_w30" = 1; then
        echo "FAIL: focused worker-session-loss mode cannot be combined with either scheduler-restart mode" >&2
        exit 1
    fi
fi
if test "$real_scheduler_f_restart_w30" = 1; then
    real_scheduler_restart_w30=1
fi
case "$suite" in
    C1F1/100000) relationship_count=1; slots_per_f=1; execution_slots=1 ;;
    C1F20/40) relationship_count=20; slots_per_f=2; execution_slots=40 ;;
    *) echo "FAIL: ICECC_P50_SUITE must be C1F1/100000 or C1F20/40" >&2; exit 1 ;;
esac
if test "$suite" = C1F20/40 && test -z "$topology"; then
    echo "FAIL: C1F20/40 requires an authenticated topology" >&2
    exit 1
fi
cache_enabled=1
case "$profile_marker" in
    RAW_II)
        cache_enabled=0
        profile_advertisement=none
        ;;
    P29V1) profile_advertisement=p29v1 ;;
    ZSTD_TU) profile_advertisement=zstd_tu ;;
    ZSTD_ROUTE) profile_advertisement=zstd_route ;;
    *)
        echo "FAIL: ICECC_P50_PROFILE must be P29V1, ZSTD_TU, ZSTD_ROUTE, or RAW_II" >&2
        exit 1
        ;;
esac
if test "$s2_process_loss" = 1; then
    if test "$external_mode" != 1 || test "$suite" != C1F1/100000 || \
            test "$cache_enabled" -ne 1; then
        echo "FAIL: S2 process-loss gate requires external cache-enabled C1F1" >&2
        exit 1
    fi
    test -n "${ICECC_P50_EXTERNAL_S2_HOOK:-}" || {
        echo "FAIL: S2 process-loss external hook is required" >&2
        exit 1
    }
fi
if test "$real_scheduler_restart_w30" = 1; then
    if test "$external_mode" != 0 || test "$suite" != C1F1/100000 || \
            test "$cache_enabled" -ne 1 || test "$warm" != 0 || test "$passes" != 1; then
        echo "FAIL: real scheduler W30 restart requires local cache-enabled C1F1, WARM=0, PASSES=1" >&2
        exit 1
    fi
    slots_per_f=30
    execution_slots=30
    # The daemon's production scheduler reconnect backoff is 20–51 seconds.
    # This fixture exercises replacement, not that production latency; use
    # the daemon's test-only 3-second reconnect cadence so the unchanged
    # per-job deadline remains the limiting bound.
    export ICECC_TESTS=1
fi
if test "$worker_session_loss" = 1 && \
        { test "$external_mode" != 0 || test "$suite" != C1F1/100000 || \
          test "$cache_enabled" -ne 1 || test "$warm" != 0 || test "$passes" != 1; }; then
    echo "FAIL: focused worker-session-loss requires local P51 C1F1, WARM=0, PASSES=1" >&2
    exit 1
fi
if test "$worker_session_loss" = 1; then
    # Keep this bounded focused probe on production one-slot capacity; it is
    # not the separate pending-window W30 scheduler-replacement scenario.
    export ICECC_TESTS=1
fi
worker_maxjobs=$slots_per_f
if test "$real_scheduler_restart_w30" = 1; then
    # The scheduler reserves one dispatch-credit slot: when the worker's
    # advertised farm capacity is N, effective_dispatch_credit() admits at
    # most N-1 outstanding assignments.  Advertise one spare compile slot so
    # this test can hold the full 30-receipt R2 window without weakening its
    # protocol/window assertion.
    worker_maxjobs=$((slots_per_f + 1))
    echo "S8_REAL_SCHEDULER_DISPATCH_CREDIT worker_maxjobs=$worker_maxjobs receipt_window=$slots_per_f"
fi
if test "$worker_session_loss" = 1; then
    # This is test-only child identity evidence; it does not alter the daemon's
    # compile/retry behavior.  The matching marker is emitted after PGID
    # ownership has been checked and before the child is registered.
    export ICECC_P50_DEBUG_ATTACH=1
fi
if test "$cache_enabled" -eq 0; then
    unset ICECC_P50_C1F1_REQUIRED
fi
case "$warm" in
    0|1) ;;
    *)
        echo "FAIL: ICECC_P50_C1F1_WARM must be 0 or 1" >&2
        exit 1
        ;;
esac
case "$passes" in
    1|2) ;;
    *)
        echo "FAIL: ICECC_P50_C1F1_PASSES must be 1 or 2" >&2
        exit 1
        ;;
esac

set +e
"$src/unittests/p50compilee2e-source.sh"
rc=$?
set -e
test "$rc" -eq 0 || exit "$rc"

for binary in \
    "$build/daemon/iceccd" \
    "$build/scheduler/icecc-scheduler" \
    "$build/client/icecc"; do
    test -x "$binary" || {
        echo "SKIP: missing built executable $binary" >&2
        exit 77
    }
done
if test "$cache_enabled" -eq 1 && test ! -x "$build/cache/icecc-cache-service"; then
    echo "SKIP: missing built executable $build/cache/icecc-cache-service" >&2
    exit 77
fi
if test "$real_scheduler_restart_w30" = 1; then
    test -x "$build/unittests/p50daemonpositive" || {
        echo "SKIP: missing built P51 receipt-gate helper $build/unittests/p50daemonpositive" >&2
        exit 77
    }
    test -n "${ICECC_TEST_DAEMON_UID:-}" || {
        echo "FAIL: real scheduler W30 gate requires ICECC_TEST_DAEMON_UID account" >&2
        exit 1
    }
    daemon_passwd_entry=$(getent passwd "$ICECC_TEST_DAEMON_UID" || true)
    test -n "$daemon_passwd_entry" || {
        echo "FAIL: ICECC_TEST_DAEMON_UID does not resolve to a daemon account" >&2
        exit 1
    }
    daemon_account=$(printf '%s\n' "$daemon_passwd_entry" | cut -d: -f1)
    daemon_uid=$(printf '%s\n' "$daemon_passwd_entry" | cut -d: -f3)
    test -n "$daemon_account" && test "$daemon_uid" -gt 0 || {
        echo "FAIL: real scheduler W30 gate requires a non-root daemon account" >&2
        exit 1
    }
fi

command -v timeout >/dev/null 2>&1 || {
    echo "SKIP: timeout(1) is required for bounded C1F1 cleanup" >&2
    exit 77
}
command -v g++ >/dev/null 2>&1 || {
    echo "SKIP: g++ is required for the C1F1 compile" >&2
    exit 77
}
command -v bash >/dev/null 2>&1 || {
    echo "SKIP: bash is required for the generated icecc-create-env tool" >&2
    exit 77
}
command -v python3 >/dev/null 2>&1 || {
    echo "SKIP: python3 is required to validate the worker scheduler address" >&2
    exit 77
}
if test "$external_mode" = 0 && ! python3 - "$worker_scheduler_host" <<'PY'
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

if test -n "${ICECC_P50_C1F1_WORKDIR:-}"; then
    work=$ICECC_P50_C1F1_WORKDIR
    case "$work" in
        /*/p50compilee2e.*|/tmp/s4-p50-fourhost-client.*)
            ;;
        *) echo "FAIL: supplied workdir must be an absolute private p50compilee2e path" >&2; exit 1 ;;
    esac
    if test "$external_mode" = 1; then
        test -d "$work" && test ! -L "$work" || { echo "FAIL: external workdir is unavailable" >&2; exit 1; }
    else
        test ! -e "$work" || { echo "FAIL: supplied workdir already exists" >&2; exit 1; }
        mkdir -p "$work"
    fi
else
    # The sidecar's attempt leaf adds 43 characters and cache.sock adds 11;
    # a long build-system TMPDIR can therefore exceed sockaddr_un.sun_path
    # before the test starts. Keep this real-process socket namespace short;
    # large farm corpora/build outputs use their separately managed scratch.
    work=$(CDPATH= cd -- "$(mktemp -d /tmp/p5e.XXXXXX)" && pwd)
fi
# The scheduler changes to its configured service account before opening the
# requested log.  Keep the test root traversable and pre-create only that log
# as writable; the cache runtime and HOME below retain their own 0700 modes.
chmod 0711 "$work"
if test "$external_mode" = 1; then
    test -f "$work/scheduler.log" && test ! -L "$work/scheduler.log" || {
        echo "FAIL: external scheduler log is unavailable" >&2; exit 1;
    }
else
    : >"$work/scheduler.log"
fi
chmod 0666 "$work/scheduler.log"
if test "$external_mode" = 1; then
    test -n "${ICECC_P50_EXTERNAL_EXECUTION_ID:-}" &&
        test -n "${ICECC_P50_EXTERNAL_MANIFEST_SHA256:-}" &&
        test -n "${ICECC_P50_EXTERNAL_AUTHORITY_SHA256:-}" &&
        test -n "${ICECC_P50_EXTERNAL_START_UTC:-}" || {
        echo "FAIL: external lifecycle identity is incomplete" >&2; exit 1;
    }
    printf 'S8_EXTERNAL_FARM_EXECUTION execution_id=%s manifest_sha256=%s authority_sha256=%s phase=start timestamp=%s\n' \
        "$ICECC_P50_EXTERNAL_EXECUTION_ID" "$ICECC_P50_EXTERNAL_MANIFEST_SHA256" \
        "$ICECC_P50_EXTERNAL_AUTHORITY_SHA256" "$ICECC_P50_EXTERNAL_START_UTC"
fi
cleanup() {
    # Batch wrappers own their compiler child and remove their planned-lane
    # marker from an EXIT trap.  Stop them before the daemons so an interrupted
    # parallel batch cannot strand a compiler or a lane lease.
    # The receipt proxy owns a namespace-local iptables REDIRECT rule.  Ask it
    # to stop through its control directory first so its destructor removes
    # that exact rule before we escalate to signals.
    if test -n "${receipt_gate_pid:-}" && kill -0 "$receipt_gate_pid" 2>/dev/null; then
        if test -n "${receipt_gate_dir:-}" && test -d "$receipt_gate_dir"; then
            abort_tmp="$receipt_gate_dir/.abort-$$"
            printf 'abort\n' >"$abort_tmp" && mv -f "$abort_tmp" "$receipt_gate_dir/abort" || {
                rm -f "$abort_tmp"
                echo "WARN: could not publish receipt-gate abort marker" >&2
            }
            for _ in $(seq 1 50); do
                kill -0 "$receipt_gate_pid" 2>/dev/null || break
                sleep 0.1
            done
        fi
        if kill -0 "$receipt_gate_pid" 2>/dev/null; then
            kill "$receipt_gate_pid" 2>/dev/null || :
            for _ in $(seq 1 20); do
                kill -0 "$receipt_gate_pid" 2>/dev/null || break
                sleep 0.1
            done
        fi
        if kill -0 "$receipt_gate_pid" 2>/dev/null; then
            kill -9 "$receipt_gate_pid" 2>/dev/null || :
        fi
        wait "$receipt_gate_pid" 2>/dev/null || :
        receipt_gate_pid=
    fi
    cleanup_pids="${batch_job_pids:-} ${receipt_gate_pid:-} ${s2_compile_pid:-} ${service_pid:-} ${client_service_pid:-} ${client_pid:-} ${worker_pids:-} ${service_pids:-} ${worker_pid:-} ${sched_pid:-}"
    for pid in $cleanup_pids; do
        test -n "$pid" && kill "$pid" 2>/dev/null || :
    done
    for _ in $(seq 1 50); do
        live=0
        for pid in $cleanup_pids; do
            if test -n "$pid" && kill -0 "$pid" 2>/dev/null; then
                live=1
            fi
        done
        test "$live" -eq 0 && break
        sleep 0.1
    done
    for pid in $cleanup_pids; do
        test -n "$pid" && kill -9 "$pid" 2>/dev/null || :
    done
    for pid in $cleanup_pids; do
        test -n "$pid" && wait "$pid" 2>/dev/null || :
    done
    if test "${ICECC_P50_C1F1_KEEP_WORK:-0}" = 1; then
        echo "INFO: preserving P50 C1F1 workdir $work" >&2
    else
        rm -rf "$work"
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$work/envs-f" "$work/envs-c" "$work/toolchain" "$work/src" "$work/out" \
    "$work/cache-runtime-f" "$work/cache-runtime-c" "$work/home"
if test "$cache_enabled" -eq 1 && test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        mkdir -p "$work/envs-f-$relationship"
        mkdir -p "$work/cache-runtime-f-$relationship"
    done
elif test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        mkdir -p "$work/envs-f-$relationship"
    done
fi
chmod 1777 "$work/envs-f" "$work/envs-c"
chmod 0700 "$work/cache-runtime-f" "$work/cache-runtime-c" "$work/home"
if test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        chmod 1777 "$work/envs-f-$relationship"
        if test "$cache_enabled" -eq 1; then
            chmod 0700 "$work/cache-runtime-f-$relationship"
        fi
    done
fi
HOME="$work/home"
export HOME
# Action sinks are role-labelled absolute files.  The production daemons
# inherit these existing sinks; the warm runner later attributes TU0/TU1 by
# their captured session/transaction boundaries.
c_action_trace="$work/s7-warm-c-action-trace.jsonl"
f_action_trace="$work/s7-warm-f-action-trace.jsonl"
source_result_trace="$work/p50-source-result-trace.jsonl"
c_legacy_wire_trace="$work/s7-measured-c-legacy-wire-trace.jsonl"
f_legacy_wire_trace="$work/s7-measured-f-legacy-wire-trace.jsonl"
scheduler_current_log="$work/scheduler.log"
s2_external_evidence="$work/external-s2-process-loss.json"
if test "$s2_process_loss" = 1; then
    test ! -e "$s2_external_evidence" || {
        echo "FAIL: S2 process-loss evidence path is not fresh" >&2
        exit 1
    }
fi
pick_port_pair() {
    python3 - "$suite" <<'PY'
import secrets
import socket
import sys

suite = sys.argv[1]

start = 40000 + 2 * secrets.randbelow(9000)
for offset in range(0, 10000, 2):
    base = start + offset
    if base + 1 >= 60000:
        base -= 18000
    sockets = []
    try:
        # The scheduler owns TCP base + 1 for its text endpoint; C and F use
        # separate ports reserved before any role starts.
        # Reserve every relationship's port before any daemon starts so the
        # twenty persistent F identities cannot partially overlap another run.
        ports = (base, base + 1, base + 2)
        if suite == "C1F20/40":
            ports += tuple(base + 3 + 2 * relationship for relationship in range(20))
        else:
            ports += (base + 3,)
        for port in ports:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(('0.0.0.0', port))
            sockets.append(sock)
    except OSError:
        for sock in sockets:
            sock.close()
        continue
    for sock in sockets:
        sock.close()
    print(base, base + 3)
    break
else:
    raise SystemExit('no available scheduler/worker port pair')
PY
}
if test "$external_mode" = 1; then
    port_sched=${ICECC_P50_EXTERNAL_SCHED_PORT:-}
    port_worker=${ICECC_P50_EXTERNAL_WORKER_PORT:-$((port_sched + 3))}
    test -n "$port_sched" || { echo "FAIL: external scheduler port required" >&2; exit 1; }
    test -n "${ICECC_P50_EXTERNAL_SCHEDULER_LOG:-}" || {
        echo "FAIL: external scheduler log required" >&2; exit 1;
    }
elif test -n "${ICECC_P50_C1F1_SCHED_PORT:-}" || test -n "${ICECC_P50_C1F1_WORKER_PORT:-}"; then
    port_sched=${ICECC_P50_C1F1_SCHED_PORT:-}
    port_worker=${ICECC_P50_C1F1_WORKER_PORT:-}
else
    ports=$(pick_port_pair)
    port_sched=${ports%% *}
    port_worker=${ports##* }
fi
test -n "$port_sched" && test -n "$port_worker" || {
    echo "FAIL: scheduler and worker ports must be specified together" >&2
    exit 1
}
test "$port_sched" != "$port_worker" || {
    echo "FAIL: scheduler and worker ports must be distinct" >&2
    exit 1
}
port_client=$((port_sched + 2))
test "$port_worker" -ne "$port_client" || {
    echo "FAIL: worker port collides with scheduler text endpoint" >&2
    exit 1
}
network="p50c1f1-$$"

# A corpus cell may provide an exact input from an authenticated source root.
# Keep root and relative path separate so the input cannot accidentally be
# resolved against the build tree or replaced by the generated smoke TU.
source_root=${ICECC_P50_C1F1_SOURCE_ROOT:-}
source_relative=${ICECC_P50_C1F1_SOURCE_RELATIVE:-}
source_input=${ICECC_P50_C1F1_INPUT:-}
include_root=${ICECC_P50_C1F1_INCLUDE_ROOT:-}
compile_db=${ICECC_P50_C1F1_COMPILE_DB:-}
compile_source=${ICECC_P50_C1F1_COMPILE_SOURCE:-}
batch_manifest=${ICECC_P50_C1F1_BATCH_MANIFEST:-}
real_scheduler_w30_rows=60
if test "$real_scheduler_f_restart_w30" = 1; then
    real_scheduler_w30_rows=90
fi
if test "$real_scheduler_restart_w30" = 1 && test -z "$batch_manifest"; then
    echo "FAIL: real scheduler W30 restart requires a validated ${real_scheduler_w30_rows}-TU batch manifest" >&2
    exit 1
fi
if test -n "$batch_manifest"; then
    test -z "$source_root" && test -z "$source_relative" && test -z "$source_input" || {
        echo "FAIL: batch manifest cannot be combined with single-input fields" >&2
        exit 1
    }
    test -f "$batch_manifest" && test ! -L "$batch_manifest" || {
        echo "FAIL: authenticated batch manifest is unavailable" >&2
        exit 1
    }
elif test -n "$source_root" || test -n "$source_relative"; then
    test -n "$source_root" && test -n "$source_relative" || {
        echo "FAIL: source root and source relative path must be supplied together" >&2
        exit 1
    }
    test "${source_root#/}" != "$source_root" || {
        echo "FAIL: source root must be an absolute path" >&2
        exit 1
    }
    case "$source_relative" in
        ''|/*|../*|*/../*|*/..)
            echo "FAIL: source relative path escapes its authenticated root" >&2
            exit 1
            ;;
    esac
    test -d "$source_root" && test ! -L "$source_root" || {
        echo "FAIL: authenticated source root is unavailable" >&2
        exit 1
    }
    source_input="$source_root/$source_relative"
fi
if test -z "$batch_manifest" && test -n "$source_input"; then
    test -f "$source_input" && test ! -L "$source_input" || {
        echo "FAIL: authenticated source input is unavailable" >&2
        exit 1
    }
    cp -- "$source_input" "$work/src/main.cpp"
elif test -z "$batch_manifest"; then
    printf '%s\n' \
        '#include <cstdint>' \
        'int p50_c1f1_translation_unit() {' \
        '    return static_cast<int>(UINT32_C(50));' \
        '}' >"$work/src/main.cpp"
fi
if test -n "$batch_manifest"; then
    # Validate every source snapshot before starting a daemon.  The TSV is
    # only a transport for authenticated paths; all observations below are
    # still emitted by the product lifecycle after each real compile.
    python3 - "$batch_manifest" >"$work/batch.tsv" <<'PY'
import hashlib, json, os, stat, sys
path = sys.argv[1]
with open(path, encoding="utf-8") as stream:
    rows = [json.loads(line) for line in stream if line.strip()]
if not rows:
    raise SystemExit("empty batch manifest")
seen = set()
for row in rows:
    if not isinstance(row, dict) or set(row) - {"tu_id", "source", "source_relative", "sha256", "predictive_input", "compile_db", "compile_db_sha256", "compile_source", "compile_output"}:
        raise SystemExit("batch manifest fields invalid")
    tu, source, expected = row.get("tu_id"), row.get("source"), row.get("sha256")
    source_relative = row.get("source_relative")
    predictive = row.get("predictive_input")
    if (not isinstance(predictive, dict) or
            set(predictive) != {"ordinal", "path", "source_relative", "sha256", "bytes"} or
            predictive.get("ordinal") != len(seen) or
            not isinstance(predictive.get("path"), str) or not os.path.isabs(predictive["path"]) or
            not isinstance(predictive.get("source_relative"), str) or not predictive["source_relative"] or
            predictive["source_relative"].startswith("/") or
            any(part in ("", ".", "..") for part in predictive["source_relative"].split("/")) or
            not isinstance(predictive.get("sha256"), str) or len(predictive["sha256"]) != 64 or
            any(char not in "0123456789abcdef" for char in predictive["sha256"]) or
            type(predictive.get("bytes")) is not int or predictive["bytes"] <= 0):
        raise SystemExit("batch predictive payload descriptor invalid")
    payload_sha, payload_bytes = predictive["sha256"], predictive["bytes"]
    predictive_info = os.lstat(predictive["path"])
    if (stat.S_ISLNK(predictive_info.st_mode) or not stat.S_ISREG(predictive_info.st_mode) or
            predictive_info.st_nlink != 1 or hashlib.sha256(open(predictive["path"], "rb").read()).hexdigest() != payload_sha or
            predictive_info.st_size != payload_bytes):
        raise SystemExit("batch predictive payload unavailable or changed")
    if (not isinstance(tu, str) or not tu or tu in seen or "\t" in tu or
            any(char not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-" for char in tu)):
        raise SystemExit("batch TU identity invalid")
    if (not isinstance(source_relative, str) or not source_relative or source_relative.startswith("/") or
            any(part in ("", ".", "..") for part in source_relative.split("/"))):
        raise SystemExit("batch source-relative identity invalid")
    if not isinstance(source, str) or not os.path.isabs(source) or "\t" in source:
        raise SystemExit("batch source must be absolute")
    try:
        info = os.lstat(source)
    except OSError:
        raise SystemExit("batch source unavailable")
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise SystemExit("batch source is not a private regular file")
    actual = hashlib.sha256(open(source, "rb").read()).hexdigest()
    if not isinstance(expected, str) or expected != actual:
        raise SystemExit("batch source digest mismatch")
    db = row.get("compile_db", "")
    db_sha = row.get("compile_db_sha256", "")
    compile_source = row.get("compile_source", source)
    compile_output = row.get("compile_output", "")
    compile_values = (db, db_sha, row.get("compile_source", ""), compile_output)
    if any(compile_values) and not all(compile_values):
        raise SystemExit("batch compile binding incomplete")
    if db or compile_source != source:
        if not isinstance(db, str) or not os.path.isabs(db) or not os.path.isfile(db) or os.path.islink(db):
            raise SystemExit("batch compile database unavailable")
        if (not isinstance(db_sha, str) or len(db_sha) != 64 or
                any(char not in "0123456789abcdef" for char in db_sha) or
                hashlib.sha256(open(db, "rb").read()).hexdigest() != db_sha):
            raise SystemExit("batch compile database digest mismatch")
        if (not isinstance(compile_source, str) or not os.path.isabs(compile_source) or
                compile_source != source or not isinstance(compile_output, str) or
                not os.path.isabs(compile_output)):
            raise SystemExit("batch compile source/output binding invalid")
    # `read` below treats tab as IFS whitespace, which collapses adjacent
    # delimiters.  Keep optional columns nonempty so an absent compile-database
    # binding cannot shift the source path into the database field.
    compile_binding = (db, db_sha, compile_source, compile_output) if db else ("-", "-", "-", "-")
    print("\t".join((tu, source, source_relative, actual, predictive["path"], predictive["source_relative"], payload_sha, str(payload_bytes), *compile_binding)))
    seen.add(tu)
PY
    batch_expected_count=${ICECC_P50_C1F1_EXPECTED_COUNT:-}
    test "$batch_expected_count" -gt 0 2>/dev/null || {
        echo "FAIL: batch expected count is required" >&2
        exit 1
    }
    batch_actual_count=$(wc -l <"$work/batch.tsv")
    test "$batch_actual_count" -eq "$batch_expected_count" || {
        echo "FAIL: batch manifest count does not match selected depth" >&2
        exit 1
    }
    if test "$real_scheduler_restart_w30" = 1 || test "$worker_session_loss" = 1; then
        if test "$worker_session_loss" = 1; then
            test "$batch_expected_count" -eq 2 || {
                echo "FAIL: focused worker-session-loss probe requires exactly two distinct TUs" >&2
                exit 1
            }
            sed -n '1p' "$work/batch.tsv" >"$work/batch-old-scheduler.tsv"
            sed -n '2p' "$work/batch.tsv" >"$work/batch-new-scheduler.tsv"
        else
            test "$batch_expected_count" -eq "$real_scheduler_w30_rows" || {
                echo "FAIL: real scheduler W30 restart requires exactly $real_scheduler_w30_rows distinct TUs" >&2
                exit 1
            }
            sed -n '1,30p' "$work/batch.tsv" >"$work/batch-old-scheduler.tsv"
            sed -n '31,60p' "$work/batch.tsv" >"$work/batch-new-scheduler.tsv"
            test "$(wc -l <"$work/batch-old-scheduler.tsv")" -eq 30 && \
                test "$(wc -l <"$work/batch-new-scheduler.tsv")" -eq 30 || {
                echo "FAIL: real scheduler W30 batch partition is not exactly 30+30" >&2
                exit 1
            }
        fi
        if test "$real_scheduler_f_restart_w30" = 1; then
            sed -n '61,90p' "$work/batch.tsv" >"$work/batch-new-f.tsv"
            test "$(wc -l <"$work/batch-new-f.tsv")" -eq 30 || {
                echo "FAIL: ordered S-to-F W30 batch partition is not exactly 30+30+30" >&2
                exit 1
            }
        fi
    fi
fi
if test "$suite" = C1F20/40; then
    test -n "$batch_manifest" || {
        echo "FAIL: C1F20/40 requires an authenticated batch manifest" >&2
        exit 1
    }
    test -f "$topology" && test ! -L "$topology" || {
        echo "FAIL: authenticated C1F20/40 topology is unavailable" >&2
        exit 1
    }
    python3 - "$topology" "$work/batch.tsv" "$batch_expected_count" >"$work/topology.tsv" <<'PY'
import json, sys
topology_path, batch_path, expected_count = sys.argv[1], sys.argv[2], int(sys.argv[3])
value = json.load(open(topology_path, encoding="utf-8"))
if (not isinstance(value, dict) or value.get("schema") != "icecream-s8-topology-assignment-v1"
        or value.get("suite") != "C1F20/40"):
    raise SystemExit("topology schema or suite invalid")
assignments = value.get("assignments", value.get("inputs"))
batch = [line.rstrip("\n").split("\t") for line in open(batch_path, encoding="utf-8")]
if not isinstance(assignments, list) or len(assignments) != expected_count:
    raise SystemExit("topology assignment count mismatch")
if len(batch) != expected_count:
    raise SystemExit("batch/topology count mismatch")
seen = set(); relations = set(); last = {}
slots = set()
for ordinal, (assignment, fields) in enumerate(zip(assignments, batch)):
    if (not isinstance(assignment, dict) or assignment.get("ordinal") != ordinal
            or assignment.get("tu_id") != fields[0]):
        raise SystemExit("topology assignment identity mismatch")
    relationship, slot = assignment.get("relationship"), assignment.get("f_slot")
    if (type(relationship) is not int or not 0 <= relationship < 20
            or type(slot) is not int or not 0 <= slot < 2):
        raise SystemExit("topology relationship or slot invalid")
    key = (relationship, slot)
    if assignment["tu_id"] in seen:
        raise SystemExit("topology duplicate TU")
    # A queue may interleave with other relationships but cannot revisit a
    # relationship's source sequence out of order.
    if relationship in last and ordinal <= last[relationship]:
        raise SystemExit("topology relationship order invalid")
    seen.add(assignment["tu_id"]); relations.add(relationship); last[relationship] = ordinal
    slots.add(key)
    print("\t".join((str(relationship), str(slot))))
if relations != set(range(20)):
    raise SystemExit("topology does not bind all twenty F relationships")
if slots != {(relationship, slot) for relationship in range(20) for slot in range(2)}:
    raise SystemExit("topology does not bind both slots of every F relationship")
PY
fi
printf '%s\n' \
    '#include <cstdint>' \
    'int p50_environment_readiness_translation_unit() {' \
    '    return static_cast<int>(UINT32_C(51));' \
    '}' >"$work/src/environment-readiness.cpp"
if test -n "$batch_manifest" || test -n "$compile_db" || test -n "$compile_source"; then
    if test -z "$batch_manifest"; then
        test -n "$compile_db" && test -n "$compile_source" || {
            echo "FAIL: compile database and source must be supplied together" >&2; exit 1;
        }
        test -f "$compile_db" && test ! -L "$compile_db" || {
            echo "FAIL: authoritative compile database is unavailable" >&2; exit 1;
        }
        test "${compile_source#/}" != "$compile_source" || {
            echo "FAIL: compile database source must be absolute" >&2; exit 1;
        }
    fi
    compile_args_for() {
        db=$1; source=$2; expected_db_output=$3; staged=$4; output=$5
        python3 - "$db" "$source" "$expected_db_output" "$staged" "$output" <<'PY'
import json, os, shlex, sys
db, source, expected_db_output, staged, output = sys.argv[1:]
entries = json.load(open(db, encoding='utf-8'))
def resolved_output(entry):
    directory = entry.get('directory')
    command = entry.get('command')
    if not isinstance(directory, str) or not isinstance(command, str):
        return None
    try:
        tokens = shlex.split(command)
    except ValueError:
        return None
    outputs = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == '-o':
            if index + 1 >= len(tokens): return None
            outputs.append(tokens[index + 1]); index += 2; continue
        if token.startswith('-o') and len(token) > 2:
            outputs.append(token[2:])
        index += 1
    if len(outputs) != 1 or not outputs[0]: return None
    value = outputs[0]
    return os.path.realpath(value if os.path.isabs(value) else os.path.join(directory, value))
matches = [e for e in entries if isinstance(e, dict) and
           os.path.realpath(e.get('file', '')) == os.path.realpath(source) and
           (not expected_db_output or
            resolved_output(e) == os.path.realpath(expected_db_output))]
if len(matches) != 1 or not isinstance(matches[0].get('command'), str): raise SystemExit(1)
tokens = shlex.split(matches[0]['command'])
if len(tokens) < 2 or source not in tokens: raise SystemExit(1)
tokens = tokens[1:]
tokens[tokens.index(source)] = staged
for i, token in enumerate(tokens[:-1]):
    if token == '-o': tokens[i + 1] = output; break
else: raise SystemExit(1)
# Icecream's GCC remote arm adds -fdirectives-only and allocator parameters.
# Keep the compile database's debug output, but do not encode those
# compiler-owned switches in DW_AT_producer: otherwise semantically identical
# local and remote objects differ only in their debug string table.
if (any(token == '-g' or token.startswith('-g') for token in tokens)
        and not any(token in ('-grecord-gcc-switches', '-gno-record-gcc-switches')
                    for token in tokens)):
    tokens.append('-gno-record-gcc-switches')
print(shlex.join(tokens))
PY
    }
fi
if test -n "$include_root"; then
    test "${include_root#/}" != "$include_root" || {
        echo "FAIL: include root must be an absolute path" >&2
        exit 1
    }
    test -d "$include_root" && test ! -L "$include_root" || {
        echo "FAIL: authenticated include root is unavailable" >&2
        exit 1
    }
fi

# The environment is made by the real icecc tool, then shipped to the real F
# daemon. This is deliberately not replaced by the host compiler PATH.
(cd "$work/toolchain" && timeout "$timeout_s" \
    bash "$build/client/icecc-create-env" "$(command -v g++)" \
    >"$work/create-env.log" 2>&1)
envtar=$(find "$work/toolchain" -maxdepth 1 -type f \( \
    -name '*.tar.gz' -o -name '*.tar.xz' -o -name '*.tar.zst' -o \
    -name '*.tar.bz2' -o -name '*.tgz' -o -name '*.tar' \
    \) -print -quit)
test -n "$envtar" || {
    echo "FAIL: real icecc-create-env produced no compiler environment" >&2
    exit 1
}

# Keep the archive identity available for the real environment transfer below.
# The daemon owns installation and verification; its startup cleanup therefore
# cannot erase a preparation tree before the live client exercises it.
envtar_sha256=$(sha256sum "$envtar" | awk '{print $1}')
envtar_bytes=$(stat -c %s "$envtar")

# A reuse cell is admitted only after the retained package and every current
# input/command/authority/toolchain snapshot has been checked.  The validator
# writes an ordinal-to-object map in this private workdir; it never invokes a
# compiler.  A missing or changed witness is a hard failure, never a direct
# reference fallback.
if test -n "$reference_witness"; then
    test "$external_mode" = 1 && test -n "$batch_manifest" && test -n "$predictive_plan" && \
        test -n "$reference_authority" && test -n "${ICECC_P50_REFERENCE_IMAGE_ID:-}" && \
        test -n "${ICECC_P50_REFERENCE_IMAGE_REFERENCE:-}" && \
        test -n "${ICECC_P50_REFERENCE_IMAGE_ARCHITECTURE:-}" && \
        test -n "${ICECC_P50_REFERENCE_IMAGE_OS:-}" && \
        test -n "${ICECC_P50_REFERENCE_IMAGE_CREATED:-}" && \
        test -n "${ICECC_P50_REFERENCE_TOOLCHAIN_SHA256:-}" && \
        test -n "${ICECC_P50_REFERENCE_TOOLCHAIN_BYTES:-}" || {
        echo "FAIL: reference reuse requires external batch, plan, authority, and image identity" >&2
        exit 1
    }
    python3 "$src/research/farmharness/s8_reference_witness.py" verify \
        --package "$reference_witness" --batch-manifest "$batch_manifest" \
        --predictive-plan "$predictive_plan" --authority "$reference_authority" \
        --image-id "$ICECC_P50_REFERENCE_IMAGE_ID" \
        --image-reference "$ICECC_P50_REFERENCE_IMAGE_REFERENCE" \
        --image-architecture "$ICECC_P50_REFERENCE_IMAGE_ARCHITECTURE" \
        --image-os "$ICECC_P50_REFERENCE_IMAGE_OS" \
        --image-created "$ICECC_P50_REFERENCE_IMAGE_CREATED" \
        --toolchain-sha256 "$ICECC_P50_REFERENCE_TOOLCHAIN_SHA256" \
        --toolchain-bytes "$ICECC_P50_REFERENCE_TOOLCHAIN_BYTES" \
        --output "$work/reference-reuse-map.json" || {
        echo "FAIL: authenticated reference witness reuse validation failed" >&2
        exit 1
    }
    reference_reuse=1
fi

# A root container still runs the production daemon's normal privilege drop.
# Let an explicit test account own the private tree so the daemon can create
# its log and sidecar runtime files after that drop.  No account is selected
# by default, preserving the ordinary host invocation.
set --
if test -n "${ICECC_TEST_DAEMON_UID:-}"; then
    daemon_gid=${ICECC_TEST_DAEMON_GID:-$ICECC_TEST_DAEMON_UID}
    chown -R "$ICECC_TEST_DAEMON_UID:$daemon_gid" "$work"
    set -- -u "$ICECC_TEST_DAEMON_UID"
fi
if test -n "${ICECC_P50_C1F1_WORKER_INTERFACE:-}"; then
    set -- "$@" -i "$ICECC_P50_C1F1_WORKER_INTERFACE"
fi

run_client_with_timeout() {
    if test -n "${ICECC_TEST_WRAPPER_USER:-}"; then
        if timeout "$timeout_s" runuser -u "$ICECC_TEST_WRAPPER_USER" -- \
                "$build/client/icecc" "$@"; then
            client_command_status=0
        else
            client_command_status=$?
        fi
    elif test -n "${ICECC_TEST_DAEMON_UID:-}"; then
        if timeout "$timeout_s" runuser -u "$ICECC_TEST_DAEMON_UID" -- \
                "$build/client/icecc" "$@"; then
            client_command_status=0
        else
            client_command_status=$?
        fi
    else
        if timeout "$timeout_s" "$build/client/icecc" "$@"; then
            client_command_status=0
        else
            client_command_status=$?
        fi
    fi
    if test -n "${ICECC_P50_CLIENT_STATUS_PATH:-}"; then
        printf '%s\n' "$client_command_status" >"$ICECC_P50_CLIENT_STATUS_PATH"
    fi
    return "$client_command_status"
}

if test "$external_mode" = 0; then
"$build/scheduler/icecc-scheduler" -p "$port_sched" -n "$network" \
    --assignment-fence-mode strict-nonce -l "$work/scheduler.log" -vvv \
    2>"$work/scheduler-startup.stderr" &
sched_pid=$!
sleep 1
kill -0 "$sched_pid" 2>/dev/null || {
    echo "FAIL: real scheduler exited during startup" >&2
    exit 1
}

# The parallel cell has one persistent F identity per relationship.  Each F
# owns two real compiler slots; the client selects the authenticated F by
# relationship for every source-stage request.  No fake peer is introduced.
worker_pids=""
service_pids=""
if test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        worker_port=$((port_worker + relationship * 2))
        test "$worker_port" -lt 60000 || { echo "FAIL: worker port range exhausted" >&2; exit 1; }
        f_trace="$work/s7-warm-f-action-trace-$relationship.jsonl"
        f_wire_trace="$work/s7-measured-f-legacy-wire-trace-$relationship.jsonl"
        if test "$cache_enabled" -eq 1; then
            ICECC_TEST_SOCKET="$work/worker-$relationship.sock" ICECC_P50_C1F1_REQUIRED=1 \
                ICECC_P50_C_ACTION_TRACE="$f_trace" ICECC_P50_F_ACTION_TRACE="$f_trace" \
                ICECC_P50_TEST_READY_TRACE="$work/ready-f-$relationship.trace" \
                ICECC_P50_F_LEGACY_WIRE_TRACE="$f_wire_trace" \
                ICECC_P50_RELATIONSHIP="$relationship" \
                "$build/daemon/iceccd" "$@" -p "$worker_port" -m 2 \
                -s "$worker_scheduler_host:$port_sched" -n "$network" -N "p50-f-$relationship" \
                -b "$work/envs-f-$relationship" -l "$work/f-$relationship.log" -vvv \
                --cache-service "$build/cache/icecc-cache-service" \
                --cache-runtime-dir "$work/cache-runtime-f-$relationship" &
        else
            ICECC_TEST_SOCKET="$work/worker-$relationship.sock" \
                ICECC_P50_C_ACTION_TRACE="$f_trace" ICECC_P50_F_ACTION_TRACE="$f_trace" \
                ICECC_P50_TEST_READY_TRACE="$work/ready-f-$relationship.trace" \
                ICECC_P50_F_LEGACY_WIRE_TRACE="$f_wire_trace" \
                ICECC_P50_RELATIONSHIP="$relationship" \
                "$build/daemon/iceccd" "$@" -p "$worker_port" -m 2 \
                -s "$worker_scheduler_host:$port_sched" -n "$network" -N "p50-f-$relationship" \
                -b "$work/envs-f-$relationship" -l "$work/f-$relationship.log" -vvv &
        fi
        worker_pid=$!
        worker_pids="$worker_pids $worker_pid"
    done
else
    if test "$cache_enabled" -eq 1; then
        ICECC_TEST_SOCKET="$work/worker.sock" ICECC_P50_C1F1_REQUIRED=1 \
            ICECC_P50_F_LEGACY_WIRE_TRACE="$f_legacy_wire_trace" \
        ICECC_P50_C_ACTION_TRACE="$f_action_trace" ICECC_P50_F_ACTION_TRACE="$f_action_trace" \
        ICECC_P50_TEST_READY_TRACE="$work/ready-f.trace" \
        "$build/daemon/iceccd" "$@" -p "$port_worker" -m "$worker_maxjobs" \
        -s "$worker_scheduler_host:$port_sched" -n "$network" -N p50-f \
        -b "$work/envs-f" -l "$work/f.log" -vvv \
            --cache-service "$build/cache/icecc-cache-service" \
            --cache-runtime-dir "$work/cache-runtime-f" 2>"$work/f-daemon-startup.stderr" &
    else
        ICECC_TEST_SOCKET="$work/worker.sock" \
            ICECC_P50_F_LEGACY_WIRE_TRACE="$f_legacy_wire_trace" \
            ICECC_P50_C_ACTION_TRACE="$f_action_trace" ICECC_P50_F_ACTION_TRACE="$f_action_trace" \
            ICECC_P50_TEST_READY_TRACE="$work/ready-f.trace" \
            "$build/daemon/iceccd" "$@" -p "$port_worker" -m "$worker_maxjobs" \
            -s "$worker_scheduler_host:$port_sched" -n "$network" -N p50-f \
            -b "$work/envs-f" -l "$work/f.log" -vvv 2>"$work/f-daemon-startup.stderr" &
    fi
    worker_pid=$!
    worker_pids="$worker_pid"
fi

if test "$cache_enabled" -eq 1; then
    ICECC_TEST_SOCKET="$work/client.sock" ICECC_P50_C1F1_REQUIRED=1 \
        ICECC_P50_C_ACTION_TRACE="$c_action_trace" ICECC_P50_F_ACTION_TRACE="$c_action_trace" \
        ICECC_P50_SOURCE_RESULT_TRACE="$source_result_trace" \
        ICECC_P50_C_LEGACY_WIRE_TRACE="$c_legacy_wire_trace" \
        ICECC_P50_TEST_READY_TRACE="$work/ready-c.trace" \
        "$build/daemon/iceccd" "$@" --no-remote -m 0 -p "$port_client" \
        -s "127.0.0.1:$port_sched" -n "$network" -N p50-c \
        -b "$work/envs-c" -l "$work/c.log" -vvv \
        --cache-service "$build/cache/icecc-cache-service" \
        --cache-runtime-dir "$work/cache-runtime-c" 2>"$work/c-daemon-startup.stderr" &
else
    ICECC_TEST_SOCKET="$work/client.sock" \
        ICECC_P50_C_ACTION_TRACE="$c_action_trace" ICECC_P50_F_ACTION_TRACE="$c_action_trace" \
        ICECC_P50_SOURCE_RESULT_TRACE="$source_result_trace" \
        ICECC_P50_C_LEGACY_WIRE_TRACE="$c_legacy_wire_trace" \
        ICECC_P50_TEST_READY_TRACE="$work/ready-c.trace" \
        "$build/daemon/iceccd" "$@" --no-remote -m 0 -p "$port_client" \
        -s "127.0.0.1:$port_sched" -n "$network" -N p50-c \
        -b "$work/envs-c" -l "$work/c.log" -vvv 2>"$work/c-daemon-startup.stderr" &
fi
client_pid=$!

logins=0
for _ in $(seq 1 30); do
    logins=$(grep -c login "$work/scheduler.log" 2>/dev/null || true)
    if test "$suite" = C1F20/40; then
        test "${logins:-0}" -ge 21 && break
    else
        test "${logins:-0}" -ge 2 && break
    fi
    sleep 1
done
required_logins=2
test "$suite" = C1F20/40 && required_logins=21
test "${logins:-0}" -ge "$required_logins" || {
    echo "FAIL: real C1F1 daemons did not register" >&2
    exit 1
}
ordinary_accepts=$(grep -F -c "accepted $worker_scheduler_host" "$work/scheduler.log" 2>/dev/null || true)
test "${ordinary_accepts:-0}" -ge "$relationship_count" || {
    echo "FAIL: scheduler did not accept every F through the required ordinary address" >&2
    exit 1
}
if test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        grep -F "I am known as $worker_scheduler_host" \
            "$work/f-$relationship.log" >/dev/null || {
            echo "FAIL: F relationship $relationship did not receive the required ordinary address from S" >&2
            exit 1
        }
    done
else
    grep -F "I am known as $worker_scheduler_host" "$work/f.log" >/dev/null || {
        echo "FAIL: F did not receive the required ordinary address from S" >&2
        exit 1
    }
fi

# The cache executable must be alive as a child of the production daemon
# wiring. Merely checking that the file exists would permit a mechanism-only
# test to masquerade as an end-to-end compile.
service_pid=
if test "$cache_enabled" -eq 1 && test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        found=
        for _ in $(seq 1 30); do
            found=$(ps -eo pid=,ppid=,args= | \
                awk -v parent="$(printf '%s' "$worker_pids" | awk -v n="$relationship" '{print $(n + 1)}')" \
                    -v exe="$build/cache/icecc-cache-service" \
                '$2 == parent && index($0, exe) > 0 { print $1; exit }')
            test -n "$found" && break
            sleep 1
        done
        test -n "$found" || { echo "FAIL: F relationship $relationship cache service missing" >&2; exit 1; }
        service_pids="$service_pids $found"
    done
    service_pid=$(printf '%s\n' "$service_pids" | awk '{print $1}')
elif test "$cache_enabled" -eq 1; then
    for _ in $(seq 1 30); do
        service_pid=$(ps -eo pid=,ppid=,args= | \
            awk -v parent="$worker_pid" -v exe="$build/cache/icecc-cache-service" \
            '$2 == parent && index($0, exe) > 0 { print $1; exit }')
        test -n "$service_pid" && break
        sleep 1
    done
    test -n "$service_pid" || {
        echo "FAIL: production daemon did not start the actual icecc-cache-service" >&2
        exit 1
    }
fi

client_service_pid=
if test "$cache_enabled" -eq 1; then
for _ in $(seq 1 30); do
    client_service_pid=$(ps -eo pid=,ppid=,args= | \
        awk -v parent="$client_pid" -v exe="$build/cache/icecc-cache-service" \
        '$2 == parent && index($0, exe) > 0 { print $1; exit }')
    test -n "$client_service_pid" && break
    sleep 1
done
test -n "$client_service_pid" || {
    echo "FAIL: C daemon did not start its authenticated local cache sidecar" >&2
    exit 1
}
else
    client_service_pid=
    if ps -eo args= | grep -F "$build/cache/icecc-cache-service" | grep -v grep >/dev/null 2>&1; then
        echo "FAIL: RAW_II unexpectedly has a cache-service process" >&2
        exit 1
    fi
    echo "S8_RAW_II mode=whole-legacy cache_disabled=1 cache_traffic=0"
fi

# A live child is not yet a usable cache endpoint.  Submit only after the
# daemon has authenticated READY and the scheduler has consumed F's real
# cache-bearing relogin; otherwise the assignment is correctly frozen without
# a handoff and a millisecond startup race masquerades as a product failure.
cache_ready=0
if test "$cache_enabled" -eq 0; then
    cache_ready=1
else
for _ in $(seq 1 30); do
    if test "$suite" = C1F20/40; then
        # A scheduler may relogin the same F more than once.  Count extracted
        # authenticated service identities, not timestamped log lines.
        ready_count=$(grep -E "RELOGIN p50-f-[0-9]+.*cache=.*cache_profiles=.*$profile_advertisement" \
            "$work/scheduler.log" 2>/dev/null |
            grep -oE 'p50-f-[0-9]+' | sort -u | wc -l)
        test "$ready_count" -ge 20 && cache_ready=1 && break
    elif grep -E "RELOGIN p50-f.*cache=.*cache_profiles=.*$profile_advertisement" \
            "$work/scheduler.log" >/dev/null 2>&1; then
        cache_ready=1; break
    fi
    sleep 1
done
test "$cache_ready" -eq 1 || {
    echo "FAIL: production F cache endpoint was not advertised READY" >&2
    exit 1
}
fi
else
    # The external transport owns these processes.  This client-only branch
    # must never create a scheduler, C daemon, or F daemon on q3; it waits on
    # the authenticated scheduler log and uses the already-running q3 C plus
    # externally placed F identities for the mature batch below.
    sched_pid=
    worker_pids=
    service_pids=
    worker_pid=
    service_pid=
    client_pid=
    client_service_pid=
    external_required=$((relationship_count + 1))
    test -n "${ICECC_P50_EXTERNAL_RESET_HOOK:-}" && test -n "${ICECC_P50_EXTERNAL_RESET_READY:-}" || {
        echo "FAIL: external F reset handshake is required" >&2; exit 1;
    }
    external_ready=0
    for _ in $(seq 1 60); do
        logins=$(grep -c login "$work/scheduler.log" 2>/dev/null || true)
        if test "$cache_enabled" -eq 0; then
            test "${logins:-0}" -ge "$external_required" && external_ready=1 && break
        elif test "$suite" = C1F20/40; then
            ready_count=$(grep -E "RELOGIN p50-f-[0-9]+.*cache=.*cache_profiles=.*$profile_advertisement" \
                "$work/scheduler.log" 2>/dev/null | grep -oE 'p50-f-[0-9]+' | sort -u | wc -l)
            test "${ready_count:-0}" -ge 20 && external_ready=1 && break
        elif grep -E "RELOGIN p50-f.*cache=.*cache_profiles=.*$profile_advertisement" \
                "$work/scheduler.log" >/dev/null 2>&1; then
            external_ready=1; break
        fi
        sleep 1
    done
    test "$external_ready" -eq 1 || { echo "FAIL: external farm READY/registration missing" >&2; exit 1; }
fi

ready_snapshot() {
    ready_path=$1
    ready_fields=$(awk '
        /^READY v2 / {
            pid = c = f = ""
            for (i = 1; i <= NF; ++i) {
                split($i, field, "=")
                if (field[1] == "pid") pid = field[2]
                if (field[1] == "C_STORE_GUID") c = field[2]
                if (field[1] == "F_STORE_GUID") f = field[2]
            }
            if (pid != "" && c != "" && f != "") last = pid " " c " " f
        }
        END { if (last == "") exit 1; print last }
    ' "$ready_path") || return 1
    set -- $ready_fields
    ready_pid=$1
    ready_c_guid=$2
    ready_f_guid=$3
}

restart_cache_sidecar() {
    sidecar_role=$1
    sidecar_relationship=$2
    old_pid=$3
    sidecar_parent=$4
    ready_path=$5
    ready_before=$(grep -c '^READY v2 ' "$ready_path" 2>/dev/null || true)
    ready_snapshot "$ready_path" || {
        echo "FAIL: missing pre-rotation READY identity ($sidecar_role-$sidecar_relationship)" >&2
        exit 1
    }
    before_pid=$ready_pid
    before_c_guid=$ready_c_guid
    before_f_guid=$ready_f_guid
    kill -9 "$old_pid"
    replacement=
    for _ in $(seq 1 300); do
        replacement=$(ps -eo pid=,ppid=,args= | awk -v parent="$sidecar_parent" \
            -v exe="$build/cache/icecc-cache-service" \
            '$2 == parent && index($0, exe) > 0 { print $1; exit }')
        if test -n "$replacement" && test "$replacement" != "$old_pid"; then
            break
        fi
        sleep 0.1
    done
    test -n "$replacement" && test "$replacement" != "$old_pid" || {
        echo "FAIL: cache sidecar did not restart ($sidecar_role-$sidecar_relationship)" >&2
        exit 1
    }
    replacement_ready=0
    for _ in $(seq 1 300); do
        ready_now=$(grep -c '^READY v2 ' "$ready_path" 2>/dev/null || true)
        if test "${ready_now:-0}" -gt "${ready_before:-0}"; then
            replacement_ready=1
            break
        fi
        sleep 0.1
    done
    test "$replacement_ready" -eq 1 || {
        echo "FAIL: replacement cache sidecar did not publish READY ($sidecar_role-$sidecar_relationship)" >&2
        exit 1
    }
    ready_snapshot "$ready_path" || {
        echo "FAIL: replacement READY identity is malformed ($sidecar_role-$sidecar_relationship)" >&2
        exit 1
    }
    test "$ready_pid" = "$replacement" || {
        echo "FAIL: READY pid does not bind replacement sidecar ($sidecar_role-$sidecar_relationship)" >&2
        exit 1
    }
    test "$before_pid" != "$ready_pid" && test "$before_c_guid" != "$ready_c_guid" && \
        test "$before_f_guid" != "$ready_f_guid" || {
        echo "FAIL: sidecar identity did not rotate ($sidecar_role-$sidecar_relationship)" >&2
        exit 1
    }
    echo "S8_SIDECAR_ROTATION role=$sidecar_role relationship=$sidecar_relationship before_pid=$before_pid after_pid=$ready_pid before_c_store_guid=$before_c_guid after_c_store_guid=$ready_c_guid before_f_store_guid=$before_f_guid after_f_store_guid=$ready_f_guid"
    sidecar_replacement=$replacement
}

compile_once() {
    label=$1
    input_path=${2:-$work/src/main.cpp}
    item_compile_db=${3:-$compile_db}
    item_compile_source=${4:-$compile_source}
    item_compile_output=${5:-}
    relationship=${6:-0}
    f_slot=${7:-0}
    timing_path=${8:-}
    preferred_host=p50-f
    if test "$suite" = C1F20/40; then
        preferred_host="p50-f-$relationship"
    fi
    remote_obj="$work/out/remote-$label.o"
    local_obj="$work/out/local-$label.o"
    client_log="$work/client-compile-$label.log"
    compile_include_args=""
    if test -n "$item_compile_db"; then
        # Depth batches stage the authenticated predictive .ii.  Remote and
        # local compilation must bind to that exact staged input; the source
        # path is used only to select the unique compile-database command.
        remote_compile_args=$(compile_args_for "$item_compile_db" "$item_compile_source" "$item_compile_output" "$input_path" "$remote_obj")
        local_compile_args=$(compile_args_for "$item_compile_db" "$item_compile_source" "$item_compile_output" "$input_path" "$local_obj")
    elif test -n "$include_root"; then
        compile_include_args="-I$include_root"
    else
        compile_include_args=""
    fi
    preprocessed_capture="$work/s7-$label-preprocessed.ii"
    if test "$cache_enabled" -eq 0; then
        cp -- "$input_path" "$preprocessed_capture"
    fi
    compile_start_ns=$(date +%s%N)
    if test -n "$timing_path"; then
        printf '%s\n' "$compile_start_ns" >"$timing_path"
    fi
    if test -n "$item_compile_db"; then
        # eval is needed to turn the safely shlex-quoted database tokens back
        # into argv. Export first: assignments before the special builtin
        # `eval` become shell variables, not necessarily the environment seen
        # by the external client called by run_client_with_timeout.
        ICECC_TEST_SOCKET="$work/client.sock"
        ICECC_TEST_REMOTEBUILD=1
        ICECC_VERSION="$envtar"
        if test "$cache_enabled" -eq 1; then
            ICECC_P50_C1F1_REQUIRED=1
        else
            unset ICECC_P50_C1F1_REQUIRED
        fi
        ICECC_P50_PREPROCESSED_CAPTURE="$preprocessed_capture"
        ICECC_P50_C_LEGACY_WIRE_TRACE="$c_legacy_wire_trace"
        ICECC_PREFERRED_HOST="$preferred_host"
        ICECC_DEBUG=debug
        ICECC_LOGFILE="$client_log"
        export ICECC_TEST_SOCKET ICECC_TEST_REMOTEBUILD ICECC_VERSION \
            ICECC_P50_PREPROCESSED_CAPTURE \
            ICECC_P50_C_LEGACY_WIRE_TRACE ICECC_PREFERRED_HOST ICECC_DEBUG ICECC_LOGFILE
        eval "run_client_with_timeout g++ $remote_compile_args"
    elif test "$cache_enabled" -eq 1; then
        ICECC_TEST_SOCKET="$work/client.sock" ICECC_TEST_REMOTEBUILD=1 \
            ICECC_VERSION="$envtar" ICECC_P50_C1F1_REQUIRED=1 \
            ICECC_P50_PREPROCESSED_CAPTURE="$preprocessed_capture" \
            ICECC_P50_C_LEGACY_WIRE_TRACE="$c_legacy_wire_trace" \
            ICECC_PREFERRED_HOST="$preferred_host" ICECC_DEBUG=debug ICECC_LOGFILE="$client_log" \
            run_client_with_timeout g++ -std=c++17 -O2 -c \
            $compile_include_args "$input_path" -o "$remote_obj"
    else
        ICECC_TEST_SOCKET="$work/client.sock" ICECC_TEST_REMOTEBUILD=1 \
            ICECC_VERSION="$envtar" ICECC_P50_C_LEGACY_WIRE_TRACE="$c_legacy_wire_trace" \
            ICECC_P50_PREPROCESSED_CAPTURE="$preprocessed_capture" \
            ICECC_PREFERRED_HOST="$preferred_host" ICECC_DEBUG=debug ICECC_LOGFILE="$client_log" \
            run_client_with_timeout g++ -std=c++17 -O2 -c \
            $compile_include_args "$input_path" -o "$remote_obj"
    fi
    compile_end_ns=$(date +%s%N)
    if test -n "$timing_path"; then
        printf '%s\n' "$compile_end_ns" >>"$timing_path"
    fi
    if test "$external_mode" = 1 && test -n "$timing_path"; then
        # Defer q3's byte-identical reference until the remote batch barrier.
        printf '%s\n' "$input_path" "$item_compile_db" "$item_compile_source" \
            "$item_compile_output" "$local_obj" "$remote_obj" >"$work/local-pending-$label"
    elif test "$reference_reuse" = 1; then
        # This is a transport copy used only to satisfy the mature finalizer's
        # local-object path contract.  The retained object is checked against
        # the remote result at the batch barrier below; no compiler is run.
        cp -- "$remote_obj" "$local_obj"
    elif test -n "$item_compile_db"; then
        eval "g++ $local_compile_args"
    else
        # Mirror the real client invocation for preparation compiles.  Feeding
        # the same bytes through stdin changes GCC's FILE symbol to <stdin>,
        # so a correct remote object would fail this byte-exact witness solely
        # because its source basename is retained.
        g++ -std=c++17 -O2 -c $compile_include_args \
            "$input_path" -o "$local_obj"
    fi
    test -s "$preprocessed_capture" || {
        echo "FAIL: completed $label preprocessor capture is missing" >&2
        exit 1
    }
    if test "$external_mode" = 0 || test -z "$timing_path"; then
        cmp -s "$remote_obj" "$local_obj" || {
            echo "FAIL: real P50 object differs from local reference ($label)" >&2
            exit 1
        }
    fi
}

s2_compile_with_process_loss() {
    s2_hook=${ICECC_P50_EXTERNAL_S2_HOOK:-}
    test -x "$s2_hook" || {
        echo "FAIL: S2 process-loss hook is not executable" >&2
        return 1
    }
    compile_once env-warm "$work/src/environment-readiness.cpp" "" "" "" 0 0 "" &
    s2_compile_pid=$!
    set +e
    "$s2_hook" "$work" "$suite" "$profile_marker" kill
    s2_hook_status=$?
    set -e
    if test "$s2_hook_status" -ne 0; then
        kill "$s2_compile_pid" 2>/dev/null || :
        set +e
        wait "$s2_compile_pid"
        set -e
        s2_compile_pid=
        echo "FAIL: S2 process-loss kill/replacement hook failed" >&2
        return 1
    fi
    set +e
    wait "$s2_compile_pid"
    s2_compile_status=$?
    set -e
    s2_compile_pid=
    test "$s2_compile_status" -eq 0 || {
        echo "FAIL: original compile did not recover after F sidecar loss (status $s2_compile_status)" >&2
        return 1
    }
}

s2_verify_process_loss() {
    s2_hook=${ICECC_P50_EXTERNAL_S2_HOOK:-}
    "$s2_hook" "$work" "$suite" "$profile_marker" verify || {
        echo "FAIL: S2 process-loss final verification hook failed" >&2
        return 1
    }
    if test ! -s "$s2_external_evidence" || test -L "$s2_external_evidence"; then
        echo "FAIL: S2 process-loss evidence is missing" >&2
        return 1
    fi
    python3 - "$s2_external_evidence" <<'PY'
import json
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
value = json.loads(path.read_text(encoding="utf-8"))
if (value.get("schema") != "icecream-s2-process-loss-v1" or
        value.get("status") != "PASS" or value.get("role") != "F" or
        value.get("relationship") != 0 or value.get("action") != "TX_BEGIN"):
    raise SystemExit("S2 process-loss evidence identity is invalid")
for key in ("marker_unchanged", "release_absent", "replacement_ready",
            "scheduler_relogin", "original_compile_completed"):
    if value.get(key) is not True:
        raise SystemExit(f"S2 process-loss evidence is missing {key}")
marker_sha = value.get("marker_sha256")
if not isinstance(marker_sha, str) or re.fullmatch(r"[0-9a-f]{64}", marker_sha) is None:
    raise SystemExit("S2 process-loss marker digest is invalid")
before = value.get("before")
after = value.get("after")
if not isinstance(before, dict) or not isinstance(after, dict):
    raise SystemExit("S2 process-loss READY identities are missing")
for identity in (before, after):
    if (type(identity.get("pid")) is not int or identity["pid"] <= 0 or
            type(identity.get("parent_pid")) is not int or identity["parent_pid"] <= 0):
        raise SystemExit("S2 process-loss PID identity is invalid")
    for key in ("c_store_guid", "f_store_guid"):
        if not isinstance(identity.get(key), str) or re.fullmatch(r"[0-9a-f]{32}", identity[key]) is None:
            raise SystemExit(f"S2 process-loss {key} is invalid")
if (before["pid"] == after["pid"] or
        before["c_store_guid"] == after["c_store_guid"] or
        before["f_store_guid"] == after["f_store_guid"] or
        before["parent_pid"] != after["parent_pid"]):
    raise SystemExit("S2 process-loss replacement identity did not rotate under one F daemon")
print("S2_PROCESS_LOSS role=F relationship=0 action=TX_BEGIN "
      f"before_pid={before['pid']} after_pid={after['pid']} "
      f"parent_pid={after['parent_pid']} marker_sha256={marker_sha} "
      "original_compile_rc=0 replacement_ready=1 scheduler_relogin=1 one_shot=1")
PY
}

# A batch always reuses this one scheduler/C/F/cache lifecycle.  Warm mode
# first compiles the exact selected ordered manifest as prewarm work; those
# rows remain outside measured evidence while the trace offsets below bind the
# measured pass(es) to the same service state.
prewarm_c_trace="$work/s7-prewarm-c-action-trace.jsonl"
prewarm_f_trace="$work/s7-prewarm-f-action-trace.jsonl"
measured_c_trace="$work/s7-measured-c-action-trace.jsonl"
measured_f_trace="$work/s7-measured-f-action-trace.jsonl"
merge_parallel_f_traces() {
    test "$suite" = C1F20/40 || return 0
    python3 - "$work" "$f_action_trace" "$work/s8-f-service-map.tsv" <<'PY'
import json, pathlib, sys
root, output, service_map = map(pathlib.Path, sys.argv[1:])
rows = []
relationships = []
for relationship in range(20):
    path = root / f"s7-warm-f-action-trace-{relationship}.jsonl"
    path_rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()
                 if line.strip()]
    identities = {row.get("f_store_guid") for row in path_rows
                  if row.get("action") == "TX_BEGIN" and row.get("actor") == "F"}
    if len(identities) != 1:
        raise SystemExit(f"F relationship {relationship} has no unique observed store identity")
    relationships.append((relationship, f"p50-f-{relationship}", identities.pop()))
    for line in path_rows:
        rows.append(line)
if not rows:
    raise SystemExit("parallel F action traces are empty")
rows.sort(key=lambda row: (int(row.get("tu_seq", -1)), int(row.get("rel_seq", -1)),
                           row.get("f_store_guid", "")))
output.write_text("".join(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n"
                             for row in rows), encoding="utf-8")
service_map.write_text("".join(f"{relationship}\t{service}\t{guid}\n"
                               for relationship, service, guid in relationships),
                       encoding="ascii")
PY
}

# Exercise the daemon-owned environment transfer before measurement.  The
# second assignment to each relationship must report that its environment is
# already installed; setup cache state is discarded by rotating only the
# sidecars below.
environment_preparation_start_ns=$(date +%s%N)
environment_warmup_count=0
environment_warmup_c_trace="$work/s8-environment-warmup-c-action-trace.jsonl"
if test "$cache_enabled" -eq 0; then
    # RAW_II has no cache sidecar, but its first remote compile still installs
    # ICECC_VERSION on each F.  Pay that setup cost before the measured batch
    # without assigning it a TU timing row or retaining its wire witness.
    if test "$suite" = C1F20/40; then
        for relationship in $(seq 0 19); do
            compile_once "raw-env-ready-$relationship" "$work/src/environment-readiness.cpp" "" "" "" "$relationship" 0 ""
            grep -F 'has env: false' "$work/client-compile-raw-env-ready-$relationship.log" >/dev/null || {
                echo "FAIL: RAW_II F environment was not installed ($relationship)" >&2
                exit 1
            }
            environment_warmup_count=$((environment_warmup_count + 1))
            echo "S8_RAW_ENV_READY relationship=$relationship label=raw-env-ready-$relationship measured=0 cache_state=disabled"
        done
    else
        compile_once raw-env-ready "$work/src/environment-readiness.cpp" "" "" "" 0 0 ""
        grep -F 'has env: false' "$work/client-compile-raw-env-ready.log" >/dev/null || {
            echo "FAIL: RAW_II F environment was not installed" >&2
            exit 1
        }
        environment_warmup_count=1
        echo "S8_RAW_ENV_READY relationship=0 label=raw-env-ready measured=0 cache_state=disabled"
    fi
    test "$environment_warmup_count" -eq "$relationship_count" || {
        echo "FAIL: RAW_II environment readiness count mismatch" >&2
        exit 1
    }
    test -s "$c_legacy_wire_trace" || {
        echo "FAIL: RAW_II environment readiness has no C wire witness" >&2
        exit 1
    }
    if test "$suite" = C1F20/40; then
        for relationship in $(seq 0 19); do
            test -s "$work/s7-measured-f-legacy-wire-trace-$relationship.jsonl" || {
                echo "FAIL: RAW_II environment readiness has no F wire witness ($relationship)" >&2
                exit 1
            }
        done
    else
        test -s "$f_legacy_wire_trace" || {
            echo "FAIL: RAW_II environment readiness has no F wire witness" >&2
            exit 1
        }
    fi
    # The daemon keeps the same role-labelled sinks for the measured pass;
    # truncate only after every F has completed readiness so setup rows cannot
    # enter the retained per-TU RAW_II curve.
    : >"$c_legacy_wire_trace"
    if test "$suite" = C1F20/40; then
        for relationship in $(seq 0 19); do
            : >"$work/s7-measured-f-legacy-wire-trace-$relationship.jsonl"
        done
    else
        : >"$f_legacy_wire_trace"
    fi
    : >"$environment_warmup_c_trace"
    : >"$work/s8-environment-warmup-f-action-trace.jsonl"
    environment_preparation_end_ns=$(date +%s%N)
elif test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        compile_once "env-warm-$relationship" "$work/src/environment-readiness.cpp" "" "" "" "$relationship" 0 ""
        grep -F 'has env: false' "$work/client-compile-env-warm-$relationship.log" >/dev/null || {
            echo "FAIL: F environment was not installed by the first real assignment ($relationship)" >&2
            exit 1
        }
        compile_once "env-ready-$relationship" "$work/src/environment-readiness.cpp" "" "" "" "$relationship" 0 ""
        grep -F 'has env: true' "$work/client-compile-env-ready-$relationship.log" >/dev/null || {
            echo "FAIL: environment-bearing assignment was not observed ($relationship)" >&2
            exit 1
        }
        echo "S8_ENV_WARMUP relationship=$relationship warmup_label=env-warm-$relationship ready_label=env-ready-$relationship warmup_has_env=false ready_has_env=true preparation_measured=0 cache_state=pre_rotation"
        environment_warmup_count=$((environment_warmup_count + 1))
    done
else
    if test "$s2_process_loss" = 1; then
        s2_compile_with_process_loss
    else
        compile_once env-warm "$work/src/environment-readiness.cpp" "" "" "" 0 0 ""
    fi
    grep -F 'has env: false' "$work/client-compile-env-warm.log" >/dev/null || {
        echo "FAIL: F environment was not installed by the first real assignment" >&2
        exit 1
    }
    compile_once env-ready "$work/src/environment-readiness.cpp" "" "" "" 0 0 ""
    grep -F 'has env: true' "$work/client-compile-env-ready.log" >/dev/null || {
        echo "FAIL: environment-bearing assignment was not observed" >&2
        exit 1
    }
    if test "$s2_process_loss" = 1; then
        s2_verify_process_loss
    fi
    echo "S8_ENV_WARMUP relationship=0 warmup_label=env-warm ready_label=env-ready warmup_has_env=false ready_has_env=true preparation_measured=0 cache_state=pre_rotation"
    environment_warmup_count=1
fi
if test "$cache_enabled" -eq 1; then
cp -- "$c_action_trace" "$environment_warmup_c_trace"
if test "$external_mode" = 1; then
    :
elif test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        cp -- "$work/s7-warm-f-action-trace-$relationship.jsonl" \
            "$work/s8-environment-warmup-f-action-trace-$relationship.jsonl"
    done
else
    cp -- "$f_action_trace" "$work/s8-environment-warmup-f-action-trace.jsonl"
fi
fi

scheduler_rotation_offset=$(stat -c %s "$work/scheduler.log")
if test "$external_mode" = 1; then
    # Remote transport performs the untimed F warmup and cache rotation.  The
    # q3 client only records the authenticated scheduler offset here.
    ready_count=$relationship_count
    post_rotation_ready=1
    if test -n "${ICECC_P50_EXTERNAL_RESET_HOOK:-}"; then
        test -x "$ICECC_P50_EXTERNAL_RESET_HOOK" || {
            echo "FAIL: external reset hook is not authenticated/executable" >&2; exit 1;
        }
        "$ICECC_P50_EXTERNAL_RESET_HOOK" "$work" "$suite" "$profile_marker"
        test -f "${ICECC_P50_EXTERNAL_RESET_READY:-}" || {
            echo "FAIL: external F reset READY witness missing" >&2; exit 1;
        }
        test -s "$work/external-rotation-evidence" || {
            echo "FAIL: external sidecar rotation evidence missing" >&2; exit 1;
        }
        cat "$work/external-rotation-evidence"
    fi
elif test "$cache_enabled" -eq 0; then
    ready_count=0
elif test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        worker_for_relationship=$(printf '%s\n' "$worker_pids" | awk -v n="$relationship" '{print $(n + 1)}')
        service_for_relationship=$(ps -eo pid=,ppid=,args= | awk -v parent="$worker_for_relationship" \
            -v exe="$build/cache/icecc-cache-service" '$2 == parent && index($0, exe) > 0 { print $1; exit }')
        test -n "$service_for_relationship" || { echo "FAIL: F sidecar disappeared before rotation ($relationship)" >&2; exit 1; }
        restart_cache_sidecar F "$relationship" "$service_for_relationship" "$worker_for_relationship" \
            "$work/ready-f-$relationship.trace"
    done
    restart_cache_sidecar C 0 "$client_service_pid" "$client_pid" "$work/ready-c.trace"
else
    restart_cache_sidecar F 0 "$service_pid" "$worker_pid" "$work/ready-f.trace"
    service_pid=$sidecar_replacement
    restart_cache_sidecar C 0 "$client_service_pid" "$client_pid" "$work/ready-c.trace"
    client_service_pid=$sidecar_replacement
fi

post_rotation_ready=0
if test "$cache_enabled" -eq 0; then
    post_rotation_ready=1
else
for _ in $(seq 1 30); do
    if test "$suite" = C1F20/40; then
        ready_count=$(tail -c +$((scheduler_rotation_offset + 1)) "$work/scheduler.log" | \
            grep -E "RELOGIN p50-f-[0-9]+.*cache=.*cache_profiles=.*$profile_advertisement" | \
            grep -oE 'p50-f-[0-9]+' | sort -u | wc -l)
        test "$ready_count" -ge 20 && post_rotation_ready=1 && break
    elif tail -c +$((scheduler_rotation_offset + 1)) "$work/scheduler.log" | \
            grep -E "RELOGIN p50-f.*cache=.*cache_profiles=.*$profile_advertisement" >/dev/null 2>&1; then
        ready_count=1
        post_rotation_ready=1
        break
    fi
    sleep 1
done
test "$post_rotation_ready" -eq 1 || {
    echo "FAIL: rotated cache endpoints were not advertised READY" >&2
    exit 1
}
fi
echo "S8_ENV_POST_ROTATION_READY relationships=$ready_count log_offset=$scheduler_rotation_offset"

if test "$external_mode" = 1; then
    if test "$suite" = C1F20/40; then
        for relationship in $(seq 0 19); do : >"$work/f-measured-log-offset-$relationship"; done
    else
        : >"$work/f-measured-log-offset-0"
    fi
elif test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        stat -c %s "$work/f-$relationship.log" >"$work/f-measured-log-offset-$relationship"
    done
else
    stat -c %s "$work/f.log" >"$work/f-measured-log-offset-0"
fi

: >"$c_action_trace"
if test "$external_mode" = 1; then
    # F traces are copied into this q3 workdir by the external transport after
    # the remote daemons are frozen; never truncate a remote trace here.
    :
elif test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        : >"$work/s7-warm-f-action-trace-$relationship.jsonl"
    done
else
    : >"$f_action_trace"
fi
environment_preparation_end_ns=${environment_preparation_end_ns:-$(date +%s%N)}
if test "$cache_enabled" -eq 0; then
    echo "S8_ENV_PREPARATION relationships=$environment_warmup_count readiness_compiles=$environment_warmup_count archive_sha256=$envtar_sha256 archive_bytes=$envtar_bytes start_ns=$environment_preparation_start_ns end_ns=$environment_preparation_end_ns measured=0 cache_state=disabled"
else
    echo "S8_ENV_PREPARATION relationships=$environment_warmup_count archive_sha256=$envtar_sha256 archive_bytes=$envtar_bytes start_ns=$environment_preparation_start_ns end_ns=$environment_preparation_end_ns measured=0 cache_state=rotated"
fi

if test -n "$batch_manifest"; then
    mkdir -p "$work/active"
    mkdir -p "$work/input-ready"
    run_one() {
        run_label=$1
        ordinal=$2
        relationship=$3
        f_slot=$4
        predictive_path=$5
        source_sha=$6
        item_compile_db=$7
        item_compile_source=$8
        item_compile_output=$9
        payload_sha=${10}
        payload_bytes=${11}
        predecessor_ordinal=${12:--1}
        job_started_ns=$(date +%s%N)
        job_state_path="$work/job-state-$run_label-$ordinal.tsv"
        client_status_path="$work/client-command-status-$run_label-$ordinal.tsv"
        ICECC_P50_CLIENT_STATUS_PATH=$client_status_path
        export ICECC_P50_CLIENT_STATUS_PATH
        timing_path="$work/timing-$run_label-$ordinal.tsv"
        staged="$work/src/$run_label-$ordinal.ii"
        marker="$work/active/$run_label-$relationship-$f_slot"
        input_ready_marker="$work/input-ready/$run_label-$relationship-$ordinal"
        failure_marker="$work/input-ready/$run_label-$relationship-$ordinal.failed"
        owner_token="$run_label:$relationship:$f_slot:$ordinal:$$"
        owner_path="$work/active/.owner-$run_label-$relationship-$f_slot-$ordinal-$$"
        compile_pid=
        owns_marker=0
        input_ready_published=0
        release_planned_lane() {
            if test "$owns_marker" -eq 1; then
                marker_owner=$(cat "$marker" 2>/dev/null || true)
                if test "$marker_owner" = "$owner_token"; then
                    rm -f "$marker"
                fi
            fi
            rm -f "$owner_path"
            owns_marker=0
        }
        run_one_cleanup() {
            run_one_exit_status=$?
            trap - EXIT
            if test -n "$compile_pid" && kill -0 "$compile_pid" 2>/dev/null; then
                kill "$compile_pid" 2>/dev/null || :
                wait "$compile_pid" 2>/dev/null || :
            fi
            release_planned_lane
            if test "$input_ready_published" -eq 0; then
                : >"$failure_marker"
            fi
            job_finished_ns=$(date +%s%N)
            if ! printf '%s\t%s\t%s\n' "$job_started_ns" "$job_finished_ns" \
                    "$run_one_exit_status" >"$job_state_path"; then
                test "$run_one_exit_status" -ne 0 || run_one_exit_status=1
            fi
            exit "$run_one_exit_status"
        }
        trap run_one_cleanup EXIT
        trap 'exit 129' HUP
        trap 'exit 130' INT
        trap 'exit 143' TERM
        rm -f "$job_state_path" "$client_status_path"

        # Only this relationship's immediately preceding source commit gates
        # admission.  Other relationships have independent predecessor
        # markers and therefore reach the C sidecar concurrently.
        if test "$predecessor_ordinal" -ge 0; then
            predecessor_marker="$work/input-ready/$run_label-$relationship-$predecessor_ordinal"
            predecessor_failure="$predecessor_marker.failed"
            while test ! -e "$predecessor_marker"; do
                test ! -e "$predecessor_failure" || {
                    echo "FAIL: predecessor source admission failed ($run_label-$ordinal)" >&2
                    return 1
                }
                # Every later TU waits here concurrently. A 5 ms external
                # sleep creates a fork storm at depth 200 and distorts the
                # product timing it is meant to observe. A 100 ms poll keeps
                # admission responsive without making the harness the load.
                sleep 0.1
            done
        fi

        # f_slot is an authenticated *planned admission lane*, not an observed
        # scheduler slot.  A hard link is the atomic lane acquisition; its
        # owner token prevents a stale wrapper from removing a successor's
        # lease.  Set owns_marker before waiting so an interrupt also removes
        # the private owner file without touching somebody else's marker.
        printf '%s\n' "$owner_token" >"$owner_path"
        owns_marker=1
        while ! ln "$owner_path" "$marker" 2>/dev/null; do sleep 0.005; done
        admission_start_ns=$(date +%s%N)
        cp -- "$predictive_path" "$staged"
        compile_once "$run_label-$ordinal" "$staged" "$item_compile_db" \
            "$item_compile_source" "$item_compile_output" "$relationship" "$f_slot" \
            "$timing_path" &
        compile_pid=$!
        preprocessed_capture="$work/s7-$run_label-$ordinal-preprocessed.ii"
        # ICECC_P50_PREPROCESSED_CAPTURE is written immediately before the
        # exact .ii is attached to the live transaction.  Then wait for the
        # product's explicit source-commit witness.  This bounds the C
        # sidecar's pending admission queue without waiting for compile/result
        # completion.
        while test ! -s "$preprocessed_capture"; do
            if ! kill -0 "$compile_pid" 2>/dev/null; then
                wait "$compile_pid" || true
                echo "FAIL: compile ended before authenticated input-ready ($run_label-$ordinal)" >&2
                return 1
            fi
            sleep 0.005
        done
        while ! grep -Fq 'source committed for P50 CompileFile' \
                "$work/client-compile-$run_label-$ordinal.log"; do
            if ! kill -0 "$compile_pid" 2>/dev/null; then
                wait "$compile_pid" || true
                echo "FAIL: compile ended before product source commit ($run_label-$ordinal)" >&2
                return 1
            fi
            sleep 0.005
        done
        input_ready_ns=$(date +%s%N)
        input_ready_tmp="$input_ready_marker.tmp.$$"
        printf '%s\n' "$input_ready_ns" >"$input_ready_tmp"
        mv -- "$input_ready_tmp" "$input_ready_marker"
        input_ready_published=1
        # compile_once publishes its second timing row immediately after the
        # remote object/result returns and before running the local reference
        # compile.  Release the planned lane at that product boundary so the
        # correctness witness cannot throttle later remote work.
        compile_start_ns=
        compile_end_ns=
        while test -z "$compile_end_ns"; do
            compile_start_ns=$(sed -n '1p' "$timing_path" 2>/dev/null || true)
            compile_end_ns=$(sed -n '2p' "$timing_path" 2>/dev/null || true)
            if test -z "$compile_end_ns" && ! kill -0 "$compile_pid" 2>/dev/null; then
                # The child writes the second row immediately before exit.
                # It can exit between the reads above and kill -0, so consume
                # the final file state before classifying the handoff as lost.
                compile_start_ns=$(sed -n '1p' "$timing_path" 2>/dev/null || true)
                compile_end_ns=$(sed -n '2p' "$timing_path" 2>/dev/null || true)
                if test -z "$compile_end_ns"; then
                    wait "$compile_pid" || true
                    compile_pid=
                    echo "FAIL: compile ended before remote-result timing ($run_label-$ordinal)" >&2
                    return 1
                fi
            fi
            test -n "$compile_end_ns" || sleep 0.005
        done
        test -n "$compile_start_ns" || {
            echo "FAIL: compile timing handoff is incomplete ($run_label-$ordinal)" >&2
            return 1
        }
        release_planned_lane
        if ! wait "$compile_pid"; then
            compile_pid=
            echo "FAIL: compile/local witness failed ($run_label-$ordinal)" >&2
            return 1
        fi
        compile_pid=
        remote_obj="$work/out/remote-$run_label-$ordinal.o"
        local_obj="$work/out/local-$run_label-$ordinal.o"
        remote_sha=$(sha256sum "$remote_obj" | awk '{print $1}')
        remote_bytes=$(stat -c %s "$remote_obj")
        if test "$external_mode" = 1; then
            local_sha=PENDING
            local_bytes=0
        else
            local_sha=$(sha256sum "$local_obj" | awk '{print $1}')
            local_bytes=$(stat -c %s "$local_obj")
        fi
        preprocessed_sha=$(sha256sum "$preprocessed_capture" | awk '{print $1}')
        preprocessed_bytes=$(stat -c %s "$preprocessed_capture")
        test "$preprocessed_sha" = "${10}" && test "$preprocessed_bytes" -eq "${11}" || {
            echo "FAIL: preprocessed payload differs from authenticated predictive descriptor ($run_label-$ordinal)" >&2
            exit 1
        }
        wait_ms=$(sed -nE 's/.*<\/wait for cs: ([0-9]+)ms>.*/\1/p' \
            "$work/client-compile-$run_label-$ordinal.log" | tail -n 1)
        test -n "$wait_ms" || { echo "FAIL: client wait-for-cs timing missing ($run_label-$ordinal)" >&2; return 1; }
        client_log="$work/client-compile-$run_label-$ordinal.log"
        observed_scheduler_job_id=$(sed -nE \
            's/.*Have to use host .* - Job ID: ([0-9]+) - env:.*/\1/p' "$client_log" | tail -n 1)
        observed_source_tu_seq=$(sed -nE \
            's/.*source committed for P50 CompileFile: .* TU sequence ([0-9]+).*/\1/p' "$client_log" | tail -n 1)
        test "$observed_scheduler_job_id" -gt 0 2>/dev/null && \
            test "$observed_source_tu_seq" -ge 0 2>/dev/null || {
            echo "FAIL: observed scheduler/source identity missing ($run_label-$ordinal)" >&2
            return 1
        }
        observed_f_service_identity=
        for _ in $(seq 1 200); do
            observed_f_service_identity=$(sed -nE \
                "s/.*BEGIN: $observed_scheduler_job_id client=[^ ]+ server=([^ (]+).*/\\1/p" \
                "$scheduler_current_log" | tail -n 1)
            test -n "$observed_f_service_identity" && break
            sleep 0.005
        done
        expected_f_service_identity=p50-f
        test "$suite" != C1F20/40 || expected_f_service_identity="p50-f-$relationship"
        test "$observed_f_service_identity" = "$expected_f_service_identity" || {
            echo "FAIL: observed F service differs from planned relationship ($run_label-$ordinal)" >&2
            return 1
        }
        witness_end_ns=$(date +%s%N)
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$ordinal" "$source_sha" "$preprocessed_capture" "$preprocessed_sha" "$preprocessed_bytes" \
            "$remote_obj" "$remote_sha" "$remote_bytes" "$local_obj" "$local_sha" "$local_bytes" \
            "$relationship" "$f_slot" "$admission_start_ns" "$input_ready_ns" \
            "$compile_start_ns" "$compile_end_ns" "$witness_end_ns" \
            "$observed_scheduler_job_id" "$observed_f_service_identity" "$observed_source_tu_seq" \
            >"$work/result-$run_label-$ordinal.tsv"
        printf '%s\n' "$((wait_ms * 1000000))" >"$work/wait-$run_label-$ordinal.ns"
    }
    run_batch() {
        run_label=$1
        emit_rows=${2:-1}
        active_batch_file=${3:-$work/batch.tsv}
        active_batch_expected=${4:-$batch_expected_count}
        batch_allow_failures=${5:-0}
        start_only=${6:-0}
        ordinal=0
        topology_input=/dev/null
        if test "$suite" = C1F20/40; then
            topology_input="$work/topology.tsv"
        fi
        batch_start_ns=$(date +%s%N)
        job_pids=""
        batch_job_pids=""
        job_entries=""
        while IFS="$(printf '\t')" read -r tu_id source_path source_relative source_sha predictive_path predictive_relative payload_sha payload_bytes item_db item_db_sha item_source item_output; do
            if test "$item_db" = "-"; then
                test "$item_db_sha" = "-" && test "$item_source" = "-" && \
                    test "$item_output" = "-" || {
                    echo "FAIL: batch compile binding has partial empty-column sentinels ($run_label-$ordinal)" >&2
                    exit 1
                }
                item_db= item_db_sha= item_source= item_output=
            else
                test "$item_db_sha" != "-" && test "$item_source" != "-" && \
                    test "$item_output" != "-" || {
                    echo "FAIL: batch compile binding has a partial sentinel row ($run_label-$ordinal)" >&2
                    exit 1
                }
            fi
            relationship=0; f_slot=0
            if test "$suite" = C1F20/40; then
                IFS="$(printf '\t')" read -r relationship f_slot <&3
            elif test "$real_scheduler_restart_w30" = 1; then
                f_slot=$((ordinal % 30))
            fi
            predecessor_file="$work/input-ready/$run_label-$relationship.last"
            predecessor_ordinal=-1
            if test "$real_scheduler_restart_w30" != 1 && test -e "$predecessor_file"; then
                predecessor_ordinal=$(cat "$predecessor_file")
                test "$predecessor_ordinal" -ge 0 2>/dev/null && \
                    test "$predecessor_ordinal" -lt "$ordinal" || {
                    echo "FAIL: relationship predecessor state invalid ($run_label-$ordinal)" >&2
                    exit 1
                }
            fi
            printf '%s\n' "$ordinal" >"$predecessor_file"
            run_one "$run_label" "$ordinal" "$relationship" "$f_slot" "$predictive_path" "$source_sha" \
                "$item_db" "$item_source" "$item_output" "$payload_sha" "$payload_bytes" \
                "$predecessor_ordinal" >"$work/job-$run_label-$ordinal.log" 2>&1 &
            job_pid=$!
            job_pids="$job_pids $job_pid"
            job_entries="$job_entries $ordinal:$job_pid"
            batch_job_pids="$job_pids"
            ordinal=$((ordinal + 1))
        done <"$active_batch_file" 3<"${topology_input:-/dev/null}"
        test "$ordinal" -eq "$active_batch_expected" || { echo "FAIL: batch manifest count changed during run" >&2; exit 1; }
        if test "$start_only" = 1; then
            return 0
        fi
        finish_batch
    }
    finish_batch() {
        batch_failed=0
        batch_failures=0
        batch_max_settlement_ms=0
        failed_entries=""
        for entry in $job_entries; do
            job_ordinal=${entry%%:*}
            pid=${entry#*:}
            if wait "$pid"; then
                wait_status=0
            else
                wait_status=$?
                batch_failed=1
                batch_failures=$((batch_failures + 1))
                failed_entries="$failed_entries $job_ordinal:$wait_status"
            fi
            job_state_path="$work/job-state-$run_label-$job_ordinal.tsv"
            test -s "$job_state_path" || {
                echo "FAIL: missing per-client settlement record ($run_label-$job_ordinal)" >&2
                return 1
            }
            IFS="$(printf '\t')" read -r job_started_ns job_finished_ns job_status <"$job_state_path"
            test "$job_status" -eq "$wait_status" 2>/dev/null || {
                echo "FAIL: per-client wait/status record disagrees ($run_label-$job_ordinal wait=$wait_status record=$job_status)" >&2
                return 1
            }
        done
        if test "$batch_failed" -ne 0; then
            batch_job_pids=""
            if test "$batch_allow_failures" = 1; then
                for entry in $job_entries; do
                    job_ordinal=${entry%%:*}
                    job_state_path="$work/job-state-$run_label-$job_ordinal.tsv"
                    IFS="$(printf '\t')" read -r job_started_ns job_finished_ns job_status <"$job_state_path"
                    test "$job_status" -eq 0 2>/dev/null && continue
                    client_status_path="$work/client-command-status-$run_label-$job_ordinal.tsv"
                    test -s "$client_status_path" || {
                        echo "FAIL: failed client wrapper lacks actual client command status ($run_label-$job_ordinal wrapper=$job_status)" >&2
                        return 1
                    }
                    IFS="$(printf '\t')" read -r client_status <"$client_status_path"
                    if test "$client_status" -eq 124 2>/dev/null || \
                            test "$client_status" -eq 137 2>/dev/null || \
                            test "$client_status" -ge 128 2>/dev/null; then
                        echo "FAIL: client command timed out or was signal-terminated ($run_label-$job_ordinal client_status=$client_status wrapper_status=$job_status)" >&2
                        return 1
                    fi
                    test "$client_status" -ne 0 2>/dev/null || {
                        echo "FAIL: wrapper failed although the client command succeeded ($run_label-$job_ordinal wrapper_status=$job_status)" >&2
                        return 1
                    }
                    wrapper_elapsed_ms=$(((job_finished_ns - job_started_ns) / 1000000))
                    test "$wrapper_elapsed_ms" -ge 0 && \
                        test "$wrapper_elapsed_ms" -le 62000 || {
                        echo "FAIL: non-success client exceeded the fixed 60s ARM budget plus 2s cleanup grace ($run_label-$job_ordinal ${wrapper_elapsed_ms}ms)" >&2
                        return 1
                    }
                    client_log="$work/client-compile-$run_label-$job_ordinal.log"
                    if test "$run_label" = old-scheduler; then
                        # This S-only leg deliberately releases an old held
                        # receipt after scheduler replacement. F may commit
                        # that exact source before the caller discovers its
                        # predecessor cannot be retried; verify_old_cohort_retries
                        # below validates the exact Error 24/new-epoch policy.
                        :
                    elif test "$run_label" = active-worker-loss && \
                            test "$job_ordinal" = 0; then
                        # The stopped active compile may already have crossed
                        # COMMIT when its worker session disappears. Strict
                        # P50 must not transparently retry that ambiguous
                        # predecessor; accept only explicit Error 24, then
                        # test recovery with a fresh job below.
                        grep -Fq 'source committed for P50 CompileFile' "$client_log" && \
                            grep -Fq 'got exception Error 24 - local daemon did not settle P50 retry predecessor' "$client_log" && \
                            grep -Fq 'remote-only policy refuses client-error fallback' "$client_log" || {
                            echo "FAIL: lost active strict-P50 victim lacks the documented explicit Error 24 policy result" >&2
                            return 1
                        }
                        echo "S8_REAL_WORKER_LOSS_VICTIM_COMMITTED_BUT_NOT_RETRIED ordinal=0 policy=Error24 remote_only=1"
                    elif grep -Fq 'source committed for P50 CompileFile' "$client_log" || \
                            ! grep -Eq 'got exception Error [0-9]+' "$client_log"; then
                        echo "FAIL: discarded B caller lacks an explicit pre-commit protocol error ($run_label-$job_ordinal status=$job_status)" >&2
                        return 1
                    fi
                    if test "$wrapper_elapsed_ms" -gt "$batch_max_settlement_ms"; then
                        batch_max_settlement_ms=$wrapper_elapsed_ms
                    fi
                done
                precommit_failures=1
                if test "$run_label" = old-scheduler; then precommit_failures=0; fi
                echo "S8_BATCH_SETTLED_WITH_FAILURES run=$run_label count=$ordinal failures=$batch_failures max_failure_settlement_ms=$batch_max_settlement_ms source_arm_budget_ms=60000 cleanup_grace_ms=2000 failures_precommit=$precommit_failures"
                return 0
            fi
            for pid in $job_pids; do kill "$pid" 2>/dev/null || :; done
            for pid in $job_pids; do wait "$pid" 2>/dev/null || :; done
            echo "FAIL: real compile job failed ($run_label)" >&2
            cat "$work"/job-"$run_label"-*.log 2>/dev/null || true
            exit 1
        fi
        batch_job_pids=""
        batch_end_ns=$(date +%s%N)
        if test "$external_mode" = 1; then
            # Correctness witnesses are intentionally post-measurement: no
            # local GCC process can overlap the final remote compile interval.
            for reference in "$work"/local-pending-"$run_label"-*; do
                test -f "$reference" || continue
                ordinal_ref=${reference##*-}
                input_ref=$(sed -n '1p' "$reference")
                db_ref=$(sed -n '2p' "$reference")
                source_ref=$(sed -n '3p' "$reference")
                output_ref=$(sed -n '4p' "$reference")
                local_ref=$(sed -n '5p' "$reference")
                remote_ref=$(sed -n '6p' "$reference")
                remote_sha=$(sha256sum "$remote_ref" | awk '{print $1}')
                remote_bytes=$(stat -c %s "$remote_ref")
                local_witness_start_ns=$(date +%s%N)
                if test "$reference_reuse" = 1; then
                    witness_object=$(python3 - "$work/reference-reuse-map.json" "$ordinal_ref" <<'PY'
import json, sys
value = json.load(open(sys.argv[1], encoding="utf-8"))
matches = [row for row in value.get("records", [])
           if row.get("ordinal") == int(sys.argv[2])]
if len(matches) != 1:
    raise SystemExit("reference witness ordinal missing or duplicated")
print(matches[0]["object_path"])
PY
                    ) || { echo "FAIL: reference witness lookup failed ($run_label-$ordinal_ref)" >&2; exit 1; }
                    test -f "$witness_object" && test ! -L "$witness_object" || {
                        echo "FAIL: retained reference object unavailable ($run_label-$ordinal_ref)" >&2; exit 1;
                    }
                    cmp -s "$remote_ref" "$witness_object" || {
                        echo "FAIL: remote object differs from retained reference witness ($run_label-$ordinal_ref)" >&2
                        exit 1
                    }
                    witness_sha=$(sha256sum "$witness_object" | awk '{print $1}')
                    witness_bytes=$(stat -c %s "$witness_object")
                    test "$witness_sha" = "$remote_sha" && test "$witness_bytes" -eq "$remote_bytes" || {
                        echo "FAIL: retained reference witness descriptor differs ($run_label-$ordinal_ref)" >&2
                        exit 1
                    }
                    cp -- "$witness_object" "$local_ref"
                    local_ref_sha="$witness_sha"
                    local_ref_bytes="$witness_bytes"
                    echo "S8_REFERENCE_REUSE package_sha256=$(python3 - "$work/reference-reuse-map.json" <<'PY'
import json, sys
print(json.load(open(sys.argv[1], encoding="utf-8"))["package_digest"])
PY
                    ) run=$run_label ordinal=$ordinal_ref remote_sha256=$remote_sha remote_bytes=$remote_bytes witness_sha256=$witness_sha witness_bytes=$witness_bytes"
                elif test "$reference_reuse" -eq 0 && test -n "$db_ref"; then
                    local_ref_args=$(compile_args_for "$db_ref" "$source_ref" "$output_ref" "$input_ref" "$local_ref")
                    eval "g++ $local_ref_args"
                elif test "$reference_reuse" -eq 0; then
                    g++ -std=c++17 -O2 -c "$input_ref" -o "$local_ref"
                else
                    echo "FAIL: reference reuse did not produce a witness object" >&2
                    exit 1
                fi
                cmp -s "$remote_ref" "$local_ref" || {
                    echo "FAIL: post-measurement local reference differs ($run_label-$ordinal_ref)" >&2
                    exit 1
                }
                local_ref_sha=$(sha256sum "$local_ref" | awk '{print $1}')
                local_ref_bytes=$(stat -c %s "$local_ref")
                python3 - "$work/result-$run_label-$ordinal_ref.tsv" "$local_ref_sha" "$local_ref_bytes" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
fields = path.read_text(encoding="utf-8").rstrip("\n").split("\t")
if len(fields) != 21:
    raise SystemExit("post-reference result field count invalid")
fields[9], fields[10] = sys.argv[2], sys.argv[3]
path.write_text("\t".join(fields) + "\n", encoding="utf-8")
PY
                echo "S8_EXTERNAL_LOCAL_WITNESS run=$run_label ordinal=$ordinal_ref remote_batch_end_ns=$batch_end_ns local_witness_start_ns=$local_witness_start_ns"
                rm -f "$reference"
            done
        fi
        batch_metrics=$(python3 - "$work" "$run_label" "$ordinal" "$batch_start_ns" \
                "$batch_end_ns" "$relationship_count" "$slots_per_f" \
                "$real_scheduler_restart_w30" <<'PY'
import pathlib, sys

root = pathlib.Path(sys.argv[1])
run, count = sys.argv[2], int(sys.argv[3])
batch_start, batch_end = int(sys.argv[4]), int(sys.argv[5])
relationship_count, slots_per_f = int(sys.argv[6]), int(sys.argv[7])
real_w30 = sys.argv[8] == "1"
records = []
for ordinal in range(count):
    fields = (root / f"result-{run}-{ordinal}.tsv").read_text(encoding="utf-8").rstrip("\n").split("\t")
    if len(fields) != 21:
        raise SystemExit(f"result field count invalid ({run}-{ordinal})")
    record = {
        "ordinal": int(fields[0]), "relationship": int(fields[11]), "lane": int(fields[12]),
        "admission_start": int(fields[13]), "input_ready": int(fields[14]),
        "compile_start": int(fields[15]), "compile_end": int(fields[16]),
        "witness_end": int(fields[17]), "job_id": int(fields[18]),
        "service": fields[19], "tu_seq": int(fields[20]),
    }
    if (record["ordinal"] != ordinal or
            not 0 <= record["relationship"] < relationship_count or
            not 0 <= record["lane"] < slots_per_f or
            record["service"] != (f"p50-f-{record['relationship']}"
                                  if relationship_count > 1 else "p50-f") or
            not batch_start <= record["admission_start"] <= record["compile_start"] <=
                record["input_ready"] <= record["compile_end"] <= record["witness_end"] <= batch_end):
        raise SystemExit(f"result concurrency identity invalid ({run}-{ordinal})")
    records.append(record)
if len({record["job_id"] for record in records}) != count:
    raise SystemExit(f"scheduler job identity is not unique ({run})")

def peak(intervals):
    events = [(start, 1) for start, _ in intervals] + [(end, -1) for _, end in intervals]
    active = maximum = 0
    for _, delta in sorted(events, key=lambda item: (item[0], item[1])):
        active += delta
        maximum = max(maximum, active)
    return maximum

per_relationship = []
for relationship in range(relationship_count):
    selected = sorted((record for record in records if record["relationship"] == relationship),
                      key=lambda record: record["ordinal"])
    if not selected:
        raise SystemExit(f"relationship {relationship} has no work ({run})")
    if real_w30:
        wire_sequences = sorted(record["tu_seq"] for record in selected)
        if (len(set(wire_sequences)) != len(selected) or
                any(right != left + 1
                    for left, right in zip(wire_sequences, wire_sequences[1:]))):
            raise SystemExit(f"relationship wire TU sequence is not unique and contiguous ({run}-{relationship})")
    else:
        for previous, current in zip(selected, selected[1:]):
            if (current["admission_start"] < previous["input_ready"] or
                    current["tu_seq"] != previous["tu_seq"] + 1):
                raise SystemExit(f"relationship admission order invalid ({run}-{relationship})")
    for lane in range(slots_per_f):
        lane_rows = [record for record in selected if record["lane"] == lane]
        for previous, current in zip(lane_rows, lane_rows[1:]):
            if current["admission_start"] < previous["compile_end"]:
                raise SystemExit(f"planned admission lane overlapped ({run}-{relationship}-{lane})")
    relationship_peak = peak([(record["admission_start"], record["compile_end"])
                              for record in selected])
    if relationship_peak > slots_per_f:
        raise SystemExit(f"relationship active-job cap exceeded ({run}-{relationship})")
    per_relationship.append(relationship_peak)

admissions = [(record["admission_start"], record["input_ready"]) for record in records]
compiles = [(record["compile_start"], record["compile_end"]) for record in records]
admitted_or_compiling = [(record["admission_start"], record["compile_end"])
                         for record in records]
remote_end = max(record["compile_end"] for record in records)
print(" ".join((
    f"makespan_ns={remote_end - batch_start}",
    f"harness_completion_ns={batch_end - batch_start}",
    f"max_concurrent_source_admissions={peak(admissions)}",
    f"max_concurrent_compile_result_jobs={peak(compiles)}",
    f"max_concurrent_admitted_or_compiling_jobs={peak(admitted_or_compiling)}",
    f"max_concurrent_active_per_relationship={max(per_relationship)}",
)))
PY
)
        if test "$emit_rows" = 1; then
            printf 'S8_BATCH_WINDOW run=%s start_ns=%s end_ns=%s\n' \
                "$run_label" "$batch_start_ns" "$batch_end_ns"
            printf 'S8_BATCH_METRICS run=%s %s\n' "$run_label" "$batch_metrics"
        fi
        ordinal=0
        while IFS="$(printf '\t')" read -r tu_id source_path source_relative source_sha predictive_path predictive_relative payload_sha payload_bytes item_db item_db_sha item_source item_output; do
            result=$(cat "$work/result-$run_label-$ordinal.tsv")
            IFS="$(printf '\t')" read -r _ result_source_sha preprocessed_capture preprocessed_sha preprocessed_bytes remote_obj remote_sha remote_bytes local_obj local_sha local_bytes planned_relationship planned_admission_lane admission_start_ns input_ready_ns compile_start_ns compile_end_ns witness_end_ns observed_scheduler_job_id observed_f_service_identity observed_source_tu_seq <<EOF
$result
EOF
            test "$result_source_sha" = "$source_sha" || {
                echo "FAIL: result/source binding changed ($run_label-$ordinal)" >&2
                exit 1
            }
            wait_ns=$(cat "$work/wait-$run_label-$ordinal.ns")
            if test "$emit_rows" = 1; then
                printf 'S8_BATCH_TU run=%s ordinal=%s tu_id=%s source_sha256=%s preprocessed_path=%s preprocessed_sha256=%s preprocessed_bytes=%s remote_path=%s remote_sha256=%s remote_bytes=%s local_path=%s local_sha256=%s local_bytes=%s admission_start_ns=%s input_ready_ns=%s compile_start_ns=%s compile_end_ns=%s witness_end_ns=%s wait_for_cs_ns=%s planned_assignment_ordinal=%s planned_relationship=%s planned_admission_lane=%s observed_scheduler_job_id=%s observed_f_service_identity=%s observed_source_tu_seq=%s\n' \
                    "$run_label" "$ordinal" "$tu_id" "$source_sha" "$preprocessed_capture" "$preprocessed_sha" "$preprocessed_bytes" \
                    "$remote_obj" "$remote_sha" "$remote_bytes" "$local_obj" "$local_sha" "$local_bytes" \
                    "$admission_start_ns" "$input_ready_ns" "$compile_start_ns" "$compile_end_ns" \
                    "$witness_end_ns" "$wait_ns" \
                    "$([ "$suite" = C1F20/40 ] && printf '%s' "$ordinal" || printf '0')" \
                    "$planned_relationship" "$planned_admission_lane" "$observed_scheduler_job_id" \
                    "$observed_f_service_identity" "$observed_source_tu_seq"
            fi
            ordinal=$((ordinal + 1))
        done <"$active_batch_file"
        echo "S8_BATCH_COMPLETE run=$run_label count=$ordinal"
    }
    publish_gate_marker() {
        marker_path=$1
        marker_tmp="$marker_path.tmp.$$"
        : >"$marker_tmp"
        mv -f -- "$marker_tmp" "$marker_path"
    }
    scheduler_epoch_from_log() {
        sed -nE 's/.*assignment fence: strict-nonce epoch=([0-9]+).*/\1/p' "$1" | head -n 1
    }
    capture_old_scheduler_assignments() {
        old_scheduler_epoch=$(scheduler_epoch_from_log "$work/scheduler.log")
        test -n "$old_scheduler_epoch" || {
            echo "FAIL: original scheduler strict-nonce epoch was not logged" >&2
            return 1
        }
        : >"$work/old-scheduler-assignments.tsv"
        seen_old_assignments=0
        for assignment_ordinal in $(seq 0 29); do
            compile_log="$work/client-compile-old-scheduler-$assignment_ordinal.log"
            job_id=$(sed -nE \
                's/.*Have to use host .* - Job ID: ([0-9]+) - env:.*/\1/p' \
                "$compile_log" | head -n 1)
            test -n "$job_id" || {
                echo "FAIL: held old cohort lacks original scheduler assignment ($assignment_ordinal)" >&2
                return 1
            }
            assignment=$(sed -nE \
                's/.*P50 assignment identity bound for job ([0-9]+) epoch ([0-9]+) nonce ([0-9]+).*/\1 \2 \3/p' \
                "$compile_log" | head -n 1)
            read -r bound_job_id bound_epoch bound_nonce <<EOF_ASSIGNMENT
$assignment
EOF_ASSIGNMENT
            test "$bound_job_id" = "$job_id" && \
                test "$bound_epoch" = "$old_scheduler_epoch" && \
                case "$bound_nonce" in ''|*[!0-9]*) false ;; *[1-9]*) true ;; *) false ;; esac || {
                echo "FAIL: old wrapper lacks exact original scheduler epoch/nonce binding (ordinal=$assignment_ordinal job=$job_id identity=$assignment)" >&2
                return 1
            }
            grep -F "put $job_id in joblist of p50-f" "$work/scheduler.log" >/dev/null 2>&1 || {
                echo "FAIL: original scheduler did not dispatch old job $job_id to the expected F worker" >&2
                return 1
            }
            if grep -F "BEGIN: $job_id " "$work/scheduler.log" >/dev/null 2>&1; then
                echo "FAIL: old receipt-held job $job_id began compiler execution before input attachment" >&2
                return 1
            fi
            printf '%s\t%s\t%s\n' "$job_id" "$bound_epoch" "$bound_nonce" \
                >>"$work/old-scheduler-assignments.tsv"
            seen_old_assignments=$((seen_old_assignments + 1))
        done
        unique_old_assignments=$(cut -f1 "$work/old-scheduler-assignments.tsv" | sort -u | wc -l)
        test "$seen_old_assignments" -eq 30 && test "$unique_old_assignments" -eq 30 || {
            echo "FAIL: held old cohort does not have 30 unique scheduler assignment identities" >&2
            return 1
        }
        echo "S8_REAL_S_OLD_ASSIGNMENTS_CAPTURED count=$seen_old_assignments old_epoch=$old_scheduler_epoch"
    }
    verify_replacement_epoch() {
        new_scheduler_epoch=$(scheduler_epoch_from_log "$work/scheduler-replacement.log")
        test -n "$new_scheduler_epoch" && test "$new_scheduler_epoch" != "$old_scheduler_epoch" || {
            echo "FAIL: replacement scheduler did not establish a distinct strict-nonce epoch" >&2
            cat "$work/scheduler-replacement.log" >&2 || true
            return 1
        }
        echo "S8_REAL_S_EPOCH_REPLACED old_epoch=$old_scheduler_epoch new_epoch=$new_scheduler_epoch old_assignment_witnesses=30"
    }
    verify_old_cohort_retries() {
        old_successes=0
        old_reassignments=0
        old_expected_failures=0
        for old_ordinal in $(seq 0 29); do
            result_file="$work/result-old-scheduler-$old_ordinal.tsv"
            client_log="$work/client-compile-old-scheduler-$old_ordinal.log"
            if ! test -f "$result_file"; then
                grep -Fq 'got exception Error 24 - local daemon did not settle P50 retry predecessor' \
                    "$client_log" && \
                grep -Fq 'remote-only policy refuses client-error fallback' "$client_log" || {
                    echo "FAIL: old wrapper did not settle through the expected fenced-assignment path (ordinal=$old_ordinal)" >&2
                    return 1
                }
                old_expected_failures=$((old_expected_failures + 1))
                continue
            fi
            old_successes=$((old_successes + 1))
            job_id=$(cut -f19 "$result_file")
            final_identity=$(sed -nE \
                's/.*P50 assignment identity bound for job ([0-9]+) epoch ([0-9]+) nonce ([0-9]+).*/\1 \2 \3/p' \
                "$client_log" | tail -n 1)
            read -r final_job_id final_epoch final_nonce <<EOF_FINAL_IDENTITY
$final_identity
EOF_FINAL_IDENTITY
            test -n "$job_id" && test "$final_job_id" = "$job_id" && \
                test "$final_epoch" = "$new_scheduler_epoch" && \
                case "$final_nonce" in ''|*[!0-9]*) false ;; *[1-9]*) true ;; *) false ;; esac && \
                grep -F "BEGIN: $job_id " "$work/scheduler-replacement.log" >/dev/null 2>&1 || {
                echo "FAIL: old wrapper success lacks a final new-epoch assignment witness (ordinal=$old_ordinal job=$job_id identity=$final_identity)" >&2
                return 1
            }
            old_reassignments=$((old_reassignments + 1))
        done
        test "$old_successes" -eq "$old_reassignments" && \
            test $((old_successes + old_expected_failures)) -eq 30 || return 1
        if tail -c +$((old_epoch_attach_offset + 1)) "$work/f.log" | \
                grep -E "P50_INPUT_ATTACH_(BEGIN|END) job=[0-9]+ epoch=$old_scheduler_epoch " \
                >/dev/null 2>&1; then
            echo "FAIL: F attached a compiler input under the retired scheduler epoch $old_scheduler_epoch" >&2
            return 1
        fi
        echo "S8_REAL_S_OLD_COHORT_SETTLED expected=30 completed=$old_successes new_epoch_reassignments=$old_reassignments expected_fenced_failures=$old_expected_failures all_callers_joined=1 no_old_epoch_attach=1"
    }
    wait_gate_marker() {
        marker_name=$1
        marker_deadline=$(( $(date +%s) + 70 ))
        while test "$(date +%s)" -lt "$marker_deadline"; do
            test ! -e "$receipt_gate_dir/failed" || {
                cat "$receipt_gate_dir/failed" >&2
                cat "$work/receipt-gate.log" >&2 || true
                echo "FAIL: receipt gate failed while waiting for $marker_name" >&2
                return 1
            }
            test -e "$receipt_gate_dir/$marker_name" && return 0
            kill -0 "$receipt_gate_pid" 2>/dev/null || {
                cat "$work/receipt-gate.log" >&2 || true
                echo "FAIL: receipt gate exited before $marker_name" >&2
                return 1
            }
            sleep 0.05
        done
        echo "FAIL: receipt gate timed out waiting for $marker_name" >&2
        return 1
    }
    restart_real_scheduler() {
        scheduler_pid_before=$sched_pid
        f_daemon_pid_before=$worker_pid
        c_daemon_pid_before=$client_pid
        f_service_pid_before=$service_pid
        c_service_pid_before=$client_service_pid
        kill -TERM "$scheduler_pid_before" 2>/dev/null || :
        wait "$scheduler_pid_before" 2>/dev/null || :
        sched_pid=
        : >"$work/scheduler-replacement.log"
        chmod 0666 "$work/scheduler-replacement.log"
        "$build/scheduler/icecc-scheduler" -p "$port_sched" -n "$network" \
            --assignment-fence-mode strict-nonce -l "$work/scheduler-replacement.log" -vvv &
        sched_pid=$!
        scheduler_current_log="$work/scheduler-replacement.log"
        scheduler_ready=0
        for _ in $(seq 1 100); do
            replacement_c_logins=$(grep -F -c 'login p50-c protocol version:' "$work/scheduler-replacement.log" 2>/dev/null || true)
            replacement_f_logins=$(grep -F -c 'login p50-f protocol version:' "$work/scheduler-replacement.log" 2>/dev/null || true)
            if test "${replacement_c_logins:-0}" -ge 1 && \
                    test "${replacement_f_logins:-0}" -ge 1; then
                scheduler_ready=1
                break
            fi
            kill -0 "$sched_pid" 2>/dev/null || break
            sleep 0.1
        done
        test "$scheduler_ready" -eq 1 || {
            echo "FAIL: real replacement scheduler did not receive C/F re-logins" >&2
            cat "$work/scheduler-replacement.log" >&2 || true
            return 1
        }
        stable_processes=1
        for process in "replacement_scheduler:$sched_pid" "F_daemon:$worker_pid" \
                "C_daemon:$client_pid" "F_cache:$service_pid" "C_cache:$client_service_pid"; do
            process_name=${process%%:*}
            process_pid=${process#*:}
            if kill -0 "$process_pid" 2>/dev/null; then
                process_state=$(ps -p "$process_pid" -o stat=,args= 2>/dev/null || true)
                echo "S8_REAL_S_PROCESS_CHECK role=$process_name pid=$process_pid state=$process_state"
            elif test "$process_name" = F_cache && \
                    test "${worker_loss_allow_f_sidecar_retirement:-0}" = 1; then
                echo "S8_REAL_S_PROCESS_CHECK role=$process_name pid=$process_pid state=missing cause_pending=worker_loss_handle_end_timeout"
            else
                echo "S8_REAL_S_PROCESS_CHECK role=$process_name pid=$process_pid state=missing"
                stable_processes=0
            fi
        done
        test "$stable_processes" -eq 1 || {
            echo "FAIL: scheduler restart changed or lost a C/F daemon/cache process" >&2
            return 1
        }
        echo "S8_REAL_S_RESTART old_scheduler_pid=$scheduler_pid_before new_scheduler_pid=$sched_pid c_daemon_pid=$client_pid f_daemon_pid=$worker_pid c_cache_pid=$client_service_pid f_cache_pid=$service_pid replacement_c_logins=$replacement_c_logins replacement_f_logins=$replacement_f_logins"
    }
    verify_results_from_f_store() {
        result_label=$1
        expected_guid=$2
        result_trace_start_offset=$3
        f_trace_start_offset=$4
        minimum_results=$5
        python3 - "$work" "$source_result_trace" "$f_action_trace" "$result_label" \
            "$expected_guid" "$result_trace_start_offset" "$f_trace_start_offset" \
            "$minimum_results" "$profile_marker" <<'PY'
import glob, json, os, re, sys

work, source_trace_path, f_trace_path, label, expected_guid, \
    source_offset_text, f_offset_text, minimum_text, profile = sys.argv[1:]
source_offset, f_offset, minimum = map(
    int, (source_offset_text, f_offset_text, minimum_text))
internal_profile = {
    "P29V1": "p29_v1",
    "ZSTD_TU": "zstd_tu",
    "ZSTD_ROUTE": "zstd_route",
}.get(profile)
if internal_profile is None:
    raise SystemExit(f"unknown public profile name: {profile}")
prefix = os.path.join(work, f"result-{label}-")
rows = []
for path in sorted(glob.glob(prefix + "*.tsv")):
    fields = open(path, encoding="utf-8").read().rstrip("\n").split("\t")
    if len(fields) != 21:
        raise SystemExit(f"malformed exact result row: {path}")
    rows.append(fields)
if len(rows) < minimum:
    raise SystemExit(f"only {len(rows)} verified outputs for {label}, expected at least {minimum}")
source_data = open(source_trace_path, "rb").read()
f_data = open(f_trace_path, "rb").read()
if source_offset > len(source_data) or f_offset > len(f_data):
    raise SystemExit("source/F trace shrank across F sidecar restart")
source_results = {}
for line in source_data[source_offset:].splitlines():
    try:
        record = json.loads(line)
    except Exception as error:
        raise SystemExit(f"malformed source-result trace record: {error}")
    if record.get("status") == 0:
        key = (int(record["wire_job_id"]), int(record["assignment_epoch"]),
               int(record["assignment_nonce"]), int(record["tu_seq"]))
        source_results.setdefault(key, set()).add(
            (record.get("c_store_guid"), record.get("profile"),
             record.get("raw_digest"), int(record["raw_bytes"])))
f_commits = set()
for line in f_data[f_offset:].splitlines():
    try:
        record = json.loads(line)
    except Exception as error:
        raise SystemExit(f"malformed post-restart F action record: {error}")
    if (record.get("actor") == "F" and record.get("action") == "INPUT_COMMITTED" and
            record.get("f_store_guid") == expected_guid):
        f_commits.add((record.get("c_store_guid"), record.get("profile"),
                       int(record["tu_seq"]), record.get("raw_digest")))
for row in rows:
    scheduler_job_id, tu_seq = int(row[18]), int(row[20])
    expected_raw_bytes = int(row[4])
    log_path = os.path.join(work, f"client-compile-{label}-{int(row[0])}.log")
    client_log = open(log_path, encoding="utf-8", errors="replace").read()
    assignments = re.findall(
        r"P50 assignment identity bound for job ([0-9]+) epoch ([0-9]+) nonce ([0-9]+)",
        client_log)
    if not assignments:
        raise SystemExit(f"successful {label} output job={scheduler_job_id} has no assignment witness")
    final_job, epoch_text, nonce_text = assignments[-1]
    if int(final_job) != scheduler_job_id:
        raise SystemExit(f"successful {label} output job identity differs from final assignment")
    exact_source = source_results.get(
        (scheduler_job_id, int(epoch_text), int(nonce_text), tu_seq), set())
    if not any(source_raw_bytes == expected_raw_bytes and
               (c_guid, internal_profile, tu_seq, digest) in f_commits
               for c_guid, source_profile, digest, source_raw_bytes in exact_source
               if source_profile == profile):
        raise SystemExit(
            f"successful {label} output job={scheduler_job_id} epoch={epoch_text} "
            f"nonce={nonce_text} TU={tu_seq} has no exact "
            f"source-result/INPUT_COMMITTED chain from restarted F store {expected_guid}")
print(f"S8_REAL_S_F_STORE_RESULT_BIND label={label} outputs={len(rows)} "
      f"matched_new_f_store={len(rows)} F_STORE_GUID={expected_guid}")
PY
    }
    verify_bounded_batch_settlement() {
        settle_label=$1
        settle_count=$2
        max_success_ms=0
        max_failure_ms=0
        success_count=0
        failure_count=0
        for settle_ordinal in $(seq 0 $((settle_count - 1))); do
            state_path="$work/job-state-$settle_label-$settle_ordinal.tsv"
            test -s "$state_path" || {
                echo "FAIL: missing exact B caller settlement state ($settle_label-$settle_ordinal)" >&2
                return 1
            }
            IFS="$(printf '\t')" read -r started_ns finished_ns exit_status <"$state_path"
            wrapper_elapsed_ms=$(((finished_ns - started_ns) / 1000000))
            client_status_path="$work/client-command-status-$settle_label-$settle_ordinal.tsv"
            test -s "$client_status_path" || {
                echo "FAIL: settled B caller lacks actual client command status ($settle_label-$settle_ordinal)" >&2
                return 1
            }
            IFS="$(printf '\t')" read -r client_status <"$client_status_path"
            if test "$client_status" -eq 124 2>/dev/null || \
                    test "$client_status" -eq 137 2>/dev/null || \
                    test "$client_status" -ge 128 2>/dev/null; then
                echo "FAIL: B client command timed out or was signal-terminated ($settle_label-$settle_ordinal client_status=$client_status wrapper_status=$exit_status)" >&2
                return 1
            fi
            if test "$exit_status" -eq 0 2>/dev/null; then
                test "$client_status" -eq 0 2>/dev/null || {
                    echo "FAIL: B wrapper succeeded with a nonzero client command status ($settle_label-$settle_ordinal client_status=$client_status)" >&2
                    return 1
                }
                timing_path="$work/timing-$settle_label-$settle_ordinal.tsv"
                test "$(wc -l <"$timing_path")" -eq 2 || {
                    echo "FAIL: successful B caller lacks exact remote compile timing ($settle_label-$settle_ordinal)" >&2
                    return 1
                }
                compile_started_ns=$(sed -n '1p' "$timing_path")
                compile_finished_ns=$(sed -n '2p' "$timing_path")
                source_elapsed_ms=$(((compile_finished_ns - compile_started_ns) / 1000000))
                test "$source_elapsed_ms" -ge 0 && test "$source_elapsed_ms" -le 60000 && \
                        test -f "$work/result-$settle_label-$settle_ordinal.tsv" || {
                    echo "FAIL: B success did not settle within the fixed 60s source budget ($settle_label-$settle_ordinal ${source_elapsed_ms}ms)" >&2
                    return 1
                }
                success_count=$((success_count + 1))
                test "$source_elapsed_ms" -le "$max_success_ms" || max_success_ms=$source_elapsed_ms
            else
                test "$client_status" -ne 0 2>/dev/null || {
                    echo "FAIL: B wrapper failed although client command returned success ($settle_label-$settle_ordinal wrapper_status=$exit_status)" >&2
                    return 1
                }
                test "$wrapper_elapsed_ms" -ge 0 && test "$wrapper_elapsed_ms" -le 62000 || {
                    echo "FAIL: B failure exceeded the fixed 60s source budget plus 2s cleanup grace ($settle_label-$settle_ordinal ${wrapper_elapsed_ms}ms)" >&2
                    return 1
                }
                client_log="$work/client-compile-$settle_label-$settle_ordinal.log"
                if grep -Fq 'source committed for P50 CompileFile' "$client_log" || \
                        ! grep -Eq 'got exception Error [0-9]+' "$client_log"; then
                    echo "FAIL: B non-success lacks a pre-commit protocol error witness ($settle_label-$settle_ordinal status=$exit_status)" >&2
                    return 1
                fi
                failure_count=$((failure_count + 1))
                test "$wrapper_elapsed_ms" -le "$max_failure_ms" || max_failure_ms=$wrapper_elapsed_ms
            fi
        done
        echo "S8_REAL_S_BOUNDED_SETTLEMENT label=$settle_label total=$settle_count successes=$success_count failures=$failure_count max_success_client_ms=$max_success_ms max_failure_client_ms=$max_failure_ms arm_budget_ms=60000 failure_cleanup_grace_ms=2000"
    }
    read_compiler_identity() {
        python3 - "$1" <<'PY'
import pathlib, sys
pid = int(sys.argv[1])
text = pathlib.Path(f"/proc/{pid}/stat").read_text()
fields = text[text.rfind(")") + 2:].split()
# The tail begins at proc stat field 3 (state); pgrp is field 5 and
# starttime is field 22, hence tail offsets 2 and 19.
print(pid, fields[2], fields[19], fields[0])
PY
    }
    run_real_worker_session_loss() {
        old_worker_pid=$worker_pid
        old_f_service_pid=$service_pid
        ready_snapshot "$work/ready-f.trace" || return 1
        old_f_store_guid=$ready_f_guid

        run_batch active-worker-loss 1 "$work/batch-old-scheduler.tsv" 1 1 1
        old_scheduler_epoch=$(scheduler_epoch_from_log "$work/scheduler.log")
        test -n "$old_scheduler_epoch" || {
            echo "FAIL: original scheduler strict-nonce epoch was not logged" >&2
            return 1
        }
        echo "S8_REAL_WORKER_LOSS_SCOPE committed_active_victim=1 scheduler_pid=$sched_pid worker_pid=$old_worker_pid f_service_pid=$old_f_service_pid epoch=$old_scheduler_epoch"

        # The heavy ordinal-0 TU is the only deliberately long compile.  The
        # worker emits an exact job/epoch/nonce/PID/PGID marker after process
        # group ownership is established.  Stop only that verified child so
        # scheduler-session loss must exercise the bounded KILL/reap path.
        victim_client_log="$work/client-compile-active-worker-loss-0.log"
        victim_assign_deadline=$(( $(date +%s) + 20 ))
        while test "$(date +%s)" -lt "$victim_assign_deadline"; do
            victim_job_id=$(sed -nE \
                's/.*Have to use host .* - Job ID: ([0-9]+) - env:.*/\1/p' \
                "$victim_client_log" 2>/dev/null | tail -n 1)
            victim_assignment=$(sed -nE \
                's/.*P50 assignment identity bound for job ([0-9]+) epoch ([0-9]+) nonce ([0-9]+).*/\1 \2 \3/p' \
                "$victim_client_log" 2>/dev/null | tail -n 1)
            test -n "$victim_job_id" && test -n "$victim_assignment" && break
            sleep 0.02
        done
        read -r victim_bound_job victim_epoch victim_nonce <<EOF_VICTIM_ASSIGNMENT
$victim_assignment
EOF_VICTIM_ASSIGNMENT
        test "$victim_job_id" -gt 0 2>/dev/null && \
                test "$victim_bound_job" = "$victim_job_id" && \
                test "$victim_epoch" = "$old_scheduler_epoch" || {
            echo "FAIL: long compiler victim lacks exact old-session job/epoch binding" >&2
            return 1
        }
        victim_marker=
        child_deadline=$(( $(date +%s) + 20 ))
        while test "$(date +%s)" -lt "$child_deadline"; do
            victim_marker=$(sed -nE \
                "s/.*P50_TEST_COMPILER_CHILD job=$victim_job_id epoch=$victim_epoch nonce=$victim_nonce pid=([0-9]+) pgid=([0-9]+).*/\\1 \\2/p" \
                "$work/f.log" | tail -n 1)
            test -n "$victim_marker" && break
            if test -f "$work/result-active-worker-loss-0.tsv"; then
                echo "FAIL: long compiler victim completed before its child identity was observed" >&2
                return 1
            fi
            sleep 0.01
        done
        read -r victim_pid victim_pgid <<EOF_VICTIM_CHILD
$victim_marker
EOF_VICTIM_CHILD
        test "$victim_pid" -gt 0 2>/dev/null && test "$victim_pgid" = "$victim_pid" || {
            echo "FAIL: worker did not expose the exact P50 compiler process group" >&2
            return 1
        }
        victim_before=$(read_compiler_identity "$victim_pid") || {
            echo "FAIL: exact compiler PID disappeared before the quiescence barrier" >&2
            return 1
        }
        read -r observed_pid observed_pgid victim_start_ticks victim_state <<EOF_VICTIM_ID
$victim_before
EOF_VICTIM_ID
        test "$observed_pid" = "$victim_pid" && \
                test "$observed_pgid" = "$victim_pgid" && \
                test "$victim_state" != Z || {
            echo "FAIL: compiler PID/PGID/starttime witness is not a live owned child" >&2
            return 1
        }
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$victim_job_id" "$victim_epoch" "$victim_nonce" "$victim_pid" \
            "$victim_pgid" "$victim_start_ticks" >"$work/worker-loss-victim.tsv"
        kill -STOP "$victim_pid" || {
            echo "FAIL: could not hold exact test-owned compiler child" >&2
            return 1
        }
        victim_stopped=0
        for _ in $(seq 1 100); do
            stopped_identity=$(read_compiler_identity "$victim_pid") || {
                echo "FAIL: stopped compiler PID disappeared before scheduler-session loss" >&2
                return 1
            }
            read -r stopped_pid stopped_pgid stopped_ticks stopped_state <<EOF_VICTIM_STOPPED
$stopped_identity
EOF_VICTIM_STOPPED
            test "$stopped_pid" = "$victim_pid" && \
                    test "$stopped_pgid" = "$victim_pgid" && \
                    test "$stopped_ticks" = "$victim_start_ticks" || {
                echo "FAIL: compiler identity changed while waiting for SIGSTOP delivery" >&2
                return 1
            }
            case "$stopped_state" in
                T|t) victim_stopped=1; break ;;
                Z) echo "FAIL: compiler exited before SIGSTOP was observed" >&2; return 1 ;;
            esac
            sleep 0.01
        done
        test "$victim_stopped" -eq 1 || {
            echo "FAIL: exact compiler identity did not enter stopped state after bounded observations" >&2
            return 1
        }
        echo "S8_REAL_WORKER_LOSS_VICTIM_STOPPED job=$victim_job_id epoch=$victim_epoch nonce=$victim_nonce pid=$victim_pid pgid=$victim_pgid start_ticks=$victim_start_ticks state=$stopped_state"

        test ! -e "$work/result-active-worker-loss-0.tsv" || {
            echo "FAIL: stopped victim produced a successful compiler result before session loss" >&2
            return 1
        }
        echo "S8_REAL_WORKER_LOSS_PRE_RESTART_OUTPUTS exact=1 victim_result=absent worker_pid=$old_worker_pid"

        worker_loss_allow_f_sidecar_retirement=1
        restart_real_scheduler || return 1
        worker_loss_allow_f_sidecar_retirement=0
        new_scheduler_epoch=$(scheduler_epoch_from_log "$work/scheduler-replacement.log")
        test -n "$new_scheduler_epoch" && test "$new_scheduler_epoch" != "$old_scheduler_epoch" || {
            echo "FAIL: replacement scheduler did not establish a distinct worker session" >&2
            return 1
        }
        quiescence_deadline=$(( $(date +%s) + 15 ))
        while test "$(date +%s)" -lt "$quiescence_deadline"; do
            grep -F "session quiescence settled compiler pid=$victim_pid pgid=$victim_pgid generation=" \
                "$work/f.log" >/dev/null 2>&1 && break
            sleep 0.05
        done
        grep -F "session quiescence TERM compiler pid=$victim_pid pgid=$victim_pgid generation=" \
            "$work/f.log" >/dev/null 2>&1 || {
            echo "FAIL: F did not TERM the exact stopped compiler group on scheduler-session loss" >&2
            return 1
        }
        grep -F "session quiescence KILL compiler pid=$victim_pid pgid=$victim_pgid generation=" \
            "$work/f.log" >/dev/null 2>&1 || {
            echo "FAIL: F did not escalate against the exact stopped compiler group" >&2
            return 1
        }
        grep -F "session quiescence settled compiler pid=$victim_pid pgid=$victim_pgid generation=" \
            "$work/f.log" >/dev/null 2>&1 || {
            echo "FAIL: F did not reap and settle the exact old-session compiler group" >&2
            return 1
        }
        if kill -0 "$victim_pid" 2>/dev/null; then
            echo "FAIL: settled compiler leader PID remains present after exact reap" >&2
            return 1
        fi
        remaining_group_members=$(ps -eo pid=,pgid=,stat= | awk -v pgid="$victim_pgid" \
            '$2 == pgid { print $1 "/" $3 }')
        test -z "$remaining_group_members" || {
            echo "FAIL: old compiler process group still has members after settlement: $remaining_group_members" >&2
            return 1
        }
        settled_line=$(grep -nF \
            "session quiescence settled compiler pid=$victim_pid pgid=$victim_pgid generation=" \
            "$work/f.log" | tail -n 1 | cut -d: -f1)
        replacement_connection_line=$(grep -nF 'Connected to scheduler (I am known as ' \
            "$work/f.log" | tail -n 1 | cut -d: -f1)
        test -n "$settled_line" && test -n "$replacement_connection_line" && \
                test "$settled_line" -lt "$replacement_connection_line" || {
            echo "FAIL: replacement F scheduler session activated before old compiler-group settlement" >&2
            return 1
        }
        echo "S8_REAL_WORKER_LOSS_ORDER exact_group_settled_f_log_line=$settled_line replacement_F_scheduler_connection_line=$replacement_connection_line group_members=0"
        test "$(kill -0 "$old_worker_pid" 2>/dev/null; printf '%s' "$?")" = 0 || {
            echo "FAIL: global scheduler-session replacement unexpectedly lost F daemon process" >&2
            return 1
        }
        if ! kill -0 "$old_f_service_pid" 2>/dev/null; then
            # A stopped compiler makes handle_end wait for exact group
            # quiescence.  If that bounded wait consumes the input lifecycle
            # deadline, the adapter must retire A and replace the sidecar.
            # Accept that *only* with the complete, job-scoped causal trace;
            # this exception is specific to active compiler loss and does
            # not relax ordinary scheduler-bounce process stability.
            grep -F "P50 input settlement job $victim_job_id action 1 status timeout reason handle_end" \
                    "$work/f.log" >/dev/null 2>&1 && \
                grep -F "session quiescence settled compiler pid=$victim_pid pgid=$victim_pgid generation=" \
                    "$work/f.log" >/dev/null 2>&1 || {
                echo "FAIL: F sidecar disappeared without exact handle_end timeout after compiler quiescence" >&2
                return 1
            }
            echo "S8_REAL_WORKER_LOSS_F_SIDECAR_RETIRED cause=handle_end_input_lifecycle_timeout job=$victim_job_id exact_quiescence=1 ordinary_bounce_exception=0"
        else
            echo "S8_REAL_WORKER_LOSS_F_SIDECAR_RETAINED pid=$old_f_service_pid"
        fi
        if test "$expect_stable_f" = 1; then
            if grep -F "P50 input settlement job $victim_job_id action 1 status timeout reason handle_end" \
                    "$work/f.log" >/dev/null 2>&1; then
                echo "FAIL: strict stable-F expectation observed handle_end input lifecycle timeout" >&2
                return 1
            fi
            lifecycle_record=$(grep -F \
                "P50 input settlement job $victim_job_id action 1 status " \
                "$work/f.log" | head -n 1 || true)
            case "$lifecycle_record" in
                *" status applied reason handle_end"*|*" status already-applied reason handle_end"*) ;;
                *) echo "FAIL: handle_end CancelAttempt did not return an exact successful lifecycle status" >&2; return 1 ;;
            esac
            lifecycle_line=$(grep -nF "$lifecycle_record" "$work/f.log" |
                head -n 1 | cut -d: -f1)
            test -n "$lifecycle_line" && test "$lifecycle_line" -lt "$settled_line" || {
                echo "FAIL: handle_end lifecycle did not settle before compiler-group cleanup" >&2
                return 1
            }
            kill -0 "$old_f_service_pid" 2>/dev/null || {
                echo "FAIL: strict stable-F expectation lost the original sidecar process" >&2
                return 1
            }
            echo "S8_REAL_WORKER_LOSS_F_LIFECYCLE_DELIVERED job=$victim_job_id before_group_settlement=1 timeout=0"
        fi
        echo "S8_REAL_WORKER_LOSS_QUIESCED scope=global_scheduler_session job=$victim_job_id pid=$victim_pid pgid=$victim_pgid start_ticks=$victim_start_ticks old_epoch=$old_scheduler_epoch new_epoch=$new_scheduler_epoch F_daemon_pid=$old_worker_pid F_sidecar_pid_before=$old_f_service_pid exact_TERM=1 exact_KILL=1 exact_reaped=1"

        ready_snapshot "$work/ready-f.trace" || {
            echo "FAIL: replacement F did not publish a fresh READY identity" >&2
            return 1
        }
        recovery_f_guid=$ready_f_guid
        if test "$expect_stable_f" = 1; then
            test "$ready_pid" = "$old_f_service_pid" && \
                    test "$recovery_f_guid" = "$old_f_store_guid" || {
                echo "FAIL: strict stable-F expectation changed READY PID/store GUID" >&2
                return 1
            }
            echo "S8_REAL_WORKER_LOSS_F_READY_STABLE pid=$ready_pid F_STORE_GUID=$recovery_f_guid"
        fi
        if ! kill -0 "$old_f_service_pid" 2>/dev/null; then
            test "$ready_pid" != "$old_f_service_pid" && \
                    test "$recovery_f_guid" != "$old_f_store_guid" || {
                echo "FAIL: F sidecar retirement did not install a distinct READY process/store identity" >&2
                return 1
            }
            echo "S8_REAL_WORKER_LOSS_F_READY_REPLACEMENT pid=$ready_pid F_STORE_GUID=$recovery_f_guid prior_pid=$old_f_service_pid prior_F_STORE_GUID=$old_f_store_guid"
        fi
        # Worker-loss is not CancelJob.  A committed victim that cannot prove
        # local predecessor settlement fails closed with Error 24; recovery
        # is exercised below with a distinct fresh R2 job, not a replay claim.
        finish_batch
        test ! -e "$work/result-active-worker-loss-0.tsv" || {
            echo "FAIL: committed worker-loss victim unexpectedly produced an object" >&2
            return 1
        }
        retry_count=$(grep -F -c \
            'P50 assignment failed; requesting one fresh strict-P50 remote assignment' \
            "$victim_client_log" 2>/dev/null || true)
        test "$retry_count" -eq 1 && \
                grep -Fq 'got exception Error 24 - local daemon did not settle P50 retry predecessor' "$victim_client_log" && \
                grep -Fq 'remote-only policy refuses client-error fallback' "$victim_client_log" || {
            echo "FAIL: committed victim did not fail closed after exactly one bounded retry attempt" >&2
            cat "$victim_client_log" >&2 || true
            return 1
        }
        echo "S8_REAL_WORKER_LOSS_SETTLED stopped_attempt=1 retry_attempts=$retry_count victim_policy=Error24_no_object victim_old_epoch=$victim_epoch recovery_epoch=$new_scheduler_epoch old_process_reaped=1"

        recovery_source_offset=$(wc -c <"$source_result_trace")
        recovery_f_offset=$(wc -c <"$f_action_trace")
        run_batch worker-loss-recovery-probe 1 "$work/batch-new-scheduler.tsv" 1 0 0
        recovery_outputs=0
        for result_row in "$work"/result-worker-loss-recovery-probe-*.tsv; do
            test -f "$result_row" && recovery_outputs=$((recovery_outputs + 1))
        done
        test "$recovery_outputs" -eq 1 || {
            echo "FAIL: post-replacement recovery probe did not produce exactly one output" >&2
            return 1
        }
        recovery_result_job=$(cut -f19 "$work/result-worker-loss-recovery-probe-0.tsv")
        replacement_f_login_line=$(grep -nF 'login p50-f protocol version:' \
            "$work/scheduler-replacement.log" | head -n 1 | cut -d: -f1)
        recovery_begin_line=$(grep -nF "BEGIN: $recovery_result_job " \
            "$work/scheduler-replacement.log" | head -n 1 | cut -d: -f1)
        test -n "$replacement_f_login_line" && test -n "$recovery_begin_line" && \
                test "$replacement_f_login_line" -lt "$recovery_begin_line" || {
            echo "FAIL: exact recovery compile began before F registered with replacement scheduler" >&2
            return 1
        }
        echo "S8_REAL_WORKER_LOSS_RECOVERY_ORDER replacement_F_login_line=$replacement_f_login_line recovery_job_begin_line=$recovery_begin_line job=$recovery_result_job"
        verify_results_from_f_store worker-loss-recovery-probe "$recovery_f_guid" \
            "$recovery_source_offset" "$recovery_f_offset" 1 || return 1
        echo "S8_REAL_WORKER_LOSS_RECOVERY_PROBE exact_outputs=$recovery_outputs F_STORE_GUID=$recovery_f_guid fresh_job_not_victim_retry=1"
        echo "S8_REAL_WORKER_SESSION_LOSS_PASS profile=$profile_marker committed_victim=1 stopped_and_reaped=1 victim_output=0 recovery_outputs=1 failure_scope=global_scheduler_session"
    }
    run_real_scheduler_restart_batches() {
        test ! -e "$work/receipt-gate" || {
            echo "FAIL: receipt gate control directory already exists" >&2
            return 1
        }
        receipt_gate_dir="$work/receipt-gate"
        mkdir -m 0777 "$receipt_gate_dir"
        "$build/unittests/p50daemonpositive" \
            --p51-commit-receipt-gate "$port_worker" "$daemon_uid" \
            30 0 "$receipt_gate_dir" >"$work/receipt-gate.log" 2>&1 &
        receipt_gate_pid=$!
        wait_gate_marker ready || return 1
        echo "S8_REAL_S_RECEIPT_GATE_READY endpoint=$port_worker sidecar_uid=$ICECC_TEST_DAEMON_UID expected=30 control=$receipt_gate_dir"

        run_batch old-scheduler 1 "$work/batch-old-scheduler.tsv" 30 1 1
        wait_gate_marker held-1 || return 1
        old_window=$(cat "$receipt_gate_dir/held-1")
        case "$old_window" in
            count=30\ first_ordinal=*\ last_ordinal=*) ;;
            *) echo "FAIL: first source-receipt gate did not hold an exact contiguous W30 interval: $old_window" >&2; return 1 ;;
        esac
        for old_ordinal in $(seq 0 29); do
            client_log="$work/client-compile-old-scheduler-$old_ordinal.log"
            if grep -Fq 'source committed for P50 CompileFile' "$client_log"; then
                echo "FAIL: old cohort published a committed source before scheduler replacement (ordinal=$old_ordinal)" >&2
                return 1
            fi
            if test -e "$work/input-ready/old-scheduler-0-$old_ordinal"; then
                echo "FAIL: old cohort reached compiler input-ready before scheduler replacement (ordinal=$old_ordinal)" >&2
                return 1
            fi
        done
        echo "S8_REAL_S_OLD_COHORT_PRE_RESTART_UNATTACHED count=30 committed_sources=0 compiler_input_ready=0"
        echo "S8_REAL_S_OLD_RECEIPTS_HELD $old_window scheduler_pid=$sched_pid"
        old_epoch_attach_offset=$(wc -c <"$work/f.log")
        capture_old_scheduler_assignments || return 1
        restart_real_scheduler || return 1
        verify_replacement_epoch || return 1
        publish_gate_marker "$receipt_gate_dir/release-1"
        wait_gate_marker released-1 || return 1
        finish_batch
        verify_old_cohort_retries || return 1

        publish_gate_marker "$receipt_gate_dir/arm-2"
        wait_gate_marker armed-2 || return 1
        if test "$real_scheduler_f_restart_w30" = 1; then
        run_batch f-restart-held 1 "$work/batch-new-scheduler.tsv" 30 1 1
        wait_gate_marker held-2 || return 1
        fresh_window=$(cat "$receipt_gate_dir/held-2")
        case "$fresh_window" in
            count=30\ first_ordinal=*\ last_ordinal=*) ;;
            *) echo "FAIL: F-restart source-receipt gate did not hold an exact contiguous W30 interval: $fresh_window" >&2; return 1 ;;
        esac
        for held_ordinal in $(seq 0 29); do
            client_log="$work/client-compile-f-restart-held-$held_ordinal.log"
            if grep -Fq 'source committed for P50 CompileFile' "$client_log" || \
                    test -e "$work/input-ready/f-restart-held-0-$held_ordinal"; then
                echo "FAIL: held B cohort reached C receipt/input-ready before F restart (ordinal=$held_ordinal)" >&2
                return 1
            fi
        done
        echo "S8_REAL_S_F_RESTART_B_HELD $fresh_window scheduler_pid=$sched_pid"
        ready_snapshot "$work/ready-f.trace" || {
            echo "FAIL: missing F READY identity before active-B restart" >&2
            return 1
        }
        f_guid_before_b_restart=$ready_f_guid
        f_service_before_b_restart=$service_pid
        source_trace_before_b_restart=$(wc -c <"$source_result_trace")
        f_trace_before_b_restart=$(wc -c <"$f_action_trace")
        restart_cache_sidecar F 0 "$service_pid" "$worker_pid" "$work/ready-f.trace"
        service_pid=$sidecar_replacement
        f_guid_after_b_restart=$ready_f_guid
        test "$f_guid_before_b_restart" != "$f_guid_after_b_restart" || {
            echo "FAIL: active-B F replacement did not rotate F store identity" >&2
            return 1
        }
        echo "S8_REAL_S_F_RESTARTED_DURING_B before_pid=$f_service_before_b_restart after_pid=$service_pid before_f_store_guid=$f_guid_before_b_restart after_f_store_guid=$f_guid_after_b_restart held_commits=30"

        # The old COMMIT frames are held inside the gate.  Abort deliberately
        # discards that exact interval and removes the one-shot network rule;
        # no old-store receipt is released into the new F incarnation.
        publish_gate_marker "$receipt_gate_dir/abort"
        if wait "$receipt_gate_pid"; then
            receipt_gate_pid=
            echo "FAIL: aborted F-restart gate unexpectedly reported success" >&2
            return 1
        fi
        receipt_gate_pid=
        test -f "$receipt_gate_dir/failed" && \
                test -f "$receipt_gate_dir/discarded-2" || {
            cat "$work/receipt-gate.log" >&2 || true
            echo "FAIL: receipt gate did not record the explicit B-window discard" >&2
            return 1
        }
        finish_batch
        b_settled=$(( $(date +%s%N) ))
        b_successes=0
        for result_row in "$work"/result-f-restart-held-*.tsv; do
            test -f "$result_row" && b_successes=$((b_successes + 1))
        done
        verify_results_from_f_store f-restart-held "$f_guid_after_b_restart" \
            "$source_trace_before_b_restart" "$f_trace_before_b_restart" 0 || return 1
        verify_bounded_batch_settlement f-restart-held 30 || return 1
        echo "S8_REAL_S_F_RESTART_B_SETTLED count=30 outputs=$b_successes bounded_clients=30 settled_ns=$b_settled"

        # A separate gate starts after the F replacement and proves a fresh
        # scheduler cohort still commits, attaches, and compiles exact output.
        receipt_gate_dir="$work/receipt-gate-c"
        mkdir -m 0777 "$receipt_gate_dir"
        "$build/unittests/p50daemonpositive" \
            --p51-commit-receipt-gate-once "$port_worker" "$daemon_uid" \
            30 0 "$receipt_gate_dir" >"$work/receipt-gate-c.log" 2>&1 &
        receipt_gate_pid=$!
        wait_gate_marker ready || return 1
        source_trace_before_c=$(wc -c <"$source_result_trace")
        f_trace_before_c=$(wc -c <"$f_action_trace")
        run_batch post-f-restart 1 "$work/batch-new-f.tsv" 30 0 1
        wait_gate_marker held-1 || return 1
        c_fresh_window=$(cat "$receipt_gate_dir/held-1")
        case "$c_fresh_window" in
            count=30\ first_ordinal=*\ last_ordinal=*) ;;
            *) echo "FAIL: post-F cohort did not hold an exact contiguous W30 interval: $c_fresh_window" >&2; return 1 ;;
        esac
        publish_gate_marker "$receipt_gate_dir/release-1"
        wait_gate_marker released-1 || return 1
        finish_batch
        publish_gate_marker "$receipt_gate_dir/finish"
        if ! wait "$receipt_gate_pid"; then
            receipt_gate_pid=
            cat "$work/receipt-gate-c.log" >&2 || true
            echo "FAIL: post-F one-shot receipt gate did not finish cleanly" >&2
            return 1
        fi
        receipt_gate_pid=
        test "$batch_failed" -eq 0 || return 1
        c_fresh_results=0
        for result_row in "$work"/result-post-f-restart-*.tsv; do
            test -f "$result_row" && c_fresh_results=$((c_fresh_results + 1))
        done
        test "$c_fresh_results" -eq 30 || {
            echo "FAIL: post-F replacement cohort produced $c_fresh_results/30 exact outputs" >&2
            return 1
        }
        verify_results_from_f_store post-f-restart "$f_guid_after_b_restart" \
            "$source_trace_before_c" "$f_trace_before_c" 30 || return 1
        echo "S8_REAL_SCHEDULER_F_RESTART_CHAIN_W30_PASS profile=$profile_marker old_scheduler_receipts=30 old_callers_settled=30 b_held_receipts=30 f_restarted_while_b_held=1 b_settled_outputs=$b_successes post_f_receipts=30 post_f_exact_outputs=$c_fresh_results old_f_guid=$f_guid_before_b_restart new_f_guid=$f_guid_after_b_restart c_f_daemons_stable=1"
        else
            run_batch new-scheduler 1 "$work/batch-new-scheduler.tsv" 30 0 1
            wait_gate_marker held-2 || return 1
            fresh_window=$(cat "$receipt_gate_dir/held-2")
            case "$fresh_window" in
                count=30\ first_ordinal=*\ last_ordinal=*) ;;
                *) echo "FAIL: second source-receipt gate did not hold an exact contiguous W30 interval: $fresh_window" >&2; return 1 ;;
            esac
            echo "S8_REAL_S_FRESH_RECEIPTS_HELD $fresh_window scheduler_pid=$sched_pid"
            publish_gate_marker "$receipt_gate_dir/release-2"
            wait_gate_marker released-2 || return 1
            finish_batch
            publish_gate_marker "$receipt_gate_dir/finish"
            if ! wait "$receipt_gate_pid"; then
                receipt_gate_pid=
                cat "$work/receipt-gate.log" >&2 || true
                echo "FAIL: persistent receipt gate did not finish cleanly" >&2
                return 1
            fi
            receipt_gate_pid=
            test "$batch_failed" -eq 0 || return 1
            fresh_results=0
            for result_row in "$work"/result-new-scheduler-*.tsv; do
                test -f "$result_row" && fresh_results=$((fresh_results + 1))
            done
            test "$fresh_results" -eq 30 || {
                echo "FAIL: replacement scheduler produced $fresh_results/30 verified fresh compile results" >&2
                return 1
            }
            echo "S8_REAL_SCHEDULER_RESTART_W30_PASS profile=$profile_marker old_receipts=30 old_callers_settled=30 fresh_receipts=30 fresh_objects_verified=$fresh_results c_f_daemons_stable=1"
        fi
    }
    if test "$warm" = 1 && test "$cache_enabled" -eq 1; then
        echo "S7_WARM_PREWARM_BEGIN"
        run_batch prewarm 0
        if test "$external_mode" = 0; then
            merge_parallel_f_traces
        fi
        test -s "$c_action_trace" || {
            echo "FAIL: prewarm product action traces are missing" >&2
            exit 1
        }
        cp -- "$c_action_trace" "$prewarm_c_trace"
        if test "$external_mode" = 0; then
            test -s "$f_action_trace" || { echo "FAIL: prewarm F action trace is missing" >&2; exit 1; }
            cp -- "$f_action_trace" "$prewarm_f_trace"
        fi
        if test "$external_mode" = 1; then
            test -x "${ICECC_P50_EXTERNAL_PREWARM_HOOK:-}" || {
                echo "FAIL: external prewarm trace handshake is required" >&2
                exit 1
            }
            "${ICECC_P50_EXTERNAL_PREWARM_HOOK}" "$work" "$suite" "$profile_marker"
            test -f "${ICECC_P50_EXTERNAL_PREWARM_READY:-}" || {
                echo "FAIL: external prewarm READY witness missing" >&2
                exit 1
            }
        fi
        prewarm_c_lines=$(wc -l <"$c_action_trace")
        if test "$external_mode" = 0; then
            prewarm_f_lines=$(wc -l <"$f_action_trace")
        else
            prewarm_f_lines=0
        fi
        echo "S7_WARM_PREWARM_COMPLETE"
    fi
    if test "$worker_session_loss" = 1; then
        run_real_worker_session_loss
    elif test "$real_scheduler_restart_w30" = 1; then
        run_real_scheduler_restart_batches
    else
        run_batch full-1 1
        if test "$passes" = 2; then
            run_batch full-2 1
        fi
    fi
    if test "$cache_enabled" -eq 1 && test "$external_mode" -eq 0; then
        merge_parallel_f_traces
    fi
else
    if test "$warm" = 1 && test "$cache_enabled" -eq 1; then
        echo "S7_WARM_PREWARM_BEGIN"
        compile_once prewarm
        test -s "$c_action_trace" || {
            echo "FAIL: prewarm product action traces are missing" >&2
            exit 1
        }
        cp -- "$c_action_trace" "$prewarm_c_trace"
        if test "$external_mode" = 0; then
            test -s "$f_action_trace" || { echo "FAIL: prewarm F action trace is missing" >&2; exit 1; }
            cp -- "$f_action_trace" "$prewarm_f_trace"
        fi
        if test "$external_mode" = 1; then
            test -x "${ICECC_P50_EXTERNAL_PREWARM_HOOK:-}" || {
                echo "FAIL: external prewarm trace handshake is required" >&2
                exit 1
            }
            "${ICECC_P50_EXTERNAL_PREWARM_HOOK}" "$work" "$suite" "$profile_marker"
            test -f "${ICECC_P50_EXTERNAL_PREWARM_READY:-}" || {
                echo "FAIL: external prewarm READY witness missing" >&2
                exit 1
            }
        fi
        prewarm_c_lines=$(wc -l <"$c_action_trace")
        if test "$external_mode" = 0; then
            prewarm_f_lines=$(wc -l <"$f_action_trace")
        else
            prewarm_f_lines=0
        fi
        echo "S7_WARM_PREWARM_COMPLETE"
    fi
    compile_once measured
fi

if test "$external_mode" = 1; then
    # The external executor owns F trace collection.  The hook blocks the
    # finalizer until every remote relationship trace has been copied into
    # this q3 workdir, so measured evidence cannot race the SSH transfer.
    test -x "${ICECC_P50_EXTERNAL_COLLECT_HOOK:-}" || {
        echo "FAIL: external F trace collection handshake is required" >&2
        exit 1
    }
    "${ICECC_P50_EXTERNAL_COLLECT_HOOK}" "$work" "$suite" "$profile_marker"
    test -f "${ICECC_P50_EXTERNAL_COLLECT_READY:-}" || {
        echo "FAIL: external F trace collection READY witness missing" >&2
        exit 1
    }
fi

if test "$external_mode" = 0 && test "$suite" = C1F20/40; then
    for relationship in $(seq 0 19); do
        measured_offset=$(cat "$work/f-measured-log-offset-$relationship")
        if tail -c +$((measured_offset + 1)) "$work/f-$relationship.log" | \
                grep -E 'start_install_environment|handle_transfer_env' >/dev/null 2>&1; then
            echo "FAIL: measured compile attempted environment installation ($relationship)" >&2
            exit 1
        fi
    done
elif test "$external_mode" = 0; then
    measured_offset=$(cat "$work/f-measured-log-offset-0")
    if tail -c +$((measured_offset + 1)) "$work/f.log" | \
            grep -E 'start_install_environment|handle_transfer_env' >/dev/null 2>&1; then
        echo "FAIL: measured compile attempted environment installation" >&2
        exit 1
    fi
fi
if test "$external_mode" = 1; then
    echo "S8_ENV_MEASURED_NO_INSTALL checked=external-transport"
else
    echo "S8_ENV_MEASURED_NO_INSTALL checked=1"
fi

    if test "$cache_enabled" -eq 1; then
test -s "$c_action_trace" || {
    echo "FAIL: measured product action traces are missing" >&2
    exit 1
}
if test "$external_mode" = 0; then
test -s "$f_action_trace" || {
    echo "FAIL: measured product F action trace is missing" >&2
    exit 1
}
fi
if test "$external_mode" = 1; then
    cp -- "$c_action_trace" "$measured_c_trace"
elif test "$warm" = 1; then
    tail -n "+$((prewarm_c_lines + 1))" "$c_action_trace" >"$measured_c_trace"
    tail -n "+$((prewarm_f_lines + 1))" "$f_action_trace" >"$measured_f_trace"
else
    cp -- "$c_action_trace" "$measured_c_trace"
    cp -- "$f_action_trace" "$measured_f_trace"
fi
if test "$external_mode" = 1; then
    test -s "$measured_c_trace" || { echo "FAIL: measured C action-trace slice is empty" >&2; exit 1; }
else
    test -s "$measured_c_trace" && test -s "$measured_f_trace" || {
        echo "FAIL: measured action-trace slice is empty" >&2
        exit 1
    }
fi
else
    : >"$measured_c_trace"
    : >"$measured_f_trace"
fi
echo "S7_PREWARM_INPUT=$work/s7-prewarm-preprocessed.ii"
echo "S7_MEASURED_INPUT=$work/s7-measured-preprocessed.ii"
echo "S7_PREWARM_C_ACTION_TRACE=$prewarm_c_trace"
echo "S7_PREWARM_F_ACTION_TRACE=$prewarm_f_trace"
echo "S7_MEASURED_C_ACTION_TRACE=$measured_c_trace"
echo "S7_MEASURED_F_ACTION_TRACE=$measured_f_trace"
echo "S7_WORKDIR=$work"
if test -n "$batch_manifest"; then
    echo "S8_SUITE=$suite"
    echo "S8_BATCH_COUNT=$batch_expected_count"
    echo "S8_BATCH_PASSES=$passes"
    echo "S8_BATCH_WARM=$warm"
    if test "$suite" = C1F20/40; then
        echo "S8_SCHEDULING mode=relationship-ordered execution_slots=40 relationships=20 planned_admission_lanes_per_relationship=2 physical_slot_observed=0"
    else
        echo "S8_SCHEDULING mode=relationship-ordered execution_slots=$execution_slots relationships=1 planned_admission_lanes_per_relationship=$slots_per_f"
    fi
    binary_roles="scheduler/icecc-scheduler daemon/iceccd client/icecc"
    test "$cache_enabled" -eq 1 && binary_roles="$binary_roles cache/icecc-cache-service"
    for binary_role in $binary_roles; do
        binary_path="$build/$binary_role"
        test -x "$binary_path" || { echo "FAIL: missing binary $binary_path" >&2; exit 1; }
        printf 'S8_BINARY role=%s sha256=%s bytes=%s path=%s\n' \
            "$binary_role" "$(sha256sum "$binary_path" | awk '{print $1}')" \
            "$(stat -c %s "$binary_path")" "$binary_path"
    done
fi

if test "$worker_session_loss" = 1; then
    echo "PASS: P51 active compiler quiescence and post-session recovery probe passed for $profile_marker"
else
    echo "PASS: all-P50 C1F1 $profile_marker compile is remote and byte-identical"
fi
