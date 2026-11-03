#!/bin/bash
# Per-invocation P50 off switch (ICECC_P50_MODE in the client wrapper).
#
# ICECC_P50_MODE used to be read only by the daemon (per host).  The client
# wrapper now reads it too, with the SAME rule (services/comm.h,
# p50_cache_mode_allows_p50): unset or exactly "on" allows P50, ANY other
# value -- "off", "bogus", the empty string -- does not.  A wrapper that does
# not allow P50 omits the cache request from its GetCS, which is the canonical
# absence the bounded legacy retry already uses; nothing else changes.
#
# Real client + two real daemons (submitter C, worker F) with armed cache
# sidecars + real scheduler.  P50 is proven from the DAEMON side, not from the
# client's exit code:
#   - worker log:     "P50 CompileFile attached exact ... input for job"
#                     (F handed the compiler the P50 input; ordinary jobs
#                     never log it);
#   - submitter log:  "P50 C-cache control descriptor delivered for
#                     assignment" (C started the cache transfer);
#   - any new "P50" line in either daemon log during an invocation.
# The scheduler's own view is the GetCS it receives: with no cache request it
# can only answer with a cache-less (legacy) assignment, which is what the
# absence of both daemon lines shows.
#
#   1. BASELINE: variable unset -> P50 assignment, remote, byte-identical.
#   2. ICECC_P50_MODE=off on ONE invocation: still remote and successful,
#      byte-identical, zero P50 lines in either daemon log, no P50 lines in
#      the client log, the worker saw an ordinary job.
#   3. The next invocation with the variable unset (and then "on") is P50
#      again: nothing sticks in the daemons.
#   4. Any other value ("bogus", the empty string) behaves as off.  This is
#      the daemon's fail-closed rule, deliberately not "off or else on".
#   5. ICECC_P50_C1F1_REQUIRED=1 + off -> clear client error (Error 22),
#      nonzero exit, no assignment requested, no local build.
#   6. ICECC_REMOTE_REQUIRED=1 + off -> remote, no P50, success.
#
# Mixed-version behaviour (stock 1.4 worker / daemon with the switch off) is
# covered by dev/mixed.py.  The mutation check is a procedure, not a leg: build
# a scratch tree whose client/main.cpp is reverted to the commit before the
# switch, point ICECC_TEST_TOP_BUILDDIR at it, and leg 2 fails (the unpatched
# client still requests P50, so the daemons log P50 activity).
#
# Same harness facts as p50ppfirst-run.sh (do not "simplify" them away):
# - the scheduler records each daemon by the TCP peer address of its
#   registration connection, so the daemons connect on a real non-loopback
#   address;
# - PID-derived scheduler port seeds can collide, so ports are probe-bound
#   (the scheduler needs port and port+1);
# - every step that can hang runs under an explicit timeout.
set -u

fail() {
    echo "p50modeoff: FAIL: $1" >&2
    echo "--- scheduler log:" >&2; head -40 "${work:-}/sched.log" >&2 || true
    echo "--- worker log:" >&2;   tail -40 "${work:-}/remote.log" >&2 || true
    echo "--- submitter log:" >&2; tail -40 "${work:-}/local.log" >&2 || true
    echo "--- live icecc processes:" >&2
    ps -ef 2>/dev/null | grep -E "iceccd|icecc-scheduler|icecc-cache" | grep -v grep >&2 || true
    for f in "${work:-}"/client-*.log; do
        [ -f "$f" ] || continue
        echo "--- client log ($f):" >&2; tail -30 "$f" >&2
    done
    exit 1
}

skip() {
    echo "SKIP: $1" >&2
    exit 77
}

dir=$(cd "$(dirname "$0")" && pwd)
top=${ICECC_TEST_TOP_BUILDDIR:-$(cd "$dir/.." && pwd)}

# The ambient environment must not decide the answer: the daemons read
# ICECC_P50_MODE themselves, and the legs set it per invocation.
unset ICECC_P50_MODE ICECC_P50_C1F1_REQUIRED ICECC_REMOTE_REQUIRED \
      ICECC_P50_PREPROCESSED_CAPTURE

