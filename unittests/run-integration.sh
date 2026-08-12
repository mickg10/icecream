#!/bin/sh
# Hardened serial integration/scenario launcher (local-oracle 5258904367 / 5259702949 /
# 5260775013).  Runs each DETERMINISTIC scenario sequentially, each with a hard timeout,
# a retained per-scenario log + duration, this-run-only cleanup + verification before and
# after every scenario, and a final PASS/FAIL/SKIP/CLEANUP-MISS table with start+end
# fingerprints and a unique artifact root.  Continues through failures; exits nonzero if
# any REQUIRED scenario did not pass, a cleanup miss was observed, or inputs changed
# during the run.  Real-farm, formal (TLA/TLAPS), and performance gates are SEPARATE
# manual stages with their own environments and evidence; they are intentionally NOT run
# here and this launcher claims no entry point for them.
set -u

dir=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$dir/.." && pwd)
TIMEOUT="${INTEGRATION_TIMEOUT:-600}"

# A known, per-run compose project id, shared by the scenario and every cleanup/verify.
proj="${INTEGRATION_COMPOSE_PROJECT:-icecc-integ-$$}"
export COMPOSE_PROJECT_NAME="$proj"

# Unique artifact root: never overwrite prior evidence (nest a unique child if the caller
# names an existing root).
if [ -n "${INTEGRATION_ARTIFACT_ROOT:-}" ]; then
    mkdir -p "$INTEGRATION_ARTIFACT_ROOT" 2>/dev/null \
        || { echo "cannot create artifact root $INTEGRATION_ARTIFACT_ROOT" >&2; exit 2; }
    ART="$INTEGRATION_ARTIFACT_ROOT/run-$$"
else
    ART="/tmp/icecc-integration-$$"
fi
# the unique per-run child is created non-recursively so a repeated run token is refused
mkdir "$ART" 2>/dev/null || { echo "artifact run dir $ART exists/uncreatable; refusing to overwrite" >&2; exit 2; }

# scenario NAME REQUIRED|OPTIONAL PATH -- a REQUIRED non-PASS forces a nonzero exit.
SCENARIOS="
clientselect          REQUIRED $dir/clientselect
batchledger-run.sh    REQUIRED $dir/batchledger-run.sh
daemonlogin-run.sh    REQUIRED $dir/daemonlogin-run.sh
daemonbatch-run.sh    REQUIRED $dir/daemonbatch-run.sh
connectivity-run.sh   REQUIRED $dir/connectivity-run.sh
schedbp-quick.sh      REQUIRED $dir/schedbp-quick.sh
schedstress-quick.sh  REQUIRED $dir/schedstress-quick.sh
remoteice-quick.sh    OPTIONAL $dir/remoteice-quick.sh
compose-run.sh        REQUIRED $top/tests/compose/run.sh
"

# --- this-run daemons/schedulers by test-tagged FULL command lines (#2: ps -eo comm
# truncates 'icecc-scheduler' and would also match unrelated system icecream). ---
RUN_PATS="iceccd .*-n (g4-|cmxb|conn|schedbp|schedstress|remoteice)|iceccd .*-N g4-daemon|icecc-scheduler .*-n (g4-|cmxb|conn|schedbp|schedstress|remoteice)"

compose_down() {
    if command -v docker >/dev/null 2>&1 && [ -f "$top/tests/compose/docker-compose.yml" ]; then
        ( cd "$top/tests/compose" && docker compose -p "$proj" down -v --remove-orphans >/dev/null 2>&1 ) || true
    fi
}
cleanup() { pkill -f "$RUN_PATS" 2>/dev/null || true; compose_down; }

# #1: exactly one integer.  #2: bounded wait so a just-signalled child is not miscounted.
survivors() {
    i=0
    while [ "$i" -lt 20 ]; do
        n=$(pgrep -fc "$RUN_PATS" 2>/dev/null); [ -n "$n" ] || n=0
        [ "$n" -eq 0 ] && break
        i=$((i + 1)); sleep 0.1
    done
    echo "$n"
}
# Separate compose-resource count for this run's project.
compose_survivors() {
    if command -v docker >/dev/null 2>&1; then
        docker ps -q --filter "label=com.docker.compose.project=$proj" 2>/dev/null | grep -c . || echo 0
    else
        echo 0
    fi
}

# #4: signal handlers that clean up and exit nonzero; EXIT stays as a backstop.
trap 'cleanup' EXIT
trap 'cleanup; exit 129' HUP
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

echo "=== integration_tests  artifact-root=$ART  per-scenario-timeout=${TIMEOUT}s  compose-project=$proj ==="

