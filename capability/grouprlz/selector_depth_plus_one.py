#!/usr/bin/env python3
"""depth + 1: find the single second causal feature that resolves the docker overlap.

Rule shape is fixed and deliberately simple: P29+BSC iff depth >= D and f2 >= F2 (or
<= F2). Two thresholds, no model. Validation holds whole PROJECTS out, never splitting a
project's profiles or its two corpus generations across train and test.
"""
import collections
import glob
import json
import os
import sys

DOCKER = os.path.expanduser("~/issue16-selector-v1/matrix44-20260818T0230Z/cells")
F16 = os.path.expanduser("~/selbind/f16")
N25 = os.path.expanduser("~/selbind/n25")
F16_GRZ = {"corpus": 7638087, "corpus2": 5456878, "corpus3": 6576037, "corpus4": 3424583,
           "corpus5": 6687096, "corpus6": 53427567, "corpus7": 662261, "corpus8": 436106,
           "corpus9": 626178, "corpus10": 743198, "corpus11": 628319, "corpus12": 2040151,
           "corpus13": 401210, "corpus14": 489989, "corpus15": 955208, "corpus16": 423581}
F16_P29 = {"corpus": 7282357, "corpus2": 8537705, "corpus3": 7873485, "corpus4": 5058543,
           "corpus5": 6742098, "corpus6": 35094169, "corpus7": 932222, "corpus8": 477912,
           "corpus9": 907733, "corpus10": 1009497, "corpus11": 738136, "corpus12": 1141797,
           "corpus13": 436126, "corpus14": 618813, "corpus15": 1258040, "corpus16": 459374}
F16_NAME = {"corpus": "llvm", "corpus2": "rocksdb", "corpus3": "duckdb", "corpus4": "abseil",
            "corpus5": "opencv", "corpus6": "godot", "corpus7": "fmt", "corpus8": "spdlog",
            "corpus9": "catch2", "corpus10": "json", "corpus11": "range-v3",
            "corpus12": "eigen", "corpus13": "re2", "corpus14": "leveldb",
            "corpus15": "simdjson", "corpus16": "cereal"}

rows = []
for f in sorted(glob.glob(f"{DOCKER}/*/*/summary.stdout")):
    s = json.load(open(f))
    c = s["causal_first_group"]
    rows.append(dict(
        family="docker", project=s["project"], profile=s["profile"],
        p29=s["p29"]["wire_bytes"], grz=s["grz"]["wire_bytes"],
        z19=s["whole_zstd"]["z19_long_bytes"], raw=s["raw_bytes"], total_tus=s["tu_count"],
        probe_tus=c["tus"], probe_raw=c["raw_bytes"],
        depth=c["p29_region_repetition"], regions=c["p29_regions"],
        distinct=c["p29_distinct_lines"], litfrac=c["p29_literal_fraction"],
        addfrac=c["grz_add_fraction"], ratio=c["grz_wire_bytes"] / c["p29_wire_bytes"],
        ref_ops=c["p29_ref_ops"], lit_ops=c["p29_literal_ops"]))
for f in sorted(glob.glob(f"{F16}/*/feat.tsv")):
    d = dict(l.rstrip("\n").split("\t", 1) for l in open(f) if "\t" in l)
    c = d["id"]
    i = lambda k: int(d[k])
    rows.append(dict(
        family="fixed16", project=F16_NAME[c], profile="fixed16",
        p29=F16_P29[c], grz=F16_GRZ[c], z19=0, raw=0,
        total_tus=i("total_tus"), probe_tus=i("probe_tus"), probe_raw=i("probe_raw"),
        depth=i("p29_region_occ") / i("p29_regions"), regions=i("p29_regions"),
        distinct=i("p29_distinct_lines"),
        litfrac=i("p29_raw_literal") / i("probe_raw"),
        addfrac=i("grz_g1_add_bytes") / i("grz_g1_out_bytes"),
        ratio=i("grz_probe_bytes") / i("p29_probe_bytes"),
        ref_ops=i("p29_op_ref"), lit_ops=i("p29_op_literal")))

for f in sorted(glob.glob(f"{N25}/*/feat.tsv")):
    d = dict(l.rstrip("\n").split("\t", 1) for l in open(f) if "\t" in l)
    i = lambda k: int(d[k])
    rows.append(dict(
        family="native", project=d["id"], profile="native",
        p29=i("p29_complete"), grz=i("grz_complete"), z19=0, raw=0,
        total_tus=i("total_tus"), probe_tus=i("probe_tus"), probe_raw=i("probe_raw"),
        depth=i("p29_region_occ") / i("p29_regions"), regions=i("p29_regions"),
        distinct=i("p29_distinct_lines"),
        litfrac=i("p29_raw_literal") / i("probe_raw"),
        addfrac=i("grz_g1_add_bytes") / i("grz_g1_out_bytes"),
        ratio=i("grz_probe_bytes") / i("p29_probe_bytes"),
        ref_ops=i("p29_op_ref"), lit_ops=i("p29_op_literal")))

