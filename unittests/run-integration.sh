#!/bin/sh
# Hardened serial integration/scenario launcher (local-oracle 5258904367 #7 / 21:30 /
# 22:28).  Runs each DETERMINISTIC scenario sequentially, each with a hard per-scenario
# timeout, a retained per-scenario log + duration, cleanup + verification BEFORE and
# AFTER every scenario, and a final PASS/FAIL/SKIP table with full fingerprints and an
# artifact root.  Continues through failures; exits nonzero if any REQUIRED scenario
# did not pass OR a cleanup miss was observed.  Real-farm, formal (TLA/TLAPS), and
# performance gates are SEPARATE named stages (see run-farm-stage / run-formal-stage /
# run-perf-stage below) with distinct environments -- deliberately not run here.
set -u

dir=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$dir/.." && pwd)
ART="${INTEGRATION_ARTIFACT_ROOT:-/tmp/icecc-integration-$$}"
TIMEOUT="${INTEGRATION_TIMEOUT:-600}"
mkdir -p "$ART" || { echo "cannot create artifact root $ART" >&2; exit 2; }

# scenario NAME REQUIRED|OPTIONAL PATH -- REQUIRED non-PASS forces a nonzero exit.
SCENARIOS="
batchledger-run.sh    REQUIRED $dir/batchledger-run.sh
daemonlogin-run.sh    REQUIRED $dir/daemonlogin-run.sh
daemonbatch-run.sh    REQUIRED $dir/daemonbatch-run.sh
connectivity-run.sh   REQUIRED $dir/connectivity-run.sh
schedbp-quick.sh      REQUIRED $dir/schedbp-quick.sh
schedstress-quick.sh  REQUIRED $dir/schedstress-quick.sh
remoteice-quick.sh    OPTIONAL $dir/remoteice-quick.sh
compose-run.sh        REQUIRED $top/tests/compose/run.sh
"

# --- cleanup: no test daemon/scheduler/compose resource may survive a scenario ---
cleanup() {
    for pat in 'icecc-scheduler .*-n (g4-|cmxb|conn|schedbp|schedstress|remoteice)' \
               'iceccd .*-n (g4-|cmxb|conn|schedbp|schedstress|remoteice)' \
               'iceccd .*-N g4-daemon'; do
        pkill -f "$pat" 2>/dev/null || true
    done
    if command -v docker >/dev/null 2>&1 && [ -f "$top/tests/compose/docker-compose.yml" ]; then
        ( cd "$top/tests/compose" && docker compose down -v --remove-orphans >/dev/null 2>&1 ) || true
    fi
}
# verify cleanup left nothing; echo the count of survivors.
survivors() {
    ps -eo comm 2>/dev/null | grep -cE '^(iceccd|icecc-scheduler)$' || echo 0
}
trap 'cleanup' EXIT HUP INT TERM

echo "=== integration_tests  artifact-root=$ART  per-scenario-timeout=${TIMEOUT}s ==="

# --- full fingerprints (local-oracle 22:28): commit, dirty tree, inputs, binaries ---
fp="$ART/fingerprints.txt"
{
    echo "commit          $(cd "$top" && git rev-parse HEAD 2>/dev/null || echo unknown)"
    if (cd "$top" && git diff --quiet 2>/dev/null); then echo "worktree        clean";
    else echo "worktree        DIRTY  diff-sha256=$(cd "$top" && git diff | sha256sum | cut -d' ' -f1)"; fi
    echo "-- make/runner inputs --"
    for f in "$top/Makefile.am" "$dir/Makefile.am" "$dir/run-integration.sh" \
             "$dir/batchledger-run.sh" "$dir/batchledger.cpp"; do
        [ -f "$f" ] && sha256sum "$f"
    done
    echo "-- executed binaries --"
    for b in "$dir/batchledger" "$dir/daemonlogin" "$dir/daemonbatch" "$dir/schedbp" \
             "$dir/connectivity" "$dir/fastesttest" "$dir/sndbuf_shim.so" "$dir/conn_shim.so" \
             "$top/daemon/iceccd" "$top/scheduler/icecc-scheduler" "$top/client/icecc"; do
        [ -f "$b" ] && sha256sum "$b"
    done
} > "$fp"
echo "--- fingerprints ($fp) ---"; cat "$fp"

rc=0
summary="$ART/summary.txt"
printf '%-22s %-8s %-7s %8s  %s\n' SCENARIO CLASS RESULT DUR_S LOG > "$summary"
echo "$SCENARIOS" | while read -r name klass path; do
    [ -z "$name" ] && continue
    log="$ART/$name.log"
    cleanup                                   # BEFORE
    if [ ! -x "$path" ]; then
        res=SKIP; [ "$klass" = REQUIRED ] && res=MISSING
        printf '%-22s %-8s %-7s %8s  %s\n' "$name" "$klass" "$res" 0 "(absent: $path)" >> "$summary"
        continue
    fi
    t0=$(date +%s)
    timeout "$TIMEOUT" "$path" > "$log" 2>&1
    code=$?
    t1=$(date +%s); dur=$((t1 - t0))
    cleanup                                   # AFTER
    left=$(survivors)
    if   [ "$code" -eq 0 ] && [ "$left" -eq 0 ]; then res=PASS
    elif [ "$code" -eq 77 ]; then res=SKIP
    elif [ "$code" -eq 124 ]; then res=TIMEOUT
    elif [ "$left" -ne 0 ]; then res=CLEANUP-MISS
    else res=FAIL
    fi
    printf '%-22s %-8s %-7s %8s  %s\n' "$name" "$klass" "$res" "$dur" "$log" >> "$summary"
done

# Determine exit: any REQUIRED row not PASS/SKIP (skip only allowed for OPTIONAL) fails.
awk 'NR>1 {
        klass=$2; res=$3;
        if (klass=="REQUIRED" && res!="PASS") bad=1;
        if (res=="CLEANUP-MISS") bad=1;
     } END { exit bad?1:0 }' "$summary" || rc=1

echo; echo "=== integration_tests SUMMARY ==="; cat "$summary"
echo "artifact-root: $ART"; echo "fingerprints:  $fp"
[ "$rc" -eq 0 ] && echo "RESULT: PASS (all required scenarios passed, no cleanup miss)" \
                || echo "RESULT: FAIL (a required scenario did not pass, or a cleanup miss)"
exit $rc
