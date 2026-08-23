#!/bin/bash
# farm_cell.sh PROJECT_DIR [MODE] [MAXTU] [JOBS] -- runs INSIDE the pinned container as root.
# Stands up a C1F1 icecream cluster (scheduler + worker iceccd -m N + submitter --no-remote),
# cmake-configures PROJECT_DIR (compile_commands.json => per-TU, compile-only, no link),
# then replays every TU REMOTE + local, byte-exact + measured (replay.py). LEGACY fence mode.
set +u
S=/work/source; O=/work/obj; export top=$O
CMAKE=${CMAKE:-/scratch/cmake/bin/cmake}
PROJ=${1:?usage: farm_cell.sh PROJECT_DIR [MODE] [MAXTU] [JOBS]}
export MODE=${2:-full}; export MAXTU=${3:-0}; export JOBS=${4:-8}
id icecc >/dev/null 2>&1 || useradd -r -s /usr/sbin/nologin icecc
: "${TMPDIR:=/scratch/tmp}"; mkdir -p "$TMPDIR"
export work=$(mktemp -d "$TMPDIR/cell.XXXXXX"); export sockdir=$(mktemp -d "$TMPDIR/csk.XXXXXX")
chmod 1777 "$work" "$sockdir"
SCHED_PORT=$((21000 + $$ % 9000)); REMOTE_PORT=$((11000 + $$ % 9000)); NET=cell$$
mkdir -p "$work/env" "$work/envs-remote" "$work/envs-local" "$work/o"; chown icecc "$work/envs-remote" "$work/envs-local"
LP= RP= SP=
cleanup(){ [ -n "$LP" ]&&kill $LP 2>/dev/null; [ -n "$RP" ]&&kill $RP 2>/dev/null; [ -n "$SP" ]&&kill $SP 2>/dev/null; sleep 1; rm -rf "$work" "$sockdir"; }
trap cleanup EXIT
fail(){ echo "CELL-FAIL: $*"; echo "--sched--"; tail -6 "$work/sched.log" 2>/dev/null; echo "--worker--"; tail -8 "$work/remote.log" 2>/dev/null; exit 1; }

# 1) compiler env a client ships
( cd "$work/env" && timeout 180 bash "$top/client/icecc-create-env" "$(command -v g++)" >"$work/cenv.log" 2>&1 )
ENVTAR=$(ls "$work"/env/*.tar.gz 2>/dev/null|head -1); [ -n "$ENVTAR" ] || fail "create-env produced no tarball ($(tail -2 $work/cenv.log))"

# 2) cluster: scheduler + worker(-m N) + submitter(--no-remote)
NW=$(nproc); [ "$NW" -gt 8 ] && NW=8
"$top/scheduler/icecc-scheduler" -p "$SCHED_PORT" -n "$NET" -l "$work/sched.log" -vvv & SP=$!
sleep 2
kill -0 $SP 2>/dev/null || fail "scheduler died ($(tail -2 $work/sched.log))"
ICECC_TEST_SOCKET="$sockdir/remote" "$top/daemon/iceccd" -p "$REMOTE_PORT" -m "$NW" -s "127.0.0.1:$SCHED_PORT" -n "$NET" -N remoteq -b "$work/envs-remote" -l "$work/remote.log" -vvv & RP=$!
ICECC_TEST_SOCKET="$sockdir/local"  "$top/daemon/iceccd" --no-remote -m 0 -s "127.0.0.1:$SCHED_PORT" -n "$NET" -N localq -b "$work/envs-local" -l "$work/local.log" -vvv & LP=$!
L=0; for i in $(seq 1 40); do L=$(grep -c "login" "$work/sched.log" 2>/dev/null); L=${L:-0}; [ "$L" -ge 2 ] 2>/dev/null && break; sleep 1; done
[ "$L" -ge 2 ] 2>/dev/null || fail "daemons never registered (logins=$L)"
grep -q "Cannot use chroot" "$work/remote.log" && fail "worker lacks CAP_SYS_CHROOT"
echo "CELL: cluster up (worker -m$NW, sched port $SCHED_PORT, logins=$L)"

# 3) configure project -> compile_commands.json (configure only, no build/link)
BUILD="$work/cmbuild"
EXTRA="-DCMAKE_BUILD_TYPE=Release"
grep -qi "FMT_TEST\|FMT_DOC" "$PROJ/CMakeLists.txt" 2>/dev/null && EXTRA="$EXTRA -DFMT_TEST=ON -DFMT_DOC=OFF -DFMT_FUZZ=OFF"
"$CMAKE" -S "$PROJ" -B "$BUILD" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $EXTRA >"$work/cfg.log" 2>&1 \
  || fail "cmake configure failed ($(tail -4 $work/cfg.log))"
export CC="$BUILD/compile_commands.json"
[ -f "$CC" ] || fail "no compile_commands.json ($(tail -3 $work/cfg.log))"
echo "CELL: configured $(basename "$PROJ") -> $(python3 -c "import json;print(len(json.load(open('$CC'))),'entries')")"

# 4) replay: every TU REMOTE + local, byte-exact + measured
python3 /scratch/replay.py
