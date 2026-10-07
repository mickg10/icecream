#!/bin/bash
# Real two-daemon remote compile for `make check` (~30s, unprivileged).
#
# A submitter-side daemon (--no-remote, 0 compile slots) and a separate
# worker daemon register with a private scheduler; one compile is forced
# onto the worker (ICECC_TEST_REMOTEBUILD + ICECC_PREFERRED_HOST) with a
# freshly generated compiler environment.  The test then requires evidence
# from every hop of the remote path, not just an object file -- the client
# builds locally by design when the remote path fails, so an object alone
# proves nothing:
#
#   1. the client's result wait completed and object bytes arrived over the
#      wire, with no local fallback;
#   2. the worker's forked compile child logged a completed remote
#      compilation (written after the descriptor sweep, so it also proves
#      the child kept a working log descriptor);
#   3. the scheduler's JobDone record shows status=0 with a nonzero result
#      payload (out=...) attributed to the worker;
#   4. the object file is nonempty and contains the expected symbol.
#
# With ICECC_TEST_ASSIGNMENT_FENCE_MODE=strict-nonce this is also the
# deletion-sensitive production-path gate for Protocol-50 assignment identity:
# removing the scheduler stamp, submitter-daemon relay, client build_remote_int
# copy, or fulfillment comparison makes the strict worker reject CompileFile.
# The default remains the permanent legacy remote-compile reference.
# This is the regression gate for the compile child's descriptor sweep: a
# sweep that closes the client channel fd makes the compile hang at the
# client's result wait (no FIN is ever sent, because the parent daemon still
# holds its own reference to the socket), which kills checks 1-3.  Because
# the historical failure mode is an INDEFINITE WAIT, every step that could
# hang runs under an explicit timeout: a reintroduced defect produces a
# bounded FAIL with logs, never a wedged test job.
#
# ICECC_TEST_REQUIRE_REMOTE=1 turns every skip condition into a failure --
# the required-CI mode: a suite that is green because this gate silently
# skipped has not tested the remote path at all.  Containerized package
# builds set it (they run as root, where the gate always can run).
set -u

REQUIRE_REMOTE=${ICECC_TEST_REQUIRE_REMOTE:-0}

skip() {
    if [ "$REQUIRE_REMOTE" = 1 ]; then
        echo "FAIL (required mode): $1" >&2
        exit 1
    fi
    echo "SKIP: $1"
    exit 77
}

dir=$(cd "$(dirname "$0")" && pwd)
top=${ICECC_TEST_TOP_BUILDDIR:-$(cd "$dir/.." && pwd)}

# Short socket dir: sun_path is limited to ~107 bytes and distcheck build
# trees exceed it.
sockdir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/iceq.XXXXXX") || exit 99
work=$(mktemp -d "${TMPDIR:-/tmp}/iceremote.XXXXXX") || exit 99

# PID-derived defaults so concurrent runs (or a developer's own scheduler)
# cannot collide on a fixed port.  The pid-derived value is only a SEED:
# another instance of this same formula (the strict row execs THIS script
# and the qa e2e stage runs both rows concurrently under -j2) lands on the
# same port whenever the two pids differ by a multiple of 9000 -- a live
# collision observed as "bind(): Address already in use" in the round-3 e2e
# stage.  Probe-bind from the seed instead: the first free port wins.
pick_seed_port() {  # <seed> -> first bindable port at or after it
    python3 - "$1" <<'PY'
import socket, sys
port = int(sys.argv[1])
for _ in range(9000):
    probe = socket.socket()
    try:
        probe.bind(("", port))
        probe.close()
        print(port)
        raise SystemExit(0)
    except OSError:
        probe.close()
        port += 1
        if port > 29999:
            port = 21000
        continue
raise SystemExit("no free scheduler port in 21000-29999")
PY
}
seed_sched=$((21000 + $$ % 9000))
seed_remote=$((11000 + $$ % 9000))
if [ -z "${ICECC_TEST_SCHED_PORT:-}" ]; then
    SCHED_PORT=$(pick_seed_port "$seed_sched") || fail "no free scheduler port in 21000-29999"
