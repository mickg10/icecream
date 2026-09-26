#!/bin/sh
# Real wrapper-to-compiler P51 gate. Ordinary mode checks a persistent link
# across separate invocations. Worker-session-loss mode (set
# ICECC_P51_WRAPPER_WORKER_SESSION_LOSS=1) is deliberately a focused one-heavy-
# compile test: it holds the exact victim compiler, loses the global scheduler
# session, expects strict-P50 Error 24/no victim object if the committed
# predecessor cannot be settled, then verifies a separate fresh R2 recovery
# compile against local g++. The opt-in staggered-quiescence variant holds two
# exact compiler groups and sends an ordinary new client through F during the
# cleanup grace. Neither mode asserts transparent victim replay. Select profiles
# with ICECC_P51_WRAPPER_PROFILES (e.g. "P29V1 ZSTD_TU" or ZSTD_ROUTE).
# Portable entrypoint (bind task scratch to /tmp, source/build as above, and
# provide the required non-loopback scheduler address and daemon uid/gid):
#   ICEFARM_TMPDIR=/tmp ICECC_P50_C1F1_WORKER_SCHEDULER_HOST=<worker-IP> \
#   ICECC_P51_WRAPPER_WORKER_SESSION_LOSS=1 \
#   ICECC_P51_WRAPPER_PROFILES='P29V1 ZSTD_TU' \
#   ICECC_TEST_TOP_SRCDIR=<source> ICECC_TEST_TOP_BUILDDIR=<build> \
#   sh <source>/unittests/p51wrappercompile-run.sh
# C1F2 W30 F-specific loss is a separate opt-in. It builds 31 tiny manifest
# inputs (the harness creates its single active victim independently):
#   ICEFARM_TMPDIR=/tmp ICECC_P50_C1F1_WORKER_SCHEDULER_HOST=<worker-IP> \
#   ICECC_P51_WRAPPER_C1F2_W30=1 \
#   ICECC_P51_WRAPPER_PROFILES='P29V1 ZSTD_TU ZSTD_ROUTE' \
#   ICECC_TEST_TOP_SRCDIR=<source> ICECC_TEST_TOP_BUILDDIR=<build> \
#   sh <source>/unittests/p51wrappercompile-run.sh
set -eu
src=${ICECC_TEST_TOP_SRCDIR:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}

w30_validate_adoption_phases() {
    sh "$src/dev/python.sh" --exec python - "$@" <<'PY'
import datetime, pathlib, re, sys

path, measured_offset_text, settled_line_text, reconnected_line_text, recovery_job = sys.argv[1:]
measured_offset = int(measured_offset_text)
settled_line = int(settled_line_text)
reconnected_line = int(reconnected_line_text)
raw = pathlib.Path(path).read_bytes()
records = raw.splitlines(keepends=True)
events = []
attaches = []
byte_offset = 0
adoption = b"P51 cache-link descriptor adopted by sidecar"
for line_number, record in enumerate(records, 1):
    line = record.decode("utf-8", errors="replace")
    if byte_offset >= measured_offset and adoption in record:
        stamp = re.search(r"\] (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d):", line)
        if stamp is None:
            raise SystemExit("A adoption lacks a wall-clock timestamp")
        when = datetime.datetime.strptime(stamp.group(1), "%Y-%m-%d %H:%M:%S")
        events.append((line_number, when))
    if f"P50_INPUT_ATTACH_BEGIN job={recovery_job} " in line:
        attaches.append(line_number)
    byte_offset += len(record)

if len(events) not in (2, 3):
    raise SystemExit(f"expected reconnect and optional idle reopen, got {len(events)} measured A adoptions")
if not events[0][0] < settled_line < events[1][0] <= reconnected_line:
    raise SystemExit("initial A link, exact old-group settlement, and reconnect link are not in phase order")
reconnected_record = records[reconnected_line - 1].decode("utf-8", errors="replace")
stamp = re.search(r"\] (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d):", reconnected_record)
if "Connected to scheduler (I am known as " not in reconnected_record or stamp is None:
    raise SystemExit("reconnect line is not the exact scheduler connection witness")
if events[1][1] != datetime.datetime.strptime(stamp.group(1), "%Y-%m-%d %H:%M:%S"):
    raise SystemExit("reconnect link adoption does not share the scheduler re-login timestamp")
if len(attaches) != 1 or events[-1][0] >= attaches[0]:
    raise SystemExit("last A link adoption did not precede exact fresh-recovery input attach")
idle_reopen = 0
if len(events) == 3:
    gap = (events[2][1] - events[1][1]).total_seconds()
    if gap < 60:
        raise SystemExit(f"third A link reopened before 60s route idle deadline (gap={gap})")
    if any(b"P50_INPUT_ATTACH_BEGIN" in record
           for record in records[events[1][0]:events[2][0] - 1]):
        raise SystemExit("A had a P50 compiler input attachment during interval classified as idle reopen")
    idle_reopen = int(gap)
print(f"measured_A_adoptions={len(events)} idle_reopen_seconds={idle_reopen}")
PY
}

