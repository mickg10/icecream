#!/usr/bin/env bash
# selector_live_history.sh — the LIVE selected-history gate.
#
# The 1F costing TSV is a differential on the always-ROUTE_S1 baseline: every TU sends ROUTE_S1
# and RAW is priced but never sent.  This runs the selector for real (--live-selector): each TU
# sends the winner, so when RAW wins, the Blocks that TU would have defined are NOT installed
# on F, and every later TU that needs one pays for it then.  That is the selected history, and
# the curve it produces is the thing the differential could only bound.
#
# WHAT THIS GATE DOES NOT ASSERT: that live is smaller than always-ROUTE_S1.  Per-TU selection
# is GREEDY, and greedy is not globally optimal here -- choosing RAW defers Block definitions
# rather than avoiding them, so a TU that looks cheaper can cost more over the rest of the
# build.  Asserting "live wins" would be assuming the result instead of measuring it.  The sign
# is REPORTED per cell, either way.
#
# A CHECK I REMOVED, because it could not fail.  An earlier version required
#     sum(base col - live col) == size(base.cf) - size(live.cf)
# and described it as the check that made this a measurement.  It is the DIFFERENCE of the two
# per-run equalities already required below, so it follows by subtraction and can never fail
# independently -- the same telescoping shape removed from the per-TU costing gate earlier.
# The baseline-vs-live size difference is kept as a MEASUREMENT; it proves nothing on its own.
#
# The discriminating work is in the codec, per TU, and is mutation-tested there: the RAW and
# ROUTE_S1 candidate costs are fixed BEFORE selection and never rewritten; the frames actually
# emitted (measured from the sink's byte offsets) must equal the SELECTED candidate's
# separately scratch-compressed cost; `common` comes from the measured physical delta; both
# candidates' full costs are reconstructed from it; and the transaction must equal the CHOSEN
# candidate's reconstruction.  Mutating the winner, the Root attribution, the BlockDef
# attribution, or an unselected candidate field each fires its own targeted check.
#
# What this launcher requires, per cell, for BOTH runs:
#   1. the codec process exits 0
#   2. byte-exact=OK  -- the receiver must reconstruct from a RAW Root just as well
#   3. one TSV row per TU
#   4. sum(actual_delta) == the SIZE of that run's C->F file on disk
#   5. every row's chosen candidate full cost == its actual delta, re-derived here from the
#      TSV rather than trusted from the codec's own check
#   6. the live run declares the LIVE basis and the baseline the differential basis, and both
#      declare the producer boundary
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
WANT_HDR=$'tu\traw_root_bytes\traw_root_frame\troute_root_bytes\troute_root_frame\troute_candidate_blockdefs\troute_blockdef_bytes\troute_blockdef_frame\temitted_root_frame\temitted_blockdefs\temitted_blockdef_frame\tactual_delta\tcommon\traw_full\troute_full\twinner\ttie\traw_cheaper'

