#!/usr/bin/env bash
# TU112 probe census on a fixed-16 corpus: corrected P29 (56c1744, stable Root tags,
# suffix-blind) plus the bounded GRZ2 probe, to expose the representation/trajectory
# features at the decision point.  Sizes only; no rates are claimed here.
set -euo pipefail

C=$1
W=${ROOT:-$HOME/selbind/f16}/$C
MAN=$HOME/ictmp/$C/manifest.txt
II=$HOME/grouprlz/ii
P29=${P29:-$HOME/issue16-p29-prefix-state/bin/codec50-stable-root-final-l23}
GRZ=${GRZ:-$HOME/issue16-selector-v1/tools/grz2g-selector}

rm -rf "$W"; mkdir -p "$W"/{p29,grz}
TUS=$(wc -l < "$MAN")
PROBE=$(( TUS < 112 ? TUS : 112 ))
head -n "$PROBE" "$MAN" > "$W/manifest.probe.txt"
tr '\n' '\0' < "$W/manifest.probe.txt" | xargs -0 cat > "$W/probe.ii"
"$GRZ" tu "$W/manifest.probe.txt" "$W/probe.tu" > /dev/null
PROBE_RAW=$(stat -Lc %s "$W/probe.ii")

P29C=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9
      --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3)
BLIND=(); (( PROBE < TUS )) && BLIND=(--open-final-entropy)

taskset -c 16-23 "$P29" --manifest "$W/manifest.probe.txt" "${P29C[@]}" \
    --mixed-dump-prefix "$W/p29/plan" > "$W/p29/plan.out" 2> "$W/p29/plan.err"
taskset -c 16-23 "$P29" --manifest "$W/manifest.probe.txt" "${P29C[@]}" \
    --literal-group-prefix "$W/p29/plan" --literal-group-tus 112 --stable-root-tags \
    "${BLIND[@]}" --literal-group-workers 8 --literal-group-skip-zstd10 \
    --literal-group-wire "$W/p29/literal.wire" > "$W/p29/g.out" 2> "$W/p29/g.err"
taskset -c 24-31 "$GRZ" enc "$W/probe.ii" "$W/grz/probe.grz" -u "$W/probe.tu" \
    -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8 \
    --curve "$W/grz/curve.tsv" > "$W/grz/e.out" 2> "$W/grz/e.err"

g()  { grep -o "$1=[0-9]*" "$2" | head -1 | cut -d= -f2; }
{
  printf 'id\t%s\ntotal_tus\t%s\nprobe_tus\t%s\nprobe_raw\t%s\n' "$C" "$TUS" "$PROBE" "$PROBE_RAW"
  printf 'p29_probe_bytes\t%s\n' "$(g TOTAL "$W/p29/g.out")"
  printf 'grz_probe_bytes\t%s\n' "$(cut -f2 "$W/grz/e.out")"
  printf 'p29_regions\t%s\n'        "$(g regions "$W/p29/g.out")"
  printf 'p29_distinct_lines\t%s\n' "$(g distinct_lines "$W/p29/g.out")"
  printf 'p29_blocks\t%s\n'         "$(g blocks "$W/p29/g.out")"
  printf 'p29_region_occ\t%s\n'     "$(sed -n 's/.*region_occ=\([0-9]*\).*/\1/p' "$W/p29/g.err" | head -1)"
  printf 'p29_raw_literal\t%s\n'    "$(sed -n 's/.*raw_literal=\([0-9]*\).*/\1/p' "$W/p29/g.out" | head -1)"
  printf 'p29_literal_wire\t%s\n'   "$(sed -n 's/.*mixed components: control=[0-9]* literal=\([0-9]*\).*/\1/p' "$W/p29/g.out" | head -1)"
  printf 'p29_control_wire\t%s\n'   "$(sed -n 's/.*mixed components: control=\([0-9]*\).*/\1/p' "$W/p29/g.out" | head -1)"
  for op in literal publish ref marker; do
    printf 'p29_op_%s\t%s\n' "$op" "$(sed -n "s/.*ops=\[.*$op=\([0-9]*\).*/\1/p" "$W/p29/g.out" | head -1)"
  done
  awk -F'\t' 'NR==2{printf "grz_g1_tu_hi\t%s\ngrz_g1_out_bytes\t%s\ngrz_g1_add_bytes\t%s\ngrz_g1_comp_bytes\t%s\ngrz_g1_closed_by\t%s\ngrz_anchor_samples\t%s\ngrz_anchor_occupied\t%s\ngrz_anchor_usable\t%s\ngrz_anchor_collisions\t%s\ngrz_anchor_matches\t%s\n",$3,$4,$5,$6,$NF,$10,$11,$12,$13,$14}' "$W/grz/curve.tsv"
  printf 'grz_groups_in_probe\t%s\n' "$(( $(wc -l < "$W/grz/curve.tsv") - 1 ))"
} > "$W/feat.tsv"
rm -f "$W/probe.ii" "$W"/p29/plan.*.raw
echo "F16PROBE_DONE $C tus=$TUS probe=$PROBE"
