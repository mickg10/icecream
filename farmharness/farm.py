#!/usr/bin/env python3
"""farm.py -- single-command icecream cluster orchestrator (runs on the hub, drives hosts via SSH).
Phases: up (scheduler + one iceccd per F, --network host, SSD scratch) -> test -> down.
Down runs in a context manager so a crash still tears the cluster down.
This first cut proves cross-host registration; the test phase is layered on next."""
import hashlib, json, os, subprocess, sys, time, argparse

IMG = "icecream/farm-node:ubuntu22-gcc11-boost174"
NET = "farmnet"
SCHED_PORT = 22000
TREE = "~/icecream-review/c9488d74"          # built P50 tree (same on every host)
SCRATCH = "~/farm-scratch"                    # host SSD scratch (bind target)

# S4 (superseded): the ORIGINAL mutable-per-host layout, one fixed
# "<set>-root" path per host that `distribute` used to repair in place on
# drift. No code path in this file resolves through this dict anymore --
# every role/host now resolves via immutable_root() below, which derives a
# CONTENT-ADDRESSED path (a fresh directory named after the exact tar
# sha256 the committed manifest currently records) that is never repaired
# in place; a rebuild that changes the manifest's tar hash simply resolves
# to a different, new path. Kept only as a documented historical/migration
# reference (see S4_ROLE_ARTIFACTS.md) -- these directories may still
# physically exist on some hosts as harmless orphaned data from before this
# migration; nothing reads them anymore.
LEGACY_MUTABLE_ROOT = {
    "p43": "~/role-artifacts/p43-root",
    "p50": "~/role-artifacts/p50-root",
}
KNOWN_SETS = tuple(sorted(LEGACY_MUTABLE_ROOT))
MANIFEST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "role-manifests")
_MANIFEST_CACHE = {}

def immutable_root(binary_set):
    """The CONTENT-ADDRESSED, immutable runtime root for `binary_set`:
    ~/role-artifacts/<set>-<tar-sha256>, derived from the committed
    manifest's OWN tar.sha256 -- never a fixed, mutable per-set path. Two
    different builds of the same set always resolve to two different
    paths (the manifest's tar hash changed), so nothing ever needs to be
    edited or repaired inside an existing published directory: a
    directory at this exact path either already has the exact content its
    name asserts (verified on every access by preflight/publish, never
    just trusted from the name alone), or it does not exist yet."""
    if binary_set not in KNOWN_SETS:
        raise ValueError(f"unknown binary set {binary_set!r} (choices: {KNOWN_SETS})")
    manifest = load_manifest(binary_set)
    return f"~/role-artifacts/{binary_set}-{manifest['tar']['sha256']}"

def role_tree(binary_set):
    return TREE if binary_set is None else immutable_root(binary_set)

def load_manifest(binary_set):
    """The committed farmharness/role-manifests/{set}.json is the authority
    preflight/distribute/launch_image all consume -- exact source SHA,
    every binary's path/sha256/mode/size, the image digest, and the
    version-probe strings. S4_ROLE_ARTIFACTS.md is narrative only; this is
    what the code checks against. Any read/parse problem is reported as a
    RuntimeError (not a raw OSError/JSONDecodeError) so main()'s existing
    `except RuntimeError` prints a clean REFUSED line instead of a
    traceback -- an unreadable manifest is exactly the kind of fail-closed
    condition gap #1/#2 exist to catch, not a crash."""
    if binary_set not in _MANIFEST_CACHE:
        path = os.path.join(MANIFEST_DIR, f"{binary_set}.json")
        try:
            with open(path) as f:
                _MANIFEST_CACHE[binary_set] = json.load(f)
        except (OSError, json.JSONDecodeError) as exc:
            raise RuntimeError(f"role-manifest for {binary_set!r} unreadable ({path}): {exc}") from exc
    return _MANIFEST_CACHE[binary_set]

def sha256_remote(host, path):
    r = sh(host, f"sha256sum {path} 2>/dev/null")
    if r.returncode != 0 or not r.stdout.strip():
        return None
    return r.stdout.split()[0]

def mode_remote(host, path):
    r = sh(host, f"stat -c %a {path} 2>/dev/null")
    if r.returncode != 0 or not r.stdout.strip():
        return None
    return r.stdout.strip()

def _write_stripped(mode_str):
    """'755' -> '555', '664' -> '444' -- each octal digit with its write
    bit (2) cleared (digit & 5, since 5 = 0b101 keeps read+execute only).
    This is exactly what publish_immutable_root()'s post-verification
    `chmod -R a-w` produces from the manifest's recorded (pre-hardening)
    mode -- verified live against a real chmod -R a-w on q3. A file whose
    mode is its manifest mode with every write bit cleared is in a
    STRICTLY SAFER state than the manifest asserts, never a more
    permissive one, so accepting it as equally valid in verify_role_files()
    below is not a weakening; requiring an EXACT match instead would mean
    the read-only hardening step permanently and incorrectly reddens its
    own output on every future verification (caught live: it did, before
    this fix)."""
    return "".join(str(int(c) & 5) for c in mode_str)

def image_digest_remote(host):
    r = sh(host, f"docker image inspect {IMG} --format '{{{{index .RepoDigests 0}}}}' 2>/dev/null")
    out = r.stdout.strip()
    return out if r.returncode == 0 and out else None

