#!/bin/bash
# farm_client.sh SCHED_IP:PORT NET PROJECT MODE MAXTU JOBS PREFER
# Runs INSIDE a container on the CLIENT host. The cluster (external scheduler + remote workers)
# is ALREADY up (launched by farm.py). This launches only a local submitter daemon (--no-remote -m0,
# so it never builds locally => forces every job REMOTE), configures the project, and replays every
# TU REMOTE + local, byte-exact + measured. The submitter registers with the external scheduler.
set +u
S=/work/source; O=/work/obj; export top=$O
CMAKE=${CMAKE:-/scratch/cmake/bin/cmake}
SCHED=${1:?usage: SCHED_IP:PORT NET PROJECT [MODE] [MAXTU] [JOBS] [PREFER]}
NET=${2:?net}; PROJ=${3:?project}; export MODE=${4:-full}; export MAXTU=${5:-0}; export JOBS=${6:-8}; export PREFER=${7:-}
id icecc >/dev/null 2>&1 || useradd -r icecc
: "${TMPDIR:=/scratch/tmp}"; mkdir -p "$TMPDIR"
export work=$(mktemp -d "$TMPDIR/cli.XXXXXX"); export sockdir=$(mktemp -d "$TMPDIR/csk.XXXXXX")
chmod 1777 "$work" "$sockdir"
mkdir -p "$work/env" "$work/envs-local" "$work/o"; chown icecc "$work/envs-local"
LP=
cleanup(){ [ -n "$LP" ]&&kill $LP 2>/dev/null; sleep 1; [ -n "$NOCLEAN" ] && { echo "CLIENT: NOCLEAN, work=$work"; return; }; rm -rf "$work" "$sockdir"; }
trap cleanup EXIT
fail(){ echo "CLIENT-FAIL: $*"; echo "--sub--"; tail -8 "$work/sub.log" 2>/dev/null; exit 1; }

# 1) compiler env this client ships to the farm
( cd "$work/env" && timeout 180 bash "$top/client/icecc-create-env" "$(command -v g++)" >"$work/cenv.log" 2>&1 )
ENVTAR=$(ls "$work"/env/*.tar.gz 2>/dev/null|head -1); [ -n "$ENVTAR" ] || fail "create-env produced no tarball ($(tail -2 $work/cenv.log))"
# CRITICAL for real (non-unittest) remote builds: the client ships THIS env to the farm.
# Without ICECC_VERSION the daemon "can't determine native environment" and falls back to <building_local>.
# The g++ env bundles the whole toolchain (incl. gcc), so a single env covers C TUs too; a 2nd same-platform
# env is ignored by icecream anyway. C TUs that emit warnings need ICECC_CARET_WORKAROUND=0 (set in replay.py).
export ICECC_VERSION="$ENVTAR"
echo "CLIENT: ICECC_VERSION=$ENVTAR"

# 2) submitter daemon: 0 local slots (never builds here) + connects to the EXTERNAL scheduler
ICECC_TEST_SOCKET="$sockdir/local" "$top/daemon/iceccd" --no-remote -m 0 -s "$SCHED" -n "$NET" -N clientsub -b "$work/envs-local" -l "$work/sub.log" -vvv & LP=$!
for i in $(seq 1 30); do grep -q "login\|Connected to scheduler\|scheduler is" "$work/sub.log" 2>/dev/null && break; sleep 1; done
grep -qi "scheduler" "$work/sub.log" 2>/dev/null || echo "CLIENT-WARN: submitter may not have reached scheduler ($(tail -2 $work/sub.log|tr '\n' '|'))"

# 3) configure project (compile_commands.json => per-TU, compile-only, no link)
BUILD="$work/cmbuild"; EXTRA="-DCMAKE_BUILD_TYPE=Release"
grep -qi "FMT_TEST\|FMT_DOC" "$PROJ/CMakeLists.txt" 2>/dev/null && EXTRA="$EXTRA -DFMT_TEST=ON -DFMT_DOC=OFF -DFMT_FUZZ=OFF"
grep -qi "rocksdb" "$PROJ/CMakeLists.txt" 2>/dev/null && EXTRA="$EXTRA -DWITH_GFLAGS=OFF -DWITH_TESTS=OFF -DWITH_BENCHMARK_TOOLS=OFF -DWITH_TOOLS=OFF -DWITH_CORE_TOOLS=OFF -DFAIL_ON_WARNINGS=OFF -DROCKSDB_BUILD_SHARED=OFF -DWITH_ALL_TESTS=OFF"
[ -n "$CMAKE_EXTRA" ] && EXTRA="$EXTRA $CMAKE_EXTRA"   # per-project override passthrough
"$CMAKE" -S "$PROJ" -B "$BUILD" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $EXTRA >"$work/cfg.log" 2>&1 || fail "cmake configure failed ($(tail -4 $work/cfg.log))"
export CC="$BUILD/compile_commands.json"; [ -f "$CC" ] || fail "no compile_commands.json"
echo "CLIENT: configured $(basename "$PROJ") -> $(python3 -c "import json;print(len(json.load(open('$CC'))),'entries')")  prefer=$PREFER sched=$SCHED"

# 4) replay: every TU REMOTE (to the farm) + local ref, byte-exact + measured
python3 /scratch/replay.py
