#!/usr/bin/env python3
"""Integrate complete prior-root rows with the P18/P21 exact ledgers."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from run_prior_root_matrix import CORPORA, ROW


def harmonic(values: list[float]) -> float:
    return len(values) / sum(1 / value for value in values)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--baseline-summary", type=Path, required=True)
    parser.add_argument("--line-summary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    baseline_report = json.loads(args.baseline_summary.read_text())
    line_report = json.loads(args.line_summary.read_text())
    baseline = {row["corpus"]: row for row in baseline_report["selected"]}
    line = {row["corpus"]: row for row in line_report["per_corpus"]}
    if set(baseline) != set(CORPORA) or set(line) != set(CORPORA):
        raise ValueError("baseline or Line ledger does not contain the balanced corpora")

    rows = []
    for corpus in CORPORA:
        report = json.loads(
            (args.input_dir / f"{corpus}-standard-root.json").read_text()
        )
        candidates = [row for row in report["rows"] if row["name"] == ROW]
        if len(candidates) != 1 or not candidates[0]["exact"]:
            raise ValueError(f"missing exact complete prior-root row: {corpus}")
        root = candidates[0]
        prior = baseline[corpus]
        lines = line[corpus]
        if not prior["exact"] or not lines["exact"]:
            raise ValueError(f"inexact source ledger: {corpus}")
        if not (root["raw_bytes"] == prior["raw_bytes"] == lines["raw_bytes"]):
            raise ValueError(f"raw-byte boundary differs: {corpus}")
        selected_structure = min(
            prior["charged_wire_bytes"], root["charged_wire_bytes"]
        )
        line_wire = lines["generated_array_line_wire_bytes"]
        cold_wire = selected_structure + line_wire
        half_wire = selected_structure + line_wire / 2
        raw = root["raw_bytes"]
        rows.append(
            {
                "corpus": corpus,
                "tus": root["tus"],
                "raw_bytes": raw,
                "p18_structural_wire_bytes": prior["charged_wire_bytes"],
                "prior_root_structural_wire_bytes": root["charged_wire_bytes"],
                "selected_structural_wire_bytes": selected_structure,
                "structural_saving_bytes": prior["charged_wire_bytes"]
                - selected_structure,
                "root_selected": root["charged_wire_bytes"]
                < prior["charged_wire_bytes"],
                "root_copy_selected_tus": root["root_copy_selected_tus"],
                "root_copy_copies": root["root_copy_copies"],
                "root_copy_copied_regions": root["root_copy_copied_regions"],
                "root_copy_receiver_state_bytes": root[
                    "root_copy_receiver_state_bytes"
                ],
                "root_copy_encoder_state_bytes": root[
                    "root_copy_encoder_state_bytes"
                ],
                "p21_line_wire_bytes": line_wire,
                "cold_wire_bytes": cold_wire,
                "cold_ratio": raw / cold_wire,
                "half_cold_projection_wire_bytes": half_wire,
                "half_cold_projection_ratio": raw / half_wire,
                "exact": True,
            }
        )

    raw = sum(row["raw_bytes"] for row in rows)
    old_structure = sum(row["p18_structural_wire_bytes"] for row in rows)
    root_structure = sum(row["prior_root_structural_wire_bytes"] for row in rows)
    selected_structure = sum(row["selected_structural_wire_bytes"] for row in rows)
    line_wire = sum(row["p21_line_wire_bytes"] for row in rows)
    cold_wire = selected_structure + line_wire
    half_wire = selected_structure + line_wire / 2
    summary = {
        "experiment": "complete exact P22 prior-root integration",
        "corpora": len(rows),
        "tus": sum(row["tus"] for row in rows),
        "raw_bytes": raw,
        "p18_structural_wire_bytes": old_structure,
        "prior_root_seed_structural_wire_bytes": root_structure,
        "selected_structural_wire_bytes": selected_structure,
        "structural_saving_bytes": old_structure - selected_structure,
        "structural_saving_fraction": 1 - selected_structure / old_structure,
        "root_win_corpora": sum(row["root_selected"] for row in rows),
        "structural_weighted_ratio": raw / selected_structure,
        "structural_equal_corpus_ratio": harmonic(
            [row["raw_bytes"] / row["selected_structural_wire_bytes"] for row in rows]
        ),
        "p21_line_wire_bytes": line_wire,
        "cold_wire_bytes": cold_wire,
        "cold_allowance_bytes": raw / 400,
        "cold_excess_bytes": cold_wire - raw / 400,
        "cold_weighted_ratio": raw / cold_wire,
        "cold_equal_corpus_ratio": harmonic([row["cold_ratio"] for row in rows]),
        "cold_400_corpora": sum(row["cold_ratio"] >= 400 for row in rows),
        "half_cold_projection_wire_bytes": half_wire,
        "half_cold_weighted_ratio": raw / half_wire,
        "half_cold_equal_corpus_ratio": harmonic(
            [row["half_cold_projection_ratio"] for row in rows]
        ),
        "half_cold_200_corpora": sum(
            row["half_cold_projection_ratio"] >= 200 for row in rows
        ),
        "maximum_root_receiver_state_bytes": max(
            row["root_copy_receiver_state_bytes"] for row in rows
        ),
        "maximum_root_encoder_state_bytes": max(
            row["root_copy_encoder_state_bytes"] for row in rows
        ),
        "exact_corpora": sum(row["exact"] for row in rows),
        "per_corpus": rows,
    }
    args.output.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    with args.tsv.open("w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=tuple(rows[0]),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps({key: value for key, value in summary.items() if key != "per_corpus"}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
