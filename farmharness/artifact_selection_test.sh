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

# Whole-script concurrency lock. research6/research7/q2 are shared, LIVE
# remote hosts that `distribute` and the corrupt-then-repair mutant (variant
# B below) mutate directly and non-atomically (`rm -rf $root && mkdir -p
# $root && tar -x -C $root` on repair is not an atomic swap). Two overlapping
# invocations of this script racing against the SAME host was observed
# directly: a corrupt-then-repair cycle's transient mid-flight state --
# byte-identical to this script's own deterministic mutant hash -- was
# caught by a DIFFERENT, concurrently-running invocation's earlier
# "preflight goes green after distribute" check, which has no way to know a
# sibling run is mid-mutation on the same host and correctly (from its own,
# incomplete point of view) reported red. Every host-mutating step this
# script performs is legitimate and self-consistent in isolation (every
# single run, start to finish, has always ended with a hash-clean, fully
# repaired, reverified host) -- the gap was purely "two runs at once", which
# this lock closes by serializing whole invocations rather than trying to
# scope locking to individual sections (simpler, and safe against a future
# section gaining shared-host access without remembering to also acquire a
# lock for it).
LOCKFILE="${ARTIFACT_TEST_LOCK:-$SCRATCHDIR/artifact_selection_test.lock}"
exec 9>"$LOCKFILE"
if ! flock -n 9; then
    echo "artifact_selection_test.sh: another run currently holds $LOCKFILE" \
         "(research6/research7/q2 are shared live hosts this suite mutates directly;" \
         "two runs interleaving can observe each other's transient corrupt-then-repair" \
         "state). Waiting for it to finish..." >&2
    flock 9
fi

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

echo "== FRESH-ARCHIVE no-ambient gate: farm.py + both manifests, from a bare git-archive extraction, zero ambient files possible =="
# The defect this row exists to catch: 02622ab6 was reviewed and passed
# against the ambient worktree, but the COMMITTED code and the COMMITTED
# manifests actually disagreed on path (farm.py read
# farmharness/role-manifests/, the manifests were committed at
# farmharness/artifacts/) -- every prior green run only worked because an
# untracked, uncommitted copy at the OTHER path happened to still be
# sitting in the shared worktree. A `git archive` extraction into a
# brand-new, otherwise-empty directory cannot be satisfied by ANY
# ambient/untracked file, by construction -- this row is what makes that
# whole defect class structurally impossible to miss again, no matter what
# else is lying around in whatever worktree happens to run this script.
REPOROOT=$(cd "$FARMDIR" && git rev-parse --show-toplevel)
ARCHIVE_COMMIT=$(cd "$FARMDIR" && git rev-parse HEAD)
ARCHIVE_DIR="$SCRATCHDIR/fresh-archive-$$"
rm -rf "$ARCHIVE_DIR"
mkdir -p "$ARCHIVE_DIR"
# The "farmharness" pathspec below is resolved relative to cwd, like any
# git pathspec -- it must run from the REPO ROOT (not from $FARMDIR, which
# already IS farmharness/) or git looks for a farmharness/farmharness that
# does not exist.
( cd "$REPOROOT" && git archive "$ARCHIVE_COMMIT" -- farmharness ) | tar -x -C "$ARCHIVE_DIR"
[ -f "$ARCHIVE_DIR/farmharness/farm.py" ] || fail "fresh-archive: farmharness/farm.py missing from the archive of $ARCHIVE_COMMIT"
python3 - "$ARCHIVE_DIR" "$ARCHIVE_COMMIT" <<'PY'
import os
import sys

archive_dir, commit = sys.argv[1], sys.argv[2]

# Prove farm.py itself resolves from THIS fresh location, not some ambient
# copy elsewhere on sys.path.
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
    # required-role coverage: every role farm.py's preflight() probes for
    # (_ROLE_PROBE_PATH's S/F/C) must have a binaries[] entry at the exact
    # expected path with a non-empty version_probe.command -- otherwise
    # preflight()'s `if entry and entry["version_probe"]["command"]:` guard
    # silently SKIPS that role's identity check instead of running it. That
    # is a real fail-open gap in the MANIFEST DATA, not just in the code,
    # and schema-key presence alone would not catch it.
    by_path = {b.get("path"): b for b in manifest.get("binaries", [])}
    for role, probe_path in farm._ROLE_PROBE_PATH.items():
        entry = by_path.get(probe_path)
        if entry is None:
            problems.append(f"{set_name}: no binaries[] entry at {probe_path!r} for role {role!r}")
            continue
        if entry.get("role") != role:
            problems.append(f"{set_name}: {probe_path} role={entry.get('role')!r}, expected {role!r}")
        cmd = entry.get("version_probe", {}).get("command")
        if not cmd:
            problems.append(f"{set_name}: {probe_path} (role {role}) has no version_probe.command -- "
                             f"preflight would silently skip this role's identity check")

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
print(f"ok - fresh-archive gate: farm imported from {farm.__file__} (git archive of {commit}), "
      f"both manifests loaded via farm.load_manifest() from that same location, schema-valid, "
      f"S/F/C role coverage confirmed present in both")
