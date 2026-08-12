#!/bin/sh
# G4 count>1 batch-ledger complete gate (issue #4).  Every named case runs in a fresh
# daemon/session; RED at 61e2b73, GREEN with the P_batch ledger correction.
#
# Nested per-case contract (local-oracle 5263490925 commit 3): each of the 37 cases runs
# under a HARD per-case timeout as a `setsid` root tagged with a distinct ICECC_IT_CASE,
# into a RETAINED per-case log + duration, with before/after owned-process listings
# recorded BEFORE any termination; leftovers make the case CLEANUP-MISS; termination is
# by exact owned PID only (never pkill/pgrep by name); one CASE/RESULT/DUR/LOG table with
# start/end fingerprints under a unique artifact root; nonzero on any non-PASS.  POSIX sh.
set -u

dir=$(cd "$(dirname "$0")" && pwd -P)
iceccd="$dir/../daemon/iceccd"
bl="$dir/batchledger"
TIMEOUT="${BATCHLEDGER_CASE_TIMEOUT:-90}"

# must run from the source unittests dir
here=$(pwd -P)
if [ "$here" != "$dir" ]; then
    echo "batchledger-run.sh requires CWD == $dir (got $here)" >&2; exit 2
fi

# explicit artifact root (no /tmp fallback: the host root fs may be full)
if [ -z "${BATCHLEDGER_ARTIFACT_ROOT:-}" ]; then
    echo "BATCHLEDGER_ARTIFACT_ROOT must be set (no /tmp fallback)" >&2; exit 2
fi
mkdir -p "$BATCHLEDGER_ARTIFACT_ROOT" 2>/dev/null \
    || { echo "cannot create artifact root $BATCHLEDGER_ARTIFACT_ROOT" >&2; exit 2; }
ART="$BATCHLEDGER_ARTIFACT_ROOT/run-$$"
mkdir "$ART" 2>/dev/null || { echo "run dir $ART exists/uncreatable; refusing to overwrite" >&2; exit 2; }
mkdir -p "$ART/cases" "$ART/tmp"
# scenario scratch under the artifact tree (inherit an outer TMPDIR only if already scratch)
export TMPDIR="$ART/tmp"

# ownership markers: inherit owner/run/scenario from the integration launcher if present,
# else generate them (standalone).  Markers are injected only on each CASE root (B.3).
IT_OWNER="${ICECC_IT_OWNER:-icecc-it-v1}"
IT_RUN="${ICECC_IT_RUN:-blrun-$$-$(date -u +%s 2>/dev/null || echo 0)}"
IT_SCEN="${ICECC_IT_SCENARIO:-blscn-$$}"

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

# --- exact-ownership helpers (scan /proc environ; never pkill/pgrep by name) --------
owned_pids() {  # print PIDs whose environ contains ALL given exact KEY=VAL tokens
    for envf in /proc/[0-9]*/environ; do
        [ -r "$envf" ] || continue
        p=${envf#/proc/}; p=${p%/environ}
        ok=1
        for tok in "$@"; do
            tr '\0' '\n' < "$envf" 2>/dev/null | grep -Fxq -- "$tok" || { ok=0; break; }
        done
        [ "$ok" = 1 ] && printf '%s\n' "$p"
    done
}
proc_tsv() {
    for p in "$@"; do
        [ -r "/proc/$p/stat" ] || continue
        st=$(cat "/proc/$p/stat" 2>/dev/null) || continue
        rest=${st#*") "}
        state=$(printf '%s' "$rest" | cut -d' ' -f1)
        ppid=$(printf '%s' "$rest" | cut -d' ' -f2)
        pgid=$(printf '%s' "$rest" | cut -d' ' -f3)
        cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
        printf '%s\t%s\t%s\t%s\t%s\n' "$p" "$ppid" "$pgid" "$state" "$cmd"
    done
}
terminate_owned() {
    [ "$#" -eq 0 ] && return 0
    kill -TERM "$@" 2>/dev/null || true
    i=0; while [ "$i" -lt 30 ]; do
        alive=0; for p in "$@"; do [ -d "/proc/$p" ] && alive=1; done
        [ "$alive" = 0 ] && return 0
        i=$((i+1)); sleep 0.1
    done
    kill -KILL "$@" 2>/dev/null || true
}
sev() { case "$1" in CLEANUP-MISS) echo 6;; TIMEOUT) echo 5;; MISSING) echo 4;; FAIL) echo 3;; SKIP) echo 2;; PASS) echo 1;; *) echo 0;; esac; }
worse() { if [ "$(sev "$1")" -ge "$(sev "$2")" ]; then echo "$1"; else echo "$2"; fi; }

