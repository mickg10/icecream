#!/bin/sh
# artifact_selection_test.sh -- fail-able gate for farm.py's per-role
# --binary-set-S/-C/-F selection mechanism (S4) and its successors:
# committed role-manifests/*.json authority, resolve-then-mutate ordering,
# and (this revision) CONTENT-ADDRESSED IMMUTABLE role-artifact roots with
# a genuinely docker-free resolution phase and read-only container mounts.
#
# WITHOUT starting a full cluster (except where explicitly and narrowly
# noted -- the race-gate section starts ONE minimal, uniquely-named,
# always-torn-down container to hash its contents from the inside):
# resolves each role's selected runtime root via farm.py's OWN role_tree()
# function and issues remote commands through farm.py's OWN sh() transport
# (imported directly, never reimplemented) -- so this test exercises the
# exact code path up() and run_client() use. It launches that role's
# executable with a version-identity probe inside the pinned container on
# the target host and asserts the reported identity matches the selected
# set:
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

# Whole-script concurrency lock. research6/research7/q2 (and q3) are
# shared, LIVE remote hosts that `distribute` and the mutant/race-gate
# sections mutate directly. Two overlapping invocations of this script
# racing against the SAME host was observed directly in an earlier
# revision (a transient mid-mutation state on one run was caught by a
# DIFFERENT, concurrently-running invocation's read-only check, which
# correctly -- from its own, incomplete point of view -- reported red).
# Every host-mutating step this script performs is legitimate and
# self-consistent in isolation; the gap was purely "two runs at once",
# which this lock closes by serializing whole invocations.
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
# executable when farm.py's role_tree(SET) resolves the runtime root (now
# the content-addressed immutable path). SET is "p43", "p50", or "default"
# (meaning no --binary-set given, i.e. role_tree(None) -- the pre-existing
# hardcoded TREE). Mounted READ-ONLY (:ro), matching production.
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
                             f"verify_role_version would silently skip this role's identity check")
    # This revision's own invariant: immutable_root() must derive from
    # tar.sha256, not any fixed/legacy path -- a manifest missing tar.sha256
    # (already checked above) would make immutable_root() raise, which is
    # itself a fail-closed outcome, but confirm the HAPPY path resolves to
    # exactly the expected content-addressed name.
    expected_root = f"~/role-artifacts/{set_name}-{manifest['tar']['sha256']}"
    actual_root = farm.immutable_root(set_name)
    if actual_root != expected_root:
        problems.append(f"{set_name}: immutable_root() = {actual_root!r}, expected {expected_root!r}")

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
print(f"ok - fresh-archive gate: farm imported from {farm.__file__} (git archive of {commit}), "
      f"both manifests loaded via farm.load_manifest() from that same location, schema-valid, "
      f"S/F/C role coverage confirmed present in both, immutable_root() derivation confirmed")
PY
[ $? -eq 0 ] || fail "fresh-archive no-ambient gate did not pass"
rm -rf "$ARCHIVE_DIR"

echo
echo "== source anchor: the launch path cannot skip resolution OR launch-validation without this test noticing =="
# EXACT count per function (secondary, source-level check -- the primary
# proof is the behavioral transport-spy gates and the skipped-preflight
# mutant further down; a call could stay textually present yet be silently
# neutered, as that mutant test demonstrates).
for spec in "resolve_launch_plan:resolve_role:3" "resolve_launch_plan:verify_launch_version:3" \
            "up:resolve_role:0" "up:revalidate_before_mutation:2" \
            "run_client:resolve_role:0" "run_client:revalidate_before_mutation:1"; do
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
        fail "$fn() calls $target() $got time(s), expected $want -- the resolve-then-mutate / launch-classified-probe ordering this successor exists for may have regressed"
    fi
done

echo
echo "== no-network TRANSPORT-SPY gates: invalid-role scenarios must record ZERO cluster-mutating actions, using REAL preflight()/verify_role_version() =="
# The prior revision of this row replaced farm.preflight WHOLESALE with a
# fake, which meant it could never prove anything about what PRODUCTION
# preflight()/verify_role_version() actually do -- only about a
# hand-written stand-in. This revision instead fakes farm.sh() (the one
# shared transport layer both real functions call) with a strict,
# manifest-derived responder: every host NOT deliberately marked "bad"
# gets the byte-exact response REAL infrastructure would give; a "bad"
# host gets a deliberately wrong sha256 (identity-level failure, scenarios
# 1-4) or a deliberately wrong version string (probe-level failure,
# scenario 5). farm.preflight and farm.verify_role_version themselves are
# never touched -- their own correctness is exhaustively covered elsewhere
# in this file (identity baseline, hash-mutant variants, the research7
# digest-mismatch row, the race-gate section); what is under test here is
# purely the ORCHESTRATION around them. The fake raises AssertionError for
# any command shape it does not recognize, so a regression that reaches
# an unexpected code path fails loudly instead of silently mis-answering.
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