PY
[ $? -eq 0 ] || fail "fresh-archive no-ambient gate did not pass"
rm -rf "$ARCHIVE_DIR"

echo
echo "== source anchor: the launch path cannot skip preflight without this test noticing =="
# EXACT count per function. All role resolution now lives in ONE place,
# resolve_launch_plan() (S + the per-worker F comprehension + C = 3 call
# sites in source, regardless of how many workers exist at runtime) -- a
# coarse "is resolve_role called at all" check would miss one of the three
# being deleted. up()/run_client() must have ZERO: they now only consume an
# already-validated LaunchPlan, so a resolve_role() call reappearing inside
# either of them would mean a role is being preflighted AFTER some other
# role's launch may already be underway -- exactly the ordering bug this
# whole successor exists to close. This anchor is a SECONDARY, source-level
# check only; the primary proof is the behavioral no-network spy gates
# below (a call could stay textually present yet be silently neutered, as
# the skipped-preflight mutant test further down demonstrates).
for spec in "resolve_launch_plan:3" "up:0" "run_client:0"; do
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
        if [ "$want" = "0" ]; then
            echo "ok - $fn() calls resolve_role() exactly 0 times (no bypassable secondary resolution path exists inside it)"
        else
            echo "ok - $fn() calls resolve_role() exactly $want time(s) (preflight cannot be silently skipped for any role)"
        fi
    else
        fail "$fn() calls resolve_role() $got time(s), expected $want -- either a role's preflight gate was deleted from resolve_launch_plan(), or up()/run_client() gained a resolve_role() call of their own (reopening the resolve-then-mutate-immediately ordering bug)"
    fi
done

echo
echo "== no-network deletion-sensitive gates: invalid-role scenarios must record ZERO mutating actions =="
# LO measured 6 real mutating actions (docker_rm + scratch-write +
# docker-run-d, x2, for S then the first worker) before a SECOND worker's
# bad preflight was even discovered under the pre-fix code, plus 4 more
# from the unconditional finally-teardown that ran anyway -- both numbers
# had to go to 0. These rows invoke farm.main() itself (the real CLI entry
# point, argv and all -- not a hand-rolled reimplementation of its control
# flow) against a directly-faked farm.preflight(). preflight()'s OWN
# correctness (hash/digest/version-probe checks) is already exhaustively
# covered elsewhere in this file (identity baseline, hash-mutant variants
# A/B, the research7 digest-mismatch row); what's under test here is
# purely the ORCHESTRATION around it -- does a refusal for one role ever
# let a DIFFERENT role's mutation through, no matter how many roles
# resolved successfully first. Every docker/scratch/push mutating
# PRIMITIVE farm.py has (docker_rm, docker_run_detached, scratch_prepare,
# push_file) is monkeypatched to a pure recorder; farm.sh itself is ALSO
# monkeypatched to hard-fail the test the instant it's called at all --
# since up()/run_client() must never be reached in any of these scenarios,
# sh() should have zero callers by construction, and this turns a
# regression that reaches them into a loud local AssertionError instead of
# a silent (and possibly hanging) real network call.
python3 - "$FARMDIR" <<'PY'
import contextlib
import io
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

def refuse_sh(*a, **kw):
    raise AssertionError(f"farm.sh() was called during a no-network spy scenario (args={a!r}) -- "
                          f"up()/run_client() must never be reached when a role plan is refused")

def make_fake_preflight(bad):
    """bad: set of (host, role) that must FAIL; everything else passes.
    A controlled, network-free stand-in for farm.preflight() -- see the
    section comment above for why faking preflight() itself (rather than
    faking sh() finely enough for the REAL preflight() to naturally
    produce the same verdict) is the right level for this test."""
    def fake_preflight(host, binary_set, role):
        if (host, role) in bad:
            return f"SIMULATED-REFUSAL host={host} role={role} set={binary_set}"
        return None
    return fake_preflight

