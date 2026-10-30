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
#   3 concurrent duplicates: two transfers of Z pause together and resume
#     together; both verify, one tree, the loser's staging copy is swept.
#   4 duplicate of an installed env: same tree (inode), no new tree.
#   5 same name, new content: W is re-pointed with a loud warning; the old
#     tree survives its running compile, then reconcile removes it.
#   6 eviction while a compile runs (node C, 1 MB cache limit): the running
#     job's name and tree survive; an idle name is evicted and its tree
#     deleted; once the job ends the next reconcile evicts it too.
#   7 an install child killed mid-unpack: no name, no tree; the next
#     reconcile sweeps its staging dir.
#   8 damage after publish: an env whose compiler cannot be exec'd loses
#     its name; reconcile removes the tree.
#   9 restart over a populated store: the daemon starts and the start-up
#     wipe empties the env dir.
#  12 a client that disconnects mid-unpack: its staging dir is gone right
#     after the cancel (no reconcile), no tree, no name.
#  13 a duplicate is not bin/true-checked again (daemon log: one "passed
#     its bin/true check" per tree, however many transfers).
#  10 dump_internals shows the store (names, hash12, counters), on the
#     worker and through the scheduler's internals fan-out.
#  11 a daemon started directly as NON-ROOT over a populated store comes
#     up and its start-up wipe empties the store.
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
portC=$((sched_port + 4))
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

# make_env <name> <label> [nocc]: envs/<name>.tar.gz (the client names the
# env by the tarball's basename without the extension).  The important
# entries come FIRST and a 3 MB incompressible pad LAST, so a transfer
# paused at 1 MB has already extracted bin/true, usr/bin/as and usr/bin/gcc.
# Distinct calls give distinct content (label + random pad).  "nocc" leaves
# out usr/bin/gcc: the env installs and verifies, but its compiler cannot
# be exec'd.
make_env() {
    local name=$1 label=$2 root="$workdir/envroot.$1"
    mkdir -p "$root/bin" "$root/usr/bin" "$root/etc" "$workdir/envs"
    cp "$workdir/fakecc" "$root/bin/true"
    cp "$workdir/fakecc" "$root/usr/bin/as"
    [ "${3:-}" = nocc ] || cp "$workdir/fakecc" "$root/usr/bin/gcc"
    echo "$label" > "$root/etc/envstore-label"
    head -c 3000000 /dev/urandom > "$root/zz-padding"
    tar -C "$root" -czf "$workdir/envs/$name.tar.gz" ./bin ./usr ./etc ./zz-padding
    rm -rf "$root"
}

port_of() { case $1 in B) echo "$portB";; C) echo "$portC";; esac; }

# xfer_to <node> <name> <tarball-base> [pause_at marker resume]: transfer
# envs/<tarball-base>.tar.gz to <node> under the env name <name>.
xfer_to() {
    local node=$1 name=$2 file=$3; shift 3
    timeout 180 "$XFER" xfer 127.0.0.1 "$(port_of "$node")" "$arch" "$name" \
        "$workdir/envs/$file.tar.gz" "$@"
}
# xfer <name> [pause_at marker resume]: envs/<name>.tar.gz to node B.
xfer() { local name=$1; shift; xfer_to B "$name" "$name" "$@"; }

# Resolved tree of a name on a node (empty if absent).
tree_on() { [ -e "$workdir/envs$1/target=$arch/$2" ] && readlink -f "$workdir/envs$1/target=$arch/$2"; }
tree_of() { tree_on B "$1"; }
# Published trees / staging dirs (not trash) on a node.
store_trees() { ls "$workdir/envs$1/store" 2>/dev/null | wc -l; }
staging_dirs() { ls "$workdir/envs$1/store/.tmp" 2>/dev/null | grep -vc '^trash\.'; }
tmp_empty() { [ -z "$(ls -A "$workdir/envs$1/store/.tmp" 2>/dev/null)" ]; }
gone() { [ ! -e "$1" ]; }

