#!/usr/bin/env python3
"""Build the sharded-replay tables straight from the harness TSVs.

Every number in the report is read from a file the harness wrote.  Nothing is
transcribed by hand, and any cell the harness did not produce is printed as a gap
rather than filled in.
"""
import os, sys, glob

# both default to the committed evidence directory beside this script
_HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sharded-deployable")
GRZ = sys.argv[1] if len(sys.argv) > 1 else _HERE
P29 = sys.argv[2] if len(sys.argv) > 2 else _HERE


def load(pat):
    out = []
    for p in sorted(glob.glob(pat)):
        rows = open(p).read().splitlines()
        if len(rows) < 2:
            continue
        hdr = rows[0].split("\t")
        for r in rows[1:]:
            v = r.split("\t")
            d = dict(zip(hdr, v))
            for k in list(d):
                if k not in ("cell", "policy", "sweep"):
                    try: d[k] = int(d[k])
                    except ValueError:
                        try: d[k] = float(d[k])
                        except ValueError: pass
            out.append(d)
    return out


def get(rows, cell, policy, W, key):
    for r in rows:
        if r["cell"] == cell and r["policy"] == policy and r["W"] == W:
            return r.get(key)
    return None


def main():
    g = load(f"{GRZ}/*.grz.tsv")
    p = load(f"{P29}/*.p29shard.tsv")
    if not g and not p:
        sys.exit("no GRZ (*.grz.tsv) or P29 (*.p29shard.tsv) rows found")
    cells = sorted({r["cell"] for r in g} | {r["cell"] for r in p})
    Ws = sorted({r["W"] for r in g + p if r["W"]})
    if not g:
        print("_(GRZ tables omitted: no `*.grz.tsv` in the evidence directory.)_\n")
    if not p:
        print("_(P29 sharded tables omitted: no `*.p29shard.tsv` in the evidence directory.)_\n")

    print("### A. What per-TU closure costs, before any sharding")
    print("One stream, corpus order. `batch112` is the old number and is a BOUND: a group")
    print("spans up to 112 TUs, so its frame is only complete once TUs that have not been")
    print("dispatched yet have arrived at one receiver, in order.\n")
    print("| cell | TUs/build | GRZ2 batch112 (bound) | GRZ2 gtu=1 | per-TU cost | P29 lg112 (bound) | P29 lg1 | per-TU cost |")
    print("|---|--:|--:|--:|--:|--:|--:|--:|")
    for c in cells:
        n = get(g, c, "sticky", 1, "n_per_build")
        bb = get(g, c, "batch112", 0, "wire_total")
        t1 = get(g, c, "sticky", 1, "wire_total")
        pc = f"+{100*(t1/bb-1):.1f}%" if bb and t1 else "—"
        print(f"| {c.split('.')[0]} | {n} | {bb:,} | {t1:,} | {pc} | (see B) | {get(p,c,'sticky',1,'cf_total'):,} | (see B) |"
              if bb and t1 and get(p, c, "sticky", 1, "cf_total") else f"| {c.split('.')[0]} | {n} | — | — | — | — | — | — |")

    print("\n### B. What SHARDING costs on top — cold build only (sticky routing)")
    print("Sum of every route's stream. GRZ2 = total wire; P29 = C→F primary only.\n")
    hdr = " | ".join(f"W={w}" for w in Ws)
    print(f"| cell | codec | {hdr} |")
    print("|---|---|" + "--:|" * len(Ws))
    for c in cells:
        for name, rows, key in (("GRZ2", g, "build1"), ("P29 C→F", p, "build1")):
            base = get(rows, c, "sticky", 1, key)
            cellsout = []
            for w in Ws:
                v = get(rows, c, "sticky", w, key)
                cellsout.append(f"{v:,} ({v/base:.2f}×)" if v and base else "—")
            print(f"| {c.split('.')[0]} | {name} | " + " | ".join(cellsout) + " |")

    print("\n### C. Stickiness is a REPORTED input — warm rebuild cost (build 2)")
    print("`rr_deg` marks a configuration where round robin degenerates to sticky because")
    print("n % W == 0; those rows are identical by construction, not by luck.\n")
    print("| cell | codec | policy | " + hdr + " |")
    print("|---|---|---|" + "--:|" * len(Ws))
    for c in cells:
        for name, rows, key in (("GRZ2", g, "build2"), ("P29 C→F", p, "build2")):
            for pol in ("sticky", "rr", "shuf"):
                out = []
                for w in Ws:
                    v = get(rows, c, pol, w, key)
                    deg = get(rows, c, pol, w, "rr_degenerate")
                    out.append(("—" if v is None else f"{v:,}" + ("*" if deg else "")))
                if any(o != "—" for o in out):
                    print(f"| {c.split('.')[0]} | {name} | {pol} | " + " | ".join(out) + " |")
    print("\n`*` = rr degenerate (n % W == 0), identical to sticky by construction.")

    print("\n### D. Memory — the cost of holding one encoder state per route")
    print("Peak RSS summed over routes (what C holds if every route is live), and the")
    print("largest single per-F decoder RSS.\n")
    print("| cell | " + hdr + " | max per-F decode RSS |")
    print("|---|" + "--:|" * len(Ws) + "--:|")
    for c in cells:
        out = []
        for w in Ws:
            v = get(g, c, "sticky", w, "rss_sum_kb")
            out.append(f"{v//1024:,} MB" if v else "—")
        d = get(g, c, "sticky", max(Ws), "dec_rss_max_kb")
        print(f"| {c.split('.')[0]} | " + " | ".join(out) + f" | {d//1024:,} MB |" if d else
              f"| {c.split('.')[0]} | " + " | ".join(out) + " | — |")

    print("\n### E. Reverse direction, reported separately and never added in")
    print("GRZ has no F→C codec channel at all. P29 does, and it GROWS with sharding:")
    print("every F independently asks for what it lacks.\n")
    print("| cell | " + " | ".join(f"{pol} W={w}" for pol in ("sticky", "rr") for w in (1, max(Ws))) + " |")
    print("|---|" + "--:|" * 4)
    for c in cells:
        out = []
        for pol in ("sticky", "rr"):
            for w in (1, max(Ws)):
                v = get(p, c, pol, w, "fc_total")
                out.append(f"{v:,}" if v else "—")
        print(f"| {c.split('.')[0]} | " + " | ".join(out) + " |")

    print("\n### F. Gate ledger (GRZ) — configurations, not spot checks")
    tot = {}
    for p2 in sorted(glob.glob(f"{GRZ}/*.grzgates.tsv")):
        rows = open(p2).read().splitlines()
        hdr = rows[0].split("\t")
        for r in rows[1:]:
            d = dict(zip(hdr, r.split("\t")))
            for k in ("routes", "g1_frames_per_tu", "g2_decode_exact", "g3_prefix_immutable",
                      "g4_immediate_routes", "g4_points"):
                tot[k] = tot.get(k, 0) + int(d[k])
            tot["configs"] = tot.get("configs", 0) + 1
    if tot:
        print(f"- configurations: {tot['configs']}, routes gated: {tot['routes']}")
        print(f"- G1 one frame per scheduled TU: {tot['g1_frames_per_tu']}/{tot['routes']} routes")
        print(f"- G2 whole route stream decodes byte-exact: {tot['g2_decode_exact']}/{tot['routes']} routes")
        print(f"- G3 per-route prefix immutability: {tot['g3_prefix_immutable']}/{3*tot['routes']} (route × build boundary)")
        print(f"- G4 immediate decodability: {tot['g4_immediate_routes']}/{tot['routes']} routes, {tot['g4_points']} truncation points")

    print("\n### G. P29 single-stream: deployable vs bound, from the gate transcripts")
    print("`lag` is the codec's own `dispatch_lag_tus`: how many TUs after a TU is dispatched")
    print("its literals become sendable. Only 0 is a transport.\n")
    print("| cell | stream C→F | lg1 C→F (deployable) | lg112 C→F (bound) | lg1 vs bound | lg1 vs stream | F→C | lg112 lag | lg112 build increments |")
    print("|---|--:|--:|--:|--:|--:|--:|--:|---|")
    for f in sorted(glob.glob(f"{P29}/*.p29gate.txt")):
        txt = open(f).read()
        if "GATE PASS" not in txt:
            print(f"| {os.path.basename(f)} | **GATE DID NOT PASS — row withheld** |"); continue
        v = {}
        for ln in txt.splitlines():
            p2 = ln.split()
            # match only the variant SUMMARY lines; the "lg112 build increments" note also
            # starts with a variant name and would otherwise clobber the entry
            if p2 and p2[0] in ("stream", "lg1", "lg112") and "C->F=" in ln:
                v[p2[0]] = {k: s for k, s in (x.split("=", 1) for x in p2[1:] if "=" in x)}
        inc = ""
        for ln in txt.splitlines():
            if "build increments" in ln:
                inc = ln.split("increments:")[1].split("]")[0].strip() + "]"
        if len(v) != 3: continue
        s, o, b = int(v["stream"]["C->F"]), int(v["lg1"]["C->F"]), int(v["lg112"]["C->F"])
        print(f"| {os.path.basename(f)[:-len('.p29gate.txt')]} | {s:,} | {o:,} | {b:,} | "
              f"+{100*(o/b-1):.1f}% | −{100*(1-o/s):.1f}% | {int(v['lg1']['F->C']):,} | "
              f"{v['lg112']['lag']} | {inc} |")

    print("\n### H. Gate ledger (P29 sharded)")
    if p:
        rt = sum(r["routes"] for r in p)
        print(f"- configurations: {len(p)}, routes gated: {rt}")
        print(f"- every route: byte-exact reconstruction AND `dispatch_lag_tus=0`, "
              f"or the configuration aborts")
        print(f"- per-route prefix immutability (route × build boundary × direction): "
              f"{sum(r['prefix_checks_passed'] for r in p)}/{6*rt}")
        print(f"- routes additionally replayed frame-by-frame off their own files: "
              f"{sum(r['replays_verified'] for r in p)} (the first route of each configuration)")



if __name__ == "__main__":
    main()
