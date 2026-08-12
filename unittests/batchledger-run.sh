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
# The short-tmp alias is created AFTER the cleanup traps (below) so a failure between its
# creation and the trap install cannot leak an owned alias.  OUR_ALIAS is the alias WE
# create (bl_cleanup removes only this); ACTIVE_ALIAS is whichever alias is in use.
OUR_ALIAS=""
ACTIVE_ALIAS=""

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
pgid_of() { st=$(cat "/proc/$1/stat" 2>/dev/null) || { printf ''; return; }; rest=${st#*") "}; printf '%s' "$rest" | cut -d' ' -f3; }
MY_PGID=$(pgid_of $$)
group_alive() { pgrep -g "$1" >/dev/null 2>&1; }   # exact process-group id, not a name
# TERM/KILL exact owned PIDs AND their exact marked groups; never our own group.  The
# bounded TERM grace waits for BOTH the captured PIDs and the captured GROUPS to
# disappear (a same-group descendant created after the snapshot keeps the group alive);
# then KILL only the PIDs/groups STILL present, so a reused group is not signalled.
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
# retain supervisor pid, leader pid/pgid, raw wait status (gap 1)
record_root_meta() {
    lrec=$(tail -1 "$1" 2>/dev/null); lpid=$(printf '%s' "$lrec" | cut -f2); lpg=$(printf '%s' "$lrec" | cut -f3)
    base=${1%.root.tsv}
    { printf 'supervisor_pid\t%s\nleader_pid\t%s\nleader_pgid\t%s\nwait_status\t%s\n' "$2" "$lpid" "$lpg" "$3"; } > "$base.rootmeta.tsv"
    printf '%s\n' "$3" > "$base.exit-status"
}
# Self-recording root wrapper (local-oracle 10:07/10:14): each case runs under a FORCED
# `setsid -f -w` supervisor; the inner session leader writes its OWN pid+pgid to
# $ROOTFILE before exec'ing the program (race-free).  A proper leader has pid==pgid,
# intentionally != the supervisor ($!); -w preserves the command status.
ROOT_WRAPPER='
  st=$(cat /proc/self/stat) || exit 125
  rest=${st#*") "}
  pgid=$(printf "%s" "$rest" | cut -d" " -f3)
  { printf "role\tpid\tpgid\n"; printf "%s\t%s\t%s\n" "$0" "$$" "$pgid"; } > "$ROOTFILE" || exit 125
  exec timeout --kill-after=10s "$@"
'
root_leader_ok() {
    rec=$(tail -1 "$1" 2>/dev/null)
    rpid=$(printf '%s' "$rec" | cut -f2); rpg=$(printf '%s' "$rec" | cut -f3)
    { [ -n "$rpid" ] && [ "$rpid" = "$rpg" ]; } && echo PASS || echo FAIL
}
sev() { case "$1" in CLEANUP-MISS) echo 6;; TIMEOUT) echo 5;; MISSING) echo 4;; FAIL) echo 3;; SKIP) echo 2;; PASS) echo 1;; *) echo 0;; esac; }
worse() { if [ "$(sev "$1")" -ge "$(sev "$2")" ]; then echo "$1"; else echo "$2"; fi; }

# gap 3 / critical (local-oracle 10:20): standalone HUP/INT/TERM cleanup that remediates
# ONLY the currently-active case, scanned by the EXACT four tokens
# OWNER+RUN+SCENARIO+CASE=$ACTIVE_CASE.  It must NEVER scan only inherited
# OWNER+RUN+SCENARIO -- in a nested run those also match the outer timeout, the batch
# runner, and its parent chain, so a normal EXIT could terminate its own parent.  When no
# case is active (normal EXIT after all cases), bl_cleanup terminates nothing.  One-shot
# so a signal handler's records are not overwritten by the EXIT trap.
ACTIVE_CASE=""
BL_CLEANED=0
bl_cleanup() {
    [ "$BL_CLEANED" = 1 ] && return 0
    BL_CLEANED=1
    if [ -n "$ACTIVE_CASE" ]; then     # a case was interrupted mid-flight -> remediate it
        mkdir -p "$ART/interrupt" 2>/dev/null || true
        own=$(owned_pids "ICECC_IT_OWNER=$IT_OWNER" "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$IT_SCEN" "ICECC_IT_CASE=$ACTIVE_CASE")
        if [ -n "$own" ]; then
            # shellcheck disable=SC2086
            proc_tsv $own > "$ART/interrupt/owned.pre.tsv" 2>/dev/null || true
            # shellcheck disable=SC2086
            terminate_owned $own
        else
            : > "$ART/interrupt/owned.pre.tsv"
        fi
        post=$(owned_pids "ICECC_IT_OWNER=$IT_OWNER" "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$IT_SCEN" "ICECC_IT_CASE=$ACTIVE_CASE")
        # shellcheck disable=SC2086
        { [ -n "$post" ] && proc_tsv $post || :; } > "$ART/interrupt/owned.post.tsv" 2>/dev/null || true
    fi
    # remove only our own short alias (symlink), never the target -- after process cleanup
    [ -n "$OUR_ALIAS" ] && rm -f "$OUR_ALIAS" 2>/dev/null || true
}
trap 'bl_cleanup' EXIT
trap 'bl_cleanup; exit 129' HUP
trap 'bl_cleanup; exit 130' INT
trap 'bl_cleanup; exit 143' TERM

