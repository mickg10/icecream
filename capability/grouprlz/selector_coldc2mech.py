#!/usr/bin/env python3
"""What separates the corpora that reach 1 GB/s cold C-encode from those that do not."""
import glob
import os
import re
import statistics

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus3": "duckdb",
      "corpus4": "abseil", "corpus5": "opencv", "corpus6": "godot",
      "corpus7": "fmt", "corpus8": "spdlog", "corpus9": "catch2",
      "corpus10": "json", "corpus11": "range-v3", "corpus12": "eigen",
      "corpus13": "re2", "corpus14": "leveldb", "corpus15": "simdjson",
      "corpus16": "cereal", "corpus17": "gcc", "corpus19": "qtbase",
      "corpus21": "pytorch", "corpus22": "folly", "corpus23": "arrow",
      "corpus24": "bitcoin"}
ROOTS = [os.path.expanduser("~/selbind/coldc"), os.path.expanduser("~/selbind/coldc2")]
Z, TUS = {}, {}
for line in open(os.path.expanduser("~/grouprlz/corpora.tsv")).readlines()[1:]:
    f = line.rstrip("\n").split("\t")
    if f[0] in NM:
        Z[f[0]], TUS[f[0]] = int(f[4]), int(f[2])

rows = []
for c in NM:
    W = next((os.path.join(r, c) for r in ROOTS if os.path.isdir(os.path.join(r, c))), None)
    if not W:
        continue
    p29 = [float(m.group(1)) for f in sorted(glob.glob(W + "/p29.*.err"))
           for m in [re.search(r"encode_GBps=([0-9.]+)", open(f).read())] if m]
    grz = [int(m.group(1)) / 1e9 for f in sorted(glob.glob(W + "/grz.*.err"))
           for m in [re.search(r"C=(\d+) B/s", open(f).read())] if m]
    out = open(sorted(glob.glob(W + "/p29.*.out"))[0]).read()
    fl = open(sorted(glob.glob(W + "/grz.*.out"))[0]).read().split("\t")
    d = re.search(r"distinct_lines=(\d+)", out)
    reg = re.search(r"regions=(\d+)", out)
    pw = int(re.search(r"TOTAL=(\d+)", out).group(1))
    gw, raw = int(fl[1]), int(fl[0])
    sel, sr = ("P29+BSC", statistics.median(p29)) if pw < gw else ("GRZ2", statistics.median(grz))
    rows.append(dict(c=c, n=NM[c], raw=raw, z=Z[c], tus=TUS[c], sel=sel, sr=sr,
                     sw=min(pw, gw), wpr=raw / Z[c],
                     dl=int(d.group(1)) if d else 0, reg=int(reg.group(1)) if reg else 0))

def corr(xs, ys):
    mx, my = statistics.mean(xs), statistics.mean(ys)
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = (sum((x - mx) ** 2 for x in xs) * sum((y - my) ** 2 for y in ys)) ** 0.5
    return num / den if den else 0

import math
rate = [r["sr"] for r in rows]
print("Pearson r against selected C-encode GB/s (n=%d)" % len(rows))
for label, get in [("raw bytes", lambda r: r["raw"]),
                   ("log10 raw bytes", lambda r: math.log10(r["raw"])),
                   ("TU count", lambda r: r["tus"]),
                   ("whole-prog compressibility raw/z19", lambda r: r["wpr"]),
                   ("log10 compressibility", lambda r: math.log10(r["wpr"])),
                   ("distinct lines", lambda r: r["dl"]),
                   ("distinct lines per MB raw", lambda r: r["dl"] / (r["raw"] / 1e6)),
                   ("log10 distinct lines per MB", lambda r: math.log10(max(r["dl"], 1) / (r["raw"] / 1e6)))]:
    print("  %-38s r=%+.3f" % (label, corr([get(r) for r in rows], rate)))

print("\nSorted by distinct lines per MB of raw (novel material density):")
print("  %-9s %13s %9s %9s %9s %-9s %8s %s" % ("corpus", "raw", "raw/z19", "distinct", "dl/MB", "selected", "GB/s", "rate>=1"))
for r in sorted(rows, key=lambda r: r["dl"] / r["raw"]):
    print("  %-9s %13d %9.1f %9d %9.1f %-9s %8.3f %s"
          % (r["n"], r["raw"], r["wpr"], r["dl"], r["dl"] / (r["raw"] / 1e6), r["sel"], r["sr"],
             "YES" if r["sr"] >= 1 else "no"))

lo = [r for r in rows if r["dl"] / (r["raw"] / 1e6) <= 2.0]
hi = [r for r in rows if r["dl"] / (r["raw"] / 1e6) > 2.0]
print("\n  dl/MB <= 2.0 : %d/%d reach 1 GB/s" % (sum(1 for r in lo if r["sr"] >= 1), len(lo)))
print("  dl/MB >  2.0 : %d/%d reach 1 GB/s" % (sum(1 for r in hi if r["sr"] >= 1), len(hi)))
