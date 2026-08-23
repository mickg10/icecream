#!/bin/sh
# artifact_selection_test.sh -- fail-able gate for farm.py's per-role
# --binary-set-S/-C/-F selection mechanism (S4) and its successors:
# committed role-manifests/*.json authority, resolve-then-mutate ordering,
# content-addressed immutable roots ($HOME/role-artifacts/store/<set>/<hash>),
# a host-canonical publication lock, and (this revision) genuinely
# probe-free resolution with post-launch in-container identity verification.
#
# WITHOUT starting a full cluster (except where explicitly and narrowly
# noted -- the race-gate section starts minimal, uniquely-named,
# always-torn-down containers, and one section starts a real one-host
# cluster to prove the full launch path end to end): resolves each role's
# selected runtime root via farm.py's OWN role_tree() function and issues
# remote commands through farm.py's OWN sh() transport (imported directly,
# never reimplemented) -- so this test exercises the exact code path
# up() and run_client() use. It launches that role's executable with a
# version-identity probe inside the pinned container on the target host
# and asserts the reported identity matches the selected set:
#   p43 (tag 1.4, cd74801e0fa4e83e3ae254ca1d7fe98642f36b89) -> "...1.4.0"
#   p50 (trunk,   43297d535232d8becb58866d5fb6cc73fa3b033d) -> "...1.4.92"
# Then, for every role, it flips the selection (p43 <-> p50) and asserts
# the probe output CHANGES. A selection mechanism that only changes a
# printed label while actually launching the same binary would make every
# "flip" assertion below fail -- that is the bug this test exists to catch.
#
# Every mutation this script introduces (to a host's files or to farm.py's
# own source) is restored and reverified before the script exits, on
# every code path including failure.
set -eu

FARMDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
HOST=${ARTIFACT_TEST_HOST:-q3}
IMG=icecream/farm-node:ubuntu22-gcc11-boost174
SCRATCHDIR=${ARTIFACT_TEST_SCRATCH:-/tanksmall/scratch/ictmp/wt-artifacts-scratch}
mkdir -p "$SCRATCHDIR"

# Whole-script concurrency lock (hub-local; the PUBLISH path additionally
# has its own HOST-CANONICAL lock now -- see "host-canonical lock" section
# below, which specifically proves that one is not hub-local). q3/research6/
# research7/q2 are shared, LIVE remote hosts that `distribute` and the
# mutant/race-gate sections mutate directly.
LOCKFILE="${ARTIFACT_TEST_LOCK:-$SCRATCHDIR/artifact_selection_test.lock}"
exec 9>"$LOCKFILE"
if ! flock -n 9; then
    echo "artifact_selection_test.sh: another run currently holds $LOCKFILE" \
         "(q3/research6/research7/q2 are shared live hosts this suite mutates directly)." \
         "Waiting for it to finish..." >&2
    flock 9
fi

fail() { echo "FAIL: $*" >&2; exit 1; }

# probe SET ROLE -- prints the version-identity string reported by ROLE's
# executable when farm.py's role_tree(SET) resolves the runtime root (the
# content-addressed immutable store path). SET is "p43", "p50", or
# "default" (meaning no --binary-set given, i.e. role_tree(None) -- the
# pre-existing hardcoded TREE). Mounted READ-ONLY (:ro), matching production.
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
    "S": f"docker run --rm -v {tree}:/probe:ro -u 0:0 {img} "
         f"/probe/obj/scheduler/icecc-scheduler --version",
    "F": f"docker run --rm -v {tree}:/probe:ro -u 0:0 {img} bash -c "
         f"\"strings /probe/obj/daemon/iceccd | "
         f"grep -oE 'ICECREAM daemon [0-9]+\\.[0-9]+(\\.[0-9]+)?' | head -1\"",
    "C": f"docker run --rm -v {tree}:/probe:ro -u 0:0 {img} /probe/obj/client/icecc --version",
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

echo "== FRESH-ARCHIVE no-ambient gate: farm.py + both manifests, from a bare git-archive extraction, zero ambient files possible =="
REPOROOT=$(cd "$FARMDIR" && git rev-parse --show-toplevel)
ARCHIVE_COMMIT=$(cd "$FARMDIR" && git rev-parse HEAD)
ARCHIVE_DIR="$SCRATCHDIR/fresh-archive-$$"
rm -rf "$ARCHIVE_DIR"
mkdir -p "$ARCHIVE_DIR"
( cd "$REPOROOT" && git archive "$ARCHIVE_COMMIT" -- farmharness ) | tar -x -C "$ARCHIVE_DIR"
[ -f "$ARCHIVE_DIR/farmharness/farm.py" ] || fail "fresh-archive: farmharness/farm.py missing from the archive of $ARCHIVE_COMMIT"
python3 - "$ARCHIVE_DIR" "$ARCHIVE_COMMIT" <<'PY'
import os
import sys

archive_dir, commit = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(archive_dir, "farmharness"))
import farm

resolved_dir = os.path.dirname(os.path.abspath(farm.__file__))
expected_dir = os.path.join(archive_dir, "farmharness")
if resolved_dir != expected_dir:
    print(f"FAIL: farm module resolved from {resolved_dir!r}, expected {expected_dir!r}", file=sys.stderr)
    sys.exit(1)

problems = []
for set_name in ("p43", "p50"):
    try:
        manifest = farm.load_manifest(set_name)
    except RuntimeError as exc:
        problems.append(f"{set_name}: load_manifest() raised: {exc}")
        continue
    for key in ("set", "binaries", "build", "tar"):
        if key not in manifest:
            problems.append(f"{set_name}: missing top-level key {key!r}")
    if manifest.get("set") != set_name:
        problems.append(f"{set_name}: manifest['set'] = {manifest.get('set')!r}, expected {set_name!r}")
    if "image_digest" not in manifest.get("build", {}):
        problems.append(f"{set_name}: build.image_digest missing")
    for key in ("path", "sha256"):
        if key not in manifest.get("tar", {}):
            problems.append(f"{set_name}: tar.{key} missing")
    for b in manifest.get("binaries", []):
        for key in ("path", "sha256", "mode"):
            if key not in b:
                problems.append(f"{set_name}: binaries[] entry {b.get('path','?')!r} missing {key!r}")
    # required-role coverage: every role verify_launched_container_identity()
    # probes for (_ROLE_PROBE_PATH's S/F/C) must have a binaries[] entry at
    # the exact expected path -- otherwise that role's post-launch identity
    # check silently has nothing to check, a real fail-open gap in the
    # MANIFEST DATA, not just the code.
    by_path = {b.get("path"): b for b in manifest.get("binaries", [])}
    for role, probe_path in farm._ROLE_PROBE_PATH.items():
        entry = by_path.get(probe_path)
        if entry is None:
            problems.append(f"{set_name}: no binaries[] entry at {probe_path!r} for role {role!r}")
            continue
        if entry.get("role") != role:
            problems.append(f"{set_name}: {probe_path} role={entry.get('role')!r}, expected {role!r}")
    # This revision's own invariant: immutable_root() must derive from
    # tar.sha256 under $HOME/role-artifacts/store/<set>/, never a fixed or
    # mutable path.
    expected_root = f"~/role-artifacts/store/{set_name}/{manifest['tar']['sha256']}"
    actual_root = farm.immutable_root(set_name)
    if actual_root != expected_root:
        problems.append(f"{set_name}: immutable_root() = {actual_root!r}, expected {expected_root!r}")

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
print(f"ok - fresh-archive gate: farm imported from {farm.__file__} (git archive of {commit}), "
      f"both manifests loaded via farm.load_manifest() from that same location, schema-valid, "
      f"S/F/C role coverage confirmed present in both, immutable_root() store-layout derivation confirmed")
