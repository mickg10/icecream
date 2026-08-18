#!/usr/bin/env python3
"""Assemble the cross-docker-profile transfer ledger over the verified-44 cells.

Every row is re-derived from the run's own per-TU curve; nothing is read back
from a log line. Rows carry the P29 source/binary SHA pins.
"""
import csv, pathlib, sys

B = pathlib.Path("/home/ttuser/gdict")
R, C = B / "runs", B / "cells44"
G = 112
PROF = {"debian": "debian-gcc", "conan": "conan-gcc",
        "brew": "linuxbrew", "fedora": "fedora-clang-libcxx"}
ENV = {"debian-gcc": ("gcc-12.2.0", "libstdc++-12"),
       "conan-gcc": ("gcc-14.2.0", "libstdc++-14"),
       "linuxbrew": ("clang-22.1.8", "libstdc++-12"),
       "fedora-clang-libcxx": ("clang-20.1.8", "libc++")}
PAIRS = [("conan", "debian"), ("conan", "brew"), ("conan", "fedora"),
         ("fedora", "debian"), ("debian", "brew"), ("debian", "conan")]
V44 = set()
for r in csv.DictReader((B / "verified-44-cells.tsv").open(), delimiter="\t"):
    V44.add((r["project"], r["profile"]))
PIN = {
    "p29_source_sha256": "2fccb899d518441ca81357f7543028bfe58fa734155c550926e22ff3e4fc125f",
    "p29_stock_binary_sha256": "8adb8b394970dd4e67aa82222874ad9a95c2d808c577c649a290b0d7d52a2493",
    "p29_run_binary_sha256": "0a092e877a0862d7bce782b22800c9a756711df10bf3961652a1e02449ed0f2e",
}


def ntu(p, prof):
    return sum(1 for _ in (C / f"{p}__{PROF[prof]}.man").open())


def pad(n):
    return n + (-n) % G


def curve(tag, prefix):
    f = R / f"{tag}.curve.tsv"
    if not f.is_file():
        return None
    rows = list(csv.DictReader(f.open(), delimiter="\t"))
    if len(rows) <= prefix:
        return None
    base = int(rows[prefix - 1]["cumulative_wire_bytes"]) if prefix else 0
    test = rows[prefix:]
    return {"wire": int(rows[-1]["cumulative_wire_bytes"]) - base, "tus": len(test),
            "raw": sum(int(r["raw_bytes"]) for r in test),
            "exact": all(r["exact"] == "true" for r in rows)}


w = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
w.writerow(["project", "corpus_set", "target_profile", "prior_profile",
            "target_compiler", "target_stdlib", "prior_compiler", "prior_stdlib",
            "compiler_axis", "stdlib_axis", "tu_aligned", "test_tus", "test_raw_bytes",
            "cold_bytes", "warm_xprofile_bytes", "warm_sameprofile_bytes",
            "gap_closed_pct", "xprofile_over_cold_pct", "decode_exact",
            "p29_source_sha256", "p29_stock_binary_sha256", "p29_run_binary_sha256"])
for p in sys.argv[1:]:
    cset = "verified-44" if (p, "debian-gcc") in V44 else "v2-pool"
    for tgt, pri in PAIRS:
        cold = curve(f"x44_{p}_{tgt}_cold", 0)
        same = curve(f"x44_{p}_{tgt}_warmsame", pad(ntu(p, tgt)))
        x = curve(f"x44_{p}_{tgt}_from_{pri}", pad(ntu(p, pri)))
        if not (cold and same and x):
            print(f"# MISSING {p} {tgt}<-{pri}", file=sys.stderr)
            continue
        gap = (cold["wire"] - x["wire"]) / (cold["wire"] - same["wire"]) * 100
        tc, ts = ENV[PROF[tgt]]
        pc, ps = ENV[PROF[pri]]
        w.writerow([p, cset, PROF[tgt], PROF[pri], tc, ts, pc, ps,
                    "same-family" if tc.split("-")[0] == pc.split("-")[0] else "cross-family",
                    "same" if ts == ps else ("version" if ts.split("-")[0] == ps.split("-")[0] else "family"),
                    str(ntu(p, tgt) == ntu(p, pri)).lower(),
                    x["tus"], x["raw"], cold["wire"], x["wire"], same["wire"],
                    round(gap, 2), round(x["wire"] / cold["wire"] * 100, 2),
                    str(cold["exact"] and same["exact"] and x["exact"]).lower(),
                    PIN["p29_source_sha256"], PIN["p29_stock_binary_sha256"],
                    PIN["p29_run_binary_sha256"]])
