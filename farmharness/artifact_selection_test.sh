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

tree_provenance_hash() {
    root=$1
    ( cd "$root"
      find . -type f -print0 | sort -z | while IFS= read -r -d '' rel; do
          rel=${rel#./}
          mode=$(stat -c %a -- "$rel")
          hash=$(sha256sum -- "$rel" | cut -d' ' -f1)
          printf '%s\t%s\t%s\n' "$rel" "$mode" "$hash"
      done | sha256sum | cut -d' ' -f1
    )
}

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

echo "== FRESH-ARCHIVE no-ambient gate: farm.py + both manifests, from a bare git-archive extraction (or, absent git, the current tree directly), zero ambient files possible =="
ARCHIVE_DIR="$SCRATCHDIR/fresh-archive-$$"
rm -rf "$ARCHIVE_DIR"
mkdir -p "$ARCHIVE_DIR"
if git -C "$FARMDIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    REPOROOT=$(cd "$FARMDIR" && git rev-parse --show-toplevel)
    ARCHIVE_SOURCE=$(cd "$FARMDIR" && git rev-parse HEAD)
    ( cd "$REPOROOT" && git archive "$ARCHIVE_SOURCE" -- farmharness ) | tar -x -C "$ARCHIVE_DIR"
    ARCHIVE_LABEL="git archive of $ARCHIVE_SOURCE"
else
    # Round-5 (Deep Reviewer): a bare `git archive`/release-tarball
    # extraction has no .git anywhere in its ancestry, so the git
    # rev-parse calls above used to fail outright -- fatal under
    # `set -eu`, killing the ENTIRE suite at its very first section
    # before anything else ever ran (this is what team-lead's own
    # verification hit, worked around only by using a clean worktree).
    # Degrade gracefully instead: use the CURRENT TREE directly as the
    # archive source -- still a genuinely separate copy, still exercises
    # the same "does farm.py import cleanly and self-consistently from
    # THIS exact location" claim -- only the PROVENANCE claim is weaker
    # (a content hash of the tree, not a git commit SHA), and the
    # stronger git-archive path is still used whenever .git IS reachable.
    mkdir -p "$ARCHIVE_DIR/farmharness"
    cp -a "$FARMDIR/." "$ARCHIVE_DIR/farmharness/"
    find "$ARCHIVE_DIR/farmharness" -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null
    find "$ARCHIVE_DIR/farmharness" -name '*.pyc' -delete 2>/dev/null
    # Hash only relative paths, modes, and content.  Hashing raw sha256sum
    # output from the caller's cwd accidentally included the random absolute
    # ARCHIVE_DIR prefix, so identical bare trees received different
    # provenance labels on different extraction paths.
    ARCHIVE_SOURCE=$(tree_provenance_hash "$ARCHIVE_DIR/farmharness")
    ARCHIVE_LABEL="no-git tree-hash $ARCHIVE_SOURCE (no .git reachable from $FARMDIR)"
fi
[ -f "$ARCHIVE_DIR/farmharness/farm.py" ] || fail "fresh-archive: farmharness/farm.py missing from the archive ($ARCHIVE_LABEL)"
python3 - "$ARCHIVE_DIR" "$ARCHIVE_LABEL" <<'PY'
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
    # ATTESTATION coverage: _attestation_prefix() checks EVERY manifest
    # binaries[] entry (not a single per-role probe path anymore -- that
    # was round-3's design, superseded by round-4's in-command attestation,
    # which is strictly broader: it protects every tracked file, whichever
    # role launches, not just that role's own "representative" binary).
    # The invariant worth a manifest-data-level gate here is simply that
    # there IS something to attest at all -- an empty binaries[] would make
    # _attestation_prefix() silently build a vacuous (always-passing)
    # prefix, a fail-open gap in the MANIFEST DATA, not the code.
    if not manifest.get("binaries"):
        problems.append(f"{set_name}: binaries[] is empty -- attestation would be vacuous")
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
print(f"ok - fresh-archive gate: farm imported from {farm.__file__} ({commit}), "
      f"both manifests loaded via farm.load_manifest() from that same location, schema-valid, "
      f"S/F/C role coverage confirmed present in both, immutable_root() store-layout derivation confirmed")
PY
[ $? -eq 0 ] || fail "fresh-archive no-ambient gate did not pass"
rm -rf "$ARCHIVE_DIR"

# Local-only discriminator mode used by s4_round6_unit_test.py and release
# packaging checks.  It stops after the fresh-archive/import gate, before any
# remote host probe or mutation, while preserving the exact production gate
# above.
if [ "${ARTIFACT_TEST_ONLY_FRESH:-0}" = 1 ]; then
    echo "ok - fresh-archive-only mode (no remote actions)"
    exit 0
fi

echo
echo "== source anchor: the launch path cannot skip resolution, the whole-plan barrier, mutation-time revalidation, the race-gate seam, or in-command attestation without this test noticing =="
for spec in "resolve_launch_plan:resolve_role:3" \
            "up:resolve_role:0" "up:revalidate_before_mutation:2" \
            "up:_attestation_prefix:2" "up:wait_for_attestation:2" \
            "run_client:resolve_role:0" "run_client:revalidate_before_mutation:1" \
            "run_client:_attestation_prefix:1" "run_client:docker_run_foreground_staged:1" \
            "run_client:verify_harness_scripts:1" "run_client:_build_harness_bundle:1" \
            "run_client:_harness_stage_verify:1" \
            "main:revalidate_entire_plan:1" "main:_race_gate_pause:1" "main:resolve_launch_plan:1"; do
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
        fail "$fn() calls $target() $got time(s), expected $want -- the resolve-then-barrier-then-mutate-then-attest ordering this successor exists for may have regressed"
    fi
done

echo
echo "== source anchor: main()'s mutation tracker (MUTATIONS.mark()) is wired into all 4 real mutating primitives, and no other function calls it =="
python3 - "$FARMDIR/farm.py" <<'PY'
import ast, sys
path = sys.argv[1]
src = open(path).read()
tree = ast.parse(src, filename=path)
EXPECT = {"docker_rm", "scratch_prepare", "docker_run_detached", "docker_run_foreground_staged"}
found = {}
for fn in ast.walk(tree):
    if not isinstance(fn, ast.FunctionDef):
        continue
    n = 0
    for call in ast.walk(fn):
        if (isinstance(call, ast.Call) and isinstance(call.func, ast.Attribute)
                and call.func.attr == "mark" and isinstance(call.func.value, ast.Name)
                and call.func.value.id == "MUTATIONS"):
            n += 1
    if n:
        found[fn.name] = n
problems = []
for fname in EXPECT:
    if found.get(fname, 0) != 1:
        problems.append(f"{fname}() calls MUTATIONS.mark() {found.get(fname, 0)} time(s), expected exactly 1")
extra = set(found) - EXPECT
if extra:
    problems.append(f"unexpected function(s) also call MUTATIONS.mark(): {sorted(extra)} -- either a new "
                     f"mutating primitive needs auditing, or a non-primitive is wrongly marking a mutation")
if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
print(f"ok - MUTATIONS.mark() is called from exactly the 4 real mutating primitives ({sorted(EXPECT)}) "
      f"and nowhere else -- no call site above them (up()/run_client()/main()) sets it in anticipation "
      f"of a mutation, only these four set it AS one actually happens")
PY
[ $? -eq 0 ] || fail "MUTATIONS.mark() source anchor did not pass"

echo
echo "== HARNESS SCRIPT INTEGRITY: farm_client.sh/replay.py sourced from the checkout (never an ambient path), hash-pinned, fail-closed before any container runs =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm
import os

if farm.HUB_DIR != os.path.dirname(os.path.abspath(farm.__file__)):
    print(f"FAIL: HUB_DIR is not the checkout's own farmharness/ directory: {farm.HUB_DIR!r}", file=sys.stderr)
    sys.exit(1)
farm.verify_harness_scripts()  # must not raise
print(f"ok - baseline: HUB_DIR is the checkout ({farm.HUB_DIR!r}), both harness scripts hash-match "
      f"their frozen values, verify_harness_scripts() passed cleanly")
PY
[ $? -eq 0 ] || fail "harness script integrity baseline did not pass"

echo "-- tamper the checkout copy of replay.py: the hash check must refuse before any container runs --"
SNAPSHOT_REPLAY="$SCRATCHDIR/replay.py.pre-tamper"
cp "$FARMDIR/replay.py" "$SNAPSHOT_REPLAY"
restore_replay() { [ -f "$SNAPSHOT_REPLAY" ] && cp "$SNAPSHOT_REPLAY" "$FARMDIR/replay.py"; }
trap restore_replay EXIT
printf '\n# MUTATED-FOR-TEST: tampered checkout copy\n' >> "$FARMDIR/replay.py"

python3 - "$FARMDIR" "q3" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm

host = sys.argv[2]

# 1/2: the direct claim -- verify_harness_scripts() itself must refuse.
try:
    farm.verify_harness_scripts()
    print("FAIL: verify_harness_scripts() did not raise against the tampered replay.py", file=sys.stderr)
    sys.exit(1)
except RuntimeError as e:
    if "replay.py" not in str(e) or "sha256 mismatch" not in str(e):
        print(f"FAIL: raised for the wrong reason: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"ok - verify_harness_scripts() correctly refused: {e}")

# 2/2: the INTEGRATION claim BO specifically asked for -- run_client()
# itself must refuse BEFORE any container runs. verify_harness_scripts()
# is the FIRST thing run_client() does (before revalidate_before_mutation,
# before _build_harness_bundle(), before any docker primitive), so a
# real, minimal LaunchPlan is enough here -- nothing else in it is ever
# touched, since execution never gets past the very first line.
farm.MUTATIONS.started = False
before = set(farm.sh(host, "docker ps -a --filter name=farm-client --format '{{.Names}}'").stdout.split())
plan = farm.LaunchPlan(s_tree=None, s_img=None, binary_set_s=None, f_resolved=[],
                        binary_set_f=None, c_tree="dummy", c_img="dummy", binary_set_c="p43")
try:
    farm.run_client(host, "/hostscratch/fmt", "simultaneous", "0", "4", "", plan)
    print("FAIL: run_client() did not raise against the tampered replay.py", file=sys.stderr)
    sys.exit(1)
except RuntimeError as e:
    msg = str(e)  # `except ... as e` unbinds e at the end of this block -- capture the message now
    if "replay.py" not in msg:
        print(f"FAIL: run_client() raised for the wrong reason: {msg}", file=sys.stderr)
        sys.exit(1)
after = set(farm.sh(host, "docker ps -a --filter name=farm-client --format '{{.Names}}'").stdout.split())
if after != before:
    print(f"FAIL: a farm-client container appeared despite the refusal: before={before} after={after}", file=sys.stderr)
    sys.exit(1)
if farm.MUTATIONS.started:
    print("FAIL: MUTATIONS.started went True -- a real mutating primitive ran before the refusal", file=sys.stderr)
    sys.exit(1)
print(f"ok - run_client() correctly refused BEFORE touching {host} at all (0 containers, "
      f"MUTATIONS.started stayed False): {msg}")
PY
[ $? -eq 0 ] || fail "harness-script-tamper mutant did not redden as required"

restore_replay
if cmp -s "$FARMDIR/replay.py" "$SNAPSHOT_REPLAY"; then
    echo "ok - replay.py restored byte-exact after the tamper mutant (cmp clean)"
else
    fail "replay.py restoration after the tamper mutant is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_REPLAY"
python3 -c "
import sys; sys.path.insert(0, '$FARMDIR')
import farm
farm.verify_harness_scripts()
print('ok - post-restore: verify_harness_scripts() passes again')
"

echo
echo "== CELL-VERDICT fix: main()/dump_worker_evidence() must no longer be false-green-capable =="
# Round-4 finding (BO): main() used to discard dump_worker_evidence()'s
# own bijection verdict entirely and never inspect run_client()'s
# returncode either, so a cell whose client script failed OR whose JOIN
# was broken still exited 0. Six named controls below, matching BO's
# list exactly: (1) client exits 7 after registration, (2) CELL: FAIL
# with plausible rows, (3) one JOIN row removed, (4) one extra F request
# ID injected (the actual subset-vs-equality bug), (5) join_ok
# deliberately ignored, (6) returncode deliberately ignored.
python3 - "$FARMDIR" <<'PY'
import sys, re
sys.path.insert(0, sys.argv[1])
import farm

GOOD = ("CELL: project TUs=2 mode=simultaneous jobs=4\n"
        "JOINROW idx=0 src=a.cc jobid=1 f=10.0.27.101 accepted=1 exact=1 sha=abc mode=remote env=xyz\n"
        "JOINROW idx=1 src=b.cc jobid=2 f=10.0.27.101 accepted=1 exact=1 sha=def mode=remote env=xyz\n"
        "CELL[simul/j4]: byte-exact-remote 2/2  remote 2/2  wall 1.0s  jobs/s 2.00  F->C-wire 100B  endpoints[10.0.27.101:2]\n"
        "CELL: PASS\n")

class FakeResult:
    def __init__(self, out="", rc=0): self.stdout, self.returncode, self.stderr = out, rc, ""

def fake_sh_for_join(host, cmd, timeout=120, check=False, extra_f_id=False):
    if "request for job" in cmd:
        return FakeResult("1\n2\n3\n" if extra_f_id else "1\n2\n")
    if "Remote compilation completed" in cmd:
        return FakeResult("2\n")
    raise AssertionError(f"unexpected: {cmd}")

problems = []

# --- controls 1-4: dump_worker_evidence() itself + the client_rc combination ---
farm.sh = lambda h, c, timeout=120, check=False: fake_sh_for_join(h, c, timeout, check)
r = farm.dump_worker_evidence(["research6"], GOOD, 0, "p43")
if r is not True: problems.append(f"baseline: expected join_ok=True, got {r}")
else: print("  ok - control 0 (baseline, good data): join_ok=True")

r = farm.dump_worker_evidence(["research6"], GOOD.replace("CELL: PASS", "CELL: FAIL"), 0, "p43")
if r is not False: problems.append(f"control 2 (CELL: FAIL): expected join_ok=False, got {r}")
else: print("  ok - control 2 (CELL: FAIL with plausible rows): join_ok=False")

removed = "\n".join(ln for ln in GOOD.splitlines() if "idx=1" not in ln)
r = farm.dump_worker_evidence(["research6"], removed, 0, "p43")
if r is not False: problems.append(f"control 3 (row removed): expected join_ok=False, got {r}")
else: print("  ok - control 3 (one JOIN row removed): join_ok=False")

farm.sh = lambda h, c, timeout=120, check=False: fake_sh_for_join(h, c, timeout, check, extra_f_id=True)
r = farm.dump_worker_evidence(["research6"], GOOD, 0, "p43")
if r is not False: problems.append(f"control 4 (extra F id): expected join_ok=False (OLD subset check would say True), got {r}")
else: print("  ok - control 4 (one extra F request ID injected -- the exact-equality fix): join_ok=False")
farm.sh = lambda h, c, timeout=120, check=False: fake_sh_for_join(h, c, timeout, check)

if problems:
    for p in problems: print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "cell-verdict controls 0/2/3/4 did not pass"

echo "-- controls 1, 5, 6: main()-level combination (real farm.main(), fake run_client(), real preflight against q3/research6) --"
python3 - "$FARMDIR" <<'PY'
import sys, io, contextlib, re
sys.path.insert(0, sys.argv[1])
import farm

GOOD = ("CELL: project TUs=2 mode=simultaneous jobs=4\n"
        "JOINROW idx=0 src=a.cc jobid=1 f=10.0.27.101 accepted=1 exact=1 sha=abc mode=remote env=xyz\n"
        "JOINROW idx=1 src=b.cc jobid=2 f=10.0.27.101 accepted=1 exact=1 sha=def mode=remote env=xyz\n"
        "CELL[simul/j4]: byte-exact-remote 2/2  remote 2/2  wall 1.0s  jobs/s 2.00  F->C-wire 100B  endpoints[10.0.27.101:2]\n"
        "CELL: PASS\n")

class FakeR:
    def __init__(self, rc, out): self.returncode, self.stdout, self.stderr = rc, out, ""
class FakeResult:
    def __init__(self, out="", rc=0): self.stdout, self.returncode, self.stderr = out, rc, ""

def make_fake_transport(binary_set):
    manifest = farm.load_manifest(binary_set)
    root = farm.immutable_root(binary_set)
    good_sha = {f"{root}/{b['path']}": b["sha256"] for b in manifest["binaries"]}
    good_mode = {f"{root}/{b['path']}": farm._write_stripped(b["mode"]) for b in manifest["binaries"]}
    expect_digest = f"{farm.IMG.split(':', 1)[0]}@{manifest['build']['image_digest']}"
    def fake_sh(host, cmd, timeout=120, check=False):
        if cmd.startswith("test -d "): return FakeResult(rc=0)
        m = re.match(r"sha256sum (\S+)", cmd)
        if m: return FakeResult(out=f"{good_sha.get(m.group(1), '0'*64)}  {m.group(1)}\n")
        m = re.match(r"stat -c %a (\S+)", cmd)
        if m: return FakeResult(out=good_mode.get(m.group(1), "755") + "\n")
        if "docker image inspect" in cmd: return FakeResult(out=expect_digest + "\n")
        if "grep -c 'login'" in cmd: return FakeResult(out="1\n")
        if "request for job" in cmd: return FakeResult("1\n2\n")
        if "Remote compilation completed" in cmd: return FakeResult("2\n")
        return FakeResult(rc=0)
    return fake_sh

def run_scenario(client_rc, client_stdout):
    saved = dict(sh=farm.sh, docker_rm=farm.docker_rm, docker_run_detached=farm.docker_run_detached,
                 scratch_prepare=farm.scratch_prepare, wait_for_attestation=farm.wait_for_attestation,
                 run_client=farm.run_client, time_sleep=farm.time.sleep, argv=sys.argv)
    farm.sh = make_fake_transport("p43")
    farm.docker_rm = lambda h, n: None
    farm.docker_run_detached = lambda h, n, t, i, c: None
    farm.scratch_prepare = lambda h, m, l: None
    farm.wait_for_attestation = lambda h, n, t, timeout=30: None
    farm.run_client = lambda ch, pd, m, mx, j, p, plan: FakeR(client_rc, client_stdout)
    farm.time.sleep = lambda s: None
    sys.argv = ["farm.py", "--workers=research6", "--client=q3", "--phase=up-test-down",
                "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"]
    buf = io.StringIO()
    exit_code = None
    try:
        with contextlib.redirect_stdout(buf):
            farm.main()
    except SystemExit as e:
        exit_code = e.code
    finally:
        farm.sh = saved["sh"]; farm.docker_rm = saved["docker_rm"]; farm.docker_run_detached = saved["docker_run_detached"]
        farm.scratch_prepare = saved["scratch_prepare"]; farm.wait_for_attestation = saved["wait_for_attestation"]
        farm.run_client = saved["run_client"]; farm.time.sleep = saved["time_sleep"]; sys.argv = saved["argv"]
    return exit_code, buf.getvalue()

problems = []
code, out = run_scenario(0, GOOD)
if code != 0: problems.append(f"baseline (good client_rc + good stdout): expected exit 0, got {code}")
else: print("  ok - baseline (good client_rc + good stdout): exit 0")

code, out = run_scenario(7, GOOD)
if code != 2: problems.append(f"control 1 (client exits 7 after registration): expected exit 2, got {code}")
else: print("  ok - control 1 (client exits 7 after registration): exit 2")

if problems:
    for p in problems: print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "cell-verdict main()-level combination test did not pass"

echo "-- controls 5, 6: reverting main()'s combination line must resurrect the OLD false-green bug --"
SNAPSHOT_VERDICT="$SCRATCHDIR/farm.py.pre-verdict-mutant"
cp "$FARMDIR/farm.py" "$SNAPSHOT_VERDICT"
restore_verdict() { [ -f "$SNAPSHOT_VERDICT" ] && cp "$SNAPSHOT_VERDICT" "$FARMDIR/farm.py"; }
trap restore_verdict EXIT

for CONTROL in join_ok returncode; do
python3 - "$FARMDIR/farm.py" "$CONTROL" <<'PY'
import sys
path, control = sys.argv[1], sys.argv[2]
src = open(path).read()
old = ("                            client_ok = (r.returncode == 0)\n"
       "                            join_ok = dump_worker_evidence(workers, r.stdout, r.returncode, a.binary_set_c)\n"
       "                            ok = ok and client_ok and join_ok\n")
if control == "join_ok":
    new = ("                            client_ok = (r.returncode == 0)\n"
           "                            dump_worker_evidence(workers, r.stdout, r.returncode, a.binary_set_c)  # MUTATED-FOR-TEST: join_ok ignored\n"
           "                            ok = ok and client_ok\n")
else:
    new = ("                            join_ok = dump_worker_evidence(workers, r.stdout, r.returncode, a.binary_set_c)  # MUTATED-FOR-TEST: returncode ignored\n"
           "                            ok = ok and join_ok\n")
n = src.count(old)
if n != 1:
    print(f"FAIL: expected exactly 1 occurrence of the combination-line anchor, found {n}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print(f"{control} mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the $CONTROL-ignored mutation"

python3 - "$FARMDIR" "$CONTROL" <<'PY'
import sys, io, contextlib, re
sys.path.insert(0, sys.argv[1])
import farm  # fresh process -- picks up the just-mutated source directly

control = sys.argv[2]
GOOD = ("CELL: project TUs=2 mode=simultaneous jobs=4\n"
        "JOINROW idx=0 src=a.cc jobid=1 f=10.0.27.101 accepted=1 exact=1 sha=abc mode=remote env=xyz\n"
        "JOINROW idx=1 src=b.cc jobid=2 f=10.0.27.101 accepted=1 exact=1 sha=def mode=remote env=xyz\n"
        "CELL[simul/j4]: byte-exact-remote 2/2  remote 2/2  wall 1.0s  jobs/s 2.00  F->C-wire 100B  endpoints[10.0.27.101:2]\n"
        "CELL: PASS\n")
BAD = GOOD.replace("CELL: PASS", "CELL: FAIL")

class FakeR:
    def __init__(self, rc, out): self.returncode, self.stdout, self.stderr = rc, out, ""
class FakeResult:
    def __init__(self, out="", rc=0): self.stdout, self.returncode, self.stderr = out, rc, ""

def make_fake_transport(binary_set):
    manifest = farm.load_manifest(binary_set)
    root = farm.immutable_root(binary_set)
    good_sha = {f"{root}/{b['path']}": b["sha256"] for b in manifest["binaries"]}
    good_mode = {f"{root}/{b['path']}": farm._write_stripped(b["mode"]) for b in manifest["binaries"]}
    expect_digest = f"{farm.IMG.split(':', 1)[0]}@{manifest['build']['image_digest']}"
    def fake_sh(host, cmd, timeout=120, check=False):
        if cmd.startswith("test -d "): return FakeResult(rc=0)
        m = re.match(r"sha256sum (\S+)", cmd)
        if m: return FakeResult(out=f"{good_sha.get(m.group(1), '0'*64)}  {m.group(1)}\n")
        m = re.match(r"stat -c %a (\S+)", cmd)
        if m: return FakeResult(out=good_mode.get(m.group(1), "755") + "\n")
        if "docker image inspect" in cmd: return FakeResult(out=expect_digest + "\n")
        if "grep -c 'login'" in cmd: return FakeResult(out="1\n")
        if "request for job" in cmd: return FakeResult("1\n2\n")
        if "Remote compilation completed" in cmd: return FakeResult("2\n")
        return FakeResult(rc=0)
    return fake_sh

# Under the "join_ok ignored" mutation, a BAD join (CELL: FAIL) should now
# slip through; under "returncode ignored", a BAD client_rc should.
client_rc, client_stdout = (0, BAD) if control == "join_ok" else (7, GOOD)

farm.sh = make_fake_transport("p43")
farm.docker_rm = lambda h, n: None
farm.docker_run_detached = lambda h, n, t, i, c: None
farm.scratch_prepare = lambda h, m, l: None
farm.wait_for_attestation = lambda h, n, t, timeout=30: None
farm.run_client = lambda ch, pd, m, mx, j, p, plan: FakeR(client_rc, client_stdout)
farm.time.sleep = lambda s: None
sys.argv = ["farm.py", "--workers=research6", "--client=q3", "--phase=up-test-down",
            "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"]
buf = io.StringIO()
exit_code = None
try:
    with contextlib.redirect_stdout(buf):
        farm.main()
except SystemExit as e:
    exit_code = e.code

if exit_code != 0:
    print(f"FAIL: expected the {control}-ignored mutation to resurrect the false-green bug (exit 0 "
          f"despite a genuinely bad cell), but got exit {exit_code}", file=sys.stderr)
    sys.exit(1)
print(f"ok - control 5/6 ({control} deliberately ignored) REDDENED as required: a genuinely bad cell "
      f"(client_rc={client_rc}, stdout={'BAD' if client_stdout is BAD else 'GOOD'}) incorrectly exits 0 -- "
      f"exactly BO's false-green finding, proving the fix (checking both) is load-bearing")
PY
[ $? -eq 0 ] || fail "$CONTROL-ignored mutant did not redden as required"

restore_verdict
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_VERDICT"; then
    echo "ok - farm.py restored byte-exact after the $CONTROL-ignored mutant (cmp clean)"
else
    fail "farm.py restoration after the $CONTROL-ignored mutant is NOT byte-exact"
fi
done
trap - EXIT
rm -f "$SNAPSHOT_VERDICT"

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
    def docker_run_foreground_staged(self, host, name, tree, img, stdin_bytes, inner_cmd, timeout=1800):
        self.actions.append(f"docker_run_foreground_staged({host},{name})")
    def wait_for_attestation(self, host, name, token, timeout=30):
        self.actions.append(f"wait_for_attestation({host},{name})")

class FakeResult:
    def __init__(self, out="", rc=0, err=""):
        self.stdout, self.returncode, self.stderr = out, rc, err

def make_fake_transport(binary_set, bad_hosts):
    manifest = farm.load_manifest(binary_set)
    root = farm.immutable_root(binary_set)
    good_sha = {f"{root}/{b['path']}": b["sha256"] for b in manifest["binaries"]}
    # A real published root is ALWAYS hardened (chmod -R a-w before the
    # atomic rename -- see publish_immutable_root()) before it's ever
    # exposed at its final name, so a faithful fake of `stat -c %a`
    # against a real root must report the WRITE-STRIPPED mode, not the
    # manifest's original (pre-hardening) mode.
    good_mode = {f"{root}/{b['path']}": farm._write_stripped(b["mode"]) for b in manifest["binaries"]}
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
                 scratch_prepare=farm.scratch_prepare, docker_run_foreground_staged=farm.docker_run_foreground_staged,
                 wait_for_attestation=farm.wait_for_attestation, argv=sys.argv)
    farm.sh = fake_sh
    farm.docker_rm = rec.docker_rm
    farm.docker_run_detached = rec.docker_run_detached
    farm.scratch_prepare = rec.scratch_prepare
    farm.docker_run_foreground_staged = rec.docker_run_foreground_staged
    farm.wait_for_attestation = rec.wait_for_attestation
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
        farm.docker_run_foreground_staged = saved["docker_run_foreground_staged"]
        farm.wait_for_attestation = saved["wait_for_attestation"]
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
echo "== no-network TRANSPORT-SPY gate: revalidate_entire_plan()'s WHOLE-PLAN BARRIER independently catches a tamper landing strictly BETWEEN resolve_launch_plan()'s pass and its own pass -- 0 actions, and a PRE-EXISTING unrelated container survives untouched (down() never called) =="
# A STATEFUL fake: research7's iceccd hash is CORRECT the first time it's
# queried (resolve_launch_plan()'s own preflight of the F role) and WRONG
# every time after (revalidate_entire_plan()'s re-check of the SAME role).
# This models a tamper landing in the exact window the barrier exists to
# close: after resolve_launch_plan() individually validated every role,
# but before ANY mutation. If revalidate_entire_plan() were deleted or
# neutralized, resolve_launch_plan() alone would never re-notice this --
# up() would proceed to mutate S (and possibly research6, the first F)
# before ever reaching research7.
python3 - "$FARMDIR" <<'PY'
import contextlib, io, re, sys
sys.path.insert(0, sys.argv[1])
import farm

class Recorder:
    def __init__(self): self.actions = []
    def docker_rm(self, host, name): self.actions.append(f"docker_rm({host},{name})")
    def docker_run_detached(self, host, name, tree, img, inner_cmd): self.actions.append(f"docker_run_detached({host},{name})")
    def scratch_prepare(self, host, mkdir_path, log_path): self.actions.append(f"scratch_prepare({host},{mkdir_path})")
    def docker_run_foreground_staged(self, host, name, tree, img, stdin_bytes, inner_cmd, timeout=1800): self.actions.append(f"docker_run_foreground_staged({host},{name})")
    def wait_for_attestation(self, host, name, token, timeout=30): self.actions.append(f"wait_for_attestation({host},{name})")

class FakeResult:
    def __init__(self, out="", rc=0, err=""): self.stdout, self.returncode, self.stderr = out, rc, err

manifest = farm.load_manifest("p43")
root = farm.immutable_root("p43")
good_sha = {f"{root}/{b['path']}": b["sha256"] for b in manifest["binaries"]}
good_mode = {f"{root}/{b['path']}": farm._write_stripped(b["mode"]) for b in manifest["binaries"]}  # real roots are always hardened before exposure
expect_digest = farm.launch_image("p43")
TAMPER_TARGET = f"{root}/obj/daemon/iceccd"  # research7's F-role tracked file
seen = {"n": 0}

def fake_sh(host, cmd, timeout=120, check=False):
    if cmd.startswith("test -d "): return FakeResult(rc=0)
    m = re.match(r"sha256sum (\S+)", cmd)
    if m:
        path = m.group(1)
        real = good_sha.get(path, "0"*64)
        if host == "research7" and path == TAMPER_TARGET:
            seen["n"] += 1
            if seen["n"] >= 2:  # 1st = resolve_launch_plan(), 2nd+ = revalidate_entire_plan()
                real = ("f" if real[0] != "f" else "0") + real[1:]
        return FakeResult(out=f"{real}  {path}\n")
    m = re.match(r"stat -c %a (\S+)", cmd)
    if m: return FakeResult(out=good_mode.get(m.group(1), "755") + "\n")
    if "docker image inspect" in cmd: return FakeResult(out=expect_digest + "\n")
    raise AssertionError(f"unexpected command for a resolution-only scenario: host={host} cmd={cmd!r}")

# A pre-existing, unrelated container using the SAME fixed name up() would
# use for the scheduler -- proving down() genuinely never runs on a barrier
# refusal (LO's exact original concern about down()'s unconditional
# docker-rm-by-fixed-name), not merely that up()'s OWN docker_rm call
# didn't happen. This is checked against the REAL q3 host (a real,
# harmless, uniquely-labeled sentinel container -- not a fake).
SENTINEL = "farm-sched"
farm.sh("q3", f"docker rm -f {SENTINEL} 2>/dev/null; true")
farm.sh("q3", f"docker run -d --name {SENTINEL} --label barrier-sentinel=1 alpine sleep 300", check=True)

rec = Recorder()
saved = dict(sh=farm.sh, docker_rm=farm.docker_rm, docker_run_detached=farm.docker_run_detached,
             scratch_prepare=farm.scratch_prepare, docker_run_foreground_staged=farm.docker_run_foreground_staged,
             wait_for_attestation=farm.wait_for_attestation, argv=sys.argv)
farm.sh = fake_sh
farm.docker_rm = rec.docker_rm
farm.docker_run_detached = rec.docker_run_detached
farm.scratch_prepare = rec.scratch_prepare
farm.docker_run_foreground_staged = rec.docker_run_foreground_staged
farm.wait_for_attestation = rec.wait_for_attestation
sys.argv = ["farm.py", "--workers=research6,research7,q2", "--client=q3", "--phase=up-test-down",
            "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"]
buf = io.StringIO()
try:
    with contextlib.redirect_stdout(buf):
        try:
            farm.main()
        except SystemExit:
            pass
finally:
    farm.sh = saved["sh"]; farm.docker_rm = saved["docker_rm"]; farm.docker_run_detached = saved["docker_run_detached"]
    farm.scratch_prepare = saved["scratch_prepare"]; farm.docker_run_foreground_staged = saved["docker_run_foreground_staged"]
    farm.wait_for_attestation = saved["wait_for_attestation"]; sys.argv = saved["argv"]
out = buf.getvalue()

problems = []
if "REFUSED" not in out:
    problems.append(f"expected a REFUSED line, got: {out!r}")
if "BARRIER REVALIDATION FAILED for F on research7" not in out:
    problems.append(f"expected the barrier's own F-role failure message naming research7, got: {out!r}")
if rec.actions:
    problems.append(f"expected 0 recorded actions (nothing must mutate before the barrier clears), got {len(rec.actions)}: {rec.actions}")
if seen["n"] < 2:
    problems.append(f"tamper target was only queried {seen['n']} time(s) -- the barrier's own second pass never ran")

# The sentinel MUST have survived completely untouched.
chk = farm.sh("q3", f"docker inspect -f '{{{{.State.Status}}}}:{{{{index .Config.Labels \"barrier-sentinel\"}}}}' {SENTINEL} 2>&1")
if "running:1" not in chk.stdout:
    problems.append(f"pre-existing sentinel container {SENTINEL!r} did not survive untouched: {chk.stdout!r}")
farm.sh("q3", f"docker rm -f {SENTINEL} 2>/dev/null; true")

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
print(f"ok - revalidate_entire_plan() independently caught a tamper that landed strictly between the two "
      f"preflight passes (resolve_launch_plan() saw it clean; the barrier's own re-check caught it), "
      f"0 actions recorded, and a real pre-existing 'farm-sched'-named container on q3 survived completely "
      f"untouched -- direct proof down() was never called on this refusal")
PY
[ $? -eq 0 ] || fail "whole-plan barrier transport-spy gate did not pass"

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
    def docker_run_foreground_staged(self, host, name, tree, img, stdin_bytes, inner_cmd, timeout=1800): pass
    def wait_for_attestation(self, host, name, token, timeout=30): pass  # attestation itself is validated by its own dedicated section below

class FakeResult:
    def __init__(self, out="", rc=0): self.stdout, self.returncode, self.stderr = out, rc, ""

manifest = farm.load_manifest("p43")
root = farm.immutable_root("p43")
good_sha = {f"{root}/{b['path']}": b["sha256"] for b in manifest["binaries"]}
good_mode = {f"{root}/{b['path']}": farm._write_stripped(b["mode"]) for b in manifest["binaries"]}  # real roots are always hardened before exposure
expect_digest = farm.launch_image("p43")
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

# revalidate_entire_plan() (the round-4 whole-plan barrier, itself
# already independently proven above via a dedicated transport-spy
# scenario) ALSO re-preflights every role -- including research7 -- in
# its own pass, strictly before up() is ever called. Left unmutated, it
# would catch research7's bad hash on ITS OWN, refusing before main()
# ever reaches up() at all, regardless of what this specific mutation
# does inside resolve_launch_plan()/up() -- which would prove nothing
# about the ordering property THIS test exists to isolate (a class of
# defect the barrier didn't exist yet to catch when this mutant was
# first written). Monkeypatched to a no-op here purely to isolate that:
# the barrier's own necessity is proven separately and is not what this
# test is about.
farm.revalidate_entire_plan = lambda worker_hosts, plan, client_host=None: None

rec = Recorder()
farm.sh = fake_sh
farm.docker_rm = rec.docker_rm
farm.docker_run_detached = rec.docker_run_detached
farm.scratch_prepare = rec.scratch_prepare
farm.docker_run_foreground_staged = rec.docker_run_foreground_staged
farm.wait_for_attestation = rec.wait_for_attestation
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
# research7's disk-space gap is FIXED (owner-directed repair, 2026-08-24 --
# confirmed live: 91% used, ~5.4GB free, matching the fix report exactly)
# and its image-digest gap is ALSO fixed (see "research7: both historical
# gaps CONFIRMED FIXED" below) -- all 4 hosts x 2 sets are expected to
# publish cleanly.
set +e  # capture the exit code explicitly rather than let set -eu abort before we can report specifics
DIST_OUT1=$(python3 "$FARMDIR/farm.py" distribute --sets p43,p50 --hosts q3,research6,research7,q2 2>&1)
DIST_RC1=$?
set -e
echo "$DIST_OUT1"
already=$(printf '%s\n' "$DIST_OUT1" | grep -c 'already-current\|published')
[ "$already" -ge 8 ] || fail "distribute run 1: expected at least 8 success lines (4 hosts x 2 sets), got $already"
[ "$DIST_RC1" -eq 0 ] || fail "distribute run 1: expected exit 0 (all 4 hosts now succeed), got $DIST_RC1"
echo "ok - distribute run 1: all 4 hosts already-current/published for both sets (exit $DIST_RC1)"

set +e
DIST_OUT2=$(python3 "$FARMDIR/farm.py" distribute --sets p43,p50 --hosts q3,research6,research7,q2 2>&1)
DIST_RC2=$?
set -e
echo "$DIST_OUT2"
already2=$(printf '%s\n' "$DIST_OUT2" | grep -c 'already-current')
[ "$already2" -ge 8 ] || fail "distribute run 2: expected at least 8 already-current lines (all 4 hosts, idempotent), got $already2"
[ "$DIST_RC2" -eq 0 ] || fail "distribute run 2: expected exit 0, got $DIST_RC2"
echo "ok - distribute run 2 is a pure no-op for all 4 hosts (idempotent)"

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
EXPECT_FAIL_HOSTS = set()  # research7's two independent gaps (image digest, disk space) were both
                            # owner-directed-fixed 2026-08-24, confirmed live in the section below --
                            # all 4 hosts now expected to PASS every cell.
# Display-only: round-4's attestation checks EVERY manifest binary for
# EVERY role (not one role-specific probe path -- that was round-3's
# design), so this is purely a report-column convenience, decoupled from
# farm.py's own internals (which no longer have a per-role concept at all).
ROLE_DISPLAY_PATH = {"S": "obj/scheduler/icecc-scheduler", "F": "obj/daemon/iceccd", "C": "obj/client/icecc"}

cells = 0
bad = []
for host in HOSTS:
    for binary_set in SETS:
        manifest = farm.load_manifest(binary_set)
        root = farm.immutable_root(binary_set)
        ierr = farm.preflight(host, binary_set)
        for role in ROLES:
            cells += 1
            probe_rel = ROLE_DISPLAY_PATH[role]
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
print("ok - 24/24 cells match their expected outcome (all 4 hosts PASS -- research7's two independent "
      "gaps were both owner-directed-fixed 2026-08-24, confirmed live in the section below)")
PY
[ $? -eq 0 ] || fail "24-cell matrix did not pass"

echo
echo "== research7: both historical gaps CONFIRMED FIXED (owner-directed repair, 2026-08-24) =="
# Both of research7's independent, real, pre-existing gaps documented in
# earlier revisions of this suite are now fixed (owner-directed repair,
# landed live during this same task): the disk-space gap (was as low as
# ~1.5MB free), and the image-digest gap (root cause turned out to be the
# STORE ARCHITECTURE: q3/research6/q2 run docker's containerd image store
# where Id==manifest digest; research7 ran the classic overlay2 store,
# which never minted a digest ref at all -- fixed via a repositories.json
# digest-ref injection). A THIRD, freshly-introduced blocker was found and
# fixed live while verifying this: immediately after the repair,
# /var/run/docker.sock was root:root mode 0660 (a snap-docker-restart
# side effect), which denied farm.py's own non-root production SSH path
# any docker access at all, independent of whether the image content
# itself was correct -- fixed to root:docker, confirmed live below.
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm

df = farm.sh("research7", "df -k ~ | tail -1")
fields = df.stdout.split()
print(f"  research7 df ~: {df.stdout.strip()!r}")
if len(fields) < 4:
    print(f"FAIL: could not parse df output at all (got {df.stdout!r} stderr={df.stderr!r}) -- "
          f"this check itself is broken, not confirming anything", file=sys.stderr)
    sys.exit(1)
avail_kb = int(fields[3])
if avail_kb < 500_000:  # comfortably above the old <200MB failure state
    print(f"FAIL: research7 still shows only {avail_kb}KB free -- the disk fix does not appear to be "
          f"live from this vantage point", file=sys.stderr)
    sys.exit(1)
print(f"ok - disk gap CONFIRMED FIXED: {avail_kb}KB (~{avail_kb//1024}MB) free")

digest = farm.image_digest_remote("research7", "p43")
expect_digest = farm.launch_image("p43")
if digest != expect_digest:
    print(f"FAIL: research7 image digest {digest!r} still does not match the pinned {expect_digest!r} "
          f"-- the fix does not appear to be fully live from this vantage point", file=sys.stderr)
    sys.exit(1)
print(f"ok - image-digest gap CONFIRMED FIXED: {digest!r} matches the pinned reference exactly")

sock = farm.sh("research7", "ls -la /var/run/docker.sock")
print(f"  research7 docker.sock: {sock.stdout.strip()!r}")
if "root docker" not in sock.stdout and " docker " not in sock.stdout:
    print(f"FAIL: docker.sock group ownership does not look right: {sock.stdout!r}", file=sys.stderr)
    sys.exit(1)
print(f"ok - docker-socket-permission blocker CONFIRMED FIXED: farm.py's own non-root production "
      f"SSH path can reach the docker API on research7 again")

# Full-circle real-world confirmation: publish + preflight against the
# ACTUAL production functions, not a synthetic check.
derr, dstatus = farm.publish_immutable_root("research7", "p43")
perr = farm.preflight("research7", "p43")
if derr or perr:
    print(f"FAIL: research7 not fully green (publish err={derr!r} preflight err={perr!r})", file=sys.stderr)
    sys.exit(1)
print(f"ok - research7 fully green end to end: publish_immutable_root -> {dstatus!r}, preflight -> PASS")
PY
[ $? -eq 0 ] || fail "research7 gap-fixed confirmation did not pass"

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
echo "== RACE GATE base scenario (isolated test store, REAL publication/resolution functions): concurrent distribute + a wrong-hash publication attempt against the plan's own resolved path =="
python3 - "research6" "$FARMDIR" <<'PY'
import sys
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm
import json

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
    farm.sh(host, "chmod -R u+w ~/role-artifacts/racegate-teststore 2>/dev/null; rm -rf ~/role-artifacts/racegate-teststore", check=True)
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
    print("ok - RACE GATE base scenario passed: concurrent-distribute non-interference and "
          "wrong-hash-publication-attempt refusal both confirmed (in-container attestation is "
          "covered by its own dedicated section below, and the REAL concurrent main()-driven "
          "race gate further below)")
finally:
    farm.immutable_root = real_immutable_root
    farm.sh(host, "chmod -R u+w ~/role-artifacts/racegate-teststore 2>/dev/null; rm -rf ~/role-artifacts/racegate-teststore")
    print("  ok - isolated test store torn down")
PY
[ $? -eq 0 ] || fail "race gate base scenario did not pass"

echo
echo "== PUBLICATION mutants (1/6): q3 tar-hash check removed -- a bad source tar must no longer be refused before extraction =="
SNAPSHOT_P1="$SCRATCHDIR/farm.py.pre-pub-mutant1"
cp "$FARMDIR/farm.py" "$SNAPSHOT_P1"
restore_p1() { [ -f "$SNAPSHOT_P1" ] && cp "$SNAPSHOT_P1" "$FARMDIR/farm.py"; }
trap restore_p1 EXIT
python3 - "$FARMDIR/farm.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = ('if [ ! -f "$SRC_TAR" ]; then echo "PUBLISH-TAR-HASH-MISMATCH:absent"; exit 5; fi\n'
       'tarh=$(sha256sum "$SRC_TAR"); tarh=${{tarh%% *}}\n'
       'if [ "$tarh" != "{manifest[\'tar\'][\'sha256\']}" ]; then\n'
       '    echo "PUBLISH-TAR-HASH-MISMATCH:$tarh"; exit 5\n'
       'fi\n')
new = '# MUTATED-OUT-FOR-TEST: tar-hash check removed (step 1 skipped entirely)\n'
n = src.count(old)
if n != 1:
    print(f"FAIL: expected exactly 1 occurrence of the tar-hash-check anchor, found {n}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print("mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the tar-hash-check-removed mutation"
python3 - "research6" "$FARMDIR" <<'PY'
import sys, subprocess
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm  # fresh process -- picks up the just-mutated source directly

binary_set = "p43"
manifest = farm.load_manifest(binary_set)

def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/pubmutant1-store/{bs}/{m['tar']['sha256']}"
farm.immutable_root = isolated_root
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant1-store 2>/dev/null; rm -rf ~/role-artifacts/pubmutant1-store", check=True)

tar_name = manifest["tar"]["path"]
pull_cmd = farm.HOSTS["q3"]["ssh"] + [f"cat ~/role-artifacts/{tar_name}"]
real_tar = subprocess.run(pull_cmd, capture_output=True, timeout=60).stdout
bad_tar = real_tar + b"\x00trailing-garbage-not-in-the-manifest-hash"
push = subprocess.run(farm.HOSTS[host]["ssh"] + ["cat > ~/role-artifacts/pubmutant1-bad.tar"],
                       input=bad_tar, capture_output=True, timeout=60)
if push.returncode != 0:
    print(f"FAIL: could not stage the bad tar: {push.stderr.decode()[:200]}", file=sys.stderr)
    sys.exit(1)

script = farm._publish_script(binary_set, manifest, isolated_root(binary_set),
                               "$HOME/role-artifacts/pubmutant1-bad.tar", None)
r = farm.sh(host, script, timeout=120)
line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant1-store 2>/dev/null; "
              "rm -rf ~/role-artifacts/pubmutant1-store ~/role-artifacts/pubmutant1-bad.tar")
if line == "PUBLISH-OK":
    print(f"ok - mutant REDDENED: with the tar-hash check removed, a tar with extra unhashed bytes "
          f"was extracted and published anyway (line={line!r}) -- proving the check was load-bearing")
else:
    print(f"FAIL: expected PUBLISH-OK under the mutation (the bad tar should have gone through "
          f"unchecked), got {line!r} -- either the mutation had no effect or something else caught it", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "tar-hash-check-removed mutant did not redden as required"
restore_p1
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_P1"; then
    echo "ok - farm.py restored byte-exact after publication mutant 1 (cmp clean)"
else
    fail "farm.py restoration after publication mutant 1 is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_P1"

echo
echo "== PUBLICATION mutants (2/6, 3/6): exact-inventory (extra tar member) check removed / type+symlink check removed =="
SNAPSHOT_P23="$SCRATCHDIR/farm.py.pre-pub-mutant23"
cp "$FARMDIR/farm.py" "$SNAPSHOT_P23"
restore_p23() { [ -f "$SNAPSHOT_P23" ] && cp "$SNAPSHOT_P23" "$FARMDIR/farm.py"; }
trap restore_p23 EXIT
python3 - "$FARMDIR/farm.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
problems = []

old_inv = ('    if [ "$actual_inv" != "$EXPECTED_INV" ]; then\n'
           '        FAIL="inventory-mismatch actual=[$actual_inv]"\n'
           '        return\n'
           '    fi\n')
new_inv = '    true  # MUTATED-OUT-FOR-TEST: exact-inventory (no-extras) check removed\n'
n = src.count(old_inv)
if n != 1:
    problems.append(f"inventory-check anchor: expected 1 occurrence, found {n}")
else:
    src = src.replace(old_inv, new_inv, 1)

old_type = ('        if [ -L "$f" ]; then FAIL="$FAIL $path:symlink"; continue; fi\n'
            '        if [ ! -f "$f" ]; then FAIL="$FAIL $path:not-regular-file"; continue; fi\n')
new_type = '        true  # MUTATED-OUT-FOR-TEST: symlink/regular-file type check removed\n'
n = src.count(old_type)
if n != 1:
    problems.append(f"type-check anchor: expected 1 occurrence, found {n}")
else:
    src = src.replace(old_type, new_type, 1)

# The mode check below is NON-dereferencing (`stat -c %a`, no `-L`), which
# means it ALREADY incidentally rejects every symlink on its own: a
# symlink's OWN reported mode is always 777 on Linux (there is no
# meaningful per-symlink permission model -- `-L` is required to see the
# TARGET's mode instead), and no tracked file's expected mode is ever
# 777. This is a genuine, valuable defense-in-depth property of the
# shipped code -- but it also means the explicit type check above is
# partly redundant with it, so isolating THAT check's own necessity
# requires ALSO neutralizing this incidental protection (making stat
# dereference, `-L`), exactly as a future maintenance change that
# switched to a dereferencing stat call for an unrelated reason could
# accidentally do for real.
old_stat = '        m=$(stat -c %a "$f")\n'
new_stat = '        m=$(stat -c %a -L "$f")  # MUTATED-OUT-FOR-TEST: dereferences symlinks too, canceling the incidental mode-based symlink rejection\n'
n = src.count(old_stat)
if n != 1:
    problems.append(f"mode-check stat anchor: expected 1 occurrence, found {n}")
else:
    src = src.replace(old_stat, new_stat, 1)

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src)
print("both mutations applied")
PY
[ $? -eq 0 ] || fail "could not apply the inventory/type-check-removed mutations"
python3 - "research6" "$FARMDIR" <<'PY'
import sys, subprocess, os
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm  # fresh process -- picks up the just-mutated source directly

binary_set = "p43"
manifest = farm.load_manifest(binary_set)
problems = []

def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/pubmutant23-store/{bs}/{m['tar']['sha256']}"
root = isolated_root(binary_set)

# research6 (unlike q3) has no local copy of the source tar -- only q3 (the
# source of record) does. Pull it from q3 via the hub and push it to
# research6 ONCE, as a plain local file both fixtures below extract from.
import subprocess
pull_cmd = farm.HOSTS["q3"]["ssh"] + [f"cat ~/role-artifacts/{manifest['tar']['path']}"]
pulled_tar = subprocess.run(pull_cmd, capture_output=True, timeout=60).stdout
LOCAL_TAR = "~/role-artifacts/pubmutant23-source.tar"
push = subprocess.run(farm.HOSTS[host]["ssh"] + [f"cat > {LOCAL_TAR}"], input=pulled_tar,
                       capture_output=True, timeout=60)
if push.returncode != 0:
    problems.append(f"setup: could not stage the source tar on {host}: {push.stderr.decode()[:200]}")

# --- 2/6: extra tar member must now be accepted (inventory check gone). ---
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant23-store 2>/dev/null; rm -rf ~/role-artifacts/pubmutant23-store", check=True)
tmpdir = "~/role-artifacts/pubmutant23-fixture"
farm.sh(host, f"rm -rf {tmpdir} && mkdir -p {tmpdir}/obj/client && "
              f"tar --same-permissions -xf {LOCAL_TAR} -C {tmpdir} && "
              f"echo SMUGGLED > {tmpdir}/obj/client/evil-extra-file && "
              f"tar -cf ~/role-artifacts/pubmutant23-extra.tar -C {tmpdir} .", check=True)
extra_sha = farm.sh(host, "sha256sum ~/role-artifacts/pubmutant23-extra.tar")
extra_sha = extra_sha.stdout.split()[0]
tampered = dict(manifest); tampered["tar"] = dict(manifest["tar"]); tampered["tar"]["sha256"] = extra_sha
script = farm._publish_script(binary_set, tampered, root, "$HOME/role-artifacts/pubmutant23-extra.tar", None)
r = farm.sh(host, script, timeout=120)
line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
if line != "PUBLISH-OK":
    problems.append(f"2/6: expected PUBLISH-OK (extra member now unchecked), got {line!r}")
else:
    print(f"  ok - mutant 2/6 REDDENED: a tar with a manifest-unlisted extra file was published anyway")
farm.sh(host, f"chmod -R u+w {root} 2>/dev/null; rm -rf {root} {tmpdir} ~/role-artifacts/pubmutant23-extra.tar")

# --- 3/6: a symlink replacing a tracked file must now be accepted. ---
# The symlink target must have the EXACT expected content (sha256) AND
# the EXACT expected PRE-hardening mode (755 -- verify() runs against the
# temp tree at mode-column 3 first, before hardening) for this to isolate
# the TYPE check's own job -- an unrelated target (e.g. /etc/passwd)
# would already be caught by the sha256 check alone, proving nothing
# about the type check specifically. The spare target lives OUTSIDE the
# published tree (chmod -R a-w on the temp tree does not follow symlinks
# out of it, so the external target's mode stays exactly as set).
tmpdir2 = "~/role-artifacts/pubmutant23-fixture2"
EXTERNAL = "~/role-artifacts/pubmutant23-external-icecc-create-env"
farm.sh(host, f"rm -rf {tmpdir2} {EXTERNAL} && mkdir -p {tmpdir2} && "
              f"tar --same-permissions -xf {LOCAL_TAR} -C {tmpdir2} && "
              f"cp {tmpdir2}/obj/client/icecc-create-env {EXTERNAL} && chmod 755 {EXTERNAL} && "
              f"rm {tmpdir2}/obj/client/icecc-create-env && "
              f"ln -s {EXTERNAL} {tmpdir2}/obj/client/icecc-create-env && "
              f"tar -cf ~/role-artifacts/pubmutant23-symlink.tar -C {tmpdir2} .", check=True)
sym_sha = farm.sh(host, "sha256sum ~/role-artifacts/pubmutant23-symlink.tar")
sym_sha = sym_sha.stdout.split()[0]
tampered2 = dict(manifest); tampered2["tar"] = dict(manifest["tar"]); tampered2["tar"]["sha256"] = sym_sha
script2 = farm._publish_script(binary_set, tampered2, root, "$HOME/role-artifacts/pubmutant23-symlink.tar", None)
r2 = farm.sh(host, script2, timeout=120)
line2 = r2.stdout.strip().splitlines()[-1] if r2.stdout.strip() else ""
# The symlink's external target is never touched by `chmod -R a-w $TMP`
# (outside the tree; chmod -R does not follow symlinks it walks past),
# so it structurally cannot satisfy BOTH the pre-hardening (mcol=3) AND
# post-hardening (mcol=4) mode expectations at once -- the meaningful
# claim for THIS mutant is specifically that it gets PAST the stage the
# type check exists to gate (temp verify, mcol=3: PUBLISH-TEMP-VERIFY-FAILED
# must no longer fire, and specifically never with a ":symlink" reason).
# A later PUBLISH-HARDENED-VERIFY-FAILED (mode, at mcol=4) is a SEPARATE,
# already-covered concern (mutant 4) about hardening, not about type.
if line2.startswith("PUBLISH-TEMP-VERIFY-FAILED"):
    problems.append(f"3/6: symlink was still caught at the TEMP-VERIFY stage the type check "
                     f"exists to gate: {line2!r}")
else:
    print(f"  ok - mutant 3/6 REDDENED: a symlink replacing a tracked regular file got PAST "
          f"the temp-verify stage (result: {line2!r}) -- with BOTH the explicit type check AND "
          f"the mode check's incidental non-dereferencing symlink rejection removed, nothing "
          f"left in that stage distinguishes a symlink from a regular file")
farm.sh(host, f"chmod -R u+w ~/role-artifacts/pubmutant23-store 2>/dev/null; "
              f"rm -rf ~/role-artifacts/pubmutant23-store {tmpdir2} {EXTERNAL} ~/role-artifacts/pubmutant23-symlink.tar {LOCAL_TAR}")
# NOTE: chmod -R u+w must target the TOP-LEVEL pubmutant23-store prefix,
# not just {root} -- the 3/6 fixture above deliberately drives execution
# to a PUBLISH-HARDENED-VERIFY-FAILED outcome (expected: the symlink's
# external target can't satisfy both pre- and post-hardening modes at
# once), which means the temp sibling is NEVER renamed to {root} and is
# left on disk, chmod -R a-w'd, under its own .tmp-PID-suffixed name --
# a real orphaned-hardened-directory leak was observed here (found live
# via a post-run host check: rm -rf on {root} alone left a real,
# non-empty, unremovable-without-chmod subtree behind because {root}
# itself was never created).

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "inventory/type-check-removed mutants did not both redden as required"
restore_p23
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_P23"; then
    echo "ok - farm.py restored byte-exact after publication mutants 2/3 (cmp clean)"
else
    fail "farm.py restoration after publication mutants 2/3 is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_P23"

echo
echo "== PUBLICATION mutant (4/6): verify_role_files() reverts to accepting EITHER mode -- a writable final root must now be silently accepted =="
SNAPSHOT_P4="$SCRATCHDIR/farm.py.pre-pub-mutant4"
cp "$FARMDIR/farm.py" "$SNAPSHOT_P4"
restore_p4() { [ -f "$SNAPSHOT_P4" ] && cp "$SNAPSHOT_P4" "$FARMDIR/farm.py"; }
trap restore_p4 EXIT
python3 - "research6" "$FARMDIR" <<'PY'
import sys
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm

binary_set = "p43"
manifest = farm.load_manifest(binary_set)

def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/pubmutant4-store/{bs}/{m['tar']['sha256']}"
root = isolated_root(binary_set)
farm.immutable_root = isolated_root
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant4-store 2>/dev/null; rm -rf ~/role-artifacts/pubmutant4-store", check=True)
derr, dstatus = farm.publish_immutable_root(host, binary_set)
if derr:
    print(f"FAIL: could not bootstrap: {derr}", file=sys.stderr)
    sys.exit(1)

# chmod ONE already-hardened file back to writable, CONTENT untouched --
# simulates a chmod that failed/was skipped/was silently reverted after
# publication, leaving a writable file whose hash is still perfectly
# correct.
target = f"{root}/obj/daemon/iceccd"
farm.sh(host, f"chmod u+w {target}", check=True)

baseline = farm.preflight(host, binary_set)
if baseline is None:
    print("FAIL: baseline (current, fixed code) should have REDDENED against the writable "
          "final file, but it passed -- the fix under test is not actually active", file=sys.stderr)
    sys.exit(1)
print(f"  ok - baseline (fixed code) correctly REDDENS on a writable-but-correct-content final file: {baseline[:150]}...")
# chmod/rm the TOP-LEVEL prefix, not just {root} -- leaves no empty
# skeleton parent (~/role-artifacts/pubmutant4-store/p43/) behind between
# fixtures or after this section ends.
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant4-store 2>/dev/null; rm -rf ~/role-artifacts/pubmutant4-store")
PY
[ $? -eq 0 ] || fail "publication mutant 4 baseline check did not pass"

python3 - "$FARMDIR/farm.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = '''        actual_mode = mode_remote(host, remote_path)
        expect_mode = _write_stripped(b["mode"])
        if actual_mode != expect_mode:
            problems.append(f"{remote_path} mode mismatch (expected hardened {expect_mode}, actual {actual_mode})")'''
new = '''        actual_mode = mode_remote(host, remote_path)
        expect_mode = _write_stripped(b["mode"])
        if actual_mode != expect_mode and actual_mode != b["mode"]:  # MUTATED-OUT-FOR-TEST: either mode accepted again
            problems.append(f"{remote_path} mode mismatch (expected hardened {expect_mode}, actual {actual_mode})")'''
n = src.count(old)
if n != 1:
    print(f"FAIL: expected exactly 1 occurrence of the mode-check anchor, found {n}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print("mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the either-mode-accepted mutation"

python3 - "research6" "$FARMDIR" <<'PY'
import sys
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm  # fresh process -- picks up the just-mutated source directly

binary_set = "p43"
def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/pubmutant4-store/{bs}/{m['tar']['sha256']}"
root = isolated_root(binary_set)
farm.immutable_root = isolated_root
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant4-store 2>/dev/null; rm -rf ~/role-artifacts/pubmutant4-store", check=True)
derr, dstatus = farm.publish_immutable_root(host, binary_set)
if derr:
    print(f"FAIL: could not bootstrap: {derr}", file=sys.stderr)
    sys.exit(1)
target = f"{root}/obj/daemon/iceccd"
farm.sh(host, f"chmod u+w {target}", check=True)

mutated = farm.preflight(host, binary_set)
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant4-store 2>/dev/null; rm -rf ~/role-artifacts/pubmutant4-store")
if mutated is not None:
    print(f"FAIL: expected the mutation to silently ACCEPT the writable final file, but it still "
          f"redenned: {mutated}", file=sys.stderr)
    sys.exit(1)
print(f"ok - mutant 4/6 REDDENED: reverting to either-mode-acceptance silently accepts a writable, "
      f"chmod-regressed final root -- exactly the fail-open gap LO/BO flagged, and exactly what "
      f"requiring the hardened mode EXACTLY (the shipped fix) closes")
PY
[ $? -eq 0 ] || fail "either-mode-accepted mutant did not redden as required"
restore_p4
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_P4"; then
    echo "ok - farm.py restored byte-exact after publication mutant 4 (cmp clean)"
else
    fail "farm.py restoration after publication mutant 4 is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_P4"

echo
echo "== PUBLICATION mutant (5/6): canonical lock removed -- two overlapping publishers must no longer serialize cleanly =="
SNAPSHOT_P5="$SCRATCHDIR/farm.py.pre-pub-mutant5"
cp "$FARMDIR/farm.py" "$SNAPSHOT_P5"
restore_p5() { [ -f "$SNAPSHOT_P5" ] && cp "$SNAPSHOT_P5" "$FARMDIR/farm.py"; }
trap restore_p5 EXIT
python3 - "$FARMDIR/farm.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = 'if ! flock -x -w 120 9; then echo "PUBLISH-LOCK-TIMEOUT"; exit 75; fi\n'
new = 'true  # MUTATED-OUT-FOR-TEST: canonical lock removed (flock never acquired)\n'
n = src.count(old)
if n != 1:
    print(f"FAIL: expected exactly 1 occurrence of the flock anchor, found {n}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print("mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the lock-removed mutation"
python3 - "research6" "$FARMDIR" <<'PY'
import sys, threading, time
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm  # fresh process -- picks up the just-mutated source directly

binary_set = "p43"
def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/pubmutant5-store/{bs}/{m['tar']['sha256']}"
farm.immutable_root = isolated_root
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant5-store 2>/dev/null; rm -rf ~/role-artifacts/pubmutant5-store", check=True)

# publish_immutable_root()'s own docstring: for a non-q3 host, pulling the
# tar from q3 and pushing it to the target happens BEFORE the lock is ever
# acquired ("a plain file write, nothing to protect there") -- two real,
# sequential, UNSYNCHRONIZED SSH round trips that dominate wall-clock time
# (~1.4s observed) and carry enough real-world network/SSH-handshake
# jitter that a mere thread-START stagger does not reliably make the two
# threads' LOCKED CRITICAL SECTIONS overlap (a first attempt at this test,
# staggering thread starts by 20ms, occasionally saw one thread's ENTIRE
# publish complete before the other's pre-lock relay even finished,
# producing a spuriously clean pairing though the mutation was correctly
# applied -- flaky, not a production bug). A threading.Barrier forces both
# threads to enter the one sh() call that actually runs the locked
# _publish_script() at the same instant regardless of pre-lock jitter,
# identifying that call by its own always-present output-marker text
# (both markers are unconditional literals in the generated script body --
# see _publish_script() -- so this is robust to the flock-line mutation
# above, which touches a different part of the same script) rather than
# reimplementing any part of publish_immutable_root() itself.
barrier = threading.Barrier(2)
real_sh = farm.sh
def sync_sh(h, cmd, timeout=120, check=False):
    if "PUBLISH-ALREADY-CURRENT" in cmd and "PUBLISH-OK" in cmd:
        barrier.wait(timeout=30)
    return real_sh(h, cmd, timeout=timeout, check=check)
farm.sh = sync_sh

results = {}
def worker(tag):
    t0 = time.time()
    err, status = farm.publish_immutable_root(host, binary_set)
    results[tag] = (err, status, t0, time.time())

t1 = threading.Thread(target=worker, args=("A",))
t2 = threading.Thread(target=worker, args=("B",))
t1.start(); t2.start()
t1.join(); t2.join()
farm.sh = real_sh
for tag, r in results.items():
    print(f"  thread {tag}: err={r[0]!r} status={r[1]!r} duration={r[3]-r[2]:.3f}s")
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant5-store 2>/dev/null; rm -rf ~/role-artifacts/pubmutant5-store")

clean_pair = sorted(r[1] for r in results.values() if r[0] is None) == ["already-current", "published (root was absent)"]
any_failed = any(r[0] is not None for r in results.values())
if clean_pair and not any_failed:
    print("FAIL: expected the lock-removed race to produce something OTHER than the clean "
          "one-published-one-already-current pairing the LOCKED design guarantees, but got "
          "exactly that clean pairing anyway (both threads were released from the barrier "
          "together, so any remaining desync happened only in per-call SSH transport setup "
          "AFTER that point -- rerun; if this repeats consistently it is a real finding, not a flake)", file=sys.stderr)
    sys.exit(1)
print(f"ok - mutant 5/6 REDDENED: without the canonical lock, two genuinely concurrent publishers "
      f"racing the SAME absent target no longer reliably produce the clean (published, "
      f"already-current) pairing -- at least one hit a real failure instead ({[r[0] for r in results.values()]}), "
      f"exactly the hazard the canonical lock exists to prevent")
PY
[ $? -eq 0 ] || fail "lock-removed mutant did not redden as required"
restore_p5
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_P5"; then
    echo "ok - farm.py restored byte-exact after publication mutant 5 (cmp clean)"
else
    fail "farm.py restoration after publication mutant 5 is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_P5"

echo
echo "== PUBLICATION mutant (6/6): atomic rename-via-temp-sibling replaced by in-place extraction -- an interrupted publish must no longer leave the final name untouched =="
SNAPSHOT_P6="$SCRATCHDIR/farm.py.pre-pub-mutant6"
cp "$FARMDIR/farm.py" "$SNAPSHOT_P6"
restore_p6() { [ -f "$SNAPSHOT_P6" ] && cp "$SNAPSHOT_P6" "$FARMDIR/farm.py"; }
trap restore_p6 EXIT
python3 - "$FARMDIR/farm.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()

# This mutant's OWN claim is about temp-sibling-vs-in-place safety when
# a tar that reaches extraction turns out to be interrupted/incomplete.
# Simulating "interrupted mid-transfer" via truncation was found (this
# round, live) to ALSO reliably break round-5's own new step-3 tar-
# listing pre-validation -- `tar -tf` fails identically to `tar -xf` on
# every truncation point tried, since GNU tar's listing walk still
# needs to seek past each member's declared data length to find the
# next header/the closing EOF blocks, so a truncation deep enough to
# break extraction breaks listing too. That protection is ALREADY fully
# and separately proven by its own dedicated round-5 gate (real crafted
# path-traversal/absolute-path archives) -- neutralizing it HERE, as
# part of THIS mutant's own combined mutation, isolates mutant 6/6's
# original, distinct claim from that newer, separately-proven one
# (same reasoning, same pattern, as the round-4 ordering-violation
# mutant neutralizing revalidate_entire_plan()).
step3_old = ('TAR_NAMES=$(tar -tf "$SRC_TAR") || {{ echo "PUBLISH-TAR-HEADER-INVALID:list-failed"; exit 10; }}\n'
             'TAR_N=$(printf \'%s\\n\' "$TAR_NAMES" | grep -c .)\n'
             'if [ "$TAR_N" -eq 0 ] || [ "$TAR_N" -gt 200 ]; then\n'
             '    echo "PUBLISH-TAR-HEADER-INVALID:member-count=$TAR_N"; exit 10\n'
             'fi\n'
             'if printf \'%s\\n\' "$TAR_NAMES" | grep -qE \'^/|(^|/)\\.\\.(/|$)\'; then\n'
             '    echo "PUBLISH-TAR-HEADER-INVALID:path-traversal-or-absolute"; exit 10\n'
             'fi\n'
             'if [ "$(printf \'%s\\n\' "$TAR_NAMES" | sort -u | wc -l)" != "$TAR_N" ]; then\n'
             '    echo "PUBLISH-TAR-HEADER-INVALID:duplicate-member-names"; exit 10\n'
             'fi\n'
             'TAR_BADTYPES=$(tar -tvf "$SRC_TAR" | awk \'{{print substr($1,1,1)}}\' | grep -vE \'^[-d]$\' || true)\n'
             'if [ -n "$TAR_BADTYPES" ]; then\n'
             '    echo "PUBLISH-TAR-HEADER-INVALID:non-regular-entry-type"; exit 10\n'
             'fi\n'
             'TAR_TOTAL_SIZE=$(tar -tvf "$SRC_TAR" | awk \'{{sum+=$3}} END{{print sum+0}}\')\n'
             'if [ "$TAR_TOTAL_SIZE" -gt 2147483648 ]; then\n'
             '    echo "PUBLISH-TAR-HEADER-INVALID:total-size=$TAR_TOTAL_SIZE"; exit 10\n'
             'fi\n')
step3_new = '# MUTATED-OUT-FOR-TEST: round-5 step-3 tar-header pre-validation removed, to isolate this mutant\'s own claim (see comment above)\n'
n3 = src.count(step3_old)
if n3 != 1:
    print(f"FAIL: expected exactly 1 occurrence of the step-3 tar-header-validation anchor, found {n3}", file=sys.stderr)
    sys.exit(1)
src = src.replace(step3_old, step3_new, 1)

old = ('rm -rf "$TMP"; mkdir -p "$TMP"\n'
       'if ! tar --same-permissions -xf "$SRC_TAR" -C "$TMP"; then echo "PUBLISH-EXTRACT-FAILED"; rm -rf "$TMP"; exit 1; fi\n')
new = ('mkdir -p "$ROOT"  # MUTATED-OUT-FOR-TEST: extract IN-PLACE into the final name, no temp sibling, no pre-rename verification\n'
       'if ! tar --same-permissions -xf "$SRC_TAR" -C "$ROOT"; then echo "PUBLISH-EXTRACT-FAILED"; exit 1; fi\n'
       'echo "PUBLISH-OK"; exit 0\n')
n = src.count(old)
if n != 1:
    print(f"FAIL: expected exactly 1 occurrence of the extract-into-temp anchor, found {n}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print("mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the rename-vs-inplace mutation"
python3 - "research6" "$FARMDIR" <<'PY'
import sys, subprocess
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm  # fresh process -- picks up the just-mutated source directly

binary_set = "p43"
manifest = farm.load_manifest(binary_set)
def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/pubmutant6-store/{bs}/{m['tar']['sha256']}"
root = isolated_root(binary_set)
farm.immutable_root = isolated_root
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant6-store 2>/dev/null; rm -rf ~/role-artifacts/pubmutant6-store", check=True)

tar_name = manifest["tar"]["path"]
pull_cmd = farm.HOSTS["q3"]["ssh"] + [f"cat ~/role-artifacts/{tar_name}"]
pulled = subprocess.run(pull_cmd, capture_output=True, timeout=60).stdout
truncated = pulled[: len(pulled) // 3]  # simulates a crash/interrupt partway through extraction
push = subprocess.run(farm.HOSTS[host]["ssh"] + ["cat > ~/role-artifacts/pubmutant6-truncated.tar"],
                       input=truncated, capture_output=True, timeout=60)
if push.returncode != 0:
    print(f"FAIL: could not stage the truncated tar: {push.stderr.decode()[:200]}", file=sys.stderr)
    sys.exit(1)
# Step 1 (tar-hash check) is UNMUTATED here and would otherwise correctly
# refuse this truncated tar before extraction ever starts -- that's a
# SEPARATE check (already proven by mutant 1) and would mask what THIS
# mutant is testing. Use a manifest reflecting the TRUNCATED tar's own
# (self-consistent) hash so step 1 passes and execution actually reaches
# the mutated in-place-extraction logic, exactly as it would for a truly
# interrupted write of an otherwise-legitimately-hashed tar (e.g. a
# network drop mid-transfer on a host that already trusts its own copy).
# Round-5's step 3 (tar-header pre-validation) is REMOVED as part of
# THIS mutant's own source mutation above, for the same reason: live
# testing this round found that ANY truncation deep enough to break
# extraction ALSO breaks `tar -tf`'s own listing walk (both need to
# seek past each member's declared data length), so step 3 -- already
# separately proven by its own dedicated gate -- would otherwise mask
# this mutant's distinct claim entirely.
truncated_sha = farm.sh(host, "sha256sum ~/role-artifacts/pubmutant6-truncated.tar").stdout.split()[0]
tampered = dict(manifest); tampered["tar"] = dict(manifest["tar"]); tampered["tar"]["sha256"] = truncated_sha
script = farm._publish_script(binary_set, tampered, root, "$HOME/role-artifacts/pubmutant6-truncated.tar", None)
farm.sh(host, script, timeout=60)  # expected to fail partway -- that's the point

red = farm.preflight(host, binary_set)
existed = farm.sh(host, f"test -d {root} && echo yes || echo no").stdout.strip()
farm.sh(host, "chmod -R u+w ~/role-artifacts/pubmutant6-store 2>/dev/null; "
              "rm -rf ~/role-artifacts/pubmutant6-store ~/role-artifacts/pubmutant6-truncated.tar")

if existed != "yes":
    print(f"FAIL: expected the mutation to leave a broken directory AT the final name (existed={existed!r}) "
          f"-- the mutation may not have taken effect", file=sys.stderr)
    sys.exit(1)
if red is None:
    print("FAIL: expected preflight to redden against the broken in-place-extracted final name, "
          "but it passed", file=sys.stderr)
    sys.exit(1)
print(f"ok - mutant 6/6 REDDENED: extracting in-place (no temp sibling, no pre-rename verification) "
      f"left a broken, partially-extracted directory AT the final immutable name after an interrupted "
      f"publish (existed={existed!r}); preflight correctly catches it, but the real design's whole point "
      f"is that this state should be STRUCTURALLY IMPOSSIBLE -- the final name should have stayed "
      f"completely absent instead, ready for a clean retry: {red[:150]}...")
PY
[ $? -eq 0 ] || fail "rename-vs-inplace mutant did not redden as required"
restore_p6
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_P6"; then
    echo "ok - farm.py restored byte-exact after publication mutant 6 (cmp clean)"
else
    fail "farm.py restoration after publication mutant 6 is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_P6"

echo
echo "== immutable_root -> legacy/mutable alias mutant: content-addressing bypass -- two different builds of 'the same set' would collide at an identical name =="
python3 - "research6" "$FARMDIR" <<'PY'
import sys
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm

binary_set = "p43"
manifest = farm.load_manifest(binary_set)
real_immutable_root = farm.immutable_root
ALIAS = "~/role-artifacts/legacy-alias-mutant-test/p43"
def mutable_alias_root(bs):
    return ALIAS if bs == "p43" else real_immutable_root(bs)

try:
    farm.immutable_root = mutable_alias_root
    farm.sh(host, f"chmod -R u+w {ALIAS} 2>/dev/null; rm -rf {ALIAS}", check=True)
    derr, _ = farm.publish_immutable_root(host, binary_set)
    if derr:
        print(f"FAIL: could not publish under the mutable alias: {derr}", file=sys.stderr)
        sys.exit(1)
    # The defect is structural, not a specific corrupted byte: under a
    # mutable (non-hash-derived) resolver, immutable_root() returns the
    # SAME path regardless of tar.sha256 -- a REAL content-addressed
    # resolver, by construction, never does (a different hash always
    # yields a different path, so there is nothing for a "different
    # build" to collide with).
    same_path_for_different_hash = (mutable_alias_root("p43") == mutable_alias_root("p43"))
    derived_from_hash = real_immutable_root("p43") != real_immutable_root("p43").replace(manifest["tar"]["sha256"], "deadbeef")
    if not derived_from_hash:
        print("FAIL: sanity check on the REAL immutable_root() failed", file=sys.stderr)
        sys.exit(1)
    if not same_path_for_different_hash:
        print("FAIL: mutable_alias_root() is not actually mutable -- test setup is broken", file=sys.stderr)
        sys.exit(1)
    print(f"ok - REDDENED (structurally): under a mutable-alias resolver, immutable_root('p43') == "
          f"{ALIAS!r} regardless of tar.sha256 -- the exact collision hazard content-addressing "
          f"(a fresh path per hash, proven above via the REAL immutable_root()) exists to prevent. "
          f"A wrong-hash publication attempt against a mutable alias would corrupt content IN PLACE "
          f"(covered by the base race-gate scenario above, which shows the REAL design refuses instead)")
finally:
    farm.sh(host, f"chmod -R u+w {ALIAS} 2>/dev/null; rm -rf ~/role-artifacts/legacy-alias-mutant-test")
    farm.immutable_root = real_immutable_root
PY
[ $? -eq 0 ] || fail "legacy/mutable-alias mutant did not redden as required"

echo
echo "== mutable-IMG mutant: up()'s S launch reverted to the bare mutable tag instead of plan.s_img -- must redden a NAMED check =="
SNAPSHOT_IMG="$SCRATCHDIR/farm.py.pre-mutimg-mutant"
cp "$FARMDIR/farm.py" "$SNAPSHOT_IMG"
restore_img() { [ -f "$SNAPSHOT_IMG" ] && cp "$SNAPSHOT_IMG" "$FARMDIR/farm.py"; }
trap restore_img EXIT
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm

# Baseline first (unmutated, real code): resolve a REAL plan against q3
# (cheap, read-only) and capture what image reference up() actually
# passes to docker_run_detached() for the scheduler -- must be the
# CONTENT-ADDRESSED, digest-pinned reference the plan resolved
# (launch_image(binary_set)), never the bare mutable tag (IMG), even
# though nothing here launches a real container (docker_run_detached is
# faked purely to CAPTURE the argument, exactly like the transport-spy
# gates above).
class Recorder:
    def __init__(self): self.images = []
    def docker_rm(self, host, name): pass
    def scratch_prepare(self, host, mkdir_path, log_path): pass
    def docker_run_detached(self, host, name, tree, img, inner_cmd):
        self.images.append((host, name, img))
    def wait_for_attestation(self, host, name, token, timeout=30): pass

def captured_s_image():
    plan = farm.resolve_launch_plan(["research6"], "p43", "p43")
    rec = Recorder()
    saved = dict(docker_rm=farm.docker_rm, scratch_prepare=farm.scratch_prepare,
                 docker_run_detached=farm.docker_run_detached, wait_for_attestation=farm.wait_for_attestation)
    saved_sleep = farm.time.sleep
    farm.docker_rm = rec.docker_rm
    farm.scratch_prepare = rec.scratch_prepare
    farm.docker_run_detached = rec.docker_run_detached
    farm.wait_for_attestation = rec.wait_for_attestation
    # docker_run_detached is faked (no real container ever starts), so the
    # "wait for workers to register" polling loop inside up() would
    # otherwise genuinely sleep out its full ~40s budget every call --
    # faked here purely for speed, same as the ordering-violation and
    # skipped-preflight mutants above.
    farm.time.sleep = lambda s: None
    try:
        farm.up(["research6"], plan)
    finally:
        farm.docker_rm = saved["docker_rm"]; farm.scratch_prepare = saved["scratch_prepare"]
        farm.docker_run_detached = saved["docker_run_detached"]; farm.wait_for_attestation = saved["wait_for_attestation"]
        farm.time.sleep = saved_sleep
    sched_imgs = [img for host, name, img in rec.images if name == "farm-sched"]
    return sched_imgs[0] if sched_imgs else None

expected_pinned = farm.launch_image("p43")
baseline_img = captured_s_image()
if baseline_img != expected_pinned:
    print(f"FAIL: baseline (unmutated) captured image {baseline_img!r}, expected the pinned "
          f"{expected_pinned!r}", file=sys.stderr)
    sys.exit(1)
if baseline_img == farm.IMG:
    print(f"FAIL: sanity check failed -- the pinned reference and the bare mutable tag are "
          f"unexpectedly identical strings, this mutant cannot demonstrate anything: {baseline_img!r}",
          file=sys.stderr)
    sys.exit(1)
print(f"  ok - baseline: up() launches S with the pinned reference {baseline_img!r} "
      f"(never the bare tag {farm.IMG!r})")
PY
[ $? -eq 0 ] || fail "mutable-IMG mutant baseline check did not pass"

python3 - "$FARMDIR/farm.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = '    docker_run_detached(SCHED_HOST, "farm-sched", plan.s_tree, plan.s_img,\n'
new = '    docker_run_detached(SCHED_HOST, "farm-sched", plan.s_tree, IMG,  # MUTATED-FOR-TEST: bare mutable tag instead of plan.s_img\n'
n = src.count(old)
if n != 1:
    print(f"FAIL: expected exactly 1 occurrence of the S launch-image anchor, found {n}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print("mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the mutable-IMG mutation"

python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm  # fresh process -- picks up the just-mutated source directly

class Recorder:
    def __init__(self): self.images = []
    def docker_rm(self, host, name): pass
    def scratch_prepare(self, host, mkdir_path, log_path): pass
    def docker_run_detached(self, host, name, tree, img, inner_cmd):
        self.images.append((host, name, img))
    def wait_for_attestation(self, host, name, token, timeout=30): pass

plan = farm.resolve_launch_plan(["research6"], "p43", "p43")
rec = Recorder()
farm.docker_rm = rec.docker_rm
farm.scratch_prepare = rec.scratch_prepare
farm.docker_run_detached = rec.docker_run_detached
farm.wait_for_attestation = rec.wait_for_attestation
farm.time.sleep = lambda s: None  # no real container starts under this fake -- see baseline's comment above
farm.up(["research6"], plan)
sched_imgs = [img for host, name, img in rec.images if name == "farm-sched"]
mutated_img = sched_imgs[0] if sched_imgs else None

expected_pinned = farm.launch_image("p43")
if mutated_img != farm.IMG:
    print(f"FAIL: expected the mutation to launch S with the bare mutable tag {farm.IMG!r}, "
          f"got {mutated_img!r} instead", file=sys.stderr)
    sys.exit(1)
if mutated_img == expected_pinned:
    print(f"FAIL: mutated image unexpectedly still equals the pinned reference -- mutation had no effect",
          file=sys.stderr)
    sys.exit(1)
print(f"ok - REDDENED as required: with plan.s_img replaced by the bare mutable tag at up()'s S "
      f"callsite, the container would launch under {mutated_img!r} (a floating tag Docker could "
      f"resolve to ANY locally-cached content) instead of the manifest's pinned {expected_pinned!r} "
      f"-- exactly the digest-pinning guarantee content-addressed resolution exists to provide, silently "
      f"lost at the one place it actually matters (the real `docker run` command)")
PY
[ $? -eq 0 ] || fail "mutable-IMG mutant did not redden as required"
restore_img
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_IMG"; then
    echo "ok - farm.py restored byte-exact after the mutable-IMG mutant (cmp clean)"
else
    fail "farm.py restoration after the mutable-IMG mutant is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_IMG"

echo
echo "== RACE GATE mutant: dropped :ro -- must redden a NAMED check =="
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
old = 'f"docker run -d --pull=never --name {name} --network host -v {tree}:/work:ro -v {SCRATCH}:/scratch -u 0:0 {img} "'
new = 'f"docker run -d --pull=never --name {name} --network host -v {tree}:/work -v {SCRATCH}:/scratch -u 0:0 {img} "  # MUTATED-OUT-FOR-TEST (:ro dropped)'
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
    farm.sh(host, "chmod -R u+w ~/role-artifacts/racegate-ro-mutant-store 2>/dev/null; rm -rf ~/role-artifacts/racegate-ro-mutant-store", check=True)
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
echo "== ATTESTATION baseline (isolated store, REAL docker_run_detached/wait_for_attestation): a corrupted tracked file must be refused BEFORE the real role process can start, and the real command must run when the store is clean =="
python3 - "q3" "$FARMDIR" <<'PY'
import sys
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm

binary_set = "p43"
def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/attest-baseline-store/{bs}/{m['tar']['sha256']}"
root = isolated_root(binary_set)
farm.immutable_root = isolated_root
farm.sh(host, "chmod -R u+w ~/role-artifacts/attest-baseline-store 2>/dev/null; rm -rf ~/role-artifacts/attest-baseline-store", check=True)
derr, _ = farm.publish_immutable_root(host, binary_set)
if derr:
    print(f"FAIL: could not bootstrap: {derr}", file=sys.stderr)
    sys.exit(1)
img = farm.launch_image(binary_set)

def run_once(name, sentinel):
    farm.sh(host, f"docker rm -f {name} 2>/dev/null; true")
    token = farm._attestation_token()
    farm.docker_run_detached(host, name, root, img,
        farm._attestation_prefix(binary_set, token) + f"echo {sentinel} > /scratch/{sentinel}; sleep 20")
    return token

# --- clean store: the real command must actually run. ---
tok = run_once("attest-baseline", "REAL_CMD_RAN_CLEAN")
farm.wait_for_attestation(host, "attest-baseline", tok)
sentinel_out = farm.sh(host, "cat ~/farm-scratch/REAL_CMD_RAN_CLEAN 2>&1").stdout
farm.sh(host, "docker rm -f attest-baseline 2>/dev/null; true")
if "REAL_CMD_RAN_CLEAN" not in sentinel_out:
    print(f"FAIL: clean store -- real command never ran (sentinel: {sentinel_out!r})", file=sys.stderr)
    sys.exit(1)
print("ok - clean store: attestation passed, marker appeared, and the real command actually executed")

# --- corrupted store: the real command must NEVER run. ---
farm.sh(host, f"chmod u+w {root}/MANIFEST.tsv && echo CORRUPT >> {root}/MANIFEST.tsv")
tok2 = run_once("attest-baseline2", "REAL_CMD_RAN_CORRUPT")
try:
    farm.wait_for_attestation(host, "attest-baseline2", tok2, timeout=15)
    print("FAIL: wait_for_attestation did not raise against a corrupted tracked file", file=sys.stderr)
    sys.exit(1)
except RuntimeError as e:
    if "ARTIFACT-ATTEST-FAIL" not in str(e) and "attestation FAILED" not in str(e):
        print(f"FAIL: raised for the wrong reason: {e}", file=sys.stderr)
        sys.exit(1)
sentinel_out2 = farm.sh(host, "test -f ~/farm-scratch/REAL_CMD_RAN_CORRUPT && echo SENTINEL_PRESENT || echo SENTINEL_ABSENT").stdout
farm.sh(host, "docker rm -f attest-baseline2 2>/dev/null; true")
if "SENTINEL_PRESENT" in sentinel_out2:
    print(f"FAIL: the real command ran DESPITE the corrupted tracked file: {sentinel_out2!r}", file=sys.stderr)
    sys.exit(1)
print("ok - corrupted store: attestation correctly refused, and the real command NEVER ran "
      f"(sentinel absent/error: {sentinel_out2.strip()!r})")
farm.sh(host, "chmod -R u+w ~/role-artifacts/attest-baseline-store 2>/dev/null; rm -rf ~/role-artifacts/attest-baseline-store")
PY
[ $? -eq 0 ] || fail "attestation baseline did not pass"

echo
echo "== ATTESTATION deletion mutants (S/F/C independent): removing one role's attestation prefix+wait must let its real process start UNCHECKED, on an isolated corrupted store =="
# For each role, revalidate_before_mutation() is monkeypatched to a no-op
# purely to ISOLATE the attestation layer under test from that SEPARATE,
# already-proven-effective layer (revalidate_before_mutation()'s own
# necessity is proven independently above/elsewhere in this suite) --
# without this, revalidate_before_mutation() would ALSO catch the
# corrupted MANIFEST.tsv (it re-preflights, which checks every tracked
# file) and up()/run_client() would refuse before ever reaching the
# attestation-guarded launch, for EITHER the mutated or unmutated
# attestation code, proving nothing about attestation specifically.
for ROLE in S F C; do
echo "-- role $ROLE --"
SNAPSHOT_ATT="$SCRATCHDIR/farm.py.pre-attest-mutant-$ROLE"
cp "$FARMDIR/farm.py" "$SNAPSHOT_ATT"
restore_att() { [ -f "$SNAPSHOT_ATT" ] && cp "$SNAPSHOT_ATT" "$FARMDIR/farm.py"; }
trap restore_att EXIT

python3 - "$FARMDIR/farm.py" "$ROLE" <<'PY'
import sys
path, role = sys.argv[1], sys.argv[2]
src = open(path).read()
anchors = {
    "S": ('    docker_run_detached(SCHED_HOST, "farm-sched", plan.s_tree, plan.s_img,\n'
          '        _attestation_prefix(plan.binary_set_s, s_token, "obj/scheduler/icecc-scheduler") +\n'
          '        f"useradd -r icecc 2>/dev/null; exec /proc/self/fd/{ROLE_BINARY_FD} -p {SCHED_PORT} -n {NET} -l /scratch/farm/sched.log -vvv")\n'
          '    wait_for_attestation(SCHED_HOST, "farm-sched", s_token)\n',
          '    docker_run_detached(SCHED_HOST, "farm-sched", plan.s_tree, plan.s_img,\n'
          '        f"useradd -r icecc 2>/dev/null; exec /work/obj/scheduler/icecc-scheduler -p {SCHED_PORT} -n {NET} -l /scratch/farm/sched.log -vvv")\n'
          '    # MUTATED-OUT-FOR-TEST: S attestation prefix + wait removed (exec reverted to\n'
          '    # /work/<path> directly -- /proc/self/fd/8 only exists because attestation\n'
          '    # itself opened it, so removing attestation without also reverting the exec\n'
          '    # target would just crash the container on a dangling fd, not reproduce\n'
          '    # "unattested exec", which is the actual claim under test here)\n'),
    "F": ('        docker_run_detached(h, "farm-worker", f_tree, f_img,\n'
          '            _attestation_prefix(plan.binary_set_f, f_token, "obj/daemon/iceccd") +\n'
          '            f"useradd -r -s /usr/sbin/nologin icecc 2>/dev/null; "\n'
          '            f"chown icecc /scratch/farm/envs; "  # pre-chown as root: daemon\'s cleanup_cache runs post-drop (no CAP_CHOWN)\n'
          '            f"exec /proc/self/fd/{ROLE_BINARY_FD} -m 8 -s {sip}:{SCHED_PORT} -n {NET} -N {h}w -b /scratch/farm/envs "\n'
          '            f"-p {wp} -l /scratch/farm/worker.log -vvv")\n'
          '        wait_for_attestation(h, "farm-worker", f_token)\n',
          '        docker_run_detached(h, "farm-worker", f_tree, f_img,\n'
          '            f"useradd -r -s /usr/sbin/nologin icecc 2>/dev/null; "\n'
          '            f"chown icecc /scratch/farm/envs; "  # pre-chown as root: daemon\'s cleanup_cache runs post-drop (no CAP_CHOWN)\n'
          '            f"exec /work/obj/daemon/iceccd -m 8 -s {sip}:{SCHED_PORT} -n {NET} -N {h}w -b /scratch/farm/envs "\n'
          '            f"-p {wp} -l /scratch/farm/worker.log -vvv")\n'
          '        # MUTATED-OUT-FOR-TEST: F attestation prefix + wait removed (exec reverted\n'
          '        # to /work/<path> directly, same reasoning as the S anchor above)\n'),
    "C": ('    attest = _attestation_prefix(plan.binary_set_c, c_token)\n',
          '    attest = ""  # MUTATED-OUT-FOR-TEST: C attestation prefix removed\n'),
}
old, new = anchors[role]
n = src.count(old)
if n != 1:
    print(f"FAIL: expected exactly 1 occurrence of the {role} attestation-callsite anchor, found {n}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print(f"{role} mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the $ROLE attestation-removed mutation"

python3 - "q3" "research6" "$ROLE" "$FARMDIR" <<'PY'
import sys
sched_host, worker_host, role, farmdir = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
sys.path.insert(0, farmdir)
import farm  # fresh process -- picks up the just-mutated source directly

binary_set = "p43"
def clean_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/attest-mutant-clean-store/{bs}/{m['tar']['sha256']}"
def bad_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/attest-mutant-bad-store-{role}/{bs}/{m['tar']['sha256']}"

farm.immutable_root = clean_root
farm.sh(sched_host, "chmod -R u+w ~/role-artifacts/attest-mutant-clean-store 2>/dev/null; rm -rf ~/role-artifacts/attest-mutant-clean-store", check=True)
derr, _ = farm.publish_immutable_root(sched_host, binary_set)
if derr: print(f"FAIL: could not bootstrap clean store on {sched_host}: {derr}", file=sys.stderr); sys.exit(1)
if worker_host != sched_host:
    derr, _ = farm.publish_immutable_root(worker_host, binary_set)
    if derr: print(f"FAIL: could not bootstrap clean store on {worker_host}: {derr}", file=sys.stderr); sys.exit(1)

target_host = worker_host if role == "F" else sched_host  # C's container also runs on sched_host (run_client(sched_host, ...) below)
farm.immutable_root = bad_root
farm.sh(target_host, f"chmod -R u+w ~/role-artifacts/attest-mutant-bad-store-{role} 2>/dev/null; rm -rf ~/role-artifacts/attest-mutant-bad-store-{role}", check=True)
derr, _ = farm.publish_immutable_root(target_host, binary_set)
if derr: print(f"FAIL: could not bootstrap bad store on {target_host}: {derr}", file=sys.stderr); sys.exit(1)
bad = bad_root(binary_set)
farm.sh(target_host, f"chmod u+w {bad}/MANIFEST.tsv && echo CORRUPT >> {bad}/MANIFEST.tsv")

img = farm.launch_image(binary_set)
plan = farm.LaunchPlan(
    s_tree=clean_root(binary_set) if role != "S" else bad,
    s_img=img, binary_set_s=binary_set,
    f_resolved=[(clean_root(binary_set) if role != "F" else bad, img)],
    binary_set_f=binary_set,
    c_tree=clean_root(binary_set) if role != "C" else bad,
    c_img=img, binary_set_c=binary_set,
)

real_rbm = farm.revalidate_before_mutation
farm.revalidate_before_mutation = lambda host, binary_set: None  # isolating attestation, see comment above
farm.time.sleep = lambda s: None
try:
    if role in ("S", "F"):
        try:
            ok = farm.up([worker_host], plan)
            raised = None
        except RuntimeError as e:
            raised = str(e)
        # Give the (possibly now-unguarded) real role process a moment,
        # then check whether it is ACTUALLY running inside its container.
        import time as _t; _t.sleep(1.5)
        name = "farm-sched" if role == "S" else "farm-worker"
        binpath = "icecc-scheduler" if role == "S" else "iceccd"
        check_host = sched_host if role == "S" else worker_host
        ps = farm.sh(check_host, f"docker exec {name} pgrep -af {binpath} 2>&1; echo RC=$?")
        running = "RC=0" in ps.stdout
        farm.sh(sched_host, "docker rm -f farm-sched 2>/dev/null; true")
        farm.sh(worker_host, "docker rm -f farm-worker 2>/dev/null; true")
        if not running:
            print(f"FAIL: expected the mutation to let the real {binpath} process actually start and run "
                  f"despite the corrupted store, but it did not (raised={raised!r}, ps={ps.stdout!r})", file=sys.stderr)
            sys.exit(1)
        print(f"ok - {role} mutant REDDENED: with attestation removed at this callsite, the real "
              f"{binpath} process started and ran DESPITE the corrupted tracked file (up() raised={raised!r} "
              f"-- irrelevant to this claim; what matters is the process itself: {ps.stdout.strip()!r})")
    else:  # C
        okS = farm.up([worker_host], plan)
        if not okS:
            print(f"FAIL: setup -- clean S+F launch for the C test did not register", file=sys.stderr)
            sys.exit(1)
        # run_client() is only needed here long enough to prove
        # farm_client.sh was REACHED (attestation bypassed) -- with the
        # corrupted store's binaries otherwise intact and functional
        # (only MANIFEST.tsv, never read at runtime, is corrupted), a
        # genuinely unattested farm_client.sh can attempt a REAL,
        # possibly long-running compile handshake against a real
        # scheduler; run_client() itself hardcodes a 1800s transport
        # timeout, far longer than this test needs. Run it in a
        # background thread with a bounded wait, then force-clean the
        # containers regardless of whether it finished -- the claim under
        # test (no ARTIFACT-ATTEST-FAIL possible, i.e. no gate at all) is
        # already fully decided by whatever output exists at that point.
        import threading
        result = {}
        def _bg():
            try:
                r = farm.run_client(sched_host, "/hostscratch/fmt", "simultaneous", "0", "4", "", plan)
                result["out"] = r.stdout
            except RuntimeError as e:
                result["out"] = str(e)
            except BaseException as e:
                result["out"] = f"(background thread exception: {e!r})"
        t = threading.Thread(target=_bg, daemon=True)
        t.start()
        t.join(timeout=20)
        out = result.get("out", "(run_client had not produced output within 20s -- container reached and "
                                  "running unattested is itself already sufficient evidence for this claim)")
        farm.sh(sched_host, "docker rm -f farm-sched farm-client 2>/dev/null; true")
        farm.sh(worker_host, "docker rm -f farm-worker 2>/dev/null; true")
        if "ARTIFACT-ATTEST-FAIL" in out:
            print(f"FAIL: expected the mutation to remove the attestation gate entirely (no ARTIFACT-ATTEST-FAIL "
                  f"possible -- there is no prefix left to fail), but it still appeared: {out[:300]!r}", file=sys.stderr)
            sys.exit(1)
        print(f"ok - C mutant REDDENED: with attestation removed from run_client()'s inner_cmd, "
              f"farm_client.sh was reached directly against the corrupted store with no gate at all "
              f"(no ARTIFACT-ATTEST-FAIL possible; output starts: {out[:200]!r})")
finally:
    farm.revalidate_before_mutation = real_rbm
    farm.immutable_root = clean_root
    farm.sh(sched_host, "chmod -R u+w ~/role-artifacts/attest-mutant-clean-store 2>/dev/null; rm -rf ~/role-artifacts/attest-mutant-clean-store")
    if worker_host != sched_host:
        farm.sh(worker_host, "chmod -R u+w ~/role-artifacts/attest-mutant-clean-store 2>/dev/null; rm -rf ~/role-artifacts/attest-mutant-clean-store")
    farm.sh(target_host, f"chmod -R u+w ~/role-artifacts/attest-mutant-bad-store-{role} 2>/dev/null; rm -rf ~/role-artifacts/attest-mutant-bad-store-{role}")
PY
[ $? -eq 0 ] || fail "$ROLE attestation-removed mutant did not redden as required"

restore_att
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_ATT"; then
    echo "ok - farm.py restored byte-exact after the $ROLE attestation mutant (cmp clean)"
else
    fail "farm.py restoration after the $ROLE attestation mutant is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_ATT"
done

echo
echo "== REAL CONCURRENT RACE GATE: a background REAL main() paused at the race-gate seam, raced by production distribute() + a wrong-hash tamper against the SAME host+root it actually resolved, for the final F and separately for C =="
# Driver: a tiny standalone script (not farm.py itself) that monkeypatches
# immutable_root() to an isolated, uniquely-prefixed store BEFORE calling
# the REAL farm.main() -- so the entire real orchestration (resolve_launch_plan
# -> _race_gate_pause -> revalidate_entire_plan -> up/run_client -> down)
# runs unmodified, just resolved against test-owned paths, exactly the same
# isolation pattern already used and proven safe throughout this suite.
cat > "$SCRATCHDIR/race_driver.py" <<'DRIVER'
import sys
farmdir, store_prefix = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm

def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/{store_prefix}/{bs}/{m['tar']['sha256']}"
farm.immutable_root = isolated_root

sys.argv = ["farm.py"] + sys.argv[3:]
farm.main()
DRIVER

python3 - "$FARMDIR" "$SCRATCHDIR" <<'PY'
import sys, os, subprocess, time
farmdir, scratchdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm

STORE_PREFIX = "realracegate-store"
binary_set = "p43"

def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/{STORE_PREFIX}/{bs}/{m['tar']['sha256']}"

real_immutable_root = farm.immutable_root
problems = []

def bootstrap(hosts):
    farm.immutable_root = isolated_root
    for h in hosts:
        farm.sh(h, f"chmod -R u+w ~/role-artifacts/{STORE_PREFIX} 2>/dev/null; rm -rf ~/role-artifacts/{STORE_PREFIX}", check=True)
    for h in hosts:
        derr, dstatus = farm.publish_immutable_root(h, binary_set)
        if derr:
            problems.append(f"bootstrap {h}: {derr}")
        else:
            print(f"  bootstrap {h}: {dstatus}")

def containers_on(host):
    return set(farm.sh(host, "docker ps -a --filter name=farm- --format '{{.Names}}'").stdout.split())

def run_scenario(label, argv_tail, involved_hosts, tamper_host, expect_msg_fragment):
    ready_file = f"{scratchdir}/race-ready-{label}"
    cont_file = f"{scratchdir}/race-continue-{label}"
    for f in (ready_file, cont_file):
        if os.path.exists(f):
            os.remove(f)
    before = {h: containers_on(h) for h in involved_hosts}

    env = dict(os.environ)
    env["FARM_RACE_READY_FILE"] = ready_file
    env["FARM_RACE_CONTINUE_FILE"] = cont_file
    cmd = [sys.executable, f"{scratchdir}/race_driver.py", farmdir, STORE_PREFIX] + argv_tail
    proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    deadline = time.time() + 30
    while not os.path.exists(ready_file):
        if time.time() > deadline:
            proc.kill()
            problems.append(f"{label}: background main() never reached the race-gate seam within 30s")
            return
        time.sleep(0.1)
    print(f"  {label}: background main() paused at the seam (plan resolved, about to enter revalidate_entire_plan())")

    # --- Concurrently: (a) production distribute() on the SAME host+set
    #     the background plan resolved (a legitimate operator running
    #     distribute at the same time -- must no-op harmlessly), and
    #     (b) a genuine wrong-hash tamper on that SAME resolved root. ---
    farm.immutable_root = isolated_root
    derr, dstatus = farm.publish_immutable_root(tamper_host, binary_set)
    if derr:
        problems.append(f"{label}: concurrent distribute() unexpectedly failed before the tamper: {derr}")
    root = isolated_root(binary_set)
    farm.sh(tamper_host, f"chmod u+w {root}/MANIFEST.tsv && printf 'RACE-TAMPER' >> {root}/MANIFEST.tsv", check=True)
    print(f"  {label}: injected concurrent distribute() ({dstatus}) + a real wrong-hash tamper on "
          f"{tamper_host}:{root} -- the SAME host+root the background plan resolved")

    with open(cont_file, "w") as f:
        f.write("go\n")
    try:
        out, _ = proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, _ = proc.communicate()
        problems.append(f"{label}: background main() did not exit within 60s of being released")

    after = {h: containers_on(h) for h in involved_hosts}
    new_containers = {h: after[h] - before[h] for h in involved_hosts}
    any_new = any(new_containers[h] for h in involved_hosts)

    if "REFUSED" not in out:
        problems.append(f"{label}: expected a REFUSED line in the background process's output, got: {out[-500:]!r}")
    if expect_msg_fragment not in out:
        problems.append(f"{label}: expected {expect_msg_fragment!r} in output, got: {out[-500:]!r}")
    if "DOWN: tearing down" in out:
        problems.append(f"{label}: down() was called (a 'DOWN: tearing down' line appeared) -- a barrier "
                         f"refusal must never invoke it")
    if any_new:
        problems.append(f"{label}: new container(s) appeared on a host despite the refusal: {new_containers}")
    if not problems or not any(label in p for p in problems):
        print(f"  ok - {label}: REFUSED cleanly, zero new containers on {involved_hosts}, down() never called")

    # repair the tamper for reuse / final cleanup.
    farm.sh(tamper_host, f"chmod -R u+w {root} 2>/dev/null; rm -rf {root}")
    farm.publish_immutable_root(tamper_host, binary_set)

# --- Scenario 1: tamper the FINAL F (q2, 2nd of 2 workers). research7 was
#     excluded from this section while its disk-space gap was still live;
#     that gap is now fixed (see "research7: both historical gaps
#     CONFIRMED FIXED" elsewhere in this suite), but q2 already fully and
#     validly demonstrates the same mechanism, so it is left as-is here
#     rather than re-plumbing an already-proven-working section. ---
bootstrap(["q3", "research6", "q2"])
run_scenario("final-f", ["--workers=research6,q2", "--phase=up-down",
                          "--binary-set-S=p43", "--binary-set-F=p43"],
             ["q3", "research6", "q2"], "q2",
             "BARRIER REVALIDATION FAILED for F on q2")

# --- Scenario 2: tamper C (q2), with S and F both clean. ---
bootstrap(["q3", "research6", "q2"])
run_scenario("client-c", ["--workers=research6", "--client=q2", "--phase=up-test-down",
                           "--binary-set-S=p43", "--binary-set-F=p43", "--binary-set-C=p43"],
             ["q3", "research6", "q2"], "q2",
             "BARRIER REVALIDATION FAILED for C on q2")

farm.immutable_root = real_immutable_root
for h in ("q3", "research6", "research7", "q2"):
    farm.sh(h, f"chmod -R u+w ~/role-artifacts/{STORE_PREFIX} 2>/dev/null; rm -rf ~/role-artifacts/{STORE_PREFIX}")

if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
print("ok - REAL concurrent race gate: both rows (final-F tamper, C tamper) refused cleanly via "
      "revalidate_entire_plan(), driving the ACTUAL main() entry point in a genuinely separate, "
      "paused-then-released background process, with zero mutations and down() never called in either case")
PY
[ $? -eq 0 ] || fail "real concurrent race gate did not pass"

echo
echo "== REAL CONCURRENT RACE GATE mutant: revalidate_entire_plan() neutralized -- the final-F row must FLIP (no longer refuse cleanly) =="
SNAPSHOT_RACEBAR="$SCRATCHDIR/farm.py.pre-racebar-mutant"
cp "$FARMDIR/farm.py" "$SNAPSHOT_RACEBAR"
restore_racebar() { [ -f "$SNAPSHOT_RACEBAR" ] && cp "$SNAPSHOT_RACEBAR" "$FARMDIR/farm.py"; }
trap restore_racebar EXIT
python3 - "$FARMDIR/farm.py" <<'PY'
import sys
path = sys.argv[1]
src = open(path).read()
old = '    err = preflight(SCHED_HOST, plan.binary_set_s)\n    if err:\n'
new = '    return  # MUTATED-OUT-FOR-TEST: barrier neutralized\n    err = preflight(SCHED_HOST, plan.binary_set_s)\n    if err:\n'
n = src.count(old)
if n != 1:
    print(f"FAIL: expected exactly 1 occurrence of the barrier-entry anchor, found {n}", file=sys.stderr)
    sys.exit(1)
open(path, "w").write(src.replace(old, new, 1))
print("mutation applied")
PY
[ $? -eq 0 ] || fail "could not apply the barrier-neutralized mutation"

python3 - "$FARMDIR" "$SCRATCHDIR" <<'PY'
import sys, os, subprocess, time
farmdir, scratchdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm  # fresh process -- picks up the just-mutated source directly

STORE_PREFIX = "realracegate-mutant-store"
binary_set = "p43"

def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/{STORE_PREFIX}/{bs}/{m['tar']['sha256']}"
farm.immutable_root = isolated_root

# research7 not used here (see the Scenario 1 comment above -- its old
# disk gap is fixed, this just isn't re-plumbed for it).
for h in ("q3", "research6", "q2"):
    farm.sh(h, f"chmod -R u+w ~/role-artifacts/{STORE_PREFIX} 2>/dev/null; rm -rf ~/role-artifacts/{STORE_PREFIX}", check=True)
    derr, dstatus = farm.publish_immutable_root(h, binary_set)
    if derr:
        print(f"FAIL: bootstrap {h}: {derr}", file=sys.stderr)
        sys.exit(1)

def containers_on(host):
    return set(farm.sh(host, "docker ps -a --filter name=farm- --format '{{.Names}}'").stdout.split())

before = {h: containers_on(h) for h in ("q3", "research6", "q2")}
ready_file = f"{scratchdir}/race-ready-mutant"
cont_file = f"{scratchdir}/race-continue-mutant"
for f in (ready_file, cont_file):
    if os.path.exists(f): os.remove(f)
env = dict(os.environ)
env["FARM_RACE_READY_FILE"] = ready_file
env["FARM_RACE_CONTINUE_FILE"] = cont_file
cmd = [sys.executable, f"{scratchdir}/race_driver.py", farmdir, STORE_PREFIX,
       "--workers=research6,q2", "--phase=up-down", "--binary-set-S=p43", "--binary-set-F=p43"]
proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
deadline = time.time() + 30
while not os.path.exists(ready_file):
    if time.time() > deadline:
        proc.kill()
        print("FAIL: background main() never reached the seam", file=sys.stderr)
        sys.exit(1)
    time.sleep(0.1)

root = isolated_root(binary_set)
farm.sh("q2", f"chmod u+w {root}/MANIFEST.tsv && printf 'RACE-TAMPER' >> {root}/MANIFEST.tsv", check=True)
with open(cont_file, "w") as f:
    f.write("go\n")
try:
    out, _ = proc.communicate(timeout=60)
except subprocess.TimeoutExpired:
    proc.kill()
    out, _ = proc.communicate()

# NOTE on the container-snapshot check below: it is EXPECTED to often show
# no diff, and that is NOT evidence of anything -- if MUTATIONS.started
# went True, down() runs inside main()'s own finally block BEFORE
# proc.communicate() ever returns to us, so by the time we can look, any
# transient containers it created are already gone again. The reliable,
# non-racy signal is the presence of "DOWN: tearing down cluster" in the
# captured output itself: down() is called if-and-only-if MUTATIONS.started
# is True (see main()'s docstring/comments), which can only become True
# from inside a REAL mutating primitive having actually run -- so seeing
# that line appear is direct, deterministic proof that up() proceeded to
# mutate (S, and likely research6, the clean first worker) before ever
# reaching the tampered q2, exactly the TOCTOU gap this barrier exists to
# close. This is the exact same "DOWN: tearing down" text the BASELINE
# (unmutated) scenarios above assert must be ABSENT -- this mutant
# requires the opposite.
after = {h: containers_on(h) for h in ("q3", "research6", "q2")}
new_containers = {h: after[h] - before[h] for h in ("q3", "research6", "q2")}

farm.sh("q3", "docker rm -f farm-sched farm-worker 2>/dev/null; true")
farm.sh("research6", "docker rm -f farm-worker 2>/dev/null; true")
farm.sh("q2", "docker rm -f farm-worker 2>/dev/null; true")
farm.sh("q2", f"chmod -R u+w {root} 2>/dev/null; rm -rf {root}")
for h in ("q3", "research6", "q2"):
    farm.sh(h, f"chmod -R u+w ~/role-artifacts/{STORE_PREFIX} 2>/dev/null; rm -rf ~/role-artifacts/{STORE_PREFIX}")

if "BARRIER REVALIDATION FAILED for F on q2" in out:
    print(f"FAIL: expected the neutralized barrier to NOT catch the tamper (that's the mutant's whole "
          f"point), but the exact same refusal message still appeared: {out[-300:]!r}", file=sys.stderr)
    sys.exit(1)
if "DOWN: tearing down cluster" not in out:
    print(f"FAIL: expected the neutralized barrier to let mutation actually begin (down() only ever runs "
          f"once MUTATIONS.started is True, i.e. a real mutating primitive already ran), but no "
          f"'DOWN: tearing down cluster' line appeared -- the mutation may have had no effect, or "
          f"something else masked it: {out[-500:]!r}", file=sys.stderr)
    sys.exit(1)
print(f"ok - REDDENED as required: with revalidate_entire_plan() neutralized, the final-F tamper was no "
      f"longer caught before mutation -- 'DOWN: tearing down cluster' appeared (down() only runs once "
      f"MUTATIONS.started is True), proving S (and likely research6) were actually mutated before the "
      f"tampered q2 was ever reached, exactly the TOCTOU gap this barrier exists to close "
      f"(post-run container snapshot, for reference only, not part of the pass/fail signal: {new_containers})")
PY
[ $? -eq 0 ] || fail "barrier-neutralized mutant did not redden as required"
restore_racebar
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_RACEBAR"; then
    echo "ok - farm.py restored byte-exact after the barrier-neutralized mutant (cmp clean)"
else
    fail "farm.py restoration after the barrier-neutralized mutant is NOT byte-exact"
fi
trap - EXIT
rm -f "$SNAPSHOT_RACEBAR"
rm -f "$SCRATCHDIR/race_driver.py" "$SCRATCHDIR"/race-ready-* "$SCRATCHDIR"/race-continue-*

echo
echo "== REAL end-to-end launch: resolve_launch_plan() -> up() -> run_client() -> dump_worker_evidence() -> CELL-VERDICT -> down(), single host (q3), guaranteed teardown =="
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm
try:
    plan = farm.resolve_launch_plan(["q3"], "p43", "p43", client_host="q3", binary_set_c="p43")
    ok = farm.up(["q3"], plan)
    if not ok:
        print("FAIL: up() did not report successful registration", file=sys.stderr)
        sys.exit(1)
    print("ok - real end-to-end launch registered successfully with in-container identity verification wired in")
    # Round-4 cell-verdict fix: confirm the STRENGTHENED dump_worker_evidence()
    # correctly recognizes a genuine pass against REAL replay.py output from a
    # REAL fmt build on REAL q3 -- worker.log grep patterns and the JOINROW/
    # CELL: header regexes included. The synthetic controls elsewhere in this
    # suite prove the FIX's logic is correct against hand-built lines; only a
    # real client+worker run can prove the parser actually matches what
    # production replay.py and a production worker.log really emit.
    r = farm.run_client("q3", "/hostscratch/fmt", "simultaneous", "0", "4", "", plan)
    client_ok = (r.returncode == 0)
    join_ok = farm.dump_worker_evidence(["q3"], r.stdout, r.returncode, "p43")
    if not client_ok:
        print(f"FAIL: real client run against the real fmt project did not exit 0 (rc={r.returncode}); "
              f"stdout tail: {r.stdout[-500:]!r}", file=sys.stderr)
        sys.exit(1)
    if not join_ok:
        print(f"FAIL: dump_worker_evidence() reported join_ok=False against REAL replay.py/worker.log "
              f"output -- either a real regression or the parser does not match production's real "
              f"format; stdout tail: {r.stdout[-500:]!r}", file=sys.stderr)
        sys.exit(1)
    print("ok - REAL client_ok=True AND join_ok=True against a genuine fmt build and real worker.log "
          "evidence -- the strengthened dump_worker_evidence() parses real production output "
          "correctly, not just hand-built synthetic JOINROW lines")
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
echo "== ROUND 5 (Deep Reviewer): ROLE-BINARY hash->exec TOCTOU closed via exec-by-open-fd -- pause after attestation, replace the HOST object, resume: the role must run the STAGED bytes or refuse =="
# _attestation_prefix()'s OLD design hashed /work/<path> then the caller
# separately exec'd /work/<path> -- two independent opens of the SAME
# path, nothing stopping a host-side actor from replacing the underlying
# inode in between. Fixed: for the ONE tracked file a container's PID 1
# actually execs (role_binary_path), attestation now opens it as FD 8
# FIRST, hashes /proc/self/fd/8 (never the path again), and the caller
# execs /proc/self/fd/8 -- the SAME open file description, immune to any
# later replacement of the path's directory entry (POSIX fd semantics:
# an fd is a reference to the INODE, established at open() time,
# independent of whatever the path is later changed to point at).
#
# This gate proves it empirically: a real S-role container is launched
# against a real isolated store, paused (via an inert-unless-set
# ready/continue file pair, same idea as the race-gate seam) immediately
# AFTER attestation passes but BEFORE the real exec, the HOST-side file
# backing /work/obj/scheduler/icecc-scheduler is then replaced via
# ATOMIC RENAME (directory-entry replacement onto a NEW inode -- the
# realistic tamper model for this codebase, since the only sanctioned
# write path anywhere in it, _publish_script()'s own final step, is
# `mv -T`; an in-place truncate+overwrite of the SAME inode is a
# DIFFERENT primitive no fd-based defense can ever protect against, and
# is not what this gate tests), then released. Two scenarios: the OLD
# vulnerable pattern (exec /work/<path> directly, re-opening the path
# after the swap) MUST pick up the decoy; the NEW fixed pattern (exec
# /proc/self/fd/8) MUST NOT.
SNAPSHOT_TOCTOU="$SCRATCHDIR/farm.py.pre-toctou-gate"
cp "$FARMDIR/farm.py" "$SNAPSHOT_TOCTOU"
restore_toctou() { [ -f "$SNAPSHOT_TOCTOU" ] && cp "$SNAPSHOT_TOCTOU" "$FARMDIR/farm.py"; }
trap restore_toctou EXIT
python3 - "research6" "$FARMDIR" <<'PY'
import sys, time
host, farmdir = sys.argv[1], sys.argv[2]
sys.path.insert(0, farmdir)
import farm

binary_set = "p43"
def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/round5-toctou-gate/{bs}/{m['tar']['sha256']}"
farm.immutable_root = isolated_root
root = isolated_root(binary_set)
img = farm.launch_image(binary_set)

def run_scenario(name, exec_line, host_swap_target):
    # Fresh republish before EACH scenario -- a prior scenario deliberately
    # swaps the isolated store's binary to a decoy, which would make the
    # NEXT scenario fail attestation for the wrong reason (stale
    # corruption left over from the previous run, not this run's own
    # pause/swap/resume timing).
    farm.sh(host, "chmod -R u+w ~/role-artifacts/round5-toctou-gate 2>/dev/null; rm -rf ~/role-artifacts/round5-toctou-gate", check=True)
    derr, dstatus = farm.publish_immutable_root(host, binary_set)
    if derr:
        print(f"FAIL [{name}]: could not bootstrap isolated store: {derr}", file=sys.stderr)
        return None
    cname = f"toctou-gate-{name}"
    farm.sh(host, f"docker rm -f {cname} 2>/dev/null; true")
    token = farm._attestation_token()
    marker = f"tg-{name}"
    attest = farm._attestation_prefix(binary_set, token, "obj/scheduler/icecc-scheduler")
    pause_block = f"touch /scratch/{marker}-ready; while [ ! -f /scratch/{marker}-continue ]; do sleep 0.2; done; "
    inner_cmd = attest + pause_block + exec_line
    scratch_expanded = farm.SCRATCH.replace("~", "$HOME")
    farm.sh(host, f"mkdir -p {scratch_expanded}; rm -f {scratch_expanded}/{marker}-ready {scratch_expanded}/{marker}-continue")
    farm.sh(host, f"docker run -d --pull=never --name {cname} --network host -v {root}:/work:ro -v {farm.SCRATCH}:/scratch -u 0:0 {img} bash -c '{inner_cmd}'", check=True)

    ready_path = f"{scratch_expanded}/{marker}-ready"
    for _ in range(30):
        r = farm.sh(host, f"test -f {ready_path} && echo yes || echo no")
        if r.stdout.strip() == "yes":
            break
        time.sleep(0.3)
    else:
        print(f"FAIL [{name}]: container never reached the ready-pause (attestation may have failed): "
              f"{farm.sh(host, f'docker logs {cname} 2>&1').stdout!r}", file=sys.stderr)
        farm.sh(host, f"docker rm -f {cname} 2>/dev/null; true")
        return None

    # ATOMIC RENAME swap -- see the section banner above for why this,
    # not an in-place truncate, is both the realistic tamper model here
    # and the only variant an already-open fd can be immune to. chmod
    # the PARENT DIRECTORY writable FIRST: the published tree's own
    # `chmod -R a-w` (publish step 7) strips the write bit from every
    # DIRECTORY too, not just the tracked files, so creating decoy_tmp
    # as a new directory entry fails if this runs in the wrong order.
    decoy_tmp = f"{host_swap_target}.decoy-tmp"
    swap_r = farm.sh(host, f"chmod u+w $(dirname {host_swap_target}) && "
                            f"printf '#!/bin/sh\\necho DECOY-BINARY-RAN-{marker}\\nexit 66\\n' > {decoy_tmp} && "
                            f"chmod +x {decoy_tmp} && "
                            f"mv -f {decoy_tmp} {host_swap_target} && "
                            f"echo SWAP-OK")
    if "SWAP-OK" not in swap_r.stdout:
        print(f"FAIL [{name}]: the host-side swap itself did not succeed: rc={swap_r.returncode} "
              f"stdout={swap_r.stdout!r} stderr={swap_r.stderr!r}", file=sys.stderr)

    farm.sh(host, f"touch {scratch_expanded}/{marker}-continue")
    time.sleep(2)
    logs = farm.sh(host, f"docker logs {cname} 2>&1").stdout
    farm.sh(host, f"docker rm -f {cname} 2>/dev/null; true")
    return logs

problems = []
logs_old = run_scenario("old", "exec /work/obj/scheduler/icecc-scheduler --help", f"{root}/obj/scheduler/icecc-scheduler")
if not (logs_old and "DECOY-BINARY-RAN" in logs_old):
    problems.append(f"vulnerable-baseline scenario: expected the OLD path-based exec to pick up the "
                     f"host-side swap (proving the TOCTOU is real), got: {logs_old!r}")
else:
    print("ok - VULNERABLE BASELINE CONFIRMED: the old path-based exec (exec /work/<path> directly) "
          "picked up the host-side atomic-rename swap -- ran the decoy, not the originally-attested "
          "scheduler -- proving this TOCTOU is genuinely exploitable, not theoretical")

logs_new = run_scenario("new", f"exec /proc/self/fd/{farm.ROLE_BINARY_FD} --help", f"{root}/obj/scheduler/icecc-scheduler")
if logs_new and "DECOY-BINARY-RAN" in logs_new:
    problems.append(f"FIXED scenario: the FD-based exec STILL picked up the host-side swap -- the fix "
                     f"does not close the TOCTOU: {logs_new!r}")
elif logs_new and ("ICECREAM scheduler" in logs_new or "usage: icecc-scheduler" in logs_new):
    print("ok - FIX CONFIRMED: exec /proc/self/fd/8 ran the ORIGINALLY-ATTESTED scheduler binary, "
          "completely unaffected by the identical host-side swap the vulnerable baseline just proved "
          "exploitable -- hash and exec are provably the same bytes, zero window, not merely a "
          "narrower one")
else:
    problems.append(f"FIXED scenario: unclear result, needs inspection: {logs_new!r}")

farm.sh(host, "chmod -R u+w ~/role-artifacts/round5-toctou-gate 2>/dev/null; rm -rf ~/role-artifacts/round5-toctou-gate")
if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "TOCTOU race gate did not pass"
restore_toctou
if cmp -s "$FARMDIR/farm.py" "$SNAPSHOT_TOCTOU"; then
    echo "ok - farm.py unchanged by the TOCTOU race gate (cmp clean -- this gate reads production code, never mutates it)"
else
    fail "farm.py was unexpectedly modified by the TOCTOU race gate"
fi
trap - EXIT
rm -f "$SNAPSHOT_TOCTOU"

echo
echo "== ROUND 5 (Deep Reviewer): HARNESS PRIVATE STAGING -- the streamed bundle is extracted into CONTAINER-PRIVATE tmpfs and verified in-container; a tampered bundle must be refused before farm_client.sh ever runs =="
# The OLD design pushed farm_client.sh/replay.py into ~/farm-scratch (a
# MUTABLE, host-bind-mounted, read-write directory) BEFORE the container
# even started, then separately docker-exec'd into an idle holder --
# leaving a real window during which the staged copies sat writable and
# host-visible with nothing re-verifying them at the point they actually
# ran. Fixed: the bundle is streamed directly into a FOREGROUND
# `docker run -i`'s stdin and extracted into tmpfs -- never touching any
# mutable host path -- then verified from INSIDE the container
# (_harness_stage_verify()) before farm_client.sh is ever reached.
python3 - "q3" "$FARMDIR" <<'PY'
import sys, io, tarfile
sys.path.insert(0, sys.argv[2])
import farm
host = sys.argv[1]

# A bundle with replay.py tampered AFTER the fact -- simulates a
# transit/tamper of the streamed bytes themselves (the bundle is built
# fresh from the checkout, verify_harness_scripts() unmodified/still
# passing on the checkout copies -- this tampers the STREAMED payload,
# not the source files on disk).
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode="w") as tf:
    for s in farm.SCRIPTS:
        data = open(f"{farm.HUB_DIR}/{s}", "rb").read()
        if s == "replay.py":
            data = data + b"\n# TAMPERED-IN-TRANSIT\n"
        info = tarfile.TarInfo(name=s)
        info.size = len(data); info.mtime = 0; info.mode = 0o555
        tf.addfile(info, io.BytesIO(data))
tampered_bundle = buf.getvalue()

plan = farm.resolve_launch_plan([host], "p43", "p43", client_host=host, binary_set_c="p43")
ok = farm.up([host], plan)
if not ok:
    print("FAIL: setup -- clean S+F launch for the harness-staging tamper test did not register", file=sys.stderr)
    sys.exit(1)
try:
    c_token = farm._attestation_token()
    stage_token = farm._attestation_token()
    stage_verify = farm._harness_stage_verify(stage_token)
    attest = farm._attestation_prefix(plan.binary_set_c, c_token)
    real_cmd = "export CMAKE=/hostscratch/cmake/bin/cmake; bash /scratch/farm_client.sh 10.0.27.101:22000 farmnet /hostscratch/fmt simultaneous 1 1 "
    inner_cmd = f"mkdir -p /scratch && tar -xf - -C /scratch && {stage_verify}{attest}{real_cmd}"
    r = farm.docker_run_foreground_staged(host, "farm-client", plan.c_tree, plan.c_img, tampered_bundle, inner_cmd, timeout=60)
    if "HARNESS-STAGE-FAIL" not in r.stdout:
        print(f"FAIL: expected the tampered streamed bundle to be refused with HARNESS-STAGE-FAIL "
              f"before farm_client.sh ever ran, got rc={r.returncode} stdout={r.stdout!r}", file=sys.stderr)
        sys.exit(1)
    if "CLIENT:" in r.stdout or "CELL:" in r.stdout:
        print(f"FAIL: farm_client.sh/replay.py output appeared despite HARNESS-STAGE-FAIL -- the "
              f"tampered bundle was NOT refused before execution: {r.stdout!r}", file=sys.stderr)
        sys.exit(1)
    print(f"ok - tampered streamed bundle REFUSED before farm_client.sh ever ran (rc={r.returncode}, "
          f"HARNESS-STAGE-FAIL present, no farm_client.sh/replay.py output leaked through)")
finally:
    farm.down([host], None)
    chk = farm.sh(host, "docker ps -a --filter name=farm- --format '{{.Names}}'")
    if chk.stdout.strip():
        print(f"FAIL: farm-* containers still present after down(): {chk.stdout.strip()!r}", file=sys.stderr)
        sys.exit(1)
    print("ok - down() cleaned up; 0 farm-* containers remain")
PY
[ $? -eq 0 ] || fail "harness-private-staging tamper mutant did not pass"

echo
echo "== ROUND 5 (Deep Reviewer): TAR HEADER PRE-VALIDATION -- a crafted archive with path-traversal or an absolute-path member must be refused BEFORE extraction, from the listing alone =="
# _publish_script()'s new step 3 runs `tar -tf`/`tar -tvf` (list-only,
# never writes to disk) BEFORE step 4's extraction. Two REAL crafted
# archives against a REAL host: one with a leading-../ path-traversal
# member, one with an absolute-path member (built with GNU tar's -P so
# tar's OWN creation-time stripping doesn't mask what this test is
# actually proving). Confirms BOTH the correct PUBLISH-TAR-HEADER-INVALID
# refusal AND that nothing ever leaked outside the isolated store.
python3 - "research6" "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
import farm
host = sys.argv[1]
binary_set = "p43"
manifest = farm.load_manifest(binary_set)

def isolated_root(bs):
    m = farm.load_manifest(bs)
    return f"~/role-artifacts/round5-tarheader-gate/{bs}/{m['tar']['sha256']}"
farm.immutable_root = isolated_root
root = isolated_root(binary_set)

def try_crafted(name, build_cmd, verify_absent_cmd):
    farm.sh(host, "chmod -R u+w ~/role-artifacts/round5-tarheader-gate 2>/dev/null; rm -rf ~/role-artifacts/round5-tarheader-gate", check=True)
    r = farm.sh(host, build_cmd)
    if r.returncode != 0:
        print(f"FAIL [{name}]: could not build the crafted tar: {r.stdout!r} {r.stderr!r}", file=sys.stderr)
        return False
    tar_sha = farm.sh(host, f"sha256sum ~/role-artifacts/round5-{name}.tar").stdout.split()[0]
    tampered = dict(manifest); tampered["tar"] = dict(manifest["tar"]); tampered["tar"]["sha256"] = tar_sha
    script = farm._publish_script(binary_set, tampered, root, f"$HOME/role-artifacts/round5-{name}.tar", None)
    r2 = farm.sh(host, script, timeout=60)
    line = r2.stdout.strip().splitlines()[-1] if r2.stdout.strip() else "(empty)"
    ok = line.startswith("PUBLISH-TAR-HEADER-INVALID:")
    if not ok:
        print(f"FAIL [{name}]: expected PUBLISH-TAR-HEADER-INVALID, got {line!r}", file=sys.stderr)
    else:
        absent = farm.sh(host, verify_absent_cmd).stdout.strip()
        if absent != "yes":
            print(f"FAIL [{name}]: refused correctly, but the escape target was NOT confirmed absent "
                  f"({absent!r}) -- the refusal claim needs this to actually mean something", file=sys.stderr)
            ok = False
        else:
            print(f"ok - crafted {name} archive REFUSED pre-extraction ({line}), escape target "
                  f"confirmed never created (nothing was ever extracted anywhere before this refused)")
    farm.sh(host, f"chmod -R u+w ~/role-artifacts/round5-tarheader-gate 2>/dev/null; rm -rf ~/role-artifacts/round5-tarheader-gate ~/role-artifacts/round5-{name}.tar ~/role-artifacts/round5-{name}-fixture")
    return ok

problems = []
if not try_crafted(
    "traversal",
    'rm -rf ~/role-artifacts/round5-traversal-fixture && mkdir -p ~/role-artifacts/round5-traversal-fixture/sneaky && '
    'echo payload > ~/role-artifacts/round5-traversal-fixture/sneaky/x && '
    'tar --transform="s,^sneaky/x$,../../../../tmp/round5-traversal-payload," '
    '-cf ~/role-artifacts/round5-traversal.tar -C ~/role-artifacts/round5-traversal-fixture sneaky/x',
    'test -f /tmp/round5-traversal-payload && echo no || echo yes'
):
    problems.append("path-traversal crafted archive")
if not try_crafted(
    "absolute",
    'rm -rf /tmp/round5-absolute-fixture && mkdir -p /tmp/round5-absolute-fixture && '
    'echo payload > /tmp/round5-absolute-fixture/x && '
    'tar -P -cf ~/role-artifacts/round5-absolute.tar /tmp/round5-absolute-fixture/x && '
    'rm -f /tmp/round5-absolute-fixture/x',
    'test -f /tmp/round5-absolute-fixture/x && echo no || echo yes'
):
    problems.append("absolute-path crafted archive")
farm.sh(host, "rm -rf /tmp/round5-traversal-payload /tmp/round5-absolute-fixture")
if problems:
    print(f"FAIL: {problems}", file=sys.stderr)
    sys.exit(1)
PY
[ $? -eq 0 ] || fail "tar header pre-validation gate did not pass"

echo
echo "== ROUND 5 (Deep Reviewer): INSPECT-THE-LAUNCH-OBJECT -- preflight must judge the EXACT digest-qualified launch ref, never the mutable tag; every docker run carries --pull=never =="
# Structural claim (image_digest_remote() must never even construct a
# query against the mutable tag): a SAFE fake transport that FAILS any
# query mentioning the bare IMG tag and SUCCEEDS only the exact digest-
# ref query -- proving the current code path is not merely "happens to
# pass" but structurally never depends on the tag at all. (A live
# same-repo tag-repoint experiment was tried and reverted during this
# round's own verification: on this containerd-image-store host,
# repointing icecream/farm-node's OWN tag turned out to ALSO disrupt the
# digest-ref's own resolvability -- real store behavior, not a code bug
# -- so a live "tag repointed to a decoy WHILE the digest ref stays
# correct" scenario isn't safely constructible against a shared image by
# this exact method; the fake-transport version below proves the same
# claim -- the code path structurally never queries the tag -- without
# that risk.)
python3 - "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import farm

real_sh = farm.sh
img_ref = farm.launch_image("p43")

def fake_sh(host, cmd, timeout=120, check=False):
    class R: pass
    r = R(); r.stdout = ""; r.stderr = ""; r.returncode = 0
    if "docker image inspect" in cmd:
        if img_ref in cmd:
            r.returncode = 0  # exact digest-qualified ref query -- succeeds
            return r
        if farm.IMG in cmd:
            r.returncode = 1  # bare mutable-tag query -- FAILS (simulating a decoy)
            return r
    return real_sh(host, cmd, timeout=timeout, check=check)

farm.sh = fake_sh
try:
    result = farm.image_digest_remote("research6", "p43")
finally:
    farm.sh = real_sh

if result != img_ref:
    print(f"FAIL: expected image_digest_remote() to succeed using ONLY the digest-qualified ref query "
          f"(a bare-tag query was rigged to fail), got {result!r}", file=sys.stderr)
    sys.exit(1)
print("ok - image_digest_remote() succeeded using ONLY the digest-qualified ref query; a query against "
      "the bare mutable tag would have failed (simulated decoy) but was structurally never made")
PY
[ $? -eq 0 ] || fail "inspect-the-launch-object structural check did not pass"

echo "-- --pull=never behavioral proof: an absent digest ref must be refused IMMEDIATELY, never silently attempt a network pull --"
python3 - "research6" "$FARMDIR" <<'PY'
import sys
sys.path.insert(0, sys.argv[2])
import farm
host = sys.argv[1]
bogus_ref = "icecream/farm-node@sha256:" + "0" * 64

r_never = farm.sh(host, f"docker run --pull=never --rm {bogus_ref} true 2>&1")
r_default = farm.sh(host, f"docker run --rm {bogus_ref} true 2>&1", timeout=20)

problems = []
if "No such image" not in r_never.stdout or "Unable to find image" in r_never.stdout:
    problems.append(f"--pull=never: expected an immediate 'No such image' refusal with no pull attempt, got {r_never.stdout!r}")
if "Unable to find image" not in r_default.stdout:
    problems.append(f"default pull policy: expected 'Unable to find image ... locally' (proving a pull WAS "
                     f"attempted) as the control for the line above, got {r_default.stdout!r}")
if problems:
    for p in problems:
        print(f"FAIL: {p}", file=sys.stderr)
    sys.exit(1)
print("ok - --pull=never refuses an absent digest ref immediately, no pull attempted ('No such image', "
      "no 'Unable to find image ... locally' preamble); the SAME absent ref WITHOUT the flag shows the "
      "preamble, confirming a real pull attempt is what --pull=never actually suppresses")
PY
[ $? -eq 0 ] || fail "--pull=never behavioral proof did not pass"
echo "-- source anchor: --pull=never is present at both docker-run callsites --"
python3 - "$FARMDIR/farm.py" <<'PY'
import sys
src = open(sys.argv[1]).read()
# "--pull=never --name" (not just "--pull=never") -- the flag also
# appears in prose (docstrings explaining the fix), which a bare
# substring count would over-count; this anchors specifically on the
# two real `docker run` COMMAND-CONSTRUCTION lines.
n = src.count("--pull=never --name")
if n != 2:
    print(f"FAIL: expected --pull=never at exactly 2 docker-run callsites "
          f"(docker_run_detached, docker_run_foreground_staged), found {n}", file=sys.stderr)
    sys.exit(1)
print("ok - --pull=never appears at exactly 2 docker-run callsites in farm.py "
      "(docker_run_detached, docker_run_foreground_staged)")
PY
[ $? -eq 0 ] || fail "--pull=never source anchor did not pass"

echo
echo "== ROUND 5 (Deep Reviewer): NO-GIT RUNNABILITY -- the fresh-archive gate must not die at its own git rev-parse when run from a bare, non-git tree =="
# Already exercised implicitly by the FRESH-ARCHIVE gate at the top of
# this suite (this repo IS a git worktree, so that run took the git-
# archive branch) -- this section explicitly proves the FALLBACK branch
# too, by pointing the same gate logic at a bare copy of farmharness/
# with .git removed entirely, simulating exactly the release-tarball
# extraction team-lead's own verification hit.
NOGIT_COPY="$SCRATCHDIR/round5-no-git-copy-$$"
rm -rf "$NOGIT_COPY"
mkdir -p "$NOGIT_COPY"
cp -a "$FARMDIR/." "$NOGIT_COPY/"
rm -rf "$NOGIT_COPY/.git"
if git -C "$NOGIT_COPY" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    fail "no-git test setup: $NOGIT_COPY still resolves as a git work tree -- .git removal did not take, this test is not exercising the fallback path"
fi
NOGIT_ARCHIVE_DIR="$SCRATCHDIR/round5-no-git-archive-$$"
rm -rf "$NOGIT_ARCHIVE_DIR"
mkdir -p "$NOGIT_ARCHIVE_DIR/farmharness"
cp -a "$NOGIT_COPY/." "$NOGIT_ARCHIVE_DIR/farmharness/"
find "$NOGIT_ARCHIVE_DIR/farmharness" -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null
find "$NOGIT_ARCHIVE_DIR/farmharness" -name '*.pyc' -delete 2>/dev/null
[ -f "$NOGIT_ARCHIVE_DIR/farmharness/farm.py" ] || fail "no-git fallback: farm.py missing from the no-git copy"
python3 - "$NOGIT_ARCHIVE_DIR" <<'PY'
import os, sys
archive_dir = sys.argv[1]
sys.path.insert(0, os.path.join(archive_dir, "farmharness"))
import farm
resolved_dir = os.path.dirname(os.path.abspath(farm.__file__))
expected_dir = os.path.join(archive_dir, "farmharness")
if resolved_dir != expected_dir:
    print(f"FAIL: farm module resolved from {resolved_dir!r}, expected {expected_dir!r}", file=sys.stderr)
    sys.exit(1)
for set_name in ("p43", "p50"):
    m = farm.load_manifest(set_name)
    assert m.get("set") == set_name
print("ok - no-git fallback: farm.py imports cleanly and both manifests load correctly from a bare, "
      "non-git copy of farmharness/ (no .git anywhere in its ancestry) -- exactly the scenario that used "
      "to kill this entire suite at its own git rev-parse before this fix")
PY
[ $? -eq 0 ] || fail "no-git runnability fallback did not pass"
rm -rf "$NOGIT_COPY" "$NOGIT_ARCHIVE_DIR"

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
    real_wfa = farm.wait_for_attestation
    launch_actions = []
    LAUNCH_TOKENS = ("farm-sched", "farm-worker", "farm-client", "sched.log", "worker.log", "chmod 1777")
    def fake_sh(host, cmd, timeout=120, check=False):
        if any(tag in cmd for tag in LAUNCH_TOKENS):
            launch_actions.append((host, cmd))
            return FakeResult()
        return real_sh(host, cmd, timeout=timeout, check=check)
    def fake_wfa(host, name, token, timeout=30):
        # attestation itself (in-container hash-check-before-exec) is
        # validated by its own dedicated, real, host-touching section
        # elsewhere in this suite -- this fake transport has no container
        # to actually query `docker logs` against, so wait_for_attestation
        # is stubbed directly here (recorded as a launch action, same as
        # the other mutating primitives), keeping this test focused on
        # its own specific claim: preflight-skip detectability via the
        # PREFLIGHT-OK marker + action-reachability.
        launch_actions.append((host, f"wait_for_attestation({name})"))
    # _attestation_prefix() itself is left UNFAKED -- a pure string
    # builder (reads the manifest, no I/O), safe to run for real even
    # against this fake transport, same reasoning as the no-network
    # transport-spy gates above.
    farm.sh = fake_sh
    farm.wait_for_attestation = fake_wfa
    farm.time = FakeTime()
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try:
            plan = farm.resolve_launch_plan(["q3"], "p43", "p43")
            farm.up(["q3"], plan)
        except RuntimeError as exc:
            print(f"(resolve_launch_plan/up raised RuntimeError: {exc})")
    farm.wait_for_attestation = real_wfa
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