else
    SCHED_PORT=$ICECC_TEST_SCHED_PORT
fi
# The scheduler binds its TEXT endpoint at SCHED_PORT+1 (compilee2e's
# "the scheduler owns TCP base + 1"), so the worker may NEVER be seeded
# there: a probe sees the port free (the scheduler has not bound yet)
# and the worker then retries "Address already in use" for its whole
# startup.  Start the walk at SCHED_PORT+2 and skip +1 by construction.
if [ -z "${ICECC_TEST_REMOTE_PORT:-}" ]; then
    REMOTE_PORT=$(pick_seed_port $((SCHED_PORT + 2))) || fail "no free remote port after the scheduler text endpoint"
else
    REMOTE_PORT=$ICECC_TEST_REMOTE_PORT
fi
# The address the daemons CONNECT to the scheduler on.  The scheduler
# records each daemon by the TCP peer address of that connection and the
# client's UseCS carries it verbatim -- so a strict all-P50 run
# (ICECC_P50_C1F1_REQUIRED) connected via loopback sees hostname
# 127.0.0.1, the protocol sentinel for scheduler-selected LOCAL, and
# correctly refuses.  On a real farm the worker's peer address is its
# real IP and the row works from 127.0.0.1; a container (the qa e2e leg)
# must pass its own eth0 address so the worker's UseCS is a real
# reachable host instead of the local sentinel.
SCHED_ADDR=${ICECC_TEST_SCHED_ADDR:-127.0.0.1}
NETNAME=remoteq$$
ASSIGNMENT_FENCE_MODE=${ICECC_TEST_ASSIGNMENT_FENCE_MODE:-legacy}
case "$ASSIGNMENT_FENCE_MODE" in
    legacy) ;;
    strict-nonce) ;;
    *) echo "FAIL: unsupported assignment fence mode $ASSIGNMENT_FENCE_MODE" >&2; exit 1 ;;
esac

fail() {
    echo "FAIL: $1" >&2
    echo "--- scheduler log:" >&2; tail -30 "$work/sched.log" >&2 || true
    echo "--- worker log:" >&2;   tail -30 "$work/remote.log" >&2 || true
    echo "--- submitter log:" >&2; tail -20 "$work/local.log" >&2 || true
    echo "--- client log:" >&2;   tail -30 "$work/icecc.log" >&2 || true
    exit 1
}

cleanup() {
    cache_children=""
    for parent in ${LOCAL_PID:-} ${REMOTE_PID:-}; do
        children_path="/proc/$parent/task/$parent/children"
        [ -r "$children_path" ] || continue
        for child in $(cat "$children_path" 2>/dev/null); do
            [ "$(readlink "/proc/$child/exe" 2>/dev/null)" = \
                "$top/cache/icecc-cache-service" ] || continue
            cache_children="$cache_children $child"
        done
    done
    owned_children="${LOCAL_PID:-} ${REMOTE_PID:-} ${SCHED_PID:-}"
    for pid in $owned_children; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in $cache_children; do
        [ "$(readlink "/proc/$pid/exe" 2>/dev/null)" = \
            "$top/cache/icecc-cache-service" ] && \
            kill -TERM "$pid" 2>/dev/null || true
    done
    for _ in $(seq 1 50); do
        live=0
        for pid in $owned_children; do
            kill -0 "$pid" 2>/dev/null && live=1
        done
        for pid in $cache_children; do
            if [ "$(readlink "/proc/$pid/exe" 2>/dev/null)" = \
                    "$top/cache/icecc-cache-service" ]; then
                live=1
            fi
        done
        [ "$live" -eq 0 ] && break
        sleep 0.1
    done
    for pid in $owned_children; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    for pid in $cache_children; do
        [ "$(readlink "/proc/$pid/exe" 2>/dev/null)" = \
            "$top/cache/icecc-cache-service" ] && \
            kill -KILL "$pid" 2>/dev/null || true
    done
    for pid in $owned_children; do
        wait "$pid" 2>/dev/null || true
    done
    rm -rf "$sockdir" "$work"
}
trap cleanup EXIT

