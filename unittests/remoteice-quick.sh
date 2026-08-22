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
# cannot collide on a fixed port.
SCHED_PORT=${ICECC_TEST_SCHED_PORT:-$((21000 + $$ % 9000))}
REMOTE_PORT=${ICECC_TEST_REMOTE_PORT:-$((11000 + $$ % 9000))}
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
    [ -n "${LOCAL_PID:-}" ] && kill "$LOCAL_PID" 2>/dev/null
    [ -n "${REMOTE_PID:-}" ] && kill "$REMOTE_PID" 2>/dev/null
    [ -n "${SCHED_PID:-}" ] && kill "$SCHED_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$sockdir" "$work"
}
trap cleanup EXIT

command -v gcc >/dev/null || skip "gcc not available"

# As root (containers, rpm %check) iceccd refuses -u root and instead picks
# its own unprivileged compile user (icecc, else nobody) -- the production
# path.  Unprivileged runs keep -u so the daemon accepts the invoking user.
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
    chown "$ICEUSER" "$work/envs-remote" "$work/envs-local"
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
"$top/daemon/iceccd" -p "$REMOTE_PORT" -m 2 -s "127.0.0.1:$SCHED_PORT" \
    -n "$NETNAME" -N remoteq -b "$work/envs-remote" $USERFLAG \
    -l "$work/remote.log" -vvv &
REMOTE_PID=$!

ICECC_TEST_SOCKET="$sockdir/local" \
"$top/daemon/iceccd" --no-remote -m 0 -s "127.0.0.1:$SCHED_PORT" \
    -n "$NETNAME" -N localq -b "$work/envs-local" $USERFLAG \
    -l "$work/local.log" -vvv &
LOCAL_PID=$!

# Both daemons must register before the compile is submitted.
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
