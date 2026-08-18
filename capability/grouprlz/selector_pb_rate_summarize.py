#!/usr/bin/env python3
"""INTERIM policy-B rate table (conservative two-pass P29 charge).

Every P29 C rate is a pessimistic FLOOR: both the raw-plane plan pass and the grouped
encode are inside the candidate clock, so a future one-pass encoder can only be faster.
Cells that clear the gate here clear it a fortiori; cells that fail are exactly the ones
a one-pass refinement would target.
"""
import collections
import glob
import os
import statistics
import sys

RATE = os.path.expanduser("~/selbind/rate")
CENSUS = os.path.expanduser("~/selbind/selector-binding-dockermatrix-policyB.tsv")
PROFILES = ["debian-gcc", "conan-gcc", "linuxbrew", "fedora-clang-libcxx"]
C_FLOOR, F_FLOOR = 1.0, 0.5

head = open(CENSUS).readline().rstrip("\n").split("\t")
cen = {}
for l in open(CENSUS).readlines()[1:]:
    d = dict(zip(head, l.rstrip("\n").split("\t")))
    cen[(d["project"], d["profile"])] = d

rows = []
for f in sorted(glob.glob(f"{RATE}/*/row.tsv")):
    d = dict(l.rstrip("\n").split("\t", 1) for l in open(f) if "\t" in l)
    k = (d["project"], d["profile"])
    d["z19"] = int(cen[k]["lo_z19_long"])
    d["raw"] = int(d["raw_bytes"])
    d["p29c"] = int(d["p29_complete_bytes"])
    d["grzc"] = int(d["grz_complete_bytes"])
    d["p29C"] = float(d["p29_C_gbps_2pass_med"])
    d["grzC"] = float(d["grz_C_gbps_med"])
    d["grzF"] = float(d["grz_F_gbps_med"])
    d["p29F"] = float(d["p29_F_gbps_codec_proxy"])
    d["size_winner"] = "P29BSC" if d["p29c"] < d["grzc"] else "GRZ2"
    d["p29_legal"] = d["p29C"] >= C_FLOOR and d["p29F"] >= F_FLOOR
    d["grz_legal"] = d["grzC"] >= C_FLOOR and d["grzF"] >= F_FLOOR
    rows.append(d)

cols = ["project", "profile", "raw_bytes", "p29_complete_bytes", "grz_complete_bytes",
        "p29_C_gbps_2pass_med", "p29_C_gbps_2pass_all", "p29_plan_s_all", "p29_grouped_s_all",
        "grz_C_gbps_med", "grz_C_gbps_all", "grz_cat_s_all", "grz_enc_s_all",
        "grz_F_gbps_med", "grz_F_gbps_all", "p29_F_gbps_codec_proxy",
        "probe_makespan_med", "probe_makespan_all",
        "p29_C_legal_2pass", "grz_C_legal", "grz_F_legal",
        "p29_complete_exact", "grz_complete_exact", "p29_F_basis", "grz_F_basis", "charge"]
print("\t".join(cols + ["size_winner", "p29_gate_pass", "grz_gate_pass"]))
for r in rows:
    print("\t".join([r.get(c, "NA") for c in cols]
                    + [r["size_winner"], str(r["p29_legal"]), str(r["grz_legal"])]))

e = sys.stderr
print("# INTERIM / conservative-two-pass P29. Gate: C >= 1 GB/s AND F >= 500 MB/s.", file=e)
print("# P29 F is the codec's in-process proxy (no standalone decoder); GRZ F is a real "
      "single-thread decode.", file=e)

agg = collections.defaultdict(lambda: dict(n=0, raw=0, p29=0, grz=0, both=0, either=0,
                                           sel=0, z19=0))
for r in rows:
    for k in (r["profile"], "ALL"):
        a = agg[k]
        a["n"] += 1
        a["raw"] += r["raw"]
        a["z19"] += r["z19"]
        a["sel"] += min(r["p29c"], r["grzc"])
        a["p29"] += r["p29_legal"]
        a["grz"] += r["grz_legal"]
        a["both"] += r["p29_legal"] and r["grz_legal"]
        a["either"] += r["p29_legal"] or r["grz_legal"]
print("\n# cells clearing the full gate under the pessimistic charge:", file=e)
for k in PROFILES + ["ALL"]:
    a = agg[k]
    print("#  %-20s n=%2d  P29 legal %2d  GRZ legal %2d  both %2d  either %2d" % (
        k, a["n"], a["p29"], a["grz"], a["both"], a["either"]), file=e)

print("\n# rate distribution (median of reps):", file=e)
for name, f in (("P29 C (2-pass)", lambda r: r["p29C"]), ("GRZ C", lambda r: r["grzC"]),
                ("GRZ F (1T)", lambda r: r["grzF"]), ("P29 F (proxy)", lambda r: r["p29F"])):
    v = sorted(f(r) for r in rows)
    print("#  %-16s min %.3f  p25 %.3f  med %.3f  p75 %.3f  max %.3f  >=floor %d/44" % (
        name, v[0], v[len(v) // 4], statistics.median(v), v[3 * len(v) // 4], v[-1],
        sum(1 for x in v if x >= (F_FLOOR if "F" in name else C_FLOOR))), file=e)

wins = [r for r in rows if r["size_winner"] == "P29BSC"]
print("\n# THE 10 CELLS WHERE P29+BSC WINS COMPLETE SIZE -- does P29 clear the gate there?",
      file=e)
print("#  %-14s %-20s %8s %8s %8s  %s" % ("project", "profile", "P29 C", "P29 F", "GRZ C", "P29 gate"), file=e)
for r in sorted(wins, key=lambda r: -r["p29C"]):
    print("#  %-14s %-20s %8.3f %8.3f %8.3f  %s" % (
        r["project"], r["profile"], r["p29C"], r["p29F"], r["grzC"],
        "PASS" if r["p29_legal"] else "FAIL"), file=e)
print("#  -> P29 clears the gate on %d of those %d cells" % (
    sum(r["p29_legal"] for r in wins), len(wins)), file=e)

a = agg["ALL"]
print("\n# size-census selection carried through: selected=%d  x=%.2f  /z19=%.4f" % (
    a["sel"], a["raw"] / a["sel"], a["sel"] / a["z19"]), file=e)
legal = sum(min(r["p29c"], r["grzc"]) if (r["p29_legal"] and r["grz_legal"]) else
            (r["p29c"] if r["p29_legal"] else (r["grzc"] if r["grz_legal"] else
                                               min(r["p29c"], r["grzc"]))) for r in rows)
nolegal = sum(1 for r in rows if not r["p29_legal"] and not r["grz_legal"])
print("# rate-legal-restricted selection: %d  x=%.2f  /z19=%.4f  (cells with NO legal "
      "candidate: %d, charged at size-min)" % (
          legal, a["raw"] / legal, legal / a["z19"], nolegal), file=e)
