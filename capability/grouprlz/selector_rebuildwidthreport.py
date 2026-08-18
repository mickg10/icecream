#!/usr/bin/env python3
"""Rebuild at the binding cache-domain widths, pinning vs not, C->F wire."""
import os

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus3": "duckdb", "corpus6": "godot",
      "corpus9": "catch2", "corpus11": "range-v3", "corpus12": "eigen", "corpus16": "cereal"}
H = ("corpus tus M assignment p1_cf p2_cf p2_over_p1 p2_cf_root p2_cf_fill p2_fc "
     "p2_missing wall_s split_ok dir_ok undirected").split()
IX = {k: i for i, k in enumerate(H)}
d = {}
for l in open(os.path.expanduser("~/selbind/rebuildwidth/rebuildwidth.tsv")).read().splitlines()[1:]:
    f = l.split("\t")
    d.setdefault(f[0], {}).setdefault(int(f[IX["M"]]), {})[f[IX["assignment"]]] = f

MS = [1, 4, 8, 30]
CF = {30: 1, 8: 4, 4: 8, 1: 30}   # M -> the c_f it is M_min(30) for
order = [c for c in ("corpus16", "corpus11", "corpus9", "corpus12", "corpus3", "corpus2",
                     "corpus", "corpus6") if c in d]


def g(c, m, a, k="p2_cf"):
    return int(d[c][m][a][IX[k]])


print("REBUILD AT THE BINDING WIDTHS -- C->F wire of the second build\n")
print("  M=1 is M_min(30) for c_f=30, M=4 for c_f=8, M=8 for c_f=4, M=30 for c_f=1.\n")
print("  %-9s %14s %14s %14s %14s %14s"
      % ("corpus", "cold build", "M=1", "M=4 pin", "M=8 pin", "M=30 pin"))
for c in order:
    if not all(m in d[c] and "sticky" in d[c][m] for m in MS):
        continue
    print("  %-9s %14d %14d %14d %14d %14d"
          % (NM[c], g(c, 1, "sticky", "p1_cf"), *[g(c, m, "sticky") for m in MS]))

print("\nWITH PINNING THE REBUILD IS WIDTH-INDEPENDENT  (M=30 vs M=1)\n")
print("  %-9s %12s %12s %10s %14s"
      % ("corpus", "M=1 pin", "M=30 pin", "growth", "cold M=30 x"))
gr = []
for c in order:
    if not all(m in d[c] and "sticky" in d[c][m] for m in MS):
        continue
    a, b = g(c, 1, "sticky"), g(c, 30, "sticky")
    gr.append(b / a)
    print("  %-9s %12d %12d %9.2f%% %13.2fx"
          % (NM[c], a, b, (b / a - 1) * 100,
             g(c, 30, "sticky", "p1_cf") / g(c, 1, "sticky", "p1_cf")))
if gr:
    print("  %-9s %12s %12s %9.2f%%" % ("MEAN", "", "", (sum(gr) / len(gr) - 1) * 100))

print("\nPINNING vs NOT, AT EACH BINDING WIDTH  (C->F wire saved by pinning)\n")
print("  %-9s %10s %10s %10s %10s" % ("corpus", *["M=%d" % m for m in MS]))
means = {m: [] for m in MS}
for c in order:
    cells = []
    for m in MS:
        if m in d[c] and "sticky" in d[c][m] and "roundrobin" in d[c][m]:
            s, r = g(c, m, "sticky"), g(c, m, "roundrobin")
            v = (s - r) / r * 100
            means[m].append(v)
            cells.append("%9.1f%%" % v)
        else:
            cells.append("%10s" % "-")
    print("  %-9s %s" % (NM[c], " ".join(cells)))
print("  %-9s %s" % ("MEAN", " ".join(
    "%9.1f%%" % (sum(means[m]) / len(means[m])) if means[m] else "%10s" % "-" for m in MS)))

print("\nDEFINITION REQUESTS ON THE REBUILD  (missing regions; pinning should be zero)\n")
print("  %-9s %10s %10s %10s %10s %12s"
      % ("corpus", "M=1 pin", "M=4 pin", "M=8 pin", "M=30 pin", "M=30 no-pin"))
for c in order:
    if not all(m in d[c] and "sticky" in d[c][m] for m in MS):
        continue
    print("  %-9s %10d %10d %10d %10d %12s"
          % (NM[c], *[g(c, m, "sticky", "p2_missing") for m in MS],
             g(c, 30, "roundrobin", "p2_missing") if "roundrobin" in d[c][30] else "-"))

bad = [(c, m, a) for c in d for m in d[c] for a in d[c][m]
       if d[c][m][a][IX["split_ok"]].split("/")[0] != d[c][m][a][IX["split_ok"]].split("/")[1]
       or d[c][m][a][IX["dir_ok"]].split("/")[0] != d[c][m][a][IX["dir_ok"]].split("/")[1]]
un = sum(int(d[c][m][a][IX["undirected"]]) for c in d for m in d[c] for a in d[c][m])
print("\n  gate: %s;  carry_undirected total = %d"
      % ("ALL PASS" if not bad else "FAILURES %s" % bad, un))