sockdir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/p50off.XXXXXX") || exit 99
work=$(mktemp -d "${TMPDIR:-/tmp}/p50offfarm.XXXXXX") || exit 99

LOCAL_PID=""
REMOTE_PID=""
SCHED_PID=""
CACHE_PIDS=""

kill_owned() {  # <pid...> -- TERM, wait bounded, then KILL
    for pid in "$@"; do
        [ -n "$pid" ] && kill -TERM "$pid" 2>/dev/null || true
    done
    for _ in $(seq 1 50); do
        live=0
        for pid in "$@"; do
            [ -n "$pid" ] || continue
            kill -0 "$pid" 2>/dev/null && live=1
        done
        [ "$live" -eq 0 ] && break
        sleep 0.1
    done
    for pid in "$@"; do
        [ -n "$pid" ] && kill -KILL "$pid" 2>/dev/null || true
    done
}

sweep_sidecars() {  # collect the daemons' cache-service children
    for parent in "${REMOTE_PID:-}" "${LOCAL_PID:-}"; do
        [ -n "$parent" ] || continue
        for child in $(cat "/proc/$parent/task/$parent/children" 2>/dev/null); do
            [ "$(readlink "/proc/$child/exe" 2>/dev/null)" = \
                "$top/cache/icecc-cache-service" ] && CACHE_PIDS="$CACHE_PIDS $child"
        done
    done
}

cleanup() {
    sweep_sidecars
    kill_owned ${REMOTE_PID:-} ${LOCAL_PID:-} ${SCHED_PID:-} $CACHE_PIDS
    wait ${REMOTE_PID:-} 2>/dev/null || true
    wait ${LOCAL_PID:-} 2>/dev/null || true
    wait ${SCHED_PID:-} 2>/dev/null || true
    rm -rf "$sockdir" "$work"
}
trap cleanup EXIT HUP INT TERM

# Probe-bound ports in 40000-44999, disjoint from remoteice-quick (21000-
# 29999), p50ppfirst (31000-39999) and the other e2e bands.  The scheduler
# binds its TCP port AND its text endpoint at port+1, so a seed that only
# probes the base port can hand it a pair whose second half is taken.
pick_port_pair() {  # <seed> -> first P>=seed with P and P+1 both bindable
    python3 - "$1" <<'PY'
import socket, sys
port = int(sys.argv[1])
for _ in range(4000):
    fds = []
    ok = True
    for candidate in (port, port + 1):
        probe = socket.socket()
        try:
            probe.bind(("", candidate))
            fds.append(probe)
        except OSError:
            probe.close()
            ok = False
            break
    for fd in fds:
        fd.close()
    if ok:
        print(port)
        raise SystemExit(0)
    port += 1
    if port > 44998:
        port = 40000
raise SystemExit("no free port pair in 40000-44999")
PY
}

SCHED_ADDR=${ICECC_TEST_SCHED_ADDR:-}
if [ -z "$SCHED_ADDR" ]; then
    SCHED_ADDR=$(hostname -I 2>/dev/null | awk '{print $1}')
fi
case "$SCHED_ADDR" in
""|127.*|0.0.0.0|::1|localhost)
    SCHED_ADDR=$(ip -4 route get 1 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "src") print $(i + 1)}' | head -1)
    ;;
esac
case "$SCHED_ADDR" in
""|127.*|0.0.0.0|::1|localhost)
    skip "no non-loopback IPv4 address for the scheduler registration (set ICECC_TEST_SCHED_ADDR)"
    ;;
esac

command -v clang >/dev/null || skip "clang not available"
command -v timeout >/dev/null || skip "timeout not available"
command -v python3 >/dev/null || skip "python3 not available"
CLANG=$(command -v clang)

for binary in \
    "$top/client/icecc" \
    "$top/daemon/iceccd" \
    "$top/scheduler/icecc-scheduler" \
    "$top/cache/icecc-cache-service"; do
    test -x "$binary" || skip "missing built executable $binary"
done

USERFLAG=""
if [ "$(id -u)" != "0" ]; then
    USERFLAG="-u $(whoami)"