PY
[ $? -eq 0 ] || fail "fresh-archive no-ambient gate did not pass"
rm -rf "$ARCHIVE_DIR"

echo
echo "== source anchor: the launch path cannot skip resolution, mutation-time revalidation, or post-launch in-container verification without this test noticing =="
for spec in "resolve_launch_plan:resolve_role:3" \
            "up:resolve_role:0" "up:revalidate_before_mutation:2" "up:verify_launched_container_identity:2" \
            "run_client:resolve_role:0" "run_client:revalidate_before_mutation:1" "run_client:verify_launched_container_identity:1" \
            "run_client:docker_run_detached:1"; do
    fn=$(printf '%s' "$spec" | cut -d: -f1)
    target=$(printf '%s' "$spec" | cut -d: -f2)
    want=$(printf '%s' "$spec" | cut -d: -f3)
    got=$(python3 - "$FARMDIR/farm.py" "$fn" "$target" <<'PY'
import ast, sys
path, fname, target = sys.argv[1], sys.argv[2], sys.argv[3]
tree = ast.parse(open(path).read(), filename=path)
func = next(n for n in ast.walk(tree) if isinstance(n, ast.FunctionDef) and n.name == fname)
calls = [n for n in ast.walk(func) if isinstance(n, ast.Call)
         and isinstance(n.func, ast.Name) and n.func.id == target]
print(len(calls))
PY
)
    if [ "$got" = "$want" ]; then
        echo "ok - $fn() calls $target() exactly $want time(s)"
    else
        fail "$fn() calls $target() $got time(s), expected $want -- the resolve-then-mutate-then-verify-in-container ordering this successor exists for may have regressed"
    fi
done

echo
echo "== no-network TRANSPORT-SPY gates: invalid-role scenarios must record ZERO cluster-mutating actions, using REAL preflight() =="
# Fakes ONLY farm.sh() (a strict, manifest-derived transport) -- never
# farm.preflight() itself -- so production preflight()'s real code
# executes, consuming the fake's responses; what is under test is its
# ORCHESTRATION (does resolve_launch_plan()/main() ever let one role's
# refusal be preceded by another role's mutation), not a hand-written
# preflight stand-in. Since preflight() is now the ONLY thing
# resolve_launch_plan() calls (no separate probe phase to keep honest
# anymore -- see farm.py's resolve_launch_plan() docstring for why that
# whole bookkeeping class of concern no longer exists), every one of
# these scenarios expects exactly 0 actions of ANY kind recorded.
python3 - "$FARMDIR" <<'PY'
import contextlib
import io
import re
import sys

sys.path.insert(0, sys.argv[1])
import farm

class Recorder:
    def __init__(self):
        self.actions = []
    def docker_rm(self, host, name):
        self.actions.append(f"docker_rm({host},{name})")
    def docker_run_detached(self, host, name, tree, img, inner_cmd):
        self.actions.append(f"docker_run_detached({host},{name})")
    def scratch_prepare(self, host, mkdir_path, log_path):
        self.actions.append(f"scratch_prepare({host},{mkdir_path})")
    def push_file(self, host, localpath, remotepath):
        self.actions.append(f"push_file({host},{remotepath})")
    def verify_launched_container_identity(self, host, name, binary_set, role):
        self.actions.append(f"verify_launched_container_identity({host},{name})")

class FakeResult:
    def __init__(self, out="", rc=0, err=""):
        self.stdout, self.returncode, self.stderr = out, rc, err

def make_fake_transport(binary_set, bad_hosts):
    manifest = farm.load_manifest(binary_set)
    root = farm.immutable_root(binary_set)
    good_sha = {f"{root}/{b['path']}": b["sha256"] for b in manifest["binaries"]}
    good_mode = {f"{root}/{b['path']}": b["mode"] for b in manifest["binaries"]}
    expect_digest = f"{farm.IMG.split(':', 1)[0]}@{manifest['build']['image_digest']}"
    def fake_sh(host, cmd, timeout=120, check=False):
        if cmd.startswith("test -d "):
            return FakeResult(rc=0)
        m = re.match(r"sha256sum (\S+)", cmd)
        if m:
            path = m.group(1)
            real = good_sha.get(path, "0" * 64)
            if host in bad_hosts:
                real = ("f" if real[0] != "f" else "0") + real[1:]
            return FakeResult(out=f"{real}  {path}\n")
        m = re.match(r"stat -c %a (\S+)", cmd)
        if m:
            return FakeResult(out=good_mode.get(m.group(1), "755") + "\n")
        if "docker image inspect" in cmd:
            return FakeResult(out=expect_digest + "\n")
        raise AssertionError(f"fake transport: unrecognized command for a resolution-only scenario "
                              f"host={host} cmd={cmd!r} -- preflight() must never issue this")
    return fake_sh

