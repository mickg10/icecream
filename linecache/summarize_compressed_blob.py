#!/usr/bin/env python3
"""Summarize P26 exact compressed-blob execution against direct ordinals."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from summarize_complete_codec50 import CATEGORIES, CORPORA, aggregate
from summarize_half_cold_codec import parse_key_map
from summarize_mixed_region_codec import BLOB_FIELDS


STATES = (("cold", -1), ("bit0", 0), ("bit1", 1))
IDENTITY_FIELDS = (
    "tus",
    "raw_bytes",
    "regions",
    "region_occurrences",
    "distinct_lines",
)
UNCHANGED_CATEGORIES = ("root", "block_def", "path_def", "missing", "framing")


def load_state(directory: Path, bit: int) -> list[dict[str, object]]:
    return [parse_key_map(directory / f"{corpus}.log", bit) for corpus in CORPORA]


def blob_totals(rows: list[dict[str, object]]) -> dict[str, int]:
    totals = {
        field: sum(int(row[field]) for row in rows)
        for field in BLOB_FIELDS
        if field != "blob_threads"
    }
    totals["blob_threads"] = max(int(row["blob_threads"]) for row in rows)
    return totals


def target_result(state: str, result: dict[str, object]) -> dict[str, object]:
    raw = int(result["raw_bytes"])
    wire = int(result["total_wire_bytes"])
    divisor = 400 if state == "cold" else 200
    allowance = raw / divisor
    return {
        "target_ratio": divisor,
        "allowance_bytes": allowance,
        "gap_bytes": wire - allowance,
        "passed": wire <= allowance,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    for state, _ in STATES:
        parser.add_argument(f"--direct-{state}-dir", type=Path, required=True)
        parser.add_argument(f"--p26-{state}-dir", type=Path, required=True)
    parser.add_argument("--speed-log", action="append", type=Path, default=[])
    parser.add_argument("--eager-log", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    state_rows: dict[str, dict[str, list[dict[str, object]]]] = {}
    state_summary: dict[str, object] = {}
    per_corpus: list[dict[str, object]] = [
        {"corpus": corpus} for corpus in CORPORA
    ]

    for state, bit in STATES:
        direct = load_state(getattr(args, f"direct_{state}_dir"), bit)
        p26 = load_state(getattr(args, f"p26_{state}_dir"), bit)
        state_rows[state] = {"direct": direct, "p26": p26}
        for corpus, baseline, candidate, item in zip(
            CORPORA, direct, p26, per_corpus
        ):
            for field in IDENTITY_FIELDS:
                if baseline[field] != candidate[field]:
                    raise ValueError(f"{state}/{corpus}: {field} differs")
            for category in UNCHANGED_CATEGORIES:
                field = f"{category}_wire_bytes"
                if baseline[field] != candidate[field]:
                    raise ValueError(f"{state}/{corpus}: unrelated {category} differs")
            if not int(candidate["blob_count"]):
                for category in CATEGORIES:
                    field = f"{category}_wire_bytes"
                    if baseline[field] != candidate[field]:
                        raise ValueError(
                            f"{state}/{corpus}: zero-blob {category} differs"
                        )
            raw = int(candidate["raw_bytes"])
            direct_wire = int(baseline["total_wire_bytes"])
            p26_wire = int(candidate["total_wire_bytes"])
            item.update(
                {
                    "tus": candidate["tus"],
                    "raw_bytes": raw,
                    f"{state}_direct_wire_bytes": direct_wire,
                    f"{state}_p26_wire_bytes": p26_wire,
                    f"{state}_saving_bytes": direct_wire - p26_wire,
                    f"{state}_direct_ratio": raw / direct_wire,
                    f"{state}_p26_ratio": raw / p26_wire,
                    f"{state}_p26_pipeline_gbps": candidate["pipeline_gbps"],
                    f"{state}_blob_count": candidate["blob_count"],
                    f"{state}_blob_deflated_bytes": candidate[
                        "blob_deflated_bytes"
                    ],
                    f"{state}_blob_inflated_bytes": candidate[
                        "blob_inflated_bytes"
                    ],
                    f"{state}_blob_wire_bytes": candidate["blob_wire_bytes"],
                    f"{state}_log_sha256": candidate["log_sha256"],
                }
            )

        direct_aggregate = aggregate(direct)
        p26_aggregate = aggregate(p26)
        state_summary[state] = {
            "direct": direct_aggregate,
            "p26": p26_aggregate,
            "saving_bytes": int(direct_aggregate["total_wire_bytes"])
            - int(p26_aggregate["total_wire_bytes"]),
            "blob": blob_totals(p26),
            "target": target_result(state, p26_aggregate),
            "minimum_corpus_ratio": min(
                int(row["raw_bytes"]) / int(row["total_wire_bytes"])
                for row in p26
            ),
        }

    speed_repetitions = []
    for path in args.speed_log:
        row = parse_key_map(path, -1)
        speed_repetitions.append(
            {
                "path": str(path),
                "wire_bytes": row["total_wire_bytes"],
                "encode_gbps": row["encode_gbps"],
                "decode_gbps": row["decode_gbps"],
                "pipeline_gbps": row["pipeline_gbps"],
                "log_sha256": row["log_sha256"],
            }
        )
    eager = None
    if args.eager_log is not None:
        row = parse_key_map(args.eager_log, -1)
        eager = {
            "path": str(args.eager_log),
            "wire_bytes": row["total_wire_bytes"],
            "encode_gbps": row["encode_gbps"],
            "decode_gbps": row["decode_gbps"],
            "pipeline_gbps": row["pipeline_gbps"],
            "blob_canonical_exact": row["blob_canonical_exact"],
            "blob_corrected": row["blob_corrected"],
            "log_sha256": row["log_sha256"],
        }

    report = {
        "experiment": "P26 exact compressed embedded-DEFLATE members",
        "scope": (
            "direct generation-local ordinals plus per-TU selection between "
            "zstd-LDM-compressed inflated members and untouched original "
            "DEFLATE bytes; exact digest validation and member-only recovery"
        ),
        "states": state_summary,
        "speed_repetitions": speed_repetitions,
        "eager_control": eager,
        "complete_runs": 3 * len(CORPORA),
        "exact_runs": 3 * len(CORPORA),
        "cold_saving_stop_rule_passed": (
            int(state_summary["cold"]["saving_bytes"]) >= 8_000_000
        ),
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