fi
if [ "$(id -u)" = "0" ]; then
    chmod 1777 "$work" "$sockdir"
    ICEUSER=nobody
    id -u icecc >/dev/null 2>&1 && ICEUSER=icecc
    USERFLAG="-u $ICEUSER"
    ICEGROUP=$(id -g "$ICEUSER")
    mkdir -p "$work/envs-remote" "$work/envs-local"
    chown "$ICEUSER:$ICEGROUP" "$work/envs-remote" "$work/envs-local"
fi

mkdir -p "$work/env" "$work/envs-remote" "$work/envs-local" "$work/out"
( cd "$work/env" && timeout 180 bash "$top/client/icecc-create-env" "$CLANG" \
      >"$work/create-env.log" 2>&1 )
ENVTAR=$(ls "$work"/env/*.tar.gz 2>/dev/null | head -1)
[ -n "$ENVTAR" ] || skip "icecc-create-env produced no tarball (see $work/create-env.log)"

timeout_s=${ICECC_P50_MODEOFF_TIMEOUT:-120}

cat >"$work/tu.c" <<'EOF'
static int helper(int x)
{
    return x + 3;
}
int modeoff_marker(int x)
{
    return helper(x) * 41 + 1;
}
EOF

# Scheduler (default enforcing-compat fence: it hands a cache-less assignment
# to a cache-armed worker when the request carries no cache fields), a worker
# daemon and a --no-remote submitter daemon, both with armed cache sidecars.
start_farm() {
    SCHED_PORT=$(pick_port_pair $((40000 + $$ % 4000))) || fail "no free scheduler port pair"
    REMOTE_PORT=$(pick_port_pair $((SCHED_PORT + 2))) || fail "no free remote port"
    NETNAME="p50off$$"
    SUBMIT_SOCKET="$sockdir/local"
    mkdir -p "$sockdir/cache-remote" "$sockdir/cache-local"
    chmod 0700 "$sockdir/cache-remote" "$sockdir/cache-local"
    if [ "$(id -u)" = "0" ]; then
        chown "$ICEUSER:$ICEGROUP" "$sockdir/cache-remote" "$sockdir/cache-local"
    fi
    rm -f "$work/sched.log" "$work/remote.log" "$work/local.log"

    "$top/scheduler/icecc-scheduler" -p "$SCHED_PORT" -n "$NETNAME" \
        -l "$work/sched.log" -vvv &
    SCHED_PID=$!

    ICECC_TEST_SOCKET="$sockdir/remote" \
    "$top/daemon/iceccd" -p "$REMOTE_PORT" -m 2 -s "$SCHED_ADDR:$SCHED_PORT" \
        -n "$NETNAME" -N p50offworker -b "$work/envs-remote" $USERFLAG \
        --cache-service "$top/cache/icecc-cache-service" \
        --cache-runtime-dir "$sockdir/cache-remote" \
        -l "$work/remote.log" -vvv &
    REMOTE_PID=$!

    ICECC_TEST_SOCKET="$SUBMIT_SOCKET" \
    "$top/daemon/iceccd" --no-remote -m 0 -s "$SCHED_ADDR:$SCHED_PORT" \
        -n "$NETNAME" -N p50offlocal -b "$work/envs-local" $USERFLAG \
        --cache-service "$top/cache/icecc-cache-service" \
        --cache-runtime-dir "$sockdir/cache-local" \
        -l "$work/local.log" -vvv &
    LOCAL_PID=$!

    logins=0
    for _ in $(seq 1 90); do
        logins=$(grep -c "login" "$work/sched.log" 2>/dev/null) || logins=0
        [ "$logins" -ge 2 ] && break
        kill -0 "$SCHED_PID" 2>/dev/null || fail "scheduler died during startup"
        sleep 0.5
    done
    [ "$logins" -ge 2 ] || fail "daemons never registered with the scheduler"
    grep -q "Cannot use chroot, no remote jobs accepted." "$work/remote.log" \
        && skip "daemon lacks CAP_SYS_CHROOT (run in a container as root)"

    # Both sidecars must be armed and advertised before the baseline leg,
    # or "P50 on" would silently mean "no cache yet".
    for _ in $(seq 1 120); do
        timeout 10 python3 - "$SCHED_ADDR" "$((SCHED_PORT + 1))" \
            >"$work/listcs.txt" 2>/dev/null <<'PY' || true
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=5)
s.sendall(b"listcs\nquit\n")
out = b""
while True:
    d = s.recv(4096)
    if not d:
        break
    out += d
sys.stdout.write(out.decode(errors="replace"))
PY
        [ "$(grep -c "cache=[0-9]" "$work/listcs.txt" 2>/dev/null)" -ge 1 ] && break
        sleep 0.5
    done
}

stop_farm() {
    sweep_sidecars
    kill_owned ${REMOTE_PID:-} ${LOCAL_PID:-} ${SCHED_PID:-} $CACHE_PIDS
    wait ${REMOTE_PID:-} 2>/dev/null || true
    wait ${LOCAL_PID:-} 2>/dev/null || true
    wait ${SCHED_PID:-} 2>/dev/null || true
    CACHE_PIDS=""
    REMOTE_PID=""
    LOCAL_PID=""
    SCHED_PID=""
}

# run_client <label> <env-assignment-or-"" ...> -- <args...>
# Extra environment applies to this one client process only.  Prints the
# client's exit status.
run_client() {
    label=$1; shift
    extra=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do
        extra+=("$1"); shift
    done
    shift
    log="$work/client-$label.log"
    env -u ICECC_P50_MODE -u ICECC_P50_C1F1_REQUIRED -u ICECC_REMOTE_REQUIRED \
        "${extra[@]}" \
        ICECC_TEST_SOCKET="$SUBMIT_SOCKET" \
        ICECC_TEST_REMOTEBUILD=1 \
        ICECC_PREFERRED_HOST=p50offworker \
        ICECC_VERSION="$ENVTAR" \
        ICECC_DEBUG=debug \
        ICECC_LOGFILE="$log" \
        timeout "$timeout_s" "$top/client/icecc" "$@" \
        >"$work/stdout-$label" 2>"$work/stderr-$label"
    echo "$?"
}

assert_remote_evidence() {  # <label>
    label=$1
    log="$work/client-$label.log"
    grep -q "</wait for cs:" "$log" \
        || fail "client $label never received the compile result (remote result path broken)"
    grep -qE "got [0-9]+ bytes" "$log" \
        || fail "client $label log shows no object bytes received from the worker"
    grep -qE "building myself|local build forced" "$log" \
        && fail "client $label fell back to a local build"
    [ "$(grep -c "Remote compilation completed with exit code 0" "$work/remote.log")" -gt "$remote_done" ] \
        || fail "worker child logged no new completed remote compilation ($label)"
    remote_done=$(grep -c "Remote compilation completed with exit code 0" "$work/remote.log")
}

# The daemon-side P50 witnesses, as byte offsets into the two daemon logs
# taken before an invocation, so each leg looks only at its own lines.
mark_logs() {
    remote_off=$(stat -c %s "$work/remote.log")
    local_off=$(stat -c %s "$work/local.log")
}
new_remote() { tail -c +$((remote_off + 1)) "$work/remote.log"; }
new_local()  { tail -c +$((local_off + 1)) "$work/local.log"; }

# The settle: a daemon log line may trail the client by a moment.
settle() { sleep 1; }

# The scheduler job id the client was assigned; the worker logs "job N" and the
# submitter daemon "assignment N" for it.  Matching by id keeps a PREVIOUS
# leg's late lines (P50 settlement trails the client) out of this leg's verdict.
job_id_of() {  # <label>
    sed -n 's/.*Have to use host .* - Job ID: \([0-9][0-9]*\) .*/\1/p' \
        "$work/client-$1.log" | head -1
}

