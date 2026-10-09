#!/bin/bash
# ICECC_P50_PREPROCESS_FIRST integration gate (real two-daemon remote farm).
#
# The mode moves the local preprocessor BEFORE the GetCS assignment request
# in build_remote()'s ordinary torepeat==1 branch, so a remote slot is only
# taken once the preprocessed source is ready.  This gate proves, on the real
# client + daemons + scheduler (no fake peer):
#
#   1. IDENTITY: the preprocessed bytes the client hands to the P50 cache
#      transfer with the flag ON are byte-identical to the ones today's
#      in-assignment path produces with the flag OFF.  The S7 capture seam
#      (ICECC_P50_PREPROCESSED_CAPTURE) is the observation point: the strict
#      all-P50 row runs the same TU twice (OFF, then ON) with two capture
#      destinations and cmp's them; both captures are written by
#      retain_p50_preprocessed_capture() from the same call_cpp(job, fd)
#      output the transfer itself reads.
#   2. END-TO-END LEGACY: with the flag ON and no cache advertisement, the
#      pre-captured file is what build_remote_int()'s legacy branch sends
#      (input_sink.send_fd instead of a second call_cpp): the remote object
#      is byte-identical to a direct local compile and every remote-path
#      witness is present.
#   3. CPP-FAILURE PARITY: a TU with a syntax error exits with the same
#      status and the same stderr with the flag ON (failure before GetCS)
#      as with the flag OFF (today's failure inside build_remote_int).
#   4. LOCAL SELECTION: a scheduler-local assignment (no remote worker)
#      with the flag ON still builds locally from the original source; the
#      pre-capture is exercised and dropped harmlessly.
#
# clang-only by design: the production gate for this mode is the farm's
# -frewrite-includes toolchain, and the gcc torepeat>1 verification path
# stays untouched.
#
# Same harness facts as remoteice-quick.sh (do not "simplify" them away):
# - the scheduler records each daemon by the TCP peer address of its
#   registration connection, so the daemons connect on a real non-loopback
#   address (a loopback connect registers as the 127.0.0.1 local sentinel
#   and the strict row would refuse its own worker);
# - PID-derived scheduler port seeds can collide, so ports are probe-bound;
# - every step that can hang runs under an explicit timeout.
set -u

fail() {
    echo "p50ppfirst: FAIL: $1" >&2
    echo "--- scheduler log:" >&2; head -40 "${work:-}/sched.log" >&2 || true
    echo "--- worker log:" >&2;   head -40 "${work:-}/remote.log" >&2 || true
    echo "--- submitter log:" >&2; head -20 "${work:-}/local.log" >&2 || true
    echo "--- live icecc processes:" >&2
    ps -ef 2>/dev/null | grep -E "iceccd|icecc-scheduler|icecc-cache" | grep -v grep >&2 || true
    for f in "${work:-}"/client-*.log; do
        [ -f "$f" ] || continue
        echo "--- client log ($f):" >&2; tail -40 "$f" >&2
    done
    exit 1
}

skip() {
    echo "SKIP: $1" >&2
    exit 77
}

dir=$(cd "$(dirname "$0")" && pwd)
top=${ICECC_TEST_TOP_BUILDDIR:-$(cd "$dir/.." && pwd)}

sockdir=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/ppf.XXXXXX") || exit 99
work=$(mktemp -d "${TMPDIR:-/tmp}/ppfarm.XXXXXX") || exit 99

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
raise SystemExit("no free port in 21000-29999")
PY
}

# The scheduler records each daemon by the TCP peer address of its
# connection; the strict row's worker must register through an ordinary
# address or its own UseCS aliases the 127.0.0.1 local sentinel.
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

# Root runs select a real account explicitly so the daemon and fixture agree
# on both UID and GID; mktemp's 0700 root-owned directories would otherwise
# silently eat both (same as remoteice-quick.sh).
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

timeout_s=${ICECC_P50_PPFIRST_TIMEOUT:-120}

cat >"$work/tu.c" <<'EOF'
static int helper(int x)
{
    return x + 3;
}
int ppfirst_marker(int x)
{
    return helper(x) * 41 + 1;
}
EOF

mkdir -p "$work/inc"
cat >"$work/inc/ppfirst_inc.h" <<'EOF'
#define PPFIRST_SCALE 7
static inline int ppfirst_inline(int x)
{
    return x * PPFIRST_SCALE;
}
EOF
cat >"$work/tu2.c" <<'EOF'
#include "ppfirst_inc.h"
int ppsecond_marker(int x)
{
    return ppfirst_inline(x) + 2;
}
EOF

