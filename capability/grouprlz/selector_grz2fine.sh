#!/usr/bin/env bash
# Fine-grained GRZ2 curve for SHAPE only: --gtu 8 instead of the binding 112.
# Different configuration => different (worse) total; never mix with the binding wire.
set -uo pipefail
GRZ=$HOME/issue16-selector-v1/tools/grz2g-selector
II=$HOME/grouprlz/ii; W=$HOME/selbind/curves
for C in "$@"; do
  nice -n 5 taskset -c 0-15 $GRZ enc $II/$C.ii $W/$C.fine.grz -u $II/$C.tu \
    -m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --gtu 8 --graw 512 --gadd 128 --hist 1024 -j 8 \
    --curve $W/$C.grz2fine.tsv > $W/$C.grz2fine.out 2> $W/$C.grz2fine.err
  echo "$C fine_wire=$(cut -f2 $W/$C.grz2fine.out) groups=$(( $(wc -l < $W/$C.grz2fine.tsv) - 1 ))" >&2
  rm -f $W/$C.fine.grz
done
echo "GRZ2 FINE DONE $(date +%T)" >&2
