#!/bin/sh
# Environment-store regression: a second transfer of an environment that a
# worker already has (or is still unpacking) must never erase the live one.
#
# Before the store, start_install_environment() unpacked straight into
# target=<platform>/<name>; a second transfer of the same name found that
# directory, failed its mkdir, and handle_transfer_env() then ran
# remove_environment_files() on the FINAL name -- under any compile running
# in it, and under the first install still unpacking.
#
# Legs (each prints "ok"/"FAILED" lines; the test fails if any leg fails):
#   1 dup-during-unpack: transfer #1 of X pauses mid-stream; transfer #2 of
#     X runs to completion; #1 resumes.  Both must verify and a remote
#     compile in X must run in X.
#   2 dup-after-publish: Y is installed and a compile is running INSIDE it
#     (the fake compiler holds on <tree>/tmp/hold); Y is transferred again;
#     the compile must still finish remotely, in the same tree.
#
# HARNESS FACTS (shared with envchardev.c.sh):
# - root-started daemons (capng drops to icecc; an unprivileged start sets
#   noremote and never accepts a transfer); ICECC_TEST_SOCKET per daemon.
# - the transfers are driven straight at the worker's port by envstore_xfer
#   (the same EnvTransfer/FileChunk/End/VerifyEnv sequence the client sends);
#   the compiles go through the real client, pinned to the worker.
# - the env is a static fake compiler (envstore_fakecc.c): the object it
#   writes is the literal "FAKEOBJ", so a remote compile in the env is told
#   apart from a local fallback (a real object) without reading any log.
src_dir=$(dirname "$0")
build_dir=${ICECC_TEST_BUILDDIR:-$src_dir}
top_build_dir=${ICECC_TEST_TOP_BUILDDIR:-$build_dir/..}
ICECCD=${ICECCD:-$top_build_dir/daemon/iceccd}
SCHEDULER=${SCHEDULER:-$top_build_dir/scheduler/icecc-scheduler}
ICECC=${ICECC:-$top_build_dir/client/icecc}
XFER=${XFER:-$build_dir/envstore_xfer}
CC_FOR_ENV=${ICECC_TEST_CC:-cc}

if [ "$(id -u)" != "0" ]; then
    echo "envstore: skipping (needs root: root-started daemons)" >&2
    exit 77
fi
id -u icecc >/dev/null 2>&1 || { echo "envstore: FAIL: no icecc user" >&2; exit 1; }
[ -x "$XFER" ] || { echo "envstore: FAIL: $XFER not built" >&2; exit 1; }

workdir=$(mktemp -d "${TMPDIR:-/tmp}/envstore.XXXXXX") || exit 1
arch=$(uname -m)
netname=envstore$$
sched_port=$((20000 + $$ % 20000))
portA=$((sched_port + 2))
portB=$((sched_port + 3))
failures=0
pids=""

ok() { echo "ok       - $*"; }
bad() { echo "FAILED   - $*"; failures=$((failures + 1)); }
check() { # <description> <command...>
    local what=$1; shift
    if "$@"; then ok "$what"; else bad "$what"; fi
}

stop_processes() { # <pids...>
    for pid in "$@"; do kill "$pid" 2>/dev/null || :; done
    for _ in $(seq 1 50); do
        live=0
        for pid in "$@"; do kill -0 "$pid" 2>/dev/null && live=1; done
        [ "$live" -eq 0 ] && break
        sleep 0.1
    done
    for pid in "$@"; do kill -9 "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; done
}
cleanup() {
    stop_processes $pids
    if [ "$failures" -ne 0 ] || [ -n "${ENVSTORE_KEEP:-}" ]; then
        echo "envstore: logs kept in $workdir" >&2
    else
        chmod -R u+w "$workdir" 2>/dev/null
        rm -rf "$workdir"
    fi
}
trap cleanup EXIT INT TERM

