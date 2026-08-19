#!/usr/bin/env bash
# selector_live_history.sh — the LIVE selected-history gate.
#
# The 1F costing TSV is a differential on the always-GLOBAL baseline: every TU sends GLOBAL_S1
# and RAW is priced but never sent.  This runs the selector for real (--live-selector): each TU
# sends the winner, so when RAW wins, the Blocks that TU would have defined are NOT installed
# on F, and every later TU that needs one pays for it then.  That is the selected history, and
# the curve it produces is the thing the differential could only bound.
#
# WHAT THIS GATE DOES NOT ASSERT: that live is smaller than always-GLOBAL.  Per-TU selection is
# GREEDY, and greedy is not globally optimal here -- choosing RAW defers Block definitions
# rather than avoiding them, so a TU that looks cheaper can cost more over the rest of the
# build.  Asserting "live wins" would be assuming the result instead of measuring it.  The sign
# is REPORTED per cell, either way.
#
# What it does require, per cell, for BOTH runs:
#   1. the codec process exits 0
#   2. byte-exact=OK  -- the decoder must reconstruct from a RAW Root just as well
#   3. one TSV row per TU
#   4. sum(global_full) == the SIZE of that run's C->F file on disk
#   5. the two runs' C->F sizes differ by EXACTLY the sum of their per-TU differences
#      -- so the effect of every choice is accounted for, including the deferred cost, and a
#      saving that quietly reappears somewhere else cannot be reported as a saving
#   6. the live run declares the LIVE basis and the baseline declares the differential basis
#
# Check 5 is the one that makes this a measurement rather than a headline: at 1F on fmt the
# live run saves 29 bytes at TU 0 and pays back 7 at TU 1 and 5 at TU 21, netting 17 -- and 17
# is exactly the difference between the two files on disk.
#
# Usage:  BIN=<codec50-sink> ./selector_live_history.sh [project/profile ...]
set -Eeuo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/selector_evidence.sh"

MX=${MX:-$HOME/ictmp/ii-matrix}
BIN=${BIN:-$HERE/build/codec50-sink}
WORK=${WORK:-$(mktemp -d /tmp/livehist.XXXXXX)}
OUT=${OUT:-$WORK/results}
REPS=${REPS:-4}

CELL=""; T=""
fail() {
  echo "LIVE-HISTORY FAIL [${CELL:-<setup>}]: $*" >&2
  if [ -n "$T" ] && [ -d "$T" ]; then echo "  working directory retained: $T" >&2; fi
  exit 1
}
trap 'rc=$?; [ $rc -eq 0 ] || fail "aborted with status $rc at line $LINENO"' ERR

[ -x "$BIN" ] || fail "codec binary not executable: $BIN"
[ -d "$MX" ] || fail "ii-matrix not found: $MX"

BASE=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024
      --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3
      --stable-root-tags --literal-ondemand --literal-group-skip-zstd10 --route-s1 1)

