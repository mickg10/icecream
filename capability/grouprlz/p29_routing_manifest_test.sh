#!/usr/bin/env bash
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MANIFEST=${1:?usage: p29_routing_manifest_test.sh MANIFEST [WORK]}
WORK=${2:-$(mktemp -d /tmp/p29-routing-manifest.XXXXXX)}
mkdir -p "$WORK"

g++ -O2 -std=c++17 -DICE_LINE_CAP_LOG2=23 -Wall -Wextra -Wpedantic -Werror \
    -pthread "$HERE/p29_routing_manifest.cpp" "$HERE/../cap_codec.cpp" \
    -o "$WORK/p29-routing-manifest" -lzstd

for policy in r0-roundrobin r0-fastest r1-resident r2-home r3-rendezvous r4-state; do
    "$WORK/p29-routing-manifest" --manifest "$MANIFEST" --max-files 4 \
        --repetitions 2 --workers 3 --slots 2,1,1 --requested-slots 3 \
        --egress-lanes 3 --policy "$policy" \
        --assignment-out "$WORK/$policy.assignment" --curve-out "$WORK/$policy.tsv" \
        >"$WORK/$policy.out" 2>"$WORK/$policy.err"
    grep -q '^routing-assignment-v1$' "$WORK/$policy.assignment"
    test "$(wc -l <"$WORK/$policy.assignment")" -eq 9
    test "$(wc -l <"$WORK/$policy.tsv")" -eq 9
    grep -q '^ROUTING_ESTIMATE schema=independent-region-zstd3-v1' "$WORK/$policy.out"
done

# Stable TUKey rendezvous must retain each TU's destination on the second repetition.
awk 'NR==2 { first[0]=$2 } NR==3 { first[1]=$2 } NR==4 { first[2]=$2 } NR==5 { first[3]=$2 }
     NR>=6 { idx=NR-6; if ($2 != first[idx]) exit 1 }
     END { if (NR != 9) exit 1 }' "$WORK/r3-rendezvous.assignment"

# A separately supplied suffix cannot alter an R4 prefix.
"$WORK/p29-routing-manifest" --manifest "$MANIFEST" --max-files 2 \
    --workers 3 --slots 2,1,1 --requested-slots 3 --egress-lanes 3 --policy r4-state \
    --assignment-out "$WORK/r4-prefix.assignment" --curve-out "$WORK/r4-prefix.tsv" \
    >"$WORK/r4-prefix.out" 2>"$WORK/r4-prefix.err"
head -n 3 "$WORK/r4-state.assignment" >"$WORK/r4-full.prefix"
cmp "$WORK/r4-prefix.assignment" "$WORK/r4-full.prefix"

g++ -O1 -g -fsanitize=address,undefined -std=c++17 -DICE_LINE_CAP_LOG2=23 \
    -Wall -Wextra -Wpedantic -Werror -pthread \
    "$HERE/p29_routing_manifest.cpp" "$HERE/../cap_codec.cpp" \
    -o "$WORK/p29-routing-manifest.san" -lzstd
ASAN_OPTIONS=detect_leaks=1 "$WORK/p29-routing-manifest.san" \
    --manifest "$MANIFEST" --max-files 2 --workers 2 --requested-slots 2 \
    --egress-lanes 2 --policy r4-state \
    --assignment-out "$WORK/r4-san.assignment" --curve-out "$WORK/r4-san.tsv" \
    >"$WORK/r4-san.out" 2>"$WORK/r4-san.err"

echo "P29 manifest R0-R4 assignment adapter PASS evidence=$WORK"