if test "${ICECC_P51_WRAPPER_C1F2_W30_ADOPTION_SELFTEST:-0}" = 1; then
    fixture_dir=$(mktemp -d "${ICEFARM_TMPDIR:-${TMPDIR:-/tmp}}/p51-wrapper-w30-parser.XXXXXX")
    trap 'rm -rf "$fixture_dir"' EXIT HUP INT TERM
    printf '%s\n' \
        '[1] 2026-01-01 00:00:00: P51 cache-link descriptor adopted by sidecar' \
        '[2] 2026-01-01 00:00:05: old group settled' \
        '[3] 2026-01-01 00:00:10: P51 cache-link descriptor adopted by sidecar' \
        '[4] 2026-01-01 00:00:10: Connected to scheduler (I am known as p50-f-a)' \
        '[5] 2026-01-01 00:00:11: P50_INPUT_ATTACH_BEGIN job=66 epoch=1 nonce=1' \
        >"$fixture_dir/two.log"
    w30_validate_adoption_phases "$fixture_dir/two.log" 0 2 4 66 >/dev/null || {
        echo "FAIL: W30 adoption parser rejected the reconnect-plus-recovery fixture" >&2
        exit 1
    }
    printf '%s\n' \
        '[1] 2026-01-01 00:00:00: P51 cache-link descriptor adopted by sidecar' \
        '[2] 2026-01-01 00:00:05: old group settled' \
        '[3] 2026-01-01 00:00:10: P51 cache-link descriptor adopted by sidecar' \
        '[4] 2026-01-01 00:00:10: Connected to scheduler (I am known as p50-f-a)' \
        'no A compiler input attachment during idle interval' \
        '[6] 2026-01-01 00:01:20: P51 cache-link descriptor adopted by sidecar' \
        '[7] 2026-01-01 00:01:21: P50_INPUT_ATTACH_BEGIN job=66 epoch=1 nonce=1' \
        >"$fixture_dir/three-idle.log"
    w30_validate_adoption_phases "$fixture_dir/three-idle.log" 0 2 4 66 >/dev/null || {
        echo "FAIL: W30 adoption parser rejected the 60s-idle-reopen fixture" >&2
        exit 1
    }
    printf '%s\n' \
        '[1] 2026-01-01 00:00:00: P51 cache-link descriptor adopted by sidecar' \
        '[2] 2026-01-01 00:00:05: old group settled' \
        '[3] 2026-01-01 00:00:10: P51 cache-link descriptor adopted by sidecar' \
        '[4] 2026-01-01 00:00:10: Connected to scheduler (I am known as p50-f-a)' \
        '[5] 2026-01-01 00:00:20: P51 cache-link descriptor adopted by sidecar' \
        '[6] 2026-01-01 00:00:21: P50_INPUT_ATTACH_BEGIN job=66 epoch=1 nonce=1' \
        >"$fixture_dir/active-churn.log"
    if w30_validate_adoption_phases "$fixture_dir/active-churn.log" 0 2 4 66 >/dev/null 2>&1; then
        echo "FAIL: W30 adoption parser accepted a third link during active-phase churn" >&2
        exit 1
    fi
    echo "S8_P51_WRAPPER_W30_ADOPTION_FIXTURES two_links=accepted idle_reopen_after_60s=accepted active_churn=rejected fixtures_only=1"
    exit 0
