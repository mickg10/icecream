#!/bin/sh
# Hardened serial integration launcher with EXACT per-run process ownership, canonical
# start/end evidence, and scratch-backed storage (local-oracle 5263490925 commit 3).
# POSIX sh.  Real-farm / formal / performance validation remain SEPARATE manual stages
# with their own environments; this launcher provides no entry point for them.
set -u

dir=$(cd "$(dirname "$0")" && pwd -P)
top=$(cd "$dir/.." && pwd -P)

# --- A.1: must run from the source unittests dir (the top-level target recurses here) -
here=$(pwd -P)
if [ "$here" != "$dir" ]; then
    echo "run-integration.sh requires CWD == $dir (got $here)" >&2
    exit 2
fi

# --- A.2: explicit artifact root; no silent /tmp fallback --------------------------
if [ -z "${INTEGRATION_ARTIFACT_ROOT:-}" ]; then
    echo "INTEGRATION_ARTIFACT_ROOT must be set (no /tmp fallback)" >&2
    exit 2
fi
mkdir -p "$INTEGRATION_ARTIFACT_ROOT" 2>/dev/null \
    || { echo "cannot create artifact root $INTEGRATION_ARTIFACT_ROOT" >&2; exit 2; }

# --- A.3: one unique child $ART with its evidence/scratch subdirs ------------------
ART="$INTEGRATION_ARTIFACT_ROOT/run-$$"
mkdir "$ART" 2>/dev/null || { echo "run dir $ART exists/uncreatable; refusing to overwrite" >&2; exit 2; }
for d in start end scenarios batchledger tmp compose-out; do
    mkdir -p "$ART/$d" || { echo "cannot create $ART/$d" >&2; exit 2; }
done

# --- A.4: scenario scratch under the artifact tree ---------------------------------
export TMPDIR="$ART/tmp"

# --- A.5: short dedicated runtime root on the scratch fs; clean only this path ------
rt_id=$$
XDG_RUNTIME_DIR="/tanksmall/scratch/ictmp/icecc-rt-$rt_id"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR" \
    || { echo "cannot create runtime dir $XDG_RUNTIME_DIR" >&2; exit 2; }
export XDG_RUNTIME_DIR
printf '%s\n' "$XDG_RUNTIME_DIR" > "$ART/xdg_runtime_dir.txt"

# --- exact-ownership marker scheme (B) ---------------------------------------------
IT_OWNER=icecc-it-v1
IT_RUN="itrun-$$-$(date -u +%s 2>/dev/null || echo 0)"

# --- signal handlers: clean only OUR runtime dir + exit nonzero (B/EXIT backstop) ---
final_cleanup() { rm -rf "$XDG_RUNTIME_DIR" 2>/dev/null || true; }
trap 'final_cleanup' EXIT
trap 'final_cleanup; exit 129' HUP
trap 'final_cleanup; exit 130' INT
trap 'final_cleanup; exit 143' TERM

echo "=== integration_tests  artifact-root=$ART  timeout=${INTEGRATION_TIMEOUT:-600}s  run=$IT_RUN ==="
TIMEOUT="${INTEGRATION_TIMEOUT:-600}"