# compile_on <node> <name> <out>: one remote compile pinned to <node>.
compile_on() {
    local node=$1 name=$2 out=$3
    printf 'int main(void){return 0;}\n' > "$out.c"
    ICECC_TEST_SOCKET="$workdir/A.sock" ICECC_VERSION="$workdir/envs/$name.tar.gz" \
    ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST="envstore$(echo "$node" | tr A-Z a-z)" \
    CCACHE_DISABLE=1 HOME="$workdir/homeA" \
        timeout 180 "$ICECC" /usr/bin/gcc -c "$out.c" -o "$out" > "$out.log" 2>&1
}
compile() { compile_on B "$@"; }
is_fakeobj() { [ "$(head -c 16 "$1" 2>/dev/null | tr -d '\000')" = "FAKEOBJ" ]; }

# ---- farm: scheduler, submitter A, worker B -----------------------------
mkdir -p "$workdir/envsA" "$workdir/envsB" "$workdir/envsC" "$workdir/homeA" "$workdir/homeB" \
    "$workdir/homeC"
chown -R icecc "$workdir"
HOME="$workdir/homeA" "$SCHEDULER" -vvv -n "$netname" -p "$sched_port" \
    -l "$workdir/sched.log" &
pids="$pids $!"
sleep 1
start_daemon() { # <label> <port> <extra args...>
    local label=$1 port=$2; shift 2
    ICECC_TEST_SOCKET="$workdir/$label.sock" HOME="$workdir/home$label" \
        env ${DAEMON_ENV:-} "$ICECCD" -vvv -n "$netname" -s "127.0.0.1:$sched_port" -N "envstore$(echo "$label" | tr A-Z a-z)" \
        -p "$port" -b "$workdir/envs$label" -l "$workdir/daemon$label.log" "$@" &
    pids="$pids $!"
}
start_daemon A "$portA" --no-remote -m 0
start_daemon B "$portB" -m 4
# Node C: a 1 MB cache limit and no keep window, for the eviction leg.
DAEMON_ENV=ICECC_TEST_ENV_KEEP_SECS=-1 start_daemon C "$portC" -m 2 --cache-limit 1
# The daemons must register (and their first stats ping must clear the
# fresh-login load) before anything is submitted.
logins_done() { [ "$(grep -c "login" "$workdir/sched.log" 2>/dev/null)" -ge 3 ]; }
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
check "leg1: X is the store tree named by the tarball's XXH3-128 digest" \
    test "$(tree_of X)" = "$(readlink -f "$workdir/envsB")/store/$("$XFER" digest "$workdir/envs/X.tar.gz")"
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

# ---- leg 3: concurrent duplicates ---------------------------------------
make_env Z z
trees_before=$(store_trees B)
xfer Z 1000000 "$workdir/z1.paused" "$workdir/z.resume" > "$workdir/z1.out" 2>&1 &
z1_pid=$!
xfer Z 1000000 "$workdir/z2.paused" "$workdir/z.resume" > "$workdir/z2.out" 2>&1 &
z2_pid=$!
wait_for 30 test -e "$workdir/z1.paused" -a -e "$workdir/z2.paused" || bad "leg3: transfers never paused"
check "leg3: two staging dirs while both unpack" test "$(staging_dirs B)" -eq 2
touch "$workdir/z.resume"
wait "$z1_pid"; z1_rc=$?
wait "$z2_pid"; z2_rc=$?
check "leg3: both concurrent transfers verified" test "$z1_rc" -eq 0 -a "$z2_rc" -eq 0
check "leg3: exactly one new tree" test "$(store_trees B)" -eq $((trees_before + 1))
check "leg3: no staging dir left" test "$(staging_dirs B)" -eq 0
check "leg3: the loser's copy is removed" wait_for 20 tmp_empty B
compile Z "$workdir/leg3.o"
check "leg3: a compile on Z ran remotely" is_fakeobj "$workdir/leg3.o"

# ---- leg 4: a duplicate of an already installed env ----------------------
x_tree=$(tree_of X)
x_ino=$(stat -c %i "$x_tree")
trees_before=$(store_trees B)
xfer X > "$workdir/x3.out" 2>&1
check "leg4: the duplicate verified" test $? -eq 0
check "leg4: X is the same tree (inode)" \
    test "$(tree_of X)" = "$x_tree" -a "$(stat -c %i "$x_tree")" = "$x_ino"
check "leg4: no new tree" test "$(store_trees B)" -eq "$trees_before"

