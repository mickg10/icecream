#!/usr/bin/env python3
"""Join the target-disjoint bake-off votes (bakeoff25.tsv) against the cold codec
ratios (study25.tsv) to show the seed-only-pretraining LIFT per codebase.
Run on quietbox2: python3 join_bakeoff.py ~/bakeoff25.tsv ~/study25.tsv
"""
import sys, csv

bake_path = sys.argv[1] if len(sys.argv) > 1 else "bakeoff25.tsv"
study_path = sys.argv[2] if len(sys.argv) > 2 else "study25.tsv"

# study25.tsv: corpus TU cold_finalratio s0_amortized_40 byte_exact z19ldm secs
cold = {}
amort = {}
with open(study_path) as f:
    for r in csv.DictReader(f, delimiter="\t"):
        try:
            cold[r["corpus"]] = float(r["cold_finalratio"])
            amort[r["corpus"]] = float(r["s0_amortized_40"])
        except (ValueError, KeyError):
            pass

rows = []
with open(bake_path) as f:
    for r in csv.DictReader(f, delimiter="\t"):
        c = r["corpus"]
        try:
            bo = float(r["charged_ratio"])
        except (ValueError, KeyError):
            continue  # skip GENFAIL/CURVEFAIL/NA
        cr = cold.get(c)
        rows.append({
            "corpus": c,
            "name": r["name"],
            "compiler": r.get("compiler", "?"),
            "package": r.get("package", "?"),
            "tus": r.get("tus", "?"),
            "cold": cr,
            "bakeoff": bo,
            "amort": amort.get(c),
            "lift": (bo / cr) if cr else None,
            "exact": r.get("exact", "?"),
        })

# sort by bake-off ratio desc
rows.sort(key=lambda x: x["bakeoff"], reverse=True)

print(f"{'codebase':<16}{'comp':<6}{'pkg':<13}{'TUs':>5}{'cold':>9}{'pretrn':>9}{'lift':>7}{'>=400?':>8}{'amort':>10}")
print("-" * 92)
n_cold400 = n_bake400 = 0
for x in rows:
    cold_s = f"{x['cold']:.0f}x" if x["cold"] else "NA"
    lift_s = f"{x['lift']:.2f}x" if x["lift"] else "NA"
    amort_s = f"{x['amort']:.0f}x" if x["amort"] else "NA"
    cross = "YES" if x["bakeoff"] >= 400 else "-"
    if x["cold"] and x["cold"] >= 400: n_cold400 += 1
    if x["bakeoff"] >= 400: n_bake400 += 1
    print(f"{x['name']:<16}{x['compiler']:<6}{x['package']:<13}{str(x['tus']):>5}"
          f"{cold_s:>9}{x['bakeoff']:>8.0f}x{lift_s:>7}{cross:>8}{amort_s:>10}")

print("-" * 92)
lifts = [x["lift"] for x in rows if x["lift"]]
print(f"n={len(rows)}  cold>=400x: {n_cold400}/{len(rows)}   "
      f"pretrained-seed-online>=400x: {n_bake400}/{len(rows)}   "
      f"median lift: {sorted(lifts)[len(lifts)//2]:.2f}x   "
      f"max lift: {max(lifts):.2f}x")

# emit a clean joined TSV
with open("bakeoff25_joined.tsv", "w") as out:
    out.write("corpus\tname\tcompiler\tpackage\ttus\tcold_finalratio\tpretrained_seed_online\tlift\ts0_amortized_40\n")
    for x in rows:
        cold_v = f"{x['cold']}" if x['cold'] else "NA"
        lift_v = f"{x['lift']:.4f}" if x['lift'] else "NA"
        amort_v = f"{x['amort']}" if x['amort'] else "NA"
        out.write(f"{x['corpus']}\t{x['name']}\t{x['compiler']}\t{x['package']}\t{x['tus']}\t"
                  f"{cold_v}\t{x['bakeoff']:.2f}\t{lift_v}\t{amort_v}\n")
print("wrote bakeoff25_joined.tsv")