wait_for() { # <seconds> <command...>
    local limit=$(( $1 * 10 )); shift
    for _ in $(seq 1 "$limit"); do "$@" && return 0; sleep 0.1; done
    return 1
}

# ---- fixture: a static fake compiler env -------------------------------
"$CC_FOR_ENV" -static -O1 -o "$workdir/fakecc" "$src_dir/envstore_fakecc.c" \
    > "$workdir/fakecc.log" 2>&1 || {
    cat "$workdir/fakecc.log" >&2
    echo "envstore: FAIL: cannot build the static fake compiler" >&2
    exit 1
}

# make_env <name> <label>: envs/<name>.tar.gz (the client names the env by
# the tarball's basename without the extension), a tarball whose important entries come FIRST and a
# 3 MB incompressible pad LAST, so a transfer paused at 1 MB has already
# extracted bin/true, usr/bin/as and usr/bin/gcc.  Distinct calls give
# distinct content (label + random pad).
make_env() {
    local name=$1 label=$2 root="$workdir/envroot.$1"
    mkdir -p "$root/bin" "$root/usr/bin" "$root/etc" "$workdir/envs"
    cp "$workdir/fakecc" "$root/bin/true"
    cp "$workdir/fakecc" "$root/usr/bin/as"
    cp "$workdir/fakecc" "$root/usr/bin/gcc"
    echo "$label" > "$root/etc/envstore-label"
    head -c 3000000 /dev/urandom > "$root/zz-padding"
    tar -C "$root" -czf "$workdir/envs/$name.tar.gz" ./bin ./usr ./etc ./zz-padding
    rm -rf "$root"
}

# xfer <name> [pause_at marker resume]: transfer envs/<name> to node B.
xfer() {
    local name=$1; shift
    timeout 180 "$XFER" xfer 127.0.0.1 "$portB" "$arch" "$name" "$workdir/envs/$name.tar.gz" "$@"
}

# Resolved tree of a name on node B (empty if absent).
tree_of() { readlink -f "$workdir/envsB/target=$arch/$1" 2>/dev/null; }

# compile <name> <out>: one remote compile pinned to node B on env <name>.
compile() {
    local name=$1 out=$2
    printf 'int main(void){return 0;}\n' > "$out.c"
    ICECC_TEST_SOCKET="$workdir/A.sock" ICECC_VERSION="$workdir/envs/$name.tar.gz" \
    ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=envstoreb CCACHE_DISABLE=1 HOME="$workdir/homeA" \
        timeout 180 "$ICECC" /usr/bin/gcc -c "$out.c" -o "$out" > "$out.log" 2>&1
}
is_fakeobj() { [ "$(head -c 16 "$1" 2>/dev/null | tr -d '\000')" = "FAKEOBJ" ]; }

# ---- farm: scheduler, submitter A, worker B -----------------------------
mkdir -p "$workdir/envsA" "$workdir/envsB" "$workdir/homeA" "$workdir/homeB"
chown -R icecc "$workdir"
HOME="$workdir/homeA" "$SCHEDULER" -vvv -n "$netname" -p "$sched_port" \
    -l "$workdir/sched.log" &
pids="$pids $!"
sleep 1
start_daemon() { # <label> <port> <extra args...>
    local label=$1 port=$2; shift 2
    ICECC_TEST_SOCKET="$workdir/$label.sock" HOME="$workdir/home$label" \
        "$ICECCD" -vvv -n "$netname" -s "127.0.0.1:$sched_port" -N "envstore$(echo "$label" | tr A-Z a-z)" \
        -p "$port" -b "$workdir/envs$label" -l "$workdir/daemon$label.log" "$@" &
    pids="$pids $!"
}
start_daemon A "$portA" --no-remote -m 0
start_daemon B "$portB" -m 4
# Both daemons must register (and their first stats ping must clear the
# fresh-login load) before anything is submitted.
logins_done() { [ "$(grep -c "login" "$workdir/sched.log" 2>/dev/null)" -ge 2 ]; }
wait_for 30 logins_done || { echo "envstore: FAIL: daemons never registered" >&2; exit 1; }
sleep 4
for pid in $pids; do
    kill -0 "$pid" 2>/dev/null || { echo "envstore: FAIL: a farm process exited early" >&2; exit 1; }