cat >"$work/syntaxerr.c" <<'EOF'
int ppfirst_broken(int x)
{
    return x :+ 1;
}
EOF

start_farm() {  # <fence-mode> (daemons + scheduler into the trap-owned pids)
    mode=$1
    SCHED_PORT=$(pick_seed_port $((21000 + $$ % 9000))) || fail "no free scheduler port"
    REMOTE_PORT=$(pick_seed_port $((SCHED_PORT + 2))) || fail "no free remote port"
    NETNAME="ppf$$-$mode"
    SUBMIT_SOCKET="$sockdir/local-$mode"
    CACHE_ARGS_REMOTE=()
    CACHE_ARGS_LOCAL=()
    if [ "$mode" = strict-nonce ]; then
        test -x "$top/cache/icecc-cache-service" || fail "cache service missing"
        mkdir -p "$sockdir/cache-remote-$mode" "$sockdir/cache-local-$mode"
        chmod 0700 "$sockdir/cache-remote-$mode" "$sockdir/cache-local-$mode"
        if [ "$(id -u)" = "0" ]; then
            chown "$ICEUSER:$ICEGROUP" "$sockdir/cache-remote-$mode" "$sockdir/cache-local-$mode"
        fi
        CACHE_ARGS_REMOTE=(--cache-service "$top/cache/icecc-cache-service"
                           --cache-runtime-dir "$sockdir/cache-remote-$mode")
        CACHE_ARGS_LOCAL=(--cache-service "$top/cache/icecc-cache-service"
                          --cache-runtime-dir "$sockdir/cache-local-$mode")
    fi

    # The farm members run as the (possibly dropped) unprivileged user:
    # a root-owned pre-created log file would be unwritable for them and
    # every diagnostic would silently vanish.  Remove instead of truncate.
    rm -f "$work/sched.log" "$work/remote.log" "$work/local.log"

    "$top/scheduler/icecc-scheduler" -p "$SCHED_PORT" -n "$NETNAME" \
        --assignment-fence-mode "$mode" -l "$work/sched.log" -vvv &
    SCHED_PID=$!

    ICECC_TEST_SOCKET="$sockdir/remote-$mode" \
    "$top/daemon/iceccd" -p "$REMOTE_PORT" -m 2 -s "$SCHED_ADDR:$SCHED_PORT" \
        -n "$NETNAME" -N ppfworker -b "$work/envs-remote" $USERFLAG \
        "${CACHE_ARGS_REMOTE[@]}" \
        -l "$work/remote.log" -vvv &
    REMOTE_PID=$!

    ICECC_TEST_SOCKET="$SUBMIT_SOCKET" \
    "$top/daemon/iceccd" --no-remote -m 0 -s "$SCHED_ADDR:$SCHED_PORT" \
        -n "$NETNAME" -N ppflocal -b "$work/envs-local" $USERFLAG \
        "${CACHE_ARGS_LOCAL[@]}" \
        -l "$work/local.log" -vvv &
    LOCAL_PID=$!

    if [ "$mode" = strict-nonce ]; then
        export ICECC_P50_C1F1_REQUIRED=1
    else
        unset ICECC_P50_C1F1_REQUIRED
    fi

    # Both daemons must register before any compile is submitted.
    logins=0
    for _ in $(seq 1 90); do
        logins=$(grep -c "login" "$work/sched.log" 2>/dev/null) || logins=0
        [ "$logins" -ge 2 ] && break
        kill -0 "$SCHED_PID" 2>/dev/null || fail "scheduler died during startup ($mode)"
        sleep 0.5
    done
    [ "$logins" -ge 2 ] || fail "daemons never registered with the scheduler ($mode)"
    if [ "$mode" = strict-nonce ]; then
        grep -q "assignment fence: strict-nonce" "$work/sched.log" \
            || fail "scheduler did not activate explicit STRICT_NONCE mode"
    fi
    grep -q "Cannot use chroot, no remote jobs accepted." "$work/remote.log" \
        && skip "daemon lacks CAP_SYS_CHROOT (run in a container as root)"
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
    unset ICECC_P50_C1F1_REQUIRED
}

