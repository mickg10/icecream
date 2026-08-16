#!/bin/bash
# EMPTY-ONLINE CONTROL: same superblock codec, NO prior (row-set empty), reusing the
# existing per-corpus traces. Gives the no-prior online baseline so
#   prior_contribution = pretrained_seed_online - empty_online   (per corpus)
# directly measures how much the (gcc-11-trained) prior TRANSLATES -- esp. across the
# toolchain boundary on the clang corpora (Firefox/ClickHouse/V8).
set -u
BK=$HOME/bakeoff
cd "$BK" || exit 1
mkdir -p reports_empty curves_empty logs
PKG="$BK/online-bootstrap-common-rocks-opencv.zst"   # ignored by empty specs; any valid pkg loads fine
OUT="$HOME/bakeoff25_empty.tsv"

declare -A NAME=( [corpus]=LLVM [corpus2]=RocksDB [corpus3]=DuckDB [corpus4]=abseil-protobuf [corpus5]=OpenCV [corpus6]=Godot [corpus7]=fmt [corpus8]=spdlog [corpus9]=Catch2 [corpus10]=nlohmann-json [corpus11]=range-v3 [corpus12]=Eigen [corpus13]=re2 [corpus14]=LevelDB [corpus15]=simdjson [corpus16]=cereal [corpus17]=GCC [corpus18]=Firefox [corpus19]=Qt6 [corpus20]=ClickHouse [corpus21]=PyTorch [corpus22]=Folly [corpus23]=Arrow [corpus24]=Bitcoin [corpus25]=V8 )
ORDER="corpus8 corpus7 corpus13 corpus14 corpus16 corpus10 corpus15 corpus23 corpus11 corpus22 corpus24 corpus3 corpus2 corpus17 corpus9 corpus4 corpus12 corpus corpus19 corpus5 corpus21 corpus6 corpus25 corpus20 corpus18"

echo -e "corpus\tname\ttus\tempty_online\texact" > "$OUT"
i=0; N=$(echo $ORDER | wc -w)
for c in $ORDER; do
  i=$((i+1))
  tr="$BK/traces/ml-$c.bin"
  if [ ! -f "$tr" ]; then echo "[$i/$N] SKIP $c (no trace)"; echo -e "$c\t${NAME[$c]}\tNA\tNA\tNOTRACE" >> "$OUT"; continue; fi
  echo "[$i/$N] EMPTY $c (${NAME[$c]})"
  if ! nice -19 python3 online_bootstrap_curves.py --test "$tr" --package-in "$PKG" \
        --online-budget 524288 --thresholds 2 --level 3 --model-level 3 \
        --publication first-use --budget-basis ids32 --row-set empty \
        --max-tus 200 --report "reports_empty/$c.json" --curve-tsv "curves_empty/$c.tsv" >"logs/empty-$c.log" 2>&1; then
    echo "[$i/$N] CURVEFAIL $c (see logs/empty-$c.log)"
    echo -e "$c\t${NAME[$c]}\tNA\tNA\tCURVEFAIL" >> "$OUT"; continue
  fi
  row=$(python3 -c "import json;d=json.load(open('reports_empty/$c.json'));r=[x for x in d['rows'] if 'online' in x['name']][0];print('%s\t%s\t%s'%(r['tus'],r['charged_ratio'],r.get('exact')))" 2>>"logs/empty-$c.log")
  if [ -z "$row" ]; then echo "[$i/$N] PARSEFAIL $c"; echo -e "$c\t${NAME[$c]}\tNA\tNA\tPARSEFAIL" >> "$OUT"; continue; fi
  echo -e "$c\t${NAME[$c]}\t$row" >> "$OUT"
  echo "[$i/$N] DONE $c (${NAME[$c]}): empty=$(echo "$row" | cut -f2)x"
done
echo "=== BAKEOFF25-EMPTY COMPLETE ==="
column -t -s$'\t' "$OUT"