done

# ---- leg 1: a second transfer while the first is still unpacking -------
make_env X x
xfer X 1000000 "$workdir/x1.paused" "$workdir/x1.resume" > "$workdir/x1.out" 2>&1 &
x1_pid=$!
wait_for 30 test -e "$workdir/x1.paused" || bad "leg1: transfer #1 never reached its pause"
sleep 1     # let the unpack child extract the first megabyte
xfer X > "$workdir/x2.out" 2>&1
x2_rc=$?
touch "$workdir/x1.resume"
wait "$x1_pid"
x1_rc=$?
echo "leg1: transfer#1 rc=$x1_rc ($(cat "$workdir/x1.out")) transfer#2 rc=$x2_rc ($(cat "$workdir/x2.out"))"
echo "leg1: X -> $(tree_of X); contents: $(ls "$(tree_of X)" 2>&1 | tr '\n' ' ')"
check "leg1: the duplicate transfer #2 verified" test "$x2_rc" -eq 0
check "leg1: the paused transfer #1 verified after the duplicate" test "$x1_rc" -eq 0
check "leg1: X still has bin/true and usr/bin/gcc" \
    test -x "$(tree_of X)/bin/true" -a -x "$(tree_of X)/usr/bin/gcc"
compile X "$workdir/leg1.o"
leg1_rc=$?
echo "leg1: compile rc=$leg1_rc object=$(head -c 16 "$workdir/leg1.o" 2>/dev/null | tr -d '\000' | tr -c '[:print:]' '.')"
check "leg1: a compile on X ran remotely in X" is_fakeobj "$workdir/leg1.o"

# ---- leg 2: a second transfer while a compile runs in the published env --
make_env Y y
xfer Y > "$workdir/y1.out" 2>&1 || bad "leg2: first install of Y failed ($(cat "$workdir/y1.out"))"
y_tree=$(tree_of Y)
y_ino=$(stat -c %i "$y_tree" 2>/dev/null)
touch "$y_tree/tmp/hold"
compile Y "$workdir/leg2.o" &
c2_pid=$!
if wait_for 60 test -e "$y_tree/tmp/fakecc.running"; then
    ok "leg2: a compile is running inside Y ($y_tree)"
else
    bad "leg2: the compile never started inside Y"
fi
xfer Y > "$workdir/y2.out" 2>&1
y2_rc=$?
echo "leg2: duplicate transfer rc=$y2_rc ($(cat "$workdir/y2.out")); Y tree after: $(ls "$y_tree" 2>&1 | tr '\n' ' ')"
check "leg2: the duplicate transfer verified" test "$y2_rc" -eq 0
check "leg2: the live tree of Y survived the duplicate (same inode)" \
    test "$(stat -c %i "$y_tree" 2>/dev/null)" = "$y_ino" -a -x "$y_tree/usr/bin/gcc"
rm -f "$y_tree/tmp/hold"
wait "$c2_pid"
c2_rc=$?
echo "leg2: compile rc=$c2_rc object=$(head -c 16 "$workdir/leg2.o" 2>/dev/null | tr -d '\000' | tr -c '[:print:]' '.')"
check "leg2: the running compile finished remotely in Y" is_fakeobj "$workdir/leg2.o"
check "leg2: Y still resolves to the same tree" test -d "$y_tree" -a "$(tree_of Y)" = "$y_tree"

if [ "$failures" -ne 0 ]; then
    echo "---- node B log (env lines) ----" >&2
    grep -E "environment|install|removing|envstore|store" "$workdir/daemonB.log" | tail -60 >&2
    echo "envstore: $failures check(s) FAILED" >&2
    exit 1
fi
echo "envstore: all checks passed"
exit 0
