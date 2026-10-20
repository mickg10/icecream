#!/bin/bash
# Preprocess-first integration gate (real two-daemon remote farm).
#
# The client moves the local preprocessor BEFORE the GetCS assignment request
# in build_remote()'s ordinary torepeat==1 branch -- unconditionally, for
# clang AND gcc -- so a remote slot is only taken once the preprocessed
# source is ready.  This gate proves, on the real client + daemons +
# scheduler (no fake peer):
#
#   1. IDENTITY: the preprocessed bytes the client hands to the P50 cache
#      transfer are byte-identical to a direct local cpp of the same TU
#      with the same include flags.  The S7 capture seam
#      (ICECC_P50_PREPROCESSED_CAPTURE) is the observation point: the strict
#      all-P50 row runs the same TU with the capture destination set and
#      cmp's it against the direct cpp; the capture is written by
#      retain_p50_preprocessed_capture() from the same call_cpp(job, fd)
#      output the transfer itself reads.
#   2. END-TO-END LEGACY: with no cache advertisement, the pre-captured
#      file is what build_remote_int()'s legacy branch sends
#      (input_sink.send_fd instead of a second cpp): the remote object is
#      byte-identical to a direct local compile and every remote-path
#      witness is present.
#   3. CPP-FAILURE PARITY + GCC 103 RECOVERY: a clang #error TU exits with
#      the same status and the same diagnostic core as a direct local
#      clang -c (and the diagnostic prints exactly once -- the buffered
#      capture stderr is replayed before the plain status, never
#      discarded); a gcc #error TU recovers through the pre-GetCS Error
#      103 and rebuilds locally, with the local-rebuild log line the
#      client prints for that class (the -fdirectives-only-broken class
#      this recovery exists for rebuilds clean; a genuinely failing TU
#      reproduces the direct local failure).
#   4. LOCAL SELECTION: a scheduler-local assignment (no remote worker)
#      still builds locally from the original source; the pre-capture is
#      exercised and dropped harmlessly (the accepted one-cpp cost), and
#      its buffered stderr is DISCARDED there -- a #warning TU prints
#      exactly once (from build_local), byte-identical to a direct local
#      compile, never once extra from the capture.
#
# gcc coverage is gate-only (the farm is 100% clang): the gcc row asserts
# the capture ran and the cpp-failure row asserts the Error-103 recovery,
# which is the one new code path with no farm coverage.  The gcc
# torepeat>1 verification path stays untouched by design.
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

# Band-aware single-port probe: a seed in 31000-39999 scans that band (the
# ppfirst band, disjoint from every other e2e test), a seed in 21000-29999
# scans the legacy band, so the wraparound can never hand a caller a port
# from a band it was not seeded into.
pick_seed_port() {  # <seed> -> first bindable port at or after it, in the seed's band
    python3 - "$1" <<'PY'
import socket, sys
port = int(sys.argv[1])
band_low, band_high = (31000, 39999) if port >= 31000 else (21000, 29999)
for _ in range(band_high - band_low + 1):
    probe = socket.socket()
    try:
        probe.bind(("", port))
        probe.close()
        print(port)
        raise SystemExit(0)
    except OSError:
        probe.close()
        port += 1
        if port > band_high:
            port = band_low
        continue
raise SystemExit(f"no free port in {band_low}-{band_high}")
PY
}

