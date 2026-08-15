#!/bin/bash
# Transfer snapshots for the 16 .ii corpora and the 14 raw-source training roots.
#
#   ./snapshot_corpuses.sh [-P N]
#
# These archives exist to be SHIPPED (rsync'd to a bakeoff box), so they are compressed
# fast -- `zstd -3 --long=27 -T0`, not the -19 used for the metrics table.  The z19-long
# numbers in corpus-metrics.md are a measurement piped to `wc -c`; no z19 artifact is
# ever stored.
#
# Output goes to /tanksmall/scratch/ictmp/corpus-snapshots/, deliberately OUTSIDE the
# git worktree, and is gitignored.  Each archive is written to a .tmp and mv'd into
# place, so a concurrent rsync can ship archives as they appear without ever seeing a
# partial file.  SHA256SUMS and REHYDRATE.md are written LAST and signal completion.
#
# Existing archives are kept; delete one to rebuild it.
set -uo pipefail
ICT=/tanksmall/scratch/ictmp
INFRA=$ICT/corpus-infra
OUT=$ICT/corpus-snapshots
POOL=${POOL:-5}
[ "${1:-}" = "-P" ] && POOL=$2

CORPORA=(corpus corpus2 corpus3 corpus4 corpus5 corpus6 corpus7 corpus8 corpus9 \
         corpus10 corpus11 corpus12 corpus13 corpus14 corpus15 corpus16)
# The 14 raw-source training roots (note: not every corpus contributes one).
SRCROOTS=($ICT/build2/rocksdb $ICT/build2/abseil-cpp $ICT/build2/opencv $ICT/build2/godot \
          $ICT/src2/fmt $ICT/src2/spdlog $ICT/src2/catch2 $ICT/src2/json $ICT/src2/range-v3 \
          $ICT/src2/eigen $ICT/src2/re2 $ICT/src2/leveldb $ICT/src2/simdjson $ICT/src2/cereal)

mkdir -p "$OUT" "$INFRA/joblogs"
jobs="$INFRA/joblogs/snapshot.jobs"
: > "$jobs"
# Biggest first so the long pole starts immediately and the pool drains evenly.
for c in corpus6 corpus5 corpus corpus12 corpus2 corpus4 corpus3 corpus9 corpus11 \
         corpus15 corpus16 corpus10 corpus14 corpus7 corpus13 corpus8; do
  echo "ii $c" >> "$jobs"
done
for d in "${SRCROOTS[@]}"; do echo "src $d" >> "$jobs"; done

echo "=== $(wc -l < "$jobs") snapshot jobs, pool=$POOL, $(date +%T) ==="
xargs -a "$jobs" -L1 -P "$POOL" bash "$INFRA/snap_one.sh"
rc=$?
echo "=== pool finished rc=$rc $(date +%T) ==="

# ---- completion signal: only written once every archive is in place -------------
missing=0
for c in "${CORPORA[@]}"; do
  [ -s "$OUT/$c.ii.tar.zst" ] || { echo "MISSING $c.ii.tar.zst"; missing=1; }
done
for d in "${SRCROOTS[@]}"; do
  [ -s "$OUT/src-$(basename "$d").tar.zst" ] || { echo "MISSING src-$(basename "$d").tar.zst"; missing=1; }
done
if [ "$missing" != "0" ]; then
  echo "INCOMPLETE -- not writing SHA256SUMS"; exit 1
fi

cp "$INFRA/REHYDRATE.md" "$OUT/REHYDRATE.md"
( cd "$OUT" && sha256sum ./*.tar.zst > SHA256SUMS )
echo "SHA256SUMS + REHYDRATE.md written -- ALL SNAPSHOTS COMPLETE $(date +%T)"
echo "total $(stat -c%s "$OUT"/*.tar.zst | awk '{s+=$1} END{printf "%d bytes (%.1f MiB)\n", s, s/1048576}')"
