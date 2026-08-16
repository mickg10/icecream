#!/usr/bin/env python3
"""Summarize full-file reconstruction S1, P9, and P21 cold ledgers."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from summarize_complete_codec50 import CATEGORIES, CORPORA, aggregate, parse_log


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--s1-dir", type=Path, required=True)
    parser.add_argument("--p9-dir", type=Path, required=True)
    parser.add_argument("--p21-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    variants = {"s1": [], "p9": [], "p21": []}
    per_corpus = []
    directories = {"s1": args.s1_dir, "p9": args.p9_dir, "p21": args.p21_dir}
    for corpus in CORPORA:
        rows = {
            name: parse_log(directory / f"{corpus}.log")
            for name, directory in directories.items()
        }
        for field in ("tus", "raw_bytes", "regions", "region_occurrences", "distinct_lines"):
            if len({row[field] for row in rows.values()}) != 1:
                raise ValueError(f"{corpus}: {field} differs between variants")
        if not all(bool(row["exact"]) for row in rows.values()):
            raise ValueError(f"{corpus}: an inexact row survived parsing")
        for category in (
            "root",
            "region_def",
            "block_def",
            "path_def",
            "missing",
            "framing",
        ):
            field = f"{category}_wire_bytes"
            if rows["p9"][field] != rows["p21"][field]:
                raise ValueError(f"{corpus}: P9/P21 {category} leg differs")
        for category in ("root", "block_def", "missing", "framing"):
            field = f"{category}_wire_bytes"
            if rows["s1"][field] != rows["p9"][field]:
                raise ValueError(f"{corpus}: S1/P9 unrelated {category} leg differs")
        for name, row in rows.items():
            variants[name].append(row)
        raw = int(rows["s1"]["raw_bytes"])
        result: dict[str, object] = {
            "corpus": corpus,
            "tus": rows["s1"]["tus"],
            "raw_bytes": raw,
            "exact": True,
        }
        for name, row in rows.items():
            total = int(row["total_wire_bytes"])
            result[f"{name}_wire_bytes"] = total
            result[f"{name}_ratio"] = raw / total
            for category in CATEGORIES[:-1]:
                result[f"{name}_{category}_wire_bytes"] = row[
                    f"{category}_wire_bytes"
                ]
        result["p9_saving_bytes"] = int(result["s1_wire_bytes"]) - int(
            result["p9_wire_bytes"]
        )
        result["p21_saving_vs_p9_bytes"] = int(result["p9_wire_bytes"]) - int(
            result["p21_wire_bytes"]
        )
        result["p21_saving_vs_s1_bytes"] = int(result["s1_wire_bytes"]) - int(
            result["p21_wire_bytes"]
        )
        per_corpus.append(result)

    aggregates = {name: aggregate(rows) for name, rows in variants.items()}
    report = {
        "experiment": "full-file reconstruction S1 versus P9 versus P21",
        "scope": (
            "one empty-receiver dense-ID conversation; exact full-.ii reconstruction "
            "with Lines, Region composition, Roots/Blocks, paths, missing lists, and framing"
        ),
        "full_file_reconstruction_complete": True,
        "total_protocol_acceptance_complete": False,
        "remaining_acceptance_blocks": [
            "generation-key and dense-map accounting for nonempty/half-cold receiver state",
            "actual C/F socket framing for the integrated P21 streams",
            "complete complementary half-cold object-cache execution",
        ],
        "aggregates": aggregates,
        "p9_saving_bytes": int(aggregates["s1"]["total_wire_bytes"])
        - int(aggregates["p9"]["total_wire_bytes"]),
        "p21_saving_vs_p9_bytes": int(aggregates["p9"]["total_wire_bytes"])
        - int(aggregates["p21"]["total_wire_bytes"]),
        "p21_saving_vs_s1_bytes": int(aggregates["s1"]["total_wire_bytes"])
        - int(aggregates["p21"]["total_wire_bytes"]),
        "per_corpus": per_corpus,
    }
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    with args.tsv.open("w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=tuple(per_corpus[0]),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(per_corpus)
    print(
        json.dumps(
            {key: value for key, value in report.items() if key != "per_corpus"},
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
