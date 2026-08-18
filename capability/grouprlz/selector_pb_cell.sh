#!/usr/bin/env bash
# One policy-B cell on a current ii-matrix verified-44 cell.
#
#   probe_tus = min(112, total_tus)   -- the selection event, never GRZ's close TU
#   P29 corrected (56c1744, --stable-root-tags); suffix-blind adds --open-final-entropy
#   identity gate must PASS before any census is usable
#   fixed total core budget: 8 cores P29 (16-23) | 8 cores GRZ (24-31), disjoint
#
# Stage clocks are read from each codec's own timers; nothing is derived by
# subtracting an inferred decode time from a wall clock.
set -uo pipefail

P=$1 PR=$2
CELL=$HOME/ictmp/ii-matrix/$P/$PR
W=${ROOT:-$HOME/selbind/pb}/$P-$PR
P29=${P29:-$HOME/selbind/p29build/codec50-56c1744}
P29SRC=${P29SRC:-$HOME/selbind/p29build/codec50.cpp}
GRZ=${GRZ:-$HOME/issue16-selector-v1/tools/grz2g-selector}
GRZSRC=${GRZSRC:-$HOME/issue16-selector-v1/tools/grz2g-selector.cpp}
IDRUN=$HOME/issue16-p29-prefix-state/src/run_p29_prefix_identity.sh
LO=$HOME/issue16-selector-v1/matrix44-20260818T0230Z/cells/$P/$PR
N="nice -n 5 ionice -c2 -n5"
T="/usr/bin/time -f %e"

[[ -f $CELL/corpus.json ]] || { echo "NOCELL $P/$PR"; exit 1; }
rm -rf "$W"; mkdir -p "$W/ex" "$W/p29" "$W/grz"

read -r PAYLOAD RAW_EXPECT TU_EXPECT < <(python3 -c "
import json,sys; d=json.load(open(sys.argv[1]))
print(d['payload']['path'], d['raw_bytes'], d['tu_count'])" "$CELL/corpus.json")

# ---------------- shared producer (measured once, reported separately) ----------
$T -o "$W/prod.extract.s" $N bash -c "zstd -d --long=31 -q -c $CELL/$PAYLOAD | tar -xf - -C $W/ex"
awk -F'\t' -v d="$W/ex/" 'NR>1{print d $2}' "$CELL/manifest.tsv" > "$W/manifest.txt"
TUS=$(wc -l < "$W/manifest.txt")
PROBE=$(( TUS < 112 ? TUS : 112 ))
head -n "$PROBE" "$W/manifest.txt" > "$W/manifest.probe.txt"
$T -o "$W/prod.concat.s" $N bash -c "tr '\n' '\0' < $W/manifest.probe.txt | xargs -0 cat > $W/probe.ii"
$T -o "$W/prod.tumap.s" $N "$GRZ" tu "$W/manifest.probe.txt" "$W/probe.tu" > /dev/null
PROBE_RAW=$(stat -Lc %s "$W/probe.ii")
[[ "$TUS" == "$TU_EXPECT" ]] || { echo "TU MISMATCH $P/$PR $TUS/$TU_EXPECT"; exit 1; }

# ---------------- identity gate (local-oracle's verified tool) ------------------
RUN_DIR="$W/id" SOURCE_MANIFEST="$W/manifest.txt" P29_BIN="$P29" P29_SOURCE="$P29SRC" \
PREFIX_TUS="$PROBE" CORES=16-31 KEEP_INPUTS=1 \
    $N bash "$IDRUN" > "$W/id.log" 2>&1
if grep -q "prefix identity PASS" "$W/id.log"; then IDENT=PASS; else IDENT=FAIL; fi
[[ "$IDENT" == PASS ]] || { echo "IDENTITY FAIL $P/$PR -- see $W/id.log"; exit 2; }

P29_COMPLETE=$(grep -o 'TOTAL=[0-9]*' "$W/id/full/grouped.stdout" | head -1 | cut -d= -f2)
P29_EXACT=$(grep -o 'byte-exact=[A-Z]*' "$W/id/full/grouped.stdout" | head -1 | cut -d= -f2)

# ---------------- fixed 8/8 core budget, concurrent bounded probes --------------
P29C=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 4 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9
      --blob-zstd-workers 2 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3)
BLIND=(); (( PROBE < TUS )) && BLIND=(--open-final-entropy)

$T -o "$W/p29/plan.s" $N taskset -c 16-23 "$P29" --manifest "$W/manifest.probe.txt" \
    "${P29C[@]}" --mixed-dump-prefix "$W/p29/plan" > "$W/p29/plan.out" 2> "$W/p29/plan.err"
cat "$W/probe.ii" > /dev/null
T0=$(date +%s.%N)
( /usr/bin/time -f "%e %U %S" -o "$W/p29/probe.t" $N taskset -c 16-23 "$P29" \
    --manifest "$W/manifest.probe.txt" "${P29C[@]}" --literal-group-prefix "$W/p29/plan" \
    --literal-group-tus 112 --stable-root-tags "${BLIND[@]}" --literal-group-workers 8 \
    --literal-group-skip-zstd10 --literal-group-wire "$W/p29/literal.wire" \
    > "$W/p29/probe.out" 2> "$W/p29/probe.err" ) &
