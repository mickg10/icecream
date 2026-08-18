#!/usr/bin/env bash
set -uo pipefail
OUT=$HOME/selbind/coldc2.tsv
printf "corpus\trep\traw\tp29_wire\tC_input_ready_s\tC_encode_ready_s\tC_total_s\tC_encode_GBps\tz19\tp29_over_z19\tpeakRSS_KiB\tgrz_wire\tidentity\tbyte_exact\n" > $OUT
for c in corpus8 corpus13 corpus14 corpus7 corpus10 corpus16 corpus15 corpus23 corpus22 corpus3 corpus24 corpus17 corpus4 corpus5 corpus19 corpus21; do
  echo "=== $c $(date +%T) ===" >&2
  nice -n 5 ionice -c2 -n5 $HOME/selbind/coldc2.sh $c >> $OUT 2>> $HOME/selbind/coldc2.err
  echo "  $c $(cat $HOME/selbind/coldc2/$c/identity.txt 2>/dev/null)" >&2
done
echo "SWEEP DONE $(date +%T)" >&2
