#!/bin/bash
# Target-disjoint online-bootstrap bake-off across all 25 corpora (local-oracle spec).
# One vote per corpus: pretrained-seed-online-k2 charged_ratio, seed-only, first 200 TUs.
# Package: rocks-opencv for all EXCEPT RocksDB(corpus2)/OpenCV(corpus5) -> llvm-godot,
#          keeping every test corpus disjoint from its pretraining set.
set -u
D=$HOME/ictmp
BK=$HOME/bakeoff
cd "$BK" || exit 1
mkdir -p traces reports curves logs
ROCKS="$BK/online-bootstrap-common-rocks-opencv.zst"
LLVMG="$BK/online-bootstrap-common-llvm-godot.zst"
OUT="$HOME/bakeoff25.tsv"

declare -A NAME=( [corpus]=LLVM [corpus2]=RocksDB [corpus3]=DuckDB [corpus4]=abseil-protobuf [corpus5]=OpenCV [corpus6]=Godot [corpus7]=fmt [corpus8]=spdlog [corpus9]=Catch2 [corpus10]=nlohmann-json [corpus11]=range-v3 [corpus12]=Eigen [corpus13]=re2 [corpus14]=LevelDB [corpus15]=simdjson [corpus16]=cereal [corpus17]=GCC [corpus18]=Firefox [corpus19]=Qt6 [corpus20]=ClickHouse [corpus21]=PyTorch [corpus22]=Folly [corpus23]=Arrow [corpus24]=Bitcoin [corpus25]=V8 )
# preprocessing driver: GCC corpus built with gcc -E; all others clang -E
declare -A COMP=( [corpus17]=gcc )

# small -> large for fast feedback
ORDER="corpus8 corpus7 corpus13 corpus14 corpus16 corpus10 corpus15 corpus23 corpus11 corpus22 corpus24 corpus3 corpus2 corpus17 corpus9 corpus4 corpus12 corpus corpus19 corpus5 corpus21 corpus6 corpus25 corpus20 corpus18"

echo -e "corpus\tname\tcompiler\tpackage\ttus\tcharged_ratio\tcharged_wire_bytes\tseed_model_bytes\texact\twall_s" > "$OUT"
i=0; N=$(echo $ORDER | wc -w)
for c in $ORDER; do
  i=$((i+1))
  m="$D/$c/manifest.txt"
  if [ ! -f "$m" ]; then echo "[$i/$N] SKIP $c (no manifest)"; continue; fi
  pkg="$ROCKS"; pkgname="rocks-opencv"
  if [ "$c" = corpus2 ] || [ "$c" = corpus5 ]; then pkg="$LLVMG"; pkgname="llvm-godot"; fi
  nm="${NAME[$c]}"; cp="${COMP[$c]:-clang}"
  echo "[$i/$N] START $c ($nm) pkg=$pkgname"
  tr="$BK/traces/ml-$c.bin"
  if ! nice -19 ./ml_bakeoff --manifest "$m" --export "$tr" --export-only --max-files 210 >"logs/gen-$c.log" 2>&1; then
    echo "[$i/$N] GENFAIL $c (see logs/gen-$c.log)"
    echo -e "$c\t$nm\t$cp\t$pkgname\tNA\tNA\tNA\tNA\tGENFAIL\tNA" >> "$OUT"; continue
  fi
  if ! nice -19 python3 online_bootstrap_curves.py --test "$tr" --package-in "$pkg" \
        --online-budget 524288 --thresholds 2 --level 3 --model-level 3 \
        --publication first-use --budget-basis ids32 --row-set pretrained \
        --pretrained-mode seed-only --max-tus 200 \
        --report "reports/$c.json" --curve-tsv "curves/$c.tsv" >"logs/run-$c.log" 2>&1; then
    echo "[$i/$N] CURVEFAIL $c (see logs/run-$c.log)"
    echo -e "$c\t$nm\t$cp\t$pkgname\tNA\tNA\tNA\tNA\tCURVEFAIL\tNA" >> "$OUT"; continue
  fi
  row=$(python3 -c "import json;d=json.load(open('reports/$c.json'));r=d['rows'][0];print('%s\t%s\t%s\t%s\t%s\t%.1f'%(r['tus'],r['charged_ratio'],r['charged_wire_bytes'],r.get('seed_model_compressed_bytes'),r.get('exact'),d['wall_seconds']))" 2>>"logs/run-$c.log")
  if [ -z "$row" ]; then
    echo "[$i/$N] PARSEFAIL $c"
    echo -e "$c\t$nm\t$cp\t$pkgname\tNA\tNA\tNA\tNA\tPARSEFAIL\tNA" >> "$OUT"; continue
  fi
  echo -e "$c\t$nm\t$cp\t$pkgname\t$row" >> "$OUT"
  echo "[$i/$N] DONE $c ($nm): ratio=$(echo "$row" | cut -f2)x tus=$(echo "$row" | cut -f1) exact=$(echo "$row" | cut -f5)"
done
echo "=== BAKEOFF25 COMPLETE ==="
column -t -s$'\t' "$OUT"
