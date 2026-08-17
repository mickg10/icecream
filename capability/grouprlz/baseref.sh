#!/bin/bash
# Whole-program z19 reference with full provenance, computed by STREAMING the complete
# ordered .ii through the zstd CLI (no in-buffer read, nothing that can hit an INT_MAX cap).
# The reference is itself round-tripped and digest-checked, so a truncated reference
# cannot silently pass.
cd ~/grouprlz
OUT=$HOME/grouprlz/baseref.tsv
ZSTD=${ZSTD:-zstd}
[ -f "$OUT" ] || echo -e "id\traw_bytes\traw_sha256\tzstd\tlevel\tlong\tcomp_bytes\tdec_bytes\troundtrip\tsecs" > "$OUT"
for c in $CORPORA; do
  grep -qP "^$c\t" "$OUT" && continue
  F=ii/$c.ii
  [ -e "$F" ] || { echo "missing $F"; continue; }
  RAW=$(stat -Lc %s "$F")
  RSHA=$(sha256sum < "$F" | cut -d' ' -f1)
  VER=$($ZSTD --version 2>&1 | grep -oP 'v[0-9]+\.[0-9]+\.[0-9]+' | head -1)
  t0=$(date +%s.%N)
  $ZSTD -19 --long=31 -T0 -c "$F" > /tmp/ref$$.zst
  t1=$(date +%s.%N)
  COMP=$(stat -c %s /tmp/ref$$.zst)
  DEC=$($ZSTD -d --long=31 -c /tmp/ref$$.zst | wc -c)
  DSHA=$($ZSTD -d --long=31 -c /tmp/ref$$.zst | sha256sum | cut -d' ' -f1)
  rm -f /tmp/ref$$.zst
  if [ "$DSHA" = "$RSHA" ] && [ "$DEC" = "$RAW" ]; then RT=OK; else RT=MISMATCH; fi
  awk -v c=$c -v r=$RAW -v rs=$RSHA -v v=$VER -v cb=$COMP -v db=$DEC -v rt=$RT \
      -v s=$(echo "$t1-$t0"|bc) 'BEGIN{printf "%s\t%.0f\t%s\t%s\t19\t31\t%d\t%.0f\t%s\t%.1f\n",c,r,rs,v,cb,db,rt,s}' | tee -a "$OUT"
done
echo BASEREF_DONE