class FakeResult:
    def __init__(self, out="", rc=0, err=""):
        self.stdout, self.returncode, self.stderr = out, rc, err

def make_fake_transport(binary_set, bad_identity_hosts=frozenset(), bad_probe_hosts=frozenset()):
    manifest = farm.load_manifest(binary_set)
    root = farm.immutable_root(binary_set)
    good_sha = {f"{root}/{b['path']}": b["sha256"] for b in manifest["binaries"]}
    good_mode = {f"{root}/{b['path']}": b["mode"] for b in manifest["binaries"]}
    expect_digest = f"{farm.IMG.split(':', 1)[0]}@{manifest['build']['image_digest']}"
    by_probe_path = {b["path"]: b for b in manifest["binaries"]}
    calls = []
    def fake_sh(host, cmd, timeout=120, check=False):
        calls.append((host, cmd))
        if cmd.startswith("test -d "):
            return FakeResult(rc=0)
        m = re.match(r"sha256sum (\S+)", cmd)
        if m:
            path = m.group(1)
            real = good_sha.get(path, "0" * 64)
            if host in bad_identity_hosts:
                real = ("f" if real[0] != "f" else "0") + real[1:]
            return FakeResult(out=f"{real}  {path}\n")
        m = re.match(r"stat -c %a (\S+)", cmd)
        if m:
            return FakeResult(out=good_mode.get(m.group(1), "755") + "\n")
        if "docker image inspect" in cmd:
            return FakeResult(out=expect_digest + "\n")
        if "docker run --rm" in cmd:
            for probe_rel, entry in by_probe_path.items():
                if probe_rel in cmd:
                    if host in bad_probe_hosts:
                        return FakeResult(out="WRONG-VERSION-STRING\n")
                    probe = entry["version_probe"]
                    expect = probe.get("expect_exact") or probe.get("expect_substring")
                    return FakeResult(out=expect + "\n")
            raise AssertionError(f"fake transport: docker run --rm matched no known probe path: {cmd!r}")
        raise AssertionError(f"fake transport: unrecognized command host={host} cmd={cmd!r}")
    return fake_sh, calls

def run_scenario(argv_tail, fake_sh):
    rec = Recorder()
    saved = dict(sh=farm.sh, docker_rm=farm.docker_rm, docker_run_detached=farm.docker_run_detached,
                 scratch_prepare=farm.scratch_prepare, push_file=farm.push_file, argv=sys.argv)
    farm.sh = fake_sh
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
        farm.sh = saved["sh"]
        farm.docker_rm = saved["docker_rm"]
        farm.docker_run_detached = saved["docker_run_detached"]
        farm.scratch_prepare = saved["scratch_prepare"]
        farm.push_file = saved["push_file"]
        sys.argv = saved["argv"]
    return buf.getvalue(), rec.actions

problems = []

def check(label, argv_tail, fake_sh, calls_ref, expect_probe_calls=None, before_fix_hint=""):
    out, actions = run_scenario(argv_tail, fake_sh)
    probe_calls = sum(1 for h, c in calls_ref if "docker run --rm" in c)
    print(f"-- {label}: {len(actions)} cluster action(s), {probe_calls} version-probe container(s) recorded --")
    if "REFUSED" not in out:
        problems.append(f"{label}: expected a REFUSED line in stdout, got: {out!r}")
    if actions:
        problems.append(f"{label}: expected 0 recorded CLUSTER-mutating actions, got {len(actions)}: {actions}")
    if expect_probe_calls is not None and probe_calls != expect_probe_calls:
        problems.append(f"{label}: expected exactly {expect_probe_calls} version-probe container(s), got {probe_calls}")
    if not actions and (expect_probe_calls is None or probe_calls == expect_probe_calls):
        print(f"ok - {label}: refused with 0 cluster-mutating actions ({before_fix_hint})")

COMMON = ["--workers=research6,research7,q2", "--client=q3", "--phase=up-test-down",
          "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"]

