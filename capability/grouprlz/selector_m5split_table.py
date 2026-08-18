#!/usr/bin/env python3
"""Per-TU Root/Need/Fill byte split: whole-run shares and the cold->warm trajectory."""
import glob
import os
import re
import sys

NM = {"corpus": "llvm", "corpus2": "rocksdb", "corpus3": "duckdb", "corpus9": "catch2",
      "corpus11": "range-v3", "corpus12": "eigen", "corpus16": "cereal"}
R = os.path.expanduser("~/selbind/m5split/runs")
H = ("logical physical worker raw wire latency_ns cumulative_raw cumulative_wire "
     "b_root b_need b_fill b_control b_carry split_ok c_root c_block c_path "
     "c_region_control c_raw_run c_array_control c_array_values missing_regions").split()
IX = {k: i for i, k in enumerate(H)}


def load(path):
    rows = []
    for line in open(path).read().splitlines()[1:]:
        f = line.split("\t")
        rows.append([int(x) for x in f])
    return rows


def share(rows, keys):
    tot = sum(r[IX["wire"]] for r in rows)
    return [(k, sum(r[IX[k]] for r in rows), sum(r[IX[k]] for r in rows) / tot * 100) for k in keys], tot


print("PER-TU FRAME SPLIT -- share of the whole relationship's wire (cap_m5, --codec z1, --real-pipes)")
print("  every row's b_root+b_need+b_fill+b_control+b_carry == its own wire (split_ok=1), 100% of rows\n")
print("  %-9s %2s %5s %14s %12s %9s %9s %9s %9s %9s"
      % ("corpus", "W", "TUs", "raw", "wire", "root%", "need%", "fill%", "ctrl%", "carry%"))
runs = []
for path in sorted(glob.glob(R + "/*.tsv")):
    b = os.path.basename(path)
    if ".base." in b:
        continue
    c, w = re.match(r"(corpus\d*)\.w(\d+)\.tsv", b).groups()
    rows = load(path)
    parts, tot = share(rows, ["b_root", "b_need", "b_fill", "b_control", "b_carry"])
    raw = sum(r[IX["raw"]] for r in rows)
    runs.append((c, int(w), rows, tot, raw))
    print("  %-9s %2s %5d %14d %12d %8.2f%% %8.2f%% %8.2f%% %8.2f%% %8.2f%%"
          % (NM[c], w, len(rows), raw, tot, *[p[2] for p in parts]))

print("\nFILL INTERIOR -- what the on-demand definitions are made of (share of TOTAL wire)")
print("  %-9s %2s %9s %9s %9s %9s %9s %9s %11s"
      % ("corpus", "W", "root_def%", "block%", "path%", "regctl%", "rawrun%", "arr%", "miss/TU"))
for c, w, rows, tot, raw in runs:
    g = lambda k: sum(r[IX[k]] for r in rows) / tot * 100
    miss = sum(r[IX["missing_regions"]] for r in rows) / len(rows)
    print("  %-9s %2s %8.2f%% %8.2f%% %8.2f%% %8.2f%% %8.2f%% %8.2f%% %11.1f"
          % (NM[c], w, g("c_root"), g("c_block"), g("c_path"), g("c_region_control"),
             g("c_raw_run"), g("c_array_control") + g("c_array_values"), miss))

print("\nCOLD -> WARM: the split by TU decile (workers=1), bytes per decile")
for c, w, rows, tot, raw in runs:
    if w != 1:
        continue
    print("\n  %s (%d TUs, %d wire bytes)" % (NM[c], len(rows), tot))
    print("    %-8s %12s %10s %8s %8s %8s %8s %9s"
          % ("decile", "wire", "raw/wire", "root%", "need%", "fill%", "ctrl%", "miss/TU"))
    n = len(rows)
    for d in range(10):
        seg = rows[n * d // 10:n * (d + 1) // 10]
        if not seg:
            continue
        sw = sum(r[IX["wire"]] for r in seg)
        sr = sum(r[IX["raw"]] for r in seg)
        g = lambda k: sum(r[IX[k]] for r in seg) / sw * 100 if sw else 0
        print("    %-8s %12d %10.1f %7.2f%% %7.2f%% %7.2f%% %7.2f%% %9.1f"
              % ("%d0-%d0%%" % (d, d + 1), sw, sr / sw if sw else 0, g("b_root"),
                 g("b_need"), g("b_fill"), g("b_control"),
                 sum(r[IX["missing_regions"]] for r in seg) / len(seg)))
