#!/usr/bin/env bash
# Can GRZ2's encode be pushed over the 1 GB/s deadline on the corpora where it is
# smaller than P29+BSC but rate-illegal?  Only the encode-side levers are touched;
# every variant is decoded and compared byte-for-byte against the input.
set -uo pipefail

GRZ=$HOME/grouprlz/grz2g
II=$HOME/grouprlz/ii
R=$HOME/selbind/tune
REPS=3
NICE="nice -n 5 ionice -c2 -n5"
mkdir -p "$R"

BASE=(-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 --gtu 112 --graw 512 --gadd 128 --hist 1024)

printf 'corpus\tvariant\traw\tout\tC_gbps_min\tC_gbps_med\tF_gbps\texact\twire_sha256\n' \
    > "$R/tune.tsv"

run() {   # $1=corpus  $2=variant label  $3.. = extra flags
  local c=$1 label=$2; shift 2
  local raw rates=() rep t f x
  raw=$(stat -Lc %s "$II/$c.ii")
  cat "$II/$c.ii" > /dev/null
  for rep in $(seq 1 $REPS); do
    /usr/bin/time -f %e -o "$R/$c.$label.$rep.wall" $NICE taskset -c 0-15 \
        "$GRZ" enc "$II/$c.ii" "$R/$c.$label.grz" -u "$II/$c.tu" "${BASE[@]}" "$@" \
        > "$R/$c.$label.$rep.out" 2> "$R/$c.$label.$rep.err" || { echo "ENC FAIL $c $label"; return; }
    rates+=("$(awk -v r="$raw" '{printf "%.4f", r/1e9/$1}' "$R/$c.$label.$rep.wall")")
  done
  $NICE taskset -c 0-15 "$GRZ" dec "$R/$c.$label.grz" /dev/null -j 1 \
      > "$R/$c.$label.dec.out" 2> "$R/$c.$label.dec.err"
  f=$(sed -n 's/.* F=\([0-9]*\) B\/s.*/\1/p' "$R/$c.$label.dec.err" | awk '{printf "%.4f", $1/1e9}')
  $NICE taskset -c 0-15 "$GRZ" dec "$R/$c.$label.grz" "$R/replay.ii" -j 1 \
      > /dev/null 2>&1
  if cmp -s "$II/$c.ii" "$R/replay.ii"; then x=YES; else x=NO; fi
  rm -f "$R/replay.ii"
  t=$(awk '{print $2}' "$R/$c.$label.1.out")
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$c" "$label" "$raw" "$t" \
      "$(printf '%s\n' "${rates[@]}" | sort -g | head -1)" \
      "$(printf '%s\n' "${rates[@]}" | sort -g | sed -n 2p)" \
      "$f" "$x" "$(sha256sum "$R/$c.$label.grz" | awk '{print $1}')" | tee -a "$R/tune.tsv"
  rm -f "$R/$c.$label.grz"
}

for c in "$@"; do
  run "$c" frozen-j8       -b 8 -j 8
  run "$c" j16             -b 8 -j 16
  run "$c" j16-b4          -b 4 -j 16
  run "$c" j16-b2          -b 2 -j 16
  run "$c" j32-b2          -b 2 -j 32
  run "$c" j16-b2-tokbsc   -b 2 -j 16 -k 4
done
echo TUNE_DONE