# 1) invalid INITIAL S -- identity-level failure, refuse before ANY F is
#    even resolved. 0 probes (pass 2 never starts).
fake1, calls1 = make_fake_transport("p43", bad_identity_hosts={"q3"})
check("invalid INITIAL S (identity)", COMMON, fake1, calls1, expect_probe_calls=0,
      before_fix_hint="was 6 pre-refusal + 4 finally-teardown cluster actions, plus faked-preflight blindness, before this fix")

# 2) invalid SECOND F of 3 -- identity-level failure on research7; q2 (3rd
#    worker) never even attempted. 0 probes.
fake2, calls2 = make_fake_transport("p43", bad_identity_hosts={"research7"})
check("invalid SECOND F of 3 (identity)", COMMON, fake2, calls2, expect_probe_calls=0,
      before_fix_hint="was 6 pre-refusal + 4 finally-teardown cluster actions before this fix")

# 3) invalid C -- S and ALL F resolve fine at the IDENTITY level; C (on a
#    host distinct from S/F, so its failure cannot be masked by a
#    same-host pass) fails identity. 0 probes (pass 2 never starts: pass 1
#    itself fails on C before pass 2 is ever reached).
fake3, calls3 = make_fake_transport("p43", bad_identity_hosts={"research7"})
check("invalid C, distinct host from S/F (identity)",
      ["--workers=research6", "--client=research7", "--phase=up-test-down",
       "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"],
      fake3, calls3, expect_probe_calls=0,
      before_fix_hint="S and F would have launched for real before this fix")

# 4) BO's variant: the FINAL worker (of 3) fails identity, after S, F[0],
#    AND F[1] already resolved successfully. 0 probes.
fake4, calls4 = make_fake_transport("p43", bad_identity_hosts={"q2"})
check("final worker (of 3) fails (identity)", COMMON, fake4, calls4, expect_probe_calls=0,
      before_fix_hint="docker-op list was non-empty before this fix (per BO)")

# 5) LO's second finding, now precisely reproduced and closed: EVERY role
#    passes identity (pass 1 completes in full, 0 docker actions), so pass
#    2 legitimately starts -- S's probe and F[0]'s probe (research6) run
#    for REAL (2 throwaway, non-cluster-mutating containers -- exactly
#    what LO traced), THEN F[1] (research7) fails its probe. Cluster
#    actions must still be 0: up()/run_client() are still never reached.
fake5, calls5 = make_fake_transport("p43", bad_probe_hosts={"research7"})
check("LATE probe-level failure after 2 earlier roles' probes already ran for real",
      ["--workers=research6,research7", "--client=q3", "--phase=up-test-down",
       "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"],
      fake5, calls5, expect_probe_calls=3,
      before_fix_hint="LO traced exactly this: 2 probe containers ran before a late-F refusal; now explicit, counted, and honestly a LAUNCH-phase concern -- still 0 cluster actions")

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "no-network transport-spy gates did not pass"
echo "ok - all 5 scenarios refused cleanly through the real farm.main() entry point using REAL preflight()/verify_role_version(), 0 cluster-mutating actions in every case"

echo
echo "== distribute: idempotent CONTENT-ADDRESSED publication onto all 4 hosts (q3 included -- it now also needs its own immutable-named copy) =="
DIST_OUT1=$(python3 "$FARMDIR/farm.py" distribute --sets p43,p50 --hosts q3,research6,research7,q2 2>&1)
DIST_RC1=$?
echo "$DIST_OUT1"
[ "$DIST_RC1" = "0" ] || fail "distribute (run 1) exited $DIST_RC1"
echo "ok - distribute run 1 exit 0"

DIST_OUT2=$(python3 "$FARMDIR/farm.py" distribute --sets p43,p50 --hosts q3,research6,research7,q2 2>&1)
DIST_RC2=$?
echo "$DIST_OUT2"
[ "$DIST_RC2" = "0" ] || fail "distribute (run 2) exited $DIST_RC2"
case "$DIST_OUT2" in
    *"published ("*) fail "distribute run 2 performed a publish -- not idempotent: $DIST_OUT2" ;;
esac
already=$(printf '%s\n' "$DIST_OUT2" | grep -c 'already-current')
[ "$already" = "8" ] || fail "distribute run 2: expected 8 already-current lines (2 sets x 4 hosts), got $already"
echo "ok - distribute run 2 is a pure no-op (8/8 already-current, no publish)"

echo
echo "== 24-CELL MATRIX: all 4 hosts x 2 sets x 3 roles through PRODUCTION preflight()+verify_role_version() (q3 included, not just direct version probes) =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm

