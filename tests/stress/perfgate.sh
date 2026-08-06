#!/bin/bash
# Scheduled scheduler performance gate (not part of `make check`).
#
# Runs the dispatch flood at two queue depths and reports, per depth and per
# PHASE (ingress = requests still arriving; drain = queue emptying):
#   - control-plane p95 / p99 / max latency
#   - total wall time and the scaling ratio between depths
#   - exact reply integrity and peak scheduler RSS
#   - a host + scheduler-revision fingerprint
#
# Gating: the absolute SLO applies to BOTH phases' maxima at both depths (a
# scheduler that answers promptly after the flood but not during it still
# misses the operational target), plus a normalized depth-scaling bound.
# Failed-run artifacts are kept and their path printed; successful runs are
# cleaned up.
#
#   perfgate.sh <icecc-scheduler> <sndbuf_shim.so> [depth1] [depth2] [slo_sec]
set -u

canon() { # resolve to an absolute path BEFORE any cd
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s/%s\n' "$(cd "$(dirname "$1")" && pwd)" "$(basename "$1")" ;;
    esac
}

SCHED=$(canon "${1:?path to icecc-scheduler}")
SHIM=$(canon "${2:?path to sndbuf_shim.so}")
D1=${3:-5000}
D2=${4:-20000}
SLO=${5:-1.0}
HERE="$(cd "$(dirname "$0")" && pwd)"
SCHEDBP=$(canon "${SCHEDBP:-$HERE/../../unittests/schedbp}")
for f in "$SCHED" "$SHIM" "$SCHEDBP"; do
    [ -e "$f" ] || { echo "ERROR: missing $f" >&2; exit 2; }
done
RUN=$(mktemp -d "${TMPDIR:-/tmp}/icecc-perfgate.XXXXXX")

REV=$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)
DIRTY=$(git -C "$HERE" diff --quiet 2>/dev/null || echo "+dirty")
# The digest names the EXECUTABLE that ran, not the repository state -- a
# stale binary against a clean tree would otherwise fingerprint as current.
SCHED_SHA=$(sha256sum "$SCHED" | cut -c1-16)
DRIVER_SHA=$(sha256sum "$SCHEDBP" | cut -c1-16)
CXXF=$(grep -m1 '^CXXFLAGS' "$HERE/../../Makefile" 2>/dev/null | cut -d= -f2- | tr -s ' ')
echo "# fingerprint: host=$(uname -n) kernel=$(uname -r) cpus=$(nproc)"
echo "# fingerprint: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ //')"
echo "# fingerprint: repo_rev=${REV}${DIRTY} scheduler_sha256=${SCHED_SHA} driver_sha256=${DRIVER_SHA}"
echo "# fingerprint: cxxflags=${CXXF:-unknown} slo=${SLO}s depths=$D1,$D2"

is_num() { case "$1" in ''|*[!0-9.]*) return 1;; *) return 0;; esac; }

FAIL=0
declare -A WALL
for depth in "$D1" "$D2"; do
    dir="$RUN/d$depth"
    mkdir -p "$dir"
    ( cd "$dir" && /usr/bin/time -f "%e %M" -o "$dir/time.txt" \
        "$SCHEDBP" "$SCHED" "$SHIM" "$depth" 5 perf > "$dir/out.log" 2>&1 )
    rc=$?
    if [ $rc -ne 0 ]; then
        echo "FAIL: schedbp exited $rc at depth $depth (log: $dir/out.log)"
        FAIL=1
        continue
    fi
    read -r wall rss < "$dir/time.txt" || { echo "FAIL: no timing output at depth $depth"; FAIL=1; continue; }
    is_num "$wall" && is_num "$rss" || { echo "FAIL: malformed timing '$wall $rss' at depth $depth"; FAIL=1; continue; }
    grep -q "$depth/$depth jobs answered exactly once" "$dir/out.log" \
        || { echo "FAIL: reply integrity at depth $depth"; FAIL=1; }
    for ph in ingress drain; do
        line=$(grep "# perf $ph " "$dir/out.log" | tail -1)
        [ -n "$line" ] || { echo "FAIL: no $ph phase report at depth $depth"; FAIL=1; continue; }
        p95=$(echo "$line" | grep -oE 'p95=[0-9.]+' | cut -d= -f2)
        p99=$(echo "$line" | grep -oE 'p99=[0-9.]+' | cut -d= -f2)
        mx=$(echo  "$line" | grep -oE 'max=[0-9.]+' | cut -d= -f2)
        ns=$(echo  "$line" | grep -oE 'samples=[0-9]+' | cut -d= -f2)
        dur=$(echo "$line" | grep -oE 'duration=[0-9.]+' | cut -d= -f2)
        is_num "$p95" && is_num "$p99" && is_num "$mx" && is_num "${ns:-x}" && is_num "${dur:-x}" \
            || { echo "FAIL: malformed $ph report at depth $depth: $line"; FAIL=1; continue; }
        # Minimum coverage scales with the phase's real duration: a phase
        # with zero or too-few samples for its length proves nothing (an
        # empty phase would otherwise pass as zeros), while a sub-second
        # phase measured back-to-back is legitimately small.
        need=3
        awk -v d="$dur" 'BEGIN{exit !(d<1.0)}' && need=1
        [ "$ns" -ge "$need" ] \
            || { echo "FAIL: only $ns $ph samples over ${dur}s at depth $depth (need >= $need)"; FAIL=1; }
        echo "  depth=$depth phase=$ph samples=$ns duration=${dur}s p95=${p95}s p99=${p99}s max=${mx}s"
        awk -v l="$mx" -v s="$SLO" 'BEGIN{exit !(l<=s)}' \
            || { echo "FAIL: $ph max ${mx}s > SLO ${SLO}s at depth $depth"; FAIL=1; }
    done
    WALL[$depth]=$wall
    echo "  depth=$depth wall=${wall}s rss=${rss}KiB"
done

if [ -n "${WALL[$D1]:-}" ] && [ -n "${WALL[$D2]:-}" ]; then
    SCALE=$(awk -v a="${WALL[$D1]}" -v b="${WALL[$D2]}" 'BEGIN{ if (a>0) printf "%.2f", b/a; else print 0 }')
    IDEAL=$(awk -v a="$D1" -v b="$D2" 'BEGIN{ printf "%.2f", b/a }')
    echo "# wall scaling ${SCALE}x for a ${IDEAL}x depth increase"
    awk -v s="$SCALE" -v i="$IDEAL" 'BEGIN{exit !(s<=i*1.5)}' \
        || { echo "FAIL: superlinear wall scaling ${SCALE}x vs ${IDEAL}x depth"; FAIL=1; }
fi

if [ "$FAIL" -eq 0 ]; then
    rm -rf "$RUN"
    echo "RESULT: PASS"
    exit 0
fi
echo "# failed-run artifacts kept: $RUN"
echo "RESULT: FAIL"
exit 1