# run_client <label> <out.o> <capture-dest-or-""> <flag 0|1> <args...>
# Prints the client's exit status.  ICECC_P50_PREPROCESS_FIRST and
# ICECC_P50_PREPROCESSED_CAPTURE are set only for the client process.
run_client() {
    label=$1; out=$2; capture=$3; ppfirst=$4; shift 4
    log="$work/client-$label.log"
    envvars=()
    if [ -n "$capture" ]; then
        rm -f "$capture"
        envvars+=("ICECC_P50_PREPROCESSED_CAPTURE=$capture")
    fi
    envvars+=("ICECC_P50_PREPROCESS_FIRST=$ppfirst")
    env -u ICECC_P50_PREPROCESSED_CAPTURE -u ICECC_P50_PREPROCESS_FIRST \
        "${envvars[@]}" \
        ICECC_TEST_SOCKET="$SUBMIT_SOCKET" \
        ICECC_TEST_REMOTEBUILD=1 \
        ICECC_PREFERRED_HOST=ppfworker \
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
    grep -q "Remote compilation completed with exit code 0" "$work/remote.log" \
        || fail "worker child never logged a completed remote compilation ($label)"
}

assert_ppfirst_ran() {  # <label>
    grep -q "preprocess-first: preprocessed source ready before assignment request" \
        "$work/client-$1.log" \
        || fail "flag-ON leg $1 did not run the preprocess-first capture"
}

assert_ppfirst_absent() {  # <label>
    grep -q "preprocess-first: preprocessed source ready before assignment request" \
        "$work/client-$1.log" \
        && fail "leg $1 unexpectedly ran the preprocess-first capture"
}

echo "p50ppfirst: legacy row (pre-captured source over the legacy wire)"

start_farm legacy

rc_a=$(run_client legacy-on-a "$work/out/legacy-on-a.o" "" 1 "$CLANG" -c "$work/tu.c" -o "$work/out/legacy-on-a.o")
[ "$rc_a" -eq 0 ] || fail "legacy flag-ON compile of tu.c exited with $rc_a"
assert_remote_evidence legacy-on-a
assert_ppfirst_ran legacy-on-a

rc_b=$(run_client legacy-on-b "$work/out/legacy-on-b.o" "" 1 "$CLANG" -c "$work/tu2.c" -I"$work/inc" -o "$work/out/legacy-on-b.o")
[ "$rc_b" -eq 0 ] || fail "legacy flag-ON compile of tu2.c exited with $rc_b"
assert_remote_evidence legacy-on-b
assert_ppfirst_ran legacy-on-b

"$CLANG" -c "$work/tu.c" -o "$work/out/local-a.o"
cmp -s "$work/out/legacy-on-a.o" "$work/out/local-a.o" \
    || fail "legacy flag-ON remote object of tu.c is not byte-identical to the local compile"
"$CLANG" -c "$work/tu2.c" -I"$work/inc" -o "$work/out/local-b.o"
cmp -s "$work/out/legacy-on-b.o" "$work/out/local-b.o" \
    || fail "legacy flag-ON remote object of tu2.c is not byte-identical to the local compile"

# A gcc-shaped job with the flag ON must stay exactly on today's path: the
# clang-only gate keeps the mode out of it entirely.
rc_g=$(run_client legacy-gcc-gate "$work/out/legacy-gate.o" "" 1 gcc -c "$work/tu.c" -o "$work/out/legacy-gate.o")
[ "$rc_g" -eq 0 ] || fail "gcc compile under flag-ON exited with $rc_g"
assert_ppfirst_absent legacy-gcc-gate
assert_remote_evidence legacy-gcc-gate
cmp -s "$work/out/legacy-gate.o" "$work/out/local-a.o" \
    || fail "gcc remote object is not byte-identical to the local compile"

# cpp-failure parity on the legacy row: same exit status, same stderr.
rc_off=$(run_client legacy-fail-off "" "" 0 "$CLANG" -c "$work/syntaxerr.c" -o "$work/out/fail-off.o")
rc_on=$(run_client legacy-fail-on "" "" 1 "$CLANG" -c "$work/syntaxerr.c" -o "$work/out/fail-on.o")
[ "$rc_off" -eq "$rc_on" ] \
    || fail "cpp-failure exit status differs: flag-OFF=$rc_off flag-ON=$rc_on"
[ "$rc_off" -ne 0 ] || fail "the syntax-error TU unexpectedly succeeded"
cmp -s "$work/stderr-legacy-fail-off" "$work/stderr-legacy-fail-on" \
    || { diff -u "$work/stderr-legacy-fail-off" "$work/stderr-legacy-fail-on" >&2 || true
         fail "cpp-failure stderr differs between flag OFF and flag ON"; }
test -s "$work/stderr-legacy-fail-on" \
    || fail "the flag-ON cpp failure did not surface the compiler diagnostic on stderr"

stop_farm

echo "p50ppfirst: scheduler-local row (pre-capture dropped on a local assignment)"

