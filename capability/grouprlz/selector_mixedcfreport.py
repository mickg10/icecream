#!/usr/bin/env python3
"""Mixed warmth, recut on the C->F-only wire; duplex kept alongside for the delta."""
import os

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus3": "duckdb", "corpus6": "godot",
      "corpus9": "catch2", "corpus11": "range-v3", "corpus12": "eigen", "corpus16": "cereal"}
H = ("corpus tus k p1_cf p2_cf p2_duplex p2_warm_cf p2_cold_cf p2_warm_duplex "
     "p2_cold_duplex p2_warm_tus p2_cold_tus p2_cf_root p2_cf_fill wall_s split_ok "
     "dir_ok undirected").split()
IX = {k: i for i, k in enumerate(H)}
d = {}
for l in open(os.path.expanduser("~/selbind/mixedwarm/mixedwarm_cf.tsv")).read().splitlines()[1:]:
    f = l.split("\t")
    d.setdefault(f[0], {})[int(f[IX["k"]])] = f
KS = [1, 2, 4, 8, 30]
order = [c for c in ("corpus16", "corpus11", "corpus9", "corpus12", "corpus3", "corpus2",
                     "corpus", "corpus6") if c in d and 1 in d[c] and 30 in d[c]]


def v(c, k, f):
    return int(d[c][k][IX[f]])


print("ROUTE TO WARM vs SPREAD OVER 30 -- C->F only, with the duplex delta\n")
print("  %-9s %14s %14s %9s %14s %9s %9s"
      % ("corpus", "warm (C->F)", "spread30 (C->F)", "x C->F", "x duplex", "delta", "C->F %"))
xs, xd = [], []
for c in order:
    a, b = v(c, 1, "p2_cf"), v(c, 30, "p2_cf")
    ad, bd = v(c, 1, "p2_duplex"), v(c, 30, "p2_duplex")
    xs.append(b / a)
    xd.append(bd / ad)
    print("  %-9s %14d %14d %8.1fx %13.1fx %8.1f%% %8.1f%%"
          % (NM[c], a, b, b / a, bd / ad, (b / a) / (bd / ad) * 100 - 100, b / bd * 100))
med = sorted(xs)[len(xs) // 2 - 1:len(xs) // 2 + 1]
medd = sorted(xd)[len(xd) // 2 - 1:len(xd) // 2 + 1]
print("  %-9s %14s %14s %8.1fx %13.1fx"
      % ("MEDIAN", "", "", sum(med) / 2, sum(medd) / 2))

print("\nBYTES PER TU ON THE C->F WIRE, BY WHO SERVED IT\n")
print("  %-9s %4s %12s %12s %10s %14s"
      % ("corpus", "k", "warm B/TU", "cold B/TU", "penalty", "duplex cold B/TU"))
for c in order:
    for k in KS:
        if k not in d[c]:
            continue
        wt, ct = v(c, k, "p2_warm_tus"), v(c, k, "p2_cold_tus")
        w = v(c, k, "p2_warm_cf") / wt if wt else 0
        x = v(c, k, "p2_cold_cf") / ct if ct else 0
        xd_ = v(c, k, "p2_cold_duplex") / ct if ct else 0
        if k in (1, 30):
            print("  %-9s %4d %12.0f %12s %10s %14s"
                  % (NM[c], k, w, "%.0f" % x if ct else "-",
                     "%.1fx" % (x / w) if ct and w else "-",
                     "%.0f" % xd_ if ct else "-"))
    print()

print("MARGINAL PRICE OF MOVING ONE TU OFF THE WARM DAEMON AT 30 SLOTS (C->F)\n")
print("  %-9s %14s %14s %14s" % ("corpus", "cold-warm B/TU", "on duplex", "delta"))
for c in order:
    ct = v(c, 30, "p2_cold_tus")
    wt = v(c, 30, "p2_warm_tus")
    if not (ct and wt):
        continue
    cf = v(c, 30, "p2_cold_cf") / ct - v(c, 30, "p2_warm_cf") / wt
    dx = v(c, 30, "p2_cold_duplex") / ct - v(c, 30, "p2_warm_duplex") / wt
    print("  %-9s %14.0f %14.0f %13.1f%%" % (NM[c], cf, dx, cf / dx * 100 - 100))

bad = [(c, k) for c in d for k in d[c]
       if d[c][k][IX["split_ok"]].split("/")[0] != d[c][k][IX["split_ok"]].split("/")[1]
       or d[c][k][IX["dir_ok"]].split("/")[0] != d[c][k][IX["dir_ok"]].split("/")[1]]
un = sum(int(d[c][k][IX["undirected"]]) for c in d for k in d[c])
print("\n  gate: %s;  carry_undirected total = %d"
      % ("ALL PASS" if not bad else "FAILURES %s" % bad, un))