# Lines that exist only when a job runs the P50 compile path.
P50_PATH_LINES="P50 CompileFile attached exact|P50_INPUT_ATTACH_BEGIN|P50 C-cache control descriptor delivered"

assert_p50_on() {  # <label>
    label=$1
    settle
    id=$(job_id_of "$label")
    [ -n "$id" ] || fail "leg $label: no scheduler job id in the client log"
    grep -qE "P50 CompileFile attached exact .* input for job $id\$" "$work/remote.log" \
        || fail "leg $label: the worker never attached a P50 input for job $id (expected a P50 job)"
    grep -qE "P50 C-cache control descriptor delivered for assignment $id " "$work/local.log" \
        || fail "leg $label: the submitter daemon never delivered a P50 cache control descriptor for assignment $id"
    grep -q "source committed for P50 CompileFile" "$work/client-$label.log" \
        || fail "leg $label: the client never committed a P50 source"
}

assert_p50_off() {  # <label>
    label=$1
    settle
    # Nothing on the P50 path ran during this invocation, in either daemon.
    if new_remote | grep -qE "$P50_PATH_LINES"; then
        new_remote | grep -E "$P50_PATH_LINES" | head -5 >&2
        fail "leg $label: the worker ran the P50 path for an invocation that must not use P50"
    fi
    if new_local | grep -qE "$P50_PATH_LINES"; then
        new_local | grep -E "$P50_PATH_LINES" | head -5 >&2
        fail "leg $label: the submitter daemon ran the P50 path for an invocation that must not use P50"
    fi
    # And no P50 line at all names this job.
    id=$(job_id_of "$label")
    if [ -n "$id" ]; then
        if grep -E "P50.*(job|assignment) $id( |\$)" "$work/remote.log" "$work/local.log" | grep -q .; then
            grep -E "P50.*(job|assignment) $id( |\$)" "$work/remote.log" "$work/local.log" | head -5 >&2
            fail "leg $label: a daemon logged P50 activity for job $id"
        fi
    fi
    if grep -qE "source committed for P50 CompileFile|P50 compiler connection|P50 assignment failed" \
            "$work/client-$label.log"; then
        fail "leg $label: the client log shows P50 activity"
    fi
}