# ---- leg 5: same name, new content ---------------------------------------
make_env W w1
make_env W2 w2
xfer W > "$workdir/w1.out" 2>&1 || bad "leg5: install of W failed"
w_old=$(tree_of W)
touch "$w_old/tmp/hold"
compile W "$workdir/leg5.o" &
c5_pid=$!
wait_for 60 test -e "$w_old/tmp/fakecc.running" || bad "leg5: the compile never started in W"
xfer_to B W W2 > "$workdir/w2.out" 2>&1
check "leg5: the new content of W verified" test $? -eq 0
w_new=$(tree_of W)
check "leg5: W now points at a different tree" test -d "$w_new" -a "$w_new" != "$w_old"
check "leg5: the re-point was logged loudly" grep -q "W CHANGED CONTENT" "$workdir/daemonB.log"
make_env T1 t1
xfer T1 > /dev/null 2>&1    # an install runs reconcile first
check "leg5: the old tree survives while its compile runs" test -x "$w_old/usr/bin/gcc"
rm -f "$w_old/tmp/hold"
wait "$c5_pid"
check "leg5: the running compile finished in the old tree" is_fakeobj "$workdir/leg5.o"
make_env T2 t2
xfer T2 > /dev/null 2>&1
check "leg5: reconcile removed the old tree after its job ended" wait_for 20 gone "$w_old"
check "leg5: the new tree stays" test -x "$w_new/usr/bin/gcc"
check "leg5: the trashed tree was rm'd (store/.tmp drained)" wait_for 20 tmp_empty B

# ---- leg 6: eviction while a compile runs (node C) -----------------------
make_env E1 e1
make_env E2 e2
make_env E3 e3
make_env E4 e4
xfer_to C E1 E1 > /dev/null 2>&1 || bad "leg6: install of E1 failed"
e1_tree=$(tree_on C E1)
touch "$e1_tree/tmp/hold"
compile_on C E1 "$workdir/leg6.o" &
c6_pid=$!
wait_for 60 test -e "$e1_tree/tmp/fakecc.running" || bad "leg6: the compile never started in E1"
xfer_to C E2 E2 > /dev/null 2>&1 || bad "leg6: install of E2 failed"
e2_tree=$(tree_on C E2)
sleep 1.1
xfer_to C E3 E3 > /dev/null 2>&1 || bad "leg6: install of E3 failed"
e3_tree=$(tree_on C E3)
check "leg6: the running job's name survived the eviction" test "$(tree_on C E1)" = "$e1_tree"
check "leg6: the running job's tree survived" test -x "$e1_tree/usr/bin/gcc"
check "leg6: the idle name E2 was evicted" test -z "$(tree_on C E2)"
check "leg6: E2's tree was deleted" wait_for 20 gone "$e2_tree"
rm -f "$e1_tree/tmp/hold"
wait "$c6_pid"
check "leg6: the compile finished remotely in E1" is_fakeobj "$workdir/leg6.o"
sleep 1.1
xfer_to C E4 E4 > /dev/null 2>&1 || bad "leg6: install of E4 failed"
check "leg6: E1 evicted once its job ended" test -z "$(tree_on C E1)"
check "leg6: E1's tree deleted once its job ended" wait_for 20 gone "$e1_tree"
check "leg6: E3's tree deleted too (over the limit)" wait_for 20 gone "$e3_tree"
check "leg6: the newest install E4 stays" test -n "$(tree_on C E4)"
check "leg6: the evicted trees were rm'd (store/.tmp drained)" wait_for 20 tmp_empty C

# ---- leg 7: an install child killed mid-unpack ---------------------------
make_env K k
xfer K 1000000 "$workdir/k.paused" "$workdir/k.resume" > "$workdir/k.out" 2>&1 &
k_pid=$!
wait_for 30 test -e "$workdir/k.paused" || bad "leg7: transfer never paused"
sleep 1
k_staging=$(ls -d "$workdir/envsB/store/.tmp/"[0-9]* 2>/dev/null | head -1)
k_child=$("$XFER" internals 127.0.0.1 "$portB" | sed -n 's/^ *Child: pid=\([0-9]*\) .*kind=1 .*/\1/p' | head -1)
check "leg7: found the unpack child and its staging dir" test -n "$k_child" -a -d "$k_staging"
[ -n "$k_child" ] && kill -9 "$k_child"
sleep 0.5
touch "$workdir/k.resume"
wait "$k_pid"
check "leg7: the killed install did not verify" test $? -ne 0
check "leg7: K has no name" test -z "$(tree_of K)"
make_env T3 t3
xfer T3 > /dev/null 2>&1    # reconcile runs before this install
check "leg7: the staging dir was swept" wait_for 20 gone "$k_staging"