def launch_image(binary_set):
    """The image reference for docker run: digest-pinned (repo@sha256:...,
    read live from the manifest -- itself populated by reading the digest
    live off q3, never hardcoded) when a binary_set is selected; the
    mutable tag stays only as the bootstrap alias for default/unselected
    launches (binary_set=None), matching prior behavior exactly."""
    if binary_set is None:
        return IMG
    manifest = load_manifest(binary_set)
    repo = IMG.split(":", 1)[0]
    return f"{repo}@{manifest['build']['image_digest']}"

def verify_role_files(host, root, manifest):
    """Pure verification: return a list of precise problem strings (empty
    list == every manifest-listed file matches on `host` exactly, sha256
    AND mode). Never mutates anything. Shared by preflight() (which must
    never repair -- only refuse) and publish_immutable_root() (which
    uses this same check to decide whether a host is already-current or
    needs a fresh publish, and to verify a temp sibling immediately after
    extracting it)."""
    problems = []
    for b in manifest["binaries"]:
        remote_path = f"{root}/{b['path']}"
        actual_sha = sha256_remote(host, remote_path)
        if actual_sha is None:
            problems.append(f"{remote_path} is absent or unreadable")
            continue
        if actual_sha != b["sha256"]:
            problems.append(f"{remote_path} sha256 mismatch (manifest {b['sha256']}, actual {actual_sha})")
            continue
        actual_mode = mode_remote(host, remote_path)
        if actual_mode != b["mode"] and actual_mode != _write_stripped(b["mode"]):
            problems.append(f"{remote_path} mode mismatch (manifest {b['mode']} or "
                             f"write-stripped {_write_stripped(b['mode'])}, actual {actual_mode})")
    return problems

def publish_immutable_root(host, binary_set):
    """Idempotently ensure immutable_root(binary_set) exists and is
    hash-clean on `host`. If it already verifies (verify_role_files() ==
    []), this is a pure no-op -- "already-current". Otherwise it extracts
    a manifest-verified copy of q3's tar into a FRESH TEMP SIBLING (never
    into the final name directly), verifies EVERY manifest entry against
    that temp copy, and only then atomically renames it into the final
    immutable name (`mv -Tn`: a single rename(2) on the same filesystem
    that refuses to clobber an existing destination -- verified live: GNU
    coreutils mv -Tn against an existing destination exits 0 but performs
    no move, so this function always re-verifies the FINAL name afterward
    rather than trusting the exit code). Successfully published trees are
    chmod'd read-only (a-w) as a defense-in-depth signal -- the real
    guarantee is that no code path in this file ever attempts to write
    into an existing published tree again, not the chmod bit alone (the
    owning user can always chmod their own files back, same as any Unix
    permission).

    An EXISTING final name that fails verification is a hard, unrepaired
    failure, by design: content-addressing means a hash-named directory
    that doesn't match its own name violates the one invariant the whole
    scheme rests on (tampering, or a bug -- either way, worth a human
    looking, not a silent auto-fix). Recovery is `rm -rf` that exact path
    on that host and re-running this function -- an explicit, visible,
    separately-authorized action, never performed automatically here.

    q3 cannot reach research6/research7/q2 directly on this network
    (confirmed: ssh from q3 to research6 fails host-key verification), so
    the hub -- which reaches every host in HOSTS -- relays the tar bytes
    rather than attempting a host-to-host rsync; on q3 itself the tar is
    extracted locally (q3 is the source of record for the tar file, so no
    relay is needed there).

    Returns (error, status): error=None and a human-readable status on
    success ("already-current" / "published (root was absent)"); a
    precise reason string as error (status=None) on failure -- including
    the hard "exists but fails verification" case above. Never touches
    docker."""
    manifest = load_manifest(binary_set)
    root = immutable_root(binary_set)

    exists = sh(host, f"test -d {root}").returncode == 0
    problems = verify_role_files(host, root, manifest) if exists else None
    if exists and not problems:
        return None, "already-current"
    if exists and problems:
        return (f"publish[{host}/{binary_set}]: {root} EXISTS but FAILS verification "
                f"({'; '.join(problems)}) -- this violates the content-addressing invariant "
                f"(a hash-named directory must always match its own name); NOT auto-repaired "
                f"(immutable roots are never fixed in place). Manual recovery: inspect for "
                f"tampering, then `rm -rf {root}` on {host} and re-run distribute."), None

    tar_name = manifest["tar"]["path"]
    if host == "q3":
        tar_bytes = None  # extracted directly on-host below; q3 is the tar's source of record
    else:
        pull = subprocess.run(HOSTS["q3"]["ssh"] + [f"cat ~/role-artifacts/{tar_name}"],
                               capture_output=True, timeout=120)
        if pull.returncode != 0 or not pull.stdout:
            return (f"publish[{host}/{binary_set}]: could not read {tar_name} from q3: "
                    f"{pull.stderr.decode(errors='replace')[:200]}"), None
        actual_tar_sha = hashlib.sha256(pull.stdout).hexdigest()
        if actual_tar_sha != manifest["tar"]["sha256"]:
            return (f"publish[{host}/{binary_set}]: tar sha256 mismatch reading from q3 "
                    f"(manifest {manifest['tar']['sha256']}, actual {actual_tar_sha})"), None
        tar_bytes = pull.stdout

    tmp = f"{root}.tmp-{os.getpid()}-{int(time.time() * 1000)}"
    if host == "q3":
        extract = sh(host, f"mkdir -p ~/role-artifacts && rm -rf {tmp} && mkdir -p {tmp} && "
                            f"tar -xf ~/role-artifacts/{tar_name} -C {tmp}", timeout=120)
        extract_ok, extract_err = extract.returncode == 0, extract.stderr
    else:
        extract_cmd = f"mkdir -p ~/role-artifacts && rm -rf {tmp} && mkdir -p {tmp} && tar -x -C {tmp}"
        push = subprocess.run(HOSTS[host]["ssh"] + [extract_cmd], input=tar_bytes,
                               capture_output=True, timeout=120)
        extract_ok, extract_err = push.returncode == 0, push.stderr.decode(errors="replace")
    if not extract_ok:
        sh(host, f"rm -rf {tmp}")
        return f"publish[{host}/{binary_set}]: extract failed on {host}: {extract_err[:200]}", None

    problems_tmp = verify_role_files(host, tmp, manifest)
    if problems_tmp:
        sh(host, f"rm -rf {tmp}")
        return (f"publish[{host}/{binary_set}]: freshly-extracted temp sibling failed verification "
                f"(never renamed into the final immutable name): {'; '.join(problems_tmp)}"), None

    # Atomic rename into the final immutable name. -Tn refuses to clobber an
    # existing destination (verified live: exits 0 but performs no move in
    # that case) -- if a concurrent publisher of this exact hash won the
    # race, that is fine, since both publishers extracted and verified the
    # SAME content; either way, the FINAL name is what gets re-verified
    # next, not the mv's exit code.
    sh(host, f"mv -Tn {tmp} {root} 2>/dev/null; rm -rf {tmp}")
    problems_final = verify_role_files(host, root, manifest)
    if problems_final:
        return (f"publish[{host}/{binary_set}]: final immutable path {root} failed verification "
                f"after publish: {'; '.join(problems_final)}"), None
    sh(host, f"chmod -R a-w {root} 2>/dev/null; true")
    return None, "published (root was absent)"

