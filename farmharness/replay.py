#!/usr/bin/env python3
"""Per-cell replayer (runs INSIDE the pinned container).
Reads a cmake compile_commands.json (inherently per-TU, compile-only -> no link),
and for each TU: builds it REMOTE through icecc AND locally with the same container g++,
asserts the objects are BYTE-IDENTICAL, that the build was actually REMOTE, and measures.
Modes: full (each TU once), sequence (cold then warm), simultaneous (concurrent, contention)."""
import hashlib, json, os, re, shlex, subprocess, sys, time, tempfile
from concurrent.futures import ThreadPoolExecutor

top   = os.environ["top"]                 # /work/obj (daemon+client binaries)
work  = os.environ["work"]
sockd = os.environ["sockdir"]
CC    = os.environ["CC"]
MODE  = os.environ.get("MODE", "full")
MAXTU = int(os.environ.get("MAXTU", "0")) # 0 = all
JOBS  = int(os.environ.get("JOBS", "8"))  # concurrency for simultaneous
PREFER = os.environ.get("PREFER", "remoteq")  # preferred remote F node name (ICECC_PREFERRED_HOST)
GXX   = "/usr/bin/g++"
ICECC = os.path.join(top, "client", "icecc")

def load_tus():
    ents = json.load(open(CC))
    tus = []
    for e in ents:
        d = e["directory"]
        src = e["file"]
        args = e["arguments"] if "arguments" in e else shlex.split(e["command"])
        # drop compiler(arg0), the -o <out>, the -c, and the source itself; keep all real flags
        flags, i = [], 1
        while i < len(args):
            a = args[i]
            if a == "-o":            i += 2; continue
            if a == "-c":            i += 1; continue
            if a in (src, os.path.abspath(src)) or os.path.abspath(os.path.join(d, a)) == os.path.abspath(os.path.join(d, src)):
                i += 1; continue
            flags.append(a); i += 1
        # only C/C++ TUs
        if src.endswith((".c", ".cc", ".cpp", ".cxx", ".C")):
            tus.append({"dir": d, "src": src, "flags": flags})
    if MAXTU: tus = tus[:MAXTU]
    return tus

def build_one(idx, tu):
    d, src, flags = tu["dir"], tu["src"], tu["flags"]
    # C sources (.c) must use gcc; C++ sources use g++. The daemon env bundles both backends.
    comp = "/usr/bin/gcc" if src.endswith(".c") else GXX
    ro = f"{work}/o/r_{idx}.o"; lo = f"{work}/o/l_{idx}.o"; clog = f"{work}/o/c_{idx}.log"
    renv = dict(os.environ,
                ICECC_TEST_SOCKET=f"{sockd}/local", ICECC_TEST_REMOTEBUILD="1",
                ICECC_CARET_WORKAROUND="0",   # don't local-recompile just to reproduce warning stderr; object stays byte-exact
                ICECC_DEBUG="debug", ICECC_LOGFILE=clog)
    if PREFER:   # single-F: pin; multi-F: leave unset so the scheduler balances across endpoints
        renv["ICECC_PREFERRED_HOST"] = PREFER
    t0 = time.monotonic()
    rp = subprocess.run([ICECC, comp, *flags, "-c", src, "-o", ro],
                        cwd=d, env=renv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=600)
    rt = time.monotonic() - t0
    lp = subprocess.run([comp, *flags, "-c", src, "-o", lo],
                        cwd=d, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=600)
    clue = ""
    try: clue = open(clog, errors="ignore").read()
    except FileNotFoundError: pass
    # honest remote proof (local-oracle gate #8): require POSITIVE remote evidence, not just absence of a local marker.
    local_fb = any(m in clue for m in ("<building_local>", "building myself", "local build forced", "can't determine native environment"))
    positive = ("wait for cs" in clue) or bool(re.search(r"got \d+ bytes", clue)) or ("Verified host" in clue)
    remote   = positive and not local_fb
    # canonical per-TU join fields (local-oracle bijection table): the assignment line carries BOTH
    # the selected F endpoint and the scheduler Job ID: "Have to use host 10.0.27.56:12000 - Job ID: 5"
    am = re.search(r"Have to use host ([0-9.]+):(\d+) - Job ID: (\d+)", clue)
    fhost = am.group(1) if am else ("remote" if remote else "LOCAL")
    jobid = int(am.group(3)) if am else -1
    wm = re.search(r"got (\d+) bytes", clue)              # F->C result payload (object) bytes, per-job, client-reported
    wire = int(wm.group(1)) if wm else 0
    rbytes = open(ro, "rb").read() if os.path.exists(ro) else b""
    lbytes = open(lo, "rb").read() if os.path.exists(lo) else b""
    exact  = (rp.returncode == 0 and lp.returncode == 0 and rbytes != b"" and rbytes == lbytes)
    osha = hashlib.sha256(rbytes).hexdigest()[:16] if rbytes else "-"
    envdig = os.path.basename(os.environ.get("ICECC_VERSION", "-")).split(".")[0]
    osz = len(rbytes)
    err = "" if (exact and remote) else (rp.stderr or lp.stderr or b"").decode(errors="ignore").strip().splitlines()[:1]
    # one canonical row per TU: identity, JobID, endpoint, acceptance, object digest, mode, env digest
    print(f"JOINROW idx={idx} src={os.path.basename(src)} jobid={jobid} f={fhost} "
          f"accepted={int(rp.returncode==0 and remote)} exact={int(exact)} sha={osha} mode={'remote' if remote else 'LOCAL'} env={envdig}", flush=True)
    return {"idx": idx, "src": os.path.basename(src), "exact": exact, "remote": remote,
            "rc": rp.returncode, "lrc": lp.returncode, "sec": rt, "osz": osz,
            "fhost": fhost, "jobid": jobid, "wire": wire, "err": err}

