#!/usr/bin/env python3
"""Cache-domain width sweep + the M_min(30) binding table.

M = distinct selected F daemons = caches opened.  The owner's 30 is 30 simultaneous
compiler SLOTS, so the binding width is M_min(30) = min|S| s.t. sum(c_f) >= 30, where
c_f = slots behind daemon f.  M=30 is therefore the c_f=1 thin-daemon spray diagnostic,
one end of the spectrum -- not the universal binding number.
"""
import os

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus3": "duckdb", "corpus6": "godot",
      "corpus9": "catch2", "corpus11": "range-v3", "corpus12": "eigen", "corpus16": "cereal"}
Z = {}
for line in open(os.path.expanduser("~/grouprlz/corpora.tsv")).readlines()[1:]:
    f = line.rstrip("\n").split("\t")
    if f[0] in NM:
        Z[f[0]] = int(f[3]) / int(f[4])
H = ("corpus tus M wire cf_total fc_total cf_root cf_fill cf_control cf_carry fc_carry "
     "carry_undirected missing wall_s split_ok dir_ok").split()
IX = {k: i for i, k in enumerate(H)}
d = {}
for l in open(os.path.expanduser("~/selbind/width/width.tsv")).read().splitlines()[1:]:
    f = l.split("\t")
    d.setdefault(f[0], {})[int(f[IX["M"]])] = f

MS = [1, 2, 4, 8, 16, 30]
CF_TO_M = {1: 30, 4: 8, 8: 4, 30: 1}      # M_min(30) for each slots-per-daemon c_f
order = [c for c in ("corpus16", "corpus11", "corpus9", "corpus12", "corpus3", "corpus2",
                     "corpus", "corpus6") if c in d]


def cf(c, m):
    return int(d[c][m][IX["cf_total"]])


print("M_min(30): the binding cache-domain width, by slots-per-daemon c_f")
print("  30 simultaneous compiler slots.  M=30 is the c_f=1 THIN-DAEMON SPRAY DIAGNOSTIC.")
print("  Feasible saving is measured against that spray, not against M=1.\n")
print("  %-9s %6s %14s %14s %14s %14s" % ("corpus", "", *["c_f=%d" % k for k in (1, 4, 8, 30)]))
print("  %-9s %6s %14s %14s %14s %14s" % ("", "", *["M_min=%d" % CF_TO_M[k] for k in (1, 4, 8, 30)]))
for c in order:
    if not all(CF_TO_M[k] in d[c] for k in CF_TO_M):
        continue
    print("  %-9s %6s %14d %14d %14d %14d"
          % (NM[c], "C->F", *[cf(c, CF_TO_M[k]) for k in (1, 4, 8, 30)]))
    base = cf(c, 30)
    print("  %-9s %6s %14s %13.1f%% %13.1f%% %13.1f%%"
          % ("", "saved", "-", *[(base - cf(c, CF_TO_M[k])) / base * 100 for k in (4, 8, 30)]))
for k in (4, 8, 30):
    vals = [(cf(c, 30) - cf(c, CF_TO_M[k])) / cf(c, 30) * 100 for c in order
            if 30 in d[c] and CF_TO_M[k] in d[c]]
    print("  MEAN saved vs the c_f=1 spray at c_f=%-2d (M_min=%2d): %5.1f%%"
          % (k, CF_TO_M[k], sum(vals) / len(vals)))

print("\nWIDTH SWEEP -- C->F wire by cache-domain width M (direction-exact)\n")
print("  %-9s %8s %13s %9s %9s %9s %9s %9s"
      % ("corpus", "raw/z19", "M=1", *["M=%d" % m for m in MS[1:]]))
for c in order:
    print("  %-9s %8.0f %13d %9s %9s %9s %9s %9s"
          % (NM[c], Z.get(c, 0), cf(c, 1),
             *["%.2fx" % (cf(c, m) / cf(c, 1)) if m in d[c] else "-" for m in MS[1:]]))

print("\nMARGINAL COST OF THE NEXT CACHE DOMAIN -- extra C->F bytes per added daemon\n")
print("  %-9s %11s %11s %11s %11s %11s" % ("corpus", "1->2", "2->4", "4->8", "8->16", "16->30"))
for c in order:
    cells = []
    for a, b in ((1, 2), (2, 4), (4, 8), (8, 16), (16, 30)):
        cells.append("%11.0f" % ((cf(c, b) - cf(c, a)) / (b - a)) if a in d[c] and b in d[c] else "%11s" % "-")
    print("  %-9s %s" % (NM[c], "".join(cells)))

print("\nMARGINAL PRICE OF THE NEXT CACHE DOMAIN, as % of the build's OWN M=1 C->F wire")
print("  There is no knee -- the cost is near-linear in width -- so pick a threshold")
print("  and read off the widest M that still clears it.\n")
print("  %-9s %8s %9s %9s %9s %9s %9s"
      % ("corpus", "raw/z19", "1->2", "2->4", "4->8", "8->16", "16->30"))
for c in order:
    cells = []
    for a, b in ((1, 2), (2, 4), (4, 8), (8, 16), (16, 30)):
        cells.append("%8.1f%%" % ((cf(c, b) - cf(c, a)) / (b - a) / cf(c, 1) * 100)
                     if a in d[c] and b in d[c] else "%9s" % "-")
    print("  %-9s %8.0f %s" % (NM[c], Z.get(c, 0), " ".join(cells)))

print("\nWIDEST M WHOSE MARGINAL DAEMON COSTS UNDER X% OF THE M=1 WIRE\n")
print("  %-9s %8s %10s %10s %10s" % ("corpus", "raw/z19", "X=5%", "X=10%", "X=25%"))
for c in order:
    row = []
    for thr in (0.05, 0.10, 0.25):
        best = 1
        for a, b in ((1, 2), (2, 4), (4, 8), (8, 16), (16, 30)):
            if a in d[c] and b in d[c] and (cf(c, b) - cf(c, a)) / (b - a) <= thr * cf(c, 1):
                best = b
            else:
                break
        row.append("%10d" % best)
    print("  %-9s %8.0f %s" % (NM[c], Z.get(c, 0), "".join(row)))

print("\nFILL SHARE OF THE C->F WIRE, BY WIDTH\n")
print("  %-9s %8s %8s %8s %8s %8s %8s" % ("corpus", *["M=%d" % m for m in MS]))
for c in order:
    print("  %-9s %s" % (NM[c], " ".join(
        "%7.1f%%" % (int(d[c][m][IX["cf_fill"]]) / cf(c, m) * 100) if m in d[c] else "%8s" % "-"
        for m in MS)))

bad = [(c, m) for c in d for m in d[c]
       if d[c][m][IX["split_ok"]].split("/")[0] != d[c][m][IX["split_ok"]].split("/")[1]
       or d[c][m][IX["dir_ok"]].split("/")[0] != d[c][m][IX["dir_ok"]].split("/")[1]]
un = sum(int(d[c][m][IX["carry_undirected"]]) for c in d for m in d[c])
print("\n  gate: split_ok and dir_ok on every row -- %s;  carry_undirected total = %d"
      % ("ALL PASS" if not bad else "FAILURES %s" % bad, un))