_ROLE_PROBE_PATH = {"S": "obj/scheduler/icecc-scheduler", "F": "obj/daemon/iceccd", "C": "obj/client/icecc"}

def preflight(host, binary_set):
    """PURE, resolution-phase identity-binding check: root presence + every
    manifest file's sha256 AND mode (catches a same-version, wrong-hash
    swap -- e.g. a trivial rebuild of one file -- which a version-string
    check alone would miss) + the pinned image's live RepoDigest. Not
    role-specific (a host+set's root/image identity doesn't depend on
    which role will use it) -- callers that need a role-specific check
    want verify_role_version() instead, which is intentionally a SEPARATE,
    later, explicitly launch-classified function: this one never invokes
    `docker run` (docker image inspect is a read-only metadata query, not
    a container launch) so it can honestly be called "resolution", with
    zero docker actions, full stop. Returns None on success, a precise
    reason string on failure. Never raises, never touches anything -- an
    absent or wrong root is refused here, never repaired here (see
    publish_immutable_root()'s docstring for why that split matters)."""
    manifest = load_manifest(binary_set)
    root = immutable_root(binary_set)

    if sh(host, f"test -d {root}").returncode != 0:
        return f"preflight[{host}/{binary_set}]: {root} is absent (run: python3 farm.py distribute --sets {binary_set} --hosts {host})"

    problems = verify_role_files(host, root, manifest)
    if problems:
        return f"preflight[{host}/{binary_set}]: " + "; ".join(problems)

    expect_digest = f"{IMG.split(':', 1)[0]}@{manifest['build']['image_digest']}"
    actual_digest = image_digest_remote(host)
    if actual_digest != expect_digest:
        return (f"preflight[{host}/{binary_set}]: image digest mismatch "
                f"(manifest {expect_digest}, actual {actual_digest!r})")
    return None

def verify_role_version(host, binary_set, role):
    """LAUNCH-phase (not resolution) check, deliberately separate from
    preflight() above: run ROLE's executable inside the pinned image,
    mounted READ-ONLY from the already-identity-verified immutable root,
    and confirm it reports the expected version identity. This DOES
    invoke `docker run --rm` -- an honest, throwaway, non-cluster-mutating
    docker action, but a docker action nonetheless, which is exactly why
    it must never be folded into preflight()'s claim of zero docker
    actions. Returns None on success (including "this role has no defined
    probe, nothing to check"), a precise reason string on failure."""
    manifest = load_manifest(binary_set)
    root = immutable_root(binary_set)
    probe_rel = _ROLE_PROBE_PATH.get(role)
    entry = next((b for b in manifest["binaries"] if b["path"] == probe_rel), None)
    if not entry or not entry["version_probe"]["command"]:
        return None
    probe = entry["version_probe"]
    if probe["command"] == "--version":
        r = sh(host, f"docker run --rm -v {root}:/probe:ro -u 0:0 {IMG} /probe/{probe_rel} --version 2>&1")
    else:  # "strings" -- no --version flag exists on this binary in either era
        r = sh(host, f"docker run --rm -v {root}:/probe:ro -u 0:0 {IMG} bash -c "
                     f"\"strings /probe/{probe_rel}\"")
    out = r.stdout
    expect = probe.get("expect_exact")
    ok = (out.strip() == expect) if expect is not None else (probe["expect_substring"] in out)
    if not ok:
        expect = expect if expect is not None else probe["expect_substring"]
        return (f"verify_role_version[{host}/{binary_set}/{role}]: version probe mismatch for "
                f"{probe_rel} (expected {expect!r}, got {out.strip()!r})")
    return None