( /usr/bin/time -f "%e %U %S" -o "$W/grz/probe.t" $N taskset -c 24-31 "$GRZ" enc \
    "$W/probe.ii" "$W/grz/probe.grz" -u "$W/probe.tu" \
    -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8 \
    --curve "$W/grz/curve.tsv" > "$W/grz/probe.out" 2> "$W/grz/probe.err" ) &
wait
MAKESPAN=$(echo "$(date +%s.%N)-$T0" | bc)

P29_PROBE=$(grep -o 'TOTAL=[0-9]*' "$W/p29/probe.out" | head -1 | cut -d= -f2)
GRZ_PROBE=$(cut -f2 "$W/grz/probe.out")
if (( GRZ_PROBE < P29_PROBE )); then SEL=GRZ2; else SEL=P29BSC; fi

{
  printf 'project\t%s\nprofile\t%s\ntotal_tus\t%s\nprobe_tus\t%s\nraw_bytes\t%s\nprobe_raw_bytes\t%s\n' \
      "$P" "$PR" "$TUS" "$PROBE" "$RAW_EXPECT" "$PROBE_RAW"
  printf 'identity\t%s\np29_complete_bytes\t%s\np29_complete_exact\t%s\n' \
      "$IDENT" "$P29_COMPLETE" "$P29_EXACT"
  printf 'p29_probe_bytes\t%s\ngrz_probe_bytes\t%s\nselected_at_probe\t%s\n' \
      "$P29_PROBE" "$GRZ_PROBE" "$SEL"
  printf 'makespan_s\t%s\np29_probe_eUS\t%s\ngrz_probe_eUS\t%s\np29_plan_s\t%s\n' \
      "$MAKESPAN" "$(cat "$W/p29/probe.t")" "$(cat "$W/grz/probe.t")" "$(cat "$W/p29/plan.s")"
  printf 'producer_extract_s\t%s\nproducer_concat_s\t%s\nproducer_tumap_s\t%s\n' \
      "$(cat "$W/prod.extract.s")" "$(cat "$W/prod.concat.s")" "$(cat "$W/prod.tumap.s")"
  printf 'p29_intern_s\t%s\n' "$(sed -n 's/^loaded+interned \([0-9.]*\)s.*/\1/p' "$W/p29/probe.err")"
  printf 'p29_plan_s1_s\t%s\n' "$(sed -n 's/^S1 LZ: \([0-9.]*\)s.*/\1/p' "$W/p29/probe.err")"
  printf 'p29_literal_entropy_s\t%s\n' "$(sed -n 's/.*literal groups:.*encode=\([0-9.]*\)s.*/\1/p' "$W/p29/probe.err")"
  printf 'grz_match_s\t%s\n' "$(sed -n 's/.*match=\([0-9.]*\) .*/\1/p' "$W/grz/probe.err")"
  printf 'grz_entropy_s\t%s\n' "$(sed -n 's/.*entropy=\([0-9.]*\) .*/\1/p' "$W/grz/probe.err")"
  printf 'grz_total_s\t%s\n' "$(sed -n 's/.*total=\([0-9.]*\) .*/\1/p' "$W/grz/probe.err")"
  awk -F'\t' 'NR==2{printf "grz_g1_close_tu\t%s\ngrz_g1_closed_by\t%s\ngrz_g1_add_bytes\t%s\ngrz_anchor_samples\t%s\ngrz_anchor_matches\t%s\n",$3,$NF,$5,$10,$14}' "$W/grz/curve.tsv"
  printf 'p29_interner\tresearch\n'
  printf 'p29_src_sha\t%s\np29_bin_sha\t%s\n' "$(sha256sum "$P29SRC"|cut -d' ' -f1)" "$(sha256sum "$P29"|cut -d' ' -f1)"
  printf 'grz_src_sha\t%s\ngrz_bin_sha\t%s\n' "$(sha256sum "$GRZSRC"|cut -d' ' -f1)" "$(sha256sum "$GRZ"|cut -d' ' -f1)"
  printf 'zstd\t%s\n' "$(zstd --version 2>&1 | grep -oP 'v[0-9.]+' | head -1)"
  if [[ -f $LO/summary.stdout ]]; then
    python3 -c "
import json;d=json.load(open('$LO/summary.stdout'))
g=d['grz'];w=d['whole_zstd']
print('lo_grz_complete_bytes\t%d'%g['wire_bytes']); print('lo_grz_exact\t%s'%g['exact'])
print('lo_grz_f_bps\t%d'%g['f_decode_bps']); print('lo_grz_wire_sha\t%s'%g['wire_sha256'])
print('lo_z19_long\t%d'%w['z19_long_bytes']); print('lo_z6_long\t%d'%w['z6_long_bytes'])"
  else
    printf 'lo_grz_complete_bytes\tNA\n'
  fi
} > "$W/row.tsv"

rm -rf "$W/ex" "$W/probe.ii" "$W/id/input/cell.ii" "$W"/p29/plan.*.raw "$W"/id/full/plan.*.raw "$W"/id/prefix/plan.*.raw
echo "PBCELL_DONE $P/$PR tus=$TUS probe=$PROBE identity=$IDENT sel@probe=$SEL"
