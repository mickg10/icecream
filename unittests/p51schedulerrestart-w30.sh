#!/bin/sh
# Real icecc-scheduler process replacement while one persistent P51 C/F pair
# holds two exact W30 source-receipt intervals. The daemon and cache processes
# stay alive; this is deliberately distinct from synthetic scheduler-epoch
# fixtures.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_TOP_BUILDDIR:-$src}
scratch=${ICEFARM_TMPDIR:-}
test -n "$scratch" && test -d "$scratch" || {
    echo "SKIP: ICEFARM_TMPDIR must name writable task scratch" >&2
    exit 77
}
test "${ICECC_TEST_P51_PRIVATE_NETNS:-}" = 1 || {
    echo "SKIP: set ICECC_TEST_P51_PRIVATE_NETNS=1 only inside a disposable private network namespace with NET_ADMIN" >&2
    exit 77
}
worker_scheduler_host=$(hostname -I 2>/dev/null | tr ' ' '\n' | \
    awk '/^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$/ { print; exit }')
test -n "$worker_scheduler_host" || {
    echo "SKIP: could not derive this test container's IPv4 bridge address" >&2
    exit 77
}
export ICECC_P50_C1F1_WORKER_SCHEDULER_HOST="$worker_scheduler_host"
echo "P51_REAL_SCHEDULER_NETWORK worker_address=$worker_scheduler_host netns=private-opt-in"
test -n "${ICECC_TEST_DAEMON_UID:-}" || {
    echo "SKIP: ICECC_TEST_DAEMON_UID must select the isolated daemon account" >&2
    exit 77
}
daemon_passwd_entry=$(getent passwd "$ICECC_TEST_DAEMON_UID" || true)
test -n "$daemon_passwd_entry" || {
    echo "SKIP: ICECC_TEST_DAEMON_UID does not resolve to an account" >&2
    exit 77
}
daemon_account=$(printf '%s\n' "$daemon_passwd_entry" | cut -d: -f1)
daemon_uid=$(printf '%s\n' "$daemon_passwd_entry" | cut -d: -f3)
daemon_gid=$(printf '%s\n' "$daemon_passwd_entry" | cut -d: -f4)
test -n "$daemon_account" && test "$daemon_uid" -gt 0 || {
    echo "SKIP: ICECC_TEST_DAEMON_UID must select a non-root account" >&2
    exit 77
}
test -x "$build/unittests/p50daemonpositive" || {
    echo "SKIP: p50daemonpositive receipt-gate helper is not built" >&2
    exit 77
}
runner_timeout=${ICECC_TEST_P51_RESTART_TIMEOUT_SECONDS:-480}
case "$runner_timeout" in
    ''|*[!0-9]*) echo "FAIL: restart timeout must be a positive integer" >&2; exit 1 ;;
esac
test "$runner_timeout" -gt 0 || {
    echo "FAIL: restart timeout must be positive" >&2
    exit 1
}

fixture=$(mktemp -d "${scratch%/}/p51s.XXXXXX")
trap 'echo "real scheduler W30 artifacts retained at $fixture"' EXIT
chmod 0755 "$fixture"
mkdir -p "$fixture/sources" "$fixture/predictive"
rows=60

sh "$src/dev/python.sh" --exec python - "$fixture" "$rows" <<'PY'
import hashlib
import json
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
rows = []
for ordinal in range(int(sys.argv[2])):
    stem = f"srestart-{ordinal:02d}"
    # A small independent TU is generated locally for this integration gate.
    # The runner validates its source hash, actual preprocessing, and exact
    # remote-vs-local object bytes for every successful fresh-epoch compile.
    source_bytes = (
        f'extern "C" int p51_s_restart_{ordinal:02d}() {{ '
        f'return {ordinal + 101}; }}\n'
    ).encode("ascii")
    source = root / "sources" / f"{stem}.cpp"
    predictive = root / "predictive" / f"{stem}.ii"
    source.write_bytes(source_bytes)
    with predictive.open("wb") as output:
        subprocess.run(["g++", "-std=c++17", "-E", str(source)],
                       check=True, stdout=output)
    rows.append({
        "tu_id": stem,
        "source": str(source),
        "source_relative": f"sources/{stem}.cpp",
        "sha256": hashlib.sha256(source_bytes).hexdigest(),
        "predictive_input": {
            "ordinal": ordinal,
            "path": str(predictive),
            "source_relative": f"predictive/{stem}.ii",
            "sha256": hashlib.sha256(predictive.read_bytes()).hexdigest(),
            "bytes": predictive.stat().st_size,
        },
    })
with (root / "batch.jsonl").open("w", encoding="utf-8") as stream:
    for row in rows:
        stream.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
