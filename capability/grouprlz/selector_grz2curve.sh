#!/usr/bin/env bash
set -uo pipefail
GRZ=$HOME/issue16-selector-v1/tools/grz2g-selector
II=$HOME/grouprlz/ii; W=$HOME/selbind/curves
for C in "$@"; do
  nice -n 5 taskset -c 0-15 $GRZ enc $II/$C.ii $W/$C.grz -u $II/$C.tu \
    -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 112 --graw 512 --gadd 128 --hist 1024 -j 8 \
    --curve $W/$C.grz2curve.tsv > $W/$C.grz2.out 2> $W/$C.grz2.err
  echo "$C rc=$? wire=$(cut -f2 $W/$C.grz2.out) groups=$(( $(wc -l < $W/$C.grz2curve.tsv) - 1 ))" >&2
done
echo "GRZ2 CURVES DONE $(date +%T)" >&2