# Only the --no-remote submitter daemon: the scheduler has no remote worker,
# so the UseCS selects the localhost fast path (maybe_build_local) and the
# flag-ON pre-capture must be dropped harmlessly.
SCHED_PORT=$(pick_seed_port $((21000 + $$ % 9000))) || fail "no free scheduler port"
NETNAME="ppflocalonly$$"
rm -f "$work/sched.log" "$work/remote.log" "$work/local.log"
"$top/scheduler/icecc-scheduler" -p "$SCHED_PORT" -n "$NETNAME" \
    --assignment-fence-mode legacy -l "$work/sched.log" -vvv &
SCHED_PID=$!
ICECC_TEST_SOCKET="$sockdir/local-only" \
"$top/daemon/iceccd" --no-remote -m 0 -s "$SCHED_ADDR:$SCHED_PORT" \
    -n "$NETNAME" -N ppflocalonly -b "$work/envs-local" $USERFLAG \
    -l "$work/local.log" -vvv &
LOCAL_PID=$!
unset ICECC_P50_C1F1_REQUIRED
SUBMIT_SOCKET="$sockdir/local-only"
logins=0
for _ in $(seq 1 60); do
    logins=$(grep -c "login" "$work/sched.log" 2>/dev/null) || logins=0
    [ "$logins" -ge 1 ] && break
    kill -0 "$SCHED_PID" 2>/dev/null || fail "scheduler died during startup (local row)"
    sleep 0.5
done
[ "$logins" -ge 1 ] || fail "local daemon never registered (local row)"

env -u ICECC_P50_PREPROCESSED_CAPTURE ICECC_P50_PREPROCESS_FIRST=1 \
    ICECC_TEST_SOCKET="$SUBMIT_SOCKET" \
    ICECC_VERSION="$ENVTAR" \
    ICECC_DEBUG=debug ICECC_LOGFILE="$work/client-local-row.log" \
    timeout "$timeout_s" "$top/client/icecc" "$CLANG" -c "$work/tu.c" -o "$work/out/local-row.o" \
    >"$work/stdout-local-row" 2>"$work/stderr-local-row"
rc_local=$?
[ "$rc_local" -eq 124 ] && fail "local-row client TIMED OUT"
[ "$rc_local" -eq 0 ] || fail "scheduler-local flag-ON compile exited with $rc_local"
[ -s "$work/out/local-row.o" ] || fail "scheduler-local flag-ON object missing"
grep -q "preprocess-first: preprocessed source ready before assignment request" \
    "$work/client-local-row.log" \
    || fail "scheduler-local row did not run the preprocess-first capture"

stop_farm

echo "p50ppfirst: strict all-P50 identity row (capture seam, OFF vs ON)"

start_farm strict-nonce

capture_off="$work/capture-off.ii"
capture_on="$work/capture-on.ii"

rc_off=$(run_client strict-off "$work/out/strict-off.o" "$capture_off" 0 "$CLANG" -c "$work/tu.c" -o "$work/out/strict-off.o")
[ "$rc_off" -eq 0 ] || fail "strict flag-OFF compile exited with $rc_off"
assert_remote_evidence strict-off
assert_ppfirst_absent strict-off
grep -q "source committed for P50 CompileFile" "$work/client-strict-off.log" \
    || fail "strict flag-OFF run never committed the P50 source"
[ -s "$capture_off" ] || fail "strict flag-OFF preprocessed capture is missing"

rc_on=$(run_client strict-on "$work/out/strict-on.o" "$capture_on" 1 "$CLANG" -c "$work/tu.c" -o "$work/out/strict-on.o")
[ "$rc_on" -eq 0 ] || fail "strict flag-ON compile exited with $rc_on"
assert_remote_evidence strict-on
assert_ppfirst_ran strict-on
grep -q "source committed for P50 CompileFile" "$work/client-strict-on.log" \
    || fail "strict flag-ON run never committed the P50 source"
[ -s "$capture_on" ] || fail "strict flag-ON preprocessed capture is missing"

cmp -s "$capture_off" "$capture_on" \
    || { diff <(head -c 400 "$capture_off") <(head -c 400 "$capture_on") >&2 || true
         fail "preprocessed bytes differ between preprocess-first OFF and ON"; }

"$CLANG" -c "$work/tu.c" -o "$work/out/local-strict.o"
cmp -s "$work/out/strict-on.o" "$work/out/local-strict.o" \
    || fail "strict flag-ON remote object is not byte-identical to the local compile"

stop_farm

echo "OK: p50ppfirst identity, legacy end-to-end, cpp-failure parity, and local selection all passed"
exit 0
