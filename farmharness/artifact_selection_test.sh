#!/bin/sh
# artifact_selection_test.sh -- fail-able gate for farm.py's per-role
# --binary-set-S/-C/-F selection mechanism (S4) AND its successor: the
# fail-closed preflight gate + committed role-manifests/*.json authority +
# idempotent `distribute` subcommand (S4 bounded-successor gaps #1-#5).
#
# WITHOUT starting a full cluster: resolves each role's selected runtime
# root via farm.py's OWN role_tree() function and issues remote commands
# through farm.py's OWN sh() transport (imported directly, never
# reimplemented) -- so this test exercises the exact code path up() and
# run_client() use. It launches that role's executable with a
# version-identity probe inside the pinned container on the target host
# and asserts the reported identity matches the selected set:
#   p43 (tag 1.4, cd74801e0fa4e83e3ae254ca1d7fe98642f36b89) -> "...1.4.0"
#   p50 (trunk,   43297d535232d8becb58866d5fb6cc73fa3b033d) -> "...1.4.92"
# Then, for every role, it flips the selection (p43 <-> p50) and asserts
# the probe output CHANGES. A selection mechanism that only changes a
# printed label while actually launching the same binary would make every
# "flip" assertion below fail -- that is the bug this test exists to catch
# (verified by deliberately introducing exactly that bug into role_tree()
# and confirming this script then reports FAIL; see the S4 report for that
# run's verbatim output).
#
# The sections below extend this with the bounded-successor's own gates:
# distribute idempotency, preflight going green only after distribute,
# a same-version wrong-hash mutant (two independent variants), and a
# skipped-preflight mutant. Every mutation this script introduces (to a
# host's files or to farm.py's own source) is restored and reverified
# before the script exits, on every code path including failure.
set -eu

FARMDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HOST=${ARTIFACT_TEST_HOST:-q3}
IMG=icecream/farm-node:ubuntu22-gcc11-boost174
SCRATCHDIR=${ARTIFACT_TEST_SCRATCH:-/tanksmall/scratch/ictmp/wt-artifacts-scratch}
mkdir -p "$SCRATCHDIR"

fail() { echo "FAIL: $*" >&2; exit 1; }

# probe SET ROLE -- prints the version-identity string reported by ROLE's
# executable when farm.py's role_tree(SET) resolves the runtime root.
# SET is "p43", "p50", or "default" (meaning no --binary-set given, i.e.
# role_tree(None) -- the pre-existing hardcoded TREE).
probe() {
    set_name=$1 role=$2
    python3 - "$set_name" "$role" "$HOST" "$IMG" "$FARMDIR" <<'PY'
import re
import sys

set_name, role, host, img, farmdir = sys.argv[1:6]
sys.path.insert(0, farmdir)
import farm

tree = farm.role_tree(None if set_name == "default" else set_name)
cmds = {
    "S": f"docker run --rm -v {tree}:/probe -u 0:0 {img} "
         f"/probe/obj/scheduler/icecc-scheduler --version",
    "F": f"docker run --rm -v {tree}:/probe -u 0:0 {img} bash -c "
         f"\"strings /probe/obj/daemon/iceccd | "
         f"grep -oE 'ICECREAM daemon [0-9]+\\.[0-9]+(\\.[0-9]+)?' | head -1\"",
    "C": f"docker run --rm -v {tree}:/probe -u 0:0 {img} /probe/obj/client/icecc --version",
}
r = farm.sh(host, cmds[role], timeout=60)
out = (r.stdout + r.stderr).strip()
if role == "S":
    m = re.search(r"ICECREAM scheduler [0-9]+\.[0-9]+(\.[0-9]+)?", out)
    out = m.group(0) if m else out
print(out)
PY
}

assert_contains() {
    haystack=$1 needle=$2 label=$3
    case "$haystack" in
        *"$needle"*) echo "ok - $label ($haystack)" ;;
        *) fail "$label: expected to contain '$needle', got '$haystack'" ;;
    esac
}

assert_differs() {
    a=$1 b=$2 label=$3
    if [ "$a" = "$b" ]; then
        fail "$label: selection flip did not change the probe output ('$a' both times) -- the mechanism only changed a label, not the launched binary"
    fi
    echo "ok - $label ('$a' != '$b')"
}