def log_launch(line):
    """Print (the existing farm.py convention -- everything else in this
    file reports via stdout, captured by callers/tests already) AND append
    to a durable hub-side log file, so PREFLIGHT-OK/PREFLIGHT-FAIL markers
    survive past a single captured-stdout run too. Best-effort on the file
    half: an unwritable log must never itself block a launch decision."""
    print(line)
    try:
        with open(os.path.join(HUB_DIR, "farm-launch.log"), "a") as f:
            f.write(line + "\n")
    except OSError:
        pass

def resolve_role(host, binary_set, role):
    """Resolve the /work bind-source and docker image for one role --
    PURE identity-binding resolution only (preflight()): zero docker
    actions. binary_set=None reproduces prior behavior exactly (TREE, the
    mutable IMG tag, no preflight -- nothing existing breaks). The
    role-specific version-probe check (verify_role_version()) is
    deliberately NOT called here -- see resolve_launch_plan()'s two-pass
    docstring for why it is a separate, later, launch-classified step.

    A selected set is ONLY preflighted here -- never published. Bringing
    role-artifacts onto a host is exclusively the explicit `distribute`
    subcommand's job; the launch path must refuse (not silently fetch) if
    that step wasn't already run, per the spec's fail-closed requirement.
    Raises RuntimeError with the exact reason on any preflight failure,
    logging a PREFLIGHT-FAIL line first; logs PREFLIGHT-OK and returns the
    resolved (tree, image) pair on success. The caller must not perform any
    docker action for this role/host until this call returns normally."""
    if binary_set is None:
        return TREE, IMG
    err = preflight(host, binary_set)
    if err:
        log_launch(f"PREFLIGHT-FAIL host={host} role={role} set={binary_set} reason={err}")
        raise RuntimeError(err)
    log_launch(f"PREFLIGHT-OK host={host} role={role} set={binary_set}")
    return immutable_root(binary_set), launch_image(binary_set)

def verify_launch_version(host, binary_set, role):
    """The launch-phase half of resolving one role -- see
    resolve_launch_plan()'s docstring. Raises RuntimeError (logging
    PREFLIGHT-FAIL first) on a version-probe mismatch; logs
    LAUNCH-VALIDATED and returns None on success or on binary_set=None
    (nothing to check)."""
    if binary_set is None:
        return
    err = verify_role_version(host, binary_set, role)
    if err:
        log_launch(f"PREFLIGHT-FAIL host={host} role={role} set={binary_set} stage=version-probe reason={err}")
        raise RuntimeError(err)
    log_launch(f"LAUNCH-VALIDATED host={host} role={role} set={binary_set}")

def revalidate_before_mutation(host, binary_set):
    """Race-gate defense, called immediately before the FIRST
    cluster-mutating action for one role/host inside up()/run_client().
    resolve_launch_plan() already validated every role in this plan
    before up()/run_client() were even called -- this closes the
    remaining TOCTOU window between that resolution and THIS role's
    actual container start, in case anything (a concurrent publish, an
    out-of-band tamper) touched the selected immutable root in between.
    Re-running preflight() here is cheap (a handful of read-only SSH
    calls, zero docker actions of its own) and is the second of the two
    independent defenses against a race on an immutable root -- the first
    being that publish_immutable_root() never writes into an existing
    final name at all. binary_set=None is a no-op, matching every other
    function in this file's convention."""
    if binary_set is None:
        return
    err = preflight(host, binary_set)
    if err:
        log_launch(f"PREFLIGHT-FAIL host={host} set={binary_set} stage=revalidate-before-mutation reason={err}")
        raise RuntimeError(f"REVALIDATION FAILED immediately before launch (immutable root changed "
                            f"since resolution): {err}")

class LaunchPlan:
    """Every role resolved for one up[-test][-down] invocation, in the
    order resolve_launch_plan() resolves them (S, then all F, then C when
    used). Carrying every resolution in one object -- rather than up() and
    run_client() each re-resolving their own roles, as before -- is what
    makes "resolve EVERYTHING before mutating ANYTHING" possible without a
    double preflight run: there is exactly one place any role/host
    combination is ever preflighted for a given invocation, and it runs
    entirely before up()/run_client() perform a single docker/scratch/push
    action. Also carries the binary_set each role resolved against
    (binary_set_s/binary_set_f/binary_set_c), purely so up()/run_client()
    can call revalidate_before_mutation() with the right set immediately
    before each role's actual container start -- the race-gate's
    TOCTOU-closing second check, independent of the immutability
    publish_immutable_root() itself already provides."""
    __slots__ = ("s_tree", "s_img", "binary_set_s", "f_resolved", "binary_set_f",
                 "c_tree", "c_img", "binary_set_c")
    def __init__(self, s_tree, s_img, binary_set_s, f_resolved, binary_set_f,
                 c_tree=None, c_img=None, binary_set_c=None):
        self.s_tree, self.s_img, self.binary_set_s = s_tree, s_img, binary_set_s
        self.f_resolved, self.binary_set_f = f_resolved, binary_set_f
        self.c_tree, self.c_img, self.binary_set_c = c_tree, c_img, binary_set_c