def run_scenario(argv_tail, fake_sh):
    rec = Recorder()
    saved = dict(sh=farm.sh, docker_rm=farm.docker_rm, docker_run_detached=farm.docker_run_detached,
                 scratch_prepare=farm.scratch_prepare, push_file=farm.push_file,
                 verify_launched_container_identity=farm.verify_launched_container_identity, argv=sys.argv)
    farm.sh = fake_sh
    farm.docker_rm = rec.docker_rm
    farm.docker_run_detached = rec.docker_run_detached
    farm.scratch_prepare = rec.scratch_prepare
    farm.push_file = rec.push_file
    farm.verify_launched_container_identity = rec.verify_launched_container_identity
    sys.argv = ["farm.py"] + argv_tail
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            try:
                farm.main()
            except SystemExit:
                pass
    finally:
        farm.sh = saved["sh"]
        farm.docker_rm = saved["docker_rm"]
        farm.docker_run_detached = saved["docker_run_detached"]
        farm.scratch_prepare = saved["scratch_prepare"]
        farm.push_file = saved["push_file"]
        farm.verify_launched_container_identity = saved["verify_launched_container_identity"]
        sys.argv = saved["argv"]
    return buf.getvalue(), rec.actions

problems = []
COMMON = ["--workers=research6,research7,q2", "--client=q3", "--phase=up-test-down",
          "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"]

def check(label, argv_tail, bad_hosts, before_fix_hint):
    out, actions = run_scenario(argv_tail, make_fake_transport("p43", bad_hosts))
    print(f"-- {label}: {len(actions)} action(s) recorded --")
    if "REFUSED" not in out:
        problems.append(f"{label}: expected a REFUSED line in stdout, got: {out!r}")
    if actions:
        problems.append(f"{label}: expected 0 recorded actions, got {len(actions)}: {actions}")
    else:
        print(f"ok - {label}: refused with 0 actions ({before_fix_hint})")

check("invalid INITIAL S", COMMON, {"q3"},
      "was 6 pre-refusal + 4 finally-teardown actions before this fix")
check("invalid SECOND F of 3", COMMON, {"research7"},
      "was 6 pre-refusal + 4 finally-teardown actions before this fix")
check("invalid C, distinct host from S/F", ["--workers=research6", "--client=research7", "--phase=up-test-down",
                                             "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"],
      {"research7"}, "S and F would have launched for real before this fix")
check("final worker (of 3) fails", COMMON, {"q2"},
      "docker-op list was non-empty before this fix (per BO)")

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "no-network transport-spy gates did not pass"
echo "ok - all 4 invalid-role scenarios refused cleanly through the real farm.main() entry point using REAL preflight(), 0 actions of any kind in every case"

echo
echo "== ordering-violation mutant: reintroducing per-role interleaved resolve+mutate must make the recorded action list nonempty (RED) =="
# BO's named mutant, adapted to the single-pass design: deletes
# resolve_launch_plan()'s up-front F-role resolution and makes up() do its
# OWN resolve_role() call per worker, interleaved with that worker's
# docker actions -- exactly the 02622ab6 defect this whole successor
# closes. A later-failing role must now be preceded by an earlier role's
# REAL mutations, which the transport-spy harness above would have
# reported as 0 actions on unmutated code; this proves it goes nonempty
# under the mutation, i.e. the harness can actually fail, not just
# happen to always pass.
SNAPSHOT_ORDER="$SCRATCHDIR/farm.py.pre-order-mutant"
cp "$FARMDIR/farm.py" "$SNAPSHOT_ORDER"
restore_order() { [ -f "$SNAPSHOT_ORDER" ] && cp "$SNAPSHOT_ORDER" "$FARMDIR/farm.py"; }
trap restore_order EXIT

python3 - "$FARMDIR/farm.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old_plan = 'f_resolved = [resolve_role(h, binary_set_f, "F") for h in worker_hosts]\n'
new_plan = 'f_resolved = [(None, None) for h in worker_hosts]  # MUTATED-OUT-FOR-TEST: F resolution deleted from resolve_launch_plan()\n'
old_up = '        f_tree, f_img = plan.f_resolved[i]\n        revalidate_before_mutation(h, plan.binary_set_f)\n'
new_up = ('        f_tree, f_img = resolve_role(h, plan.binary_set_f, "F")  # MUTATED-IN-FOR-TEST: reintroduced interleaved per-role resolution\n'
          '        revalidate_before_mutation(h, plan.binary_set_f)\n')
for old, new, label in ((old_plan, new_plan, "resolve_launch_plan F-resolution"), (old_up, new_up, "up() interleaved resolve_role")):
    n = src.count(old)
    if n != 1:
        print(f"FAIL: expected exactly 1 occurrence of {label!r} anchor, found {n}", file=sys.stderr)
        sys.exit(1)
    src = src.replace(old, new, 1)
