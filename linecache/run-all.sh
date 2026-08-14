#!/usr/bin/env bash
# Reproduce the full online-superblock measurement matrix (issue #16).
# One-command regeneration target for the report data. Absolute paths throughout.
set -euo pipefail
D=/tanksmall/scratch/ictmp/icecream-superblock/linecache
T=$D/traces
BIN=$D/online-superblock
mkdir -p "$T"
cd "$D"
g++ -O3 -DNDEBUG -march=native -std=c++17 online-superblock.cpp -o "$BIN" -lzstd

run() { # tag manifest
  local tag=$1 man=$2
  echo "=== $tag ==="
  taskset -c 3 "$BIN" --manifest "$man" --tag "$tag" --K 8 --sweepK 2,4,8,16,32,64 \
      --batch-min 16 --tracedir "$T" >"$T/stdout-$tag.log" 2>"$T/stderr-$tag.log"
  echo "$tag exit=$? (see $T/stdout-$tag.log)"
}

run llvm     /tanksmall/scratch/ictmp/corpus/manifest.txt
run rocksdb  /tanksmall/scratch/ictmp/corpus2/manifest.txt
run duckdb   /tanksmall/scratch/ictmp/corpus3/manifest.txt
echo "ALL CORPORA DONE"