# reject a pre-existing tagged process for THIS run before starting any case
pre=$(owned_pids "ICECC_IT_OWNER=$IT_OWNER" "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$IT_SCEN")
if [ -n "$pre" ]; then
    # shellcheck disable=SC2086
    proc_tsv $pre > "$ART/preexisting.tsv"
    echo "RESULT: FAIL (PREEXISTING owned process; see $ART/preexisting.tsv)" >&2; exit 3
fi

# start/end input fingerprints (reject a change mid-run)
hash_inputs() { for b in "$bl" "$iceccd" "$dir/batchledger.cpp" "$dir/batchledger-run.sh"; do [ -f "$b" ] && sha256sum "$b"; done; }
hash_inputs > "$ART/fingerprints-start.txt"
start_inputs=$(hash_inputs)

echo "=== batchledger-run  artifact-root=$ART  per-case-timeout=${TIMEOUT}s  run=$IT_RUN ==="

rc=0
summary="$ART/summary.txt"
printf '%-22s %-13s %8s  %s\n' CASE RESULT DUR_S LOG > "$summary"
if [ ! -x "$bl" ]; then
    printf '%-22s %-13s %8s  %s\n' "(binary)" MISSING 0 "(absent: $bl)" >> "$summary"; rc=1
fi

for c in $CASES; do
    [ -z "$c" ] && continue
    [ -x "$bl" ] || break
    log="$ART/cases/$c.log"
    case_id="case-$c-$$"
    t0=$(date +%s)
    # per-case root: distinct ICECC_IT_CASE marker + setsid + timeout
    env ICECC_IT_OWNER="$IT_OWNER" ICECC_IT_RUN="$IT_RUN" ICECC_IT_SCENARIO="$IT_SCEN" ICECC_IT_CASE="$case_id" \
        setsid -w timeout --kill-after=10s "$TIMEOUT" "$bl" "$iceccd" "$c" > "$log" 2>&1 &
    root=$!; wait "$root"; code=$?
    t1=$(date +%s); dur=$((t1 - t0))

    scen=FAIL
    [ "$code" -eq 0 ]   && scen=PASS
    [ "$code" -eq 124 ] && scen=TIMEOUT

    sleep 1
    left=$(owned_pids "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$IT_SCEN" "ICECC_IT_CASE=$case_id")
    if [ -n "$left" ]; then
        # shellcheck disable=SC2086
        proc_tsv $left > "$ART/cases/$c.precleanup.tsv"
        res=$(worse "$scen" CLEANUP-MISS)
        # shellcheck disable=SC2086
        terminate_owned $left
    else
        : > "$ART/cases/$c.precleanup.tsv"; res="$scen"
    fi
    post=$(owned_pids "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$IT_SCEN" "ICECC_IT_CASE=$case_id")
    # shellcheck disable=SC2086
    proc_tsv $post > "$ART/cases/$c.postcleanup.tsv"
    [ -s "$ART/cases/$c.postcleanup.tsv" ] && res=$(worse "$res" CLEANUP-MISS)

    printf '%-22s %-13s %8s  %s\n' "$c" "$res" "$dur" "$log" >> "$summary"
    [ "$res" = PASS ] || { rc=1; echo "batchledger case $res: $c (log $log)" >&2; }
done

hash_inputs > "$ART/fingerprints-end.txt"
[ "$start_inputs" != "$(cat "$ART/fingerprints-end.txt")" ] && { echo "input/binary changed during the run" >&2; rc=1; }

echo; echo "=== batchledger SUMMARY ==="; cat "$summary"
echo "artifact-root: $ART"
[ "$rc" -eq 0 ] && echo "RESULT: PASS (all cases passed, no cleanup miss, stable inputs)" \
                || echo "RESULT: FAIL (a case did not pass, a cleanup miss, or inputs changed)"
exit $rc