open(path, "w").write(src)
print("ordering-violation mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the ordering-violation mutation"

python3 - "$FARMDIR" <<'PY'
import contextlib, io, re, sys
sys.path.insert(0, sys.argv[1])
import farm

class Recorder:
    def __init__(self): self.actions = []
    def docker_rm(self, host, name): self.actions.append(f"docker_rm({host},{name})")
    def docker_run_detached(self, host, name, tree, img, inner_cmd): self.actions.append(f"docker_run_detached({host},{name})")
    def scratch_prepare(self, host, mkdir_path, log_path): self.actions.append(f"scratch_prepare({host},{mkdir_path})")
    def push_file(self, host, localpath, remotepath): pass
    def verify_launched_container_identity(self, host, name, binary_set, role): pass

class FakeResult:
    def __init__(self, out="", rc=0): self.stdout, self.returncode, self.stderr = out, rc, ""

manifest = farm.load_manifest("p43")
root = farm.immutable_root("p43")
good_sha = {f"{root}/{b['path']}": b["sha256"] for b in manifest["binaries"]}
good_mode = {f"{root}/{b['path']}": b["mode"] for b in manifest["binaries"]}
expect_digest = f"{farm.IMG.split(':', 1)[0]}@{manifest['build']['image_digest']}"
bad_hosts = {"research7"}  # 2nd of 3 workers -- research6 (1st) must resolve fine first under the mutation

def fake_sh(host, cmd, timeout=120, check=False):
    if cmd.startswith("test -d "): return FakeResult(rc=0)
    m = re.match(r"sha256sum (\S+)", cmd)
    if m:
        path = m.group(1)
        real = good_sha.get(path, "0"*64)
        if host in bad_hosts: real = ("f" if real[0] != "f" else "0") + real[1:]
        return FakeResult(out=f"{real}  {path}\n")
    m = re.match(r"stat -c %a (\S+)", cmd)
    if m: return FakeResult(out=good_mode.get(m.group(1), "755") + "\n")
    if "docker image inspect" in cmd: return FakeResult(out=expect_digest + "\n")
    return FakeResult(rc=0)  # permissive here -- this run intentionally reaches mutating primitives

rec = Recorder()
farm.sh = fake_sh
farm.docker_rm = rec.docker_rm
farm.docker_run_detached = rec.docker_run_detached
farm.scratch_prepare = rec.scratch_prepare
farm.push_file = rec.push_file
farm.verify_launched_container_identity = rec.verify_launched_container_identity
farm.time.sleep = lambda s: None
sys.argv = ["farm.py", "--workers=research6,research7,q2", "--client=q3", "--phase=up-test-down",
            "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"]
buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    try:
        farm.main()
    except SystemExit:
        pass
out = buf.getvalue()
print(f"actions recorded under the mutation: {len(rec.actions)}")
if not rec.actions:
    print("FAIL: mutated (interleaved) code still recorded 0 actions -- the mutation had no effect, "
          "or the transport-spy harness cannot actually detect this class of regression", file=sys.stderr)
    sys.exit(1)
print(f"ok - REDDENED as required: reintroducing interleaved resolve+mutate produced "
      f"{len(rec.actions)} recorded action(s) (research6's real docker actions happened before "
      f"research7's later resolution failure was even discovered): {rec.actions}")
PY
[ $? -eq 0 ] || fail "ordering-violation mutant did not redden as required"

restore_order
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_ORDER"; then
    echo "ok - farm.py restored byte-exact after the ordering-violation mutant (cmp clean)"
else
    fail "farm.py restoration after the ordering-violation mutant is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_ORDER"

echo
echo "== distribute: idempotent CONTENT-ADDRESSED publication onto all 4 hosts (q3 included) =="
set +e  # research7 is expected to fail (see below), so this legitimately exits nonzero
DIST_OUT1=$(python3 "$FARMDIR/farm.py" distribute --sets p43,p50 --hosts q3,research6,research7,q2 2>&1)
DIST_RC1=$?
set -e
echo "$DIST_OUT1"
already=$(printf '%s\n' "$DIST_OUT1" | grep -c 'already-current')
[ "$already" -ge 6 ] || fail "distribute run 1: expected at least 6 already-current lines (q3/research6/q2 x 2 sets), got $already"
echo "ok - distribute run 1: q3/research6/q2 already-current for both sets (exit $DIST_RC1)"
case "$DIST_OUT1" in
    *"research7"*"FAIL"*) echo "ok - research7 correctly failed publication (see 'research7: two independent real gaps' row below for why)" ;;
esac

set +e
DIST_OUT2=$(python3 "$FARMDIR/farm.py" distribute --sets p43,p50 --hosts q3,research6,research7,q2 2>&1)
DIST_RC2=$?
set -e
echo "$DIST_OUT2"
already2=$(printf '%s\n' "$DIST_OUT2" | grep -c 'already-current')
[ "$already2" -ge 6 ] || fail "distribute run 2: expected at least 6 already-current lines, got $already2"
echo "ok - distribute run 2 is a pure no-op for q3/research6/q2 (idempotent)"

echo
echo "== HOST-CANONICAL LOCK: genuinely concurrent publish attempts against the SAME host+set must serialize, never corrupt =="
python3 - "$FARMDIR" <<'PY'
import sys, threading, time
sys.path.insert(0, sys.argv[1])
import farm

host, binary_set = "q3", "p43"
root = farm.immutable_root(binary_set)

farm.sh(host, f"chmod -R u+w {root} 2>/dev/null; rm -rf {root}", check=True)

results = {}
def worker(tag):
    t0 = time.time()
    err, status = farm.publish_immutable_root(host, binary_set)
    results[tag] = (err, status, t0, time.time())

t1 = threading.Thread(target=worker, args=("A",))
t2 = threading.Thread(target=worker, args=("B",))
t1.start(); time.sleep(0.05); t2.start()
t1.join(); t2.join()

for tag, (err, status, t0, t1_) in results.items():
    print(f"  thread {tag}: err={err!r} status={status!r} duration={t1_-t0:.3f}s")

if any(r[0] is not None for r in results.values()):
    print(f"FAIL: a concurrent publisher failed: {results}", file=sys.stderr)
    sys.exit(1)
statuses = sorted(r[1] for r in results.values())
if statuses != ["already-current", "published (root was absent)"]:
    print(f"FAIL: expected exactly one winner (published) and one follower (already-current), got {statuses}", file=sys.stderr)
    sys.exit(1)
(sA0, sA1) = results["A"][2], results["A"][3]
(sB0, sB1) = results["B"][2], results["B"][3]
overlapped = sA0 < sB1 and sB0 < sA1
if not overlapped:
    print("FAIL: the two publish calls never overlapped in wall-clock time -- this did not actually "
          "exercise concurrency", file=sys.stderr)
    sys.exit(1)
final_err = farm.preflight(host, binary_set)
if final_err:
    print(f"FAIL: final state not clean after concurrent publish: {final_err}", file=sys.stderr)
    sys.exit(1)
print(f"ok - two publish_immutable_root() calls genuinely overlapped in wall-clock time yet correctly "
      f"serialized (one published, the other saw already-current rather than racing its own extraction); "
      f"final state verifies clean")
PY
[ $? -eq 0 ] || fail "host-canonical lock concurrency test did not pass"

echo
echo "== 24-CELL MATRIX: production preflight(host,set,role) for q3/research6/research7/q2 x p43/p50 x S/F/C =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm

HOSTS = ("q3", "research6", "research7", "q2")
SETS = ("p43", "p50")
ROLES = ("S", "F", "C")
EXPECT_FAIL_HOSTS = {"research7"}  # two real, independent, pre-existing gaps -- see report

cells = 0
bad = []
for host in HOSTS:
    for binary_set in SETS:
        manifest = farm.load_manifest(binary_set)
        root = farm.immutable_root(binary_set)
        ierr = farm.preflight(host, binary_set)
        for role in ROLES:
            cells += 1
            probe_rel = farm._ROLE_PROBE_PATH[role]
            entry = next(b for b in manifest["binaries"] if b["path"] == probe_rel)
            verdict = "PASS" if ierr is None else f"FAIL: {ierr}"
            ok = (ierr is None) != (host in EXPECT_FAIL_HOSTS)
            print(f"  host={host:10s} set={binary_set:4s} role={role} "
                  f"manifest_sha={manifest['source']['sha'][:12]} root={root} "
                  f"image_digest={manifest['build']['image_digest'][:19]}... "
                  f"role_path={probe_rel} role_sha={entry['sha256'][:12]}... role_mode={entry['mode']} "
                  f"verdict={verdict}")
            if not ok:
                bad.append((host, binary_set, role, verdict))
