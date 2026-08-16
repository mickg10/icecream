#!/usr/bin/env python3
"""Validate and summarize the balanced C-only-seed stability matrix."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Sequence

from run_seed_stability_matrix import CORPORA, SCENARIOS


EMPTY = "empty-online-k2"
SEED = "pretrained-seed-online-k2"


def equal_corpus_ratio(rows: Sequence[dict], key: str) -> float:
    return len(rows) / sum(1.0 / row[key] for row in rows)


def weighted_ratio(rows: Sequence[dict], wire_key: str) -> float:
    return sum(row["raw_bytes"] for row in rows) / sum(
        row[wire_key] for row in rows
    )


def load_row(path: Path, corpus: str, scenario: str) -> dict:
    report = json.loads(path.read_text())
    rows = {row["name"]: row for row in report["rows"]}
    if set(rows) != {EMPTY, SEED}:
        raise ValueError(f"{path}: expected exactly the two online rows")
    if not rows[EMPTY]["exact"] or not rows[SEED]["exact"]:
        raise ValueError(f"{path}: inexact row")
    if report.get("row_set") != "online" or report.get("pretrained_mode") != "seed-only":
        raise ValueError(f"{path}: wrong row selection")
    empty_curve = rows[EMPTY]["per_tu_curve"]
    seed_curve = rows[SEED]["per_tu_curve"]
    if len(empty_curve) != len(seed_curve) or not empty_curve:
        raise ValueError(f"{path}: curve length differs or is empty")
    for empty_point, seed_point in zip(empty_curve, seed_curve):
        empty_boundary = (
            empty_point["tu"],
            empty_point["raw_bytes"],
            empty_point["cumulative_raw_bytes"],
        )
        seed_boundary = (
            seed_point["tu"],
            seed_point["raw_bytes"],
            seed_point["cumulative_raw_bytes"],
        )
        if empty_boundary != seed_boundary:
            raise ValueError(f"{path}: empty and seed input boundaries differ")
    empty_wire = rows[EMPTY]["charged_wire_bytes"]
    seed_wire = rows[SEED]["charged_wire_bytes"]
    raw_bytes = rows[SEED]["raw_bytes"]
    target = report["target"]
    return {
        "corpus": corpus,
        "scenario": scenario,
        "tus": rows[SEED]["tus"],
        "raw_bytes": raw_bytes,
        "empty_wire_bytes": empty_wire,
        "seed_wire_bytes": seed_wire,
        "empty_ratio": raw_bytes / empty_wire,
        "seed_ratio": raw_bytes / seed_wire,
        "seed_gain_vs_empty_percent": (empty_wire / seed_wire - 1.0) * 100.0,
        "seed_strictly_smaller": seed_wire < empty_wire,
        "seed_published_assets": rows[SEED]["seed_published_assets"],
        "all_published_assets": rows[SEED]["online_published_assets"],
        "final_logical_state_bytes": rows[SEED]["final_logical_state_bytes"],
        "package_sha256": report["initial_package"]["sha256"],
        "affected_tus": target.get("perturbation", {}).get("affected_tus", 0),
        "affected_occurrences": target.get("perturbation", {}).get(
            "affected_occurrences", 0
        ),
        "exact": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--summary-json", required=True)
    args = parser.parse_args()

    rows = [
        load_row(
            args.input_dir / f"{corpus}-{scenario}.json",
            corpus,
            scenario,
        )
        for scenario in SCENARIOS
        for corpus in CORPORA
    ]
    standard = {
        row["corpus"]: row for row in rows if row["scenario"] == "standard"
    }
    for row in rows:
        baseline = standard[row["corpus"]]
        if row["raw_bytes"] != baseline["raw_bytes"]:
            raise ValueError(
                f"{row['corpus']}/{row['scenario']}: total raw bytes changed"
            )
        row["seed_change_vs_standard_percent"] = (
            baseline["seed_wire_bytes"] / row["seed_wire_bytes"] - 1.0
        ) * 100.0
        row["empty_change_vs_standard_percent"] = (
            baseline["empty_wire_bytes"] / row["empty_wire_bytes"] - 1.0
        ) * 100.0

    with open(args.output, "w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=tuple(rows[0]),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)

    scenario_rows: list[dict] = []
    for scenario in SCENARIOS:
        selected = [row for row in rows if row["scenario"] == scenario]
        scenario_rows.append(
            {
                "scenario": scenario,
                "corpora": len(selected),
                "raw_bytes": sum(row["raw_bytes"] for row in selected),
                "empty_wire_bytes": sum(
                    row["empty_wire_bytes"] for row in selected
                ),
                "seed_wire_bytes": sum(row["seed_wire_bytes"] for row in selected),
                "empty_equal_corpus_ratio": equal_corpus_ratio(
                    selected, "empty_ratio"
                ),
                "seed_equal_corpus_ratio": equal_corpus_ratio(
                    selected, "seed_ratio"
                ),
                "empty_byte_weighted_ratio": weighted_ratio(
                    selected, "empty_wire_bytes"
                ),
                "seed_byte_weighted_ratio": weighted_ratio(
                    selected, "seed_wire_bytes"
                ),
                "seed_strict_wins_vs_empty": sum(
                    row["seed_strictly_smaller"] for row in selected
                ),
                "seed_loss_or_tie_corpora": [
                    row["corpus"]
                    for row in selected
                    if not row["seed_strictly_smaller"]
                ],
                "minimum_seed_gain_vs_empty_percent": min(
                    row["seed_gain_vs_empty_percent"] for row in selected
                ),
                "maximum_seed_gain_vs_empty_percent": max(
                    row["seed_gain_vs_empty_percent"] for row in selected
                ),
                "exact_corpora": sum(row["exact"] for row in selected),
            }
        )

    corpus_rows: list[dict] = []
    for corpus in CORPORA:
        selected = [row for row in rows if row["corpus"] == corpus]
        worst = min(selected, key=lambda row: row["seed_change_vs_standard_percent"])
        best = max(selected, key=lambda row: row["seed_change_vs_standard_percent"])
        smallest_gain = min(selected, key=lambda row: row["seed_gain_vs_empty_percent"])
        corpus_rows.append(
            {
                "corpus": corpus,
                "standard_seed_ratio": standard[corpus]["seed_ratio"],
                "minimum_seed_ratio": min(row["seed_ratio"] for row in selected),
                "maximum_seed_ratio": max(row["seed_ratio"] for row in selected),
                "worst_scenario": worst["scenario"],
                "worst_change_vs_standard_percent": worst[
                    "seed_change_vs_standard_percent"
                ],
                "best_scenario": best["scenario"],
                "best_change_vs_standard_percent": best[
                    "seed_change_vs_standard_percent"
                ],
                "minimum_seed_gain_vs_empty_percent": smallest_gain[
                    "seed_gain_vs_empty_percent"
                ],
                "minimum_gain_scenario": smallest_gain["scenario"],
                "seed_wins_vs_empty": sum(
                    row["seed_strictly_smaller"] for row in selected
                ),
                "scenarios": len(selected),
                "exact_scenarios": sum(row["exact"] for row in selected),
            }
        )

    summary = {
        "experiment": "balanced C-only-seed reorder and input-change stability",
        "corpora": len(CORPORA),
        "scenarios": list(SCENARIOS),
        "runs": len(rows),
        "exact_runs": sum(row["exact"] for row in rows),
        "seed_strict_wins_vs_empty": sum(
            row["seed_strictly_smaller"] for row in rows
        ),
        "seed_loss_or_tie_runs": [
            f"{row['corpus']}/{row['scenario']}"
            for row in rows
            if not row["seed_strictly_smaller"]
        ],
        "scenario_summary": scenario_rows,
        "corpus_summary": corpus_rows,
    }
    Path(args.summary_json).write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
