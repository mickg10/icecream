#!/usr/bin/env python3
"""Summarize the exact direct-ordinal cold and cache-half executions."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path

from summarize_complete_codec50 import CORPORA, aggregate
from summarize_mixed_region_codec import parse_mixed


KEY_PATTERN = re.compile(
    r"key map: half_cold_bit=(-?\d+) preloaded_regions=(\d+) "
    r"preloaded_raw_bytes=(\d+) associated_regions=(\d+) "
    r"association_wire=(\d+) missing_reply_wire=(\d+)"
)

KEY_FIELDS = (
    "half_cold_bit",
    "preloaded_regions",
    "preloaded_raw_bytes",
    "associated_regions",
    "association_wire_bytes",
    "missing_reply_wire_bytes",
)


def parse_direct(path: Path, expected_bit: int) -> dict[str, object]:
    row = parse_mixed(path)
    text = path.read_text()
    if "direct ordinals: generation_latched=1 region_namespaces=1" not in text:
        raise ValueError(f"{path}: missing direct-ordinal execution marker")
    found = KEY_PATTERN.search(text)
    if found is None:
        raise ValueError(f"{path}: missing direct-ordinal cache ledger")
    row.update(dict(zip(KEY_FIELDS, map(int, found.groups()))))
    if int(row["half_cold_bit"]) != expected_bit:
        raise ValueError(f"{path}: expected cache bit {expected_bit}")
    if int(row["associated_regions"]) or int(row["association_wire_bytes"]):
        raise ValueError(f"{path}: direct ordinals retained an association plane")
    if int(row["missing_reply_wire_bytes"]) != int(row["missing_wire_bytes"]):
        raise ValueError(f"{path}: direct NEED does not close the missing category")
    return row


def half_aggregate(rows: list[dict[str, object]]) -> dict[str, object]:
    result = aggregate(rows)
    raw = int(result["raw_bytes"])
    total = int(result["total_wire_bytes"])
    result.update(
        {
            "half_cold_200_allowance_bytes": raw / 200,
            "gap_to_half_cold_200_bytes": total - raw / 200,
            "preloaded_regions": sum(int(row["preloaded_regions"]) for row in rows),
            "preloaded_raw_bytes": sum(
                int(row["preloaded_raw_bytes"]) for row in rows
            ),
            "minimum_corpus_ratio": min(
                int(row["raw_bytes"]) / int(row["total_wire_bytes"])
                for row in rows
            ),
        }
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--p24-dir", type=Path, required=True)
    parser.add_argument("--p25-cold-dir", type=Path, required=True)
    parser.add_argument("--p25-bit0-dir", type=Path, required=True)
    parser.add_argument("--p25-bit1-dir", type=Path, required=True)
    parser.add_argument("--direct-cold-dir", type=Path, required=True)
    parser.add_argument("--direct-bit0-dir", type=Path, required=True)
    parser.add_argument("--direct-bit1-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    variants: dict[str, list[dict[str, object]]] = {
        "p24": [],
        "p25_cold": [],
        "p25_bit0": [],
        "p25_bit1": [],
        "direct_cold": [],
        "direct_bit0": [],
        "direct_bit1": [],
    }
    per_corpus: list[dict[str, object]] = []
    for corpus in CORPORA:
        p24 = parse_mixed(args.p24_dir / f"{corpus}.log")
        p25 = {
            "cold": parse_mixed(args.p25_cold_dir / f"{corpus}.log"),
            "bit0": parse_mixed(args.p25_bit0_dir / f"{corpus}.log"),
            "bit1": parse_mixed(args.p25_bit1_dir / f"{corpus}.log"),
        }
        direct = {
            "cold": parse_direct(args.direct_cold_dir / f"{corpus}.log", -1),
            "bit0": parse_direct(args.direct_bit0_dir / f"{corpus}.log", 0),
            "bit1": parse_direct(args.direct_bit1_dir / f"{corpus}.log", 1),
        }
        for candidate in (*p25.values(), *direct.values()):
            for field in (
                "tus",
                "raw_bytes",
                "regions",
                "region_occurrences",
                "distinct_lines",
            ):
                if candidate[field] != p24[field]:
                    raise ValueError(f"{corpus}: {field} differs")
        for category in ("root", "line_def", "region_def", "path_def"):
            field = f"{category}_wire_bytes"
            if direct["cold"][field] != p24[field]:
                raise ValueError(f"{corpus}: direct cold {category} changed")
        for bit in (0, 1):
            half = direct[f"bit{bit}"]
            for category in ("root", "block_def"):
                field = f"{category}_wire_bytes"
                if half[field] != direct["cold"][field]:
                    raise ValueError(f"{corpus}: direct bit {bit} {category} changed")
        if (
            int(direct["bit0"]["preloaded_regions"])
            + int(direct["bit1"]["preloaded_regions"])
            != int(p24["regions"])
        ):
            raise ValueError(f"{corpus}: cache complements do not partition Regions")

        variants["p24"].append(p24)
        for state, row in p25.items():
            variants[f"p25_{state}"].append(row)
        for state, row in direct.items():
            variants[f"direct_{state}"].append(row)

        raw = int(p24["raw_bytes"])
        item: dict[str, object] = {
            "corpus": corpus,
            "tus": p24["tus"],
            "raw_bytes": raw,
            "regions": p24["regions"],
            "exact": True,
            "p24_wire_bytes": p24["total_wire_bytes"],
            "p25_cold_wire_bytes": p25["cold"]["total_wire_bytes"],
        }
        for state in ("cold", "bit0", "bit1"):
            row = direct[state]
            wire = int(row["total_wire_bytes"])
            item.update(
                {
                    f"direct_{state}_wire_bytes": wire,
                    f"direct_{state}_ratio": raw / wire,
                    f"direct_{state}_pipeline_gbps": row["pipeline_gbps"],
                    f"direct_{state}_preloaded_regions": row["preloaded_regions"],
                    f"direct_{state}_preloaded_raw_bytes": row[
                        "preloaded_raw_bytes"
                    ],
                    f"direct_{state}_log_sha256": row["log_sha256"],
                }
            )
        item["direct_cold_delta_vs_p24_bytes"] = (
            int(direct["cold"]["total_wire_bytes"])
            - int(p24["total_wire_bytes"])
        )
        item["direct_cold_delta_vs_p25_bytes"] = (
            int(direct["cold"]["total_wire_bytes"])
            - int(p25["cold"]["total_wire_bytes"])
        )
        per_corpus.append(item)

    aggregates = {
        name: (
            half_aggregate(rows)
            if name in {"direct_bit0", "direct_bit1"}
            else aggregate(rows)
        )
        for name, rows in variants.items()
    }
    report = {
        "experiment": "M1 direct generation-local typed Region ordinals",
        "scope": (
            "one generation-latched C/F conversation; Root-referenced Blocks decode "
            "before F computes Region NEED; cold and both deterministic cache halves"
        ),
        "full_file_reconstruction_complete": True,
        "direct_ordinal_need_complete": True,
        "association_plane_removed": True,
        "aggregates": aggregates,
        "direct_cold_delta_vs_p24_bytes": int(
            aggregates["direct_cold"]["total_wire_bytes"]
        )
        - int(aggregates["p24"]["total_wire_bytes"]),
        "direct_cold_delta_vs_p25_bytes": int(
            aggregates["direct_cold"]["total_wire_bytes"]
        )
        - int(aggregates["p25_cold"]["total_wire_bytes"]),
        "both_direct_cache_halves_pass_200": all(
            float(aggregates[f"direct_bit{bit}"]["weighted_ratio"]) >= 200
            for bit in (0, 1)
        ),
        "cold_400_pass": float(aggregates["direct_cold"]["weighted_ratio"])
        >= 400,
        "chronological_h200_complete": False,
        "remaining_acceptance_blocks": [
            "explicit public-Line cache identity and public-Line NEED/FILL",
            "independently decodable per-TU semantic frames",
            "chronological C50/H200 plus 50%-snapshot resume",
            "multi-F, bounded eviction, and actual two-process socket execution",
            "cold-400 closure",
        ],
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