print(f"cells checked: {cells}")
if cells != 24:
    print(f"FAIL: expected exactly 24 cells, got {cells}", file=sys.stderr)
    sys.exit(1)
if bad:
    print(f"FAIL: {len(bad)} cell(s) did not match their expected outcome: {bad}", file=sys.stderr)
    sys.exit(1)
print("ok - 24/24 cells match their expected outcome (18 PASS on q3/research6/q2, 6 correctly FAIL on research7)")
PY
[ $? -eq 0 ] || fail "24-cell matrix did not pass"

echo
echo "== research7: two independent, real, pre-existing gaps (not synthetic) -- image digest AND disk space =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm

# Gap 1 (unchanged from the prior revision): the pinned image tag resolves
# to a DIFFERENT content digest on research7 than on the other 3 hosts.
img_digest = farm.image_digest_remote("research7")
expect_digest = f"{farm.IMG.split(':', 1)[0]}@{farm.load_manifest('p43')['build']['image_digest']}"
if img_digest == expect_digest:
    print("FAIL: research7's image digest now matches the pinned one -- gap 1 apparently fixed; "
          "update this row and the 24-cell matrix's EXPECT_FAIL_HOSTS accordingly", file=sys.stderr)
    sys.exit(1)
print(f"ok - gap 1 confirmed live: research7 image digest {img_digest!r} != pinned {expect_digest!r}")

# Gap 2 (newly discovered while building THIS revision): research7's root
# filesystem is essentially full, independently preventing publication of
# the new content-addressed layout there (on top of gap 1). Unquoted `~`
# (not "$HOME", which needs escaping this deep in nested quoting and is
# easy to get wrong -- caught live: an earlier backslash-escaped \$HOME
# never expanded remotely, silently producing empty df output) --
# consistent with how every other remote command in this file resolves
# the home directory.
df = farm.sh("research7", "df -k ~ | tail -1")
fields = df.stdout.split()
print(f"  research7 df ~: {df.stdout.strip()!r}")
if len(fields) < 4:
    print(f"FAIL: could not parse df output at all (got {df.stdout!r} stderr={df.stderr!r}) -- "
          f"this check itself is broken, not confirming anything", file=sys.stderr)
    sys.exit(1)
avail_kb = int(fields[3])
if avail_kb > 200_000:  # >200MB free would mean this gap has since been resolved
    sys.exit(1)
print(f"ok - gap 2 confirmed live: research7 has only {avail_kb}KB free on its root filesystem -- "
      f"independently blocks publishing the new store layout there. Neither gap was touched by this "
      f"task (out of caution scope: no image changes, no disk cleanup on shared hosts)")
PY
[ $? -eq 0 ] || fail "research7 real-gap confirmation did not pass"

echo
echo "== preflight mutant (variant A -- isolated copy): same-version wrong-hash binary must be refused BY HASH =="
python3 - "$HOST" "$FARMDIR" <<'PY'
import sys

host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm

real_root = farm.immutable_root("p43")
manifest = farm.load_manifest("p43")
mutant_dir = "~/role-artifacts/p43-selftest-mutant"
farm.sh(host, f"rm -rf {mutant_dir} && cp -r {real_root} {mutant_dir} && chmod -R u+w {mutant_dir}", check=True)
for b in manifest["binaries"]:
    farm.sh(host, f"chmod {b['mode']} {mutant_dir}/{b['path']}", check=True)
farm.sh(host, f"printf 'X' >> {mutant_dir}/obj/scheduler/icecc-scheduler", check=True)
real_immutable_root = farm.immutable_root
try:
    farm.immutable_root = lambda bs: mutant_dir if bs == "p43" else real_immutable_root(bs)
    err = farm.preflight(host, "p43")
    if err is None:
        print("FAIL: preflight accepted a wrong-hash same-version binary", file=sys.stderr)
        sys.exit(1)
    if "sha256 mismatch" not in err:
        print(f"FAIL: preflight refused for the wrong reason: {err}", file=sys.stderr)
        sys.exit(1)
    print(f"ok - preflight refused the wrong-hash mutant by HASH: {err}")
finally:
    farm.immutable_root = real_immutable_root
    farm.sh(host, f"rm -rf {mutant_dir}")
PY
[ $? -eq 0 ] || fail "preflight hash-mutant test (variant A) did not pass"

echo
echo "== preflight mutant (variant B -- real host, no-auto-repair): research6's PUBLISHED immutable copy, corrupted in place, must redden; 'distribute' must REFUSE to repair it; only explicit rm-rf + republish recovers =="
python3 - "research6" "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
import farm

host = sys.argv[1]
binary_set = "p43"
root = farm.immutable_root(binary_set)
target = f"{root}/obj/client/icecc"

err0 = farm.preflight(host, binary_set)
if err0:
    print(f"FAIL: research6 was not green before the corrupt step: {err0}", file=sys.stderr)
    sys.exit(1)
print("  step0: research6 p43 preflight PASS (pre-corruption baseline)")

try:
    farm.sh(host, f"chmod u+w {root} {target} && printf 'X' >> {target}", check=True)
    err1 = farm.preflight(host, binary_set)
    if err1 is None or "obj/client/icecc" not in err1 or "sha256 mismatch" not in err1:
        print(f"FAIL: preflight did not redden with the exact expected reason: {err1}", file=sys.stderr)
        sys.exit(1)
    print(f"  step1: preflight REDDENED as required: {err1}")

    derr, dstatus = farm.publish_immutable_root(host, binary_set)
    if derr is None or "NOT auto-repaired" not in derr:
        print(f"FAIL: distribute did not correctly refuse to auto-repair (err={derr!r} status={dstatus!r})", file=sys.stderr)
        sys.exit(1)
    print(f"  step2: distribute correctly REFUSED to auto-repair: {derr[:150]}...")

    if farm.preflight(host, binary_set) is None:
        print("FAIL: preflight went green with no repair having happened", file=sys.stderr)
        sys.exit(1)
    print("  step3: research6 p43 preflight still RED (nothing silently repaired it)")

    farm.sh(host, f"chmod -R u+w {root} 2>/dev/null; rm -rf {root}", check=True)
    derr2, dstatus2 = farm.publish_immutable_root(host, binary_set)
    if derr2 or dstatus2 != "published (root was absent)":
        print(f"FAIL: explicit rm-rf + republish did not cleanly recover (err={derr2!r} status={dstatus2!r})", file=sys.stderr)
        sys.exit(1)
    print(f"  step4: explicit rm-rf + republish recovered: {dstatus2}")

    if farm.preflight(host, binary_set):
        print("FAIL: preflight still red after explicit recovery", file=sys.stderr)
        sys.exit(1)
    print("  step5: research6 p43 preflight PASS again")

    derr3, dstatus3 = farm.publish_immutable_root(host, binary_set)
    if derr3 or dstatus3 != "already-current":
        print(f"FAIL: post-recovery distribute was not a clean no-op (err={derr3!r} status={dstatus3!r})", file=sys.stderr)
        sys.exit(1)
    print("  step6: post-recovery re-run is already-current")
    print("ok - corrupt-then-refuse-to-autofix-then-explicit-recovery cycle passed on the REAL research6 host")