# ---- process-ownership helpers (scan /proc environ; never pkill/pgrep by name) ----
# print PIDs whose /proc/PID/environ contains ALL given exact KEY=VAL tokens
owned_pids() {
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
# PID<TAB>PPID<TAB>PGID<TAB>STATE<TAB>COMMAND for each pid argument
proc_tsv() {
    for p in "$@"; do
        [ -r "/proc/$p/stat" ] || continue
        st=$(cat "/proc/$p/stat" 2>/dev/null) || continue
        rest=${st#*") "}                 # strip "PID (comm) "
        # rest = state ppid pgrp session ...
        state=$(printf '%s' "$rest" | cut -d' ' -f1)
        ppid=$(printf '%s' "$rest" | cut -d' ' -f2)
        pgid=$(printf '%s' "$rest" | cut -d' ' -f3)
        cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
        printf '%s\t%s\t%s\t%s\t%s\n' "$p" "$ppid" "$pgid" "$state" "$cmd"
    done
}
# TERM then KILL only the given owned PIDs (bounded wait); never name-based
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

# --- B.2: reject a pre-existing tagged test process BEFORE injecting any marker -----
pre=$(owned_pids "ICECC_IT_OWNER=$IT_OWNER")
if [ -n "$pre" ]; then
    # shellcheck disable=SC2086
    proc_tsv $pre > "$ART/preexisting.tsv"
    echo "RESULT: FAIL (PREEXISTING tagged test process(es); see $ART/preexisting.tsv)" >&2
    cat "$ART/preexisting.tsv" >&2
    exit 3
fi

# --- A.6: write+fsync probe and >=2GiB free in $ART and $TMPDIR (INFRA-FAIL=exit 2) -
infra_probe() {
    for d in "$ART" "$TMPDIR"; do
        python3 - "$d" <<'PY' || return 1
import os, sys
d = sys.argv[1]
p = os.path.join(d, ".probe")
try:
    fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.write(fd, b"probe\n"); os.fsync(fd); os.close(fd); os.unlink(p)
    st = os.statvfs(d)
    avail = st.f_bavail * st.f_frsize
    if avail < 2 * 1024**3:
        sys.stderr.write("less than 2GiB free in %s (%d bytes)\n" % (d, avail)); sys.exit(1)
except OSError as e:
    sys.stderr.write("probe failed in %s: %s\n" % (d, e)); sys.exit(1)
PY
    done
    return 0
}
if ! infra_probe; then
    echo "RESULT: FAIL (INFRA-FAIL: write/fsync probe or free-space check failed)" >&2
    exit 2
fi

# --- E: one canonical snapshot() used for both start/ and end/ ----------------------
snapshot() {  # $1 = dest dir (start|end)
    d="$1"; tmpf="$ART/.snap.$$"
    {
        echo "commit $(cd "$top" && git rev-parse HEAD 2>/dev/null || echo unknown)"
        echo "-- git status --porcelain=v1 --untracked-files=all --"
        (cd "$top" && git status --porcelain=v1 --untracked-files=all 2>/dev/null) || true
        echo "-- sha256: runner + scenario inputs --"
        for f in "$dir/run-integration.sh" "$dir/batchledger-run.sh" "$dir/remoteice-quick.sh" \
                 "$dir/connectivity-run.sh" "$dir/schedbp-quick.sh" "$dir/schedstress-quick.sh" \
                 "$dir/daemonlogin-run.sh" "$dir/daemonbatch-run.sh" "$dir/clientselect.cpp" \
                 "$top/daemon/clientselect.h"; do
            [ -f "$f" ] && sha256sum "$f"
        done
        echo "-- sha256: executed binaries / libraries --"
        for b in "$dir/clientselect" "$dir/batchledger" "$dir/daemonlogin" "$dir/daemonbatch" \
                 "$dir/connectivity" "$dir/schedbp" "$dir/fastesttest" \
                 "$dir/sndbuf_shim.so" "$dir/conn_shim.so" \
                 "$top/daemon/iceccd" "$top/scheduler/icecc-scheduler" "$top/client/icecc"; do
            [ -f "$b" ] && sha256sum "$b"
        done
        echo "-- sha256: compose inputs / worker script --"
        for c in "$top/tests/compose/run.sh" "$top/tests/compose/docker-compose.yml" \
                 "$top/tests/compose/Dockerfile"; do
            [ -f "$c" ] && sha256sum "$c"
        done
    } > "$tmpf" 2>/dev/null
    if [ ! -s "$tmpf" ]; then
        echo "EVIDENCE-FAIL: empty snapshot for $d" >&2; rm -f "$tmpf"; return 1
    fi
    sync
    mv "$tmpf" "$d/evidence.txt" || { echo "EVIDENCE-FAIL: cannot place $d/evidence.txt" >&2; return 1; }
    return 0
}
if ! snapshot "$ART/start"; then echo "RESULT: FAIL (EVIDENCE-FAIL at start)" >&2; exit 2; fi

# --- scenario table -----------------------------------------------------------------
# NAME CLASS PATH
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

summary="$ART/summary.txt"
printf '%-22s %-8s %-12s %8s  %s\n' SCENARIO CLASS RESULT DUR_S LOG > "$summary"

# classification precedence helper: pick the more severe of two results
# order: CLEANUP-MISS > TIMEOUT > MISSING > FAIL > SKIP > PASS
sev() { case "$1" in CLEANUP-MISS) echo 6;; TIMEOUT) echo 5;; MISSING) echo 4;; FAIL) echo 3;; SKIP) echo 2;; PASS) echo 1;; *) echo 0;; esac; }
worse() { if [ "$(sev "$1")" -ge "$(sev "$2")" ]; then echo "$1"; else echo "$2"; fi; }