command -v gcc >/dev/null || skip "gcc not available"

# Root runs select a real account explicitly so the daemon and fixture agree
# on both UID and GID (the fallback numeric group is not portable).
USERFLAG=""
if [ "$(id -u)" != 0 ]; then
    USERFLAG="-u $(whoami)"
fi

# Compiler environment for the worker, generated the way a client would.
mkdir -p "$work/env" "$work/envs-remote" "$work/envs-local"

# As root the services drop to their own unprivileged user BEFORE opening
# logs or installing environments; mktemp's 0700 root-owned directories
# would silently eat both.  /tmp semantics for the shared dirs fix that.
# The env basedirs additionally need to be OWNED by the daemon's compile
# user: with libcap-ng the daemon keeps only CAP_SYS_CHROOT after the
# drop, so cleanup_cache()'s chown on a root-owned basedir is EPERM and
# fatal -- production works because /var/cache/icecream is owned by that
# user, and the test mirrors it.
if [ "$(id -u)" = 0 ]; then
    chmod 1777 "$work" "$sockdir"
    ICEUSER=nobody
    id -u icecc >/dev/null 2>&1 && ICEUSER=icecc
    USERFLAG="-u $ICEUSER"
    ICEGROUP=$(id -g "$ICEUSER")
    chown "$ICEUSER:$ICEGROUP" "$work/envs-remote" "$work/envs-local"
fi

# STRICT_NONCE is the all-P50 row: the scheduler intentionally withholds a
# READY assignment until the selected worker has re-logged with a live cache
# endpoint.  Give both daemons their production sidecars; the submitter's is
# local-only, while the worker's positive advertisement releases READY.
REMOTE_CACHE_ARGS=()
LOCAL_CACHE_ARGS=()
if [ "$ASSIGNMENT_FENCE_MODE" = strict-nonce ]; then
    CACHE_SERVICE="$top/cache/icecc-cache-service"
    [ -x "$CACHE_SERVICE" ] || skip "built cache service is unavailable"
    mkdir -p "$sockdir/cache-remote" "$sockdir/cache-local"
    chmod 0700 "$sockdir/cache-remote" "$sockdir/cache-local"
    if [ "$(id -u)" = 0 ]; then
        chown "$ICEUSER:$ICEGROUP" "$sockdir/cache-remote" "$sockdir/cache-local"
    fi
    REMOTE_CACHE_ARGS=(--cache-service "$CACHE_SERVICE"
                       --cache-runtime-dir "$sockdir/cache-remote")
    LOCAL_CACHE_ARGS=(--cache-service "$CACHE_SERVICE"
                      --cache-runtime-dir "$sockdir/cache-local")
    export ICECC_P50_C1F1_REQUIRED=1
fi
( cd "$work/env" && timeout 120 bash "$top/client/icecc-create-env" "$(command -v gcc)" \
      >"$work/create-env.log" 2>&1 )