echo "== source anchor: the launch path cannot skip preflight without this test noticing =="
# EXACT count per function, not mere presence -- up() has two call sites
# (scheduler role + the per-worker F role inside the loop), so a coarse
# "is resolve_role called at all" check would miss one of the two being
# deleted. run_client() has one (the C role).
for spec in "up:2" "run_client:1"; do
    fn=${spec%%:*}
    want=${spec##*:}
    got=$(python3 - "$FARMDIR/farm.py" "$fn" <<'PY'
import ast, sys
path, fname = sys.argv[1], sys.argv[2]
tree = ast.parse(open(path).read(), filename=path)
func = next(n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef) and n.name == fname)
calls = [n for n in ast.walk(func) if isinstance(n, ast.Call)
         and isinstance(n.func, ast.Name) and n.func.id == "resolve_role"]
print(len(calls))
PY
)
    if [ "$got" = "$want" ]; then
        echo "ok - $fn() calls resolve_role() exactly $want time(s) (preflight cannot be silently skipped for any role)"
    else
        fail "$fn() calls resolve_role() $got time(s), expected $want -- a selected --binary-set would launch with NO preflight for at least one role"
    fi
done

echo
echo "== distribute: idempotent materialization onto research6/research7/q2 =="
DIST_OUT1=$(python3 "$FARMDIR/farm.py" distribute --sets p43,p50 --hosts research6,research7,q2 2>&1)
DIST_RC1=$?
echo "$DIST_OUT1"
[ "$DIST_RC1" = "0" ] || fail "distribute (run 1) exited $DIST_RC1"
echo "ok - distribute run 1 exit 0"

DIST_OUT2=$(python3 "$FARMDIR/farm.py" distribute --sets p43,p50 --hosts research6,research7,q2 2>&1)
DIST_RC2=$?
echo "$DIST_OUT2"
[ "$DIST_RC2" = "0" ] || fail "distribute (run 2) exited $DIST_RC2"
case "$DIST_OUT2" in
    *bootstrapped*|*repaired*) fail "distribute run 2 performed a transfer -- not idempotent: $DIST_OUT2" ;;
esac
already=$(printf '%s\n' "$DIST_OUT2" | grep -c 'already-current')
[ "$already" = "6" ] || fail "distribute run 2: expected 6 already-current lines (2 sets x 3 hosts), got $already"
echo "ok - distribute run 2 is a pure no-op (6/6 already-current, no transfer)"

echo
echo "== preflight goes green after distribute (research6, q2 -- research7 has a separate, real, pre-existing image-digest gap; see report) =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm

bad = []
for host in ("research6", "q2"):
    for binary_set in ("p43", "p50"):
        for role in ("S", "F", "C"):
            err = farm.preflight(host, binary_set, role)
            print(f"  preflight {host}/{binary_set}/{role}: {'PASS' if err is None else 'FAIL: ' + err}")
            if err:
                bad.append((host, binary_set, role, err))