CELLS=("$@")
[ ${#CELLS[@]} -gt 0 ] || CELLS=(re2/debian-gcc fmt/debian-gcc cereal/debian-gcc leveldb/debian-gcc spdlog/debian-gcc)

mkdir -p "$OUT"
selector_evidence_init "$OUT" "$BIN"
RESULTS=$OUT/selector-live-selected-history.tsv
printf '# LIVE selected history vs the always-ROUTE_S1 baseline; both physical C->F streams\n' >"$RESULTS"
printf 'cell\tTUs\tbaseline_cf\tlive_cf\tdelta\tdelta_pct\tRAW_wins\tROUTE_wins\tties\tlive_route_counterfactual\troute_candidate_blockdefs_live\temitted_blockdefs_base\temitted_blockdefs_live\n' >>"$RESULTS"

run_one() { # run_one <tag> <extra flag...>
  local tag=$1; shift
  local rc=0
  "$BIN" --manifest "$T/man$REPS" "${BASE[@]}" "$@" \
         --selector-tsv "$T/$tag.tsv" --cf-sink "$T/$tag.cf" --fc-sink "$T/$tag.fc" \
         >"$T/$tag.out" 2>"$T/$tag.err" || rc=$?
  [ "$rc" -eq 0 ] || fail "$tag: codec exited $rc"
  grep -qF 'byte-exact=OK' "$T/$tag.out" || fail "$tag: byte-exact is not OK"
  local hdr; hdr=$(head -1 "$T/$tag.tsv")
  [ "$hdr" = "$WANT_HDR" ] || fail "$tag: unexpected selector TSV header"
  local rows; rows=$(( $(wc -l <"$T/$tag.tsv") - 1 ))
  [ "$rows" = "$TU" ] || fail "$tag: selector TSV has $rows rows, expected $TU"
  local sum; sum=$(awk -F'\t' 'NR>1{g+=$12}END{printf "%.0f", g}' "$T/$tag.tsv")
  local size; size=$(stat -c %s "$T/$tag.cf")
  [ "$sum" = "$size" ] || fail "$tag: TSV actual_delta sums to $sum but the C->F file holds $size"
  # 5. re-derived here rather than trusted: for every row the CHOSEN candidate's full cost
  #    must equal the actual transaction.  raw_full and route_full are reconstructed from a
  #    MEASURED common plus SEPARATELY costed candidate frames, so the unchosen one is a real
  #    counterfactual and this comparison has something to disagree with.
  local bad; bad=$(awk -F'\t' 'NR>1{
        chosen = ($16=="RAW") ? $14 : $15
        if (chosen != $12) { print $1; n++ }
      } END { }' "$T/$tag.tsv" | head -3 | tr '\n' ' ')
  [ -z "$bad" ] || fail "$tag: the chosen candidate does not reconstruct the actual transaction at TU(s): $bad"
  grep -qF 'SELECTOR boundary:' "$T/$tag.out" || fail "$tag: the producer boundary is not declared"
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
  # The size difference is a MEASUREMENT and is reported as one.  It is deliberately not
  # dressed up as a proof of per-TU attribution: that lives in the codec's own checks, which
  # are mutation-tested against the winner, both frame attributions, and an unselected field.
  rawwin=$(awk -F'\t' 'NR>1 && $16=="RAW"{n++}END{print n+0}' "$T/live.tsv")
  globwin=$(awk -F'\t' 'NR>1 && $16=="ROUTE_S1"{n++}END{print n+0}' "$T/live.tsv")
  ties=$(awk -F'\t' 'NR>1 && $17==1{n++}END{print n+0}' "$T/live.tsv")
  route_candidate_defs_live=$(awk -F'\t' 'NR>1{n+=$6}END{print n+0}' "$T/live.tsv")
  blk_base=$(awk -F'\t' 'NR>1{n+=$10}END{print n+0}' "$T/base.tsv")
  blk_live=$(awk -F'\t' 'NR>1{n+=$10}END{print n+0}' "$T/live.tsv")
  # The counterfactual the separated records make available: what the live run WOULD have
  # cost had it sent ROUTE_S1 for every TU against its own selected history.
  live_route_cf=$(awk -F'\t' 'NR>1{g+=$15}END{printf "%.0f", g}' "$T/live.tsv")
  delta=$((base_cf - live_cf))
  printf '%s\t%s\t%s\t%s\t%+d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$P" "$TU" "$base_cf" "$live_cf" "$delta" \
      "$(awk -v d="$delta" -v b="$base_cf" 'BEGIN{printf "%+.4f%%", d*100/b}')" \
      "$rawwin" "$globwin" "$ties" "$live_route_cf" "$route_candidate_defs_live" "$blk_base" "$blk_live" >>"$RESULTS"
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
echo "all ${#CELLS[@]} cell(s): both runs byte-exact, both accountings closed against the file on"
echo "disk, and every row's chosen candidate reconstructs its actual transaction."
echo "A positive delta means live sent FEWER bytes than always-ROUTE_S1."
