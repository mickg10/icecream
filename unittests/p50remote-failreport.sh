#!/bin/sh
# REMOTE-FAILURE HONEST REPORTING regression (R15, virtu2-diag).
#
# The worker maps BOTH a real ENOMEM kill (EXIT_OUT_OF_MEMORY=105) and a
# worker-side I/O error (EXIT_IO_ERROR=107 -- the 60s input-read timeout,
# EOF/protocol error while reading the preprocessed file, open/socket
# failures, or the compile child's own "no space left on device" report
# via workit's disk heuristic) onto the SAME `was_out_of_memory` bit,
# because that bit is the LOCAL-FALLBACK trigger and old peers fall back
# on it (removing it for IO errors would surface a worker IO hiccup as a
# user-visible compile failure).  The pre-fix client then printed ONE text
# for all of it: "the server ran out of memory, recompiling locally" --
# actively wrong for everything except a real OOM kill.
#
# The fix (client/remote.cpp) keeps the wire byte-identical and picks the
# text from the status the worker binds on that path (105/107; a stock/old
# worker may leave it 0 or stale): 105 -> the honest OOM text, 107 -> the
# remote I/O error text pointing at the worker log, anything else -> a
# neutral fallback text.  The worker (daemon/serve.cpp) additionally
# logs the honest class line at the mapping site, and workit's IO paths
# name the path and errno.
#
# This test pins the 107 class end to end, via the ONE deterministic
# in-container shape: a fixture compiler (a real ELF binary -- the
# compile child is execv'd INSIDE the env chroot, which has no shell, so
# a #!/bin/sh stub can never start and only produces EXIT_COMPILER_MISSING)
# that prints "no space left on device" to stderr and exits 1.  workit's
# disk-space heuristic matches that stderr and returns EXIT_IO_ERROR (107)
# -- the same class as the fd-bug's real-world failures.  The worker's log
# must name the class (serve.cpp's mapping line), the client must NOT
# print the OOM text for it and must print the 107 text instead, and the
# client still produces an object via local fallback (the bit's whole
# job: behaviour unchanged, only reporting is honest).  A plain compiler
# exit WITHOUT a heuristic match never reaches this mapping at all (the
# status passes through to the user unchanged) and is out of scope here;
# the unit leg pins the 105/0/old-worker text cases.
#
# HARNESS FACTS (same as envchardev.c.sh; do not "simplify" them away):
# - the env install is CLIENT-PULLED after a UseCS with got_env=false, so
#   a SECOND, env-less daemon is structurally required;
# - root-started daemons bind the DEFAULT /var/run/icecc path, so
#   ICECC_TEST_SOCKET pins EACH daemon's unix socket;
# - the scheduler records each daemon by the TCP peer address of its
#   connection, so the daemons connect on the container's eth0 address
#   (a loopback connect registers as the 127.0.0.1 local sentinel);
# - the client's lstat floor rejects tarballs under 500 bytes;
# - a >= 4s settle after login is required before the first GetCS.
set -u

fail() { echo "p50remote-failreport: FAIL: $*" 1>&2; exit 1; }

src_dir=$(dirname "$0")
build_dir=${ICECC_TEST_BUILDDIR:-$src_dir}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}
ICECCD=${1:-$top_build_dir/daemon/iceccd}
ICECC=${ICECC:-$top_build_dir/client/icecc}
SCHEDULER=${SCHEDULER:-$top_build_dir/scheduler/icecc-scheduler}

# Root-gated: the fixture env install and the worker's chroot need the
# root-started capng-drop shape (the same gating as envchardev.c.sh).
if [ "$(id -u)" != "0" ]; then
    echo "p50remote-failreport: skipping (needs the root-started daemon shape)" 1>&2
    exit 77
fi
icecc_user=""
if id -u icecc >/dev/null 2>&1; then icecc_user=icecc; fi

SCHED_ADDR=${ICECC_TEST_SCHED_ADDR:-127.0.0.1}

workdir=$(mktemp -d /tmp/p5failrep.XXXXXX) || fail mktemp
trap 'kill $sched_pid $daemonA_pid $daemonB_pid 2>/dev/null; rm -rf "$workdir"' EXIT INT TERM

# A REAL env via icecc-create-env, then the env's ./usr/bin/{gcc,g++}
# REPLACED with a real ELF fixture: prints "no space left on device" to
# stderr, exits 1.  workit's disk heuristic matches that stderr line and
# returns EXIT_IO_ERROR -- the 107 class the pre-fix text mislabelled as
# OOM.  It must be a compiled binary, not a script: the compile child is
# execv'd inside the chroot where no shell exists.
create_env="$top_build_dir/client/icecc-create-env"
[ -f "$create_env" ] || create_env="$top_build_dir/client/icecc-create-env.in"
[ -f "$create_env" ] || fail "no icecc-create-env in $top_build_dir/client"
mkdir -p "$workdir/envpack"
cat > "$workdir/envpack/fakecc.c" <<'EOF'
#include <stdio.h>
int main(void) {
    fprintf(stderr, "fakecc: no space left on device\n");
    return 1;
}
EOF
gcc -static -O0 -o "$workdir/envpack/fakecc" "$workdir/envpack/fakecc.c" 2>"$workdir/fakecc-build.log" || {
    cat "$workdir/fakecc-build.log" >&2
    fail "could not build the fixture compiler"
}
(cd "$workdir/envpack" && bash "$create_env" --gcc "$(command -v gcc)" "$(command -v g++)" \
    > "$workdir/create-env.log" 2>&1) || {
    cat "$workdir/create-env.log" >&2
    fail "icecc-create-env fixture failed"
}
built_tar=$(awk '{print $NF}' "$workdir/create-env.log" | grep -E '\.tar\.gz$' | tail -1)
[ -n "$built_tar" ] && [ -s "$workdir/envpack/$built_tar" ] || \
    fail "create-env produced no tarball"
