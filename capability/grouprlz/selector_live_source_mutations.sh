#!/usr/bin/env bash
# selector_live_source_mutations.sh — prove the live selector's four attribution checks
# reject source-level faults, not merely post-processed evidence edits.
#
# Each case copies codec50-sink.cpp, changes exactly one named expression, builds that source,
# and requires the resulting executable to stop at the intended check:
#   winner       selected representation is not the retained-cost argmin
#   root         selected Root expectation differs from the emitted Root frame
#   blockdef     selected BlockDef expectation differs from the emitted BlockDef frame
#   unselected   the candidate not selected for this TU is changed after selection
#
# An unmodified control must complete and reconstruct exactly.  The first few paths from the
# supplied manifest are sufficient because every mutation is deterministic on the first TU;
# limiting the fixture keeps this source-compilation gate cheap enough for every acceptance run.
#
# Usage: BIN=<current codec50-sink> ./selector_live_source_mutations.sh <manifest>
#        WORK=<retained scratch> MUTATION_TUS=<number of manifest entries; default 4>
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MAN=${1:?usage: selector_live_source_mutations.sh <manifest>}
BIN=${BIN:-$HERE/build/codec50-sink}
WORK=${WORK:-$(mktemp -d /tmp/selector-source-mut.XXXXXX)}
MUTATION_TUS=${MUTATION_TUS:-4}
LIBBSC_DIR=${LIBBSC_DIR:-$HOME/libbsc}
LIBBSC_A=${LIBBSC_A:-$HOME/grouprlz/libbsc.a}
LIBZSTD_A=${LIBZSTD_A:-/usr/lib/x86_64-linux-gnu/libzstd.a}

fail() { echo "SELECTOR SOURCE MUTATION FAIL: $*" >&2; exit 1; }
trap 'rc=$?; [ "$rc" -eq 0 ] || echo "  evidence retained: $WORK" >&2' EXIT

[ -x "$BIN" ] || fail "codec binary not executable: $BIN"
[ -s "$MAN" ] || fail "manifest is empty or missing: $MAN"
for f in "$LIBBSC_DIR/libbsc/libbsc.h" "$LIBBSC_A" "$LIBZSTD_A"; do
  [ -e "$f" ] || fail "missing build dependency: $f"
done
mkdir -p "$WORK"
sed -n "1,${MUTATION_TUS}p" "$MAN" >"$WORK/manifest"
[ -s "$WORK/manifest" ] || fail "could not select a fixture from $MAN"

BASE=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024
      --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3
      --stable-root-tags --literal-ondemand --literal-group-skip-zstd10 --route-s1 1
      --live-selector)
CXXFLAGS=(-O2 -std=c++17 -fopenmp -Wformat=2 -Werror=format -DWITH_BSC_GROUPS
          -I"$HERE" -I"$LIBBSC_DIR/libbsc")

run_codec() { # run_codec <tag> <executable>
  local tag=$1 exe=$2 rc=0
  "$exe" --manifest "$WORK/manifest" "${BASE[@]}" \
      --selector-tsv "$WORK/$tag.tsv" --cf-sink "$WORK/$tag.cf" --fc-sink "$WORK/$tag.fc" \
      >"$WORK/$tag.out" 2>"$WORK/$tag.err" || rc=$?
  return "$rc"
}

# The control prevents a build/environment failure from masquerading as four caught faults.
if ! run_codec control "$BIN"; then
  fail "unmodified control failed (see control.out/control.err)"
fi
grep -qF 'byte-exact=OK' "$WORK/control.out" || fail "unmodified control was not byte-exact"
echo "control: unmodified source accepted"

mutate() { # mutate <mode> <source>
  local mode=$1 src=$2 needle count
  case $mode in
    winner)
      needle='const bool shouldRaw = snapRawFrame < snapRouteRootFrame+snapRouteDefFrame;'
      count=$(grep -Fc "$needle" "$src"); [ "$count" -eq 1 ] || fail "$mode mutation anchor count is $count, expected 1"
      sed -i 's/const bool shouldRaw = snapRawFrame < snapRouteRootFrame+snapRouteDefFrame;/const bool shouldRaw = !r.chosenRaw;/' "$src"
      ;;
    root)
      needle='selExpectRootFrame = chooseRaw?rawRootFrameC:routeRootFrameC;'
      count=$(grep -Fc "$needle" "$src"); [ "$count" -eq 1 ] || fail "$mode mutation anchor count is $count, expected 1"
      sed -i 's/selExpectRootFrame = chooseRaw?rawRootFrameC:routeRootFrameC;/selExpectRootFrame = (chooseRaw?rawRootFrameC:routeRootFrameC)+1;/' "$src"
      ;;
    blockdef)
      needle='selExpectDefFrame  = chooseRaw?0:routeDefFrameC;'
      count=$(grep -Fc "$needle" "$src"); [ "$count" -eq 1 ] || fail "$mode mutation anchor count is $count, expected 1"
      sed -i 's/selExpectDefFrame  = chooseRaw?0:routeDefFrameC;/selExpectDefFrame  = (chooseRaw?0:routeDefFrameC)+1;/' "$src"
      ;;
    unselected)
      needle='0,0,0,0,0,0,0,chooseRaw,tie});'
      count=$(grep -Fc "$needle" "$src"); [ "$count" -eq 1 ] || fail "$mode mutation anchor count is $count, expected 1"
      sed -i '/0,0,0,0,0,0,0,chooseRaw,tie});/a\            if(chooseRaw) selRows.back().routeDefFrame ^= 1; else selRows.back().rawRootFrame ^= 1;' "$src"
      ;;
    *) fail "unknown mutation: $mode" ;;
  esac
  cmp -s "$HERE/codec50-sink.cpp" "$src" && fail "$mode changed no source"
  return 0
}

declare -A EXPECT=(
  [winner]='does not minimise the retained candidate costs'
  [root]='emitted Root frame'
  [blockdef]='emitted BlockDef frame'
  [unselected]='a candidate cost was rewritten by selection'
)

for mode in winner root blockdef unselected; do
  src=$WORK/codec50-sink.$mode.cpp
  exe=$WORK/codec50-sink.$mode
  cp "$HERE/codec50-sink.cpp" "$src"
  mutate "$mode" "$src"
  g++ "${CXXFLAGS[@]}" "$src" -o "$exe" "$LIBBSC_A" "$LIBZSTD_A" -lz -lpthread \
      >"$WORK/build.$mode.out" 2>"$WORK/build.$mode.err" || fail "$mode source did not build"
  rc=0; run_codec "$mode" "$exe" || rc=$?
  [ "$rc" -ne 0 ] || fail "$mode mutation was accepted"
  grep -qF "${EXPECT[$mode]}" "$WORK/$mode.err" || \
      fail "$mode stopped, but not at its intended check"
  printf 'caught %-10s exit=%d  %s\n' "$mode" "$rc" "${EXPECT[$mode]}"
done

echo "all 4 source mutations reached their intended checks, and the unmodified control passed"