remote_done=0
start_farm

# The baseline farm must be genuinely P50-capable, or every "off" assertion
# below would pass vacuously.
grep -q "cache=[0-9]" "$work/listcs.txt" \
    || { cat "$work/listcs.txt" >&2; fail "no worker advertised a cache endpoint (sidecar not armed)"; }

"$CLANG" -c "$work/tu.c" -o "$work/out/direct.o" || fail "direct local compile failed"
same_object() {  # <label>
    cmp -s "$work/out/$1.o" "$work/out/direct.o" \
        || fail "leg $1: remote object is not byte-identical to the local compile"
}

echo "p50modeoff: leg 1 -- baseline, variable unset"
mark_logs
rc=$(run_client base -- "$CLANG" -c "$work/tu.c" -o "$work/out/base.o")
[ "$rc" -eq 0 ] || fail "baseline compile exited with $rc"
assert_remote_evidence base
assert_p50_on base
same_object base

echo "p50modeoff: leg 2 -- ICECC_P50_MODE=off on one invocation"
mark_logs
rc=$(run_client off ICECC_P50_MODE=off -- "$CLANG" -c "$work/tu.c" -o "$work/out/off.o")
[ "$rc" -eq 0 ] || fail "ICECC_P50_MODE=off compile exited with $rc"
assert_remote_evidence off
assert_p50_off off
same_object off
grep -q "ICECC_P50_MODE=off: not requesting P50 for this invocation" "$work/client-off.log" \
    || fail "leg off: the client did not log that it skipped P50"

echo "p50modeoff: leg 3 -- the next invocations are P50 again"
mark_logs
rc=$(run_client after-unset -- "$CLANG" -c "$work/tu.c" -o "$work/out/after-unset.o")
[ "$rc" -eq 0 ] || fail "post-off unset compile exited with $rc"
assert_remote_evidence after-unset
assert_p50_on after-unset
same_object after-unset
mark_logs
rc=$(run_client after-on ICECC_P50_MODE=on -- "$CLANG" -c "$work/tu.c" -o "$work/out/after-on.o")
[ "$rc" -eq 0 ] || fail "post-off ICECC_P50_MODE=on compile exited with $rc"
assert_remote_evidence after-on
assert_p50_on after-on
same_object after-on