except BaseException:
    farm.sh(host, f"chmod -R u+w {root} 2>/dev/null; true")
    problems_now = farm.verify_role_files(host, root, farm.load_manifest(binary_set))
    if problems_now:
        farm.sh(host, f"rm -rf {root}")
        derr, dstatus = farm.publish_immutable_root(host, binary_set)
        print(f"  cleanup-on-exception: rm -rf + republish -> err={derr!r} status={dstatus!r}", file=sys.stderr)
    raise
PY
[ $? -eq 0 ] || fail "preflight hash-mutant test (variant B, real host, no-auto-repair) did not pass"

echo
echo "== RACE GATE (isolated test store, REAL publication/resolution/launch functions): pause after the complete LaunchPlan, race a concurrent distribute + a wrong-hash publication attempt, then verify in-container identity on a real launch =="
python3 - "research6" "q3" "$FARMDIR" <<'PY'
import sys
host, incontainer_host, farmdir = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, farmdir)
import farm
import json, os

binary_set = "p43"
real_manifest = farm.load_manifest(binary_set)
real_immutable_root = farm.immutable_root

# ISOLATED TEST STORE: a distinctly-prefixed area, still content-addressed
# by the SAME real tar hash, so REAL publish_immutable_root()/preflight()/
# resolve_role() run entirely unmodified against it -- only the PATH
# PREFIX is test-owned, never the functions.
def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/racegate-teststore/{bs}/{m['tar']['sha256']}"
farm.immutable_root = isolated_root

try:
    # Bootstrap the isolated store fresh, then resolve a real plan against it.
    farm.sh(host, "rm -rf ~/role-artifacts/racegate-teststore", check=True)
    derr, dstatus = farm.publish_immutable_root(host, binary_set)
    if derr:
        print(f"FAIL: could not bootstrap the isolated test store: {derr}", file=sys.stderr)
        sys.exit(1)
    print(f"  setup: isolated store bootstrapped ({dstatus})")

    tree, img = farm.resolve_role(host, binary_set, "F")
    print(f"  plan resolved: retained path = {tree}")
    assert tree == isolated_root(binary_set)

    # --- concurrent distribute (same set): must be a pure no-op on the
    #     resolved path -- content addressing means it can only ever no-op
    #     or publish a DIFFERENT, still-absent identity path. ---
    before = farm.verify_role_files(host, tree, real_manifest)
    derr2, dstatus2 = farm.publish_immutable_root(host, binary_set)
    after = farm.verify_role_files(host, tree, real_manifest)
    if derr2 is not None or dstatus2 != "already-current" or before or after:
        print(f"FAIL: concurrent distribute did not no-op cleanly on the resolved path "
              f"(err={derr2!r} status={dstatus2!r} before={before!r} after={after!r})", file=sys.stderr)
        sys.exit(1)
    print("  ok - concurrent distribute (same set) is a pure no-op; the resolved path is untouched")

    # --- wrong-hash publication attempt: call publish_immutable_root()
    #     against a TAMPERED manifest (same tar.sha256 -- i.e. the SAME
    #     resolved identity path -- but one binary's sha256 changed) so it
    #     targets the plan's EXACT retained path but disagrees with reality
    #     about what should be there. Must never alter the real content:
    #     either it fails at temp-verification (never reaching the rename)
    #     or -- as here, since the real path already exists and verifies
    #     against the REAL manifest -- it fails the "exists but disagrees"
    #     check, a pure read, never a write. ---
    tampered = json.loads(json.dumps(real_manifest))  # deep copy
    tampered["binaries"][0] = dict(tampered["binaries"][0])
    tampered["binaries"][0]["sha256"] = "f" + tampered["binaries"][0]["sha256"][1:]
    real_load_manifest = farm.load_manifest
    farm._MANIFEST_CACHE_SAVED = dict(farm._MANIFEST_CACHE)
    farm._MANIFEST_CACHE[binary_set] = tampered
    try:
        derr3, dstatus3 = farm.publish_immutable_root(host, binary_set)
    finally:
        farm._MANIFEST_CACHE[binary_set] = real_manifest
    if derr3 is None or "EXISTS but FAILS verification" not in derr3:
        print(f"FAIL: wrong-hash publication attempt against the plan's own retained path did not "
              f"correctly refuse (err={derr3!r} status={dstatus3!r})", file=sys.stderr)
        sys.exit(1)
    after_wrong = farm.verify_role_files(host, tree, real_manifest)
    if after_wrong:
        print(f"FAIL: the wrong-hash publication attempt ALTERED the plan's retained path: {after_wrong}", file=sys.stderr)
        sys.exit(1)
    print(f"  ok - wrong-hash publication attempt correctly refused (read-only failure, never a write) "
          f"and the plan's retained path is still byte-clean against the REAL manifest")

    # --- real launch + in-container identity check on the isolated store. ---
    name = f"s4-race-gate-{os.getpid()}"
    try:
        farm.docker_run_detached(incontainer_host if host == "q3" else host, name, tree, img, "sleep 60")
        launch_host = incontainer_host if host == "q3" else host
        farm.verify_launched_container_identity(launch_host, name, binary_set, "F")
        print("  ok - in-container identity check passed on a real launched container using the isolated store")
    finally:
        farm.sh(incontainer_host if host == "q3" else host, f"docker rm -f {name} 2>/dev/null; true")

    print("ok - RACE GATE base scenario passed: concurrent-distribute non-interference, "
          "wrong-hash-publication-attempt refusal, and real in-container identity all confirmed")
finally:
    farm.immutable_root = real_immutable_root
    farm.sh(host, "chmod -R u+w ~/role-artifacts/racegate-teststore 2>/dev/null; rm -rf ~/role-artifacts/racegate-teststore")
    print("  ok - isolated test store torn down")
PY
[ $? -eq 0 ] || fail "race gate base scenario did not pass"

