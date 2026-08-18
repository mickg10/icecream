#!/usr/bin/env python3
"""
Sharded per-C->F-route GRZ2 replay.

Why a single corpus-order stream is not enough
----------------------------------------------
Real icecream shards: the client sends each TU as an individual job to a SELECTED
remote worker.  With workers Fa/Fb/Fc the dispatched sequences look like
Fa: 1,6,7,9 / Fb: 2,4,10 / Fc: 3,5...  Each C->F route is a separate persistent
stream carrying only ITS subset of TUs.  So "--gtu 1 on one corpus-order stream"
is STILL a batch bound: it assumes every TU reaches one receiver, in order.

This harness builds one persistent encoder state PER ROUTE, closes a complete
frame at every scheduled TU (--gtu 1), and charges the SUM of every route stream.

Gates (each is enforced; a failure aborts the config, it is never published):
  G1  one complete frame per scheduled TU on that route
  G2  the whole route stream decodes byte-exact from its own bytes alone
  G3  prefix immutability: re-encoding the route's build-1..k subset reproduces
      the leading bytes of the full route stream EXACTLY (byte-for-byte)
  G4  immediate decodability: truncating the route stream at frame j's physical
      end offset still reconstructs TUs 0..j on that route -- no frame depends
      on a TU that has not been dispatched yet
"""
import argparse, hashlib, json, os, random, shutil, subprocess, sys, time

HOME = os.path.expanduser("~")
GRZ = f"{HOME}/selbind/grzflush/grz2g-flush"
MATRIX = f"{HOME}/ictmp/ii-matrix"
# the binding, with the ONLY change being the per-TU close
GRZP = "-m g2 -K 256 -s 6 -t 21 -l 4 -k 5 -b 8 --graw 512 --gadd 128 --hist 1024 -j 8".split()
END_FRAME = 36  # bytes appended after the last group's end_offset by a complete encode