if bad:
    print(f"FAIL: {len(bad)} preflight check(s) failed after distribute", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "preflight is not green on research6/q2 after distribute"
echo "ok - preflight green on research6 and q2 for both sets, all 3 roles (12/12)"

echo
echo "== research7 image-digest gap: real, pre-existing, correctly refused (not a synthetic mutant) =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm
err = farm.preflight("research7", "p43", "S")
if err is None:
    print("FAIL: preflight passed on research7 despite the known image-digest mismatch", file=sys.stderr)
    sys.exit(1)
if "image digest mismatch" not in err:
    print(f"FAIL: preflight refused research7 for the wrong reason: {err}", file=sys.stderr)
    sys.exit(1)
print(f"ok - preflight correctly refuses research7 by IMAGE DIGEST: {err}")
PY
[ $? -eq 0 ] || fail "research7 image-digest refusal check did not pass"

echo
echo "== preflight mutant (variant A -- isolated copy): same-version wrong-hash binary must be refused BY HASH =="
python3 - "$HOST" "$FARMDIR" <<'PY'
import subprocess
import sys

host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm

mutant_dir = "~/role-artifacts/p43-root-selftest-mutant"
farm.sh(host, f"rm -rf {mutant_dir} && cp -r ~/role-artifacts/p43-root {mutant_dir}", check=True)
# trivial touch: append one printable byte -- same embedded version string
# (it's earlier in the file), different sha256 for the whole file
farm.sh(host, f"printf 'X' >> {mutant_dir}/obj/scheduler/icecc-scheduler", check=True)
try:
    farm.ROLE_SET_DIR["p43"] = mutant_dir
    err = farm.preflight(host, "p43", "S")
    if err is None:
        print("FAIL: preflight accepted a wrong-hash same-version binary", file=sys.stderr)
        sys.exit(1)
    if "sha256 mismatch" not in err:
        print(f"FAIL: preflight refused for the wrong reason: {err}", file=sys.stderr)
        sys.exit(1)
    print(f"ok - preflight refused the wrong-hash mutant by HASH: {err}")
finally:
    farm.ROLE_SET_DIR["p43"] = "~/role-artifacts/p43-root"
    farm.sh(host, f"rm -rf {mutant_dir}")
PY
[ $? -eq 0 ] || fail "preflight hash-mutant test (variant A) did not pass"

echo
echo "== preflight mutant (variant B -- real host, corrupt-then-repair): research6's DISTRIBUTED copy, corrupted in place, must redden, then 'distribute' must repair it =="
python3 - "research6" "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
import farm

host = sys.argv[1]
target = "~/role-artifacts/p43-root/obj/client/icecc"

# 0) baseline: research6 is known-green for p43/C going into this (verified
#    by the "preflight goes green after distribute" section above).
err0 = farm.preflight(host, "p43", "C")
if err0:
    print(f"FAIL: research6 was not green before the corrupt step: {err0}", file=sys.stderr)
    sys.exit(1)
print("  step0: research6 p43/C preflight PASS (pre-corruption baseline)")

try:
    # 1) corrupt the REAL distributed copy in place (append one byte --
    #    same embedded version string, different sha256).
    r = farm.sh(host, f"printf 'X' >> {target}", check=True)

    # 2) preflight must now REDDEN, naming this exact file's hash mismatch.
    err1 = farm.preflight(host, "p43", "C")
    if err1 is None:
        print("FAIL: preflight accepted the corrupted research6 copy", file=sys.stderr)
        sys.exit(1)
    if "obj/client/icecc" not in err1 or "sha256 mismatch" not in err1:
        print(f"FAIL: preflight refused for the wrong reason: {err1}", file=sys.stderr)
        sys.exit(1)
    print(f"  step1: preflight REDDENED as required: {err1}")

    # 3) re-run distribute -- must detect the drift and repair it.
    derr, dstatus = farm.distribute_role_artifacts(host, "p43")
    if derr:
        print(f"FAIL: distribute could not repair research6: {derr}", file=sys.stderr)
        sys.exit(1)
    if not dstatus.startswith("repaired"):
        print(f"FAIL: distribute did not report a repair (got status={dstatus!r}) -- "
              f"either it wrongly no-op'd on corrupted content, or something else is off", file=sys.stderr)
        sys.exit(1)
    print(f"  step2: distribute repaired it: {dstatus}")

    # 4) preflight must be green again.
    err2 = farm.preflight(host, "p43", "C")
    if err2:
        print(f"FAIL: preflight still red after repair: {err2}", file=sys.stderr)
        sys.exit(1)
    print("  step3: research6 p43/C preflight PASS again (post-repair)")

    # 5) idempotence of the repair itself: a THIRD distribute call must be
    #    a pure no-op now.
    derr2, dstatus2 = farm.distribute_role_artifacts(host, "p43")
    if derr2 or dstatus2 != "already-current":
        print(f"FAIL: post-repair distribute was not a clean no-op (err={derr2!r} status={dstatus2!r})", file=sys.stderr)
        sys.exit(1)
    print("  step4: post-repair re-run is already-current (repair itself is idempotent)")
    print("ok - corrupt-then-repair-then-reverify-idempotent cycle passed on the REAL research6 host")
except BaseException:
    # Unconditional safety net: whatever happened above, make sure research6
    # ends this test in a known-good, hash-verified state -- never leave a
    # corrupted file on a shared host.
    derr, dstatus = farm.distribute_role_artifacts(host, "p43")
    print(f"  cleanup-on-exception: distribute -> err={derr!r} status={dstatus!r}", file=sys.stderr)
    raise
PY
[ $? -eq 0 ] || fail "preflight hash-mutant test (variant B, real host) did not pass"

echo
echo "== post-mutant repair verification: research6 hash-clean for BOTH sets, all roles =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm
bad = []
for binary_set in ("p43", "p50"):
    for role in ("S", "F", "C"):
        err = farm.preflight("research6", binary_set, role)
        if err:
            bad.append((binary_set, role, err))
if bad:
    print(f"FAIL: research6 not fully green post-repair: {bad}", file=sys.stderr)
    sys.exit(1)
print("ok - research6 fully green post-repair (6/6)")
PY
[ $? -eq 0 ] || fail "research6 not restored to a fully green state"

echo
echo "== skipped-preflight mutant: neutralizing resolve_role() in up() must make the PREFLIGHT-OK marker for that role DISAPPEAR =="
# Safety: this mutates farm.py's OWN source on disk, then imports the
# mutated module in a fresh subprocess. It NEVER calls a code path that
# performs a real docker/ssh mutating action -- sh() is monkeypatched so
# that the moment execution would touch a farm-sched/farm-worker/farm-client
# container, the call is intercepted and faked instead of reaching the
# network. farm.py itself is always restored (cmp-verified) before this
# block exits, on every path, via the trap below.
SNAPSHOT="$SCRATCHDIR/farm.py.pre-mutant-snapshot"
cp "$FARMDIR/farm.py" "$SNAPSHOT"
restore_farm_py() {
    if [ -f "$SNAPSHOT" ]; then
        cp "$SNAPSHOT" "$FARMDIR/farm.py"
    fi
}
trap restore_farm_py EXIT

DYNAMIC_PROBE='
import contextlib, io, sys
sys.path.insert(0, sys.argv[1])
import farm

class FakeResult:
    returncode = 0
    stdout = ""
    stderr = ""

class FakeTime:
    @staticmethod
    def sleep(s):
        pass

def run_mocked_up():
    real_sh = farm.sh
    launch_actions = []
    # Pure per-command content match -- NOT sticky state. A sticky
    # "once launch starts, fake everything" flag is wrong here: up()
    # calls resolve_role() again for EACH worker role, interleaved with
    # the launch actions for the scheduler role, and each of those
    # resolve_role() calls must still reach the REAL, read-only preflight
    # checks (or this harness itself would falsely blind preflight for
    # every role after the first). These six tokens cover every command
    # up() issues once actually launching/tearing down/polling a container
    # (docker_rm uses the "farm-{sched,worker,client}" name, so does the
    # docker run --name of same, plus the scratch chmod/mkdir prep and the
    # sched.log/worker.log polling) and appear in none of the commands
    # preflight() itself issues (sha256sum/stat on role-artifacts paths,
    # docker image inspect, docker run --rm --v .../probe with no --name
    # and no scratch mount).
    LAUNCH_TOKENS = ("farm-sched", "farm-worker", "farm-client", "sched.log", "worker.log", "chmod 1777")
    def fake_sh(host, cmd, timeout=120, check=False):
        if any(tag in cmd for tag in LAUNCH_TOKENS):
            launch_actions.append((host, cmd))
            return FakeResult()
        return real_sh(host, cmd, timeout=timeout, check=check)
    farm.sh = fake_sh
    farm.time = FakeTime()
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try:
            farm.up(["q3"], "p43", "p43")
        except RuntimeError as exc:
            print(f"(up raised RuntimeError: {exc})")
    return buf.getvalue(), launch_actions

out, actions = run_mocked_up()
sys.stdout.write(out)
print(f"__LAUNCH_ACTIONS__={len(actions)}", file=sys.stderr)
'

echo "-- baseline (unmutated): both S and F PREFLIGHT-OK markers must appear --"
BASE_STDOUT=$(python3 -c "$DYNAMIC_PROBE" "$FARMDIR" 2>"$SCRATCHDIR/base.stderr")
echo "$BASE_STDOUT" | grep -q 'PREFLIGHT-OK host=q3 role=S set=p43' || fail "baseline: expected S-role PREFLIGHT-OK marker missing"
echo "$BASE_STDOUT" | grep -q 'PREFLIGHT-OK host=q3 role=F set=p43' || fail "baseline: expected F-role PREFLIGHT-OK marker missing"
BASE_ACTIONS=$(grep -oE '__LAUNCH_ACTIONS__=[0-9]+' "$SCRATCHDIR/base.stderr" | cut -d= -f2)
[ "${BASE_ACTIONS:-0}" -gt 0 ] || fail "baseline: mocked launch actions were never reached (harness itself is broken)"
echo "ok - baseline: both PREFLIGHT-OK markers present, $BASE_ACTIONS mocked launch action(s) reached after them"

echo "-- mutating farm.py: neutralize the S-role resolve_role() call inside up() --"
python3 - "$FARMDIR/farm.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = '    s_tree, s_img = resolve_role(SCHED_HOST, binary_set_s, "S")\n'
new = '    s_tree, s_img = role_tree(binary_set_s), IMG  # MUTATED-OUT-FOR-TEST (S preflight skipped)\n'
count = src.count(old)
if count != 1:
    print(f"FAIL: expected exactly 1 occurrence of the S-role resolve_role() call, found {count}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print("mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the skipped-preflight source mutation"

echo "-- mutated: the S-role PREFLIGHT-OK marker must now be ABSENT (F-role, untouched, must still appear -- precise detection, not a blanket breakage) --"
MUT_STDOUT=$(python3 -c "$DYNAMIC_PROBE" "$FARMDIR" 2>"$SCRATCHDIR/mut.stderr")
if echo "$MUT_STDOUT" | grep -q 'PREFLIGHT-OK host=q3 role=S set=p43'; then
    fail "mutated farm.py still emitted the S-role PREFLIGHT-OK marker -- mutation had no effect, test is not exercising the intended code path"
fi
echo "$MUT_STDOUT" | grep -q 'PREFLIGHT-OK host=q3 role=F set=p43' || fail "mutated: F-role marker unexpectedly also disappeared (mutation too broad)"
MUT_ACTIONS=$(grep -oE '__LAUNCH_ACTIONS__=[0-9]+' "$SCRATCHDIR/mut.stderr" | cut -d= -f2)
[ "${MUT_ACTIONS:-0}" -gt 0 ] || fail "mutated: expected mocked launch actions to still be reached (unguarded) -- got none, harness state suspect"
echo "ok - REDDENED as required: S-role PREFLIGHT-OK marker is missing, yet execution still reached $MUT_ACTIONS launch action(s) unguarded -- exactly the gap this test exists to catch"

echo "-- restoring farm.py and reverifying byte-exact restoration --"
restore_farm_py
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT"; then
    echo "ok - farm.py restored byte-exact (cmp clean)"
else
    fail "farm.py restoration is NOT byte-exact vs the pre-mutant snapshot"
fi

echo "-- reconfirming PASS after restoration --"
RESTORED_STDOUT=$(python3 -c "$DYNAMIC_PROBE" "$FARMDIR" 2>/dev/null)
echo "$RESTORED_STDOUT" | grep -q 'PREFLIGHT-OK host=q3 role=S set=p43' || fail "post-restore: S-role PREFLIGHT-OK marker still missing"
echo "$RESTORED_STDOUT" | grep -q 'PREFLIGHT-OK host=q3 role=F set=p43' || fail "post-restore: F-role PREFLIGHT-OK marker missing"
echo "ok - post-restore: both PREFLIGHT-OK markers present again"

trap - EXIT
rm -f "$SCRATCHDIR/base.stderr" "$SCRATCHDIR/mut.stderr" "$SNAPSHOT"

echo
echo "== identity baseline: each (role, set) reports its own version =="
for role in S F C; do
    p43_out=$(probe p43 "$role")
    p50_out=$(probe p50 "$role")
    assert_contains "$p43_out" "1.4.0" "role=$role set=p43 reports P43 identity"
    assert_contains "$p50_out" "1.4.92" "role=$role set=p50 reports trunk identity"
done

echo
echo "== selection-flip mutation: same role, opposite set, output MUST change =="
for role in S F C; do
    p43_out=$(probe p43 "$role")
    p50_out=$(probe p50 "$role")
    assert_differs "$p43_out" "$p50_out" "role=$role flip p43<->p50 changes the launched binary's reported identity"
done

echo
echo "PASS: artifact_selection_test -- selection mechanism changes the launched binary, not merely its label; preflight is fail-closed and wired into the real launch path; distribute is idempotent and repair-capable"
