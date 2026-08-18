#!/usr/bin/env bash
# One ii-matrix (project, profile) cell: measure GRZ2 and P29+BSC end to end.
#
# The cell archive is expanded, the TUs concatenated in manifest.tsv ordinal order,
# and the whole-cell zstd-19 --long=31 reference taken from that same real file.
# Both codecs are measured on a warm page cache at one thread and at their frozen
# parallel policy; decode is always single thread. Expanded inputs are deleted after.
set -uo pipefail

P=$1 PR=$2
REPS=${REPS:-3}
CORES=${CORES:-0-15}
CELL=$HOME/ictmp/ii-matrix/$P/$PR
W=${W:-$HOME/selbind/dm}/$P-$PR
GRZ=${GRZ:-$HOME/grouprlz/grz2g}
P29=${P29:-/tmp/issue16-p29-bsc-integration-20260817/codec50-bsc-znver3}
NICE="nice -n 5 ionice -c2 -n5"
T="/usr/bin/time -f %e"

[[ -f $CELL/corpus.json ]] || { echo "NOCELL $P/$PR (no corpus.json)" >&2; exit 1; }
rm -rf "$W"; mkdir -p "$W"

read -r PAYLOAD RAW_EXPECT TU_EXPECT < <(python3 -c "
import json,sys; d=json.load(open(sys.argv[1]))
print(d['payload']['path'], d['raw_bytes'], d['tu_count'])" "$CELL/corpus.json")

$NICE zstd -d --long=31 -q -c "$CELL/$PAYLOAD" | tar -xf - -C "$W" || { echo "UNTAR FAIL $P/$PR" >&2; exit 1; }
# Newer cells ship ii/ only and keep manifest.tsv beside the archive; older ones
# (the <project>-<profile>.ii.tar.zst generation) carry it inside the tar.
MAN=$CELL/manifest.tsv
[[ -f $MAN ]] || MAN=$W/manifest.tsv
[[ -f $MAN ]] || { echo "NOMANIFEST $P/$PR" >&2; exit 1; }

awk -F'\t' -v d="$W/" 'NR>1{print d $2}' "$MAN" > "$W/manifest.txt"
tr '\n' '\0' < "$W/manifest.txt" | xargs -0 cat > "$W/cell.ii"
RAW=$(stat -Lc %s "$W/cell.ii")
TUS=$(wc -l < "$W/manifest.txt")
[[ "$RAW" == "$RAW_EXPECT" && "$TUS" == "$TU_EXPECT" ]] || {
    echo "MISMATCH $P/$PR raw=$RAW/$RAW_EXPECT tus=$TUS/$TU_EXPECT" >&2; exit 1; }
"$GRZ" tu "$W/manifest.txt" "$W/cell.tu" > /dev/null

# whole-cell reference, taken with stat -Lc on the real concatenated file
$NICE zstd -q -f -19 --long=31 -T0 -o "$W/cell.z19.zst" "$W/cell.ii"
Z19=$(stat -Lc %s "$W/cell.z19.zst")
printf '%s\t%s\t%s\t%s\t%s\n' "$P" "$PR" "$TUS" "$RAW" "$Z19" > "$W/cell.meta"

cat "$W/cell.ii" > /dev/null
$T -o "$W/read.warm.s" bash -c "cat $W/cell.ii > /dev/null"

GRZ_POLICY=(-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024)
P29_COMMON=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
            --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9
            --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3)

for tag in j1 j8; do
  j=${tag#j}
  for rep in $(seq 1 "$REPS"); do
    $T -o "$W/grz.$tag.enc.$rep.wall" $NICE taskset -c "$CORES" \
        "$GRZ" enc "$W/cell.ii" "$W/$tag.grz" -u "$W/cell.tu" "${GRZ_POLICY[@]}" -j "$j" \
        > "$W/grz.$tag.enc.$rep.out" 2> "$W/grz.$tag.enc.$rep.err"
  done
  sha256sum "$W/$tag.grz" | awk '{print $1}' >> "$W/grz.wiresha"
done
for rep in $(seq 1 "$REPS"); do
  $T -o "$W/grz.dec.$rep.wall" $NICE taskset -c "$CORES" \
      "$GRZ" dec "$W/j8.grz" /dev/null -j 1 > /dev/null 2> "$W/grz.dec.$rep.err"
done
$NICE taskset -c "$CORES" "$GRZ" dec "$W/j8.grz" "$W/replay.ii" -j 1 > /dev/null 2>&1
if cmp -s "$W/cell.ii" "$W/replay.ii"; then echo YES > "$W/grz.exact"; else echo NO > "$W/grz.exact"; fi
rm -f "$W/replay.ii"

$NICE taskset -c "$CORES" "$P29" --manifest "$W/manifest.txt" "${P29_COMMON[@]}" \
    --blob-threads 8 --blob-zstd-workers 4 --mixed-dump-prefix "$W/plan" \
    > "$W/p29.plan.out" 2> "$W/p29.plan.err"
p29_run() {
  local tag=$1 bt=$2 bw=$3 lw=$4 rep
  for rep in $(seq 1 "$REPS"); do
    $T -o "$W/p29.$tag.$rep.wall" $NICE taskset -c "$CORES" \
        "$P29" --manifest "$W/manifest.txt" "${P29_COMMON[@]}" \
        --blob-threads "$bt" --blob-zstd-workers "$bw" \
        --literal-group-prefix "$W/plan" --literal-group-tus 112 \
        --literal-group-workers "$lw" --literal-group-skip-zstd10 \
        --literal-group-wire "$W/p29.$tag.literal.wire" \
        > "$W/p29.$tag.$rep.out" 2> "$W/p29.$tag.$rep.err"
  done
}
p29_run native 8 4 16
p29_run 1T 1 1 1

rm -rf "$W/ii" "$W/cell.ii" "$W/cell.z19.zst" "$W"/plan.*.raw "$W"/j1.grz "$W"/environment
echo "DMCELL_DONE $P/$PR tus=$TUS raw=$RAW z19=$Z19"
