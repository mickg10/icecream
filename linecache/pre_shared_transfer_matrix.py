#!/usr/bin/env python3
"""Build the no-install-charge transfer matrix from exact per-TU curves."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Sequence

from compare_seed_bootstrap import (
    CANONICAL_CORPUS_ORDER,
    EMPTY,
    INSTALLED,
    REQUIRED_ROWS,
    SEED,
    corpus_sort_key,
    equal_corpus_ratio,
    load_inputs,
    validate_prefix,
)
from summarize_online_bootstrap import parse_input


DEFAULT_CHECKPOINTS = (50, 100, 150, 200, 250, 300)
METHOD_ROWS = {
    "empty_online": EMPTY,
    "pre_shared_installed": INSTALLED,
    "pre_shared_c_only": SEED,
}
ONE_GBIT_BYTES_PER_SECOND = 125_000_000
ONE_GBYTE_PER_SECOND = 1_000_000_000


def transfer_point(row: dict, ordinal: int) -> dict:
    """Return cumulative build-wire accounting at one exact TU boundary.

    The research harness charged ``initial_model_wire_bytes`` at TU zero.  An
    installed pre-shared package is not transferred on the build path, so this
    function removes that one-time installation charge.  Definitions selected
    during the build remain charged.
    """

    curve = row["per_tu_curve"]
    if not 0 < ordinal <= len(curve):
        raise ValueError(f"{row['name']}: TU {ordinal} is outside its curve")
    point = curve[ordinal - 1]
    if point["tu"] != ordinal:
        raise ValueError(f"{row['name']}: non-canonical TU ordinal")
    installation_bytes = int(row.get("initial_model_wire_bytes", 0))
    build_wire_bytes = int(point["cumulative_charged_bytes"]) - installation_bytes
    if build_wire_bytes <= 0:
        raise ValueError(
            f"{row['name']}: installation charge exceeds build-path bytes at TU {ordinal}"
        )
    raw_bytes = int(point["cumulative_raw_bytes"])
    return {
        "tu": ordinal,
        "raw_bytes": raw_bytes,
        "build_wire_bytes": build_wire_bytes,
        "average_wire_bytes_per_tu": build_wire_bytes / ordinal,
        "ratio": raw_bytes / build_wire_bytes,
        "installation_bytes_excluded": installation_bytes,
        "ideal_1gbit_seconds": build_wire_bytes / ONE_GBIT_BYTES_PER_SECOND,
        "ideal_1GBps_seconds": build_wire_bytes / ONE_GBYTE_PER_SECOND,
    }


def validate_rows(rows_by_corpus: dict[str, dict[str, dict]]) -> None:
    if not rows_by_corpus:
        raise ValueError("at least one corpus is required")
    for corpus, rows in rows_by_corpus.items():
        missing = set(REQUIRED_ROWS) - set(rows)
        if missing:
            raise ValueError(f"{corpus}: missing rows {sorted(missing)}")
        lengths = {name: len(rows[name]["per_tu_curve"]) for name in REQUIRED_ROWS}
        if len(set(lengths.values())) != 1:
            raise ValueError(f"{corpus}: curve lengths differ: {lengths}")
        total_tus = next(iter(lengths.values()))
        validate_prefix(corpus, rows, total_tus)
        for name in REQUIRED_ROWS:
            if rows[name]["tus"] != total_tus:
                raise ValueError(f"{corpus}: {name} TU count differs from its curve")
            if not rows[name]["exact"]:
                raise ValueError(f"{corpus}: {name} is inexact")


def checkpoint_labels(total_tus: int, checkpoints: Sequence[int]) -> list[tuple[str, int]]:
    labels = [(str(value), value) for value in checkpoints if value <= total_tus]
    labels.append(("full", total_tus))
    return labels


def make_long_rows(
    rows_by_corpus: dict[str, dict[str, dict]],
    checkpoints: Sequence[int],
) -> list[dict]:
    output: list[dict] = []
    for corpus in sorted(rows_by_corpus, key=corpus_sort_key):
        rows = rows_by_corpus[corpus]
        total_tus = rows[EMPTY]["tus"]
        for checkpoint, ordinal in checkpoint_labels(total_tus, checkpoints):
            points = {
                method: transfer_point(rows[row_name], ordinal)
                for method, row_name in METHOD_ROWS.items()
            }
            empty_wire = points["empty_online"]["build_wire_bytes"]
            for method, point in points.items():
                saved = empty_wire - point["build_wire_bytes"]
                output.append(
                    {
                        "corpus": corpus,
                        "checkpoint": checkpoint,
                        "tu": ordinal,
                        "corpus_tus": total_tus,
                        "method": method,
                        "raw_bytes": point["raw_bytes"],
                        "build_wire_bytes": point["build_wire_bytes"],
                        "average_wire_bytes_per_tu": point[
                            "average_wire_bytes_per_tu"
                        ],
                        "ratio": point["ratio"],
                        "saved_vs_empty_bytes": saved,
                        "saved_vs_empty_percent": saved / empty_wire * 100.0,
                        "installation_bytes_excluded": point[
                            "installation_bytes_excluded"
                        ],
                        "ideal_1gbit_seconds": point["ideal_1gbit_seconds"],
                        "ideal_1GBps_seconds": point["ideal_1GBps_seconds"],
                        "exact": True,
                    }
                )
    return output


def aggregate_checkpoint(
    rows_by_corpus: dict[str, dict[str, dict]],
    checkpoint: int | None,
) -> dict:
    points: dict[str, list[dict]] = {method: [] for method in METHOD_ROWS}
    corpora: list[str] = []
    for corpus in sorted(rows_by_corpus, key=corpus_sort_key):
        rows = rows_by_corpus[corpus]
        ordinal = rows[EMPTY]["tus"] if checkpoint is None else checkpoint
        if ordinal > rows[EMPTY]["tus"]:
            continue
        corpora.append(corpus)
        for method, row_name in METHOD_ROWS.items():
            points[method].append(transfer_point(rows[row_name], ordinal))

    if not corpora:
        raise ValueError(f"no corpus reaches checkpoint {checkpoint}")
    raw_bytes = sum(point["raw_bytes"] for point in points["empty_online"])
    methods: dict[str, dict] = {}
    for method, values in points.items():
        wire_bytes = sum(point["build_wire_bytes"] for point in values)
        methods[method] = {
            "build_wire_bytes": wire_bytes,
            "byte_weighted_ratio": raw_bytes / wire_bytes,
            "equal_corpus_ratio": equal_corpus_ratio(
                [point["ratio"] for point in values]
            ),
            "ideal_1gbit_seconds": wire_bytes / ONE_GBIT_BYTES_PER_SECOND,
            "ideal_1GBps_seconds": wire_bytes / ONE_GBYTE_PER_SECOND,
        }

    empty_wire = methods["empty_online"]["build_wire_bytes"]
    for method, values in methods.items():
        values["saved_vs_empty_bytes"] = empty_wire - values["build_wire_bytes"]
        values["saved_vs_empty_percent"] = (
            empty_wire - values["build_wire_bytes"]
        ) / empty_wire * 100.0

    fixed_pre_shared = min(
        ("pre_shared_installed", "pre_shared_c_only"),
        key=lambda method: methods[method]["build_wire_bytes"],
    )
    return {
        "checkpoint": "full" if checkpoint is None else str(checkpoint),
        "eligible_corpora": len(corpora),
        "corpora": corpora,
        "raw_bytes": raw_bytes,
        "lower_fixed_pre_shared_method": fixed_pre_shared,
        "methods": methods,
    }


def make_aggregates(
    rows_by_corpus: dict[str, dict[str, dict]],
    checkpoints: Sequence[int],
) -> list[dict]:
    return [
        *(aggregate_checkpoint(rows_by_corpus, value) for value in checkpoints),
        aggregate_checkpoint(rows_by_corpus, None),
    ]


def write_tsv(path: Path, rows: Sequence[dict]) -> None:
    if not rows:
        raise ValueError(f"cannot write empty table: {path}")
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=tuple(rows[0]),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)


def flatten_aggregate(row: dict) -> dict:
    output = {
        "checkpoint": row["checkpoint"],
        "eligible_corpora": row["eligible_corpora"],
        "raw_bytes": row["raw_bytes"],
        "lower_fixed_pre_shared_method": row["lower_fixed_pre_shared_method"],
    }
    for method, values in row["methods"].items():
        for name, value in values.items():
            output[f"{method}_{name}"] = value
    return output


def format_bytes(value: int) -> str:
    return f"{value:,}"


def format_mib(value: int) -> str:
    return f"{value / (1024 * 1024):.3f}"


def render_markdown(summary: dict) -> str:
    aggregates = {row["checkpoint"]: row for row in summary["aggregates"]}
    full = aggregates["full"]
    installed_full = full["methods"]["pre_shared_installed"]
    seed_full = full["methods"]["pre_shared_c_only"]

    lines = [
        "# Pre-shared bootstrap transfer matrix: 16 exact structural corpora",
        "",
        "## Outcome",
        "",
        (
            "When the pre-shared package is already installed at C and F, its package bytes "
            "are not build traffic. Under that accounting, the installed-package row is the "
            "smallest fixed method across the complete 16-corpus suite: "
            f"**{format_bytes(installed_full['build_wire_bytes'])} B**, "
            f"saving **{format_bytes(installed_full['saved_vs_empty_bytes'])} B "
            f"({installed_full['saved_vs_empty_percent']:.2f}%)** versus empty online learning."
        ),
        "",
        (
            "The C-only seed remains nearly tied at "
            f"**{format_bytes(seed_full['build_wire_bytes'])} B** "
            f"({seed_full['saved_vs_empty_percent']:.2f}% below empty). The installed row is "
            f"smaller by {format_bytes(seed_full['build_wire_bytes'] - installed_full['build_wire_bytes'])} B "
            "over the complete suite."
        ),
        "",
        (
            "This result covers the exact **Region-program structural layer only**. It does "
            "not include first-use Line text, all value/literal streams, missing-object "
            "traffic, or final protocol framing, so its ratios do not establish total cold "
            "400x. The current packages are target-disjoint cross-fit capability artifacts "
            "learned from expanded traces; they are not yet portable raw-source bootstrap models."
        ),
        "",
        "## Accounting",
        "",
        "For every corpus, method, and TU boundary:",
        "",
        "```text",
        "build_wire_bytes = cumulative_charged_bytes - initial_model_wire_bytes",
        "compression_ratio = cumulative_raw_bytes / build_wire_bytes",
        "ideal_1_Gbit/s_seconds = build_wire_bytes / 125,000,000",
        "ideal_1_GB/s_seconds = build_wire_bytes / 1,000,000,000",
        "```",
        "",
        (
            "Definitions actually selected during the build remain charged. Only the immutable "
            "pre-installed package is excluded. Archive-storage size and raw `.ii` size are not "
            "used as substitutes for protocol bytes."
        ),
        "",
        "## Aggregate checkpoint matrix",
        "",
        (
            "Checkpoint cohorts contain only corpora that reach that absolute TU. `full` uses "
            "every corpus at its own complete endpoint. Ratios in this table are byte-weighted "
            "because the question is total transfer; the equal-corpus harmonic result is also "
            "retained in the machine summary."
        ),
        "",
        "| TU | corpora | raw GiB | empty MiB | installed MiB | C-only MiB | installed saved | C-only saved | empty ratio | installed ratio | C-only ratio | installed @1 Gbit/s | installed @1 GB/s |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for checkpoint in (*map(str, summary["checkpoints"]), "full"):
        row = aggregates[checkpoint]
        empty = row["methods"]["empty_online"]
        installed = row["methods"]["pre_shared_installed"]
        seed = row["methods"]["pre_shared_c_only"]
        lines.append(
            f"| {checkpoint} | {row['eligible_corpora']} | "
            f"{row['raw_bytes'] / (1024 ** 3):.3f} | "
            f"{format_mib(empty['build_wire_bytes'])} | "
            f"{format_mib(installed['build_wire_bytes'])} | "
            f"{format_mib(seed['build_wire_bytes'])} | "
            f"{installed['saved_vs_empty_percent']:.2f}% | "
            f"{seed['saved_vs_empty_percent']:.2f}% | "
            f"{empty['byte_weighted_ratio']:.2f}x | "
            f"{installed['byte_weighted_ratio']:.2f}x | "
            f"{seed['byte_weighted_ratio']:.2f}x | "
            f"{installed['ideal_1gbit_seconds']:.6f}s | "
            f"{installed['ideal_1GBps_seconds']:.6f}s |"
        )

    by_key = {
        (row["corpus"], row["checkpoint"], row["method"]): row
        for row in summary["rows"]
    }
    lines.extend(
        [
            "",
            "## Per-corpus requested-boundary matrix",
            "",
            (
                "Each cell is cumulative **empty / installed / C-only build wire in MiB**. "
                "A dash means the corpus completed before that TU. `full` is always present."
            ),
            "",
            "| corpus | TU 50 | TU 100 | TU 150 | TU 200 | TU 250 | TU 300 | full |",
            "|---|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    corpus_names = sorted(
        {row["corpus"] for row in summary["rows"]}, key=corpus_sort_key
    )
    for corpus in corpus_names:
        cells = []
        for checkpoint in (*map(str, summary["checkpoints"]), "full"):
            key = (corpus, checkpoint, "empty_online")
            if key not in by_key:
                cells.append("—")
                continue
            values = [
                by_key[(corpus, checkpoint, method)]["build_wire_bytes"]
                for method in METHOD_ROWS
            ]
            cells.append(" / ".join(format_mib(value) for value in values))
        lines.append(f"| {corpus} | " + " | ".join(cells) + " |")

    lines.extend(
        [
            "",
            "## Complete per-corpus endpoints",
            "",
            "| corpus | TUs | raw GiB | empty B | installed B | C-only B | installed saved | C-only saved | empty | installed | C-only |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    installed_wins = seed_wins = 0
    for corpus in corpus_names:
        rows = {
            method: by_key[(corpus, "full", method)] for method in METHOD_ROWS
        }
        empty = rows["empty_online"]
        installed = rows["pre_shared_installed"]
        seed = rows["pre_shared_c_only"]
        installed_wins += installed["build_wire_bytes"] < seed["build_wire_bytes"]
        seed_wins += seed["build_wire_bytes"] < installed["build_wire_bytes"]
        lines.append(
            f"| {corpus} | {empty['tu']} | {empty['raw_bytes'] / (1024 ** 3):.3f} | "
            f"{format_bytes(empty['build_wire_bytes'])} | "
            f"{format_bytes(installed['build_wire_bytes'])} | "
            f"{format_bytes(seed['build_wire_bytes'])} | "
            f"{installed['saved_vs_empty_percent']:.2f}% | "
            f"{seed['saved_vs_empty_percent']:.2f}% | "
            f"{empty['ratio']:.2f}x | {installed['ratio']:.2f}x | {seed['ratio']:.2f}x |"
        )

    package_groups: dict[str, dict] = {}
    for package in summary["packages"]:
        group = package_groups.setdefault(
            package["sha256"],
            {
                "sha256": package["sha256"],
                "raw_bytes": package["raw_bytes"],
                "compressed_bytes": package["compressed_bytes"],
                "targets": [],
            },
        )
        group["targets"].append(package["corpus"])
    lines.extend(
        [
            "",
            "## Pre-shared artifacts",
            "",
            "These bytes are installation/package footprint and are reported separately from build wire.",
            "",
            "| SHA-256 | targets | definitions raw B | package B |",
            "|---|---:|---:|---:|",
        ]
    )
    for group in sorted(package_groups.values(), key=lambda value: value["sha256"]):
        lines.append(
            f"| `{group['sha256']}` | {len(group['targets'])} | "
            f"{format_bytes(group['raw_bytes'])} | {format_bytes(group['compressed_bytes'])} |"
        )

    lines.extend(
        [
            "",
            "## Interpretation and next experiment",
            "",
            f"The installed package is smaller at {installed_wins}/16 complete endpoints; "
            f"the C-only seed is smaller at {seed_wins}/16. Once installation is correctly "
            "excluded, the old conclusion that C-only seeding wins 15/16 is no longer true: "
            "that conclusion compared against a row that charged the package on every build.",
            "",
            (
                "The difference between the two pre-shared placements is small at aggregate "
                "completion, while both remain materially better than empty start. The next "
                "decision should therefore be driven by the complete codec and its reorder/change "
                "curves, not this structural-only margin."
            ),
            "",
            "Required next measurements:",
            "",
            "1. Train the same candidate representation from portable raw-source corpora and test it on project/profile holdouts.",
            "2. Carry the selected definitions into the complete codec and account for Line text, values, residuals, missing replies, and framing.",
            "3. Run standard, reverse, deterministic shuffles, dependency-root replacement, delayed change, revert, and half-cold state.",
            "4. Require exact reconstruction and at least 1 GB/s on the complete C++ path.",
            "",
            "## Machine-readable evidence",
            "",
            "- `linecache/ml-artifacts/pre-shared-transfer-matrix-16corpus.tsv`",
            "- `linecache/ml-artifacts/pre-shared-transfer-matrix-16corpus-aggregate.tsv`",
            "- `linecache/ml-artifacts/pre-shared-transfer-matrix-16corpus-summary.json`",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", action="append", type=parse_input, required=True)
    parser.add_argument(
        "--checkpoints", nargs="+", type=int, default=DEFAULT_CHECKPOINTS
    )
    parser.add_argument("--require-corpora", type=int)
    parser.add_argument("--long-tsv", type=Path, required=True)
    parser.add_argument("--aggregate-tsv", type=Path, required=True)
    parser.add_argument("--summary-json", type=Path, required=True)
    parser.add_argument("--report-md", type=Path, required=True)
    args = parser.parse_args()

    if any(value <= 0 for value in args.checkpoints):
        raise ValueError("checkpoints must be positive")
    if len(set(args.checkpoints)) != len(args.checkpoints):
        raise ValueError("checkpoints must be unique")

    rows_by_corpus, packages = load_inputs(args.input)
    validate_rows(rows_by_corpus)
    if args.require_corpora is not None and len(rows_by_corpus) != args.require_corpora:
        raise ValueError(
            f"expected {args.require_corpora} corpora, found {len(rows_by_corpus)}"
        )

    long_rows = make_long_rows(rows_by_corpus, args.checkpoints)
    aggregates = make_aggregates(rows_by_corpus, args.checkpoints)
    for path in (args.long_tsv, args.aggregate_tsv, args.summary_json, args.report_md):
        path.parent.mkdir(parents=True, exist_ok=True)
    write_tsv(args.long_tsv, long_rows)
    write_tsv(args.aggregate_tsv, [flatten_aggregate(row) for row in aggregates])

    package_rows = []
    for corpus in sorted(packages, key=corpus_sort_key):
        package = packages[corpus]
        seed = rows_by_corpus[corpus][SEED]
        package_rows.append(
            {
                "corpus": corpus,
                "sha256": package["sha256"],
                "raw_bytes": seed["seed_model_raw_bytes"],
                "compressed_bytes": seed["seed_model_compressed_bytes"],
            }
        )
    summary = {
        "experiment": "exact structural transfer, installation bytes excluded",
        "accounting": {
            "build_wire_bytes": (
                "cumulative_charged_bytes - initial_model_wire_bytes"
            ),
            "one_gbit_bytes_per_second": ONE_GBIT_BYTES_PER_SECOND,
            "one_GBps_bytes_per_second": ONE_GBYTE_PER_SECOND,
            "scope": "Region-program structural layer only",
        },
        "corpora": len(rows_by_corpus),
        "canonical_corpus_order": list(CANONICAL_CORPUS_ORDER),
        "checkpoints": list(args.checkpoints),
        "packages": package_rows,
        "aggregates": aggregates,
        "rows": long_rows,
    }
    args.summary_json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    args.report_md.write_text(render_markdown(summary))
    print(json.dumps({"corpora": len(rows_by_corpus), "aggregates": aggregates}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