HOSTS = ("q3", "research6", "research7", "q2")
SETS = ("p43", "p50")
ROLES = ("S", "F", "C")
EXPECT_FAIL_HOSTS = {"research7"}  # real, pre-existing image-digest gap

cells = 0
bad = []
for host in HOSTS:
    for binary_set in SETS:
        ierr = farm.preflight(host, binary_set)
        for role in ROLES:
            cells += 1
            if ierr:
                verdict = f"FAIL(identity): {ierr}"
                ok = host in EXPECT_FAIL_HOSTS
            else:
                verr = farm.verify_role_version(host, binary_set, role)
                if verr:
                    verdict = f"FAIL(probe): {verr}"
                    ok = False
                else:
                    verdict = "PASS"
                    ok = host not in EXPECT_FAIL_HOSTS
            print(f"  {host:10s} {binary_set:4s} {role}: {verdict}")
            if not ok:
                bad.append((host, binary_set, role, verdict))
print(f"cells checked: {cells}")
if cells != 24:
    print(f"FAIL: expected exactly 24 cells (4 hosts x 2 sets x 3 roles), got {cells}", file=sys.stderr)
    sys.exit(1)
if bad:
    print(f"FAIL: {len(bad)} cell(s) did not match their expected outcome: {bad}", file=sys.stderr)
    sys.exit(1)
print("ok - 24/24 cells match their expected outcome (18 PASS on q3/research6/q2, "
      "6 correctly FAIL-by-image-digest on research7)")
PY
[ $? -eq 0 ] || fail "24-cell matrix did not pass"

echo
echo "== research7 image-digest gap: real, pre-existing, correctly refused (not a synthetic mutant) =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm
err = farm.preflight("research7", "p43")
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
import sys

host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm

real_root = farm.immutable_root("p43")
manifest = farm.load_manifest("p43")
mutant_dir = "~/role-artifacts/p43-root-selftest-mutant"
farm.sh(host, f"rm -rf {mutant_dir} && cp -r {real_root} {mutant_dir} && chmod -R u+w {mutant_dir}", check=True)
# `cp -r` from a read-only (write-stripped) source does not necessarily
# preserve the ORIGINAL manifest-recorded modes (observed live: it does
# not, on this coreutils/umask combination) -- restore each tracked
# file's exact manifest mode so the ONLY deviation this mutant introduces
# is the deliberate sha256 corruption below, not an incidental mode drift
# from the copy itself.
for b in manifest["binaries"]:
    farm.sh(host, f"chmod {b['mode']} {mutant_dir}/{b['path']}", check=True)