# Now that bl_cleanup owns OUR_ALIAS, choose/create the short-tmp alias.  Honor an inherited
# TMPDIR only when it resolves BENEATH our artifact root (then it is NOT ours to remove);
# else create our own short alias -> $ART/tmp, assigned to OUR_ALIAS only after `ln -s`
# succeeds.  BOTH paths record the active alias + physical target; only OUR_ALIAS is removed.
_use_inherited=0
if [ -n "${TMPDIR:-}" ]; then
    _it=$(readlink -f "$TMPDIR" 2>/dev/null)
    _br=$(readlink -f "$BATCHLEDGER_ARTIFACT_ROOT" 2>/dev/null)
    case "$_it/" in "$_br"/*) _use_inherited=1;; esac
fi
if [ "$_use_inherited" = 1 ]; then
    ACTIVE_ALIAS="$TMPDIR"                       # inherited -> not owned; keep as-is
else
    _cand="/tanksmall/scratch/ictmp/icecc-tmp-bl-$$"
    ln -s "$ART/tmp" "$_cand" || { echo "cannot create short tmp alias $_cand" >&2; exit 2; }
    OUR_ALIAS="$_cand"
    [ "$(readlink -f "$OUR_ALIAS")" = "$(readlink -f "$ART/tmp")" ] \
        || { echo "tmp alias does not resolve to \$ART/tmp" >&2; exit 2; }
    export TMPDIR="$OUR_ALIAS"
    ACTIVE_ALIAS="$OUR_ALIAS"
fi
printf 'alias\t%s\ntarget\t%s\n' "$ACTIVE_ALIAS" "$(readlink -f "$TMPDIR")" > "$ART/tmp-alias.txt"
_len=$(printf '%s' "$TMPDIR/icecream-g4-batch.XXXXXX/iceccd.sock" | wc -c)
[ "$_len" -le 107 ] || { echo "RESULT: FAIL (INFRA-FAIL: farm socket shape $_len > 107)" >&2; exit 2; }

# NOTE: there is deliberately no run/scenario-marker preflight here -- a nested invocation
# legitimately runs inside processes that already carry the inherited OWNER+RUN+SCENARIO
# (this runner and the outer timeout).  The only invariant checked is the exact FOUR-token
# per-case one (OWNER+RUN+SCENARIO+CASE=case_id), done in the loop below with a fresh id.

# start/end input fingerprints (reject a change mid-run)
# STRICT: every listed input is required and every hash must succeed (a missing file
# makes sha256sum fail -> the function returns nonzero; no partial fingerprint).
hash_inputs() { sha256sum "$bl" "$iceccd" "$dir/batchledger.cpp" "$dir/batchledger-run.sh" || return 1; }
# Write a fingerprint to a SIBLING temp, retain stderr, fsync, then atomically rename --
# a direct `hash_inputs > final` would truncate the final name and leave a partial file
# on a mid-list sha256sum failure.  The final path exists only on full success.
write_fingerprint() {  # $1 = final path
    _t="$1.tmp"; _e="$1.err"
    if ! hash_inputs > "$_t" 2> "$_e"; then rm -f "$_t"; return 1; fi
    python3 -c 'import os,sys; fd=os.open(sys.argv[1], os.O_RDONLY); os.fsync(fd); os.close(fd)' "$_t" \
        || { rm -f "$_t"; return 1; }
    mv "$_t" "$1" || return 1
    return 0
}
if ! write_fingerprint "$ART/fingerprints-start.txt"; then
    echo "RESULT: FAIL (fingerprint(start) generation failed; see $ART/fingerprints-start.txt.err)" >&2; exit 2
fi

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
    # per-case preflight: the exact four-token invariant -- no process may already carry
    # OWNER+RUN+SCENARIO+CASE=case_id (the fresh id).  Empty record on PASS; a collision
    # (never expected with a fresh id) is recorded and exits 3.
    pcase=$(owned_pids "ICECC_IT_OWNER=$IT_OWNER" "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$IT_SCEN" "ICECC_IT_CASE=$case_id")
    if [ -n "$pcase" ]; then
        # shellcheck disable=SC2086
        proc_tsv $pcase > "$ART/cases/$c.preexisting.tsv"
        echo "batchledger PREEXISTING case marker: $c" >&2; exit 3
    fi
    : > "$ART/cases/$c.preexisting.tsv"
    t0=$(date +%s)
    # per-case root: distinct ICECC_IT_CASE marker + forced-supervisor setsid + timeout
    rootf="$ART/cases/$c.root.tsv"
    ACTIVE_CASE="$case_id"   # arm the signal trap for THIS case only
    env ICECC_IT_OWNER="$IT_OWNER" ICECC_IT_RUN="$IT_RUN" ICECC_IT_SCENARIO="$IT_SCEN" ICECC_IT_CASE="$case_id" ROOTFILE="$rootf" \
        setsid -f -w sh -c "$ROOT_WRAPPER" case-root "$TIMEOUT" "$bl" "$iceccd" "$c" > "$log" 2>&1 &
    supervisor_pid=$!
    j=0; while [ "$j" -lt 100 ] && [ ! -s "$rootf" ]; do j=$((j+1)); sleep 0.02; done
    wait "$supervisor_pid"; code=$?
    root_ok=$(root_leader_ok "$rootf")
    record_root_meta "$rootf" "$supervisor_pid" "$code"
    t1=$(date +%s); dur=$((t1 - t0))

    scen=FAIL
    [ "$code" -eq 0 ]   && scen=PASS
    [ "$code" -eq 124 ] && scen=TIMEOUT
    scen=$(worse "$scen" "$root_ok")   # leader pid != pgid -> at least FAIL

    sleep 1
    left=$(owned_pids "ICECC_IT_OWNER=$IT_OWNER" "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$IT_SCEN" "ICECC_IT_CASE=$case_id")
    if [ -n "$left" ]; then
        # shellcheck disable=SC2086
        proc_tsv $left > "$ART/cases/$c.precleanup.tsv"
        res=$(worse "$scen" CLEANUP-MISS)
        # shellcheck disable=SC2086
        terminate_owned $left
    else
        : > "$ART/cases/$c.precleanup.tsv"; res="$scen"
    fi
    post=$(owned_pids "ICECC_IT_OWNER=$IT_OWNER" "ICECC_IT_RUN=$IT_RUN" "ICECC_IT_SCENARIO=$IT_SCEN" "ICECC_IT_CASE=$case_id")
    # shellcheck disable=SC2086
    proc_tsv $post > "$ART/cases/$c.postcleanup.tsv"
    [ -s "$ART/cases/$c.postcleanup.tsv" ] && res=$(worse "$res" CLEANUP-MISS)
    ACTIVE_CASE=""   # this case's scan + remediation is complete; disarm the trap

    printf '%-22s %-13s %8s  %s\n' "$c" "$res" "$dur" "$log" >> "$summary"
    [ "$res" = PASS ] || { rc=1; echo "batchledger case $res: $c (log $log)" >&2; }
done

if ! write_fingerprint "$ART/fingerprints-end.txt"; then
    echo "fingerprint(end) generation failed (see $ART/fingerprints-end.txt.err)" >&2; rc=1
elif ! cmp -s "$ART/fingerprints-start.txt" "$ART/fingerprints-end.txt"; then
    echo "input/binary changed during the run" >&2; rc=1
fi

echo; echo "=== batchledger SUMMARY ==="; cat "$summary"
echo "artifact-root: $ART"
# print the active alias + physical target BEFORE the EXIT trap removes an owned alias
echo "tmp-alias:     $ACTIVE_ALIAS -> $(readlink -f "$TMPDIR" 2>/dev/null)"
[ "$rc" -eq 0 ] && echo "RESULT: PASS (all cases passed, no cleanup miss, stable inputs)" \
                || echo "RESULT: FAIL (a case did not pass, a cleanup miss, or inputs changed)"
exit $rc