# ---- leg 8: damage after publish -----------------------------------------
make_env D d nocc
xfer D > "$workdir/d.out" 2>&1 || bad "leg8: install of D failed ($(cat "$workdir/d.out"))"
d_tree=$(tree_of D)
compile D "$workdir/leg8.o"
check "leg8: the compiler could not start (no remote object)" eval '! is_fakeobj "$workdir/leg8.o"'
check "leg8: the damaged name was unlinked" test -z "$(tree_of D)"
check "leg8: the damage was logged" grep -q "could not start its compiler" "$workdir/daemonB.log"
make_env T4 t4
xfer T4 > /dev/null 2>&1
check "leg8: reconcile removed the damaged tree" wait_for 20 gone "$d_tree"
check "leg8: and rm'd it (store/.tmp drained)" wait_for 20 tmp_empty B

# ---- leg 12: a client disconnecting mid-unpack ---------------------------
make_env G g
ENVSTORE_XFER_DISCONNECT=1 xfer G 1000000 "$workdir/g.paused" "$workdir/g.resume" \
    > "$workdir/g.out" 2>&1 &
g_pid=$!
wait_for 30 test -e "$workdir/g.paused" || bad "leg12: transfer never paused"
sleep 1
check "leg12: a staging dir exists while it unpacks" test "$(staging_dirs B)" -eq 1
trees_before=$(store_trees B)
touch "$workdir/g.resume"
wait "$g_pid"
check "leg12: the staging dir is gone right after the cancel" wait_for 5 eval '[ "$(staging_dirs B)" -eq 0 ]'
check "leg12: and its trash is removed" wait_for 20 tmp_empty B
check "leg12: no new tree" test "$(store_trees B)" -eq "$trees_before"
check "leg12: G has no name" test -z "$(tree_of G)"

# ---- leg 13: a duplicate is not checked again -------------------------------
checks_for() { grep -c "($(hash12 "$1")) passed its bin/true check" "$workdir/daemonB.log"; }
hash12() { "$XFER" digest "$workdir/envs/$1.tar.gz" | cut -c1-12; }
make_env V v
xfer V > /dev/null 2>&1 || bad "leg13: install of V failed"
check "leg13: V was checked once when published" test "$(checks_for V)" -eq 1
hits_before=$("$XFER" internals 127.0.0.1 "$portB" | sed -n 's/.*EnvStore:.* duplicate_hits=\([0-9]*\).*/\1/p')
xfer V > "$workdir/v2.out" 2>&1
check "leg13: the duplicate transfer verified (answered from the store)" test $? -eq 0
hits_after=$("$XFER" internals 127.0.0.1 "$portB" | sed -n 's/.*EnvStore:.* duplicate_hits=\([0-9]*\).*/\1/p')
check "leg13: it counted as a duplicate hit" test "$hits_after" -eq $((hits_before + 1))
check "leg13: but was not checked again" test "$(checks_for V)" -eq 1
check "leg13: nor was concurrent duplicate Z (leg 3)" test "$(checks_for Z)" -eq 1
check "leg13: nor was the repeat of X (leg 4)" test "$(checks_for X)" -eq 1

# ---- leg 10: the store in dump_internals (worker and scheduler fan-out) ---
# (before leg 9: the restart resets the counters)
counter() { # <internals file> <name>: the EnvStore counter value
    sed -n "s/.*EnvStore:.* $2=\([0-9]*\).*/\1/p" "$1" | head -1
}
"$XFER" internals 127.0.0.1 "$portB" > "$workdir/internalsB.txt"
"$XFER" internals 127.0.0.1 "$portC" > "$workdir/internalsC.txt"
"$XFER" control 127.0.0.1 $((sched_port + 1)) internals > "$workdir/internalsS.txt"
grep -E "Environments:|Env |EnvStore:" "$workdir/internalsB.txt"
check "leg10: B lists X with its hash12" \
    grep -q "Env $arch/X -> $(hash12 X) size=[1-9][0-9]* last_use_age=[0-9]*s running=0" "$workdir/internalsB.txt"
check "leg10: B lists W with its NEW content's hash12" \
    grep -q "Env $arch/W -> $(hash12 W2) " "$workdir/internalsB.txt"