def resolve_launch_plan(worker_hosts, binary_set_s, binary_set_f, client_host=None, binary_set_c=None):
    """Resolve the COMPLETE requested role plan -- S, every F host, and C
    (only when client_host is not None, i.e. the phase will actually use a
    client) -- in TWO passes, both entirely before up()/run_client()
    perform a single docker removal/start, scratch write, or client push.

    PASS 1 -- pure identity-binding resolution (resolve_role() ->
    preflight()): root presence, per-file sha256/mode, and the image
    digest. Genuinely zero `docker run` for every role, in order (S, every
    F, C). Raises RuntimeError with the exact reason on the first role
    that fails; by construction, at the moment this raises, NOTHING has
    happened for ANY role in this plan -- including roles that resolved
    successfully earlier in this same pass. That is the ordering fix this
    whole function exists for: previously, up() resolved+launched the
    scheduler, THEN resolved+launched each worker in the same loop, so a
    bad second worker was only discovered after the scheduler and first
    worker were already torn down and started (LO's finding on 02622ab6).

    PASS 2 -- launch-phase version-probe validation (verify_launch_version()
    -> verify_role_version()), only reached once EVERY role in pass 1
    succeeded. Each probe is a throwaway `docker run --rm` container --
    never farm-sched/farm-worker/farm-client, so still no CLUSTER action
    happens here -- but it IS a docker action, honestly a distinct,
    later, launch-classified step, never folded into pass 1's claim of
    zero docker actions (LO's second finding: production preflight() used
    to run these probes DURING resolution, so a late-failing role could
    be preceded by other roles' probe containers having already run for
    real, silently contradicting a "zero docker actions before refusal"
    claim). A role that fails here, after earlier roles' probes already
    ran, still leaves zero CLUSTER-mutating actions taken -- up() and
    run_client() are never reached either way.

    Neither up() nor run_client() resolve or preflight anything
    themselves -- they only ever consume a LaunchPlan that this function
    already validated in full, in both passes."""
    s_tree, s_img = resolve_role(SCHED_HOST, binary_set_s, "S")
    f_resolved = [resolve_role(h, binary_set_f, "F") for h in worker_hosts]
    c_tree = c_img = None
    if client_host is not None:
        c_tree, c_img = resolve_role(client_host, binary_set_c, "C")

    verify_launch_version(SCHED_HOST, binary_set_s, "S")
    for h in worker_hosts:
        verify_launch_version(h, binary_set_f, "F")
    if client_host is not None:
        verify_launch_version(client_host, binary_set_c, "C")

    return LaunchPlan(s_tree, s_img, binary_set_s, f_resolved, binary_set_f,
                       c_tree, c_img, binary_set_c)

# per-host SSH argv + LAN IP + role capability
HOSTS = {
    "q3":        {"ssh": ["ssh","-o","HostName=10.0.27.101","-o","HostKeyAlias=tt-quietbox3","-o","BatchMode=yes","mickg10@tt-quietbox3"], "ip": "10.0.27.101"},
    "research6": {"ssh": ["ssh","-o","BatchMode=yes","research6"],                                                                        "ip": "10.0.27.56"},
    "research7": {"ssh": ["ssh","-o","BatchMode=yes","research7"],                                                                        "ip": "10.0.27.58"},
    "q2":        {"ssh": ["ssh","-o","HostName=100.91.242.69","-o","BatchMode=yes","mickg10@tt-quietbox2"],                               "ip": "10.0.27.212"},
}
SCHED_HOST = "q3"

