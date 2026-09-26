#!/bin/sh
# Durable opt-in/opt-out and bounded-JSON check for service timing diagnostics.
set -eu

tmp_root=${ICEFARM_TMPDIR:-${TMPDIR:-/tmp}}
work=$(mktemp -d "$tmp_root/p50-service-metrics.XXXXXX")
src=${ICECC_TEST_TOP_SRCDIR:-$(cd "$(dirname "$0")/.." && pwd)}
cleanup() {
    result=$?
    if [ "$result" -eq 0 ]; then
        rm -rf "$work"
    else
        echo "p50 service metrics diagnostics retained at $work" >&2
        for output in enabled.out enabled.err disabled.out disabled.err; do
            if [ -f "$work/$output" ]; then
                echo "--- $output ---" >&2
                cat "$work/$output" >&2
            fi
        done
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

if ICECC_P50_DIAGNOSTICS=1 ./p50cacheservice --aggregate-fit-exact \
        >"$work/enabled.out" 2>"$work/enabled.err"; then
    :
else
    result=$?
    echo "opt-in aggregate-fit-exact exited $result" >&2
    exit 1
fi
if env -u ICECC_P50_DIAGNOSTICS ./p50cacheservice --aggregate-fit-exact \
        >"$work/disabled.out" 2>"$work/disabled.err"; then
    :
else
    result=$?
    echo "opt-out aggregate-fit-exact exited $result" >&2
    exit 1
fi

sh "$src/dev/python.sh" --exec python3 - \
    "$work/enabled.err" "$work/disabled.err" <<'PY'
import json
import sys

enabled_path, disabled_path = sys.argv[1:]
prefix = "P51_SERVICE_METRICS "
enabled = open(enabled_path, encoding="utf-8").read().splitlines()
disabled = open(disabled_path, encoding="utf-8").read()
records = []
for line in enabled:
    if line.startswith(prefix):
        records.append(json.loads(line[len(prefix):]))

if not records:
    raise SystemExit("opt-in run emitted no P51_SERVICE_METRICS JSON")
for record in records:
    expected = {
        "v", "pid", "elapsed_ms", "snapshot", "consistency",
        "raw_bytes_current", "raw_bytes_high_water", "raw_bytes_limit",
        "active_p51_credit_count", "admission_wait", "read_queue", "read",
        "delivery",
    }
    if set(record) != expected:
        raise SystemExit("service metrics top-level schema mismatch")
    if record["v"] != 1 or record["snapshot"] not in {"periodic", "stop"}:
        raise SystemExit("service metrics version/snapshot mismatch")
    if record["consistency"] != ("final" if record["snapshot"] == "stop"
                                  else "best_effort"):
        raise SystemExit("service metrics consistency label mismatch")
    for name in ("admission_wait", "read_queue", "read", "delivery"):
        series = record[name]
        if set(series) != {"count", "total_ns", "max_ns"}:
            raise SystemExit(f"service metrics {name} schema mismatch")
        if any(type(series[key]) is not int or series[key] < 0
               for key in series):
            raise SystemExit(f"service metrics {name} values are invalid")
finals = [record for record in records if record["snapshot"] == "stop"]
if not finals:
    raise SystemExit("opt-in run omitted final stop snapshot")
for record in finals:
    if record["raw_bytes_current"] != 0 or record["active_p51_credit_count"] != 0:
        raise SystemExit("final service metrics retained raw bytes or credit")
    if not (0 < record["raw_bytes_high_water"] <= record["raw_bytes_limit"]):
        raise SystemExit("service metrics raw high-water violates configured limit")
    for name in ("admission_wait", "read_queue", "read", "delivery"):
        series = record[name]
        if series["count"] < 1 or series["total_ns"] < series["max_ns"]:
            raise SystemExit(f"final service metrics {name} population is invalid")
if prefix in disabled:
    raise SystemExit("opt-out run emitted P51_SERVICE_METRICS")
PY

echo "p50 service metrics opt-in/opt-out JSON: PASS"
