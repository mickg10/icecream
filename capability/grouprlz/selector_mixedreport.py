#!/usr/bin/env python3
"""Mixed warmth: one warm daemon, k-1 cold. What is a warm TU worth, and what is
recruiting a cold daemon worth?"""
import os

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus3": "duckdb", "corpus6": "godot",
      "corpus9": "catch2", "corpus11": "range-v3", "corpus12": "eigen", "corpus16": "cereal"}
H = ("corpus tus k warm cold pass1_wire pass2_wire p2_cf p2_fc p2_root p2_fill "
     "p2_warm_wire p2_cold_wire p2_warm_tus p2_cold_tus wall_s split_ok dir_ok").split()
IX = {k: i for i, k in enumerate(H)}
d = {}
for l in open(os.path.expanduser("~/selbind/mixedwarm/mixedwarm.tsv")).read().splitlines()[1:]:
    f = l.split("\t")
    d.setdefault(f[0], {})[int(f[IX["k"]])] = f

cold30 = {}
p = os.path.expanduser("~/selbind/cfsplit/cfsplit.tsv")
if os.path.exists(p):
    for l in open(p).read().splitlines()[1:]:
        f = l.split("\t")
        if f[2] == "30":
            cold30[f[0]] = int(f[3])

KS = [1, 2, 4, 8, 30]
order = [c for c in ("corpus16", "corpus11", "corpus9", "corpus12", "corpus3", "corpus2",
                     "corpus", "corpus6") if c in d]

print("MIXED WARMTH -- pass 2 lands on a farm with 1 warm daemon and k-1 cold ones.")
print("  Pass 1 ran entirely on worker 0, so it alone carries this project's history.\n")
print("BYTES PER TU, BY WHO SERVED IT\n")
print("  %-9s %4s %12s %12s %10s %8s %8s"
      % ("corpus", "k", "warm B/TU", "cold B/TU", "penalty", "warmTUs", "coldTUs"))
for c in order:
    for k in KS:
        r = d[c].get(k)
        if not r:
            continue
        wt, ct = int(r[IX["p2_warm_tus"]]), int(r[IX["p2_cold_tus"]])
        ww, cw = int(r[IX["p2_warm_wire"]]), int(r[IX["p2_cold_wire"]])
        w = ww / wt if wt else 0
        x = cw / ct if ct else 0
        print("  %-9s %4d %12.0f %12s %10s %8d %8d"
              % (NM[c], k, w, "%.0f" % x if ct else "-",
                 "%.1fx" % (x / w) if ct and w else "-", wt, ct))
    print()

print("WHAT WARMTH IS WORTH -- and only if you route to it\n")
print("  %-9s %14s %14s %10s %14s %10s"
      % ("corpus", "route to warm", "spread over 30", "x", "cold 30 (no warm)", "warm buys"))
gain, buys = [], []
for c in order:
    a, b = d[c].get(1), d[c].get(30)
    if not (a and b):
        continue
    w1, w30 = int(a[IX["pass2_wire"]]), int(b[IX["pass2_wire"]])
    c30 = cold30.get(c)
    gain.append(w30 / w1)
    row = "  %-9s %14d %14d %9.1fx" % (NM[c], w1, w30, w30 / w1)
    if c30:
        buys.append((w30 - c30) / c30 * 100)
        row += " %14d %9.1f%%" % (c30, (w30 - c30) / c30 * 100)
    print(row)
if gain:
    print("  %-9s %14s %14s %9.1fx %14s %9.1f%%"
          % ("MEAN", "", "", sum(gain) / len(gain), "",
             sum(buys) / len(buys) if buys else 0))

print("\nMARGINAL COST OF RECRUITING COLD DAEMONS (pass-2 wire vs k=1)\n")
print("  %-9s %10s %10s %10s %10s" % ("corpus", "k=2", "k=4", "k=8", "k=30"))
for c in order:
    a = d[c].get(1)
    if not a:
        continue
    base = int(a[IX["pass2_wire"]])
    print("  %-9s %s" % (NM[c], " ".join(
        "%9.1fx" % (int(d[c][k][IX["pass2_wire"]]) / base) if k in d[c] else "        -"
        for k in KS[1:])))