def sh(host, cmd, timeout=120, check=False):
    r = subprocess.run(HOSTS[host]["ssh"] + [cmd], capture_output=True, text=True, timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(f"[{host}] {cmd!r} rc={r.returncode}: {r.stderr.strip()[:300]}")
    return r

def docker_rm(host, name):
    sh(host, f"docker rm -f {name} 2>/dev/null; true")

def docker_run_detached(host, name, tree, img, inner_cmd):
    """Start a detached (docker run -d), named, persistent container.
    Together with docker_rm(), this is the ONLY docker-mutating primitive
    up() uses -- factored out of the inline sh() calls that used to be
    here specifically so a no-network test can monkeypatch exactly these
    two named functions (and nothing else, no command-text pattern
    matching needed) to prove zero containers are touched for a launch
    plan that never made it past resolution. `/work` is mounted READ-ONLY
    (`:ro`) -- the running scheduler/daemon/client only ever READ their own
    binary from it (all logs/state go to the separate `/scratch` mount,
    which stays writable); nothing at runtime has a legitimate reason to
    write into an immutable role-artifact root, so the container itself is
    now structurally prevented from doing so (verified live: a write
    attempt through a `:ro` bind mount fails with "Read-only file system")."""
    sh(host, f"docker run -d --name {name} --network host -v {tree}:/work:ro -v {SCRATCH}:/scratch -u 0:0 {img} "
             f"bash -c '{inner_cmd}'", check=True)

def scratch_prepare(host, mkdir_path, log_path):
    """Reset the SCRATCH-relative logging area on host before a launch.
    The ONLY scratch-mutating primitive up() uses -- factored out for the
    same reason as docker_run_detached() above."""
    sh(host, f"mkdir -p {mkdir_path} && chmod 1777 {SCRATCH}/farm && rm -f {log_path}", check=True)

def up(worker_hosts, plan):
    """Launch the cluster from an ALREADY-validated LaunchPlan -- up()
    itself no longer resolves or preflights anything: every role in this
    plan was already resolved (identity-bound AND version-validated), for
    every host, inside resolve_launch_plan() before this function was ever
    called. up() therefore performs ONLY the mutating actions (docker_rm,
    scratch_prepare, docker_run_detached), each immediately preceded by
    revalidate_before_mutation() -- a cheap, zero-docker-run re-check that
    the SAME immutable root resolve_launch_plan() already validated is
    still exactly what it was (the race-gate's second, independent defense
    against anything touching a selected root between resolution and this
    specific role's actual container start). up() can no longer discover a
    bad role partway through a launch that already tore down or started an
    earlier one (the defect LO/BO both flagged on 02622ab6)."""
    sip = HOSTS[SCHED_HOST]["ip"]
    print(f"UP: scheduler on {SCHED_HOST}({sip}):{SCHED_PORT}  workers={worker_hosts}  "
          f"S-tree={plan.s_tree}  F-trees={[t for t, _ in plan.f_resolved]}")
    # scheduler (--network host so it binds the LAN IP). The daemon/scheduler drop privileges to
    # icecc at startup, so the -l log dir must be writable by that user => useradd icecc + 1777 dir.
    revalidate_before_mutation(SCHED_HOST, plan.binary_set_s)
    docker_rm(SCHED_HOST, "farm-sched")
    scratch_prepare(SCHED_HOST, f"{SCRATCH}/farm", f"{SCRATCH}/farm/sched.log")
    docker_run_detached(SCHED_HOST, "farm-sched", plan.s_tree, plan.s_img,
        f"useradd -r icecc 2>/dev/null; exec /work/obj/scheduler/icecc-scheduler -p {SCHED_PORT} -n {NET} -l /scratch/farm/sched.log -vvv")
    time.sleep(3)
    # one worker per F -- every host's role was already resolved, in order,
    # inside resolve_launch_plan(), before this loop (or anything else in
    # this function) performed a single docker/scratch action.
    for i, h in enumerate(worker_hosts):
        wp = 12000 + i
        f_tree, f_img = plan.f_resolved[i]
        revalidate_before_mutation(h, plan.binary_set_f)
        docker_rm(h, "farm-worker")
        # chmod only the mickg-owned dir (not -R: stale daemon files are uid-999, unchmod-able by host user; rm clears them)
        scratch_prepare(h, f"{SCRATCH}/farm/envs", f"{SCRATCH}/farm/worker.log")
        docker_run_detached(h, "farm-worker", f_tree, f_img,
            f"useradd -r -s /usr/sbin/nologin icecc 2>/dev/null; "
            f"chown icecc /scratch/farm/envs; "  # pre-chown as root: daemon's cleanup_cache runs post-drop (no CAP_CHOWN)
            f"exec /work/obj/daemon/iceccd -m 8 -s {sip}:{SCHED_PORT} -n {NET} -N {h}w -b /scratch/farm/envs "
            f"-p {wp} -l /scratch/farm/worker.log -vvv")
    # wait for all workers to register
    want = len(worker_hosts)
    got = 0
    for _ in range(40):
        r = sh(SCHED_HOST, f"grep -c 'login' {SCRATCH}/farm/sched.log 2>/dev/null; true")
        try: got = int((r.stdout.strip() or "0").splitlines()[0])
        except Exception: got = 0
        if got >= want: break
        time.sleep(1)
    r = sh(SCHED_HOST, f"grep -iE 'login|Login|connect|node' {SCRATCH}/farm/sched.log 2>/dev/null | tail -8; true")
    print(f"UP: registrations={got}/{want}")
    print("SCHED LOG (tail):"); print(r.stdout.rstrip())
    if got < want:
        for h in worker_hosts:
            wl = sh(h, f"grep -iE 'scheduler|connect|login|fail|error' {SCRATCH}/farm/worker.log 2>/dev/null | tail -6; true")
            print(f"WORKER {h} LOG:"); print(wl.stdout.rstrip() or "(empty)")
    return got >= want

SCRIPTS = ["farm_client.sh", "replay.py"]   # pushed to the client host's /scratch before a test
HUB_DIR = "/tanksmall/scratch/claude-tmp/claude-4103/-tanksmall-MICKG2-mickg-src/a39fdb74-75de-4406-a55d-a11442769f8a/scratchpad"

def push_file(host, localpath, remotepath):
    data = open(localpath, "rb").read()
    p = subprocess.run(HOSTS[host]["ssh"] + [f"cat > {remotepath}"], input=data, capture_output=True, timeout=60)
    if p.returncode != 0:
        raise RuntimeError(f"push {localpath}->{host}:{remotepath} failed: {p.stderr.decode()[:200]}")

def run_client(client_host, project_dir, mode, maxtu, jobs, prefer, plan):
    """Run the client against an ALREADY-validated LaunchPlan's C
    resolution -- like up(), run_client() no longer resolves or
    preflights anything itself; that already happened inside
    resolve_launch_plan(), before up() was even called, let alone this
    function. revalidate_before_mutation() runs first, before push_file()
    even -- push_file() is itself one of the actions that must never
    happen ahead of a full, successful (re)validation. `/work` is mounted
    READ-ONLY (`:ro`), same reasoning as docker_run_detached()."""
    revalidate_before_mutation(client_host, plan.binary_set_c)
    sched = f"{HOSTS[SCHED_HOST]['ip']}:{SCHED_PORT}"
    for s in SCRIPTS:
        push_file(client_host, f"{HUB_DIR}/{s}", f"{SCRATCH.replace('~', '$HOME')}/{s}")
    # resolve ~ on the client for the bind (docker needs an absolute host path)
    docker_rm(client_host, "farm-client")
    cmd = (f"docker run --rm --name farm-client --network host -v {plan.c_tree}:/work:ro -v {SCRATCH}:/scratch -u 0:0 {plan.c_img} "
           f"bash /scratch/farm_client.sh {sched} {NET} {project_dir} {mode} {maxtu} {jobs} {prefer}")
    print(f"TEST: client={client_host} project={project_dir} mode={mode} maxtu={maxtu} jobs={jobs} prefer={prefer} "
          f"C-tree={plan.c_tree}")
    r = sh(client_host, cmd, timeout=1800)
    print(r.stdout.rstrip())
    if r.stderr.strip():
        print("TEST STDERR:", r.stderr.strip()[:400])
    return r

def dump_worker_evidence(worker_hosts, client_stdout=""):
    # local-oracle canonical JOIN (2026-08-23 ruling): one row per expected TU, gated as a BIJECTION —
    # client rows (JobID, endpoint, accepted, exact, sha) vs F-side job-id sets + completion counts.
    rows = []
    for ln in client_stdout.splitlines():
        if ln.startswith("JOINROW "):
            d = dict(kv.split("=", 1) for kv in ln.split()[1:] if "=" in kv)
            rows.append(d)
    fmap = {}   # host -> (requested job-id set, completions)
    for h in worker_hosts:
        r = sh(h, f"grep -oE 'request for job [0-9]+' {SCRATCH}/farm/worker.log 2>/dev/null | grep -oE '[0-9]+'; true")
        ids = set(int(x) for x in r.stdout.split()) if r.stdout.strip() else set()
        c = sh(h, f"grep -c 'Remote compilation completed with exit code 0' {SCRATCH}/farm/worker.log 2>/dev/null; true")
        try: comp = int((c.stdout.strip().splitlines() or ["0"])[0])
        except Exception: comp = 0
        fmap[h] = (ids, comp)
        print(f"  F={h}: requested-job-ids={len(ids)} completions(exit0)={comp}")
    # bijection checks
    n = len(rows)
    cl_ids = [int(r0["jobid"]) for r0 in rows]
    ok_rows  = all(r0["accepted"] == "1" and r0["exact"] == "1" and r0["mode"] == "remote" for r0 in rows)
    uniq     = len(set(cl_ids)) == n and -1 not in cl_ids
    f_union  = set().union(*(v[0] for v in fmap.values())) if fmap else set()
    covered  = set(cl_ids) <= f_union
    no_orph  = sum(v[1] for v in fmap.values()) == n     # F completions == accepted client rows (no orphan)
    verdict = ok_rows and uniq and covered and no_orph and n > 0
    print(f"CELL-JOIN: rows={n} all-accepted-exact-remote={ok_rows} jobids-unique={uniq} "
          f"F-coverage={covered} no-orphans={no_orph} -> BIJECTION {'OK' if verdict else 'FAIL'}")
    return verdict

def down(worker_hosts, client_host=None):
    print("DOWN: tearing down cluster")
    docker_rm(SCHED_HOST, "farm-sched")
    for h in worker_hosts:
        docker_rm(h, "farm-worker")
    if client_host:
        docker_rm(client_host, "farm-client")

def distribute(sets, hosts):
    """`python3 farm.py distribute --sets p43,p50 --hosts research6,research7,q2`
    -- the ONLY code path allowed to write role-artifacts onto a host
    (never called from resolve_role()/up()/run_client()/
    revalidate_before_mutation(); those only ever preflight-check and
    refuse). Publishes the CONTENT-ADDRESSED immutable_root(binary_set) on
    each host: already-published-and-verified is a pure no-op; anything
    else is published fresh via publish_immutable_root() (temp-sibling
    extract, full verify, atomic rename -- never an in-place edit of an
    existing immutable directory). q3 is included (not skipped) -- it also
    needs its own immutable-named copy published from its local tar, same
    as every other host, so preflight/resolve_role never has to special-
    case where a root's content actually lives. Exits nonzero if any
    (set, host) pair ends in a hard failure, INCLUDING an existing
    immutable path that fails verification (a content-addressing
    invariant violation this function deliberately does not auto-repair --
    see publish_immutable_root()'s docstring)."""
    failures = []
    for binary_set in sets:
        if binary_set not in KNOWN_SETS:
            failures.append(f"{binary_set}: unknown binary set (choices: {KNOWN_SETS})")
            continue
        for host in hosts:
            if host not in HOSTS:
                failures.append(f"{binary_set}/{host}: unknown host (choices: {sorted(HOSTS)})")
                continue
            err, status = publish_immutable_root(host, binary_set)
            if err:
                print(f"DISTRIBUTE FAIL {binary_set}->{host}: {err}")
                failures.append(f"{binary_set}/{host}: {err}")
            else:
                print(f"DISTRIBUTE OK {binary_set}->{host}: {status}")
    if failures:
        print(f"DISTRIBUTE: {len(failures)} failure(s)")
        sys.exit(1)
    print("DISTRIBUTE: all sets/hosts current")

def main():
    if len(sys.argv) > 1 and sys.argv[1] == "distribute":
        # Separate, minimal parser so this new subcommand can't perturb the
        # existing flag surface below in any way (no shared ArgumentParser).
        dp = argparse.ArgumentParser(prog="farm.py distribute")
        dp.add_argument("--sets", required=True, help="comma list, e.g. p43,p50")
        dp.add_argument("--hosts", required=True, help="comma list of target hosts")
        da = dp.parse_args(sys.argv[2:])
        distribute(da.sets.split(","), da.hosts.split(","))
        return
    ap = argparse.ArgumentParser()
    ap.add_argument("--workers", default="research6", help="comma list of worker hosts")
    ap.add_argument("--client", default="q3", help="client host (runs the submitter + compiles)")
    ap.add_argument("--project", default="/scratch/fmt")
    ap.add_argument("--mode", default="simultaneous")
    ap.add_argument("--maxtu", default="0")
    ap.add_argument("--jobs", default="16")
    ap.add_argument("--phase", default="up-test-down", choices=["up","up-down","up-test-down"])
    ap.add_argument("--binary-set-S", dest="binary_set_s", default=None, choices=KNOWN_SETS,
                     help="scheduler role binary set (default: existing hardcoded TREE, unchanged)")
    ap.add_argument("--binary-set-C", dest="binary_set_c", default=None, choices=KNOWN_SETS,
                     help="client role binary set (default: existing hardcoded TREE, unchanged)")
    ap.add_argument("--binary-set-F", dest="binary_set_f", default=None, choices=KNOWN_SETS,
                     help="worker(F) role binary set, applied to every worker host (default: existing hardcoded TREE, unchanged)")
    a = ap.parse_args()
    workers = a.workers.split(",")
    prefer = (workers[0] + "w") if len(workers) == 1 else ""   # pin for 1 F; let scheduler balance for >1 F
    want_client = a.phase == "up-test-down"
    ok = False
    down_needed = False   # flips True only once real launch actions begin --
                           # see resolve_launch_plan()/up() docstrings. A
                           # plan-validation refusal must NEVER call down():
                           # down() docker-rm's the fixed farm-{sched,worker,
                           # client} names unconditionally, which could
                           # disturb a pre-existing, unrelated, exact-name
                           # running cluster while this invocation is simply
                           # declining to start a NEW one (LO's finding).
    try:
        try:
            plan = resolve_launch_plan(workers, a.binary_set_s, a.binary_set_f,
                                        a.client if want_client else None, a.binary_set_c)
        except RuntimeError as exc:
            # Nothing was resolved-and-launched, and nothing was even
            # resolved for EVERY role (a later role's failure means
            # earlier roles' successful resolutions were never acted on
            # either) -- refuse cleanly, zero docker/scratch/push actions
            # taken, down() never invoked.
            print(f"REFUSED: {exc}")
        else:
            down_needed = True   # every role in the plan validated; real
                                  # actions begin now, so teardown owes a
                                  # visit regardless of what happens below.
            try:
                ok = up(workers, plan)
            except RuntimeError as exc:
                # Real launch actions had already begun (down_needed is
                # already True) -- this is a genuine command failure
                # mid-launch, not a plan-validation refusal.
                print(f"LAUNCH FAILED: {exc}")
                ok = False
            else:
                print("CLUSTER:", "REGISTERED-OK" if ok else "REGISTRATION-FAILED")
                if ok and want_client:
                    try:
                        r = run_client(a.client, a.project, a.mode, a.maxtu, a.jobs, prefer, plan)
                    except RuntimeError as exc:
                        print(f"LAUNCH FAILED: {exc}")
                        ok = False
                    else:
                        dump_worker_evidence(workers, r.stdout)
    finally:
        if down_needed and a.phase in ("up-down", "up-test-down"):
            down(workers, a.client)
    sys.exit(0 if ok else 2)

# S4: guarded so role_tree()/HOSTS/KNOWN_SETS can be imported (e.g. by
# artifact_selection_test.sh) without triggering a live cluster deploy.
if __name__ == "__main__":
    main()
