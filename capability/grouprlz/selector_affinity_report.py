#!/usr/bin/env python3
"""DENSE_AFFINITY: the wire cost of spreading a build across k F daemons."""
import os
from collections import defaultdict

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus3": "duckdb", "corpus6": "godot",
      "corpus9": "catch2", "corpus11": "range-v3", "corpus12": "eigen", "corpus16": "cereal"}
P = os.path.expanduser("~/selbind/affinity/affinity.tsv")
rows = [l.split("\t") for l in open(P).read().splitlines()[1:]]
H = ("corpus tus workers assignment raw wire b_root b_need b_fill b_control b_carry "
     "wall_s split_ok distinct_workers").split()
IX = {k: i for i, k in enumerate(H)}

rr = defaultdict(dict)
alt = defaultdict(dict)
for r in rows:
    key = int(r[IX["workers"]])
    d = dict(wire=int(r[IX["wire"]]), root=int(r[IX["b_root"]]), need=int(r[IX["b_need"]]),
             fill=int(r[IX["b_fill"]]), wall=float(r[IX["wall_s"]]), tus=int(r[IX["tus"]]),
             ok=r[IX["split_ok"]], dw=int(r[IX["distinct_workers"]]))
    if r[IX["assignment"]] == "roundrobin":
        rr[r[0]][key] = d
    else:
        alt[r[0]][r[IX["assignment"]]] = d

KS = [1, 2, 4, 8, 16, 32]
print("FAN-OUT COST -- total C->F wire when one build is spread across k F daemons")
print("  round-robin, cold, single build.  Wire is exact and deterministic; every row's")
print("  per-TU split closes against its own wire.\n")
print("  %-9s %5s %12s %10s %10s %10s %10s %10s"
      % ("corpus", "TUs", "k=1 wire", *["k=%d" % k for k in KS[1:]]))
for c in sorted(rr, key=lambda x: NM.get(x, x)):
    d = rr[c]
    if 1 not in d:
        continue
    print("  %-9s %5d %12d %10s %10s %10s %10s %10s"
          % (NM.get(c, c), d[1]["tus"], d[1]["wire"],
             *["%.2fx" % (d[k]["wire"] / d[1]["wire"]) if k in d else "-" for k in KS[1:]]))

print("\nAVOIDABLE FRACTION -- (wire(k) - wire(1)) / wire(k): the share of the wire that")
print("routing to ONE warm daemon instead of k would not have sent at all\n")
print("  %-9s %8s %8s %8s %8s %8s" % ("corpus", *["k=%d" % k for k in KS[1:]]))
avg = defaultdict(list)
for c in sorted(rr, key=lambda x: NM.get(x, x)):
    d = rr[c]
    cells = []
    for k in KS[1:]:
        if k in d:
            v = (d[k]["wire"] - d[1]["wire"]) / d[k]["wire"] * 100
            avg[k].append(v)
            cells.append("%7.1f%%" % v)
        else:
            cells.append("       -")
    print("  %-9s %s" % (NM.get(c, c), " ".join(cells)))
print("  %-9s %s" % ("MEAN", " ".join("%7.1f%%" % (sum(avg[k]) / len(avg[k])) for k in KS[1:])))

print("\nWHERE IT GOES -- multiplier vs k=1, by frame kind (mean over corpora)")
print("  %-6s %10s %10s %10s %12s" % ("k", "Root x", "Need x", "Fill x", "per-consumer"))
print("  %-6s %10s %10s %10s %12s" % ("", "", "", "", "Fill bytes x"))
for k in KS:
    rs, ns, fs, ps = [], [], [], []
    for c, d in rr.items():
        if k in d and 1 in d:
            rs.append(d[k]["root"] / d[1]["root"])
            ns.append(d[k]["need"] / d[1]["need"])
            fs.append(d[k]["fill"] / d[1]["fill"])
            ps.append(d[k]["fill"] / k / d[1]["fill"])
    if rs:
        print("  %-6d %10.2f %10.2f %10.2f %12.3f"
              % (k, sum(rs) / len(rs), sum(ns) / len(ns), sum(fs) / len(fs), sum(ps) / len(ps)))

print("\nASSIGNMENT POLICY AT FIXED FAN-OUT (k=8) -- wire vs round-robin")
print("  %-9s %12s %12s %12s" % ("corpus", "roundrobin", "sticky", "random"))
for c in sorted(rr, key=lambda x: NM.get(x, x)):
    if 8 not in rr[c]:
        continue
    base = rr[c][8]["wire"]
    cells = []
    for a in ("sticky", "random"):
        v = alt[c].get(a)
        cells.append("%+.2f%%" % ((v["wire"] - base) / base * 100) if v else "-")
    print("  %-9s %12d %12s %12s" % (NM.get(c, c), base, *cells))

print("\nTRANSPORT MAKESPAN (wall s) -- C-side bound, see caveat")
print("  %-9s %8s %8s %8s %8s %8s %8s" % ("corpus", *["k=%d" % k for k in KS]))
for c in sorted(rr, key=lambda x: NM.get(x, x)):
    d = rr[c]
    print("  %-9s %s" % (NM.get(c, c),
                         " ".join("%8.2f" % d[k]["wall"] if k in d else "       -" for k in KS)))