echo "p50modeoff: leg 4 -- any other value is off (fail closed, as in the daemon)"
for v in bogus ON Off; do
    mark_logs
    rc=$(run_client "val-$v" "ICECC_P50_MODE=$v" -- "$CLANG" -c "$work/tu.c" -o "$work/out/val-$v.o")
    [ "$rc" -eq 0 ] || fail "ICECC_P50_MODE=$v compile exited with $rc"
    assert_remote_evidence "val-$v"
    assert_p50_off "val-$v"
    same_object "val-$v"
done
mark_logs
rc=$(run_client val-empty "ICECC_P50_MODE=" -- "$CLANG" -c "$work/tu.c" -o "$work/out/val-empty.o")
[ "$rc" -eq 0 ] || fail "ICECC_P50_MODE= (empty) compile exited with $rc"
assert_remote_evidence val-empty
assert_p50_off val-empty
same_object val-empty

echo "p50modeoff: leg 5 -- ICECC_P50_C1F1_REQUIRED=1 with the switch off is a client error"
mark_logs
rm -f "$work/out/strict.o"
rc=$(run_client strict ICECC_P50_C1F1_REQUIRED=1 ICECC_P50_MODE=off -- "$CLANG" -c "$work/tu.c" -o "$work/out/strict.o")
[ "$rc" -ne 0 ] || fail "strict P50 with ICECC_P50_MODE=off unexpectedly succeeded"
[ "$rc" -ne 124 ] || fail "strict P50 with ICECC_P50_MODE=off TIMED OUT"
grep -q "Error 22 - ICECC_P50_C1F1_REQUIRED conflicts with ICECC_P50_MODE=off" "$work/client-strict.log" \
    || fail "strict conflict did not log the Error 22 message"
# Without a log file the same error is what the user sees on stderr.
env -u ICECC_LOGFILE -u ICECC_DEBUG ICECC_P50_C1F1_REQUIRED=1 ICECC_P50_MODE=off \
    ICECC_TEST_SOCKET="$SUBMIT_SOCKET" ICECC_TEST_REMOTEBUILD=1 \
    ICECC_PREFERRED_HOST=p50offworker ICECC_VERSION="$ENVTAR" \
    timeout "$timeout_s" "$top/client/icecc" "$CLANG" -c "$work/tu.c" -o "$work/out/strict2.o" \
    >/dev/null 2>"$work/stderr-strict2"
rc2=$?
[ "$rc2" -ne 0 ] && [ "$rc2" -ne 124 ] || fail "strict conflict (stderr run) exited with $rc2"
grep -q "Error 22 - ICECC_P50_C1F1_REQUIRED conflicts with ICECC_P50_MODE=off" "$work/stderr-strict2" \
    || { cat "$work/stderr-strict2" >&2; fail "strict conflict did not print the Error 22 message on stderr"; }
[ ! -e "$work/out/strict.o" ] || fail "strict conflict produced an object (a local fallback ran)"
grep -q "</wait for cs:" "$work/client-strict.log" \
    && fail "strict conflict still asked the scheduler for an assignment"
assert_p50_off strict

echo "p50modeoff: leg 6 -- ICECC_REMOTE_REQUIRED=1 with the switch off still builds remotely"
mark_logs
rc=$(run_client remote-required ICECC_REMOTE_REQUIRED=1 ICECC_P50_MODE=off -- "$CLANG" -c "$work/tu.c" -o "$work/out/remote-required.o")
[ "$rc" -eq 0 ] || fail "ICECC_REMOTE_REQUIRED=1 with off exited with $rc"
assert_remote_evidence remote-required
assert_p50_off remote-required
same_object remote-required

echo "p50modeoff: leg 3b -- and the invocation after all of that is P50 again"
mark_logs
rc=$(run_client final -- "$CLANG" -c "$work/tu.c" -o "$work/out/final.o")
[ "$rc" -eq 0 ] || fail "final unset compile exited with $rc"
assert_remote_evidence final
assert_p50_on final
same_object final

stop_farm

echo "OK: p50modeoff per-invocation P50 off switch (legs 1-6) passed"
exit 0
