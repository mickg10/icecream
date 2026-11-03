#!/bin/sh
# ENV-INSTALL CHARDEV-WARNING regression (item (c) of the fd-bug stream, virtu-fdfix).
#
# The env tarball may carry ./dev/null as a character device.  Extracting it
# needs CAP_MKNOD, which the unprivileged daemon never has: libarchive
# reports the failed mknod as a WARNING and the entry goes missing -- the
# STOCK loop tolerated that without a word.  The fix logs a NAMED warning
# (daemon/environment.cpp -> icecc::chardev_extract_is_complete()) and the
# install PROCEEDS: with the std-fd fix a chroot without /dev/null no
# longer breaks the compiler, so rejecting the env (the earlier loud-fail
# shape) would have refused every dev-carrying env on every uid-dropped
# node for no gain.  This test pins the warning + the WORKING env.
#
# HARNESS FACTS (live-verified; do not "simplify" them away):
# - The env install is CLIENT-PULLED after a UseCS with got_env=false
#   (client/remote.cpp build_remote_int sends EnvTransferMsg + the tarball to
#   the chosen remote daemon).  A SINGLE-daemon netname can never trigger it:
#   the only daemon is also the submitter, the scheduler always matches the
#   submitter's own env (envs_match != "") and answers NoCS -> local.  A
#   SECOND, env-less daemon is structurally required.
# - Pin ICECC_PREFERRED_HOST to the env-less node: fresh nets pick randomly.
# - Wait >= 4s after both logins (fresh login reports load 1000 until the
#   first stats ping clears eligibility).
# - Pin ICECC_TEST_SOCKET per daemon: a root-started daemon (CAP_SYS_CHROOT)
#   binds the DEFAULT /var/run/icecc path, not $HOME/.iceccd.socket -- and
#   two daemons would unlink each other's default socket.
# - Root-STARTED daemons (capng drops to icecc internally): an unprivileged
#   start sets noremote and never accepts the transfer.  The extract child
#   then runs as icecc WITHOUT CAP_MKNOD -- the condition the warning names.
# - The fixture tarball must be >= 500 bytes (the client's lstat floor).
#
# GREEN at virtu-fdfix: the install SUCCEEDS (env dir present), node B's log
# carries the named warning, and a remote job on that env exits 0.
# RED at c8bb615f: the env installs with the device missing and NO warning
# line -- the silent-breakage the fix names.
src_dir=$(dirname "$0")
build_dir=${ICECC_TEST_BUILDDIR:-$src_dir}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}
ICECCD=${ICECCD:-$top_build_dir/daemon/iceccd}
SCHEDULER=${SCHEDULER:-$top_build_dir/scheduler/icecc-scheduler}
ICECC=${ICECC:-$top_build_dir/client/icecc}

fail() { echo "envchardev: FAIL: $*" 1>&2; exit 1; }

# Stop a set of background PIDS for REAL: signal, then wait (bounded) for
# each to actually EXIT, then kill -9 anything that ignored the signal,
# then reap with wait.  A scheduler that is only SIGNALED keeps its
# listeners open until its poll loop notices -- and with SO_REUSEADDR a
# live listener still blocks the next bind -- so the next test in the
# same make-check run (p50remote-failreport.sh starts a scheduler on the
# same default port) loses the bind race if this test's teardown only
# fires a kill and exits.  The round-14 root stage failed exactly there.
stop_processes() { # <pids...>
    for pid in "$@"; do
        kill "$pid" 2>/dev/null || :
    done
    for _ in $(seq 1 50); do
        live=0
        for pid in "$@"; do
            kill -0 "$pid" 2>/dev/null && live=1
        done
        [ "$live" -eq 0 ] && break
        sleep 0.1
    done
    for pid in "$@"; do
        kill -9 "$pid" 2>/dev/null || :
        wait "$pid" 2>/dev/null || :
    done
}

# The fixture (mknod) needs root; the daemons run root-started (capng drop).
if [ "$(id -u)" != "0" ]; then
    echo "envchardev: skipping (needs root: mknod fixture + root-started daemons)" >&2
    exit 77
fi
icecc_user=""
if id -u icecc >/dev/null 2>&1; then icecc_user=icecc; fi
[ -n "$icecc_user" ] || fail "no icecc user (the capng drop target)"

workdir=$(mktemp -d /tmp/envchardev.XXXXXX) || fail mktemp
trap 'stop_processes "$sched_pid" "$daemonA_pid" "$daemonB_pid"; rm -rf "$workdir"' EXIT INT TERM

# A REAL env via icecc-create-env (the compiler + its lib deps -- a bare
# copied gcc binary cannot exec inside the env chroot, missing libs), then
# ./dev/null added as a REAL char device (>= 500 bytes: the client's lstat
# floor).  The remote job must actually run and exit 0 on this env.
create_env="$top_build_dir/client/icecc-create-env"
[ -f "$create_env" ] || create_env="$top_build_dir/client/icecc-create-env.in"
[ -f "$create_env" ] || fail "no icecc-create-env in $top_build_dir/client"
mkdir -p "$workdir/envpack"
(cd "$workdir/envpack" && bash "$create_env" --gcc "$(command -v gcc)" "$(command -v g++)" \
    > "$workdir/create-env.log" 2>&1) || {
    cat "$workdir/create-env.log" >&2
    fail "icecc-create-env fixture failed"
}
built_tar=$(awk '{print $NF}' "$workdir/create-env.log" | grep -E '\.tar\.gz$' | tail -1)
[ -n "$built_tar" ] && [ -s "$workdir/envpack/$built_tar" ] || \
    fail "create-env produced no tarball"
