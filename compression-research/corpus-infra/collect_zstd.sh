#!/bin/bash
# Phase 2: the z19-long metrics pass -- 32 jobs (one .ii and one raw-source measurement
# per corpus) through a worker pool, each job single-threaded and deprioritised.
#
#   ./collect_zstd.sh [-P N]        (default pool 6)
#
# Only `zstd -19 --long=31` is measured; the plain 8 MB-window numbers were dropped.
# 31 is zstd's hard maximum window (2 GiB), so the six corpora whose .ii exceed 2 GiB
# are window-limited and their number is a LOWER bound on cross-TU redundancy.
#
# Nothing is stored but the size: each job pipes the concatenated content straight into
# `wc -c`.  Results land in comp2/ (one file per job, plus results.tsv), so the pass is
# resumable and never depends on job completion order.
set -uo pipefail
INFRA=/tanksmall/scratch/ictmp/corpus-infra
POOL=${POOL:-6}
[ "${1:-}" = "-P" ] && POOL=$2

mkdir -p "$INFRA/comp2" "$INFRA/joblogs"
touch "$INFRA/comp2/.lock"
[ -f "$INFRA/comp2/results.tsv" ] || : > "$INFRA/comp2/results.tsv"

jobs="$INFRA/joblogs/z19.jobs"
# Largest input first: the long pole (corpus6 .ii, 5.9 GB single-threaded) has to start
# immediately or it decides the wall time on its own.
{
  for c in corpus6 corpus5 corpus corpus12 corpus2 corpus4 corpus3 corpus9 corpus11 \
           corpus15 corpus16 corpus10 corpus14 corpus7 corpus13 corpus8; do
    echo "$c ii"
  done
  for c in corpus corpus6 corpus5 corpus3 corpus2 corpus4 corpus15 corpus12 corpus10 \
           corpus14 corpus11 corpus9 corpus7 corpus16 corpus8 corpus13; do
    echo "$c src"
  done
} > "$jobs"

echo "=== $(wc -l < "$jobs") z19-long jobs, pool=$POOL, $(date +%T) ==="
xargs -a "$jobs" -L1 -P "$POOL" bash "$INFRA/z19_one.sh"
echo "=== pool finished rc=$? $(date +%T) ==="
echo "results: $(wc -l < "$INFRA/comp2/results.tsv") lines, $(ls "$INFRA"/comp2/corpus*.{ii,src} 2>/dev/null | wc -l)/32 measurements"