run_one() {  # $1 name  $2 class  $3 path
    name="$1"; klass="$2"; path="$3"
    log="$ART/scenarios/$name.log"
    scn_id="scn-$name-$$"
    if [ ! -x "$path" ]; then
        res=SKIP; [ "$klass" = REQUIRED ] && res=MISSING
        printf '%-22s %-8s %-12s %8s  %s\n' "$name" "$klass" "$res" 0 "(absent: $path)" >> "$summary"
        return
    fi

    # compose row (C) has its own ownership + resource accounting
    if [ "$name" = "compose-run.sh" ]; then
        run_compose "$path"
        return
    fi

    # nested batch runner (B.10-12): inherit owner/run/scenario, per-case markers inside
    extra=""
    if [ "$name" = "batchledger-run.sh" ]; then
        extra="BATCHLEDGER_ARTIFACT_ROOT=$ART/batchledger"
    fi

    t0=$(date +%s)
    # B.5: scenario root = env markers + setsid -w + timeout; tracked as the root PGID
    # shellcheck disable=SC2086
    env ICECC_IT_OWNER="$IT_OWNER" ICECC_IT_RUN="$IT_RUN" ICECC_IT_SCENARIO="$scn_id" $extra \
        setsid -w timeout --kill-after=10s "$TIMEOUT" "$path" > "$log" 2>&1 &
    root=$!
    wait "$root"; code=$?
    t1=$(date +%s); dur=$((t1 - t0))

    # scenario status from the tracked root exit
    scen=FAIL
    [ "$code" -eq 0 ]   && scen=PASS
    [ "$code" -eq 77 ]  && scen=SKIP
    [ "$code" -eq 124 ] && scen=TIMEOUT

    # B.5 grace, B.6 record owned leftovers BEFORE terminating
    sleep 2
    left=$(owned_pids "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$scn_id")
    if [ -n "$left" ]; then
        # shellcheck disable=SC2086
        proc_tsv $left > "$ART/scenarios/$name.precleanup.tsv"
        res=$(worse "$scen" CLEANUP-MISS)
        # shellcheck disable=SC2086
        terminate_owned $left
    else
        : > "$ART/scenarios/$name.precleanup.tsv"
        res="$scen"
    fi
    post=$(owned_pids "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$scn_id")
    # shellcheck disable=SC2086
    proc_tsv $post > "$ART/scenarios/$name.postcleanup.tsv"
    [ -s "$ART/scenarios/$name.postcleanup.tsv" ] && res=$(worse "$res" CLEANUP-MISS)

    printf '%-22s %-8s %-12s %8s  %s\n' "$name" "$klass" "$res" "$dur" "$log" >> "$summary"
}