check "leg10: B store totals" grep -q "Environments: names=[1-9][0-9]* trees=[1-9]" "$workdir/internalsB.txt"
check "leg10: B counts installs" test "$(counter "$workdir/internalsB.txt" installs)" -ge 10
check "leg10: B counts duplicate hits (legs 1-4)" test "$(counter "$workdir/internalsB.txt" duplicate_hits)" -ge 4
check "leg10: B counts the re-point" test "$(counter "$workdir/internalsB.txt" repoints)" -eq 1
check "leg10: B counts the killed unpack" test "$(counter "$workdir/internalsB.txt" unpack_failures)" -ge 1
check "leg10: B counts the damaged name" test "$(counter "$workdir/internalsB.txt" damaged_unlinks)" -eq 1
check "leg10: B counts reconcile runs" test "$(counter "$workdir/internalsB.txt" reconcile_runs)" -ge 10
check "leg10: B counts freed bytes" test "$(counter "$workdir/internalsB.txt" bytes_freed)" -gt 0
check "leg10: C counts evictions" test "$(counter "$workdir/internalsC.txt" evictions)" -ge 3
check "leg10: the scheduler's internals fan-out shows B's section" \
    grep -q "Env $arch/X -> $(hash12 X) " "$workdir/internalsS.txt"
check "leg10: the scheduler's internals fan-out shows C's section" \
    grep -q "Env $arch/E4 -> $(hash12 E4) " "$workdir/internalsS.txt"

# ---- leg 9: restart over a populated store -------------------------------
b_pid=$(echo $pids | cut -d' ' -f3)
stop_processes "$b_pid"
check "leg9: trees are in the store before the restart" test "$(store_trees B)" -gt 0
start_daemon B "$portB" -m 4
sleep 3
b_pid=$(echo $pids | awk '{print $NF}')
check "leg9: the daemon restarted over the store" kill -0 "$b_pid"
# Root-started, but capng drops it to icecc: the wipe ran as icecc.
euid() { awk '/^Uid:/ {print $3}' "/proc/$1/status" 2>/dev/null; }
check "leg9: the restarted daemon runs as icecc" test "$(euid "$b_pid")" = "$(id -u icecc)"
check "leg9: the start-up wipe emptied the store" test "$(store_trees B)" -eq 0
check "leg9: and dropped every name" test -z "$(ls "$workdir/envsB/target=$arch" 2>/dev/null)"

# ---- leg 11: a non-root daemon restarted over a populated store ---------
command -v setpriv > /dev/null || bad "leg11: setpriv missing"
wait_for 30 eval '"$XFER" internals 127.0.0.1 "$portB" > /dev/null 2>&1' || bad "leg11: B not up"
sleep 4    # the restarted B must log in before it accepts a transfer
xfer X > /dev/null 2>&1 || bad "leg11: reinstall of X failed"
x_tree=$(tree_of X)
check "leg11: the store is populated" test "$(store_trees B)" -gt 0 -a -d "$x_tree"
stop_processes "$b_pid"
ICECC_TEST_SOCKET="$workdir/B2.sock" HOME="$workdir/homeB" \
    setpriv --reuid="$(id -u icecc)" --regid="$(id -g icecc)" --clear-groups \
    "$ICECCD" -vvv -n "$netname" -s "127.0.0.1:$sched_port" -N envstoreb -p "$portB" \
    -b "$workdir/envsB" -l "$workdir/daemonB2.log" &
b2_pid=$!
pids="$pids $b2_pid"
sleep 3
check "leg11: the non-root daemon came up over the store" kill -0 "$b2_pid"
check "leg11: it runs as icecc, not root" test "$(euid "$b2_pid")" = "$(id -u icecc)"
check "leg11: no envs-dir cleanup failure" eval '! grep -q "failed to clean up envs dir" "$workdir/daemonB2.log"'
check "leg11: its start-up wipe emptied the store" test "$(store_trees B)" -eq 0
check "leg11: the tree is gone" gone "$x_tree"
check "leg11: and every name" test -z "$(ls "$workdir/envsB/target=$arch" 2>/dev/null)"

if [ "$failures" -ne 0 ]; then
    echo "---- node B log (env lines) ----" >&2
    grep -E "environment|install|removing|envstore|store" "$workdir/daemonB.log" | tail -60 >&2
    echo "envstore: $failures check(s) FAILED" >&2
    exit 1
fi
echo "envstore: all checks passed"
exit 0
