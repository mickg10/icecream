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

# --- A.4: scenario scratch under the artifact tree, exposed through a SHORT symlink ---
# The canonical $ART/tmp is too deep for a farm UNIX socket (sun_path <= 107 bytes), so
# expose it through one unique short symlink alias and use THAT as TMPDIR.  All bytes stay
# under the canonical artifact; the alias only shortens the path.  Cleaned (alias only) at
# the end, target retained.
TMP_ALIAS="/tanksmall/scratch/ictmp/icecc-tmp-$$"
ln -s "$ART/tmp" "$TMP_ALIAS" || { echo "cannot create short tmp alias $TMP_ALIAS" >&2; exit 2; }
[ "$(readlink -f "$TMP_ALIAS")" = "$(readlink -f "$ART/tmp")" ] \
    || { echo "tmp alias does not resolve to \$ART/tmp" >&2; exit 2; }
export TMPDIR="$TMP_ALIAS"
printf 'alias\t%s\ntarget\t%s\n' "$TMP_ALIAS" "$(readlink -f "$ART/tmp")" > "$ART/tmp-alias.txt"
# assert each farm socket shape fits sun_path under the short alias
for _pref in icecream-g4-batch icecream-g4-login; do
    _len=$(printf '%s' "$TMPDIR/$_pref.XXXXXX/iceccd.sock" | wc -c)
    [ "$_len" -le 107 ] || { echo "RESULT: FAIL (INFRA-FAIL: farm socket shape $_len > 107: $TMPDIR/$_pref.XXXXXX/iceccd.sock)" >&2; exit 2; }
