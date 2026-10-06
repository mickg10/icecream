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
# - Pin ICECC_TEST_SOCKET per daemon (both bind the same $HOME socket path;
#   the later starter unlinks the earlier one).
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

# The fixture (mknod) needs root; the daemons run root-started (capng drop).
if [ "$(id -u)" != "0" ]; then
    echo "envchardev: skipping (needs root: mknod fixture + root-started daemons)" >&2
    exit 77
fi
icecc_user=""
if id -u icecc >/dev/null 2>&1; then icecc_user=icecc; fi
[ -n "$icecc_user" ] || fail "no icecc user (the capng drop target)"

workdir=$(mktemp -d /tmp/envchardev.XXXXXX) || fail mktemp
trap 'kill $sched_pid $daemonA_pid $daemonB_pid 2>/dev/null; rm -rf "$workdir"' EXIT INT TERM

# A minimal env whose ./dev/null is a real char device (>= 500 bytes: the
# client's lstat floor rejects smaller tarballs).  The compiler in the env
# is the REAL gcc: the remote job must actually run and exit 0 on it.
mkdir -p "$workdir/envroot/dev" "$workdir/envroot/bin"
cp "$(command -v gcc)" "$workdir/envroot/bin/gcc" 2>/dev/null || fail "no gcc for the fixture"
head -c 2048 /dev/urandom > "$workdir/envroot/pad"
mknod "$workdir/envroot/dev/null" c 1 3 || fail "mknod fixture failed"
tar -C "$workdir/envroot" -czf "$workdir/env.tgz" . || fail "tar fixture failed"
tar -tzf "$workdir/env.tgz" | grep -q "dev/null" || fail "fixture lacks dev/null"
[ "$(wc -c < "$workdir/env.tgz")" -ge 500 ] || fail "fixture under the client's 500-byte floor"

# Two env dirs (A = the submitter's daemon; B = the env-less node).
mkdir -p "$workdir/envsA" "$workdir/envsB" "$workdir/homeA" "$workdir/homeB"
chown -R "$icecc_user" "$workdir"

HOME="$workdir/homeA" "$SCHEDULER" -vv -n envchardevnet -l "$workdir/sched.log" &
sched_pid=$!
sleep 1
HOME="$workdir/homeA" "$ICECCD" -vv -n envchardevnet -N localnode -p 10245 \
    -b "$workdir/envsA" -l "$workdir/daemonA.log" &
daemonA_pid=$!
HOME="$workdir/homeB" "$ICECCD" -vv -n envchardevnet -N remotenode -p 10246 \
    -b "$workdir/envsB" -l "$workdir/daemonB.log" &
daemonB_pid=$!
sleep 5
for pid in $sched_pid $daemonA_pid $daemonB_pid; do
    kill -0 "$pid" 2>/dev/null || fail "a daemon exited early: $(tail -3 "$workdir"/*.log 2>/dev/null)"
done

# One remote compile pinned to node B: it pulls the env install, then RUNS.
printf 'int main(void){return 0;}\n' > "$workdir/t.c"
chown "$icecc_user" "$workdir/t.c"
ICECC_TEST_SOCKET="$workdir/homeA/iceccd.socket" \
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
