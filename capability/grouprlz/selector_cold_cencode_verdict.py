#!/usr/bin/env python3
"""COLD_1F_STICKY_CODEC verdict: both codecs, both bars, on the same in-memory basis."""
import glob
import os
import re
import statistics

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus9": "catch2",
      "corpus11": "range-v3", "corpus12": "eigen"}
Z = {}
for line in open(os.path.expanduser("~/grouprlz/corpora.tsv")).readlines()[1:]:
    f = line.rstrip("\n").split("\t")
    if f[0] in NM:
        Z[f[0]] = int(f[4])

print("COLD_1F_STICKY_CODEC -- isolated cold C-encode, in-memory producer, 3 reps, uncontended\n")
print("  P29 clock  = C_encode_ready (bytes resident -> last payload fully encoded),")
print("               an explicit timestamp pair, nothing subtracted.")
print("  GRZ2 clock = the codec's own ENC timer over a page-cache-warm mmap -- the same")
print("               bytes-already-in-memory basis. Both exclude any file-read artifact.\n")

rows = []
for c in sorted(NM, key=lambda k: NM[k]):
    W = os.path.expanduser("~/selbind/coldc/%s" % c)
    if not os.path.isdir(W):
        continue
    p29 = [float(m.group(1))
           for f in sorted(glob.glob(W + "/p29.*.err"))
           for m in [re.search(r"encode_GBps=([0-9.]+)", open(f).read())] if m]
    grz = [int(m.group(1)) / 1e9
           for f in sorted(glob.glob(W + "/grz.*.err"))
           for m in [re.search(r"C=(\d+) B/s", open(f).read())] if m]
    o = sorted(glob.glob(W + "/p29.*.out"))
    og = sorted(glob.glob(W + "/grz.*.out"))
    if not (p29 and grz and o and og):
        continue
    mm = re.search(r"TOTAL=(\d+)", open(o[0]).read())
    fl = open(og[0]).read().split("\t")
    rows.append((c, int(fl[0]), int(mm.group(1)), statistics.median(p29),
                 int(fl[1]), statistics.median(grz), Z[c]))

print("  %-10s %12s %10s %9s %9s %10s %9s %9s"
      % ("corpus", "raw", "P29 wire", "P29/z19", "P29 GB/s", "GRZ2 wire", "GRZ/z19", "GRZ GB/s"))
for c, raw, pw, pr, gw, gr, z in rows:
    print("  %-10s %12d %10d %9.4f %9.3f %10d %9.4f %9.3f"
          % (NM[c], raw, pw, pw / z, pr, gw, gw / z, gr))

print("\n  FINAL VERDICT -- the SELECTED codec and BOTH its bars\n")
print("  %-10s %-9s %11s %12s %s" % ("corpus", "selected", "sel/z19", "sel GB/s", "BOTH_BARS_PASS"))
npass = traw = tsel = tz = 0
for c, raw, pw, pr, gw, gr, z in rows:
    sel, sw, sr = ("P29+BSC", pw, pr) if pw < gw else ("GRZ2", gw, gr)
    ok = (sw / z <= 1.10) and (sr >= 1.0)
    npass += ok
    traw += raw
    tsel += sw
    tz += z
    print("  %-10s %-9s %11.4f %12.3f %s"
          % (NM[c], sel, sw / z, sr, "TRUE" if ok else "FALSE"))

print("\n  AGGREGATE: %d/%d corpora pass BOTH bars (size <= 1.10x z19 AND C-encode >= 1 GB/s)"
      % (npass, len(rows)))
print("  selected total %d B over %d raw = %.1fx raw, %.4f x z19" % (tsel, traw, traw / tsel, tsel / tz))
print("\n  Per-codec >= 1 GB/s: P29 %d/%d, GRZ2 %d/%d"
      % (sum(1 for r in rows if r[3] >= 1), len(rows), sum(1 for r in rows if r[5] >= 1), len(rows)))
