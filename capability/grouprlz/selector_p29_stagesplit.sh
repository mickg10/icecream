#!/usr/bin/env bash
# Complete-encode stage split using the observation-only instrumented 56c1744 build.
set -uo pipefail
P=$1 PR=${2:-debian-gcc}
W=$HOME/selbind/stage/$P-$PR; rm -rf "$W"; mkdir -p "$W/ex"
CELL=$HOME/ictmp/ii-matrix/$P/$PR
P29=$HOME/selbind/p29build/codec50-inst
P29C="--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3"
PAY=$(python3 -c "import json;print(json.load(open('$CELL/corpus.json'))['payload']['path'])")
zstd -d --long=31 -q -c "$CELL/$PAY" | tar -xf - -C "$W/ex"
awk -F'\t' -v d="$W/ex/" 'NR>1{print d $2}' "$CELL/manifest.tsv" > "$W/man"
taskset -c 16-23 $P29 --manifest "$W/man" $P29C --mixed-dump-prefix "$W/pl" > "$W/pl.out" 2> "$W/pl.err"
taskset -c 16-23 $P29 --manifest "$W/man" $P29C --literal-group-prefix "$W/pl" --literal-group-tus 112 \
    --stable-root-tags --literal-group-workers 8 --literal-group-skip-zstd10 \
    --literal-group-wire "$W/lit" > "$W/g.out" 2> "$W/g.err"
rm -rf "$W/ex" "$W"/pl.*.raw
sed -n 's/^STAGECLOCK .*process_total=\([0-9.]*\) predict=\([0-9.]*\) scan=\([0-9.]*\) lines=\([0-9.]*\) store=\([0-9.]*\).*iters=\([0-9]*\) predict_hit=\([0-9]*\) scan_hit=\([0-9]*\) new_regions=\([0-9]*\) lines_interned=\([0-9]*\)/\1 \2 \3 \4 \5 \6 \7 \8 \9 \10/p' "$W/g.err" | head -1 | \
awk -v p="$P" -v pr="$PR" -v il="$(sed -n 's/^loaded+interned \([0-9.]*\)s.*/\1/p' "$W/g.err"|head -1)" \
    -v tt="$(sed -n 's/.*total=\([0-9.]*\)s.*/\1/p' "$W/g.err"|tail -1)" \
    '{printf "%s\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.1f\t%.1f\t%.1f\t%.1f\t%s\t%s\t%s\t%s\t%s\n",
      p,pr,il,tt,$1,$2,$3,$4,100*$2/$1,100*$3/$1,100*$4/$1,100*$5/$1,$6,$7,$8,$9,$10}'