tar -xzf "$workdir/envpack/$built_tar" -C "$workdir/envpack/root" 2>/dev/null || {
    mkdir -p "$workdir/envpack/root" && \
    tar -xzf "$workdir/envpack/$built_tar" -C "$workdir/envpack/root"; }
mkdir -p "$workdir/envpack/root/dev"
mknod "$workdir/envpack/root/dev/null" c 1 3 || fail "mknod fixture failed"
tar -C "$workdir/envpack/root" -czf "$workdir/env.tgz" . || fail "tar fixture failed"
tar -tzf "$workdir/env.tgz" | grep -q "dev/null" || fail "fixture lacks dev/null"
[ "$(wc -c < "$workdir/env.tgz")" -ge 500 ] || fail "fixture under the client's 500-byte floor"

# Two env dirs (A = the submitter's daemon; B = the env-less node).
mkdir -p "$workdir/envsA" "$workdir/envsB" "$workdir/homeA" "$workdir/homeB"
chown -R "$icecc_user" "$workdir"

# Probe-bound ports, not the defaults: the scheduler's 8765 (and the daemons'
# 10245/10246) are also taken by other tests in the same parallel root stage
# (p50remote-failreport.sh starts a scheduler on 8765), and the loser dies at
# bind.  The scheduler binds its port AND port+1, so probe the pair; the
# daemons then name it with -s instead of finding it by broadcast, on a real
# (non-loopback) address as the scheduler registers daemons by peer address.
# 45000-49999 is disjoint from the other e2e tests' bands.
sched_addr=$(hostname -I 2>/dev/null | awk '{print $1}')
case "$sched_addr" in
""|127.*) echo "envchardev: skipping (no non-loopback IPv4 address)" >&2; exit 77 ;;
esac
ports=$(python3 - $$ <<'PY'
import socket, sys
def free(*ports):
    socks = []
    try:
        for port in ports:
            sock = socket.socket()
            socks.append(sock)
            sock.bind(("", port))
        return True
    except OSError:
        return False
    finally:
        for sock in socks:
            sock.close()
for port in range(45000 + int(sys.argv[1]) % 4000, 49990):
    if free(port, port + 1, port + 2, port + 3):
        print(port, port + 2, port + 3)
        raise SystemExit(0)
raise SystemExit("no free ports in 45000-49999")
PY
) || fail "no free scheduler/daemon ports"
set -- $ports
sched_port=$1 portA=$2 portB=$3

# ICECC_TEST_SOCKET on EACH daemon pins its unix socket (a root-started
# daemon with CAP_SYS_CHROOT binds the DEFAULT /var/run/icecc path, NOT
# $HOME/.iceccd.socket -- and both daemons would race the same default;
# the client below connects to daemon A's pinned path).
ICECC_TEST_SOCKET="$workdir/a.sock" HOME="$workdir/homeA" "$SCHEDULER" -vvv -p "$sched_port" -n envchardevnet -l "$workdir/sched.log" &
sched_pid=$!
sleep 1
ICECC_TEST_SOCKET="$workdir/a.sock" HOME="$workdir/homeA" "$ICECCD" -vv -n envchardevnet -N localnode -p "$portA" \
    -s "$sched_addr:$sched_port" -b "$workdir/envsA" -l "$workdir/daemonA.log" &
daemonA_pid=$!
ICECC_TEST_SOCKET="$workdir/b.sock" HOME="$workdir/homeB" "$ICECCD" -vv -n envchardevnet -N remotenode -p "$portB" \
    -s "$sched_addr:$sched_port" -b "$workdir/envsB" -l "$workdir/daemonB.log" &
daemonB_pid=$!
# With -s the daemons connect on their own timer (a few seconds), not at the
# first broadcast: wait for BOTH logins, then the 4s+ eligibility settle.
for _ in $(seq 1 60); do
    [ "$(grep -c 'login ' "$workdir/sched.log" 2>/dev/null)" -ge 2 ] && break
    sleep 0.5
done
sleep 5
for pid in $sched_pid $daemonA_pid $daemonB_pid; do
    kill -0 "$pid" 2>/dev/null || fail "a daemon exited early: $(tail -3 "$workdir"/*.log 2>/dev/null)"
done

# One remote compile pinned to node B: it pulls the env install, then RUNS.
printf 'int main(void){return 0;}\n' > "$workdir/t.c"
chown "$icecc_user" "$workdir/t.c"
ICECC_TEST_SOCKET="$workdir/a.sock" \
ICECC_VERSION="$workdir/env.tgz" \
ICECC_PREFERRED_HOST=remotenode CCACHE_DISABLE=1 HOME="$workdir/homeA" \
    "$ICECC" /usr/bin/gcc -c "$workdir/t.c" -o "$workdir/t.o" \
    >/dev/null 2>&1 || true
sleep 4

logB="$workdir/daemonB.log"
schedlog="$workdir/sched.log"
# 1. the install SUCCEEDED (env dir present on node B)
[ -d "$workdir/envsB/target=x86_64" ] || fail "env did not install on node B (check $logB)"
# 2. the named warning is there (the char device could not be created)
grep -q "character device.*could not be created\|installs WITHOUT" "$logB" 2>/dev/null || \
    fail "no chardev warning in node B's log (silent missing device: the exact pre-fix state)"
# 3. the remote job on that env actually ran and exited 0 (the scheduler's
#    END line for our job: status=0)
grep -q "END .*status=0" "$schedlog" 2>/dev/null || \
    fail "no completed remote job on the dev-less env (check $schedlog)"

echo "envchardev: OK (env installed WITH a named chardev warning; remote job exited 0 on it)"