# trivial touch: append one printable byte -- same embedded version string
# (it's earlier in the file), different sha256 for the whole file
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
echo "== preflight mutant (variant B -- real host, content-addressed no-auto-repair): research6's PUBLISHED immutable copy, corrupted in place, must redden, and 'distribute' must REFUSE to auto-repair it (never edits an existing final name); only an explicit rm-rf + republish recovers =="
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
    # 1) corrupt the REAL published copy in place (chmod back first -- the
    #    owner can always do this, same as any Unix permission; append one
    #    byte -- same embedded version string, different sha256).
    farm.sh(host, f"chmod u+w {root} {target} && printf 'X' >> {target}", check=True)

    # 2) preflight must now REDDEN, naming this exact file's hash mismatch.
    err1 = farm.preflight(host, binary_set)
    if err1 is None:
        print("FAIL: preflight accepted the corrupted research6 copy", file=sys.stderr)
        sys.exit(1)
    if "obj/client/icecc" not in err1 or "sha256 mismatch" not in err1:
        print(f"FAIL: preflight refused for the wrong reason: {err1}", file=sys.stderr)
        sys.exit(1)
    print(f"  step1: preflight REDDENED as required: {err1}")

    # 3) distribute must REFUSE to auto-repair -- an existing final name
    #    that fails verification is a hard error under content-addressing,
    #    never fixed in place.
    derr, dstatus = farm.publish_immutable_root(host, binary_set)
    if derr is None:
        print(f"FAIL: publish_immutable_root silently 'fixed' a corrupted immutable root "
              f"(status={dstatus!r}) -- content-addressed roots must NEVER be auto-repaired", file=sys.stderr)
        sys.exit(1)
    if "NOT auto-repaired" not in derr:
        print(f"FAIL: publish_immutable_root refused for the wrong reason: {derr}", file=sys.stderr)
        sys.exit(1)
    print(f"  step2: distribute correctly REFUSED to auto-repair: {derr[:150]}...")

    # 4) preflight must still be red (nothing silently changed).
    err2 = farm.preflight(host, binary_set)
    if err2 is None:
        print("FAIL: preflight went green with no repair having happened", file=sys.stderr)
        sys.exit(1)
    print("  step3: research6 p43 preflight still RED (confirmed nothing silently repaired it)")

    # 5) explicit, narrated recovery: rm -rf the now-invalid immutable path,
    #    then republish fresh under the SAME content-addressed name.
    farm.sh(host, f"chmod -R u+w {root} 2>/dev/null; rm -rf {root}", check=True)
    derr2, dstatus2 = farm.publish_immutable_root(host, binary_set)
    if derr2 or dstatus2 != "published (root was absent)":
        print(f"FAIL: explicit rm-rf + republish did not cleanly recover (err={derr2!r} status={dstatus2!r})", file=sys.stderr)
        sys.exit(1)
    print(f"  step4: explicit rm-rf + republish recovered: {dstatus2}")

    err3 = farm.preflight(host, binary_set)
    if err3:
        print(f"FAIL: preflight still red after explicit recovery: {err3}", file=sys.stderr)
        sys.exit(1)
    print("  step5: research6 p43 preflight PASS again (post-recovery)")

    derr3, dstatus3 = farm.publish_immutable_root(host, binary_set)
    if derr3 or dstatus3 != "already-current":
        print(f"FAIL: post-recovery distribute was not a clean no-op (err={derr3!r} status={dstatus3!r})", file=sys.stderr)
        sys.exit(1)
    print("  step6: post-recovery re-run is already-current (recovery result is itself idempotent)")
    print("ok - corrupt-then-refuse-to-autofix-then-explicit-recovery cycle passed on the REAL research6 host")
except BaseException:
    # Unconditional safety net: never leave a corrupted or half-recovered
    # immutable root on a shared host.
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
echo "== RACE GATE: pause after complete plan resolution, race a concurrent distribute + an out-of-band tamper against the selected immutable root, then hash from INSIDE a real launched container =="
python3 - "research6" "q3" "$FARMDIR" <<'PY'
import sys

host, incontainer_host, farmdir = sys.argv[1], sys.argv[2], sys.argv[3]
sys.path.insert(0, farmdir)
import farm
import os

binary_set = "p43"
root = farm.immutable_root(binary_set)
manifest = farm.load_manifest(binary_set)

# --- Setup: resolve for real, exactly like resolve_launch_plan() would,
#     capturing the exact immutable path a launch would have committed to. ---
err_setup = farm.preflight(host, binary_set)
if err_setup:
    print(f"FAIL: {host} not clean going into the race-gate test: {err_setup}", file=sys.stderr)
    sys.exit(1)
print(f"  setup: plan resolution captured immutable root = {root}")

# --- Property A: a concurrent `distribute` for the SAME set/host, racing
#     immediately after this resolution, must be a pure no-op -- content
#     addressing means it can only ever no-op (already-current) or publish
#     a DIFFERENT new name; it must never touch THIS already-selected one. ---
before = farm.verify_role_files(host, root, manifest)
if before:
    print(f"FAIL: root was not actually clean pre-race: {before}", file=sys.stderr)
    sys.exit(1)
derr, dstatus = farm.publish_immutable_root(host, binary_set)
after = farm.verify_role_files(host, root, manifest)
if derr is not None or dstatus != "already-current" or after:
    print(f"FAIL: property A -- concurrent distribute did not no-op cleanly "
          f"(err={derr!r} status={dstatus!r} after={after!r})", file=sys.stderr)
    sys.exit(1)
print("  ok - property A: concurrent distribute was a pure no-op; the resolved immutable root is untouched")

# --- Property B: an out-of-band tamper landing in the window between
#     resolution and the actual container start must be caught by
#     revalidate_before_mutation() -- the race gate's second, independent
#     defense (the first being that publish never edits an existing final
#     name at all). Restored via explicit rm-rf + republish afterward,
#     content-addressing's only valid recovery path. ---
target = f"{root}/obj/client/icecc"
try:
    farm.sh(host, f"chmod u+w {root} {target} && printf 'X' >> {target}", check=True)
    try:
        farm.revalidate_before_mutation(host, binary_set)
        print("FAIL: revalidate_before_mutation ACCEPTED a tampered immutable root -- "
              "the race window is open", file=sys.stderr)
        sys.exit(1)
    except RuntimeError as e:
        if "sha256 mismatch" not in str(e):
            print(f"FAIL: revalidate_before_mutation refused for the wrong reason: {e}", file=sys.stderr)
            sys.exit(1)
        print(f"  ok - property B: revalidate_before_mutation REFUSED the out-of-band tamper: {e}")
