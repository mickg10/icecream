#!/usr/bin/env bash
# One selector-binding cell: measure GRZ2 and P29+BSC on one fixed-16 corpus.
#
# Both codecs are measured on a WARM page cache so that neither is charged for NVMe
# read of the .ii bytes (the real C-side already holds the preprocessor output in RAM).
#
# Thread configurations measured per codec:
#   native = the codec's own frozen policy (GRZ2 enc -j 8 / P29 16 literal workers, 8 blob)
#   1T     = one thread everywhere on the C side
# Decode is always single-thread for both.
set -uo pipefail

C=$1                      # corpus id, e.g. corpus2
NAME=${2:-$C}
REPS=${REPS:-3}
CORES=${CORES:-0-15}
ROOT=${ROOT:-$HOME/selbind}
GRZ=${GRZ:-$HOME/grouprlz/grz2g}
P29=${P29:-/tmp/issue16-p29-bsc-integration-20260817/codec50-bsc-znver3}
II=$HOME/grouprlz/ii
MAN=$HOME/ictmp/$C/manifest.txt
R=$ROOT/runs/$C
NICE="nice -n 5 ionice -c2 -n5"
T="/usr/bin/time -f %e"

mkdir -p "$R"
rm -f "$R"/*.wiresha
[[ -x $GRZ && -x $P29 && -f $MAN && -f $II/$C.ii && -f $II/$C.tu ]] || { echo "MISSING inputs for $C" >&2; exit 1; }

RAW=$(stat -Lc %s "$II/$C.ii")

# --- warm page cache for both input copies ------------------------------------------
cat "$II/$C.ii" > /dev/null
tr '\n' '\0' < "$MAN" | xargs -0 cat > /dev/null
cat "$II/$C.ii" > /dev/null
$T -o "$R/read.warm.s" bash -c "tr '\n' '\0' < $MAN | xargs -0 cat > /dev/null"

GRZ_POLICY=(-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024)
P29_COMMON=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs
            --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9
            --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3)

# ===================================================================== GRZ2
grz_enc() {   # $1=threads  $2=tag
  local j=$1 tag=$2 rep
  for rep in $(seq 1 "$REPS"); do
    $T -o "$R/grz.$tag.enc.$rep.wall" $NICE taskset -c "$CORES" \
        "$GRZ" enc "$II/$C.ii" "$R/$tag.grz" -u "$II/$C.tu" \
        "${GRZ_POLICY[@]}" -j "$j" > "$R/grz.$tag.enc.$rep.out" 2> "$R/grz.$tag.enc.$rep.err" || return 1
    sha256sum "$R/$tag.grz" | awk '{print $1}' >> "$R/grz.$tag.wiresha"
  done
}
grz_enc 8 j8 || echo "GRZ ENC j8 FAILED" >&2
grz_enc 1 j1 || echo "GRZ ENC j1 FAILED" >&2

for rep in $(seq 1 "$REPS"); do
  $T -o "$R/grz.dec.$rep.wall" $NICE taskset -c "$CORES" \
      "$GRZ" dec "$R/j8.grz" /dev/null -j 1 \
      > "$R/grz.dec.$rep.out" 2> "$R/grz.dec.$rep.err"
done
$NICE taskset -c "$CORES" "$GRZ" dec "$R/j8.grz" "$R/replay.ii" -j 1 \
    > "$R/grz.dec.exact.out" 2> "$R/grz.dec.exact.err"
if cmp -s "$II/$C.ii" "$R/replay.ii"; then echo YES > "$R/grz.exact"; else echo NO > "$R/grz.exact"; fi
rm -f "$R/replay.ii"

# ===================================================================== P29 + BSC
# Plan pass: dumps the literal/lengths material the BSC group encoder consumes.
# It is a harness artifact (a production encoder forms this in-process), so it is
# NOT charged to the C rate; the grouped pass below repeats the whole P29 encode.
$NICE taskset -c "$CORES" "$P29" --manifest "$MAN" "${P29_COMMON[@]}" \
    --blob-threads 8 --blob-zstd-workers 4 --mixed-dump-prefix "$R/plan" \
    > "$R/p29.plan.out" 2> "$R/p29.plan.err"

p29_run() {   # $1=tag  $2=blob-threads  $3=blob-zstd-workers  $4=literal-group-workers
  local tag=$1 bt=$2 bw=$3 lw=$4 rep
  for rep in $(seq 1 "$REPS"); do
    $T -o "$R/p29.$tag.$rep.wall" $NICE taskset -c "$CORES" \
        "$P29" --manifest "$MAN" "${P29_COMMON[@]}" \
        --blob-threads "$bt" --blob-zstd-workers "$bw" \
        --literal-group-prefix "$R/plan" --literal-group-tus 112 \
        --literal-group-workers "$lw" --literal-group-skip-zstd10 \
        --literal-group-wire "$R/p29.$tag.literal.wire" \
        > "$R/p29.$tag.$rep.out" 2> "$R/p29.$tag.$rep.err"
  done
}
p29_run native 8 4 16
p29_run 1T 1 1 1

rm -f "$R"/plan.*.raw "$R"/j1.grz
echo "CELL_DONE $C raw=$RAW"
