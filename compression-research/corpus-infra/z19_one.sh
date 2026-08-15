#!/bin/bash
# One z19-long metrics job: measure `zstd -19 --long=31` over the pure concatenated
# content of a file set.  SINGLE-THREADED on purpose -- the pool fills the cores with
# concurrent corpora, not with threads inside one file, and single-threaded output is
# the reproducible number (MT framing changes the size slightly).
#
#   z19_one.sh <corpusN> {ii|src}
#
# Deprioritised (nice 19 + idle I/O class): these are the slow jobs and must yield to
# the transfer snapshots and to whatever else is on the box.
#
# Result is written to comp2/<corpus>.<which> only after the whole pipeline exits 0, so
# the file's existence means the number is trustworthy and the job is resumable.
set -uo pipefail
INFRA=/tanksmall/scratch/ictmp/corpus-infra
c="$1"; which="$2"
list="$INFRA/lists/$c.$which.list"
out="$INFRA/comp2/$c.$which"
mkdir -p "$INFRA/comp2"

[ -s "$out" ] && { echo "[$c.$which] cached $(cat "$out")"; exit 0; }
[ -s "$list" ] || { echo "[$c.$which] EMPTY LIST -> null"; echo null > "$out"; exit 0; }

t0=$SECONDS
xargs -a "$list" -d'\n' cat | nice -n 19 ionice -c3 zstd -19 --long=31 -c | wc -c > "$out.tmp"
st=("${PIPESTATUS[@]}")
if [ "${st[0]}" != "0" ] || [ "${st[1]}" != "0" ] || [ "${st[2]}" != "0" ]; then
  echo "[$c.$which] PIPELINE FAILED (${st[*]})"; rm -f "$out.tmp"; exit 1
fi
mv "$out.tmp" "$out"
bytes=$(cat "$out")
# one line per job, order-independent; flock so concurrent appends cannot interleave
flock "$INFRA/comp2/.lock" -c "printf '%s\t%s\tz19long\t%s\n' '$c' '$which' '$bytes' >> '$INFRA/comp2/results.tsv'"
echo "[$c.$which] $bytes bytes in $((SECONDS - t0))s"