PY

fixture_id=${fixture##*.}
for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    case "$profile" in
        P29V1) profile_tag=29 ;;
        ZSTD_TU) profile_tag=TU ;;
        ZSTD_ROUTE) profile_tag=RT ;;
    esac
    work="${scratch%/}/p50compilee2e.sr-$fixture_id-$profile_tag"
    socket_probe="$work/cache-runtime-f/attempt-4-00000000000000000000000000000000/cache.sock"
    test "${#socket_probe}" -le 107 || {
        echo "FAIL: real-S scratch path exceeds AF_UNIX sun_path budget (${#socket_probe}/107)" >&2
        exit 1
    }
    test ! -e "$work" || { echo "FAIL: refusing to reuse $work" >&2; exit 1; }
    log="$fixture/$profile.log"
    set +e
    env \
        -u ICECC_TEST_P51_MULTILINK \
        -u ICECC_TEST_P51_VERTICAL \
        -u ICECC_TEST_P51_VERTICAL_W30 \
        -u ICECC_TEST_P51_RESTART_F_C1F2 \
        -u ICECC_TEST_P51_RESTART_C_C2F1 \
        -u ICECC_TEST_P51_RESTART_W30_F_C1F2 \
        -u ICECC_TEST_P51_RESTART_W30_C_C2F1 \
        -u ICECC_TEST_P51_SYNTH_SCHEDULER_W30 \
        -u ICECC_TEST_P51_CANCEL_REPLACEMENT \
        -u ICECC_TEST_SCHEDULER_BACKPRESSURE \
        -u ICECC_TEST_PENDING_DISCONNECT \
        -u ICECC_TEST_R1_PUBLISH_ONLY \
        -u ICECC_TEST_SNDBUF_SHIM \
        -u ICECC_TEST_SCHEDULER_RCVBUF \
        ICECC_TEST_TOP_SRCDIR="$src" \
        ICECC_TEST_TOP_BUILDDIR="$build" \
        ICECC_TEST_DAEMON_UID="$ICECC_TEST_DAEMON_UID" \
        ICECC_TEST_DAEMON_GID="${ICECC_TEST_DAEMON_GID:-$daemon_gid}" \
        ICECC_TEST_WRAPPER_USER="${ICECC_TEST_WRAPPER_USER:-root}" \
        ICECC_TEST_POSITIVE_DAEMON=1 \
        ICECC_P51_MODE=on \
        ICECC_P50_PROFILE="$profile" \
        ICECC_P50_SUITE=C1F1/100000 \
        ICECC_P50_C1F1_BATCH_MANIFEST="$fixture/batch.jsonl" \
        ICECC_P50_C1F1_EXPECTED_COUNT=60 \
        ICECC_P50_C1F1_PASSES=1 \
        ICECC_P50_C1F1_WARM=0 \
        ICECC_P50_C1F1_REAL_SCHEDULER_RESTART_W30=1 \
        ICECC_P50_C1F1_WORKDIR="$work" \
        ICECC_P50_C1F1_KEEP_WORK=1 \
        ICECC_P50_C1F1_TIMEOUT=300 \
        timeout --foreground --signal=TERM --kill-after=20s "$runner_timeout" \
            "$src/unittests/p50compilee2e-run.sh" >"$log" 2>&1
    status=$?
    set -e
    test "$status" -eq 0 || {
        cat "$log"
        echo "FAIL: actual scheduler W30 replacement failed for $profile (status $status)" >&2
        exit 1
    }
    grep -F "S8_REAL_SCHEDULER_RESTART_W30_PASS profile=$profile old_receipts=30 old_callers_settled=30 fresh_receipts=30 fresh_objects_verified=30 c_f_daemons_stable=1" \
        "$log" >/dev/null || {
        cat "$log"
        echo "FAIL: $profile did not produce the complete real-S W30 evidence marker" >&2
        exit 1
    }
    test -f "$work/scheduler-replacement.log" && \
        grep -Fq 'login p50-c protocol version:' "$work/scheduler-replacement.log" && \
        grep -Fq 'login p50-f protocol version:' "$work/scheduler-replacement.log" || {
        cat "$log"
        echo "FAIL: replacement scheduler process log is missing its C/F registrations" >&2
        exit 1
    }
    printf 'P51_REAL_SCHEDULER_RESTART_W30_PASS profile=%s old_fresh_receipt_windows=30+30 log=%s work=%s replacement_scheduler_log=%s\n' \
        "$profile" "$log" "$work" "$work/scheduler-replacement.log"
done

echo "PASS: real scheduler process replacement qualified with held W30 source receipts for all three profiles"