ENVTAR=$(ls "$work"/env/*.tar.gz 2>/dev/null | head -1)
[ -n "$ENVTAR" ] || skip "icecc-create-env produced no tarball (see $work/create-env.log)"

if [ "$ASSIGNMENT_FENCE_MODE" = strict-nonce ]; then
    "$top/scheduler/icecc-scheduler" -p "$SCHED_PORT" -n "$NETNAME" \
        --assignment-fence-mode strict-nonce -l "$work/sched.log" -vvv &
else
    "$top/scheduler/icecc-scheduler" -p "$SCHED_PORT" -n "$NETNAME" \
        -l "$work/sched.log" -vvv &
fi
SCHED_PID=$!

ICECC_TEST_SOCKET="$sockdir/remote" \
"$top/daemon/iceccd" -p "$REMOTE_PORT" -m 2 -s "$SCHED_ADDR:$SCHED_PORT" \
    -n "$NETNAME" -N remoteq -b "$work/envs-remote" $USERFLAG \
    "${REMOTE_CACHE_ARGS[@]}" \
    -l "$work/remote.log" -vvv &
REMOTE_PID=$!

ICECC_TEST_SOCKET="$sockdir/local" \
"$top/daemon/iceccd" --no-remote -m 0 -s "$SCHED_ADDR:$SCHED_PORT" \
    -n "$NETNAME" -N localq -b "$work/envs-local" $USERFLAG \
    "${LOCAL_CACHE_ARGS[@]}" \
    -l "$work/local.log" -vvv &
LOCAL_PID=$!

# Both daemons must register before the compile is submitted.  30s: the
# round-5 "never registered" was NOT slow registration -- the worker's own
# listen bind had collided with the scheduler's TEXT endpoint port
# (sched+1; fixed above by seeding the worker walk at sched+2), so the
# worker could not even start accepting while the gate expired.
for _ in $(seq 1 60); do
    logins=$(grep -c "login" "$work/sched.log" 2>/dev/null) || logins=0
    [ "$logins" -ge 2 ] && break
    kill -0 "$SCHED_PID" 2>/dev/null || fail "scheduler died during startup"
    sleep 0.5
done
[ "${logins:-0}" -ge 2 ] || fail "daemons never registered with the scheduler"
if [ "$ASSIGNMENT_FENCE_MODE" = strict-nonce ]; then
    grep -q "assignment fence: strict-nonce" "$work/sched.log" \
        || fail "scheduler did not activate explicit STRICT_NONCE mode"
fi

# Accepting remote jobs needs CAP_SYS_CHROOT (tests/Makefile.am test-prepare
# grants it with setcap; containers running as root have it).  Without it the
# worker downgrades itself to --no-remote and this test cannot run.
if grep -q "Cannot use chroot, no remote jobs accepted." "$work/remote.log"; then
    skip "daemon lacks CAP_SYS_CHROOT (sudo setcap cap_sys_chroot+ep $top/daemon/iceccd, or run in a container as root)"
fi

cat >"$work/tu.c" <<'EOF'
int remote_ice_quick_marker(int x) { return x * 41 + 1; }
EOF

ICECC_TEST_SOCKET="$sockdir/local" \
ICECC_TEST_REMOTEBUILD=1 \
ICECC_PREFERRED_HOST=remoteq \
ICECC_VERSION="$ENVTAR" \
ICECC_DEBUG=debug \
ICECC_LOGFILE="$work/icecc.log" \
timeout 120 "$top/client/icecc" gcc -c "$work/tu.c" -o "$work/tu.o" \
    >"$work/compile.out" 2>&1
rc=$?

[ $rc -eq 124 ] && fail "compile TIMED OUT after 120s -- the remote result path is hanging again"
[ $rc -eq 0 ] || fail "compile exited with $rc"
[ -s "$work/tu.o" ] || fail "object file missing or empty"
nm "$work/tu.o" 2>/dev/null | grep -q remote_ice_quick_marker \
    || fail "object file lacks the expected symbol"

# The client's result wait must have COMPLETED (the closing tag never
# appears when the worker cannot send its result back), the object must have
# arrived over the wire, and no local fallback may have produced it instead.
grep -q "</wait for cs:" "$work/icecc.log" \
    || fail "client never received the compile result (remote result path broken)"
grep -qE "got [0-9]+ bytes" "$work/icecc.log" \
    || fail "client log shows no object bytes received from the worker"
grep -qE "building myself|local build forced" "$work/icecc.log" \
    && fail "client fell back to a local build"

# The worker's compile child must have completed and said so -- this line is
# written by the forked child AFTER the descriptor sweep, so it also proves
# the child kept a working log descriptor.
grep -q "Remote compilation completed with exit code 0" "$work/remote.log" \
    || fail "worker child never logged a completed remote compilation"

sleep 1   # let the worker's JobDone reach the scheduler
grep -qE "END [0-9]+ status=0 .*out=[1-9].* server=remoteq" "$work/sched.log" \
    || fail "scheduler never recorded a successful JobDone from the worker with a result payload"

if [ "$ASSIGNMENT_FENCE_MODE" = strict-nonce ]; then
    echo "OK: strict P50 remote compile crossed scheduler, relay, client, and fulfillment admission"
else
    echo "OK: remote compile on second daemon, result streamed back, JobDone recorded"
fi
exit 0
