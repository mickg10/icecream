#!/usr/bin/env python3
"""One row per corpus: both codecs, both bars, on the same in-memory basis."""
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

cols = ["id", "name", "tus", "raw", "wp_z19", "wp_ratio", "distinct_lines", "dl_per_MB",
        "P29_bytes", "P29_over_z19", "P29_Cgbps", "P29_spread_pct",
        "GRZ2_bytes", "GRZ2_over_z19", "GRZ2_Cgbps", "GRZ2_spread_pct",
        "selected_codec", "selected_bytes", "selected_over_z19", "selected_Cgbps",
        "size_bar_ok", "rate_bar_ok", "BOTH_BARS_PASS", "peakRSS_KiB",
        "identity_control", "codec_byte_exact"]
print("\t".join(cols))
out = []
for c in NM:
    W = next((os.path.join(r, c) for r in ROOTS if os.path.isdir(os.path.join(r, c))), None)
    if not W:
        continue
    p29 = [float(m.group(1)) for f in sorted(glob.glob(W + "/p29.*.err"))
           for m in [re.search(r"encode_GBps=([0-9.]+)", open(f).read())] if m]
    grz = [int(m.group(1)) / 1e9 for f in sorted(glob.glob(W + "/grz.*.err"))
           for m in [re.search(r"C=(\d+) B/s", open(f).read())] if m]
    rss = [int(m.group(1)) for f in sorted(glob.glob(W + "/p29.*.err"))
           for m in [re.search(r"peakRSS_KiB=(\d+)", open(f).read())] if m]
    text = open(sorted(glob.glob(W + "/p29.*.out"))[0]).read()
    fl = open(sorted(glob.glob(W + "/grz.*.out"))[0]).read().split("\t")
    pw = int(re.search(r"TOTAL=(\d+)", text).group(1))
    dl = int(re.search(r"distinct_lines=(\d+)", text).group(1))
    gw, raw, z = int(fl[1]), int(fl[0]), Z[c]
    pr, gr = statistics.median(p29), statistics.median(grz)
    sel, sw, sr = ("P29+BSC", pw, pr) if pw < gw else ("GRZ2", gw, gr)
    ident = open(W + "/identity.txt").read().strip() if os.path.exists(W + "/identity.txt") else "n/a"
    idc = "PASS" if "IDENTITY=PASS" in ident else ("n/a" if ident == "n/a" else "FAIL")
    bx = "OK" if "BYTEEXACT=OK" in ident else re.search(r"byte-exact=([A-Z]*)", text).group(1)
    out.append([c, NM[c], TUS[c], raw, z, "%.1f" % (raw / z), dl, "%.1f" % (dl / (raw / 1e6)),
                pw, "%.4f" % (pw / z), "%.3f" % pr, "%.2f" % ((max(p29) - min(p29)) / pr * 100),
                gw, "%.4f" % (gw / z), "%.3f" % gr, "%.2f" % ((max(grz) - min(grz)) / gr * 100),
                sel, sw, "%.4f" % (sw / z), "%.3f" % sr,
                "YES" if sw / z <= 1.10 else "NO", "YES" if sr >= 1.0 else "NO",
                "TRUE" if (sw / z <= 1.10 and sr >= 1.0) else "FALSE",
                max(rss) if rss else 0, idc, bx])
for row in sorted(out, key=lambda r: r[3]):
    print("\t".join(str(v) for v in row))