def die(msg):
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        die(f"command failed ({r.returncode}): {' '.join(map(str, cmd))}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
    return r


def run_timed(cmd):
    """run under /usr/bin/time -v, return (result, peak_rss_kb)."""
    r = subprocess.run(["/usr/bin/time", "-v"] + [str(c) for c in cmd],
                       capture_output=True, text=True)
    if r.returncode != 0:
        die(f"command failed ({r.returncode}): {' '.join(map(str, cmd))}\n{r.stdout[-2000:]}\n{r.stderr[-3000:]}")
    rss = None
    for ln in r.stderr.splitlines():
        if "Maximum resident set size" in ln:
            rss = int(ln.rsplit(":", 1)[1].strip())
    if rss is None:
        die("could not read peak RSS from /usr/bin/time -v")
    return r, rss


# ---------------------------------------------------------------- corpus ----
def resolve_cell(proj, prof, work):
    """Extract + sha-verify the payload; return (list of .ii paths, provenance dict)."""
    d = os.path.join(work, "src")
    j = os.path.join(MATRIX, proj, prof, "corpus.json")
    if not os.path.isfile(j):
        die(f"no corpus.json for {proj}/{prof}")
    meta = json.load(open(j))
    rel, sha, tus = meta["payload"]["path"], meta["payload"]["sha256"], meta["tu_count"]
    arc = os.path.join(MATRIX, proj, prof, rel)
    if not os.path.isfile(arc):
        die(f"payload {rel} missing")
    h = hashlib.sha256()
    with open(arc, "rb") as f:
        for blk in iter(lambda: f.read(1 << 22), b""):
            h.update(blk)
    if h.hexdigest() != sha:
        die(f"payload sha mismatch for {proj}/{prof}")
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    # plain `tar --zstd` refuses these frames ("window size larger than maximum")
    p1 = subprocess.Popen(["zstd", "-d", "--long=31", "-c", arc], stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL)
    p2 = subprocess.Popen(["tar", "-xf", "-", "-C", d], stdin=p1.stdout)
    p1.stdout.close()
    if p2.wait() != 0 or p1.wait() != 0:
        die("extract failed")
    files = sorted(os.path.join(r, f) for r, _, fs in os.walk(d) for f in fs if f.endswith(".ii"))
    if len(files) != tus:
        die(f"extracted {len(files)} .ii != corpus.json tu_count {tus}")
    return files, {"payload": rel, "sha256": sha, "tu_count": tus}


# -------------------------------------------------------------- routing ----
def schedule(n, builds, W, policy, seed=12345):
    """Declared deterministic scheduler simulator.

    Returns dispatch[] of (build_index, tu_index_within_build, worker).  Dispatch
    order is the order C hands jobs out; each worker's subsequence is that
    worker's stream order, which is what the encoder must preserve.

      sticky  the same source TU always lands on the same worker in every build
              (w = tu_index % W).  Maximum cross-build affinity.
      rr      strict round robin over the global dispatch order (w = t % W).
              Degenerates to sticky whenever n % W == 0 -- reported, not hidden.
      shuf    per-build deterministic shuffle, then round robin over the shuffled
              order: balanced load, no cross-build identity affinity.  Models a
              load-balanced dispatcher handing each TU to whichever F frees first.
    """
    rng = random.Random(seed)
    out, t = [], 0
    for b in range(builds):
        order = list(range(n))
        if policy == "shuf":
            rng.shuffle(order)
        for pos, i in enumerate(order):
            if policy == "sticky":
                w = i % W
            elif policy == "rr":
                w = t % W
            elif policy == "shuf":
                w = pos % W
            else:
                die(f"unknown policy {policy}")
            out.append((b, i, w))
            t += 1
    return out


def read_curve(path):
    rows = []
    with open(path) as f:
        hdr = f.readline().rstrip("\n").split("\t")
        ix = {k: i for i, k in enumerate(hdr)}
        for ln in f:
            p = ln.rstrip("\n").split("\t")
            rows.append({k: p[i] for k, i in ix.items()})
    return rows


def encode_route(work, tag, paths, curve_needed=True, gtu=1, build_tus=None):
    """Encode one persistent route stream.  Returns (grz, blob, curve rows, rss)."""
    d = os.path.join(work, tag)
    os.makedirs(d, exist_ok=True)
    man, blob, tum = f"{d}/man.txt", f"{d}/blob", f"{d}/tu.map"
    grz, curve = f"{d}/out.grz", f"{d}/curve.tsv"
    with open(man, "w") as f:
        f.write("".join(p + "\n" for p in paths))
    with open(blob, "wb") as out:
        for p in paths:
            with open(p, "rb") as src:
                shutil.copyfileobj(src, out, 1 << 22)
    run([GRZ, "tu", man, tum])
    cmd = [GRZ, "enc", blob, grz, "-u", tum] + GRZP + ["--gtu", str(gtu)]
    if build_tus:
        cmd += ["--build-tus", str(build_tus)]
    if curve_needed:
        cmd += ["--curve", curve]
    _, rss = run_timed(cmd)
    return grz, blob, (read_curve(curve) if curve_needed else None), rss


def batch_bound(work, files, n, builds):
    """The reference this replaces: ONE corpus-order stream at the --gtu 112 binding,
    with --build-tus so each build still closes.  It is a BOUND, not a transport --
    a group spans up to 112 TUs, so a frame is only complete once TUs that have not
    been dispatched yet have arrived at one receiver, in order."""
    d = os.path.join(work, "batch112")
    if os.path.isdir(d):
        shutil.rmtree(d)
    paths = [files[i] for _ in range(builds) for i in range(n)]
    grz, blob, curve, rss = encode_route(d, "s", paths, gtu=112, build_tus=n)
    size = os.path.getsize(grz)
    dec = f"{d}/s/dec.bin"
    _, drss = run_timed([GRZ, "dec", grz, dec, "-j", "8"])
    if subprocess.run(["cmp", "-s", dec, blob]).returncode != 0:
        die("batch112: full decode differs from input")
    eo = [int(r["end_offset"]) for r in curve]
    closes = [int(r["end_offset"]) for r in curve if r["closed_by"] == "build"]
    if len(closes) != builds:
        die(f"batch112: {len(closes)} build closes != {builds}")
    if size != eo[-1] + END_FRAME:
        die("batch112: filesize != last end_offset + END frame")
    per = []
    prev = 0
    for c in closes:
        per.append(c - prev)
        prev = c
    per[-1] += END_FRAME
    frames = len(curve)
    shutil.rmtree(d)
    return dict(wire_total=size, per_build=per, frames=frames, rss_kb=rss,
                dec_rss_kb=drss)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("proj")
    ap.add_argument("prof")
    ap.add_argument("--workers", default="1,4,8,16,32")
    ap.add_argument("--policies", default="sticky,rr,shuf")
    ap.add_argument("--builds", type=int, default=4)
    ap.add_argument("--work", default=f"{HOME}/selbind/shard/w")
    ap.add_argument("--out", required=True)
    ap.add_argument("--sweep", choices=["full", "sample"], default="sample",
                    help="G4 immediate-decodability: every frame, or boundaries+stride")
    ap.add_argument("--stride", type=int, default=8)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()

    cell = f"{a.proj}.{a.prof}"
    work = os.path.join(a.work, cell)
    os.makedirs(work, exist_ok=True)
    files, prov = resolve_cell(a.proj, a.prof, work)
    n = len(files)
    print(f"[{cell}] {n} TUs/build x {a.builds} builds  payload={prov['payload']} sha=OK", flush=True)

    rows, gate_rows = [], []
    bb = batch_bound(work, files, n, a.builds)
    rows.append(dict(cell=cell, policy="batch112", W=0, routes=1, n_per_build=n,
                     builds=a.builds, total_tus=n * a.builds,
                     wire_total=bb["wire_total"],
                     **{f"build{b+1}": bb["per_build"][b] for b in range(a.builds)},
                     rss_sum_kb=bb["rss_kb"], rss_max_kb=bb["rss_kb"],
                     dec_rss_max_kb=bb["dec_rss_kb"], reverse_bytes=0,
                     rr_degenerate=0, route_tus_min=n * a.builds,
                     route_tus_max=n * a.builds, secs=0))
    print(f"  {'batch112':>12}  routes=1   wire={bb['wire_total']:<12} "
          f"builds={bb['per_build']}  frames={bb['frames']} (BOUND, not a transport)",
          flush=True)
    for policy in a.policies.split(","):
        for W in [int(x) for x in a.workers.split(",")]:
            t0 = time.time()
            disp = schedule(n, a.builds, W, policy)
            routes = {}
            for b, i, w in disp:
                routes.setdefault(w, []).append((b, i))
            degenerate = (policy == "rr" and n % W == 0)
            cfg = f"{policy}.W{W}"
            cdir = os.path.join(work, cfg)
            if os.path.isdir(cdir):
                shutil.rmtree(cdir)
            os.makedirs(cdir)

            tot_bytes = 0
            per_build = [0] * a.builds
            rss_sum, rss_max = 0, 0
            dec_rss_max = 0
            g1 = g2 = g3 = g4 = 0
            g4_points = 0
            for w in sorted(routes):
                seq = routes[w]
                paths = [files[i] for _, i in seq]
                grz, blob, curve, rss = encode_route(cdir, f"r{w}", paths)
                rss_sum += rss
                rss_max = max(rss_max, rss)
                size = os.path.getsize(grz)
                tot_bytes += size

                # --- G1: exactly one complete frame per scheduled TU -------------
                if len(curve) != len(seq):
                    die(f"{cfg} r{w}: {len(curve)} frames != {len(seq)} scheduled TUs")
                for gi, row in enumerate(curve):
                    if int(row["group"]) != gi or int(row["tu_hi"]) - int(row["tu_lo"]) != 1:
                        die(f"{cfg} r{w}: frame {gi} does not cover exactly one TU")
                eo = [int(r["end_offset"]) for r in curve]
                if eo != sorted(eo) or len(set(eo)) != len(eo):
                    die(f"{cfg} r{w}: end offsets not strictly increasing")
                if size != eo[-1] + END_FRAME:
                    die(f"{cfg} r{w}: filesize {size} != last end_offset {eo[-1]} + END {END_FRAME}")
                g1 += 1

                # --- per-build split from PHYSICAL offsets in the one stream -----
                last_of_build = {}
                for k, (b, _) in enumerate(seq):
                    last_of_build[b] = k
                prev = 0
                for b in range(a.builds):
                    if b in last_of_build:
                        cut = eo[last_of_build[b]]
                        per_build[b] += cut - prev
                        prev = cut
                per_build[a.builds - 1] += END_FRAME  # final close charged to the last build

                # --- G2: full decode from this route's own bytes ----------------
                dec = f"{cdir}/r{w}/dec.bin"
                _, drss = run_timed([GRZ, "dec", grz, dec, "-j", "8"])
                dec_rss_max = max(dec_rss_max, drss)
                if subprocess.run(["cmp", "-s", dec, blob]).returncode != 0:
                    die(f"{cfg} r{w}: full decode differs from the route input")
                os.remove(dec)
                g2 += 1

                # --- G3: prefix immutability on THIS stream ---------------------
                # re-encode the route's build-1..k subset; it must reproduce the
                # leading bytes of the full stream byte-for-byte
                for k in range(a.builds - 1):
                    if k not in last_of_build:
                        continue
                    sub = [files[i] for b, i in seq if b <= k]
                    if not sub:
                        continue
                    sgrz, sblob, scurve, _ = encode_route(cdir, f"r{w}.p{k}", sub, curve_needed=True)
                    cut = eo[last_of_build[k]]
                    ssize = os.path.getsize(sgrz)
                    if ssize != int(scurve[-1]["end_offset"]) + END_FRAME:
                        die(f"{cfg} r{w}: sub-encode p{k} filesize/END mismatch")
                    if int(scurve[-1]["end_offset"]) != cut:
                        die(f"{cfg} r{w}: sub-encode p{k} ends at {scurve[-1]['end_offset']}, "
                            f"full stream boundary is {cut}")
                    with open(sgrz, "rb") as f:
                        sb = f.read(cut)
                    with open(grz, "rb") as f:
                        fb = f.read(cut)
                    if sb != fb:
                        first = next(i for i in range(cut) if sb[i] != fb[i])
                        die(f"{cfg} r{w}: build-{k+1} prefix NOT immutable (diverges at byte {first})")
                    shutil.rmtree(f"{cdir}/r{w}.p{k}")
                    g3 += 1  # counts (route, build-boundary) checks that passed

                # --- G4: immediate decodability of every dispatched frame -------
                tu_off = [0]
                for p in paths:
                    tu_off.append(tu_off[-1] + os.path.getsize(p))
                if a.sweep == "full":
                    pts = list(range(len(seq)))
                else:
                    pts = sorted(set([0, len(seq) - 1] + list(last_of_build.values()) +
                                     list(range(0, len(seq), max(1, a.stride)))))
                trunc = f"{cdir}/r{w}/trunc.grz"
                outp = f"{cdir}/r{w}/pfx.bin"
                with open(grz, "rb") as f:
                    whole = f.read()
                with open(blob, "rb") as f:
                    wblob = f.read()
                for jx in pts:
                    with open(trunc, "wb") as f:
                        f.write(whole[:eo[jx]])
                    run([GRZ, "decprefix", trunc, outp, "-g", str(jx + 1), "-j", "8"])
                    with open(outp, "rb") as f:
                        got = f.read()
                    if got != wblob[:tu_off[jx + 1]]:
                        die(f"{cfg} r{w}: frame {jx} does not reconstruct TUs 0..{jx} "
                            f"from its own truncated stream")
                    g4_points += 1
                os.remove(trunc)
                os.remove(outp)
                g4 += 1
                os.remove(blob)
                if not a.keep:
                    os.remove(grz)

            rows.append(dict(cell=cell, policy=policy, W=W, routes=len(routes),
                             n_per_build=n, builds=a.builds, total_tus=n * a.builds,
                             wire_total=tot_bytes,
                             **{f"build{b+1}": per_build[b] for b in range(a.builds)},
                             rss_sum_kb=rss_sum, rss_max_kb=rss_max,
                             dec_rss_max_kb=dec_rss_max,
                             reverse_bytes=0,
                             rr_degenerate=int(degenerate),
                             route_tus_min=min(len(v) for v in routes.values()),
                             route_tus_max=max(len(v) for v in routes.values()),
                             secs=round(time.time() - t0, 1)))
            gate_rows.append(dict(cell=cell, policy=policy, W=W, routes=len(routes),
                                  g1_frames_per_tu=g1, g2_decode_exact=g2,
                                  g3_prefix_immutable=g3, g4_immediate_routes=g4,
                                  g4_points=g4_points, sweep=a.sweep))
            print(f"  {cfg:>12}  routes={len(routes):<3} wire={tot_bytes:<12} "
                  f"builds={per_build}  rssSum={rss_sum//1024}MB  "
                  f"gates {g1}/{g2}/{g3}/{g4} of {len(routes)} ({g4_points} pts)  "
                  f"{time.time()-t0:.0f}s", flush=True)
            if not a.keep:
                shutil.rmtree(cdir)

    with open(a.out, "w") as f:
        keys = list(rows[0])
        f.write("\t".join(keys) + "\n")
        for r in rows:
            f.write("\t".join(str(r[k]) for k in keys) + "\n")
    with open(a.out.replace(".tsv", "") + ".gates.tsv", "w") as f:
        keys = list(gate_rows[0])
        f.write("\t".join(keys) + "\n")
        for r in gate_rows:
            f.write("\t".join(str(r[k]) for k in keys) + "\n")
    with open(a.out.replace(".tsv", "") + ".prov.json", "w") as f:
        json.dump(dict(cell=cell, **prov, binding=" ".join(GRZP) + " --gtu 1",
                       seed=12345, sweep=a.sweep), f, indent=1)
    print(f"WROTE {a.out}", flush=True)


if __name__ == "__main__":
    main()
