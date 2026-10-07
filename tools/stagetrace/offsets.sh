#!/usr/bin/env bash
# offsets.sh -- probe per-host clock offset vs. the local (reference) clock,
# for use with analyze.py's --offsets flag.
#
# stage_trace() records realtime_ns using CLOCK_REALTIME on whichever host
# produced the record. Those values are only directly comparable across
# hosts if clock skew between hosts is corrected for first. This script
# does a simple two-way ssh round-trip probe per host, over several
# samples, and keeps the lowest-RTT sample (least queuing/jitter) as the
# best offset estimate.
#
# Usage:
#   ./offsets.sh host1 host2 host3 ... > offsets.json
#   ./offsets.sh --samples 11 H1 H2 > offsets.json
#
# Output: a JSON object {"host": offset_ns, ...} where offset_ns is added
# to that host's realtime_ns to align it to THIS machine's clock (the one
# offsets.sh is run from). The local machine itself is the implicit
# reference and is not included in the map (analyze.py treats any host
# missing from the map as offset 0).
#
# Requires: ssh access (key-based; no sudo, no password prompts -- per
# the "one sudo per ssh, never loop sudo" rule this script never invokes
# sudo at all) and python3 on each remote host.
set -euo pipefail

SAMPLES=7
HOSTS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --samples)
            SAMPLES="$2"
            shift 2
            ;;
        -h|--help)
            sed -n '2,27p' "$0"
            exit 0
            ;;
        *)
            HOSTS+=("$1")
            shift
            ;;
    esac
done

if [[ ${#HOSTS[@]} -eq 0 ]]; then
    echo "usage: $0 [--samples N] host1 [host2 ...]" >&2
    exit 1
fi

REMOTE_CLOCK_CMD='python3 -c "import time; print(time.clock_gettime_ns(time.CLOCK_REALTIME))"'

now_ns() {
    python3 -c "import time; print(time.clock_gettime_ns(time.CLOCK_REALTIME))"
}

probe_one() {
    local host="$1"
    local best_rtt=""
    local best_offset=""
    local i t_before remote_ts t_after rtt mid local_mid offset

    for ((i = 0; i < SAMPLES; i++)); do
        t_before=$(now_ns)
        if ! remote_ts=$(ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" "$REMOTE_CLOCK_CMD" 2>/dev/null); then
            continue
        fi
        t_after=$(now_ns)
        [[ "$remote_ts" =~ ^[0-9]+$ ]] || continue

        rtt=$((t_after - t_before))
        mid=$((t_before + rtt / 2))
        offset=$((remote_ts - mid))

        if [[ -z "$best_rtt" || "$rtt" -lt "$best_rtt" ]]; then
            best_rtt="$rtt"
            best_offset="$offset"
        fi
    done

    if [[ -z "$best_offset" ]]; then
        echo "warning: offsets.sh: no successful probe of $host (ssh/py3 failure?)" >&2
        return 1
    fi

    echo "warning: offsets.sh: $host best_rtt=${best_rtt}ns offset=${best_offset}ns" >&2
    printf '%s\n' "$best_offset"
}

entries=()
for host in "${HOSTS[@]}"; do
    if offset=$(probe_one "$host"); then
        entries+=("\"$host\": $offset")
    fi
done

{
    echo "{"
    for i in "${!entries[@]}"; do
        sep=","
        [[ $i -eq $((${#entries[@]} - 1)) ]] && sep=""
        echo "  ${entries[$i]}$sep"
    done
    echo "}"
}
