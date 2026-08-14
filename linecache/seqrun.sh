#!/usr/bin/env bash
set -uo pipefail
D=/tanksmall/scratch/ictmp/icecream-superblock/linecache
cd "$D"
g++ -O3 -DNDEBUG -march=native -std=c++17 online-superblock.cpp -o online-superblock -lzstd || { echo "BUILD FAIL"; exit 1; }
declare -A M=( [llvm]=/tanksmall/scratch/ictmp/corpus/manifest.txt [rocksdb]=/tanksmall/scratch/ictmp/corpus2/manifest.txt [duckdb]=/tanksmall/scratch/ictmp/corpus3/manifest.txt )
for tag in llvm rocksdb duckdb; do
  echo "START $tag $(date +%T)"
  taskset -c 3 ./online-superblock --manifest "${M[$tag]}" --tag "$tag" --K 8 --sweepK 2,4,8,16,32,64 --batch-min 16 --tracedir traces >"traces/stdout-$tag.log" 2>"traces/stderr-$tag.log"
  echo "END $tag exit=$? $(date +%T)"
done
touch traces/ALLDONE
echo "ALLDONE $(date +%T)"