# Reject pre-existing tagged test processes at start.
pre=$(survivors)
if [ "$pre" -ne 0 ]; then echo "RESULT: FAIL (pre-existing tagged test process(es): $pre)" >&2; exit 3; fi

# --- #6: start fingerprints (porcelain, tracked diff, untracked inputs, runners, image) ---
hash_inputs() {
    for f in "$dir/batchledger" "$dir/clientselect" "$dir/daemonlogin" "$dir/daemonbatch" "$dir/schedbp" \
             "$dir/connectivity" "$dir/fastesttest" "$dir/sndbuf_shim.so" "$dir/conn_shim.so" \
             "$top/daemon/iceccd" "$top/scheduler/icecc-scheduler" "$top/client/icecc" \
             "$dir/run-integration.sh" "$dir/batchledger-run.sh" "$dir/clientselect.cpp" \
             "$top/daemon/clientselect.h" "$top/tests/compose/run.sh" \
             "$top/tests/compose/docker-compose.yml"; do
        [ -f "$f" ] && sha256sum "$f"
    done
}
fp="$ART/fingerprints-start.txt"
{
    echo "top             $top"
    echo "commit          $(cd "$top" && git rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "-- git status --porcelain --"
    (cd "$top" && git status --porcelain 2>/dev/null) || true
    echo "-- tracked diff vs HEAD (sha256) --"
    echo "  $(cd "$top" && git diff HEAD -- . 2>/dev/null | sha256sum | cut -d' ' -f1)"
    echo "-- inputs / binaries / compose (start) --"
    hash_inputs
} > "$fp"
echo "--- fingerprints ($fp) ---"; cat "$fp"
start_inputs=$(hash_inputs)

rc=0
summary="$ART/summary.txt"
printf '%-22s %-8s %-12s %8s  %s\n' SCENARIO CLASS RESULT DUR_S LOG > "$summary"
echo "$SCENARIOS" | while read -r name klass path; do
    [ -z "$name" ] && continue
    log="$ART/$name.log"
    cleanup                                     # BEFORE (defensive)
    if [ ! -x "$path" ]; then
        res=SKIP; [ "$klass" = REQUIRED ] && res=MISSING
        printf '%-22s %-8s %-12s %8s  %s\n' "$name" "$klass" "$res" 0 "(absent: $path)" >> "$summary"
        continue
    fi
    t0=$(date +%s)
    timeout "$TIMEOUT" "$path" > "$log" 2>&1
    code=$?
    t1=$(date +%s); dur=$((t1 - t0))
    # #5: record cleanup state BEFORE terminating; cleanup outranks scenario status.
    left=$(survivors); cleft=$(compose_survivors)
    scen=FAIL
    [ "$code" -eq 0 ] && scen=PASS
    [ "$code" -eq 77 ] && scen=SKIP
    [ "$code" -eq 124 ] && scen=TIMEOUT
    if [ "$left" -ne 0 ] || [ "$cleft" -ne 0 ]; then res=CLEANUP-MISS; else res=$scen; fi
    cleanup                                     # AFTER (terminate any recorded leftovers)
    printf '%-22s %-8s %-12s %8s  %s\n' "$name" "$klass" "$res" "$dur" "$log" >> "$summary"
done

# End fingerprints + reject any input/binary change during the run.
efp="$ART/fingerprints-end.txt"; hash_inputs > "$efp"
inputs_changed=0
[ "$start_inputs" != "$(cat "$efp")" ] && inputs_changed=1

# Exit: any REQUIRED row not PASS (SKIP only for OPTIONAL) fails; any CLEANUP-MISS fails;
# inputs changing mid-run fails.
awk 'NR>1 {
        klass=$2; res=$3;
        if (klass=="REQUIRED" && res!="PASS") bad=1;
        if (res=="CLEANUP-MISS") bad=1;
     } END { exit bad?1:0 }' "$summary" || rc=1
[ "$inputs_changed" -eq 1 ] && rc=1

echo; echo "=== integration_tests SUMMARY ==="; cat "$summary"
echo "artifact-root: $ART"; echo "fingerprints:  $fp  +  $efp"
[ "$inputs_changed" -eq 1 ] && echo "NOTE: inputs/binaries changed during the run (start vs end differ)"
[ "$rc" -eq 0 ] && echo "RESULT: PASS (all required scenarios passed, no cleanup miss, stable inputs)" \
                || echo "RESULT: FAIL (a required scenario did not pass, a cleanup miss, or inputs changed)"
exit $rc