done

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
# active scenario/project identifiers the signal trap remediates mid-flight (item 8)
ACTIVE_COMPOSE_PROJ=""

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
pgid_of() { st=$(cat "/proc/$1/stat" 2>/dev/null) || { printf ''; return; }; rest=${st#*") "}; printf '%s' "$rest" | cut -d' ' -f3; }
MY_PGID=$(pgid_of $$)   # the launcher's own group -- NEVER a remediation target
group_alive() { pgrep -g "$1" >/dev/null 2>&1; }   # exact process-group id, not a name
# TERM then KILL only the exact owned PIDs AND their exact marked process groups (a
# descendant created after the PID snapshot is caught by the group).  The bounded TERM
# grace waits for BOTH the captured PIDs and the captured GROUPS to disappear; then KILL
# only those still present, so a reused group is not signalled.  Never the launcher's own
# PGID; never name-based.
terminate_owned() {
    [ "$#" -eq 0 ] && return 0
    _pgids=""
    for _p in "$@"; do _g=$(pgid_of "$_p"); [ -n "$_g" ] && [ "$_g" != "$MY_PGID" ] && _pgids="$_pgids $_g"; done
    _pgids=$(printf '%s\n' $_pgids | sort -u)
    kill -TERM "$@" 2>/dev/null || true
    for _g in $_pgids; do kill -TERM "-$_g" 2>/dev/null || true; done
    i=0; while [ "$i" -lt 30 ]; do
        alive=0
        for _p in "$@"; do [ -d "/proc/$_p" ] && alive=1; done
        for _g in $_pgids; do group_alive "$_g" && alive=1; done
        [ "$alive" = 0 ] && break
        i=$((i+1)); sleep 0.1
    done
    for _p in "$@"; do [ -d "/proc/$_p" ] && kill -KILL "$_p" 2>/dev/null || true; done
    for _g in $_pgids; do group_alive "$_g" && kill -KILL "-$_g" 2>/dev/null || true; done
}
# Self-recording root wrapper (local-oracle 10:07/10:14): launched under a FORCED
# `setsid -f -w` supervisor so the arrangement is caller-independent -- -f always forks,
# so $! is the supervisor/waiter and the inner sh is the new-session LEADER.  The inner
# sh writes its OWN pid+pgid to $ROOTFILE BEFORE exec'ing the program (race-free, no
# parent /proc poll).  A proper leader has pid==pgid, intentionally != the supervisor;
# -w preserves the timeout/command status through the supervisor's wait.
ROOT_WRAPPER='
  st=$(cat /proc/self/stat) || exit 125
  rest=${st#*") "}
  pgid=$(printf "%s" "$rest" | cut -d" " -f3)
  { printf "role\tpid\tpgid\n"; printf "%s\t%s\t%s\n" "$0" "$$" "$pgid"; } > "$ROOTFILE" || exit 125
  exec timeout --kill-after=10s "$@"
'
# PASS iff a *.root.tsv self-record is present and its leader pid == pgid (a proper
# session leader).  The leader is intentionally NOT the supervisor ($!) under setsid -f.
root_leader_ok() {
    rec=$(tail -1 "$1" 2>/dev/null)
    rpid=$(printf '%s' "$rec" | cut -f2); rpg=$(printf '%s' "$rec" | cut -f3)
    { [ -n "$rpid" ] && [ "$rpid" = "$rpg" ]; } && echo PASS || echo FAIL
}
# Retain supervisor pid, leader pid/pgid, and the RAW wait status (gap 1: classification
# alone loses 125/126/127 and signal-derived values).  $1=rootf $2=supervisor $3=code
record_root_meta() {
    lrec=$(tail -1 "$1" 2>/dev/null); lpid=$(printf '%s' "$lrec" | cut -f2); lpg=$(printf '%s' "$lrec" | cut -f3)
    base=${1%.root.tsv}
    { printf 'supervisor_pid\t%s\nleader_pid\t%s\nleader_pgid\t%s\nwait_status\t%s\n' "$2" "$lpid" "$lpg" "$3"; } > "$base.rootmeta.tsv"
    printf '%s\n' "$3" > "$base.exit-status"
}

# --- signal / early-exit cleanup uses the ownership data (item 8) --------------------
# Records the exact marked processes + the active project's labeled resources, remediates
# ONLY those (TERM->wait->KILL; exact-project compose down), writes post records, removes
# only this run's XDG runtime dir, and preserves the signal-derived nonzero exit.
CLEANED=0
NORMAL_DONE=0
final_cleanup() {
    # gap 4: one-shot -- a signal handler runs this, then the EXIT trap must NOT run a
    # second empty pass that overwrites the nonempty interrupt records.
    [ "$CLEANED" = 1 ] && return 0
    CLEANED=1
    # local-oracle 10:20: reserve interrupt/ for actual HUP/INT/TERM/early-failure paths.
    # A NORMAL completion already did per-scenario scan/remediation + wrote remaining-owned;
    # here it must only drop this run's runtime dir, with no interrupt evidence.
    if [ "$NORMAL_DONE" = 1 ]; then
        rm -rf "$XDG_RUNTIME_DIR" 2>/dev/null || true
        [ -n "${TMP_ALIAS:-}" ] && rm -f "$TMP_ALIAS" 2>/dev/null || true   # alias only; target retained
        return 0
    fi
    _cdir="$ART/interrupt"; mkdir -p "$_cdir" 2>/dev/null || true
    if [ -n "${IT_RUN:-}" ]; then
        intr=$(owned_pids "ICECC_IT_OWNER=$IT_OWNER" "ICECC_IT_RUN=$IT_RUN")
        if [ -n "$intr" ]; then
            # shellcheck disable=SC2086
            proc_tsv $intr > "$_cdir/owned.pre.tsv" 2>/dev/null || true
            # shellcheck disable=SC2086
            terminate_owned $intr
        else
            : > "$_cdir/owned.pre.tsv"
        fi
    fi
    if [ -n "${ACTIVE_COMPOSE_PROJ:-}" ] && command -v docker >/dev/null 2>&1; then
        for _k in containers networks volumes; do
            case "$_k" in containers) _q="ps -a";; networks) _q="network ls";; volumes) _q="volume ls";; esac
            # shellcheck disable=SC2086
            docker $_q --filter "label=com.docker.compose.project=$ACTIVE_COMPOSE_PROJ" -q 2>/dev/null > "$_cdir/compose.pre.$_k" || true
        done
        ( cd "$top/tests/compose" && docker compose -p "$ACTIVE_COMPOSE_PROJ" down -v --remove-orphans >/dev/null 2>&1 ) || true
        for _k in containers networks volumes; do
            case "$_k" in containers) _q="ps -a";; networks) _q="network ls";; volumes) _q="volume ls";; esac
            # shellcheck disable=SC2086
            docker $_q --filter "label=com.docker.compose.project=$ACTIVE_COMPOSE_PROJ" -q 2>/dev/null > "$_cdir/compose.post.$_k" || true
        done
    fi
    if [ -n "${IT_RUN:-}" ]; then
        post=$(owned_pids "ICECC_IT_OWNER=$IT_OWNER" "ICECC_IT_RUN=$IT_RUN")
        # shellcheck disable=SC2086
        { [ -n "$post" ] && proc_tsv $post || :; } > "$_cdir/owned.post.tsv" 2>/dev/null || true
    fi
    rm -rf "$XDG_RUNTIME_DIR" 2>/dev/null || true
    [ -n "${TMP_ALIAS:-}" ] && rm -f "$TMP_ALIAS" 2>/dev/null || true   # alias only; target retained
}
trap 'final_cleanup' EXIT
trap 'final_cleanup; exit 129' HUP
trap 'final_cleanup; exit 130' INT
trap 'final_cleanup; exit 143' TERM

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
# STRICT: `set -e` inside a subshell is SUPPRESSED when the subshell is itself tested by
# `if`/`!` (dash and bash both), so errexit cannot be relied on here.  Instead EVERY
# git/hash operation carries an explicit `|| return 1`; a missing required input makes
# sha256sum fail -> the body returns nonzero -> EVIDENCE-FAIL.  The completed temp file is
# fsync'd, then atomically renamed.
snapshot_body() {
    _commit=$(cd "$top" && git rev-parse HEAD) || return 1
    printf 'commit %s\n' "$_commit"
    printf -- '-- git status --porcelain=v1 --untracked-files=all --\n'
    ( cd "$top" && git status --porcelain=v1 --untracked-files=all ) || return 1
    printf -- '-- sha256: runner + scenario scripts --\n'
    sha256sum "$dir/run-integration.sh" "$dir/batchledger-run.sh" "$dir/remoteice-quick.sh" \
              "$dir/connectivity-run.sh" "$dir/schedbp-quick.sh" "$dir/schedstress-quick.sh" \
              "$dir/daemonlogin-run.sh" "$dir/daemonbatch-run.sh" || return 1
    printf -- '-- sha256: comparator + client env-generator --\n'
    sha256sum "$dir/clientselect.cpp" "$top/daemon/clientselect.h" \
              "$top/client/icecc-create-env" "$top/client/icecc-create-env.in" || return 1
    printf -- '-- sha256: compose inputs (image build + verifier + worker) --\n'
    sha256sum "$top/tests/compose/run.sh" "$top/tests/compose/docker-compose.yml" \
              "$top/tests/compose/Dockerfile" "$top/tests/compose/verify.py" \
              "$top/tests/compose/worker.sh" || return 1
    printf -- '-- sha256: executed binaries / libraries --\n'
    sha256sum "$dir/clientselect" "$dir/batchledger" "$dir/daemonlogin" "$dir/daemonbatch" \
              "$dir/connectivity" "$dir/schedbp" "$dir/fastesttest" \
              "$dir/sndbuf_shim.so" "$dir/conn_shim.so" \
              "$top/daemon/iceccd" "$top/scheduler/icecc-scheduler" "$top/client/icecc" || return 1
    return 0
}
snapshot() {  # $1 = dest dir (start|end)
    d="$1"; tmpf="$d/.evidence.tmp"
    if ! snapshot_body > "$tmpf" 2> "$d/evidence.err"; then
        echo "EVIDENCE-FAIL: snapshot command failed for $d (see $d/evidence.err)" >&2
        rm -f "$tmpf"; return 1
    fi
    if [ ! -s "$tmpf" ]; then echo "EVIDENCE-FAIL: empty snapshot for $d" >&2; rm -f "$tmpf"; return 1; fi
    python3 -c 'import os,sys; fd=os.open(sys.argv[1], os.O_RDONLY); os.fsync(fd); os.close(fd)' "$tmpf" \
        || { echo "EVIDENCE-FAIL: fsync failed for $d" >&2; rm -f "$tmpf"; return 1; }
    mv "$tmpf" "$d/evidence.txt" || { echo "EVIDENCE-FAIL: rename failed for $d" >&2; return 1; }
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
    # B.5: scenario root = env markers + a self-recording FORCED setsid supervisor whose
    # inner session leader writes its own pid/pgid before exec'ing timeout.
    rootf="$ART/scenarios/$name.root.tsv"
    # shellcheck disable=SC2086
    env ICECC_IT_OWNER="$IT_OWNER" ICECC_IT_RUN="$IT_RUN" ICECC_IT_SCENARIO="$scn_id" ROOTFILE="$rootf" $extra \
        setsid -f -w sh -c "$ROOT_WRAPPER" scenario-root "$TIMEOUT" "$path" > "$log" 2>&1 &
    supervisor_pid=$!
    j=0; while [ "$j" -lt 100 ] && [ ! -s "$rootf" ]; do j=$((j+1)); sleep 0.02; done
    wait "$supervisor_pid"; code=$?
    root_ok=$(root_leader_ok "$rootf")
    record_root_meta "$rootf" "$supervisor_pid" "$code"
    t1=$(date +%s); dur=$((t1 - t0))

    # scenario status from the tracked root exit
    scen=FAIL
    [ "$code" -eq 0 ]   && scen=PASS
    [ "$code" -eq 77 ]  && scen=SKIP
    [ "$code" -eq 124 ] && scen=TIMEOUT
    scen=$(worse "$scen" "$root_ok")   # PGID != root PID -> at least FAIL

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
    # C.1: refuse pre-existing containers, networks, OR volumes for this exact project
    if command -v docker >/dev/null 2>&1; then
        ec=$(docker ps -aq --filter "label=com.docker.compose.project=$proj" 2>/dev/null | awk 'END {print NR}')
        en=$(docker network ls -q --filter "label=com.docker.compose.project=$proj" 2>/dev/null | awk 'END {print NR}')
        ev=$(docker volume ls -q --filter "label=com.docker.compose.project=$proj" 2>/dev/null | awk 'END {print NR}')
        if [ "${ec:-0}" -ne 0 ] || [ "${en:-0}" -ne 0 ] || [ "${ev:-0}" -ne 0 ]; then
            printf '%-22s %-8s %-12s %8s  %s\n' "$name" REQUIRED CLEANUP-MISS 0 "(preexisting project $proj c=$ec n=$en v=$ev)" >> "$summary"; return
        fi
    fi
    t0=$(date +%s)
    ACTIVE_COMPOSE_PROJ="$proj"   # let the signal trap remediate this exact project
    rootf="$ART/scenarios/compose-run.sh.root.tsv"
    env ICECC_IT_OWNER="$IT_OWNER" ICECC_IT_RUN="$IT_RUN" ICECC_IT_SCENARIO="$scn_id" ROOTFILE="$rootf" \
        COMPOSE_PROJECT_NAME="$proj" ICECC_RUN_ID="$scn_id" ICECC_OUT_DIR="$ART/compose-out" \
        setsid -f -w sh -c "$ROOT_WRAPPER" compose-root "$TIMEOUT" "$path" > "$log" 2>&1 &
    supervisor_pid=$!
    j=0; while [ "$j" -lt 100 ] && [ ! -s "$rootf" ]; do j=$((j+1)); sleep 0.02; done
    wait "$supervisor_pid"; code=$?
    root_ok=$(root_leader_ok "$rootf")
    record_root_meta "$rootf" "$supervisor_pid" "$code"
    t1=$(date +%s); dur=$((t1 - t0))
    scen=FAIL; [ "$code" -eq 0 ] && scen=PASS; [ "$code" -eq 124 ] && scen=TIMEOUT
    scen=$(worse "$scen" "$root_ok")

    # item 7: HOST-process accounting -- same 2s grace + exact RUN+SCENARIO scan +
    # pre/post TSVs + exact remedial termination as every other scenario, kept SEPARATE
    # from the container/network/volume records below.
    sleep 2
    hleft=$(owned_pids "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$scn_id")
    if [ -n "$hleft" ]; then
        # shellcheck disable=SC2086
        proc_tsv $hleft > "$ART/scenarios/compose.precleanup.tsv"
        scen=$(worse "$scen" CLEANUP-MISS)
        # shellcheck disable=SC2086
        terminate_owned $hleft
    else
        : > "$ART/scenarios/compose.precleanup.tsv"
    fi
    hpost=$(owned_pids "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$scn_id")
    # shellcheck disable=SC2086
    proc_tsv $hpost > "$ART/scenarios/compose.postcleanup.tsv"
    [ -s "$ART/scenarios/compose.postcleanup.tsv" ] && scen=$(worse "$scen" CLEANUP-MISS)

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
    ACTIVE_COMPOSE_PROJ=""
}

