#!/bin/sh
# Separate all-P50 STRICT_NONCE row over the capability-aware real remote
# compile harness. The legacy row remains remoteice-quick.sh's default.
#
# The qa e2e stage runs BOTH rows concurrently (-j2).  Each row derives its
# scheduler port from its own pid, and two pids a multiple of 9000 apart
# seed the SAME port; a probe-bind cannot fix that race (both instances
# probe the port free, then race to bind it -- the loser's scheduler dies
# with "bind(): Address already in use").  This row therefore picks its
# ports in a range DISJOINT from the legacy row's (30000+, probed free)
# and exports them before exec: the two rows can never race on a port.
set -eu
export ICECC_TEST_ASSIGNMENT_FENCE_MODE=strict-nonce
pick_port() {
    python3 - "$1" <<'PY'
import socket, sys
port = int(sys.argv[1])
for _ in range(1000):
    probe = socket.socket()
    try:
        probe.bind(("", port))
        probe.close()
        print(port)
        raise SystemExit(0)
    except OSError:
        probe.close()
        port += 1
        if port > 30999:
            port = 30000
        continue
raise SystemExit("no free port in 30000-30999")
PY
}
seed=$((30000 + $$ % 1000))
[ -n "${ICECC_TEST_SCHED_PORT:-}" ] ||     ICECC_TEST_SCHED_PORT=$(pick_port "$seed") || {
        echo "FAIL: no free scheduler port in 30000-30999 for the strict row" >&2
        exit 1
    }
# NOT sched+1: the scheduler binds its TEXT endpoint at sched_port+1
# (compilee2e: "the scheduler owns TCP base + 1"), so a worker seeded
# there collides with it -- the round-5 e2e stage had the worker retrying
# "Failed to bind ... Address already in use" for its whole 30s budget
# while the scheduler silently held the port.  Walk the worker port from
# sched+2: by construction it can never land on the scheduler's text
# endpoint, and the probe still skips any port already held.
[ -n "${ICECC_TEST_REMOTE_PORT:-}" ] ||     ICECC_TEST_REMOTE_PORT=$(pick_port $((ICECC_TEST_SCHED_PORT + 2))) || {
        echo "FAIL: no free remote port for the strict row" >&2
        exit 1
    }
export ICECC_TEST_SCHED_PORT ICECC_TEST_REMOTE_PORT
exec "$(dirname -- "$0")/remoteice-quick.sh" "$@"
