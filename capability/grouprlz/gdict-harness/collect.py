#!/usr/bin/env python3
"""Assemble the generic-dict cold-start ledger from the P29+BSC run artifacts."""
import csv, re, pathlib, sys

R = pathlib.Path("/home/ttuser/gdict/runs")

def out(tag):   return (R / f"{tag}.out").read_text()
def err(tag):   return (R / f"{tag}.err").read_text()

def total(tag): return int(re.search(r"TOTAL=(\d+)", out(tag)).group(1))
def distinct(tag): return int(re.search(r"distinct_lines=(\d+)", out(tag)).group(1))
def exact_ok(tag): return "byte-exact=OK" in out(tag)
def raw_bytes(tag): return int(re.search(r" raw=(\d+) ", err(tag)).group(1))
def cencode(tag): return float(re.search(r"C-encode ([\d.]+) GB/s", err(tag)).group(1))

def curve(tag, prefix):
    rows = list(csv.DictReader(open(R / f"{tag}.curve.tsv"), delimiter="\t"))
    base = int(rows[prefix-1]["cumulative_wire_bytes"]) if prefix else 0
    test = rows[prefix:]
    return {
        "wire": int(rows[-1]["cumulative_wire_bytes"]) - base,
        "tus": len(test),
        "raw": sum(int(r["raw_bytes"]) for r in test),
        "exact": all(r["exact"] == "true" for r in rows),
    }

WP_Z19 = {"rocksdb": 6199621, "abseil": 4020529, "cereal": 471206,
          "firefox": 80430607, "llvmfull": 73382413}

def row(corpus, dict_name, dict_tag, dict_prefix_tus, dict_zst, nodict_tag,
        withdict_tag, warm_tag, warm_prefix):
    nod = total(nodict_tag)
    wd = curve(withdict_tag, dict_prefix_tus)
    wm = curve(warm_tag, warm_prefix)
    gap = (nod - wd["wire"]) / (nod - wm["wire"]) * 100.0
    # differential C-encode rate over the test segment only
    t_pre = raw_bytes(dict_tag) / (cencode(dict_tag) * 1e9)
    t_all = raw_bytes(withdict_tag) / (cencode(withdict_tag) * 1e9)
    gbps = wd["raw"] / (t_all - t_pre) / 1e9 if t_all > t_pre else float("nan")
    new_lines = distinct(withdict_tag) - distinct(dict_tag)
    cover = (1 - new_lines / distinct(nodict_tag)) * 100.0
    return {
        "corpus": corpus, "dict_train_set": dict_name,
        "dict_tus": dict_prefix_tus, "dict_raw_bytes": raw_bytes(dict_tag),
        "dict_distinct_lines": distinct(dict_tag), "dict_bytes": dict_zst,
        "test_tus": wd["tus"], "test_raw_bytes": wd["raw"],
        "cold_nodict_bytes": nod, "cold_withdict_bytes": wd["wire"],
        "warm_bytes": wm["wire"], "gap_closed_pct": round(gap, 2),
        "dict_line_coverage_pct": round(cover, 1),
        "wp_z19_bytes": WP_Z19[corpus],
        "cold_nodict_over_z19": round(nod / WP_Z19[corpus], 4),
        "cold_withdict_over_z19": round(wd["wire"] / WP_Z19[corpus], 4),
        "cold_withdict_GBps": round(gbps, 2),
        "decode_exact": str(wd["exact"] and wm["exact"] and exact_ok(nodict_tag)).lower(),
    }
