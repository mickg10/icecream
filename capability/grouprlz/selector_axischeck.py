#!/usr/bin/env python3
"""Are all four dashboard lines on the SAME TU axis?

The join already asserts GRZ2 against the fast interner. This adds the P29 line
(cap_m5, workers=1): its per-TU `raw` must equal the fast interner's per-TU `raw`
for every TU, or the two curves are indexing different files.
"""
import os
import sys

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus6": "godot", "corpus9": "catch2",
      "corpus11": "range-v3", "corpus12": "eigen"}
W = os.path.expanduser("~/selbind/curves")
M5 = os.path.expanduser("~/selbind/m5split/runs")

print("%-9s %-9s %6s %-14s %s" % ("corpus", "name", "TUs", "axis", "detail"))
for c in sys.argv[1:]:
    f = W + "/%s.fusedcurve.tsv" % c
    m = M5 + "/%s.w1.tsv" % c
    if not os.path.exists(f):
        print("%-9s %-9s %6s %-14s" % (c, NM.get(c, c), "-", "NO_FUSED"))
        continue
    if not os.path.exists(m):
        print("%-9s %-9s %6s %-14s %s" % (c, NM.get(c, c), "-", "NO_P29", "cap_m5 curve missing"))
        continue
    fr = [l.split("\t") for l in open(f).read().splitlines()[1:]]
    mr = [l.split("\t") for l in open(m).read().splitlines()[1:]]
    if len(fr) != len(mr):
        print("%-9s %-9s %6s %-14s fused=%d p29=%d"
              % (c, NM.get(c, c), "-", "LEN_MISMATCH", len(fr), len(mr)))
        continue
    bad = [i for i in range(len(fr)) if int(fr[i][1]) != int(mr[i][3])]
    # also check the physical index is the identity (Standard order)
    perm = [i for i in range(len(mr)) if int(mr[i][0]) != int(mr[i][1])]
    raw_f = sum(int(r[1]) for r in fr)
    raw_m = sum(int(r[3]) for r in mr)
    print("%-9s %-9s %6d %-14s raw fused=%d p29=%d%s%s"
          % (c, NM.get(c, c), len(fr), "MATCH" if not bad else "MISMATCH", raw_f, raw_m,
             "" if not bad else "  first bad tu=%d" % bad[0],
             "" if not perm else "  NON-IDENTITY ORDER at %d TUs" % len(perm)))
