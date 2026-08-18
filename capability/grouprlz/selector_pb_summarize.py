#!/usr/bin/env python3
"""Merge the corrected policy-B P29 rows with local-oracle's retained GRZ + zstd rows.

This is a measurement ledger, not a fitted selector. `selected_at_probe` is only the
argmin of the two TU112 probe censuses; it is recorded as an observation.
"""
import collections
import glob
import json
import os
import sys

ROWS = sorted(glob.glob(os.path.expanduser("~/selbind/pb/*/row.tsv")))
PROFILES = ["debian-gcc", "conan-gcc", "linuxbrew", "fedora-clang-libcxx"]

rows = []
for f in ROWS:
    d = dict(l.rstrip("\n").split("\t", 1) for l in open(f) if "\t" in l)
    cell = os.path.basename(os.path.dirname(f))
    meta = os.path.expanduser("~/selbind/dm/%s/cell.meta" % cell)
    lo = os.path.expanduser(
        "~/issue16-selector-v1/matrix44-20260818T0230Z/cells/%s/%s/summary.stdout"
        % (d["project"], d["profile"]))
    if os.path.exists(lo):
        s = json.load(open(lo))
        d["lo_grz_complete_bytes"] = str(s["grz"]["wire_bytes"])
        d["lo_grz_exact"] = str(s["grz"]["exact"])
        d["lo_grz_f_bps"] = str(s["grz"]["f_decode_bps"])
        d["lo_z19_long"] = str(s["whole_zstd"]["z19_long_bytes"])
        d["legacy_p29_bytes"] = str(s["p29"]["wire_bytes"])
        d["z19_source"] = "local-oracle-retained"
    elif os.path.exists(meta):
        d["lo_z19_long"] = open(meta).read().strip().split("\t")[4]
        d["z19_source"] = "implementer-dm-sweep"
    else:
        d["z19_source"] = "MISSING"
    for k in ("lo_grz_complete_bytes", "lo_grz_exact", "lo_grz_f_bps", "lo_z19_long",
              "legacy_p29_bytes"):
        d.setdefault(k, "NA")
    rows.append(d)

cols = ["project", "profile", "total_tus", "probe_tus", "raw_bytes", "probe_raw_bytes",
        "identity", "p29_complete_bytes", "legacy_p29_bytes", "p29_complete_exact",
        "lo_grz_complete_bytes", "lo_grz_exact", "lo_grz_f_bps", "lo_z19_long", "z19_source",
        "p29_probe_bytes", "grz_probe_bytes", "selected_at_probe",
        "grz_g1_close_tu", "grz_g1_closed_by", "grz_g1_add_bytes",
        "grz_anchor_samples", "grz_anchor_matches",
        "makespan_s", "p29_probe_eUS", "grz_probe_eUS", "p29_plan_s",
        "producer_extract_s", "producer_concat_s", "p29_intern_s", "p29_plan_s1_s",
        "p29_literal_entropy_s", "grz_match_s", "grz_entropy_s", "grz_total_s",
        "p29_interner", "p29_src_sha", "p29_bin_sha", "grz_src_sha", "grz_bin_sha", "zstd"]
print("\t".join(cols))
for r in rows:
    print("\t".join(r.get(c, "NA") for c in cols))

e = sys.stderr
print("# cells=%d  identity_PASS=%d/%d  p29_complete_byte_exact=%d/%d" % (
    len(rows), sum(r["identity"] == "PASS" for r in rows), len(rows),
    sum(r["p29_complete_exact"] == "OK" for r in rows), len(rows)), file=e)
print("# GRZ complete rows reused from local-oracle: %d/%d (pending: %s)" % (
    sum(r["lo_grz_complete_bytes"] != "NA" for r in rows), len(rows),
    " ".join("%s/%s" % (r["project"], r["profile"])
             for r in rows if r["lo_grz_complete_bytes"] == "NA")), file=e)

agg = collections.defaultdict(lambda: dict(
    n=0, raw=0, praw=0, p29=0, grz=0, z19=0, z19n=0, p29p=0, grzp=0, mk=0.0, cs=0.0,
    sel=collections.Counter(), close=collections.Counter()))
for r in rows:
    for k in (r["profile"], "ALL"):
        a = agg[k]
        a["n"] += 1
        a["raw"] += int(r["raw_bytes"])
        a["praw"] += int(r["probe_raw_bytes"])
        a["p29"] += int(r["p29_complete_bytes"])
        a["p29p"] += int(r["p29_probe_bytes"])
        a["grzp"] += int(r["grz_probe_bytes"])
        if r["lo_z19_long"] != "NA":
            a["z19"] += int(r["lo_z19_long"])
            a["z19n"] += 1
        if r["lo_grz_complete_bytes"] != "NA":
            a["grz"] += int(r["lo_grz_complete_bytes"])
        a["mk"] += float(r["makespan_s"])
        p, g = r["p29_probe_eUS"].split(), r["grz_probe_eUS"].split()
        a["cs"] += float(p[1]) + float(p[2]) + float(g[1]) + float(g[2])
        a["sel"][r["selected_at_probe"]] += 1
        a["close"][r["grz_g1_closed_by"]] += 1

for k in PROFILES + ["ALL"]:
    a = agg[k]
    print("# %-20s n=%2d raw=%7.2fGB  P29corrected=%10d  /z19=%.4f (%d refs)  "
          "argmin@probe=%s  g1_close=%s" % (
              k, a["n"], a["raw"] / 1e9, a["p29"], a["p29"] / a["z19"], a["z19n"],
              dict(a["sel"]), dict(a["close"])), file=e)
    print("#   %-18s probe: P29=%9d GRZ=%9d (GRZ/P29=%.4f)  makespan_sum=%6.2fs  "
          "core-s/probe-GiB=%.2f" % (
              "", a["p29p"], a["grzp"], a["grzp"] / a["p29p"], a["mk"],
              a["cs"] / (a["praw"] / 2 ** 30)), file=e)