for r in rows:
    r["label"] = "P29" if r["p29"] < r["grz"] else "GRZ"
    r["remaining_tus"] = r["total_tus"] - r["probe_tus"]
    r["seen_frac"] = r["probe_tus"] / r["total_tus"]
    r["regions_per_mb"] = r["regions"] / (r["probe_raw"] / 1e6)
    r["distinct_per_region"] = r["distinct"] / r["regions"]
    r["ref_per_lit"] = r["ref_ops"] / max(1, r["lit_ops"])

FEATS = ["remaining_tus", "seen_frac", "regions", "regions_per_mb", "distinct_per_region",
         "addfrac", "litfrac", "ratio", "ref_per_lit"]


def regret(rows_, pick):
    return sum((r["p29"] if pick(r) else r["grz"]) - min(r["p29"], r["grz"]) for r in rows_)


def frozen(r):   # the baseline: cumulative raw at TU112 >= 500 MB
    return r["probe_raw"] >= 500_000_000


def grid(train, feat):
    """best (D, F2, sense) on `train` by total regret, ties broken by fewer errors."""
    best = None
    ds = sorted({r["depth"] for r in train})
    fs = sorted({r[feat] for r in train})
    for D in ds:
        for F2 in fs:
            for sense in (1, -1):
                pick = lambda r: r["depth"] >= D and (r[feat] >= F2 if sense > 0 else r[feat] <= F2)
                g = regret(train, pick)
                err = sum(1 for r in train if (pick(r)) != (r["label"] == "P29"))
                if best is None or (g, err) < (best[0], best[1]):
                    best = (g, err, D, F2, sense)
    return best


def rule(D, F2, sense, feat):
    return lambda r: r["depth"] >= D and (r[feat] >= F2 if sense > 0 else r[feat] <= F2)


e = sys.stderr
print("rows: docker %d, fixed16 %d, native %d" % (
    sum(r["family"] == "docker" for r in rows), sum(r["family"] == "fixed16" for r in rows),
    sum(r["family"] == "native" for r in rows)), file=e)
print("\nBaseline regret (lower is better):", file=e)
for fam in ("docker", "fixed16", "native"):
    sub = [r for r in rows if r["family"] == fam]
    print("  %-8s frozen-500MB %10d | always-GRZ %10d | always-P29 %10d | oracle 0" % (
        fam, regret(sub, frozen), regret(sub, lambda r: False),
        regret(sub, lambda r: True)), file=e)

print("\nLEAVE-ONE-PROJECT-OUT (fit on all other projects, both families together):", file=e)
projects = sorted({r["project"] for r in rows})
results = []
for feat in FEATS:
    held = collections.defaultdict(int)
    for p in projects:
        train = [r for r in rows if r["project"] != p]
        test = [r for r in rows if r["project"] == p]
        _, _, D, F2, sense = grid(train, feat)
        pick = rule(D, F2, sense, feat)
        for r in test:
            held[r["family"]] += (r["p29"] if pick(r) else r["grz"]) - min(r["p29"], r["grz"])
    results.append((held["docker"] + held["fixed16"] + held["native"],
                    held["docker"], held["fixed16"], held["native"], feat))
results.sort()
print("  %-22s %12s %12s %12s %12s" % ("depth + feature", "held docker", "held fixed16",
                                        "held native", "total"), file=e)
for tot, dk, f16, nat, feat in results:
    print("  depth + %-14s %12d %12d %12d %12d" % (feat, dk, f16, nat, tot), file=e)

best_feat = results[0][4]
g, err, D, F2, sense = grid(rows, best_feat)
pick = rule(D, F2, sense, best_feat)
print("\nBEST-ON-ALL rule (reported for inspection, NOT held out):", file=e)
print("  P29+BSC iff depth >= %.4g and %s %s %.6g" % (
    D, best_feat, ">=" if sense > 0 else "<=", F2), file=e)
for fam in ("docker", "fixed16", "native"):
    sub = [r for r in rows if r["family"] == fam]
    tp = sum(1 for r in sub if pick(r) and r["label"] == "P29")
    fp = sum(1 for r in sub if pick(r) and r["label"] == "GRZ")
    fn = sum(1 for r in sub if not pick(r) and r["label"] == "P29")
    print("  %-8s regret %10d (frozen %10d)  TP=%d FP=%d FN=%d" % (
        fam, regret(sub, pick), regret(sub, frozen), tp, fp, fn), file=e)
print("\n  the named cells:", file=e)
for r in rows:
    if (r["family"] == "fixed16" and r["project"] in ("godot", "llvm", "eigen", "rocksdb")) or r["family"] == "native":
        print("    %-8s %-10s depth %7.1f  %s %12.4g  label %s  rule %s  %s" % (
            r["family"], r["project"], r["depth"], best_feat, r[best_feat], r["label"],
            "P29" if pick(r) else "GRZ",
            "ok" if (pick(r) == (r["label"] == "P29")) else "WRONG"), file=e)