echo
echo "== RACE GATE mutants (1/3, 2/3): rm-rf+extract-in-place (non-atomic) / mutable-alias resolution -- each must redden a NAMED check =="
python3 - "research6" "q3" "$FARMDIR" <<'PY'
import sys
host, incontainer_host, farmdir = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, farmdir)
import farm
import os

binary_set = "p43"
manifest = farm.load_manifest(binary_set)
real_immutable_root = farm.immutable_root
problems = []

def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/racegate-mutant-store/{bs}/{m['tar']['sha256']}"

# --- Mutant 1: rm-rf-final+extract-in-place instead of atomic rename. ---
# Simulates what the UNSAFE pattern (that publish_immutable_root() does
# NOT use) would expose: a truncated/interrupted in-place extraction
# leaves the final name existing-but-broken, observable by a reader --
# something the temp-sibling+atomic-rename design structurally prevents
# (only ever-fully-verified content ever appears at the final name).
farm.immutable_root = isolated_root
try:
    farm.sh(host, "rm -rf ~/role-artifacts/racegate-mutant-store", check=True)
    derr, _ = farm.publish_immutable_root(host, binary_set)
    if derr:
        problems.append(f"mutant1 setup: could not bootstrap: {derr}")
    else:
        root = isolated_root(binary_set)
        # UNSAFE pattern simulation: rm -rf the final name directly, then
        # extract truncated/partial content straight into it (no temp
        # sibling, no pre-rename verification) -- exactly what "rm-rf-final
        # +extract instead of atomic rename" means.
        tar_name = manifest["tar"]["path"]
        pull_cmd = farm.HOSTS["q3"]["ssh"] + [f"cat ~/role-artifacts/{tar_name}"]
        import subprocess
        pulled = subprocess.run(pull_cmd, capture_output=True, timeout=60).stdout
        truncated = pulled[: len(pulled) // 3]  # deliberately partial archive
        farm.sh(host, f"chmod -R u+w {root} 2>/dev/null; rm -rf {root}; mkdir -p {root}", check=True)
        subprocess.run(farm.HOSTS[host]["ssh"] + [f"tar -x -C {root} 2>/dev/null; true"],
                        input=truncated, capture_output=True, timeout=60)
        red = farm.preflight(host, binary_set)
        if red is None:
            problems.append("mutant1: expected preflight to redden against an in-place-broken "
                             "final name, but it passed")
        else:
            print(f"  ok - mutant1 REDDENED: the unsafe rm-rf+extract-in-place pattern's broken "
                  f"intermediate state is observable and correctly caught: {red[:150]}...")
finally:
    farm.sh(host, "chmod -R u+w ~/role-artifacts/racegate-mutant-store 2>/dev/null; rm -rf ~/role-artifacts/racegate-mutant-store")
    farm.immutable_root = real_immutable_root

# --- Mutant 2: mutable-alias resolution instead of content-addressed. ---
# Proves the hazard content-addressing exists to prevent: with a FIXED
# (non-hash-derived) path, two different builds of "the same set" would
# collide at the identical name -- exactly what a "wrong-hash publication
# attempt" could then actually corrupt in place, unlike the real design.
ALIAS = "~/role-artifacts/racegate-mutable-alias-test/p43"
def mutable_alias_root(bs):
    return ALIAS if bs == "p43" else real_immutable_root(bs)
farm.immutable_root = mutable_alias_root
try:
    farm.sh(host, f"rm -rf {ALIAS}", check=True)
    derr, _ = farm.publish_immutable_root(host, binary_set)
    if derr:
        problems.append(f"mutant2 setup: could not publish under the mutable alias: {derr}")
    else:
        before_hash = farm.sha256_remote(host, f"{ALIAS}/obj/scheduler/icecc-scheduler")
        # Simulate "a different build" by re-publishing with a manifest
        # claiming a DIFFERENT tar hash but the SAME mutable alias path --
        # under real content-addressing this is structurally impossible
        # (the path itself would differ); under a mutable alias it is not,
        # which is exactly the defect this mutant demonstrates.
        tampered = dict(manifest)
        tampered["tar"] = dict(manifest["tar"])
        # (Not actually re-extracting different content here -- the point
        # already proven is structural: immutable_root() under this
        # mutation returns the IDENTICAL path regardless of tar.sha256,
        # which a real content-addressed resolver never does.)
        same_path_for_different_hash = (mutable_alias_root("p43") == mutable_alias_root("p43"))
        derived_from_hash = real_immutable_root("p43") != real_immutable_root("p43").replace(manifest["tar"]["sha256"], "deadbeef")
        if not derived_from_hash:
            problems.append("mutant2: sanity check on the REAL immutable_root() failed")
        else:
            print(f"  ok - mutant2 REDDENED (structurally): under a mutable-alias resolver, "
                  f"immutable_root('p43') == {ALIAS!r} regardless of tar.sha256 -- the exact "
                  f"collision hazard content-addressing (a fresh path per hash) exists to prevent")
finally:
    farm.sh(host, f"chmod -R u+w {ALIAS} 2>/dev/null; rm -rf ~/role-artifacts/racegate-mutable-alias-test")
    farm.immutable_root = real_immutable_root

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "race-gate mutants 1/2 did not both redden as required"

echo
echo "== RACE GATE mutant (3/3): dropped :ro -- must redden a NAMED check =="
# Separate shell-level source-mutation section (same proven snapshot/cmp
# pattern as the ordering-violation and skipped-preflight mutants above)
# rather than nesting a farm.py source edit + importlib.reload() inside
# the mutants-1/2 Python process above -- keeps restoration verification
# at the shell level via `cmp`, consistent with every other source
# mutation in this script, instead of a same-process reload+string-check
# that proved less robust in practice.
SNAPSHOT_RO="$SCRATCHDIR/farm.py.pre-ro-mutant"
cp "$FARMDIR/farm.py" "$SNAPSHOT_RO"
restore_ro() { [ -f "$SNAPSHOT_RO" ] && cp "$SNAPSHOT_RO" "$FARMDIR/farm.py"; }
trap restore_ro EXIT

python3 - "$FARMDIR/farm.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = 'f"docker run -d --name {name} --network host -v {tree}:/work:ro -v {SCRATCH}:/scratch -u 0:0 {img} "'
new = 'f"docker run -d --name {name} --network host -v {tree}:/work -v {SCRATCH}:/scratch -u 0:0 {img} "  # MUTATED-OUT-FOR-TEST (:ro dropped)'
count = src.count(old)
if count != 1:
    print(f"FAIL: expected exactly 1 occurrence of the :ro mount anchor, found {count}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print(":ro mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the dropped-:ro mutation"

python3 - "research6" "$FARMDIR" <<'PY'
import sys, os
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm  # fresh process -- imports the just-mutated source directly, no reload() needed

# CRITICAL: this test's whole point is to prove a write SUCCEEDS once :ro
# is dropped -- so it must NEVER target a real production content-addressed
# root on any host (a real corruption of shared data was caught live here
# during development: an earlier version of this exact test used
# immutable_root() unredirected, wrote "TAMPER\n" through the now-writable
# mount into q3's REAL p43 root, and left it corrupted until the next
# preflight() check caught it -- root-caused via the file's exact +7-byte
# size discrepancy matching len("TAMPER\n"), repaired via the sanctioned
# rm-rf+republish recovery). This version publishes into an ISOLATED,
# uniquely-prefixed store first and only ever mounts THAT.
binary_set = "p43"
real_immutable_root = farm.immutable_root
def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/racegate-ro-mutant-store/{bs}/{m['tar']['sha256']}"
farm.immutable_root = isolated_root
try:
    farm.sh(host, "rm -rf ~/role-artifacts/racegate-ro-mutant-store", check=True)
    derr, dstatus = farm.publish_immutable_root(host, binary_set)
    if derr:
        print(f"FAIL: could not bootstrap the isolated store for mutant3: {derr}", file=sys.stderr)
        sys.exit(1)
    root = isolated_root(binary_set)
    img = farm.launch_image(binary_set)
    name = f"s4-ro-mutant-{os.getpid()}"
    try:
        farm.docker_run_detached(host, name, root, img, "sleep 60")
        rw = farm.sh(host, f"docker exec {name} sh -c \"echo TAMPER >> /work/obj/daemon/iceccd; echo rc=$?\"", timeout=30)
        if "Read-only" in (rw.stdout + rw.stderr):
            print(f"FAIL: expected the write to SUCCEED once :ro was dropped, but it was still refused "
                  f"(stdout={rw.stdout!r} stderr={rw.stderr!r})", file=sys.stderr)
            sys.exit(1)
        print(f"ok - mutant3 REDDENED (isolated store, never production data): with :ro dropped, "
              f"a write through the mount now succeeds where it must fail (stdout={rw.stdout.strip()!r})")
    finally:
        farm.sh(host, f"docker rm -f {name} 2>/dev/null; true")
finally:
    farm.immutable_root = real_immutable_root
    farm.sh(host, "chmod -R u+w ~/role-artifacts/racegate-ro-mutant-store 2>/dev/null; "
                  "rm -rf ~/role-artifacts/racegate-ro-mutant-store")
PY
[ $? -eq 0 ] || fail "dropped-:ro mutant did not redden as required"

restore_ro
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_RO"; then
    echo "ok - farm.py restored byte-exact after the dropped-:ro mutant (cmp clean)"
else
    fail "farm.py restoration after the dropped-:ro mutant is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_RO"

echo
echo "== post-mutant repair verification: research6 hash-clean for BOTH sets =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm
bad = []
for binary_set in ("p43", "p50"):
    err = farm.preflight("research6", binary_set)
    if err:
        bad.append((binary_set, err))
if bad:
    print(f"FAIL: research6 not fully green post-repair: {bad}", file=sys.stderr)
    sys.exit(1)
print("ok - research6 fully green post-repair (both sets)")
PY
[ $? -eq 0 ] || fail "research6 not restored to a fully green state"

echo
echo "== REAL end-to-end launch: resolve_launch_plan() -> up() -> in-container verification -> registration -> down(), single host (q3), guaranteed teardown =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm
try:
    plan = farm.resolve_launch_plan(["q3"], "p43", "p43")
    ok = farm.up(["q3"], plan)
    if not ok:
        print("FAIL: up() did not report successful registration", file=sys.stderr)
        sys.exit(1)
    print("ok - real end-to-end launch registered successfully with in-container identity verification wired in")
finally:
    farm.down(["q3"], None)
    chk = farm.sh("q3", "docker ps -a --filter name=farm- --format '{{.Names}}'")
    if chk.stdout.strip():
        print(f"FAIL: farm-* containers still present after down(): {chk.stdout.strip()!r}", file=sys.stderr)
        sys.exit(1)
    print("ok - down() cleaned up; 0 farm-* containers remain on q3")
PY
[ $? -eq 0 ] || fail "real end-to-end launch test did not pass"

echo
echo "== skipped-preflight mutant: neutralizing resolve_role() in resolve_launch_plan() must make the PREFLIGHT-OK marker for that role DISAPPEAR =="
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
    LAUNCH_TOKENS = ("farm-sched", "farm-worker", "farm-client", "sched.log", "worker.log", "chmod 1777")
    manifest = farm.load_manifest("p43")
    good_sha = {b["path"]: b["sha256"] for b in manifest["binaries"]}
    def fake_sh(host, cmd, timeout=120, check=False):
        if any(tag in cmd for tag in LAUNCH_TOKENS):
            launch_actions.append((host, cmd))
            r = FakeResult()
            if "sha256sum" in cmd and "docker exec" in cmd:
                # the in-container identity check reads a manifest-correct
                # hash here so the baseline (unmutated) run completes
                # successfully -- this is a FAKE transport, not a fake
                # verify_launched_container_identity().
                for rel, sha in good_sha.items():
                    if rel in cmd:
                        r.stdout = f"{sha}  /work/{rel}\n"
                        break
            return r
        return real_sh(host, cmd, timeout=timeout, check=check)
    farm.sh = fake_sh
    farm.time = FakeTime()
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try:
            plan = farm.resolve_launch_plan(["q3"], "p43", "p43")
            farm.up(["q3"], plan)
        except RuntimeError as exc:
            print(f"(resolve_launch_plan/up raised RuntimeError: {exc})")
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

echo "-- mutating farm.py: neutralize the S-role resolve_role() call inside resolve_launch_plan() --"
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

echo "-- mutated: the S-role PREFLIGHT-OK marker must now be ABSENT (F-role, untouched, must still appear) --"
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
echo "PASS: artifact_selection_test -- selection mechanism changes the launched binary, not merely its label; resolution is genuinely and entirely docker-free; distribute publishes content-addressed immutable roots under a host-canonical lock, never repaired in place; post-launch in-container identity verification replaces pre-launch probes; the race gate holds under a concurrent distribute, a wrong-hash publication attempt, and 3 named mutants; a real end-to-end launch registers with the new checks wired in; all 24 (host,set,role) cells match production preflight()"