def run_pass(tus, concurrent):
    os.makedirs(f"{work}/o", exist_ok=True)
    t0 = time.monotonic()
    if concurrent:
        with ThreadPoolExecutor(max_workers=JOBS) as ex:
            res = list(ex.map(lambda p: build_one(*p), enumerate(tus)))
    else:
        res = [build_one(i, tu) for i, tu in enumerate(tus)]
    wall = time.monotonic() - t0
    ok  = sum(1 for r in res if r["exact"] and r["remote"])
    rem = sum(1 for r in res if r["remote"])
    bad = [r for r in res if not (r["exact"] and r["remote"])]
    return res, ok, rem, wall, bad

def report(tag, res, ok, rem, wall):
    n = len(res)
    jps = n / wall if wall > 0 else 0
    wire = sum(r["wire"] for r in res)
    fh = {}
    for r in res:
        if r["remote"]: fh[r["fhost"]] = fh.get(r["fhost"], 0) + 1
    fhs = " ".join(f"{k}:{v}" for k, v in sorted(fh.items())) or "-"
    bad = [r for r in res if not (r["exact"] and r["remote"])]
    print(f"CELL[{tag}]: byte-exact-remote {ok}/{n}  remote {rem}/{n}  wall {wall:.1f}s  jobs/s {jps:.2f}  F->C-wire {wire}B  endpoints[{fhs}]")
    for r in bad[:6]:
        print(f"    BAD {r['src']}  exact={r['exact']} remote={r['remote']} fhost={r['fhost']} rc={r['rc']} lrc={r['lrc']}  {r['err']}")
    return ok == n and n > 0

def main():
    tus = load_tus()
    print(f"CELL: project TUs={len(tus)} mode={MODE} jobs={JOBS}")
    if not tus: print("CELL-FAIL: no TUs in compile_commands.json"); sys.exit(1)
    allpass = True
    if MODE in ("full", "sequence"):
        res, ok, rem, wall, bad = run_pass(tus, concurrent=False)
        allpass &= report("full/cold", res, ok, rem, wall)
    if MODE == "sequence":
        res, ok, rem, wall, bad = run_pass(tus, concurrent=False)
        allpass &= report("seq/warm", res, ok, rem, wall)
    if MODE == "simultaneous":
        res, ok, rem, wall, bad = run_pass(tus, concurrent=True)
        allpass &= report(f"simul/j{JOBS}", res, ok, rem, wall)
    print("CELL: PASS" if allpass else "CELL: FAIL")
    sys.exit(0 if allpass else 2)

main()
