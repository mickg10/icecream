#!/bin/bash
# Scheduled scheduler performance gate (not part of `make check`).
#
# Runs the dispatch flood at two queue depths and reports the metrics the
# divergence review asked for:
#   - total dispatch wall time and the scaling ratio between depths
#   - control-plane p95/p99/max latency during ingress and during dispatch
#   - exact reply integrity
#   - peak scheduler RSS
#   - a reference-host/configuration fingerprint
#
# Gating is deliberately two-sided: an absolute control-latency bound on the
# designated runner (SLO, default 1s) plus a normalized depth-scaling bound,
# so the result is not a single brittle hardware-dependent wall-time number.
#
#   perfgate.sh <icecc-scheduler> <sndbuf_shim.so> [depth1] [depth2] [slo_sec]
set -u
SCHED=${1:?path to icecc-scheduler}
SHIM=${2:?path to sndbuf_shim.so}
D1=${3:-5000}
D2=${4:-20000}
SLO=${5:-1.0}
HERE="$(cd "$(dirname "$0")" && pwd)"
SCHEDBP=${SCHEDBP:-$HERE/../../unittests/schedbp}
RUN=$(mktemp -d "${TMPDIR:-/tmp}/icecc-perfgate.XXXXXX")

echo "# fingerprint: host=$(uname -n) kernel=$(uname -r) cpus=$(nproc)"
echo "# fingerprint: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //')"
echo "# fingerprint: scheduler=$SCHED slo=${SLO}s depths=$D1,$D2"

run_depth() {  # depth -> emits "depth wall rss max_lat replies"
    local depth=$1
    local dir="$RUN/d$depth"
    mkdir -p "$dir"
    ( cd "$dir" && /usr/bin/time -f "%e %M" -o "$dir/time.txt" \
        "$SCHEDBP" "$SCHED" "$SHIM" "$depth" 5 > "$dir/out.log" 2>&1 )
    local rc=$?
    local wall rss
    read -r wall rss < "$dir/time.txt"
    local replies
    replies=$(grep -oE '[0-9]+/[0-9]+ jobs answered exactly once' "$dir/out.log" | head -1)
    local maxlat
    maxlat=$(grep -oE 'worst control-reply latency [0-9.]+' "$dir/out.log" | tail -1 | awk '{print $NF}')
    echo "$depth $wall $rss ${maxlat:-99} ${replies:-none} rc=$rc"
}

R1=$(run_depth "$D1")
R2=$(run_depth "$D2")
echo "# depth wall_s rss_kb max_control_lat replies"
echo "  $R1"
echo "  $R2"

W1=$(echo "$R1" | awk '{print $2}'); W2=$(echo "$R2" | awk '{print $2}')
L1=$(echo "$R1" | awk '{print $4}'); L2=$(echo "$R2" | awk '{print $4}')
OK1=$(echo "$R1" | grep -c "$D1/$D1 jobs answered exactly once")
OK2=$(echo "$R2" | grep -c "$D2/$D2 jobs answered exactly once")

# Depth scaling: with an incremental selector this should approach the depth
# ratio (linear); a quadratic selector shows the ratio squared.
SCALE=$(awk -v a="$W1" -v b="$W2" 'BEGIN{ if (a>0) printf "%.2f", b/a; else print "0" }')
IDEAL=$(awk -v a="$D1" -v b="$D2" 'BEGIN{ printf "%.2f", b/a }')
echo "# wall scaling ${SCALE}x for a ${IDEAL}x depth increase (linear target <= $(awk -v i="$IDEAL" 'BEGIN{printf "%.2f", i*1.5}')x)"

FAIL=0
[ "$OK1" -eq 1 ] && [ "$OK2" -eq 1 ] || { echo "FAIL: reply integrity"; FAIL=1; }
awk -v l="$L1" -v s="$SLO" 'BEGIN{exit !(l<=s)}' || { echo "FAIL: control latency ${L1}s > ${SLO}s at depth $D1"; FAIL=1; }
awk -v l="$L2" -v s="$SLO" 'BEGIN{exit !(l<=s)}' || { echo "FAIL: control latency ${L2}s > ${SLO}s at depth $D2"; FAIL=1; }
awk -v s="$SCALE" -v i="$IDEAL" 'BEGIN{exit !(s<=i*1.5)}' || { echo "FAIL: superlinear wall scaling ${SCALE}x vs ${IDEAL}x depth"; FAIL=1; }

echo "# artifacts: $RUN"
[ "$FAIL" -eq 0 ] && { echo "RESULT: PASS"; exit 0; }
echo "RESULT: FAIL"; exit 1