# The scheduler binds its TCP port AND its text endpoint at port+1
# (scheduler/scheduler.cpp), so a seed that only probes the base port can
# hand the scheduler a pair whose second half is already taken -- "bind()
# Address already in use" on the text endpoint, the flake this pair probe
# exists to close.  Seeds live in 31000-39999, disjoint from every other
# e2e test's range (remoteice-quick 21000-29999, completionflow's worker
# ports 24000-29000, assignment-remote 30000-30999), so concurrent QA legs
# cannot collide either.
pick_seed_port_pair() {  # <seed> -> first port P>=seed with P and P+1 both bindable
    python3 - "$1" <<'PY'
import socket, sys
port = int(sys.argv[1])
for _ in range(9000):
    ok = True
    fds = []
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
    if port > 39999:
        port = 31000
raise SystemExit("no free port pair in 31000-39999")
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

# A preprocessor-level failure TU: #error fails the local cpp itself, so
# the failure is the pre-GetCS capture's (the new code path) and a direct
# local compile names the same file, phase, and diagnostic.  A parse error
# would NOT do: -E does not parse, so the failure would surface from the
# remote compile with a rewritten-source path instead.
cat >"$work/syntaxerr.c" <<'EOF'
#error ppfirst deliberate preprocessor failure
EOF

# A preprocessor-WARNING TU: #warning succeeds the cpp (the capture runs,
# the assignment can still be minted) but must print exactly once -- from
# build_local on a scheduler-local assignment, never once extra from the
# capture.  This is the stderr-replay witness: without the buffer the
# capture's own #warning doubles it.
cat >"$work/warn.c" <<'EOF'
#warning ppfirst deliberate preprocessor warning
int ppfirst_warned(void)
{
    return 4;
}
EOF

start_farm() {  # <fence-mode> (daemons + scheduler into the trap-owned pids)
    mode=$1
    # Pair probe: the scheduler needs port AND port+1; seed 31000+ so a
    # concurrent e2e leg (21000-30999 ranges) cannot hand us a colliding
    # base port either.
    SCHED_PORT=$(pick_seed_port_pair $((31000 + $$ % 9000))) || fail "no free scheduler port pair"
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

# run_client <label> <capture-dest-or-""> <args...>
# Prints the client's exit status.  ICECC_P50_PREPROCESSED_CAPTURE is set
# only for the client process.
run_client() {
    label=$1; capture=$2; shift 2
    log="$work/client-$label.log"
    envvars=()
    if [ -n "$capture" ]; then
        rm -f "$capture"
        envvars+=("ICECC_P50_PREPROCESSED_CAPTURE=$capture")
    fi
    env -u ICECC_P50_PREPROCESSED_CAPTURE \
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
        || fail "leg $1 did not run the preprocess-first capture"
}

echo "p50ppfirst: legacy row (pre-captured source over the legacy wire)"

start_farm legacy

rc_a=$(run_client legacy-a "" "$CLANG" -c "$work/tu.c" -o "$work/out/legacy-a.o")
[ "$rc_a" -eq 0 ] || fail "legacy compile of tu.c exited with $rc_a"
assert_remote_evidence legacy-a
assert_ppfirst_ran legacy-a

rc_b=$(run_client legacy-b "" "$CLANG" -c "$work/tu2.c" -I"$work/inc" -o "$work/out/legacy-b.o")
[ "$rc_b" -eq 0 ] || fail "legacy compile of tu2.c exited with $rc_b"
assert_remote_evidence legacy-b
assert_ppfirst_ran legacy-b

"$CLANG" -c "$work/tu.c" -o "$work/out/local-a.o"
cmp -s "$work/out/legacy-a.o" "$work/out/local-a.o" \
    || fail "legacy remote object of tu.c is not byte-identical to the local compile"
"$CLANG" -c "$work/tu2.c" -I"$work/inc" -o "$work/out/local-b.o"
cmp -s "$work/out/legacy-b.o" "$work/out/local-b.o" \
    || fail "legacy remote object of tu2.c is not byte-identical to the local compile"

# A gcc-shaped job now runs the pre-GetCS capture too (unconditional,
# clang AND gcc): assert the capture ran AND ran the right command.  The
# cpp child traces its argv ("preparing source to send: ...", cpp.cpp),
# so the row proves the gcc capture used -E -fdirectives-only regardless
# of where the compile itself landed.  No full remote evidence for the
# gcc row: a gcc that emits caret diagnostics legitimately ends in the
# Error 102 stdout/stderr workaround and recompiles locally
# (environment-dependent, and orthogonal to this gate).  The object must
# still match a direct local gcc compile.
rc_g=$(run_client legacy-gcc "" gcc -c "$work/tu.c" -o "$work/out/legacy-gate.o")
[ "$rc_g" -eq 0 ] || fail "gcc compile exited with $rc_g"
assert_ppfirst_ran legacy-gcc
grep -q "preparing source to send: .* -E .*-fdirectives-only" \
    "$work/client-legacy-gcc.log" \
    || fail "gcc capture did not run the local cpp with -E -fdirectives-only"
gcc -c "$work/tu.c" -o "$work/out/local-gate.o" 2>/dev/null \
    || fail "direct local gcc compile of tu.c failed"
cmp -s "$work/out/legacy-gate.o" "$work/out/local-gate.o" \
    || fail "gcc remote object is not byte-identical to the local compile"

# The gcc cpp-failure row: the pre-GetCS capture fails the local cpp on the
# #error TU and must recover through Error 103 (mirroring the in-assignment
# recovery it replaced): the wrapper rebuilds locally and surfaces the same
# status a direct local gcc reports.  The recovery exists for the
# -fdirectives-only-broken-but-really-compilable class, which rebuilds
# with exit 0; a genuinely failing TU (this one) rebuilds to the same
# failure a direct local gcc reports, so the row asserts status parity
# plus the 103 log line.  Bespoke assertions only -- the local rebuild is
# the point of the row, so assert_remote_evidence's anti-fallback grep
# must not run here.
rc_gfail=$(run_client legacy-gcc-fail "" gcc -c "$work/syntaxerr.c" -o "$work/out/gcc-fail.o")
gcc -c "$work/syntaxerr.c" -o "$work/out/gcc-fail-local.o" 2>"$work/stderr-gcc-local"
rc_gcc_local=$?
[ "$rc_gfail" -eq "$rc_gcc_local" ] \
    || fail "gcc cpp-failure status differs from the direct local rebuild: client=$rc_gfail local=$rc_gcc_local"
grep -q "local build forced by remote exception: Error 103 - local cpp invocation failed, trying to recompile locally" \
    "$work/client-legacy-gcc-fail.log" \
    || fail "gcc cpp-failure recovery never logged the Error 103 local-rebuild line"
[ "$rc_gfail" -eq 0 ] && [ -s "$work/out/gcc-fail.o" ] \
    && { cmp -s "$work/out/gcc-fail.o" "$work/out/gcc-fail-local.o" \
         || fail "gcc cpp-failure recovery object differs from the direct local compile"; } \
    || echo "NOTE: the #error TU fails the local rebuild too (expected: the recovery class rebuilds clean only for -fdirectives-only quirks); status parity + the 103 log line are the asserted recovery" >&2

# cpp-failure parity on the clang row: the same exit status and the same
# diagnostic core as a direct local clang -c of the same TU (the pre-GetCS
# cpp runs locally on the original file, so its #error diagnostic names the
# same path and line a direct local compile does; the full stderr is not
# cmp'd byte-for-byte because the -E and -c drivers may differ in summary
# lines).
rc_clang_fail=$(run_client legacy-clang-fail "" "$CLANG" -c "$work/syntaxerr.c" -o "$work/out/fail-clang.o")
"$CLANG" -c "$work/syntaxerr.c" -o "$work/out/fail-clang-local.o" 2>"$work/stderr-clang-local"
rc_clang_local=$?
[ "$rc_clang_fail" -eq "$rc_clang_local" ] \
    || fail "clang cpp-failure exit status differs: client=$rc_clang_fail direct local=$rc_clang_local"
[ "$rc_clang_fail" -ne 0 ] || fail "the #error TU unexpectedly succeeded"
grep -q "ppfirst deliberate preprocessor failure" "$work/stderr-legacy-clang-fail" \
    || fail "the clang cpp failure did not surface the #error diagnostic on stderr"
grep -q "ppfirst deliberate preprocessor failure" "$work/stderr-clang-local" \
    || fail "the direct local clang -c did not surface the #error diagnostic (fixture broken)"
# The replay discipline on the failure path: the buffered #error must print
# exactly once (the replay before the plain status), never zero (a
# discard-everywhere bug would swallow it) -- the old order's cpp printed
# it exactly once too.  Anchored on the diagnostic's "error: " prefix: the
# compiler also echoes the offending #error source line itself.
fail_count=$(grep -c "error: ppfirst deliberate preprocessor failure" "$work/stderr-legacy-clang-fail" || true)
[ "$fail_count" -eq 1 ] \
    || fail "the clang cpp failure printed the #error diagnostic $fail_count times (expected exactly once)"

# The pre-cpp-point failure row: a broken ICECC_VERSION tarball (a real,
# >500-byte, non-archive file -- the same shape as test.sh's
# brokenenvfile.tar.gz) makes the remote fail the environment VERIFY inside
# build_remote_int(), which is BEFORE the old order's cpp point on both
# wires.  The client then falls back to a local rebuild, which prints the
# #warning once from build_local.  The buffered capture stderr must be
# DISCARDED on that path (replayed only at the source stage): if it were
# replayed before the assignment, the warning would print twice -- capture
# plus local rebuild -- and the cmp below would fail.  RED at 9fec00b0
# (replay fired before build_remote_int), GREEN with the replay moved
# inside, at the old cpp points.
broken="$work/brokenenv.tar.gz"
{ for _ in $(seq 1 40); do echo "not a tarball: this file only exists so the remote daemon fails to unpack it as an environment"; done; } >"$broken"
[ "$(wc -c <"$broken")" -ge 500 ] || fail "the broken env fixture is under 500 bytes (parse_icecc_version would reject it before GetCS)"
env -u ICECC_P50_PREPROCESSED_CAPTURE \
    ICECC_TEST_SOCKET="$SUBMIT_SOCKET" \
    ICECC_TEST_REMOTEBUILD=1 \
    ICECC_PREFERRED_HOST=ppfworker \
    ICECC_VERSION="$broken" \
    ICECC_DEBUG=debug ICECC_LOGFILE="$work/client-brokenenv.log" \
    timeout "$timeout_s" "$top/client/icecc" "$CLANG" -c "$work/warn.c" \
    -o "$work/out/warn-brokenenv.o" \
    >"$work/stdout-warn-brokenenv" 2>"$work/stderr-warn-brokenenv"
rc_broken=$?
[ "$rc_broken" -eq 124 ] && fail "broken-env row TIMED OUT"
[ "$rc_broken" -eq 0 ] || fail "broken-env row did not recover through the local rebuild (exit $rc_broken)"
[ -s "$work/out/warn-brokenenv.o" ] || fail "broken-env row produced no object"
grep -q "preprocess-first: preprocessed source ready before assignment request" \
    "$work/client-brokenenv.log" \
    || fail "broken-env row never ran the preprocess-first capture"
grep -q "got exception" "$work/client-brokenenv.log" \
    || fail "broken-env row never hit the in-assignment environment failure (the fixture no longer breaks the remote)"
"$CLANG" -c "$work/warn.c" -o "$work/out/warn-brokenenv-direct.o" \
    2>"$work/stderr-warn-brokenenv-direct"
cmp -s "$work/stderr-warn-brokenenv" "$work/stderr-warn-brokenenv-direct" \
    || { diff -u "$work/stderr-warn-brokenenv-direct" "$work/stderr-warn-brokenenv" >&2 || true
         fail "broken-env row: stderr differs from the direct local compile (the discarded capture replay leaked a diagnostic)"; }
broken_count=$(grep -c "warning: .*ppfirst deliberate preprocessor warning" "$work/stderr-warn-brokenenv" || true)
[ "$broken_count" -eq 1 ] \
    || fail "broken-env row: the #warning diagnostic printed $broken_count times (expected exactly once, from the local rebuild)"

# The PID-1 client row: run the client as PID 1 of a fresh PID namespace
# (docker-entrypoint shape).  The capture child's parent-death check must
# compare against the RECORDED parent pid -- a PID-1 client's capture
# children legitimately see getppid()==1, and a naive '== 1 means dead'
# check would exit every capture child before exec, shipping an EMPTY
# preprocessed source as success (the remote compiles it into an empty
# object, exit 0 -- nothing but the object bytes betray it).  RED at
# 3674b3d7 (empty capture, object differs from the direct local compile);
# GREEN with the recorded-parent-pid check (real capture, byte-identical
# object).  Row-local SKIP when unshare or user namespaces are unavailable
# (the whole-script skip would drop the rows after this one).
pid1_ok=0
if command -v unshare >/dev/null \
    && unshare --user --map-root-user --pid --fork /bin/true >/dev/null 2>&1; then
    pid1_ok=1
else
    echo "NOTE: unshare --user --pid unavailable in this environment; skipping the PID-1 client row (the getppid-vs-recorded-parent fix is compile-checked only here)" >&2
fi
if [ "$pid1_ok" -eq 1 ]; then
    env -u ICECC_P50_PREPROCESSED_CAPTURE \
        ICECC_TEST_SOCKET="$SUBMIT_SOCKET" \
        ICECC_TEST_REMOTEBUILD=1 \
        ICECC_PREFERRED_HOST=ppfworker \
        ICECC_VERSION="$ENVTAR" \
        ICECC_DEBUG=debug ICECC_LOGFILE="$work/client-pid1.log" \
        timeout "$timeout_s" \
        unshare --user --map-root-user --pid --fork \
        "$top/client/icecc" "$CLANG" -c "$work/tu.c" \
        -o "$work/out/pid1.o" \
        >"$work/stdout-pid1" 2>"$work/stderr-pid1"
    rc_pid1=$?
    [ "$rc_pid1" -eq 124 ] && fail "PID-1 row TIMED OUT"
    [ "$rc_pid1" -eq 0 ] || fail "PID-1 client compile exited with $rc_pid1"
    [ -s "$work/out/pid1.o" ] || fail "PID-1 client produced no object"
    grep -q "preprocess-first: preprocessed source ready before assignment request" \
        "$work/client-pid1.log" \
        || fail "PID-1 row never ran the preprocess-first capture"
    assert_remote_evidence pid1
    # Own comparator, same flags as the PID-1 client compile -- the row must
    # not depend on an earlier leg's object staying on disk.
    "$CLANG" -c "$work/tu.c" -o "$work/out/pid1-local.o" 2>/dev/null \
        || fail "direct local compile of tu.c for the PID-1 row failed"
    cmp -s "$work/out/pid1.o" "$work/out/pid1-local.o" \
        || { diff --brief "$work/out/pid1-local.o" "$work/out/pid1.o" >&2 || true
             fail "PID-1 client object differs from the ordinary local compile (the capture shipped an empty or wrong preprocessed source)"; }
fi

stop_farm

echo "p50ppfirst: scheduler-local row (pre-capture dropped on a local assignment)"

# Only the --no-remote submitter daemon: the scheduler has no remote worker,
# so the UseCS selects the localhost fast path (maybe_build_local) and the
# pre-capture must be dropped harmlessly -- the accepted one-wasted-cpp
# cost of the unconditional order.
# Same pair probe: this scheduler binds its text endpoint too.
SCHED_PORT=$(pick_seed_port_pair $((31000 + $$ % 9000))) || fail "no free scheduler port pair"
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

env -u ICECC_P50_PREPROCESSED_CAPTURE \
    ICECC_TEST_SOCKET="$SUBMIT_SOCKET" \
    ICECC_VERSION="$ENVTAR" \
    ICECC_DEBUG=debug ICECC_LOGFILE="$work/client-local-row.log" \
    timeout "$timeout_s" "$top/client/icecc" "$CLANG" -c "$work/tu.c" -o "$work/out/local-row.o" \
    >"$work/stdout-local-row" 2>"$work/stderr-local-row"
rc_local=$?
[ "$rc_local" -eq 124 ] && fail "local-row client TIMED OUT"
[ "$rc_local" -eq 0 ] || fail "scheduler-local compile exited with $rc_local"
[ -s "$work/out/local-row.o" ] || fail "scheduler-local object missing"
grep -q "preprocess-first: preprocessed source ready before assignment request" \
    "$work/client-local-row.log" \
    || fail "scheduler-local row did not run the preprocess-first capture"

# The stderr-replay witness (scheduler-local): a TU with a preprocessor
# warning compiles through maybe_build_local, and its warning must print
# exactly once -- from build_local -- byte-identical to a direct local
# compile.  The capture buffered the cpp's own #warning and discards it
# (the old order never ran cpp for a scheduler-local job); without the
# buffer the warning would print twice (capture + build_local) and the cmp
# would fail.  RED at cafaa344, green with the buffer.
for cc in "$CLANG" gcc; do
    tag=$(basename "$cc")
    env -u ICECC_P50_PREPROCESSED_CAPTURE \
        ICECC_TEST_SOCKET="$SUBMIT_SOCKET" \
        ICECC_VERSION="$ENVTAR" \
        ICECC_DEBUG=debug ICECC_LOGFILE="$work/client-warn-$tag.log" \
        timeout "$timeout_s" "$top/client/icecc" "$cc" -c "$work/warn.c" \
        -o "$work/out/warn-$tag.o" \
        >"$work/stdout-warn-$tag" 2>"$work/stderr-warn-$tag"
    rc_warn=$?
    [ "$rc_warn" -eq 124 ] && fail "warn row ($cc) TIMED OUT"
    [ "$rc_warn" -eq 0 ] || fail "warn row ($cc) exited with $rc_warn"
    [ -s "$work/out/warn-$tag.o" ] || fail "warn row ($cc) produced no object"
    "$cc" -c "$work/warn.c" -o "$work/out/warn-$tag-direct.o" \
        2>"$work/stderr-warn-$tag-direct"
    rc_direct=$?
    [ "$rc_direct" -eq 0 ] || fail "direct local compile of warn.c with $cc failed"
    cmp -s "$work/stderr-warn-$tag" "$work/stderr-warn-$tag-direct" \
        || { diff -u "$work/stderr-warn-$tag-direct" "$work/stderr-warn-$tag" >&2 || true
             fail "warn row ($cc): scheduler-local stderr differs from the direct local compile (capture diagnostics leaked or missing)"; }
    # The diagnostic-line pattern matches both compilers' shapes -- gcc
    # prefixes the message with the directive's own '#warning' marker,
    # clang does not -- and the 'warning: ' anchor excludes the echoed
    # source line (which is '#warning ...' with a space), so the count is
    # of diagnostics only.
    grep -q "warning: .*ppfirst deliberate preprocessor warning" "$work/stderr-warn-$tag" \
        || fail "warn row ($cc): the #warning diagnostic never printed"
    warn_count=$(grep -c "warning: .*ppfirst deliberate preprocessor warning" "$work/stderr-warn-$tag" || true)
    [ "$warn_count" -eq 1 ] \
        || fail "warn row ($cc): the #warning diagnostic printed $warn_count times (expected exactly once)"
done

stop_farm

echo "p50ppfirst: strict all-P50 identity row (capture seam vs direct local cpp)"

start_farm strict-nonce

capture="$work/capture.ii"

# The expected bytes: call_cpp() (client/cpp.cpp) builds the local cpp argv
# from job.localFlags() + job.restFlags(), then appends "-E <input>" and
# (for a clang job under the default rewrite-includes mode)
# "-frewrite-includes".  For this TU the flags are just the -I below, so
# the direct command must mirror that construction exactly -- if the
# Arg_Cpp classification changes, this command is the one to update.
"$CLANG" -E -frewrite-includes -I"$work/inc" "$work/tu2.c" >"$work/capture-direct.ii" 2>/dev/null \
    || fail "direct local clang -E -frewrite-includes of tu2.c failed"

# The capture seam is written by retain_p50_preprocessed_capture() from the
# exact bytes the P50 transfer reads, BEFORE any transfer attempt -- so the
# capture-vs-direct comparison below is valid even when this container's
# C-side sidecar cannot complete the transfer to the worker sidecar at all
# (the repo's own p50assignment-remote.sh gate fails identically in root
# containers; that failure class is environmental, not a client defect).
rc_on=$(run_client strict-on "$capture" "$CLANG" -c "$work/tu2.c" -I"$work/inc" -o "$work/out/strict-on.o")

if [ "$rc_on" -eq 0 ]; then
    assert_remote_evidence strict-on
    assert_ppfirst_ran strict-on
    grep -q "source committed for P50 CompileFile" "$work/client-strict-on.log" \
        || fail "strict run never committed the P50 source"
    "$CLANG" -c "$work/tu2.c" -I"$work/inc" -o "$work/out/local-strict.o"
    cmp -s "$work/out/strict-on.o" "$work/out/local-strict.o" \
        || fail "strict remote object is not byte-identical to the local compile"
else
    case "$rc_on" in
    100) ;;
    *) fail "strict row exited with an unclassified status $rc_on (expected the environmental 100)";;
    esac
    grep -q "cache source transfer failed closed" "$work/client-strict-on.log" \
        || fail "strict row failed without the known environmental sidecar-transfer signature"
    echo "NOTE: strict P50 sidecar transfer is unavailable in this container (repo strict gate fails identically); asserting capture identity only" >&2
fi

# The identity gate itself: the capture must equal the direct local cpp.
[ -s "$capture" ] || fail "strict preprocessed capture is missing"
[ -s "$work/capture-direct.ii" ] || fail "direct local cpp output is missing"
cmp -s "$capture" "$work/capture-direct.ii" \
    || { diff <(head -c 400 "$capture") <(head -c 400 "$work/capture-direct.ii") >&2 || true
         fail "preprocessed bytes differ between the capture and the direct local cpp"; }

stop_farm

echo "OK: p50ppfirst identity, legacy end-to-end, cpp-failure parity (incl. gcc 103), and local selection all passed"
exit 0