mkdir -p "$workdir/envpack/root"
tar -xzf "$workdir/envpack/$built_tar" -C "$workdir/envpack/root" || fail "extract fixture failed"
for cc in gcc g++; do
    rm -f "$workdir/envpack/root/usr/bin/$cc"
    cp "$workdir/envpack/fakecc" "$workdir/envpack/root/usr/bin/$cc"
    chmod 0755 "$workdir/envpack/root/usr/bin/$cc"
done
mkdir -p "$workdir/envpack/root/dev"
mknod "$workdir/envpack/root/dev/null" c 1 3 2>/dev/null || true
tar -C "$workdir/envpack/root" -czf "$workdir/env.tgz" . || fail "tar fixture failed"
[ "$(wc -c < "$workdir/env.tgz")" -ge 500 ] || fail "fixture under the client's 500-byte floor"

mkdir -p "$workdir/envsA" "$workdir/envsB" "$workdir/homeA" "$workdir/homeB"
chown -R "${icecc_user:-nobody}" "$workdir"

ICECC_TEST_SOCKET="$workdir/a.sock" HOME="$workdir/homeA" "$SCHEDULER" -vvv -n failrepnet -l "$workdir/sched.log" &
sched_pid=$!
sleep 1
ICECC_TEST_SOCKET="$workdir/a.sock" HOME="$workdir/homeA" "$ICECCD" -vvv -n failrepnet -N localnode -p 10255 \
    -b "$workdir/envsA" -l "$workdir/daemonA.log" &
daemonA_pid=$!
# No -s: the daemons discover the scheduler by broadcast on the netname
# (the same shape envchardev.c.sh runs); SCHED_ADDR only matters for the
# strict-localhost rows and is kept for parity, unused here.
ICECC_TEST_SOCKET="$workdir/b.sock" HOME="$workdir/homeB" "$ICECCD" -vvv -n failrepnet -N remotenode -p 10256 \
    -b "$workdir/envsB" -l "$workdir/daemonB.log" &
daemonB_pid=$!
sleep 5
for pid in $sched_pid $daemonA_pid $daemonB_pid; do
    kill -0 "$pid" 2>/dev/null || fail "a daemon exited early: $(tail -3 "$workdir"/*.log 2>/dev/null)"
done

printf 'int main(void){return 0;}\n' > "$workdir/t.c"
chown "${icecc_user:-nobody}" "$workdir/t.c"
# ICECC_DEBUG=warning: the R15 client lines (remote.cpp:1405-1412) are
# log_warning and the client defaults to Error level -- without this the
# honest 107 text is invisible in client.log and gate 3 greps dead air.
ICECC_TEST_SOCKET="$workdir/a.sock" \
ICECC_VERSION="$workdir/env.tgz" \
ICECC_PREFERRED_HOST=remotenode CCACHE_DISABLE=1 ICECC_DEBUG=warning \
    HOME="$workdir/homeA" \
    "$ICECC" /usr/bin/gcc -c "$workdir/t.c" -o "$workdir/t.o" \
    > "$workdir/client.log" 2>&1 || true
sleep 4

logB="$workdir/daemonB.log"
schedlog="$workdir/sched.log"
clientlog="$workdir/client.log"

# 1. the env installed on node B (the fixture rode the real install path)
[ -d "$workdir/envsB/target=x86_64" ] || fail "env did not install on node B (check $logB)"

# 2. THE RED/GREEN GATE: the 107-class failure must NOT produce the OOM
#    text anywhere -- not the client (the R15 fix: it prints the 107 text),
#    not the worker.  RED at d36df986: the pre-fix client printed the OOM
#    text for exactly this class.  The exception text ("Error 101 - the
#    server ran out of memory, recompiling locally") is DELIBERATELY
#    unchanged (wire compat, old peers fall back on it) and main.cpp
#    echoes error.what() into this log as "local build forced by remote
#    exception: Error 101 ...", so that one line is excluded -- everything
#    else carrying the OOM wording is a regression.
grep -v "local build forced by remote exception" "$clientlog" 2>/dev/null | \
    grep -q "ran out of memory" && \
    fail "client printed the OOM text for a worker I/O error (RED: honest reporting missing)"
grep -q "ran out of memory" "$logB" 2>/dev/null && \
    fail "worker printed the OOM text for a worker I/O error"

# 3. the honest texts: the worker names the class (serve.cpp's mapping
#    line), the client names the I/O error and the worker host (the UseCS
#    hostname -- the worker's TCP peer address, not the netname).
grep -q "worker-side I/O error" "$logB" 2>/dev/null || \
    fail "worker log lacks the honest class line (check $logB)"
grep -q "remote I/O error on " "$clientlog" 2>/dev/null || \
    fail "client log lacks the 107 text naming the worker host (check $clientlog)"
# 3b. the worker's line names the real status the client received (107).
grep -q "status 107" "$logB" 2>/dev/null || \
    fail "worker's class line does not name status 107 (check $logB)"

# 4. local fallback behaviour UNCHANGED: the bit fired the fallback (the
#    whole reason the bit stays set), the local compiler produced the object.
[ -s "$workdir/t.o" ] || fail "no local-fallback object (fallback behaviour changed)"

echo "p50remote-failreport: OK (107-class worker I/O error: no OOM text, class + status named, fallback object present)"
