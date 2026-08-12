#!/bin/sh
# G4 count>1 batch-ledger complete gate (issue #4).  Every named case runs in a fresh
# daemon/session; RED at 61e2b73, GREEN with the P_batch ledger correction.
#
# Per-case contract (local-oracle 23:08/23:16 + launcher corrections 5259702949): each
# case runs under a HARD per-case timeout, into a RETAINED per-case log, with a recorded
# wall-clock duration; the suite cleans this run's test daemons BEFORE and AFTER every
# case, verifies only THIS run's state (test-tagged full-command patterns, bounded
# wait), prints a final CASE/RESULT/DUR/LOG table + fingerprints + a unique artifact
# root, continues through failures, and exits nonzero if ANY case did not PASS or a
# cleanup miss was observed.  Cleanup status takes precedence over case status.
set -u

dir=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$dir/.." && pwd)
iceccd="$dir/../daemon/iceccd"
bl="$dir/batchledger"
TIMEOUT="${BATCHLEDGER_CASE_TIMEOUT:-90}"

# #8: never overwrite prior evidence -- always a unique run directory.  If the caller
# names a root, nest a unique child under it; otherwise use a pid-stamped path.
if [ -n "${BATCHLEDGER_ARTIFACT_ROOT:-}" ]; then
    mkdir -p "$BATCHLEDGER_ARTIFACT_ROOT" 2>/dev/null \
        || { echo "cannot create artifact root $BATCHLEDGER_ARTIFACT_ROOT" >&2; exit 2; }
    ART="$BATCHLEDGER_ARTIFACT_ROOT/run-$$"
else
    ART="/tmp/batchledger-$$"
fi
# the unique per-run child is created non-recursively so a repeated run token is refused
mkdir "$ART" 2>/dev/null || { echo "artifact run dir $ART exists/uncreatable; refusing to overwrite" >&2; exit 2; }

CASES="self-control \
 client-done-filter late-usecs teardown-clean local-capacity batch-nocs \
 teardown-eof-0 teardown-eof-1 teardown-eof-2 teardown-eof-3 teardown-early-1 \
 teardown-local-queued teardown-local-active teardown-normal-local \
 scalar-count0 scalar-count1-remote scalar-count1-local \
 mixed-LRR mixed-RLR mixed-RNR nocs-dedup-excess \
 transition-1 transition-2 transition-3 \
 invalid-jobdone local-lifetime compile-started jobbegin-send-fail \
 usecs-fields helper-controls eof-controls fifo-controls done-controls reader-controls \
 audit-jobbegin audit-delivery-fail audit-sched-loss"

# This run's test daemons/schedulers only, by test-tagged full command lines
# (#2: ps -eo comm truncates 'icecc-scheduler' and would also match unrelated system
# icecream processes).  These are the patterns the cases actually launch.
RUN_PATS="iceccd .*-n g4-|iceccd .*-N g4-daemon|icecc-scheduler .*-n g4-"

cleanup() {
    old_ifs=$IFS; IFS='|'
    for pat in $RUN_PATS; do pkill -f "$pat" 2>/dev/null || true; done
    IFS=$old_ifs
}

# #1: exactly one integer on stdout (no `grep -c || echo 0` double-print).  #2: bounded
# wait so a just-signalled child that is still exiting is not miscounted as a survivor.
survivors() {
    i=0
    while [ "$i" -lt 20 ]; do
        n=$(pgrep -fc "$RUN_PATS" 2>/dev/null); [ -n "$n" ] || n=0
        [ "$n" -eq 0 ] && break
        i=$((i + 1)); sleep 0.1
    done
    echo "$n"
}

# #4: explicit signal handlers that clean up and exit with a signal-derived status;
# EXIT stays as a backstop.
trap 'cleanup' EXIT
trap 'cleanup; exit 129' HUP
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

echo "=== batchledger-run  artifact-root=$ART  per-case-timeout=${TIMEOUT}s ==="

# Reject any tagged test process that already exists at launcher start.
pre_existing=$(survivors)
if [ "$pre_existing" -ne 0 ]; then
    echo "RESULT: FAIL (pre-existing tagged test process(es) at start: $pre_existing)" >&2
    exit 3
fi

# The executed inputs/binaries, hashed identically at start and end so a change
# during the run is detectable (a normal build must not be able to invalidate the
# evidence mid-run).
hash_inputs() {
    for b in "$bl" "$iceccd" "$dir/batchledger.cpp" "$dir/batchledger-run.sh"; do
        [ -f "$b" ] && sha256sum "$b"
    done
}
# #6: fingerprint porcelain status, tracked-vs-HEAD, and the executed inputs/binaries.
fp="$ART/fingerprints-start.txt"
{
    echo "commit          $(cd "$top" && git rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "-- git status --porcelain --"
    (cd "$top" && git status --porcelain 2>/dev/null) || true
    echo "-- tracked diff vs HEAD (sha256) --"
    echo "  $(cd "$top" && git diff HEAD -- . 2>/dev/null | sha256sum | cut -d' ' -f1)"
    echo "-- inputs / binaries (start) --"
    hash_inputs
} > "$fp"
echo "--- fingerprints ($fp) ---"; cat "$fp"
start_inputs=$(hash_inputs)

rc=0
summary="$ART/summary.txt"
printf '%-22s %-13s %8s  %s\n' CASE RESULT DUR_S LOG > "$summary"
for c in $CASES; do
    [ -z "$c" ] && continue
    log="$ART/$c.log"
    cleanup                                     # BEFORE (defensive)
    if [ ! -x "$bl" ]; then
        printf '%-22s %-13s %8s  %s\n' "$c" MISSING 0 "(absent: $bl)" >> "$summary"; rc=1; continue
    fi
    t0=$(date +%s)
    timeout "$TIMEOUT" "$bl" "$iceccd" "$c" > "$log" 2>&1
    code=$?
    t1=$(date +%s); dur=$((t1 - t0))
    # Detect + RECORD leftovers BEFORE terminating them: survivors() applies a bounded
    # normal-exit grace, so a nonzero count here is a genuine cleanup miss by the case,
    # not a just-signalled child.  Classification uses this count; cleanup() then
    # terminates the leftovers so the next case starts clean.
    left=$(survivors)
    # #5: cleanup status takes precedence over case status.
    if   [ "$left" -ne 0 ];   then res=CLEANUP-MISS; rc=1
    elif [ "$code" -eq 0 ];   then res=PASS
    elif [ "$code" -eq 124 ]; then res=TIMEOUT; rc=1
    else res=FAIL; rc=1
    fi
    cleanup                                     # AFTER (terminate any recorded leftovers)
    printf '%-22s %-13s %8s  %s\n' "$c" "$res" "$dur" "$log" >> "$summary"
    [ "$res" = PASS ] || echo "batchledger case $res: $c (log $log)" >&2
done

# End fingerprints + reject any input/binary change during the run.
efp="$ART/fingerprints-end.txt"; hash_inputs > "$efp"
if [ "$start_inputs" != "$(cat "$efp")" ]; then
    echo "RESULT: FAIL (input/binary changed during the run -- start vs end hashes differ)" >&2
    rc=1
fi

echo; echo "=== batchledger SUMMARY ==="; cat "$summary"
echo "artifact-root: $ART"; echo "fingerprints:  $fp  +  $efp"
[ "$rc" -eq 0 ] && echo "RESULT: PASS (all cases passed, no cleanup miss, stable inputs)" \
                || echo "RESULT: FAIL (a case did not pass, a cleanup miss, or inputs changed)"
exit $rc
