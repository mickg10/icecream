#!/usr/bin/env python3
"""Summarize complementary exact half-cold Region-cache executions."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path

from summarize_complete_codec50 import CATEGORIES, CORPORA, aggregate
from summarize_mixed_region_codec import MIXED_FIELDS, parse_mixed


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


def parse_key_map(path: Path, expected_bit: int) -> dict[str, object]:
    row = parse_mixed(path)
    found = KEY_PATTERN.search(path.read_text())
    if found is None:
        raise ValueError(f"{path}: missing key-map ledger")
    row.update(dict(zip(KEY_FIELDS, map(int, found.groups()))))
    if int(row["half_cold_bit"]) != expected_bit:
        raise ValueError(f"{path}: expected half-cold bit {expected_bit}")
    if (
        int(row["association_wire_bytes"])
        + int(row["missing_reply_wire_bytes"])
        != int(row["missing_wire_bytes"])
    ):
        raise ValueError(f"{path}: association/reply split does not close missing leg")
    if int(row["associated_regions"]) != int(row["regions"]):
        raise ValueError(f"{path}: not every dense Region id was associated")
    return row


def half_aggregate(rows: list[dict[str, object]]) -> dict[str, object]:
    result = aggregate(rows)
    raw = int(result["raw_bytes"])
    total = int(result["total_wire_bytes"])
    result["half_cold_200_allowance_bytes"] = raw / 200
    result["gap_to_half_cold_200_bytes"] = total - raw / 200
    result["preloaded_regions"] = sum(
        int(row["preloaded_regions"]) for row in rows
    )
    result["preloaded_raw_bytes"] = sum(
        int(row["preloaded_raw_bytes"]) for row in rows
    )
    result["associated_regions"] = sum(
        int(row["associated_regions"]) for row in rows
    )
    result["association_wire_bytes"] = sum(
        int(row["association_wire_bytes"]) for row in rows
    )
    result["missing_reply_wire_bytes"] = sum(
        int(row["missing_reply_wire_bytes"]) for row in rows
    )
    result["minimum_corpus_ratio"] = min(
        int(row["raw_bytes"]) / int(row["total_wire_bytes"]) for row in rows
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--p24-dir", type=Path, required=True)
    parser.add_argument("--keymap-cold-dir", type=Path, required=True)
    parser.add_argument("--bit0-dir", type=Path, required=True)
    parser.add_argument("--bit1-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    cold_rows: list[dict[str, object]] = []
    keymap_cold_rows: list[dict[str, object]] = []
    bit_rows: dict[int, list[dict[str, object]]] = {0: [], 1: []}
    per_corpus: list[dict[str, object]] = []
    for corpus in CORPORA:
        cold = parse_mixed(args.p24_dir / f"{corpus}.log")
        keymap_cold = parse_key_map(
            args.keymap_cold_dir / f"{corpus}.log", -1
        )
        halves = {
            0: parse_key_map(args.bit0_dir / f"{corpus}.log", 0),
            1: parse_key_map(args.bit1_dir / f"{corpus}.log", 1),
        }
        for field in (
            "tus",
            "raw_bytes",
            "regions",
            "region_occurrences",
            "distinct_lines",
        ):
            if keymap_cold[field] != cold[field]:
                raise ValueError(f"{corpus}: key-map cold {field} differs")
        for category in ("root", "line_def", "region_def", "block_def", "path_def", "framing"):
            field = f"{category}_wire_bytes"
            if keymap_cold[field] != cold[field]:
                raise ValueError(f"{corpus}: key-map cold {category} leg changed")
        if (
            int(keymap_cold["missing_reply_wire_bytes"])
            != int(cold["missing_wire_bytes"])
            or int(keymap_cold["total_wire_bytes"])
            != int(cold["total_wire_bytes"])
            + int(keymap_cold["association_wire_bytes"])
        ):
            raise ValueError(f"{corpus}: key-map cold accounting differs")
        for bit, half in halves.items():
            for field in (
                "tus",
                "raw_bytes",
                "regions",
                "region_occurrences",
                "distinct_lines",
                "associated_regions",
            ):
                reference = cold["regions"] if field == "associated_regions" else cold[field]
                if half[field] != reference:
                    raise ValueError(f"{corpus}: bit {bit} {field} differs")
            for category in ("root", "block_def"):
                field = f"{category}_wire_bytes"
                if half[field] != cold[field]:
                    raise ValueError(f"{corpus}: bit {bit} {category} leg changed")
        if (
            int(halves[0]["preloaded_regions"])
            + int(halves[1]["preloaded_regions"])
            != int(cold["regions"])
        ):
            raise ValueError(f"{corpus}: complementary preload does not partition Regions")
        if halves[0]["association_wire_bytes"] != halves[1]["association_wire_bytes"]:
            raise ValueError(f"{corpus}: association wire depends on preload complement")
        if (
            halves[0]["association_wire_bytes"]
            != keymap_cold["association_wire_bytes"]
        ):
            raise ValueError(f"{corpus}: association wire depends on cache temperature")

        cold_rows.append(cold)
        keymap_cold_rows.append(keymap_cold)
        for bit in (0, 1):
            bit_rows[bit].append(halves[bit])
        raw = int(cold["raw_bytes"])
        item: dict[str, object] = {
            "corpus": corpus,
            "tus": cold["tus"],
            "raw_bytes": raw,
            "regions": cold["regions"],
            "exact": True,
            "p24_cold_wire_bytes": cold["total_wire_bytes"],
            "p24_cold_ratio": raw / int(cold["total_wire_bytes"]),
            "keymap_cold_wire_bytes": keymap_cold["total_wire_bytes"],
            "keymap_cold_ratio": raw / int(keymap_cold["total_wire_bytes"]),
            "keymap_cold_association_wire_bytes": keymap_cold[
                "association_wire_bytes"
            ],
            "keymap_cold_log_sha256": keymap_cold["log_sha256"],
        }
        for bit in (0, 1):
            half = halves[bit]
            prefix = f"bit{bit}_"
            item.update(
                {
                    f"{prefix}wire_bytes": half["total_wire_bytes"],
                    f"{prefix}ratio": raw / int(half["total_wire_bytes"]),
                    f"{prefix}pipeline_gbps": half["pipeline_gbps"],
                    f"{prefix}preloaded_regions": half["preloaded_regions"],
                    f"{prefix}preloaded_raw_bytes": half["preloaded_raw_bytes"],
                    f"{prefix}association_wire_bytes": half[
                        "association_wire_bytes"
                    ],
                    f"{prefix}missing_reply_wire_bytes": half[
                        "missing_reply_wire_bytes"
                    ],
                    f"{prefix}log_sha256": half["log_sha256"],
                }
            )
            for category in CATEGORIES[:-1]:
                item[f"{prefix}{category}_wire_bytes"] = half[
                    f"{category}_wire_bytes"
                ]
            for field in MIXED_FIELDS:
                item[f"{prefix}{field}"] = half[field]
        item["harder_half_bit"] = min(
            (0, 1), key=lambda bit: float(item[f"bit{bit}_ratio"])
        )
        item["harder_half_ratio"] = min(
            float(item["bit0_ratio"]), float(item["bit1_ratio"])
        )
        per_corpus.append(item)

    cold = aggregate(cold_rows)
    keymap_cold = half_aggregate(keymap_cold_rows)
    halves = {f"bit{bit}": half_aggregate(bit_rows[bit]) for bit in (0, 1)}
    report = {
        "experiment": "P25 exact complementary half-cold Region-cache execution",
        "scope": (
            "one C/F conversation; stable C-cache u64 keys associated to dense u32 ids; "
            "F preloads the exact key&1 complement, binds its own hits, returns the "
            "missing ids, decodes fills, expands Roots/Blocks, and reconstructs every .ii byte"
        ),
        "half_cold_definition": (
            "two complementary partitions by stable Region key low bit; each holds half "
            "the Region objects by count, while retained raw bytes may be skewed"
        ),
        "full_file_reconstruction_complete": True,
        "key_association_and_missing_exchange_complete": True,
        "total_protocol_acceptance_complete": False,
        "cold_p24_without_key_map": cold,
        "complete_keymap_cold": keymap_cold,
        "halves": halves,
        "both_aggregate_half_cold_200_pass": all(
            float(row["weighted_ratio"]) >= 200 for row in halves.values()
        ),
        "both_minimum_pipeline_1gbps_pass": all(
            float(row["minimum_pipeline_gbps"]) >= 1 for row in halves.values()
        ),
        "per_corpus_both_half_200_pass_count": sum(
            float(row["harder_half_ratio"]) >= 200 for row in per_corpus
        ),
        "remaining_acceptance_blocks": [
            "cold key-map execution and cold-400 closure",
            "actual C/F socket framing and ordered-lane lifecycle",
            "bounded cache eviction and multi-F assignment",
            "reorder/change/revert replay over persistent keys and online state",
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