fi

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
worker_session_loss=${ICECC_P51_WRAPPER_WORKER_SESSION_LOSS:-0}
expect_stable_f=${ICECC_P51_WRAPPER_EXPECT_STABLE_F:-0}
staggered_quiescence=${ICECC_P51_WRAPPER_STAGGERED_QUIESCENCE:-0}
c1f2_w30=${ICECC_P51_WRAPPER_C1F2_W30:-0}
case "$worker_session_loss" in
    0|1) ;;
    *) echo "FAIL: ICECC_P51_WRAPPER_WORKER_SESSION_LOSS must be 0 or 1" >&2; exit 1 ;;
esac
case "$expect_stable_f" in
    0|1) ;;
    *) echo "FAIL: ICECC_P51_WRAPPER_EXPECT_STABLE_F must be 0 or 1" >&2; exit 1 ;;
esac
if test "$expect_stable_f" = 1 && test "$worker_session_loss" != 1; then
    echo "FAIL: stable-F expectation requires ICECC_P51_WRAPPER_WORKER_SESSION_LOSS=1" >&2
    exit 1
fi
case "$staggered_quiescence" in
    0|1) ;;
    *) echo "FAIL: ICECC_P51_WRAPPER_STAGGERED_QUIESCENCE must be 0 or 1" >&2; exit 1 ;;
esac
case "$c1f2_w30" in
    0|1) ;;
    *) echo "FAIL: ICECC_P51_WRAPPER_C1F2_W30 must be 0 or 1" >&2; exit 1 ;;
esac
if test "$c1f2_w30" = 1 && { test "$worker_session_loss" != 0 || \
        test "$expect_stable_f" != 0 || test "$staggered_quiescence" != 0; }; then
    echo "FAIL: C1F2 W30 wrapper mode cannot be combined with C1F1 loss modes" >&2
    exit 1
fi
if test "$staggered_quiescence" = 1 && \
        { test "$worker_session_loss" != 1 || test "$expect_stable_f" != 1; }; then
    echo "FAIL: staggered quiescence requires worker-session-loss and strict stable-F mode" >&2
    exit 1
fi
if test "$worker_session_loss" = 1; then
    jobs=2
    test "$staggered_quiescence" = 0 || jobs=3
else
    jobs=${ICECC_P51_WRAPPER_JOBS:-100}
fi
if test "$c1f2_w30" = 1; then
    jobs=31
fi
case "$jobs" in
    ''|*[!0-9]*|0) echo "FAIL: ICECC_P51_WRAPPER_JOBS must be a positive integer" >&2; exit 1 ;;
esac

sh "$src/dev/python.sh" --exec python - "$fixture" "$jobs" "$worker_session_loss" "$staggered_quiescence" <<'PY'
import hashlib
import json
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
count = int(sys.argv[2])
worker_session_loss = sys.argv[3] == "1"
staggered_quiescence = sys.argv[4] == "1"
rows = []
for ordinal in range(count):
    stem = f"tu-{ordinal:02d}"
    if worker_session_loss and (ordinal == 0 or (staggered_quiescence and ordinal == 1)):
        # Deliberately expensive real translation units give the harness time
        # to capture exact remote compiler groups. The normal loss gate makes
        # only ordinal zero heavy; staggered mode makes ordinals zero and one
        # active before scheduler-session loss.
        body = ("\n".join(
            f'extern "C" int p51_worker_loss_{index}(int value) '
            f'{{ return value + {index + 17}; }}'
            for index in range(80000)) + "\n").encode()
    else:
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

profiles=${ICECC_P51_WRAPPER_PROFILES:-"P29V1 ZSTD_TU ZSTD_ROUTE"}
case "$profiles" in
    "P29V1 ZSTD_TU ZSTD_ROUTE"|"P29V1 ZSTD_TU"|P29V1|ZSTD_TU|ZSTD_ROUTE) ;;
    *) echo "FAIL: ICECC_P51_WRAPPER_PROFILES must select one supported profile or the default set" >&2; exit 1 ;;