CELLS=("$@")
[ ${#CELLS[@]} -gt 0 ] || CELLS=(re2/debian-gcc fmt/debian-gcc cereal/debian-gcc leveldb/debian-gcc spdlog/debian-gcc)

mkdir -p "$OUT"
selector_evidence_init "$OUT" "$BIN"
RESULTS=$OUT/selector-live-selected-history.tsv
printf '# LIVE selected history vs the always-GLOBAL baseline; both physical C->F streams\n' >"$RESULTS"
printf 'cell\tTUs\tbaseline_cf\tlive_cf\tdelta\tdelta_pct\tRAW_wins\tGLOBAL_wins\tblocks_sent_base\tblocks_sent_live\n' >>"$RESULTS"

run_one() { # run_one <tag> <extra flag...>
  local tag=$1; shift
  local rc=0
  "$BIN" --manifest "$T/man$REPS" "${BASE[@]}" "$@" \
         --selector-tsv "$T/$tag.tsv" --cf-sink "$T/$tag.cf" --fc-sink "$T/$tag.fc" \
         >"$T/$tag.out" 2>"$T/$tag.err" || rc=$?
  [ "$rc" -eq 0 ] || fail "$tag: codec exited $rc"
  grep -qF 'byte-exact=OK' "$T/$tag.out" || fail "$tag: byte-exact is not OK"
  local rows; rows=$(( $(wc -l <"$T/$tag.tsv") - 1 ))
  [ "$rows" = "$TU" ] || fail "$tag: selector TSV has $rows rows, expected $TU"
  local sum; sum=$(awk -F'\t' 'NR>1{g+=$12}END{printf "%.0f", g}' "$T/$tag.tsv")
  local size; size=$(stat -c %s "$T/$tag.cf")
  [ "$sum" = "$size" ] || fail "$tag: TSV global_full sums to $sum but the C->F file holds $size"
}

for cell in "${CELLS[@]}"; do
  CELL=$cell
  P=${cell%%/*}; PR=${cell##*/}
  T=$WORK/$P.$PR; rm -rf "$T"; mkdir -p "$T/ii"
  J=$MX/$P/$PR/corpus.json
  [ -f "$J" ] || fail "no corpus.json at $J"
  meta=$(python3 -c "
import json,sys
d=json.load(open(sys.argv[1]))
print(d['payload']['path'], d['payload']['sha256'], d['tu_count'])" "$J")
  read -r REL SHA N <<<"$meta"
  have=$(sha256sum "$MX/$P/$PR/$REL" | cut -d' ' -f1)
  [ "$have" = "$SHA" ] || fail "payload sha256 mismatch"
  zstd -d --long=31 -c "$MX/$P/$PR/$REL" 2>/dev/null | tar -xf - -C "$T/ii"
  find "$T/ii" -name '*.ii' | sort >"$T/man"
  M=$(wc -l <"$T/man"); [ "$M" = "$N" ] || fail "extracted $M .ii files, corpus.json says $N"
  : >"$T/man$REPS"; for ((i=0; i<REPS; ++i)); do cat "$T/man" >>"$T/man$REPS"; done
  TU=$((N * REPS))

  run_one base
  run_one live --live-selector
  grep -qF 'SELECTOR basis: differential' "$T/base.out" || fail "the baseline did not declare the differential basis"
  grep -qF 'SELECTOR basis: LIVE' "$T/live.out" || fail "the live run did not declare the LIVE basis"

  base_cf=$(stat -c %s "$T/base.cf"); live_cf=$(stat -c %s "$T/live.cf")
  # 5. every byte of the difference must be attributable to per-TU rows.  A saving that
  #    quietly reappears elsewhere is not a saving, and this is what catches it.
  rowdelta=$(paste <(awk -F'\t' 'NR>1{print $12}' "$T/base.tsv") <(awk -F'\t' 'NR>1{print $12}' "$T/live.tsv") \
             | awk '{d+=$1-$2}END{printf "%.0f", d}')
  [ "$rowdelta" = "$((base_cf - live_cf))" ] || fail \
      "per-TU differences sum to $rowdelta but the two C->F files differ by $((base_cf - live_cf))"

  rawwin=$(awk -F'\t' 'NR>1 && $13=="RAW"{n++}END{print n+0}' "$T/live.tsv")
  globwin=$(awk -F'\t' 'NR>1 && $13=="GLOBAL_S1"{n++}END{print n+0}' "$T/live.tsv")
  blk_base=$(awk -F'\t' 'NR>1{n+=$6}END{print n+0}' "$T/base.tsv")
  blk_live=$(awk -F'\t' 'NR>1{n+=$6}END{print n+0}' "$T/live.tsv")
  delta=$((base_cf - live_cf))
  printf '%s\t%s\t%s\t%s\t%+d\t%s\t%s\t%s\t%s\t%s\n' "$P" "$TU" "$base_cf" "$live_cf" "$delta" \
      "$(awk -v d="$delta" -v b="$base_cf" 'BEGIN{printf "%+.4f%%", d*100/b}')" \
      "$rawwin" "$globwin" "$blk_base" "$blk_live" >>"$RESULTS"
  selector_evidence_cell "$OUT" "$P.$PR" "$BIN ${BASE[*]} [--live-selector]" \
      "$T/base.out" "$T/base.err" "$T/base.tsv" "$T/base.cf" "$T/base.fc" \
      "$T/live.out" "$T/live.err" "$T/live.tsv" "$T/live.cf" "$T/live.fc"
  echo "OK $P/$PR: baseline $base_cf, live $live_cf ($((base_cf - live_cf)) bytes), RAW won $rawwin/$TU"
  rm -rf "$T"; T=""
done

CELL=""
echo
cat "$RESULTS"
echo
echo "all ${#CELLS[@]} cell(s): both runs byte-exact, both accountings closed, every byte of the"
echo "difference attributed to per-TU rows.  A positive delta means live sent FEWER bytes."