# --- run every scenario in order (main shell -- NOT a pipe subshell -- so the signal
#     trap sees the active compose project / run markers) ------------------------------
while read -r name klass path; do
    [ -z "$name" ] && continue
    # D: hand the remote row a retained root under the artifact tree
    if [ "$name" = "remoteice-quick.sh" ]; then
        ICECC_REMOTE_ARTIFACT_ROOT="$ART/scenarios/remoteice"; export ICECC_REMOTE_ARTIFACT_ROOT
    fi
    run_one "$name" "$klass" "$path"
done <<SCENARIO_LIST
$SCENARIOS
SCENARIO_LIST

# --- end snapshot + compare ---------------------------------------------------------
snap_ok=1
snapshot "$ART/end" || snap_ok=0
snap_changed=0
if [ "$snap_ok" = 1 ]; then
    cmp -s "$ART/start/evidence.txt" "$ART/end/evidence.txt" || snap_changed=1
fi

# --- final exit: any required non-PASS / cleanup-miss / evidence / snapshot change --
rc=0
# a REQUIRED row must PASS; an OPTIONAL row may only PASS or SKIP (gap 5); any
# CLEANUP-MISS fails.
awk 'NR>1 {
        if ($2=="REQUIRED" && $3!="PASS") bad=1;
        if ($2=="OPTIONAL" && $3!="PASS" && $3!="SKIP") bad=1;
        if ($3=="CLEANUP-MISS") bad=1;
     } END { exit bad?1:0 }' "$summary" || rc=1
