#!/bin/bash
# Walking skeleton + acceptance gate (runs INSIDE the pinned container as root).
# Stands up C1F1 (scheduler + worker iceccd -m2 + submitter --no-remote), then distributes
# a REAL multi-file compile through icecc, byte-exact-cmp each object vs a local reference,
# and measures wire bytes + wall time. LEGACY mode. SSD scratch via TMPDIR.
set +u
S=/work/source; O=/work/obj
top=$O
id icecc >/dev/null 2>&1 || useradd -r -s /usr/sbin/nologin icecc
: "${TMPDIR:=/tmp}"
work=$(mktemp -d "$TMPDIR/skel.XXXXXX"); sockdir=$(mktemp -d "$TMPDIR/skq.XXXXXX")
chmod 1777 "$work" "$sockdir"
SCHED_PORT=$((21000 + $$ % 9000)); REMOTE_PORT=$((11000 + $$ % 9000)); NET=skel$$
ICEUSER=icecc
mkdir -p "$work/env" "$work/envs-remote" "$work/envs-local"; chown $ICEUSER "$work/envs-remote" "$work/envs-local"
cleanup(){ [ -n "$LP" ]&&kill $LP 2>/dev/null; [ -n "$RP" ]&&kill $RP 2>/dev/null; [ -n "$SP" ]&&kill $SP 2>/dev/null; sleep 1; rm -rf "$work" "$sockdir"; }
trap cleanup EXIT
fail(){ echo "SKEL-FAIL: $*"; echo "--sched--"; tail -8 "$work/sched.log" 2>/dev/null; echo "--worker--"; tail -12 "$work/remote.log" 2>/dev/null; exit 1; }

# compiler env (what a client ships)
( cd "$work/env" && timeout 180 bash "$top/client/icecc-create-env" "$(command -v g++)" >"$work/cenv.log" 2>&1 )
ENVTAR=$(ls "$work"/env/*.tar.gz 2>/dev/null|head -1); [ -n "$ENVTAR" ] || fail "create-env produced no tarball ($(tail -3 $work/cenv.log))"

"$top/scheduler/icecc-scheduler" -p "$SCHED_PORT" -n "$NET" -l "$work/sched.log" -vvv & SP=$!
sleep 2
echo "DBG: sched pid=$SP alive=$(kill -0 $SP 2>/dev/null&&echo Y||echo N) port=$SCHED_PORT listening=$(ss -ltn 2>/dev/null|grep -cE ":$SCHED_PORT ")"
echo "DBG: sched.log head: $(head -3 "$work/sched.log" 2>/dev/null|tr '\n' '|')"
ICECC_TEST_SOCKET="$sockdir/remote" "$top/daemon/iceccd" -p "$REMOTE_PORT" -m 2 -s "127.0.0.1:$SCHED_PORT" -n "$NET" -N remoteq -b "$work/envs-remote" -l "$work/remote.log" -vvv & RP=$!
ICECC_TEST_SOCKET="$sockdir/local"  "$top/daemon/iceccd" --no-remote -m 0 -s "127.0.0.1:$SCHED_PORT" -n "$NET" -N localq -b "$work/envs-local" -l "$work/local.log" -vvv & LP=$!
# wait for both daemons to register
kill -0 $SP 2>/dev/null || fail "scheduler died at startup ($(tail -3 $work/sched.log 2>/dev/null))"
L=0
for i in $(seq 1 40); do L=$(grep -c "login" "$work/sched.log" 2>/dev/null); L=${L:-0}; [ "$L" -ge 2 ] 2>/dev/null && break; sleep 1; done
[ "$L" -ge 2 ] 2>/dev/null || fail "daemons never registered (logins=$L); sched: $(grep -iE 'login|connect|got' $work/sched.log 2>/dev/null|tail -3); worker: $(grep -iE 'scheduler|connect|login|fail' $work/remote.log 2>/dev/null|tail -3)"
grep -q "Cannot use chroot" "$work/remote.log" && fail "worker lacks CAP_SYS_CHROOT"

# --- REAL distributed build: compile a set of real icecream TUs, remote via icecc + local ref, cmp + measure ---
FLAGS="-std=c++17 -O2 -I$O -I$S -I$S/services -I$S/client -I$S/daemon"
FILES="services/logging.cpp services/comm.cpp services/job.cpp client/arg.cpp client/cpp.cpp"
n=0; ok=0; wire=0; t0=$(date +%s%N)
for rel in $FILES; do
  f="$S/$rel"; [ -f "$f" ] || continue
  n=$((n+1)); ro="$work/r_$n.o"; lo="$work/l_$n.o"; clog="$work/c_$n.log"
  ICECC_TEST_SOCKET="$sockdir/local" ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=remoteq ICECC_DEBUG=debug ICECC_LOGFILE="$clog" \
    timeout 120 "$top/client/icecc" g++ $FLAGS -c "$f" -o "$ro" >"$work/o_$n.out" 2>&1
  rc=$?
  /usr/bin/g++ $FLAGS -c "$f" -o "$lo" 2>"$work/le_$n.log"; lrc=$?
  wb=$(grep -oE "got [0-9]+ bytes" "$clog" 2>/dev/null | grep -oE "[0-9]+" | awk '{s+=$1} END{print s+0}'); wb=${wb:-0}
  remote_ok=$(grep -qE "building myself|local build forced" "$clog" 2>/dev/null && echo LOCAL || echo REMOTE)
  if [ $rc -eq 0 ] && [ $lrc -eq 0 ] && [ "$remote_ok" = REMOTE ] && cmp -s "$ro" "$lo"; then
    ok=$((ok+1)); wire=$((wire+wb)); echo "  OK  $rel  ($(stat -c%s "$ro")B obj, wire ${wb}B, $remote_ok)"
  else
    echo "  BAD $rel  rc=$rc lrc=$lrc path=$remote_ok cmp=$(cmp -s "$ro" "$lo" 2>/dev/null && echo same || echo DIFF)  $(grep -m1 -iE 'error|fail' "$work/o_$n.out" "$work/le_$n.log" 2>/dev/null|head -1)"
  fi
done
t1=$(date +%s%N); dt_ms=$(( (t1 - t0) / 1000000 )); [ "$dt_ms" -lt 1 ] && dt_ms=1
echo "SKEL-RESULT: byte-exact-remote $ok/$n  total-wire=${wire}B  wall=$(awk "BEGIN{printf \"%.2f\", $dt_ms/1000}")s  jobs/s=$(awk "BEGIN{printf \"%.2f\", $n*1000/$dt_ms}")"
[ "$ok" -eq "$n" ] && [ "$n" -gt 0 ] && echo "SKEL: PASS (all $n TUs built REMOTE + byte-identical to local)" || echo "SKEL: INCOMPLETE ($ok/$n)"
