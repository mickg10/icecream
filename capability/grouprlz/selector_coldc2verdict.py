#!/usr/bin/env python3
"""COLD_1F_STICKY_CODEC verdict over the broadened native set (22 corpora)."""
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
        Z[f[0]] = int(f[4])
        TUS[f[0]] = int(f[2])

rows = []
for c in NM:
    W = next((os.path.join(r, c) for r in ROOTS if os.path.isdir(os.path.join(r, c))), None)
    if not W:
        continue
    p29 = [float(m.group(1)) for f in sorted(glob.glob(W + "/p29.*.err"))
           for m in [re.search(r"encode_GBps=([0-9.]+)", open(f).read())] if m]
    grz = [int(m.group(1)) / 1e9 for f in sorted(glob.glob(W + "/grz.*.err"))
           for m in [re.search(r"C=(\d+) B/s", open(f).read())] if m]
    o, og = sorted(glob.glob(W + "/p29.*.out")), sorted(glob.glob(W + "/grz.*.out"))
    if not (p29 and grz and o and og):
        print("  SKIP %s (incomplete)" % c)
        continue
    mm = re.search(r"TOTAL=(\d+)", open(o[0]).read())
    fl = open(og[0]).read().split("\t")
    ident = "n/a"
    if os.path.exists(W + "/identity.txt"):
        ident = open(W + "/identity.txt").read().strip()
    rows.append(dict(c=c, raw=int(fl[0]), pw=int(mm.group(1)), pr=statistics.median(p29),
                     gw=int(fl[1]), gr=statistics.median(grz), z=Z[c], tus=TUS[c],
                     ident=ident, spread=(max(p29) - min(p29)) / statistics.median(p29) * 100))
rows.sort(key=lambda r: r["raw"])

print("COLD_1F_STICKY_CODEC -- isolated cold C-encode, in-memory producer, 3 reps, uncontended")
print("  P29 clock  = C_encode_ready (bytes resident -> last payload fully encoded).")
print("  GRZ2 clock = the codec's own ENC timer over a page-cache-warm mmap.\n")
print("  %-9s %5s %13s %11s %8s %8s %11s %8s %8s %6s"
      % ("corpus", "TUs", "raw", "P29 wire", "P29/z19", "P29 GB/s", "GRZ2 wire", "GRZ/z19", "GRZ GB/s", "spr%"))
for r in rows:
    print("  %-9s %5d %13d %11d %8.4f %8.3f %11d %8.4f %8.3f %6.2f"
          % (NM[r["c"]], r["tus"], r["raw"], r["pw"], r["pw"] / r["z"], r["pr"],
             r["gw"], r["gw"] / r["z"], r["gr"], r["spread"]))

print("\n  SELECTED codec (smaller wire) and BOTH bars: size <= 1.10x z19 AND C-encode >= 1 GB/s\n")
print("  %-9s %13s %-9s %10s %10s %s" % ("corpus", "raw", "selected", "sel/z19", "sel GB/s", "BOTH_BARS_PASS"))
npass = traw = tsel = tz = 0
psize = prate = 0
for r in rows:
    sel, sw, sr = ("P29+BSC", r["pw"], r["pr"]) if r["pw"] < r["gw"] else ("GRZ2", r["gw"], r["gr"])
    sizeok, rateok = sw / r["z"] <= 1.10, sr >= 1.0
    npass += sizeok and rateok
    psize += sizeok
    prate += rateok
    traw += r["raw"]
    tsel += sw
    tz += r["z"]
    r["sel"], r["sw"], r["sr"], r["ok"] = sel, sw, sr, sizeok and rateok
    print("  %-9s %13d %-9s %10.4f %10.3f %s"
          % (NM[r["c"]], r["raw"], sel, sw / r["z"], sr,
             "TRUE" if (sizeok and rateok) else
             "FALSE (%s)" % ("size" if not sizeok else "" ) + ("rate" if not rateok else "")))

n = len(rows)
print("\n  AGGREGATE: %d/%d pass BOTH bars   (size bar alone %d/%d, rate bar alone %d/%d)"
      % (npass, n, psize, n, prate, n))
print("  selected total %d B over %d raw = %.1fx raw, %.4f x z19" % (tsel, traw, traw / tsel, tsel / tz))
print("  Per-codec >= 1 GB/s: P29 %d/%d, GRZ2 %d/%d"
      % (sum(1 for r in rows if r["pr"] >= 1), n, sum(1 for r in rows if r["gr"] >= 1), n))
print("  Identity control (instrumented vs un-instrumented build, same flags):")
for r in rows:
    if r["ident"] != "n/a":
        print("    %-9s %s" % (NM[r["c"]], r["ident"]))

big = [r for r in rows if r["raw"] >= 1e9]
print("\n  Corpora >= 1 GB raw: %d/%d pass both bars" % (sum(1 for r in big if r["ok"]), len(big)))
small = [r for r in rows if r["raw"] < 1e9]
print("  Corpora <  1 GB raw: %d/%d pass both bars" % (sum(1 for r in small if r["ok"]), len(small)))
