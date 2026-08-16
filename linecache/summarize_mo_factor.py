#!/usr/bin/env python3
"""Summarize P27's exact causal GNU MO catalog factor."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path

from summarize_complete_codec50 import CORPORA, aggregate
from summarize_half_cold_codec import parse_key_map


STATES = (("cold", -1), ("bit0", 0), ("bit1", 1))
IDENTITY_FIELDS = (
    "tus",
    "raw_bytes",
    "regions",
    "region_occurrences",
    "distinct_lines",
)
UNCHANGED_CATEGORIES = ("root", "region_def", "block_def", "path_def", "missing", "framing")
MO_PATTERN = re.compile(
    r"MO factor: policy_tus=(\d+) candidate_wire=(\d+) selected_tus=(\d+) "
    r"members=(\d+) member_raw=(\d+) new_originals=(\d+) "
    r"C_dictionary=(\d+) F_dictionary=(\d+)"
    r"(?: C_string_bytes=(\d+) F_string_bytes=(\d+))?"
)
MO_FIELDS = (
    "mo_policy_tus",
    "mo_candidate_wire_bytes",
    "mo_selected_tus",
    "mo_members",
    "mo_member_raw_bytes",
    "mo_new_originals",
    "mo_c_dictionary",
    "mo_f_dictionary",
    "mo_c_string_bytes",
    "mo_f_string_bytes",
)


def corpus_log(directory: Path, corpus: str) -> Path:
    named = directory / f"{corpus}.log"
    if named.exists():
        return named
    return directory / f"corpus{CORPORA.index(corpus) + 1}.log"


def parse_p27(path: Path, expected_bit: int) -> dict[str, object]:
    row = parse_key_map(path, expected_bit)
    found = MO_PATTERN.search(path.read_text())
    if found is None:
        raise ValueError(f"{path}: missing MO factor ledger")
    values = [int(value) if value is not None else 0 for value in found.groups()]
    row.update(dict(zip(MO_FIELDS, values)))
    if row["mo_c_dictionary"] != row["mo_f_dictionary"]:
        raise ValueError(f"{path}: C/F MO dictionary counts differ")
    if row["mo_c_string_bytes"] != row["mo_f_string_bytes"]:
        raise ValueError(f"{path}: C/F MO string-byte counts differ")
    if int(row["mo_selected_tus"]) > int(row["mo_policy_tus"]):
        raise ValueError(f"{path}: selected more MO TUs than the policy admitted")
    return row


def sum_fields(rows: list[dict[str, object]], fields: tuple[str, ...]) -> dict[str, int]:
    return {field: sum(int(row[field]) for row in rows) for field in fields}


def target(state: str, aggregate_row: dict[str, object]) -> dict[str, object]:
    divisor = 400 if state == "cold" else 200
    raw = int(aggregate_row["raw_bytes"])
    wire = int(aggregate_row["total_wire_bytes"])
    allowance = raw / divisor
    return {
        "ratio": divisor,
        "allowance_bytes": allowance,
        "gap_bytes": wire - allowance,
        "passed": wire <= allowance,
    }


def speed_row(path: Path) -> dict[str, object]:
    row = parse_key_map(path, -1)
    return {
        "path": str(path),
        "wire_bytes": row["total_wire_bytes"],
        "encode_gbps": row["encode_gbps"],
        "decode_gbps": row["decode_gbps"],
        "pipeline_gbps": row["pipeline_gbps"],
        "log_sha256": row["log_sha256"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    for state, _ in STATES:
        parser.add_argument(f"--p27-{state}-dir", type=Path, required=True)
        parser.add_argument(f"--godot-p26-{state}-log", type=Path, required=True)
    parser.add_argument("--speed-log", action="append", type=Path, default=[])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    summaries: dict[str, object] = {}
    per_corpus: list[dict[str, object]] = [{"corpus": corpus} for corpus in CORPORA]
    for state, bit in STATES:
        directory = getattr(args, f"p27_{state}_dir")
        candidate = [parse_p27(corpus_log(directory, corpus), bit) for corpus in CORPORA]
        control_path = getattr(args, f"godot_p26_{state}_log")
        godot_control = parse_key_map(control_path, bit)
        baseline: list[dict[str, object]] = []
        for corpus, row, item in zip(CORPORA, candidate, per_corpus):
            reference = godot_control if corpus == "godot" else row
            for field in IDENTITY_FIELDS:
                if reference[field] != row[field]:
                    raise ValueError(f"{state}/{corpus}: {field} differs")
            for category in UNCHANGED_CATEGORIES:
                field = f"{category}_wire_bytes"
                if reference[field] != row[field]:
                    raise ValueError(f"{state}/{corpus}: unrelated {category} differs")
            if corpus != "godot" and int(row["blob_count"]):
                raise ValueError(f"{state}/{corpus}: unexpected blob members")
            if corpus != "godot" and any(int(row[field]) for field in MO_FIELDS):
                raise ValueError(f"{state}/{corpus}: zero-blob corpus changed MO state")
            baseline.append(reference)
            raw = int(row["raw_bytes"])
            old_wire = int(reference["total_wire_bytes"])
            new_wire = int(row["total_wire_bytes"])
            item.update(
                {
                    "tus": row["tus"],
                    "raw_bytes": raw,
                    f"{state}_p26_wire_bytes": old_wire,
                    f"{state}_p27_wire_bytes": new_wire,
                    f"{state}_saving_bytes": old_wire - new_wire,
                    f"{state}_p26_ratio": raw / old_wire,
                    f"{state}_p27_ratio": raw / new_wire,
                    f"{state}_p27_pipeline_gbps": row["pipeline_gbps"],
                    f"{state}_blob_count": row["blob_count"],
                    f"{state}_blob_wire_bytes": row["blob_wire_bytes"],
                    f"{state}_mo_selected_tus": row["mo_selected_tus"],
                    f"{state}_mo_members": row["mo_members"],
                    f"{state}_mo_member_raw_bytes": row["mo_member_raw_bytes"],
                    f"{state}_mo_new_originals": row["mo_new_originals"],
                    f"{state}_log_sha256": row["log_sha256"],
                }
            )

        baseline_aggregate = aggregate(baseline)
        candidate_aggregate = aggregate(candidate)
        summaries[state] = {
            "same_machine_p26": baseline_aggregate,
            "p27": candidate_aggregate,
            "saving_bytes": int(baseline_aggregate["total_wire_bytes"])
            - int(candidate_aggregate["total_wire_bytes"]),
            "mo": sum_fields(candidate, MO_FIELDS[:6]),
            "final_dictionary": {
                "entries": max(int(row["mo_c_dictionary"]) for row in candidate),
                "string_bytes_per_side": max(
                    int(row["mo_c_string_bytes"]) for row in candidate
                ),
            },
            "target": target(state, candidate_aggregate),
        }

    speeds = [speed_row(path) for path in args.speed_log]
    report = {
        "experiment": "P27 exact causal GNU MO catalog factor beneath P26",
        "scope": (
            "canonical MO members only; explicit original-string definitions and ids, "
            "exact translations and ordinary-member residual; C commits dictionary entries "
            "only when mode 4 is selected; F reconstructs each catalog byte-for-byte"
        ),
        "states": summaries,
        "complete_runs": len(STATES) * len(CORPORA),
        "exact_runs": len(STATES) * len(CORPORA),
        "speed_repetitions": speeds,
        "minimum_designated_pipeline_gbps": min(
            (float(row["pipeline_gbps"]) for row in speeds), default=None
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
    print(json.dumps({key: value for key, value in report.items() if key != "per_corpus"}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
