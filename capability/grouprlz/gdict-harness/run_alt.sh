#!/bin/bash
# Two-phase P29+BSC run: $1=tag  $2=manifest
set -euo pipefail
CPUS=${CPUS:-0-23}
B=/home/ttuser/gdict
tag="$1"; man="$2"; shift 2 || true
mkdir -p "$B/runs" "$B/plans/$tag"
CFG=(--z 3 --mixed-regions --byte-array-lines --direct-ordinals
     --compressed-blobs --blob-threads 8 --blob-lazy-fallback --mo-factor --s1-max-chain 1024
     --blob-z 9 --blob-zstd-workers 4 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3)
# phase 1: dump lanes
/usr/bin/time -v taskset -c "$CPUS" "$B/bin/${BIN:-codec50-bsc-znver3}" --manifest "$man" "${CFG[@]}" \
  --mixed-dump-prefix "$B/plans/$tag/p" > "$B/runs/$tag.plan.out" 2> "$B/runs/$tag.plan.err"
# phase 2: BSC literal groups + curves
/usr/bin/time -v taskset -c "$CPUS" "$B/bin/${BIN:-codec50-bsc-znver3}" --manifest "$man" "${CFG[@]}" \
  --literal-group-prefix "$B/plans/$tag/p" --literal-group-tus 112 --literal-group-workers 16 \
  --literal-group-skip-zstd10 --literal-group-wire "$B/runs/$tag.wire" \
  --curve-tsv "$B/runs/$tag.curve.tsv" --component-curve-tsv "$B/runs/$tag.components.tsv" \
  "$@" > "$B/runs/$tag.out" 2> "$B/runs/$tag.err"
echo "== $tag =="; grep -E "byte-exact|TOTAL=" "$B/runs/$tag.out" | head -3
