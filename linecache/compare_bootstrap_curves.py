#!/usr/bin/env python3
"""Compare cold empty-start and frozen-package online learning curves."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from summarize_online_bootstrap import parse_input


EMPTY = "empty-online-k2"
PRETRAINED = "pretrained-online-k2"


def equal_corpus_ratio(ratios: list[float]) -> float:
    return len(ratios) / sum(1.0 / ratio for ratio in ratios)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", action="append", type=parse_input, required=True)
    parser.add_argument("--invalid-pretrained", nargs="*", default=())
    parser.add_argument("--checkpoints", nargs="+", type=int, default=(1, 5, 10, 25, 50, 100, 200))
    parser.add_argument("--output", required=True)
    parser.add_argument("--summary-json")
    args = parser.parse_args()

    reports_by_corpus: dict[str, list[Path]] = {}
    for corpus, path in args.input:
        reports_by_corpus.setdefault(corpus, []).append(path)

    invalid = set(args.invalid_pretrained)
    comparisons: list[dict] = []
    loaded_rows: dict[str, dict[str, dict]] = {}
    package_hashes: set[str] = set()
    package_wire_bytes: set[int] = set()
    for corpus, paths in reports_by_corpus.items():
        if corpus in invalid:
            continue
        rows: dict[str, dict] = {}
        for path in paths:
            report = json.loads(path.read_text())
            package_hashes.add(report["initial_package"]["sha256"])
            for row in report["rows"]:
                if row["name"] not in (EMPTY, PRETRAINED):
                    continue
                if row["name"] in rows:
                    raise ValueError(f"{corpus}: duplicate {row['name']} row")
                rows[row["name"]] = row
        if set(rows) != {EMPTY, PRETRAINED}:
            raise ValueError(f"{corpus}: empty and pretrained online rows are required")
        empty = rows[EMPTY]
        pretrained = rows[PRETRAINED]
        if (empty["tus"], empty["raw_bytes"]) != (
            pretrained["tus"], pretrained["raw_bytes"]
        ):
            raise ValueError(f"{corpus}: row input boundaries differ")
        if not empty["exact"] or not pretrained["exact"]:
            raise ValueError(f"{corpus}: inexact row")
        if empty.get("publication") != pretrained.get("publication"):
            raise ValueError(f"{corpus}: publication policies differ")

        package_wire_bytes.add(pretrained["initial_model_wire_bytes"])
        empty_curve = empty["per_tu_curve"]
        pretrained_curve = pretrained["per_tu_curve"]
        wins = [
            package["cumulative_charged_bytes"] <= cold["cumulative_charged_bytes"]
            for cold, package in zip(empty_curve, pretrained_curve, strict=True)
        ]
        first_win = next((ordinal for ordinal, win in enumerate(wins, 1) if win), None)
        stable_win = None
        suffix_wins = True
        for index in range(len(wins) - 1, -1, -1):
            suffix_wins = suffix_wins and wins[index]
            if suffix_wins:
                stable_win = index + 1

        comparison = {
            "corpus": corpus,
            "tus": empty["tus"],
            "raw_bytes": empty["raw_bytes"],
            "publication": empty.get("publication", "promotion"),
            "package_wire_bytes": pretrained["initial_model_wire_bytes"],
            "empty_wire_bytes": empty["charged_wire_bytes"],
            "pretrained_wire_bytes": pretrained["charged_wire_bytes"],
            "empty_ratio": empty["charged_ratio"],
            "pretrained_ratio": pretrained["charged_ratio"],
            "pretrained_delta_percent": (
                pretrained["charged_ratio"] / empty["charged_ratio"] - 1.0
            ) * 100.0,
            "first_win_tu": first_win,
            "stable_win_tu": stable_win,
            "winner": (
                "pretrained"
                if pretrained["charged_wire_bytes"] < empty["charged_wire_bytes"]
                else "empty"
            ),
            "exact": True,
        }
        comparisons.append(comparison)
        loaded_rows[corpus] = rows

    columns = tuple(comparisons[0])
    with open(args.output, "w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=columns,
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(comparisons)

    checkpoints: list[dict] = []
    for checkpoint in args.checkpoints:
        empty_ratios: list[float] = []
        pretrained_ratios: list[float] = []
        pretrained_wins = 0
        for rows in loaded_rows.values():
            empty = rows[EMPTY]
            pretrained = rows[PRETRAINED]
            if empty["tus"] < checkpoint:
                continue
            cold = empty["per_tu_curve"][checkpoint - 1]
            package = pretrained["per_tu_curve"][checkpoint - 1]
            empty_ratios.append(cold["cumulative_charged_ratio"])
            pretrained_ratios.append(package["cumulative_charged_ratio"])
            pretrained_wins += int(
                package["cumulative_charged_bytes"]
                <= cold["cumulative_charged_bytes"]
            )
        checkpoints.append({
            "tu": checkpoint,
            "corpora": len(empty_ratios),
            "empty_equal_corpus_ratio": equal_corpus_ratio(empty_ratios),
            "pretrained_equal_corpus_ratio": equal_corpus_ratio(pretrained_ratios),
            "pretrained_delta_percent": (
                equal_corpus_ratio(pretrained_ratios)
                / equal_corpus_ratio(empty_ratios)
                - 1.0
            ) * 100.0,
            "pretrained_wins": pretrained_wins,
        })

    empty_final = [row["empty_ratio"] for row in comparisons]
    pretrained_final = [row["pretrained_ratio"] for row in comparisons]
    summary = {
        "corpora": len(comparisons),
        "package_sha256": sorted(package_hashes),
        "package_wire_bytes": sorted(package_wire_bytes),
        "pretrained_wins": sum(row["winner"] == "pretrained" for row in comparisons),
        "empty_wins": sum(row["winner"] == "empty" for row in comparisons),
        "final_empty_equal_corpus_ratio": equal_corpus_ratio(empty_final),
        "final_pretrained_equal_corpus_ratio": equal_corpus_ratio(pretrained_final),
        "final_pretrained_delta_percent": (
            equal_corpus_ratio(pretrained_final) / equal_corpus_ratio(empty_final) - 1.0
        ) * 100.0,
        "checkpoints": checkpoints,
        "comparisons": comparisons,
    }
    if args.summary_json:
        Path(args.summary_json).write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n"
        )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
