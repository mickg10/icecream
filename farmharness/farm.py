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

# S4 role-artifact selection: per-role hash-bound runtime roots (see
# ~/role-artifacts/{p43,p50}-root/MANIFEST.tsv on a prepared host), each
# laid out as obj/{scheduler,daemon,client}/... exactly like TREE, so it is
# a drop-in /work bind-source. binary_set=None (the default everywhere)
# preserves the exact prior hardcoded-TREE behavior for every role.
ROLE_SET_DIR = {
    "p43": "~/role-artifacts/p43-root",
    "p50": "~/role-artifacts/p50-root",
}
MANIFEST_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "role-manifests")
_MANIFEST_CACHE = {}

def role_tree(binary_set):
    if binary_set is None:
        return TREE
    if binary_set not in ROLE_SET_DIR:
        raise ValueError(f"unknown binary set {binary_set!r} (choices: {sorted(ROLE_SET_DIR)})")
    return ROLE_SET_DIR[binary_set]

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
    never repair -- only refuse) and distribute_role_artifacts() (which
    uses this same check to decide whether a host is already-current or
    needs a (re)sync, and to verify a fresh copy immediately after
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
        if actual_mode != b["mode"]:
            problems.append(f"{remote_path} mode mismatch (manifest {b['mode']}, actual {actual_mode})")
    return problems

def distribute_role_artifacts(host, binary_set):
    """Idempotently materialize ROLE_SET_DIR[binary_set] on `host`: verify
    what's already there against the manifest (verify_role_files()); if it
    already matches exactly, no-op. Otherwise (root absent, incomplete, OR
    every-file-verified-wrong -- e.g. drift/corruption/tampering since the
    last distribute), (re)transfer a manifest-verified copy of q3's tar and
    verify again post-copy. This IS a repair path, deliberately: distribute
    is only ever invoked explicitly by an operator (the `distribute`
    subcommand), never automatically by the launch path (resolve_role()
    calls preflight() only) -- so a repair here is always a visible,
    intentional action, never a side effect of trying to launch a cluster.
    preflight staying separate and non-repairing is what keeps "distribute
    wasn't run" and "distribute was run and is wrong" both fail-closed on
    the launch path; this function existing IS the remediation for either.

    q3 cannot reach research6/research7/q2 directly on this network
    (confirmed: ssh from q3 to research6 fails host-key verification), so
    the hub -- which reaches every host in HOSTS -- relays the tar bytes
    rather than attempting a host-to-host rsync.

    Returns (error, status): error=None and a human-readable status string
    on success ("already-current" / "bootstrapped (root was absent)" /
    "repaired: <problems>"); a precise reason string as error (status=None)
    on failure. Never touches docker."""
    if host == "q3":
        return None, "source-of-record (q3 itself, not distributed)"
    manifest = load_manifest(binary_set)
    root = ROLE_SET_DIR[binary_set]

    exists = sh(host, f"test -d {root}").returncode == 0
    problems = verify_role_files(host, root, manifest) if exists else ["root directory is absent"]
    if not problems:
        return None, "already-current"

    tar_name = manifest["tar"]["path"]
    pull = subprocess.run(HOSTS["q3"]["ssh"] + [f"cat ~/role-artifacts/{tar_name}"],
                           capture_output=True, timeout=120)
    if pull.returncode != 0 or not pull.stdout:
        return (f"distribute[{host}/{binary_set}]: could not read {tar_name} from q3: "
                f"{pull.stderr.decode(errors='replace')[:200]}"), None
    actual_tar_sha = hashlib.sha256(pull.stdout).hexdigest()
    if actual_tar_sha != manifest["tar"]["sha256"]:
        return (f"distribute[{host}/{binary_set}]: tar sha256 mismatch reading from q3 "
                f"(manifest {manifest['tar']['sha256']}, actual {actual_tar_sha})"), None

    extract_cmd = f"mkdir -p ~/role-artifacts && rm -rf {root} && mkdir -p {root} && tar -x -C {root}"
    push = subprocess.run(HOSTS[host]["ssh"] + [extract_cmd], input=pull.stdout,
                           capture_output=True, timeout=120)
    if push.returncode != 0:
        return (f"distribute[{host}/{binary_set}]: extract failed on {host}: "
                f"{push.stderr.decode(errors='replace')[:200]}"), None

    problems_after = verify_role_files(host, root, manifest)
    if problems_after:
        return (f"distribute[{host}/{binary_set}]: post-copy verification FAILED: "
                f"{'; '.join(problems_after)}"), None
    if "root directory is absent" in problems:
        return None, "bootstrapped (root was absent)"
    return None, "repaired: " + "; ".join(problems)

_ROLE_PROBE_PATH = {"S": "obj/scheduler/icecc-scheduler", "F": "obj/daemon/iceccd", "C": "obj/client/icecc"}

def preflight(host, binary_set, role):
    """Fail-closed gate: before ANY docker action (including docker_rm) for
    a selected binary_set, verify on `host` that every manifest-listed
    file's sha256 AND mode match exactly (catches a same-version,
    wrong-hash swap -- e.g. a trivial rebuild of one file -- which a
    version-string check alone would miss), the pinned image's live
    RepoDigest matches the manifest, and the launched role binary's own
    identity probe matches. Returns None on success; a precise reason
    string on failure. Never raises, never touches docker, and -- unlike
    distribute_role_artifacts() -- never mutates anything: an absent or
    wrong root is refused here, not repaired here (see
    distribute_role_artifacts()'s docstring for why that split matters)."""
    manifest = load_manifest(binary_set)
    root = ROLE_SET_DIR[binary_set]

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

    probe_rel = _ROLE_PROBE_PATH.get(role)
    entry = next((b for b in manifest["binaries"] if b["path"] == probe_rel), None)
    if entry and entry["version_probe"]["command"]:
        probe = entry["version_probe"]
        if probe["command"] == "--version":
            r = sh(host, f"docker run --rm -v {root}:/probe -u 0:0 {IMG} /probe/{probe_rel} --version 2>&1")
        else:  # "strings" -- no --version flag exists on this binary in either era
            r = sh(host, f"docker run --rm -v {root}:/probe -u 0:0 {IMG} bash -c "
                         f"\"strings /probe/{probe_rel}\"")
        out = r.stdout
        expect = probe.get("expect_exact")
        if expect is not None:
            ok = out.strip() == expect
        else:
            expect = probe["expect_substring"]
            ok = expect in out
        if not ok:
            return (f"preflight[{host}/{binary_set}]: version probe mismatch for {probe_rel} "
                    f"(expected {expect!r}, got {out.strip()!r})")
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
    """Resolve the /work bind-source and docker image for one role launch.
    binary_set=None reproduces prior behavior exactly (TREE, the mutable
    IMG tag, no preflight -- nothing existing breaks).

    A selected set is ONLY preflighted here -- never distributed. Bringing
    role-artifacts onto a host is exclusively the explicit `distribute`
    subcommand's job; the launch path must refuse (not silently fetch) if
    that step wasn't already run, per the spec's fail-closed requirement.
    Raises RuntimeError with the exact reason on any preflight failure,
    logging a PREFLIGHT-FAIL line first; logs PREFLIGHT-OK and returns the
    resolved (tree, image) pair on success. The caller must not perform any
    docker action for this role/host until this call returns normally."""
    if binary_set is None:
        return TREE, IMG
    err = preflight(host, binary_set, role)
    if err:
        log_launch(f"PREFLIGHT-FAIL host={host} role={role} set={binary_set} reason={err}")
        raise RuntimeError(err)
    log_launch(f"PREFLIGHT-OK host={host} role={role} set={binary_set}")
    return ROLE_SET_DIR[binary_set], launch_image(binary_set)

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

def up(worker_hosts, binary_set_s=None, binary_set_f=None):
    sip = HOSTS[SCHED_HOST]["ip"]
    # S4: resolve_role runs distribute+preflight (when a set is selected)
    # and raises BEFORE any docker action for that role/host if either
    # fails -- this call must stay ahead of docker_rm/docker run below.
    s_tree, s_img = resolve_role(SCHED_HOST, binary_set_s, "S")
    print(f"UP: scheduler on {SCHED_HOST}({sip}):{SCHED_PORT}  workers={worker_hosts}  "
          f"S-set={binary_set_s or 'default'}({s_tree})  F-set={binary_set_f or 'default'}")
    # scheduler (--network host so it binds the LAN IP). The daemon/scheduler drop privileges to
    # icecc at startup, so the -l log dir must be writable by that user => useradd icecc + 1777 dir.
    docker_rm(SCHED_HOST, "farm-sched")
    sh(SCHED_HOST, f"mkdir -p {SCRATCH}/farm && chmod 1777 {SCRATCH}/farm && rm -f {SCRATCH}/farm/sched.log", check=True)
    sh(SCHED_HOST,
       f"docker run -d --name farm-sched --network host -v {s_tree}:/work -v {SCRATCH}:/scratch -u 0:0 {s_img} "
       f"bash -c 'useradd -r icecc 2>/dev/null; exec /work/obj/scheduler/icecc-scheduler -p {SCHED_PORT} -n {NET} -l /scratch/farm/sched.log -vvv'",
       check=True)
    time.sleep(3)
    # one worker per F -- resolve_role runs per HOST (each host's own files
    # are distributed/preflighted independently; one host having a stale
    # or corrupted copy must not be masked by another host being fine).
    for i, h in enumerate(worker_hosts):
        wp = 12000 + i
        f_tree, f_img = resolve_role(h, binary_set_f, "F")
        docker_rm(h, "farm-worker")
        # chmod only the mickg-owned dir (not -R: stale daemon files are uid-999, unchmod-able by host user; rm clears them)
        sh(h, f"mkdir -p {SCRATCH}/farm/envs && chmod 1777 {SCRATCH}/farm && rm -f {SCRATCH}/farm/worker.log", check=True)
        sh(h,
           f"docker run -d --name farm-worker --network host -v {f_tree}:/work -v {SCRATCH}:/scratch -u 0:0 {f_img} "
           f"bash -c 'useradd -r -s /usr/sbin/nologin icecc 2>/dev/null; "
           f"chown icecc /scratch/farm/envs; "  # pre-chown as root: daemon's cleanup_cache runs post-drop (no CAP_CHOWN)
           f"exec /work/obj/daemon/iceccd -m 8 -s {sip}:{SCHED_PORT} -n {NET} -N {h}w -b /scratch/farm/envs "
           f"-p {wp} -l /scratch/farm/worker.log -vvv'",
           check=True)
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

def run_client(client_host, project_dir, mode, maxtu, jobs, prefer, binary_set_c=None):
    sched = f"{HOSTS[SCHED_HOST]['ip']}:{SCHED_PORT}"
    # S4: distribute+preflight (when a set is selected) before any docker
    # action for the client host, same as up()'s S/F roles.
    c_tree, c_img = resolve_role(client_host, binary_set_c, "C")
    for s in SCRIPTS:
        push_file(client_host, f"{HUB_DIR}/{s}", f"{SCRATCH.replace('~', '$HOME')}/{s}")
    # resolve ~ on the client for the bind (docker needs an absolute host path)
    docker_rm(client_host, "farm-client")
    cmd = (f"docker run --rm --name farm-client --network host -v {c_tree}:/work -v {SCRATCH}:/scratch -u 0:0 {c_img} "
           f"bash /scratch/farm_client.sh {sched} {NET} {project_dir} {mode} {maxtu} {jobs} {prefer}")
    print(f"TEST: client={client_host} project={project_dir} mode={mode} maxtu={maxtu} jobs={jobs} prefer={prefer} "
          f"C-set={binary_set_c or 'default'}({c_tree})")
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
    (never called from resolve_role()/up()/run_client(); those only ever
    preflight-check and refuse). Idempotent: a host already holding a
    byte-exact, hash-verified copy is left untouched and reported
    already-current; anything else (absent, partial, drifted, tampered) is
    (re)synced from q3's manifest-verified tar and re-verified after copy.
    q3 itself is skipped (it is the source of record). Exits nonzero if any
    (set, host) pair ends in a hard failure (tar unreadable/wrong hash on
    q3, transfer failure, or post-copy verification still failing)."""
    failures = []
    for binary_set in sets:
        if binary_set not in ROLE_SET_DIR:
            failures.append(f"{binary_set}: unknown binary set (choices: {sorted(ROLE_SET_DIR)})")
            continue
        for host in hosts:
            if host not in HOSTS:
                failures.append(f"{binary_set}/{host}: unknown host (choices: {sorted(HOSTS)})")
                continue
            err, status = distribute_role_artifacts(host, binary_set)
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
    ap.add_argument("--binary-set-S", dest="binary_set_s", default=None, choices=sorted(ROLE_SET_DIR),
                     help="scheduler role binary set (default: existing hardcoded TREE, unchanged)")
    ap.add_argument("--binary-set-C", dest="binary_set_c", default=None, choices=sorted(ROLE_SET_DIR),
                     help="client role binary set (default: existing hardcoded TREE, unchanged)")
    ap.add_argument("--binary-set-F", dest="binary_set_f", default=None, choices=sorted(ROLE_SET_DIR),
                     help="worker(F) role binary set, applied to every worker host (default: existing hardcoded TREE, unchanged)")
    a = ap.parse_args()
    workers = a.workers.split(",")
    prefer = (workers[0] + "w") if len(workers) == 1 else ""   # pin for 1 F; let scheduler balance for >1 F
    ok = False
    try:
        try:
            ok = up(workers, a.binary_set_s, a.binary_set_f)
        except RuntimeError as exc:
            # S4: distribute/preflight refused before any docker action for
            # the failing role/host -- report it as a launch refusal, not a
            # cluster-registration failure, and let the finally block's
            # down() run as a harmless no-op cleanup (nothing was started).
            print(f"REFUSED: {exc}")
            ok = False
        else:
            print("CLUSTER:", "REGISTERED-OK" if ok else "REGISTRATION-FAILED")
            if ok and a.phase == "up-test-down":
                try:
                    r = run_client(a.client, a.project, a.mode, a.maxtu, a.jobs, prefer, a.binary_set_c)
                except RuntimeError as exc:
                    print(f"REFUSED: {exc}")
                    ok = False
                else:
                    dump_worker_evidence(workers, r.stdout)
    finally:
        if a.phase in ("up-down", "up-test-down"):
            down(workers, a.client)
    sys.exit(0 if ok else 2)

# S4: guarded so role_tree()/HOSTS/ROLE_SET_DIR can be imported (e.g. by
# artifact_selection_test.sh) without triggering a live cluster deploy.
if __name__ == "__main__":
    main()
