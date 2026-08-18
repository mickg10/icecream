#!/usr/bin/env bash
# THE decisive docker measurement, ready to run the moment P29 has a fast interner.
#
# Policy A: run BOTH codecs to completion concurrently on a fixed total core budget
# (disjoint 8 P29 / 8 GRZ), take the per-cell minimum wire, and report per cell:
#   - P29's own complete-encode rate, and whether it clears the >= 1 GB/s C gate
#   - policy A's rate, whose makespan is max(P29, GRZ) -- it clears iff P29 does
#   - the selected (minimum) bytes, for the aggregate x and / z19
#
# No classifier is involved: policy A yields the per-cell oracle by construction, which
# is why it is the measurement that settles the docker lane. To run it against the fast
# interner, set P29= to that build; nothing else changes.
set -euo pipefail

P=${1:?project}; PR=${2:?profile}
W=${ROOT:-$HOME/selbind/polA}/$P-$PR
CELL=$HOME/ictmp/ii-matrix/$P/$PR
P29=${P29:-$HOME/issue16-p29-prefix-state/bin/codec50-stable-root-final-l23}
GRZ=${GRZ:-$HOME/issue16-selector-v1/tools/grz2g-selector}
P29C=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
      --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9
      --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3)
GRZP=(-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8)

rm -rf "$W"; mkdir -p "$W"/{ex,p29,grz}
read -r PAY RAW TU < <(python3 -c "
import json,sys; d=json.load(open(sys.argv[1]))
print(d['payload']['path'], d['raw_bytes'], d['tu_count'])" "$CELL/corpus.json")
zstd -d --long=31 -q -c "$CELL/$PAY" | tar -xf - -C "$W/ex"
awk -F'\t' -v d="$W/ex/" 'NR>1{print d $2}' "$CELL/manifest.tsv" > "$W/man"
[[ $(wc -l < "$W/man") -eq $TU ]] || { echo "TU mismatch for $P/$PR" >&2; exit 5; }
tr '\n' '\0' < "$W/man" | xargs -0 cat > "$W/cell.ii"
"$GRZ" tu "$W/man" "$W/cell.tu" > /dev/null
cat "$W/cell.ii" > /dev/null

# The two candidate paths are written to disk with paths already expanded: a
# `bash -c "$(declare -f f); f"` subshell would not inherit $W and would fail silently
# into the wrong directory.
cat > "$W/p29.sh" <<SH
#!/usr/bin/env bash
set -euo pipefail
taskset -c 16-23 "$P29" --manifest "$W/man" ${P29C[*]} --mixed-dump-prefix "$W/p29/pl" > "$W/p29/pl.out" 2> "$W/p29/pl.err"
taskset -c 16-23 "$P29" --manifest "$W/man" ${P29C[*]} --literal-group-prefix "$W/p29/pl" --literal-group-tus 112 --stable-root-tags --literal-group-workers 8 --literal-group-skip-zstd10 --literal-group-wire "$W/p29/lit" > "$W/p29/g.out" 2> "$W/p29/g.err"
SH
cat > "$W/grz.sh" <<SH
#!/usr/bin/env bash
set -euo pipefail
taskset -c 24-31 "$GRZ" enc "$W/cell.ii" "$W/grz/cell.grz" -u "$W/cell.tu" ${GRZP[*]} > "$W/grz/e.out" 2> "$W/grz/e.err"
SH
chmod +x "$W/p29.sh" "$W/grz.sh"

T0=$(date +%s.%N)
/usr/bin/time -f '%e %U %S %M' -o "$W/p29.res" "$W/p29.sh" & A=$!
/usr/bin/time -f '%e %U %S %M' -o "$W/grz.res" "$W/grz.sh" & B=$!
SA=0; SB=0; wait $A || SA=$?; wait $B || SB=$?
MK=$(echo "$(date +%s.%N)-$T0" | bc)
(( SA == 0 && SB == 0 )) || { echo "candidate failed p29=$SA grz=$SB" >&2; exit 6; }

PB=$(grep -o 'TOTAL=[0-9]*' "$W/p29/g.out" | head -1 | cut -d= -f2)
GB=$(cut -f2 "$W/grz/e.out")
PE=$(awk '{print $1}' "$W/p29.res"); GE=$(awk '{print $1}' "$W/grz.res")
taskset -c 24-31 "$GRZ" dec "$W/grz/cell.grz" "$W/grz/replay" -j 1 > /dev/null 2>&1
if cmp -s "$W/cell.ii" "$W/grz/replay"; then X=YES; else X=NO; fi
rm -f "$W/grz/replay" "$W/cell.ii" "$W"/p29/pl.*.raw
find "$W/ex" -mindepth 1 -delete

awk -v p="$P" -v pr="$PR" -v raw="$RAW" -v pb="$PB" -v gb="$GB" \
    -v pe="$PE" -v ge="$GE" -v mk="$MK" -v x="$X" 'BEGIN {
  m = (pb < gb) ? pb : gb; w = (pb < gb) ? "P29BSC" : "GRZ2";
  printf "%s\t%s\t%d\t%d\t%d\t%d\t%s\t%.3f\t%.3f\t%.3f\t%s\t%s\t%s\n",
    p, pr, raw, pb, gb, m, w, raw/1e9/pe, raw/1e9/ge, raw/1e9/mk,
    (raw/1e9/pe >= 1 ? "P29_LEGAL" : "P29_SLOW"),
    (raw/1e9/mk >= 1 ? "POLICY_A_LEGAL" : "POLICY_A_SLOW"), x }'
