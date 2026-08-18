#!/usr/bin/env python3
"""Per-run boundary ledger for the verified-44 cross-profile sweep."""
import csv, pathlib

B = pathlib.Path("/home/ttuser/gdict")
C = B / "cells44"
G = 112
PROF = {"debian": "debian-gcc", "conan": "conan-gcc",
        "brew": "linuxbrew", "fedora": "fedora-clang-libcxx"}
CONDS = [("cold", None), ("warmsame", "self"), ("from_debian", "debian"),
         ("from_brew", "brew"), ("from_conan", "conan"), ("from_fedora", "fedora")]
PROJECTS = ["catch2", "cereal", "eigen", "fmt", "leveldb", "nlohmann-json",
            "opencv", "range-v3", "re2", "rocksdb", "spdlog", "abseil"]

print("run\tprefix_tus\ttest_tus\tprefix_cum_wire\ttotal_cum_wire\ttest_wire\texact_all\texact_test")
for p in PROJECTS:
    for tgt in ("conan", "fedora", "debian"):
        for cond, pri in CONDS:
            tag = f"x44_{p}_{tgt}_{cond}"
            f = B / "runs" / f"{tag}.curve.tsv"
            if not f.is_file():
                continue
            src = tgt if pri == "self" else pri
            n = 0
            if pri is not None:
                n = sum(1 for _ in (C / f"{p}__{PROF[src]}.man").open())
                n += (-n) % G
            rows = list(csv.DictReader(f.open(), delimiter="\t"))
            base = int(rows[n - 1]["cumulative_wire_bytes"]) if n else 0
            end = int(rows[-1]["cumulative_wire_bytes"])
            test = rows[n:]
            ea = all(r["exact"] == "true" for r in rows)
            et = all(r["exact"] == "true" for r in test)
            print(f"{tag}\t{n}\t{len(test)}\t{base}\t{end}\t{end - base}\t{ea}\t{et}")
