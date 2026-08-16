#!/usr/bin/env python3
"""Compare empty, installed-package, and C-only-seed startup curves."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path
from typing import Sequence

from summarize_online_bootstrap import parse_input


EMPTY = "empty-online-k2"
INSTALLED = "pretrained-online-k2"
SEED = "pretrained-seed-online-k2"
REQUIRED_ROWS = (EMPTY, INSTALLED, SEED)
DEFAULT_CHECKPOINTS = (1, 5, 10, 25, 50, 100, 200)
CANONICAL_CORPUS_ORDER = (
    "llvm",
    "rocksdb",
    "duckdb",
    "abseil",
    "opencv",
    "godot",
    "fmt",
    "spdlog",
    "catch2",
    "nlohmann-json",
    "range-v3",
    "eigen",
    "re2",
    "leveldb",
    "simdjson",
    "cereal",
)


def equal_corpus_ratio(ratios: Sequence[float]) -> float:
    if not ratios:
        raise ValueError("at least one ratio is required")
    return len(ratios) / sum(1.0 / ratio for ratio in ratios)


def weighted_ratio(points: Sequence[dict]) -> float:
    if not points:
        raise ValueError("at least one point is required")
    return sum(point["raw_bytes"] for point in points) / sum(
        point["wire_bytes"] for point in points
    )


def curve_point(row: dict, ordinal: int) -> dict:
    curve = row["per_tu_curve"]
    if not 0 < ordinal <= len(curve):
        raise ValueError(f"{row['name']}: TU {ordinal} is outside its curve")
    point = curve[ordinal - 1]
    if point["tu"] != ordinal:
        raise ValueError(f"{row['name']}: non-canonical TU ordinal")
    return {
        "raw_bytes": point["cumulative_raw_bytes"],
        "wire_bytes": point["cumulative_charged_bytes"],
        "ratio": point["cumulative_charged_ratio"],
    }


def first_and_stable_strict_win(
    contender: dict,
    reference: dict,
    limit: int,
) -> tuple[int | None, int | None]:
    wins = [
        contender["per_tu_curve"][index]["cumulative_charged_bytes"]
        < reference["per_tu_curve"][index]["cumulative_charged_bytes"]
        for index in range(limit)
    ]
    first = next((index + 1 for index, win in enumerate(wins) if win), None)
    stable = None
    suffix_wins = True
    for index in range(limit - 1, -1, -1):
        suffix_wins = suffix_wins and wins[index]
        if suffix_wins:
            stable = index + 1
    return first, stable


def validate_prefix(corpus: str, rows: dict[str, dict], limit: int) -> None:
    for name in REQUIRED_ROWS:
        row = rows[name]
        if not row["exact"]:
            raise ValueError(f"{corpus}: {name} is inexact")
        if len(row["per_tu_curve"]) < limit:
            raise ValueError(f"{corpus}: {name} is shorter than the seed curve")
    for index in range(limit):
        boundaries = {
            (
                rows[name]["per_tu_curve"][index]["tu"],
                rows[name]["per_tu_curve"][index]["raw_bytes"],
                rows[name]["per_tu_curve"][index]["cumulative_raw_bytes"],
            )
            for name in REQUIRED_ROWS
        }
        if len(boundaries) != 1:
            raise ValueError(f"{corpus}: input boundary differs at TU {index + 1}")


def load_inputs(
    inputs: Sequence[tuple[str, Path]],
) -> tuple[dict[str, dict[str, dict]], dict[str, dict]]:
    rows_by_corpus: dict[str, dict[str, dict]] = {}
    seed_packages: dict[str, dict] = {}
    for corpus, path in inputs:
        report = json.loads(path.read_text())
        rows = rows_by_corpus.setdefault(corpus, {})
        for row in report["rows"]:
            name = row["name"]
            if name not in REQUIRED_ROWS:
                continue
            if name in rows:
                raise ValueError(f"{corpus}: duplicate {name} row")
            rows[name] = row
            if name == SEED:
                if row.get("pretrained_mode") != "seed-only":
                    raise ValueError(f"{corpus}: seed row is not C-only")
                seed_packages[corpus] = report["initial_package"]
    return rows_by_corpus, seed_packages


def corpus_sort_key(corpus: str) -> tuple[int, str]:
    try:
        return CANONICAL_CORPUS_ORDER.index(corpus), corpus
    except ValueError:
        return len(CANONICAL_CORPUS_ORDER), corpus


def compare_corpora(
    rows_by_corpus: dict[str, dict[str, dict]],
    seed_packages: dict[str, dict],
) -> list[dict]:
    comparisons: list[dict] = []
    for corpus in sorted(rows_by_corpus, key=corpus_sort_key):
        rows = rows_by_corpus[corpus]
        missing = set(REQUIRED_ROWS) - set(rows)
        if missing:
            raise ValueError(f"{corpus}: missing rows {sorted(missing)}")
        seed = rows[SEED]
        limit = seed["tus"]
        if limit != len(seed["per_tu_curve"]):
            raise ValueError(f"{corpus}: seed row curve length differs from TU count")
        validate_prefix(corpus, rows, limit)

        points = {name: curve_point(row, limit) for name, row in rows.items()}
        empty_first, empty_stable = first_and_stable_strict_win(
            seed, rows[EMPTY], limit
        )
        installed_first, installed_stable = first_and_stable_strict_win(
            seed, rows[INSTALLED], limit
        )
        seed_point = points[SEED]
        seed_package = seed_packages[corpus]
        comparisons.append(
            {
                "corpus": corpus,
                "screen_tus": limit,
                "raw_bytes": seed_point["raw_bytes"],
                "empty_wire_bytes": points[EMPTY]["wire_bytes"],
                "installed_wire_bytes": points[INSTALLED]["wire_bytes"],
                "seed_wire_bytes": seed_point["wire_bytes"],
                "empty_ratio": points[EMPTY]["ratio"],
                "installed_ratio": points[INSTALLED]["ratio"],
                "seed_ratio": seed_point["ratio"],
                "seed_vs_empty_percent": (
                    seed_point["ratio"] / points[EMPTY]["ratio"] - 1.0
                )
                * 100.0,
                "seed_vs_installed_percent": (
                    seed_point["ratio"] / points[INSTALLED]["ratio"] - 1.0
                )
                * 100.0,
                "seed_first_strict_win_vs_empty_tu": empty_first,
                "seed_stable_strict_win_vs_empty_tu": empty_stable,
                "seed_first_strict_win_vs_installed_tu": installed_first,
                "seed_stable_strict_win_vs_installed_tu": installed_stable,
                "seed_selected_tus": seed["online_selected_tus"],
                "promoted_assets": seed["online_promoted_assets"],
                "seed_published_assets": seed["seed_published_assets"],
                "all_published_assets": seed["online_published_assets"],
                "candidate_observations": seed["candidate_observations"],
                "final_logical_state_bytes": seed["final_logical_state_bytes"],
                "definition_wire_bytes": seed["definition_wire_bytes"],
                "seed_artifact_assets": seed["seed_assets"],
                "seed_artifact_raw_bytes": seed["seed_model_raw_bytes"],
                "seed_artifact_compressed_bytes": seed[
                    "seed_model_compressed_bytes"
                ],
                "seed_artifact_sha256": seed_package["sha256"],
                "seed_strictly_smaller_than_empty": (
                    seed_point["wire_bytes"] < points[EMPTY]["wire_bytes"]
                ),
                "seed_strictly_smaller_than_installed": (
                    seed_point["wire_bytes"] < points[INSTALLED]["wire_bytes"]
                ),
                "exact": True,
            }
        )
    return comparisons


def checkpoint_summary(
    rows_by_corpus: dict[str, dict[str, dict]],
    checkpoint: int,
) -> dict:
    eligible = {
        corpus: rows
        for corpus, rows in rows_by_corpus.items()
        if set(REQUIRED_ROWS) <= set(rows)
        and len(rows[SEED]["per_tu_curve"]) >= checkpoint
    }
    if not eligible:
        raise ValueError(f"no corpus reaches checkpoint {checkpoint}")
    points_by_name = {
        name: [curve_point(rows[name], checkpoint) for rows in eligible.values()]
        for name in REQUIRED_ROWS
    }
    equal = {
        name: equal_corpus_ratio([point["ratio"] for point in points])
        for name, points in points_by_name.items()
    }
    weighted = {
        name: weighted_ratio(points) for name, points in points_by_name.items()
    }
    seed_strict_empty: list[str] = []
    seed_equal_empty: list[str] = []
    seed_larger_empty: list[str] = []
    seed_strict_installed: list[str] = []
    seed_equal_installed: list[str] = []
    seed_larger_installed: list[str] = []
    for corpus in sorted(eligible, key=corpus_sort_key):
        rows = eligible[corpus]
        seed_wire = curve_point(rows[SEED], checkpoint)["wire_bytes"]
        empty_wire = curve_point(rows[EMPTY], checkpoint)["wire_bytes"]
        installed_wire = curve_point(rows[INSTALLED], checkpoint)["wire_bytes"]
        if seed_wire < empty_wire:
            seed_strict_empty.append(corpus)
        elif seed_wire == empty_wire:
            seed_equal_empty.append(corpus)
        else:
            seed_larger_empty.append(corpus)
        if seed_wire < installed_wire:
            seed_strict_installed.append(corpus)
        elif seed_wire == installed_wire:
            seed_equal_installed.append(corpus)
        else:
            seed_larger_installed.append(corpus)
    return {
        "tu": checkpoint,
        "corpora": len(eligible),
        "empty_equal_corpus_ratio": equal[EMPTY],
        "installed_equal_corpus_ratio": equal[INSTALLED],
        "seed_equal_corpus_ratio": equal[SEED],
        "seed_vs_empty_equal_percent": (equal[SEED] / equal[EMPTY] - 1.0) * 100.0,
        "seed_vs_installed_equal_percent": (
            equal[SEED] / equal[INSTALLED] - 1.0
        )
        * 100.0,
        "empty_byte_weighted_ratio": weighted[EMPTY],
        "installed_byte_weighted_ratio": weighted[INSTALLED],
        "seed_byte_weighted_ratio": weighted[SEED],
        "seed_strict_wins_vs_empty": len(seed_strict_empty),
        "seed_strict_win_corpora_vs_empty": seed_strict_empty,
        "seed_ties_vs_empty": len(seed_equal_empty),
        "seed_tie_corpora_vs_empty": seed_equal_empty,
        "seed_losses_vs_empty": len(seed_larger_empty),
        "seed_loss_corpora_vs_empty": seed_larger_empty,
        "seed_strict_wins_vs_installed": len(seed_strict_installed),
        "seed_strict_win_corpora_vs_installed": seed_strict_installed,
        "seed_ties_vs_installed": len(seed_equal_installed),
        "seed_tie_corpora_vs_installed": seed_equal_installed,
        "seed_losses_vs_installed": len(seed_larger_installed),
        "seed_loss_corpora_vs_installed": seed_larger_installed,
    }


def endpoint_summary(comparisons: Sequence[dict]) -> dict:
    def points(prefix: str) -> list[dict]:
        return [
            {
                "raw_bytes": row["raw_bytes"],
                "wire_bytes": row[f"{prefix}_wire_bytes"],
                "ratio": row[f"{prefix}_ratio"],
            }
            for row in comparisons
        ]

    by_name = {name: points(name) for name in ("empty", "installed", "seed")}
    equal = {
        name: equal_corpus_ratio([point["ratio"] for point in values])
        for name, values in by_name.items()
    }
    weighted = {name: weighted_ratio(values) for name, values in by_name.items()}
    logical_state = sorted(row["final_logical_state_bytes"] for row in comparisons)
    return {
        "corpora": len(comparisons),
        "raw_bytes": sum(row["raw_bytes"] for row in comparisons),
        "empty_wire_bytes": sum(row["empty_wire_bytes"] for row in comparisons),
        "installed_wire_bytes": sum(
            row["installed_wire_bytes"] for row in comparisons
        ),
        "seed_wire_bytes": sum(row["seed_wire_bytes"] for row in comparisons),
        "empty_equal_corpus_ratio": equal["empty"],
        "installed_equal_corpus_ratio": equal["installed"],
        "seed_equal_corpus_ratio": equal["seed"],
        "seed_vs_empty_equal_percent": (
            equal["seed"] / equal["empty"] - 1.0
        )
        * 100.0,
        "seed_vs_installed_equal_percent": (
            equal["seed"] / equal["installed"] - 1.0
        )
        * 100.0,
        "empty_byte_weighted_ratio": weighted["empty"],
        "installed_byte_weighted_ratio": weighted["installed"],
        "seed_byte_weighted_ratio": weighted["seed"],
        "seed_vs_empty_weighted_percent": (
            weighted["seed"] / weighted["empty"] - 1.0
        )
        * 100.0,
        "seed_vs_installed_weighted_percent": (
            weighted["seed"] / weighted["installed"] - 1.0
        )
        * 100.0,
        "seed_strict_wins_vs_empty": sum(
            row["seed_strictly_smaller_than_empty"] for row in comparisons
        ),
        "seed_loss_or_tie_corpora_vs_empty": [
            row["corpus"]
            for row in comparisons
            if not row["seed_strictly_smaller_than_empty"]
        ],
        "seed_strict_wins_vs_installed": sum(
            row["seed_strictly_smaller_than_installed"] for row in comparisons
        ),
        "seed_loss_or_tie_corpora_vs_installed": [
            row["corpus"]
            for row in comparisons
            if not row["seed_strictly_smaller_than_installed"]
        ],
        "exact_corpora": sum(row["exact"] for row in comparisons),
        "minimum_final_logical_state_bytes": logical_state[0],
        "median_final_logical_state_bytes": statistics.median(logical_state),
        "maximum_final_logical_state_bytes": logical_state[-1],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", action="append", type=parse_input, required=True)
    parser.add_argument(
        "--checkpoints",
        nargs="+",
        type=int,
        default=DEFAULT_CHECKPOINTS,
    )
    parser.add_argument("--require-corpora", type=int)
    parser.add_argument("--output", required=True)
    parser.add_argument("--summary-json", required=True)
    args = parser.parse_args()

    rows_by_corpus, seed_packages = load_inputs(args.input)
    comparisons = compare_corpora(rows_by_corpus, seed_packages)
    if args.require_corpora is not None and len(comparisons) != args.require_corpora:
        raise ValueError(
            f"expected {args.require_corpora} corpora, found {len(comparisons)}"
        )

    with open(args.output, "w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=tuple(comparisons[0]),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(comparisons)

    package_groups: dict[str, dict] = {}
    for row in comparisons:
        digest = row["seed_artifact_sha256"]
        group = package_groups.setdefault(
            digest,
            {
                "sha256": digest,
                "assets": row["seed_artifact_assets"],
                "raw_bytes": row["seed_artifact_raw_bytes"],
                "compressed_bytes": row["seed_artifact_compressed_bytes"],
                "targets": [],
            },
        )
        if (
            group["assets"],
            group["raw_bytes"],
            group["compressed_bytes"],
        ) != (
            row["seed_artifact_assets"],
            row["seed_artifact_raw_bytes"],
            row["seed_artifact_compressed_bytes"],
        ):
            raise ValueError("one seed hash has inconsistent metadata")
        group["targets"].append(row["corpus"])

    summary = {
        "experiment": "C-only seed versus empty and installed-package starts",
        "endpoint_semantics": (
            "Each corpus ends at min(200, complete corpus TUs); checkpoints are "
            "survivor cohorts when fewer corpora remain."
        ),
        "endpoint": endpoint_summary(comparisons),
        "checkpoints": [
            checkpoint_summary(rows_by_corpus, checkpoint)
            for checkpoint in args.checkpoints
        ],
        "seed_packages": sorted(package_groups.values(), key=lambda row: row["sha256"]),
        "comparisons": comparisons,
    }
    Path(args.summary_json).write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
