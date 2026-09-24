#!/bin/sh
# Real wrapper-to-compiler P51 persistence gate.  One C/F relationship handles
# 31 separate compiler invocations per profile; the production runner compares
# every remote object byte-for-byte with a local g++ build.
set -eu

src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
build=${ICECC_TEST_TOP_BUILDDIR:-$src}
scratch=${ICEFARM_TMPDIR:-}
test -n "$scratch" && test -d "$scratch" || {
    echo "SKIP: ICEFARM_TMPDIR must name writable task scratch" >&2
    exit 77
}
test -n "${ICECC_P50_C1F1_WORKER_SCHEDULER_HOST:-}" || {
    echo "SKIP: a reachable non-loopback worker scheduler address is required" >&2
    exit 77
}

fixture=$(mktemp -d "${scratch%/}/p51-wrapper-fixture.XXXXXX")
trap 'echo "P51 wrapper artifacts retained at $fixture"' EXIT
chmod 0755 "$fixture"
mkdir -p "$fixture/sources" "$fixture/predictive"
jobs=${ICECC_P51_WRAPPER_JOBS:-100}
case "$jobs" in
    ''|*[!0-9]*|0) echo "FAIL: ICECC_P51_WRAPPER_JOBS must be a positive integer" >&2; exit 1 ;;
esac

sh "$src/dev/python.sh" --exec python - "$fixture" "$jobs" <<'PY'
import hashlib
import json
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
count = int(sys.argv[2])
rows = []
for ordinal in range(count):
    stem = f"tu-{ordinal:02d}"
    body = (
        f'extern "C" int p51_w31_{ordinal:02d}() {{ '
        f'return {ordinal + 17}; }}\n'
    ).encode()
    source = root / "sources" / f"{stem}.cpp"
    predictive = root / "predictive" / f"{stem}.ii"
    source.write_bytes(body)
    with predictive.open("wb") as output:
        subprocess.run(
            ["g++", "-std=c++17", "-E", str(source)],
            check=True, stdout=output)
    digest = hashlib.sha256(body).hexdigest()
    predictive_digest = hashlib.sha256(predictive.read_bytes()).hexdigest()
    rows.append({
        "tu_id": stem,
        "source": str(source),
        "source_relative": f"sources/{stem}.cpp",
        "sha256": digest,
        "predictive_input": {
            "ordinal": ordinal,
            "path": str(predictive),
            "source_relative": f"predictive/{stem}.ii",
            "sha256": predictive_digest,
            "bytes": predictive.stat().st_size,
        },
    })
with (root / "batch.jsonl").open("w", encoding="utf-8") as stream:
    for row in rows:
        stream.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
PY

for profile in P29V1 ZSTD_TU ZSTD_ROUTE; do
    fixture_id=${fixture##*.}
    case "$profile" in
        P29V1) profile_tag=29 ;;
        ZSTD_TU) profile_tag=TU ;;
        ZSTD_ROUTE) profile_tag=RT ;;
    esac
    # The sidecar appends `/attempt-N-<32 hex>` and then `/cache.sock` under
    # this directory. Keep the per-profile work basename short enough for
    # AF_UNIX sun_path even when the host scratch mount is deeply nested.
    work="${scratch%/}/p50compilee2e.w-$fixture_id-$profile_tag"
    # The supervised test adapter has a bounded four-attempt budget.
    socket_probe="$work/cache-runtime-f/attempt-4-00000000000000000000000000000000/cache.sock"
    test "${#socket_probe}" -le 107 || {
        echo "FAIL: wrapper scratch path exceeds AF_UNIX sun_path budget: " \
             "${#socket_probe}/107 bytes for /attempt-<n>-<32hex>/cache.sock" >&2
        exit 1
    }
    log="$fixture/$profile.log"
    test ! -e "$work" || {
        echo "FAIL: refusing to reuse wrapper workdir $work" >&2
        exit 1
    }
    set +e
    env \
        ICECC_TEST_TOP_SRCDIR="$src" \
        ICECC_TEST_TOP_BUILDDIR="$build" \
        ICECC_P51_MODE=on \
        ICECC_P50_PROFILE="$profile" \
        ICECC_P50_SUITE=C1F1/100000 \
        ICECC_P50_C1F1_BATCH_MANIFEST="$fixture/batch.jsonl" \
        ICECC_P50_C1F1_EXPECTED_COUNT="$jobs" \
        ICECC_P50_C1F1_PASSES=1 \
        ICECC_P50_C1F1_WARM=0 \
        ICECC_P50_C1F1_WORKDIR="$work" \
        ICECC_P50_C1F1_KEEP_WORK=1 \
        ICECC_P50_C1F1_TIMEOUT=300 \
        "$src/unittests/p50compilee2e-run.sh" >"$log" 2>&1
    status=$?
    set -e
    test "$status" -eq 0 || {
        cat "$log"
        echo "FAIL: actual P51 wrapper compile failed for $profile (status $status)" >&2
        exit 1
    }
    grep -F "S8_BATCH_COMPLETE run=full-1 count=$jobs" "$log" >/dev/null || {
        echo "FAIL: $profile did not complete all 31 compiler jobs" >&2
        exit 1
    }
    grep -F "PASS: all-P50 C1F1 $profile compile is remote and byte-identical" \
        "$log" >/dev/null || {
        echo "FAIL: $profile object-comparison gate did not pass" >&2
        exit 1
    }
    test -f "$work/f.log" && test ! -L "$work/f.log" || {
        cat "$log"
        echo "FAIL: F daemon log is missing for $profile" >&2
        exit 1
    }
    measured_offset=$(cat "$work/f-measured-log-offset-0")
    measured_f_log=$(tail -c +$((measured_offset + 1)) "$work/f.log")
    ready_count=$(printf '%s\n' "$measured_f_log" | \
        grep -F -c 'P51 cache-link descriptor adopted by sidecar' || true)
    legacy_count=$(printf '%s\n' "$measured_f_log" | \
        grep -F -c 'P50_CACHE_SESSION_READY request=' || true)
    test "$ready_count" -eq 1 && test "$legacy_count" -eq 0 || {
        cat "$log"
        cat "$work/f.log"
        echo "FAIL: $profile did not use exactly one persistent P51 link " \
             "(P51-ready=$ready_count R1-ready=$legacy_count)" >&2
        exit 1
    }
    printf 'P51_WRAPPER_COMPILE_PASS profile=%s jobs=%s persistent_links=%s log=%s f_log=%s\n' \
        "$profile" "$jobs" "$ready_count" "$log" "$work/f.log"
done

echo "PASS: actual P51 wrapper compiled $jobs TUs on one persistent link per profile"
