#!/usr/bin/env bash
# Round 2: the four small corpora that -b 2 did not lift over 1 GB/s, plus the
# single-thread cost of the -b 2 policy on the three that it did lift.
set -uo pipefail

GRZ=$HOME/grouprlz/grz2g
II=$HOME/grouprlz/ii
R=$HOME/selbind/tune
REPS=3
NICE="nice -n 5 ionice -c2 -n5"
mkdir -p "$R"
OUT=$R/tune2.tsv
printf 'corpus\tvariant\traw\tout\tC_gbps_min\tC_gbps_med\tF_gbps\texact\tload\twire_sha256\n' > "$OUT"

BASE=(-m g2 -K 256 -s 6 -l 4 -k 5 --gtu 112 --graw 512 --gadd 128)

run() {
  local c=$1 label=$2; shift 2
  local raw rates=() rep t f x
  raw=$(stat -Lc %s "$II/$c.ii")
  cat "$II/$c.ii" > /dev/null
  for rep in $(seq 1 $REPS); do
    /usr/bin/time -f %e -o "$R/2.$c.$label.$rep.wall" $NICE taskset -c 0-15 \
        "$GRZ" enc "$II/$c.ii" "$R/2.$c.$label.grz" -u "$II/$c.tu" "${BASE[@]}" "$@" \
        > "$R/2.$c.$label.$rep.out" 2> "$R/2.$c.$label.$rep.err" || { echo "ENC FAIL $c $label"; return; }
    rates+=("$(awk -v r="$raw" '{printf "%.4f", r/1e9/$1}' "$R/2.$c.$label.$rep.wall")")
  done
  $NICE taskset -c 0-15 "$GRZ" dec "$R/2.$c.$label.grz" /dev/null -j 1 \
      > /dev/null 2> "$R/2.$c.$label.dec.err"
  f=$(sed -n 's/.* F=\([0-9]*\) B\/s.*/\1/p' "$R/2.$c.$label.dec.err" | awk '{printf "%.4f", $1/1e9}')
  $NICE taskset -c 0-15 "$GRZ" dec "$R/2.$c.$label.grz" "$R/replay.ii" -j 1 > /dev/null 2>&1
  if cmp -s "$II/$c.ii" "$R/replay.ii"; then x=YES; else x=NO; fi
  rm -f "$R/replay.ii"
  t=$(awk '{print $2}' "$R/2.$c.$label.1.out")
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$c" "$label" "$raw" "$t" \
      "$(printf '%s\n' "${rates[@]}" | sort -g | head -1)" \
      "$(printf '%s\n' "${rates[@]}" | sort -g | sed -n 2p)" \
      "$f" "$x" "$(cut -d' ' -f1 /proc/loadavg)" \
      "$(sha256sum "$R/2.$c.$label.grz" | awk '{print $1}')" | tee -a "$OUT"
  rm -f "$R/2.$c.$label.grz"
}

# the four that -b 2 left short: attack fixed startup cost (index table, history ring)
for c in corpus14 corpus13 corpus7 corpus8; do
  run "$c" b2-t21-h1024   -t 21 --hist 1024 -b 2 -j 16
  run "$c" b2-t18-h1024   -t 18 --hist 1024 -b 2 -j 16
  run "$c" b2-t21-h128    -t 21 --hist 128  -b 2 -j 16
  run "$c" b2-t18-h128    -t 18 --hist 128  -b 2 -j 16
  run "$c" b1-t18-h128    -t 18 --hist 128  -b 1 -j 16
done

# the three that -b 2 lifted: confirm, and price the same policy at one thread
for c in corpus3 corpus15 corpus10; do
  run "$c" b2-j16-confirm -t 21 --hist 1024 -b 2 -j 16
  run "$c" b2-j1          -t 21 --hist 1024 -b 2 -j 1
  run "$c" b8-j8-control  -t 21 --hist 1024 -b 8 -j 8
done
echo TUNE2_DONE
