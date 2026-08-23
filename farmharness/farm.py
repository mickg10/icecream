#!/usr/bin/env python3
"""farm.py -- single-command icecream cluster orchestrator (runs on the hub, drives hosts via SSH).
Phases: up (scheduler + one iceccd per F, --network host, SSD scratch) -> test -> down.
Down runs in a context manager so a crash still tears the cluster down.
This first cut proves cross-host registration; the test phase is layered on next."""
import subprocess, sys, time, argparse

IMG = "icecream/farm-node:ubuntu22-gcc11-boost174"
NET = "farmnet"
SCHED_PORT = 22000
TREE = "~/icecream-review/c9488d74"          # built P50 tree (same on every host)
SCRATCH = "~/farm-scratch"                    # host SSD scratch (bind target)

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

def up(worker_hosts):
    sip = HOSTS[SCHED_HOST]["ip"]
    print(f"UP: scheduler on {SCHED_HOST}({sip}):{SCHED_PORT}  workers={worker_hosts}")
    # scheduler (--network host so it binds the LAN IP). The daemon/scheduler drop privileges to
    # icecc at startup, so the -l log dir must be writable by that user => useradd icecc + 1777 dir.
    docker_rm(SCHED_HOST, "farm-sched")
    sh(SCHED_HOST, f"mkdir -p {SCRATCH}/farm && chmod 1777 {SCRATCH}/farm && rm -f {SCRATCH}/farm/sched.log", check=True)
    sh(SCHED_HOST,
       f"docker run -d --name farm-sched --network host -v {TREE}:/work -v {SCRATCH}:/scratch -u 0:0 {IMG} "
       f"bash -c 'useradd -r icecc 2>/dev/null; exec /work/obj/scheduler/icecc-scheduler -p {SCHED_PORT} -n {NET} -l /scratch/farm/sched.log -vvv'",
       check=True)
    time.sleep(3)
    # one worker per F
    for i, h in enumerate(worker_hosts):
        wp = 12000 + i
        docker_rm(h, "farm-worker")
        # chmod only the mickg-owned dir (not -R: stale daemon files are uid-999, unchmod-able by host user; rm clears them)
        sh(h, f"mkdir -p {SCRATCH}/farm/envs && chmod 1777 {SCRATCH}/farm && rm -f {SCRATCH}/farm/worker.log", check=True)
        sh(h,
           f"docker run -d --name farm-worker --network host -v {TREE}:/work -v {SCRATCH}:/scratch -u 0:0 {IMG} "
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

def run_client(client_host, project_dir, mode, maxtu, jobs, prefer):
    sched = f"{HOSTS[SCHED_HOST]['ip']}:{SCHED_PORT}"
    for s in SCRIPTS:
        push_file(client_host, f"{HUB_DIR}/{s}", f"{SCRATCH.replace('~', '$HOME')}/{s}")
    # resolve ~ on the client for the bind (docker needs an absolute host path)
    docker_rm(client_host, "farm-client")
    cmd = (f"docker run --rm --name farm-client --network host -v {TREE}:/work -v {SCRATCH}:/scratch -u 0:0 {IMG} "
           f"bash /scratch/farm_client.sh {sched} {NET} {project_dir} {mode} {maxtu} {jobs} {prefer}")
    print(f"TEST: client={client_host} project={project_dir} mode={mode} maxtu={maxtu} jobs={jobs} prefer={prefer}")
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

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--workers", default="research6", help="comma list of worker hosts")
    ap.add_argument("--client", default="q3", help="client host (runs the submitter + compiles)")
    ap.add_argument("--project", default="/scratch/fmt")
    ap.add_argument("--mode", default="simultaneous")
    ap.add_argument("--maxtu", default="0")
    ap.add_argument("--jobs", default="16")
    ap.add_argument("--phase", default="up-test-down", choices=["up","up-down","up-test-down"])
    a = ap.parse_args()
    workers = a.workers.split(",")
    prefer = (workers[0] + "w") if len(workers) == 1 else ""   # pin for 1 F; let scheduler balance for >1 F
    ok = False
    try:
        ok = up(workers)
        print("CLUSTER:", "REGISTERED-OK" if ok else "REGISTRATION-FAILED")
        if ok and a.phase == "up-test-down":
            r = run_client(a.client, a.project, a.mode, a.maxtu, a.jobs, prefer)
            dump_worker_evidence(workers, r.stdout)
    finally:
        if a.phase in ("up-down", "up-test-down"):
            down(workers, a.client)
    sys.exit(0 if ok else 2)

main()
