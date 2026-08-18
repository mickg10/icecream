#!/usr/bin/env bash
# codec50 (P29+BSC) per-TU cumulative wire on the BATCH C-side basis -- the same
# configuration and the same wire as the cold-C-encode /goal row.  --curve-tsv is a
# pre-existing codec50 feature and self-checks that the per-TU sums equal the totals.
set -uo pipefail
BIN=$HOME/selbind/p29build/codec50-refZ; W=$HOME/selbind/curves
P29C="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3"
for C in "$@"; do
  T=$(mktemp -d); MAN=$HOME/ictmp/$C/manifest.txt
  nice -n 5 taskset -c 0-15 $BIN --manifest $MAN $P29C --mixed-dump-prefix $T/pl > $T/pl.out 2>&1
  nice -n 5 taskset -c 0-15 $BIN --manifest $MAN $P29C --literal-group-prefix $T/pl \
    --literal-group-tus 112 --stable-root-tags --literal-group-workers 8 \
    --literal-group-skip-zstd10 --literal-group-wire $T/lit \
    --curve-tsv $W/$C.p29batch.tsv --component-curve-tsv $W/$C.p29components.tsv \
    > $W/$C.p29batch.out 2> $W/$C.p29batch.err
  TOT=$(grep -o "TOTAL=[0-9]*" $W/$C.p29batch.out | head -1 | cut -d= -f2)
  END=$(tail -1 $W/$C.p29batch.tsv | cut -f5)
  NEX=$(awk -F"\t" "NR>1 && \$7!=\"true\"" $W/$C.p29batch.tsv | wc -l)
  echo "$C TOTAL=$TOT curve_end=$END match=$([ "$TOT" = "$END" ] && echo YES || echo NO) rows=$(( $(wc -l < $W/$C.p29batch.tsv) - 1 )) non_exact_tus=$NEX" >&2
  rm -rf $T
done
echo "P29 BATCH CURVES DONE $(date +%T)" >&2
