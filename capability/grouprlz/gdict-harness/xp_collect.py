#!/usr/bin/env python3
"""Assemble the cross-docker-profile transfer ledger."""
import csv, re, pathlib, sys
B = pathlib.Path("/home/ttuser/gdict"); R = B / "runs"; C = B / "cells"
G = 112
PROF = {"debian": "debian-gcc", "conan": "conan-gcc", "brew": "linuxbrew", "fedora": "fedora-clang-libcxx"}
# (compiler family, stdlib) per profile
ENV = {"debian-gcc": ("gcc-12", "libstdc++-12"), "conan-gcc": ("gcc-14", "libstdc++-14"),
       "linuxbrew": ("clang-22", "libstdc++-12"), "fedora-clang-libcxx": ("clang-20", "libc++")}
PAIRS = [("conan", "debian"), ("conan", "brew"), ("conan", "fedora"),
         ("fedora", "debian"), ("debian", "brew"), ("debian", "conan")]

def ntu(p, prof):
    return sum(1 for _ in (C / f"{p}__{PROF[prof]}.man").open())

def curve(tag, prefix):
    f = R / f"{tag}.curve.tsv"
    if not f.is_file(): return None
    rows = list(csv.DictReader(f.open(), delimiter="\t"))
    if len(rows) <= prefix: return None
    base = int(rows[prefix-1]["cumulative_wire_bytes"]) if prefix else 0
    test = rows[prefix:]
    return {"wire": int(rows[-1]["cumulative_wire_bytes"]) - base, "tus": len(test),
            "raw": sum(int(r["raw_bytes"]) for r in test),
            "exact": all(r["exact"] == "true" for r in rows)}

def pad(n): return n + (-n) % G

w = csv.writer(sys.stdout, delimiter="\t", lineterminator="\n")
w.writerow(["project", "target_profile", "prior_profile", "compiler_axis", "stdlib_axis",
            "test_tus", "test_raw_bytes", "cold_bytes", "warm_xprofile_bytes",
            "warm_sameprofile_bytes", "gap_closed_pct", "xprofile_over_cold_pct", "decode_exact"])
for p in sys.argv[1:]:
    for tgt, pri in PAIRS:
        cold = curve(f"xp_{p}_{tgt}_cold", 0)
        same = curve(f"xp_{p}_{tgt}_warmsame", pad(ntu(p, tgt)))
        x = curve(f"xp_{p}_{tgt}_from_{pri}", pad(ntu(p, pri)))
        if not (cold and same and x): continue
        gap = (cold["wire"] - x["wire"]) / (cold["wire"] - same["wire"]) * 100
        tc, ts = ENV[PROF[tgt]]; pc, ps = ENV[PROF[pri]]
        w.writerow([p, PROF[tgt], PROF[pri],
                    "same-family" if tc.split("-")[0] == pc.split("-")[0] else "cross-family",
                    "same" if ts == ps else ("version" if ts.split("-")[0] == ps.split("-")[0] else "family"),
                    x["tus"], x["raw"], cold["wire"], x["wire"], same["wire"],
                    round(gap, 2), round(x["wire"] / cold["wire"] * 100, 2),
                    str(cold["exact"] and same["exact"] and x["exact"]).lower()])