esac
suite=C1F1/100000
if test "$c1f2_w30" = 1; then suite=C1F2/31; fi
for profile in $profiles; do
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
        ICECC_P50_SUITE="$suite" \
        ICECC_P50_C1F1_BATCH_MANIFEST="$fixture/batch.jsonl" \
        ICECC_P50_C1F1_EXPECTED_COUNT="$jobs" \
        ICECC_P50_C1F1_PASSES=1 \
        ICECC_P50_C1F1_WARM=0 \
        ICECC_P50_C1F1_WORKDIR="$work" \
        ICECC_P50_C1F1_KEEP_WORK=1 \
        ICECC_P50_C1F1_TIMEOUT=300 \
        ICECC_P50_C1F1_TEST_WORKER_SESSION_LOSS="$worker_session_loss" \
        ICECC_P50_C1F1_EXPECT_STABLE_F="$expect_stable_f" \
        ICECC_P50_C1F1_TEST_STAGGERED_QUIESCENCE="$staggered_quiescence" \
        ICECC_P50_C1F2_F_LOSS_W30="$c1f2_w30" \
        "$src/unittests/p50compilee2e-run.sh" >"$log" 2>&1
    status=$?
    set -e
    test "$status" -eq 0 || {
        cat "$log"
        echo "FAIL: actual P51 wrapper compile failed for $profile (status $status)" >&2
        exit 1
    }
    if test "$c1f2_w30" = 1; then
        grep -F "S8_C1F2_W30_F_SPECIFIC_LOSS_PASS profile=$profile exact_A_commits_held=30 healthy_B_progress=1 old_A_group_reaped_before_A_readmission=1 fresh_A_recovery=1" \
            "$log" >/dev/null || {
            cat "$log"
            echo "FAIL: $profile C1F2 W30 F-specific loss/isolation gate did not pass" >&2
            exit 1
        }
    elif test "$worker_session_loss" = 1; then
        grep -F "S8_REAL_WORKER_SESSION_LOSS_PASS profile=$profile committed_victim=1 stopped_and_reaped=1 victim_output=0 recovery_outputs=1 failure_scope=global_scheduler_session" \
            "$log" >/dev/null || {
            cat "$log"
            echo "FAIL: $profile active compiler loss/recovery probe did not pass" >&2
            exit 1
        }
        test ! -e "$work/result-active-worker-loss-0.tsv" && \
            test -s "$work/result-worker-loss-recovery-probe-0.tsv" || {
            echo "FAIL: $profile committed victim/Error24 and fresh recovery outputs disagree with worker-loss scope" >&2
            exit 1
        }
        if test "$staggered_quiescence" = 1; then
            test ! -e "$work/result-active-worker-loss-1.tsv" && \
                test -s "$work/out/grace-ordinary.o" || {
                echo "FAIL: staggered exact groups or grace-period compile did not settle as expected" >&2
                exit 1
            }
            grep -F "S8_REAL_WORKER_LOSS_GRACE_ADMISSION_PASS" "$log" >/dev/null || {
                cat "$log"
                echo "FAIL: $profile did not prove queued client admission after exact cleanup" >&2
                exit 1
            }
        fi
        if test "$expect_stable_f" = 1; then
            grep -F "S8_REAL_WORKER_LOSS_F_LIFECYCLE_DELIVERED" "$log" >/dev/null || {
                cat "$log"
                echo "FAIL: $profile lifecycle was not delivered before compiler-group settlement" >&2
                exit 1
            }
            grep -F "S8_REAL_WORKER_LOSS_F_READY_STABLE" "$log" >/dev/null || {
                cat "$log"
                echo "FAIL: $profile did not preserve the original F READY identity" >&2
                exit 1
            }
        fi
    else
        grep -F "S8_BATCH_COMPLETE run=full-1 count=$jobs" "$log" >/dev/null || {
            echo "FAIL: $profile did not complete all $jobs compiler jobs" >&2
            exit 1
        }
        grep -F "PASS: all-P50 C1F1 $profile compile is remote and byte-identical" \
            "$log" >/dev/null || {
            echo "FAIL: $profile object-comparison gate did not pass" >&2
            exit 1
        }
    fi
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
    if test "$c1f2_w30" = 1; then
        test "$legacy_count" -eq 0 || {
            echo "FAIL: $profile used a legacy R1 cache-session link" >&2
            exit 1
        }
        recovery_job=$(sed -nE \
            '/S8_BATCH_TU run=w30-a-fresh-recovery /s/.*observed_scheduler_job_id=([0-9]+).*/\1/p' \
            "$log" | tail -n 1)
        reconnected_line=$(sed -nE \
            's/.*S8_C1F2_W30_A_QUIESCED_BEFORE_READMISSION .*reconnected_f_log_line=([0-9]+).*/\1/p' \
            "$log" | tail -n 1)
        settled_line=$(sed -nE \
            's/.*S8_C1F2_W30_A_QUIESCED_BEFORE_READMISSION .*settled_f_log_line=([0-9]+).*/\1/p' \
            "$log" | tail -n 1)
        test -n "$recovery_job" && test -n "$reconnected_line" && test -n "$settled_line" || {
            echo "FAIL: $profile lacks the exact fresh-A job or re-login line needed to classify link phases" >&2
            exit 1
        }
        if ! phase_evidence=$(w30_validate_adoption_phases \
                "$work/f.log" "$measured_offset" "$settled_line" "$reconnected_line" "$recovery_job"); then
            echo "FAIL: $profile A adoption phases do not show one active-phase link, loss reconnect, and at most one valid idle reopen" >&2
            exit 1
        fi
        case "$ready_count:$phase_evidence" in
            2:*measured_A_adoptions=2*|3:*measured_A_adoptions=3*) ;;
            *) echo "FAIL: $profile A adoption count disagrees with phase parser: $phase_evidence" >&2; exit 1 ;;
        esac
        b_ready_count=$(grep -F -c 'P51 cache-link descriptor adopted by sidecar' "$work/f-b.log" || true)
        b_legacy_count=$(grep -F -c 'P50_CACHE_SESSION_READY request=' "$work/f-b.log" || true)
        test "$b_ready_count" -eq 1 && test "$b_legacy_count" -eq 0 || {
            cat "$work/f-b.log"
            echo "FAIL: $profile did not preserve exactly one healthy B P51 link " \
                 "(P51-ready=$b_ready_count R1-ready=$b_legacy_count)" >&2
            exit 1
        }
    elif test "$worker_session_loss" = 1; then
        expected_ready_count=2
        if test "$expect_stable_f" = 1; then
            # With the committed CancelAttempt delivered before compiler
            # cleanup, the original F sidecar and its persistent P51 link
            # survive; no replacement link should be counted.
            expected_ready_count=1
        fi
        test "$ready_count" -eq "$expected_ready_count" && test "$legacy_count" -eq 0 || {
            cat "$log"
            cat "$work/f.log"
            echo "FAIL: $profile used an unexpected number of P51 links for active victim and recovery " \
                 "(expected=$expected_ready_count) " \
                 "(P51-ready=$ready_count R1-ready=$legacy_count)" >&2
            exit 1
        }
    elif test "$ready_count" -eq 1 && test "$legacy_count" -eq 0; then
        :
    else
        cat "$log"
        cat "$work/f.log"
        echo "FAIL: $profile did not use exactly one persistent P51 link " \
             "(P51-ready=$ready_count R1-ready=$legacy_count)" >&2
        exit 1
    fi
    if test "$c1f2_w30" = 1; then
        printf 'P51_WRAPPER_C1F2_W30_PASS profile=%s manifest_tus=31 held_A_commits=30 healthy_B_progress=1 fresh_A_recovery=1 A_P51_adoptions=%s B_P51_adoptions=%s phases=%s log=%s f_a_log=%s f_b_log=%s\n' \
            "$profile" "$ready_count" "$b_ready_count" "$phase_evidence" "$log" "$work/f.log" "$work/f-b.log"
    elif test "$worker_session_loss" = 1; then
        printf 'P51_WRAPPER_WORKER_LOSS_PASS profile=%s fixture_tus=%s victim=Error24_no_object fresh_exact_objects=1 P51_adoptions=%s R1_ready=0 log=%s f_log=%s\n' \
            "$profile" "$jobs" "$ready_count" "$log" "$work/f.log"
    else
        printf 'P51_WRAPPER_COMPILE_PASS profile=%s jobs=%s persistent_links=%s log=%s f_log=%s\n' \
            "$profile" "$jobs" "$ready_count" "$log" "$work/f.log"
    fi
done

if test "$c1f2_w30" = 1; then
    echo "PASS: actual P51 C1F2 W30 F-specific loss, healthy-B isolation, cleanup, and fresh-A recovery passed per selected profile"
elif test "$worker_session_loss" = 1; then
    echo "PASS: P51 stopped-victim cleanup and fresh-job exact-object recovery passed per selected profile"
else
    echo "PASS: actual P51 wrapper compiled $jobs TUs on one persistent link per profile"
fi