finally:
    farm.sh(host, f"chmod -R u+w {root} 2>/dev/null; rm -rf {root}")
    derr2, dstatus2 = farm.publish_immutable_root(host, binary_set)
    if derr2 or farm.preflight(host, binary_set):
        print(f"FAIL: could not restore {host} to a clean state after the tamper test "
              f"(err={derr2!r})", file=sys.stderr)
        sys.exit(1)
    print(f"  ok - {host} restored to a fully clean, verified state ({dstatus2})")

# --- After a REAL launch (one minimal, uniquely-named, always-torn-down
#     container -- never farm-sched/farm-worker/farm-client), hash the
#     executable FROM INSIDE the running container against the manifest,
#     and confirm :ro is enforced from inside a real launched container
#     too (not just the isolated docker-run probe in the earlier :ro
#     unit check). ---
role = "F"
probe_rel = farm._ROLE_PROBE_PATH[role]
entry = next(b for b in manifest["binaries"] if b["path"] == probe_rel)
img = farm.launch_image(binary_set)
name = f"s4-race-gate-probe-{os.getpid()}"
try:
    farm.sh(incontainer_host, f"docker run -d --name {name} --network host -v {root}:/work:ro -u 0:0 {img} sleep 60", check=True)
    r = farm.sh(incontainer_host, f"docker exec {name} sha256sum /work/{probe_rel}", timeout=30, check=True)
    actual_sha = r.stdout.split()[0]
    if actual_sha != entry["sha256"]:
        print(f"FAIL: in-container hash {actual_sha} != manifest {entry['sha256']}", file=sys.stderr)
        sys.exit(1)
    print(f"  ok - hash observed FROM INSIDE the running container matches the manifest exactly ({actual_sha})")
    rw = farm.sh(incontainer_host, f"docker exec {name} sh -c \"echo TAMPER >> /work/{probe_rel}; echo rc=$?\"", timeout=30)
    if "Read-only" not in (rw.stdout + rw.stderr):
        print(f"FAIL: expected a read-only filesystem error writing through :ro from inside the "
              f"container, got stdout={rw.stdout!r} stderr={rw.stderr!r}", file=sys.stderr)
        sys.exit(1)
    print("  ok - :ro is enforced from inside a REAL launched container too")
finally:
    farm.sh(incontainer_host, f"docker rm -f {name} 2>/dev/null; true")
    chk = farm.sh(incontainer_host, f"docker ps -a --filter name={name} --format '{{{{.Names}}}}'")
    if chk.stdout.strip():
        print(f"FAIL: cleanup did not remove {name} on {incontainer_host}", file=sys.stderr)
        sys.exit(1)
    print(f"  ok - cleanup confirmed: {name} no longer exists on {incontainer_host}")

print("ok - RACE GATE: concurrent-distribute non-interference, out-of-band-tamper revalidation-refusal, "
      "and in-container hash-vs-manifest identity all confirmed on real hosts")
PY
[ $? -eq 0 ] || fail "race gate did not pass"

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
echo "== skipped-preflight mutant: neutralizing resolve_role() in resolve_launch_plan() must make the PREFLIGHT-OK marker for that role DISAPPEAR =="
# Safety: this mutates farm.py's OWN source on disk, then imports the
# mutated module in a fresh subprocess. It NEVER calls a code path that
# performs a real docker/ssh cluster-mutating action -- sh() is
# monkeypatched so that the moment execution would touch a
# farm-sched/farm-worker/farm-client container (or the scratch-prep/log
# commands around them), the call is intercepted and faked instead of
# reaching the network; every OTHER command (real, read-only
# preflight/verify_role_version checks) passes through to the real
# transport. farm.py itself is always restored (cmp-verified) before this
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
echo "PASS: artifact_selection_test -- selection mechanism changes the launched binary, not merely its label; resolution is genuinely docker-free and launch-validation is honestly a separate later phase; distribute publishes content-addressed immutable roots that are never repaired in place; the race gate holds under a concurrent distribute and an out-of-band tamper; all 24 (host,set,role) cells match production preflight()"
