#!/usr/bin/env python3
"""
P29 sharded per-C->F-route replay, on the PHYSICAL two-direction sinks.

Same scheduler simulator as the GRZ replay (imported, not re-implemented, so the two
codecs are compared on identical routing).  One codec process per route = one persistent
encoder state per route, fed only that route's TUs in its dispatch order.  The score is
the SUM of every route's C->F sink; the F->C sinks are summed separately and never added.

Binding: the deployable one -- --literal-group-tus 1, so no frame spans a TU that has not
been dispatched (dispatch_lag_tus=0, asserted from the codec's own output).

KNOWN OVERSTATEMENT OF THE SHARDED RESULT, stated up front: running one codec process per
route also gives each route its OWN dictionary, so Region/Block ordinals are dense over
that route's subset.  Real icecream has ONE client keyspace across all routes, whose
ordinals are sparser.  This harness therefore UNDERSTATES the id cost of sharding; the
definition/literal cost it does measure is not affected.
"""
import argparse, json, os, re, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shardrun import schedule, resolve_cell, run, die, HOME

BIN = f"{HOME}/selbind/p29build/codec50-sink"
BASE = ("--z 3 --mixed-regions --byte-array-lines --direct-ordinals --compressed-blobs "
        "--blob-threads 4 --blob-lazy-fallback --mo-factor --s1-max-chain 1024 --blob-z 9 "
        "--blob-zstd-workers 2 --blob-zstd-job-mib 5 --blob-zstd-overlap-log 3 "
        "--stable-root-tags").split()


def encode_route(d, paths, replay=False):
    """One route = one codec process.  Returns (cf_bytes, fc_bytes, per-TU cf offsets)."""
    os.makedirs(d, exist_ok=True)
    man = f"{d}/man.txt"
    with open(man, "w") as f:
        f.write("".join(p + "\n" for p in paths))
    # pass 1 dumps this route's literal bytes; pass 2 plans one frame per TU
    run([BIN, "--manifest", man] + BASE + ["--mixed-dump-prefix", f"{d}/pl"])
    cmd = ([BIN, "--manifest", man] + BASE +
           ["--literal-group-prefix", f"{d}/pl", "--literal-group-tus", "1",
            "--literal-group-workers", "4", "--literal-group-skip-zstd10",
            "--literal-group-wire", f"{d}/lit",
            "--cf-sink", f"{d}/cf", "--fc-sink", f"{d}/fc", "--sink-curve", f"{d}/sink.tsv"])
    r = run(cmd)
    if "byte-exact=OK" not in r.stdout:
        die(f"{d}: not byte-exact")
    lag = re.search(r"dispatch_lag_tus=(\d+)", r.stdout)
    if not lag or lag.group(1) != "0":
        die(f"{d}: dispatch lag is not zero")
    if replay:
        # second, stream-driven pass: every frame the run consumes must come back off the
        # files it just wrote, in order, byte-identical, with nothing left over
        v = run(cmd + ["--sink-replay"])
        if "SINK REPLAY OK" not in v.stdout or "byte-exact=OK" not in v.stdout:
            die(f"{d}: replay did not confirm")
    off = [int(l.split("\t")[2]) for l in open(f"{d}/sink.tsv").read().splitlines()[1:]]
    fcoff = [int(l.split("\t")[3]) for l in open(f"{d}/sink.tsv").read().splitlines()[1:]]
    if len(off) != len(paths):
        die(f"{d}: {len(off)} sink rows != {len(paths)} TUs")
    cf, fc = os.path.getsize(f"{d}/cf"), os.path.getsize(f"{d}/fc")
    if off[-1] != cf or fcoff[-1] != fc:
        die(f"{d}: final sink offset differs from the file size")
    return cf, fc, off


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("proj"); ap.add_argument("prof")
    ap.add_argument("--workers", default="1,4,8,16,32")
    ap.add_argument("--policies", default="sticky,rr,shuf")
    ap.add_argument("--builds", type=int, default=4)
    ap.add_argument("--work", default=f"{HOME}/selbind/p29shard/w")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    cell = f"{a.proj}.{a.prof}"
    work = os.path.join(a.work, cell); os.makedirs(work, exist_ok=True)
    files, prov = resolve_cell(a.proj, a.prof, work)
    n = len(files)
    print(f"[{cell}] {n} TUs/build x {a.builds}  payload={prov['payload']} sha=OK", flush=True)

    rows = []
    for policy in a.policies.split(","):
        for W in [int(x) for x in a.workers.split(",")]:
            t0 = time.time()
            routes = {}
            for b, i, w in schedule(n, a.builds, W, policy):
                routes.setdefault(w, []).append((b, i))
            cdir = os.path.join(work, f"{policy}.W{W}")
            if os.path.isdir(cdir): shutil.rmtree(cdir)
            cf_tot = fc_tot = 0
            per_build = [0] * a.builds
            replays = 0
            for w in sorted(routes):
                seq = routes[w]
                # the first route of every config is additionally replayed frame-by-frame
                rep = (w == min(routes))
                cf, fc, off = encode_route(f"{cdir}/r{w}", [files[i] for _, i in seq], replay=rep)
                replays += int(rep)
                cf_tot += cf; fc_tot += fc
                last = {}
                for k, (b, _) in enumerate(seq): last[b] = k
                prev = 0
                for b in range(a.builds):
                    if b in last:
                        per_build[b] += off[last[b]] - prev; prev = off[last[b]]
            shutil.rmtree(cdir)
            rows.append(dict(cell=cell, policy=policy, W=W, routes=len(routes), n_per_build=n,
                             cf_total=cf_tot, fc_total=fc_tot,
                             **{f"build{b+1}": per_build[b] for b in range(a.builds)},
                             rr_degenerate=int(policy == "rr" and n % W == 0),
                             replays_verified=replays, secs=round(time.time() - t0, 1)))
            print(f"  {policy}.W{W:<3} routes={len(routes):<3} cf={cf_tot:<11} fc={fc_tot:<9} "
                  f"builds={per_build}  replay_ok={replays}  {time.time()-t0:.0f}s", flush=True)

    with open(a.out, "w") as f:
        keys = list(rows[0]); f.write("\t".join(keys) + "\n")
        for r in rows: f.write("\t".join(str(r[k]) for k in keys) + "\n")
    print(f"WROTE {a.out}", flush=True)


if __name__ == "__main__":
    main()