def run_scenario(argv_tail, bad_roles):
    rec = Recorder()
    saved = dict(preflight=farm.preflight, sh=farm.sh, docker_rm=farm.docker_rm,
                 docker_run_detached=farm.docker_run_detached,
                 scratch_prepare=farm.scratch_prepare, push_file=farm.push_file,
                 argv=sys.argv)
    farm.preflight = make_fake_preflight(bad_roles)
    farm.sh = refuse_sh
    farm.docker_rm = rec.docker_rm
    farm.docker_run_detached = rec.docker_run_detached
    farm.scratch_prepare = rec.scratch_prepare
    farm.push_file = rec.push_file
    sys.argv = ["farm.py"] + argv_tail
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            try:
                farm.main()
            except SystemExit:
                pass
    finally:
        farm.preflight = saved["preflight"]
        farm.sh = saved["sh"]
        farm.docker_rm = saved["docker_rm"]
        farm.docker_run_detached = saved["docker_run_detached"]
        farm.scratch_prepare = saved["scratch_prepare"]
        farm.push_file = saved["push_file"]
        sys.argv = saved["argv"]
    return buf.getvalue(), rec.actions

problems = []
COMMON = ["--workers=research6,research7,q2", "--client=q3", "--phase=up-test-down",
          "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"]

def check(label, argv_tail, bad_roles, before_fix_hint):
    out, actions = run_scenario(argv_tail, bad_roles)
    print(f"-- {label}: {len(actions)} action(s) recorded --")
    if "REFUSED" not in out:
        problems.append(f"{label}: expected a REFUSED line in stdout, got: {out!r}")
    if actions:
        problems.append(f"{label}: expected 0 recorded mutating actions, got {len(actions)}: {actions}")
    else:
        print(f"ok - {label}: refused with 0 mutating actions ({before_fix_hint})")

# 1) invalid INITIAL S -- must refuse before ANY F is even resolved.
check("invalid INITIAL S", COMMON, {("q3", "S")},
      "was 6 pre-refusal + 4 finally-teardown actions before this fix")

# 2) invalid SECOND F of 3 -- S and F[0] (research6) resolve fine first;
#    F[1] (research7) fails; F[2] (q2) is never even attempted (the list
#    comprehension short-circuits on the first exception) -- proves a
#    LATE-discovered bad role, after other roles already resolved
#    successfully, still blocks every mutation.
check("invalid SECOND F (of 3)", COMMON, {("research7", "F")},
      "was 6 pre-refusal + 4 finally-teardown actions before this fix")

# 3) invalid C -- S and ALL F resolve fine; only C (checked last, and only
#    because this phase actually uses a client) fails -- proves S/F's
#    success alone must never be enough to launch anything.
check("invalid C", ["--workers=research6", "--client=q3", "--phase=up-test-down",
                     "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"],
      {("q3", "C")}, "S and F would have launched for real before this fix")

# 4) BO's variant: the FINAL worker (of 3) fails, after S, F[0], AND F[1]
#    all resolved successfully -- recorded docker-op list must be empty.
check("final worker (of 3) fails", COMMON, {("q2", "F")},
      "docker-op list was non-empty before this fix (per BO)")

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "no-network deletion-sensitive gates did not pass"
echo "ok - all 4 invalid-role scenarios refused cleanly through the real farm.main() entry point, 0 mutating actions and 0 finally-teardown actions in every case"

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
echo "== skipped-preflight mutant: neutralizing resolve_role() in resolve_launch_plan() must make the PREFLIGHT-OK marker for that role DISAPPEAR =="
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
    # "once launch starts, fake everything" flag is wrong here:
    # resolve_launch_plan() calls resolve_role() for the scheduler AND for
    # every worker role BEFORE up() runs at all now, and each of those
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
            # Mirrors the two-step sequence main() itself now uses: resolve the COMPLETE
            # plan first (this is where the mutation below lands), only
            # then call up() with whatever it resolved -- exactly what lets
            # this test tell "the S-role PREFLIGHT-OK marker is gone" apart
            # from "up() was never reached at all".
            plan = farm.resolve_launch_plan(["q3"], "p43", "p43")
            farm.up(["q3"], plan.s_tree, plan.s_img, plan.f_resolved)
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