[ "$snap_ok" = 0 ] && rc=1
[ "$snap_changed" = 1 ] && rc=1
# a remaining owned process across the whole run is a failure.  Item 6/8: always write
# remaining-owned.tsv (even empty) so the final evidence is inspectable on PASS too, and
# remediate any remaining exact-run processes AFTER recording them.
remain=$(owned_pids "ICECC_IT_OWNER=$IT_OWNER" "ICECC_IT_RUN=$IT_RUN")
if [ -n "$remain" ]; then
    # shellcheck disable=SC2086
    proc_tsv $remain > "$ART/remaining-owned.tsv"; rc=1
    # shellcheck disable=SC2086
    terminate_owned $remain
else
    : > "$ART/remaining-owned.tsv"
fi

echo; echo "=== integration_tests SUMMARY ==="; cat "$summary"
echo "artifact-root: $ART"
echo "tmp-alias:     $TMP_ALIAS -> $(readlink -f "$TMP_ALIAS" 2>/dev/null || echo '(removed)')"
[ "$snap_changed" = 1 ] && echo "NOTE: start/end evidence snapshots differ"
[ -n "${remain:-}" ] && echo "NOTE: owned processes remained at end (see remaining-owned.tsv)"
[ "$rc" -eq 0 ] && echo "RESULT: PASS (all required scenarios passed, no cleanup miss, stable evidence)" \
                || echo "RESULT: FAIL (a required scenario did not pass, a cleanup miss, evidence failure, or snapshot change)"
# reached the normal end: the EXIT trap must not synthesize interrupt evidence
NORMAL_DONE=1
exit $rc