run_compose() {  # $1 path
    path="$1"; name=compose-run.sh; log="$ART/scenarios/$name.log"
    scn_id="scn-compose-$$"
    proj=$(printf 'icecc_%s' "$scn_id" | tr -c 'a-zA-Z0-9' '_' | tr 'A-Z' 'a-z')
    # C.1: refuse pre-existing resources for this exact project label
    if command -v docker >/dev/null 2>&1; then
        exist=$(docker ps -aq --filter "label=com.docker.compose.project=$proj" 2>/dev/null | awk 'END {print NR}')
        [ "${exist:-0}" -ne 0 ] && { printf '%-22s %-8s %-12s %8s  %s\n' "$name" REQUIRED CLEANUP-MISS 0 "(preexisting project $proj)" >> "$summary"; return; }
    fi
    t0=$(date +%s)
    # shellcheck disable=SC2086
    env ICECC_IT_OWNER="$IT_OWNER" ICECC_IT_RUN="$IT_RUN" ICECC_IT_SCENARIO="$scn_id" \
        COMPOSE_PROJECT_NAME="$proj" ICECC_RUN_ID="$scn_id" ICECC_OUT_DIR="$ART/compose-out" \
        setsid -w timeout --kill-after=10s "$TIMEOUT" "$path" > "$log" 2>&1 &
    root=$!; wait "$root"; code=$?
    t1=$(date +%s); dur=$((t1 - t0))
    scen=FAIL; [ "$code" -eq 0 ] && scen=PASS; [ "$code" -eq 124 ] && scen=TIMEOUT
    # C.2/3: record labeled resources AFTER the script's own shutdown, BEFORE remedial
    if command -v docker >/dev/null 2>&1; then
        docker ps -a  --filter "label=com.docker.compose.project=$proj" --format '{{.ID}} {{.Names}} {{.Status}}' 2>/dev/null > "$ART/scenarios/compose.pre.containers"
        docker network ls --filter "label=com.docker.compose.project=$proj" --format '{{.ID}} {{.Name}}' 2>/dev/null > "$ART/scenarios/compose.pre.networks"
        docker volume ls  --filter "label=com.docker.compose.project=$proj" --format '{{.Name}}' 2>/dev/null > "$ART/scenarios/compose.pre.volumes"
        c=$(awk 'END {print NR}' "$ART/scenarios/compose.pre.containers")
        n=$(awk 'END {print NR}' "$ART/scenarios/compose.pre.networks")
        v=$(awk 'END {print NR}' "$ART/scenarios/compose.pre.volumes")
        res="$scen"
        if [ "$c" -ne 0 ] || [ "$n" -ne 0 ] || [ "$v" -ne 0 ]; then
            res=$(worse "$scen" CLEANUP-MISS)
            # C.4/5: remedial exact-project down only after recording the miss
            ( cd "$top/tests/compose" && docker compose -p "$proj" down -v --remove-orphans >/dev/null 2>&1 ) || true
        fi
        docker ps -a  --filter "label=com.docker.compose.project=$proj" --format '{{.ID}} {{.Names}}' 2>/dev/null > "$ART/scenarios/compose.post.containers"
        docker network ls --filter "label=com.docker.compose.project=$proj" --format '{{.ID}} {{.Name}}' 2>/dev/null > "$ART/scenarios/compose.post.networks"
        docker volume ls  --filter "label=com.docker.compose.project=$proj" --format '{{.Name}}' 2>/dev/null > "$ART/scenarios/compose.post.volumes"
        pc=$(awk 'END {print NR}' "$ART/scenarios/compose.post.containers")
        pn=$(awk 'END {print NR}' "$ART/scenarios/compose.post.networks")
        pv=$(awk 'END {print NR}' "$ART/scenarios/compose.post.volumes")
        { [ "$pc" -ne 0 ] || [ "$pn" -ne 0 ] || [ "$pv" -ne 0 ]; } && res=$(worse "$res" CLEANUP-MISS)
    else
        res=$(worse "$scen" MISSING)
    fi
    printf '%-22s %-8s %-12s %8s  %s\n' "$name" REQUIRED "$res" "$dur" "$log" >> "$summary"
}

# --- run every scenario in order ---------------------------------------------------
echo "$SCENARIOS" | while read -r name klass path; do
    [ -z "$name" ] && continue
    # D: hand the remote row a retained root under the artifact tree
    if [ "$name" = "remoteice-quick.sh" ]; then
        ICECC_REMOTE_ARTIFACT_ROOT="$ART/scenarios/remoteice" export ICECC_REMOTE_ARTIFACT_ROOT
    fi
    run_one "$name" "$klass" "$path"
done

# --- end snapshot + compare ---------------------------------------------------------
snap_ok=1
snapshot "$ART/end" || snap_ok=0
snap_changed=0
if [ "$snap_ok" = 1 ]; then
    cmp -s "$ART/start/evidence.txt" "$ART/end/evidence.txt" || snap_changed=1
fi

# --- final exit: any required non-PASS / cleanup-miss / evidence / snapshot change --
rc=0
awk 'NR>1 { if ($2=="REQUIRED" && $3!="PASS") bad=1; if ($3=="CLEANUP-MISS") bad=1 } END { exit bad?1:0 }' "$summary" || rc=1
[ "$snap_ok" = 0 ] && rc=1
[ "$snap_changed" = 1 ] && rc=1
# a remaining owned process across the whole run is a failure
remain=$(owned_pids "ICECC_IT_RUN=$IT_RUN")
[ -n "$remain" ] && { proc_tsv $remain > "$ART/remaining-owned.tsv"; rc=1; }

echo; echo "=== integration_tests SUMMARY ==="; cat "$summary"
echo "artifact-root: $ART"
[ "$snap_changed" = 1 ] && echo "NOTE: start/end evidence snapshots differ"
[ -n "${remain:-}" ] && echo "NOTE: owned processes remained at end (see remaining-owned.tsv)"
[ "$rc" -eq 0 ] && echo "RESULT: PASS (all required scenarios passed, no cleanup miss, stable evidence)" \
                || echo "RESULT: FAIL (a required scenario did not pass, a cleanup miss, evidence failure, or snapshot change)"
exit $rc
