#!/usr/bin/env python3
"""Policy-B classifier bracket, size-only (no rates).

Upper end  = oracle: per-cell min(complete P29+BSC, complete GRZ2).
Lower end  = the naive rule: pick whichever TU112 census is smaller, then ship that
             codec's complete wire.  On this matrix the GRZ census is smaller on 44/44,
             so the naive rule degenerates to GRZ2-always.

Everything between is what a cheap classifier could recover.  The question is whether
the 10 cells where P29+BSC wins the COMPLETE size are separable from features that are
observable AT the TU112 decision point.
"""
import collections
import os
import sys

TSV = os.path.expanduser("~/selbind/selector-binding-dockermatrix-policyB.tsv")
head = open(TSV).readline().rstrip("\n").split("\t")
rows = [dict(zip(head, l.rstrip("\n").split("\t"))) for l in open(TSV).readlines()[1:]]

for r in rows:
    r["raw"] = int(r["raw_bytes"])
    r["z19"] = int(r["lo_z19_long"])
    r["p29c"] = int(r["p29_complete_bytes"])
    r["grzc"] = int(r["lo_grz_complete_bytes"])
    r["p29p"] = int(r["p29_probe_bytes"])
    r["grzp"] = int(r["grz_probe_bytes"])
    r["label"] = "P29BSC" if r["p29c"] < r["grzc"] else "GRZ2"
    # features observable at the decision point
    r["ratio"] = r["grzp"] / r["p29p"]          # <1 means GRZ smaller at TU112
    r["close_early"] = r["grz_g1_closed_by"] != "tu"
    r["anchor_hit"] = int(r["grz_anchor_matches"]) / max(1, int(r["grz_anchor_samples"]))
    r["probe_frac"] = int(r["probe_raw_bytes"]) / r["raw"]

RAW = sum(r["raw"] for r in rows)
Z19 = sum(r["z19"] for r in rows)


def total(pick):
    return sum(r["grzc"] if pick(r) == "GRZ2" else r["p29c"] for r in rows)


def line(name, t):
    print("  %-46s %12d  x=%8.2f  /z19=%.4f" % (name, t, RAW / t, t / Z19))


print("BRACKET (size-only, %d cells, raw=%.2f GB, z19=%d)\n" % (len(rows), RAW / 1e9, Z19))
line("ORACLE  per-cell min(complete)", total(lambda r: r["label"]))
line("NAIVE   smaller TU112 census -> GRZ2 always", total(lambda r: "GRZ2"))
line("  reference: P29+BSC always", total(lambda r: "P29BSC"))
print("  %-46s %12d  x=%8.2f  /z19=%.4f" % ("  reference: whole-program zstd-19", Z19, RAW / Z19, 1.0))
gap = total(lambda r: "GRZ2") - total(lambda r: r["label"])
print("\n  bracket width = %d B (%.4f x z19 of headroom a classifier could recover)\n"
      % (gap, gap / Z19))

wins = [r for r in rows if r["label"] == "P29BSC"]
print("The %d cells where P29+BSC wins the COMPLETE size (GRZ is smaller at TU112 on all "
      "44, so these are pure tail overtakes):" % len(wins))
print("  %-14s %-20s %10s %10s %8s %8s %6s %8s" %
      ("project", "profile", "GRZ compl", "P29 compl", "margin", "TU112 r", "close", "anchor"))
for r in sorted(wins, key=lambda r: r["ratio"]):
    print("  %-14s %-20s %10d %10d %+8d %8.4f %6s %8.4f" % (
        r["project"], r["profile"], r["grzc"], r["p29c"], r["p29c"] - r["grzc"],
        r["ratio"], r["grz_g1_closed_by"], r["anchor_hit"]))

print("\nFeature separability, P29BSC-win (n=%d) vs GRZ2-win (n=%d):"
      % (len(wins), len(rows) - len(wins)))
grz = [r for r in rows if r["label"] == "GRZ2"]


def spread(f, name):
    a = sorted(f(r) for r in wins)
    b = sorted(f(r) for r in grz)
    print("  %-22s P29-win [%.4f .. %.4f] med %.4f | GRZ-win [%.4f .. %.4f] med %.4f%s"
          % (name, a[0], a[-1], a[len(a) // 2], b[0], b[-1], b[len(b) // 2],
             "   SEPARABLE" if a[0] > b[-1] or a[-1] < b[0] else ""))


spread(lambda r: r["ratio"], "TU112 census ratio")
spread(lambda r: r["anchor_hit"], "g1 anchor hit rate")
spread(lambda r: r["probe_frac"], "probe raw fraction")

print("\nPer-profile prior:")
for pf in ["debian-gcc", "conan-gcc", "linuxbrew", "fedora-clang-libcxx"]:
    sub = [r for r in rows if r["profile"] == pf]
    n = sum(r["label"] == "P29BSC" for r in sub)
    print("  %-22s P29+BSC wins %d/%d" % (pf, n, len(sub)))
print("\nPer-project prior:")
for pj in sorted({r["project"] for r in rows}):
    sub = [r for r in rows if r["project"] == pj]
    n = sum(r["label"] == "P29BSC" for r in sub)
    if n:
        print("  %-22s P29+BSC wins %d/%d" % (pj, n, len(sub)))

print("\nWhat single thresholds on the TU112 ratio would recover:")
best = None
for r in sorted(rows, key=lambda r: r["ratio"]):
    thr = r["ratio"]
    t = total(lambda x: "P29BSC" if x["ratio"] >= thr else "GRZ2")
    tp = sum(1 for x in rows if x["ratio"] >= thr and x["label"] == "P29BSC")
    fp = sum(1 for x in rows if x["ratio"] >= thr and x["label"] == "GRZ2")
    if best is None or t < best[1]:
        best = (thr, t, tp, fp)
print("  best single-threshold rule: ratio >= %.4f -> P29+BSC  (%d correct, %d wrong)" % (
    best[0], best[2], best[3]))
line("  that rule", best[1])
print("  recovers %.1f%% of the bracket" % (
    100 * (total(lambda r: "GRZ2") - best[1]) / gap), file=sys.stdout)
