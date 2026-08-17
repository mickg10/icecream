#!/usr/bin/env python3
"""Validate and summarize exact per-TU codec50 component ledgers."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
from typing import Sequence


WIRE_COLUMNS = (
    "root_wire_bytes",
    "block_wire_bytes",
    "path_wire_bytes",
    "framing_wire_bytes",
    "region_control_wire_bytes",
    "region_other_wire_bytes",
    "literal_wire_bytes",
    "array_control_wire_bytes",
    "array_values_wire_bytes",
    "source_control_wire_bytes",
    "source_files_wire_bytes",
    "selector_wire_bytes",
    "blob_wire_bytes",
    "blob_patch_wire_bytes",
    "line_other_wire_bytes",
    "association_wire_bytes",
    "missing_request_wire_bytes",
    "blob_fallback_request_wire_bytes",
    "blob_fallback_reply_wire_bytes",
    "missing_other_wire_bytes",
)

RAW_COLUMNS = (
    "region_control_raw_bytes",
    "literal_raw_bytes",
    "array_control_raw_bytes",
    "array_values_raw_bytes",
    "source_control_raw_bytes",
    "source_files_raw_bytes",
    "blob_raw_bytes",
    "blob_patch_raw_bytes",
)

FAMILIES = {
    "literal": ("literal_wire_bytes",),
    "blob": ("blob_wire_bytes", "blob_patch_wire_bytes"),
    "array": ("array_control_wire_bytes", "array_values_wire_bytes"),
    "region control": ("region_control_wire_bytes", "region_other_wire_bytes"),
    "structure": ("root_wire_bytes", "block_wire_bytes", "path_wire_bytes"),
    "request/association": (
        "association_wire_bytes",
        "missing_request_wire_bytes",
        "blob_fallback_request_wire_bytes",
        "blob_fallback_reply_wire_bytes",
        "missing_other_wire_bytes",
    ),
    "source": ("source_control_wire_bytes", "source_files_wire_bytes"),
    "framing": ("framing_wire_bytes",),
    "other": ("selector_wire_bytes", "line_other_wire_bytes"),
}

CHECKPOINTS = (1, 10, 25, 50, 100, 150, 200, 250, 300, 400, 500)
BAND_ENDS = (50, 100, 150, 200, 250, 300, 400, 500)


def parse_curve_argument(value: str) -> tuple[str, Path]:
    try:
        name, raw_path = value.split("=", 1)
    except ValueError as error:
        raise argparse.ArgumentTypeError("curve must be NAME=PATH") from error
    if not name or not raw_path:
        raise argparse.ArgumentTypeError("curve must be NAME=PATH")
    return name, Path(raw_path)


def read_curve(name: str, path: Path) -> list[dict[str, int]]:
    rows: list[dict[str, int]] = []
    with path.open(newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        expected = {
            "tu",
            "raw_bytes",
            "wire_bytes",
            "cumulative_raw_bytes",
            "cumulative_wire_bytes",
            "exact",
            *RAW_COLUMNS,
            *WIRE_COLUMNS,
        }
        missing = expected.difference(reader.fieldnames or ())
        if missing:
            raise ValueError(f"{path}: missing columns: {sorted(missing)}")
        cumulative_raw = cumulative_wire = 0
        for raw in reader:
            if "corpus" in raw and raw["corpus"] != name:
                continue
            ordinal = len(rows) + 1
            if raw["exact"] != "true":
                raise ValueError(f"{path}: TU {ordinal} is not exact")
            row = {
                key: int(value)
                for key, value in raw.items()
                if key not in ("corpus", "exact") and value is not None
            }
            cumulative_raw += row["raw_bytes"]
            cumulative_wire += row["wire_bytes"]
            component_wire = sum(row[column] for column in WIRE_COLUMNS)
            if row["tu"] != ordinal:
                raise ValueError(f"{path}: expected TU {ordinal}, got {row['tu']}")
            if component_wire != row["wire_bytes"]:
                raise ValueError(
                    f"{path}: TU {ordinal} components={component_wire}, "
                    f"wire={row['wire_bytes']}"
                )
            if (
                row["cumulative_raw_bytes"] != cumulative_raw
                or row["cumulative_wire_bytes"] != cumulative_wire
            ):
                raise ValueError(f"{path}: invalid cumulative values at TU {ordinal}")
            rows.append(row)
    if not rows:
        raise ValueError(f"{path}: no rows for {name}")
    return rows


def sum_columns(rows: Sequence[dict[str, int]], columns: Sequence[str]) -> int:
    return sum(row[column] for row in rows for column in columns)


def family_values(rows: Sequence[dict[str, int]]) -> dict[str, int]:
    return {
        family: sum_columns(rows, columns)
        for family, columns in FAMILIES.items()
    }


def nearest_rank(values: Sequence[int], fraction: float) -> int:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def summarize_slice(rows: Sequence[dict[str, int]]) -> dict:
    family_wire = family_values(rows)
    return {
        "tus": len(rows),
        "raw_bytes": sum(row["raw_bytes"] for row in rows),
        "wire_bytes": sum(row["wire_bytes"] for row in rows),
        "family_wire_bytes": family_wire,
    }


def summarize_corpus(name: str, path: Path, rows: Sequence[dict[str, int]]) -> dict:
    total = summarize_slice(rows)
    wire_values = [row["wire_bytes"] for row in rows]
    checkpoints = []
    for count in (*CHECKPOINTS, len(rows)):
        if count <= len(rows) and not any(row["tu"] == count for row in checkpoints):
            point = summarize_slice(rows[:count])
            point["tu"] = count
            checkpoints.append(point)

    band_ends = [value for value in BAND_ENDS if value < len(rows)] + [len(rows)]
    bands = []
    start = 0
    for end in band_ends:
        band = summarize_slice(rows[start:end])
        band["first_tu"] = start + 1
        band["last_tu"] = end
        bands.append(band)
        start = end

    component_wire = {
        column.removesuffix("_wire_bytes"): sum(row[column] for row in rows)
        for column in WIRE_COLUMNS
    }
    raw_material = {
        column.removesuffix("_raw_bytes"): sum(row[column] for row in rows)
        for column in RAW_COLUMNS
    }
    top_tus = sorted(rows, key=lambda row: row["wire_bytes"], reverse=True)[:10]
    return {
        "name": name,
        "source": str(path),
        **total,
        "component_wire_bytes": component_wire,
        "raw_material_bytes": raw_material,
        "checkpoints": checkpoints,
        "bands": bands,
        "per_tu_wire_distribution": {
            "p50_bytes": nearest_rank(wire_values, 0.50),
            "p90_bytes": nearest_rank(wire_values, 0.90),
            "p99_bytes": nearest_rank(wire_values, 0.99),
            "max_bytes": max(wire_values),
        },
        "top_tus": [
            {
                "tu": row["tu"],
                "raw_bytes": row["raw_bytes"],
                "wire_bytes": row["wire_bytes"],
                "family_wire_bytes": family_values((row,)),
            }
            for row in top_tus
        ],
    }


def load_precomputed(path: Path, corpora: Sequence[dict]) -> dict:
    evidence = json.loads(path.read_text())
    if evidence.get("schema") != "precomputed-material-lane-ceiling-v1":
        raise ValueError(f"{path}: incompatible precomputed evidence schema")
    curves = {corpus["name"]: corpus for corpus in corpora}
    for corpus in evidence["corpora"]:
        name = corpus["name"]
        if name not in curves:
            raise ValueError(f"{path}: no component curve for {name}")
        curve = curves[name]
        if (
            corpus["current_complete_wire_bytes"] != curve["wire_bytes"]
            or corpus["tus"] != curve["tus"]
        ):
            raise ValueError(f"{path}: current total differs for {name}")
        current_lane_wire = 0
        candidate_names: set[str] | None = None
        for lane in corpus["lanes"]:
            raw_component = lane["raw_component"]
            wire_components = lane["wire_components"]
            expected_raw = curve["raw_material_bytes"][raw_component]
            expected_wire = sum(
                curve["component_wire_bytes"][component]
                for component in wire_components
            )
            if lane["raw_bytes"] != expected_raw or lane["current_wire_bytes"] != expected_wire:
                raise ValueError(f"{path}: {name}/{lane['name']} differs from curve")
            if not lane["decoded_sha256_verified"]:
                raise ValueError(f"{path}: {name}/{lane['name']} is not decode-verified")
            current_lane_wire += lane["current_wire_bytes"]
            names = set(lane["candidate_bytes"])
            candidate_names = names if candidate_names is None else candidate_names & names
        fixed_wire = corpus["current_complete_wire_bytes"] - current_lane_wire
        if corpus["fixed_wire_bytes"] != fixed_wire:
            raise ValueError(f"{path}: fixed-wire total differs for {name}")
        projections = {
            "current_causal": corpus["current_complete_wire_bytes"],
        }
        for candidate in sorted(candidate_names or ()):
            projections[candidate] = fixed_wire + sum(
                lane["candidate_bytes"][candidate] for lane in corpus["lanes"]
            )
        if {"zstd19_long", "zpaq_m3"}.issubset(candidate_names or ()):
            projections["best_zstd19_or_m3_per_lane"] = fixed_wire + sum(
                min(
                    lane["candidate_bytes"]["zstd19_long"],
                    lane["candidate_bytes"]["zpaq_m3"],
                )
                for lane in corpus["lanes"]
            )
        corpus["projections"] = projections
        corpus["projection_over_whole_ii_z19"] = {
            candidate: value / corpus["whole_ii_z19_bytes"]
            for candidate, value in projections.items()
        }
        corpus["projection_delta_to_gate"] = {
            candidate: value - corpus["cold_gate_bytes"]
            for candidate, value in projections.items()
        }
    return evidence


def fmt_bytes(value: int) -> str:
    return f"{value:,}"


def family_cells(point: dict) -> str:
    values = point["family_wire_bytes"]
    return " | ".join(fmt_bytes(values[family]) for family in FAMILIES)


def write_markdown(report: dict, path: Path, detail_name: str) -> None:
    lines = [
        "# P29 exact per-TU transfer-component breakdown",
        "",
        "`codec50 --component-curve-tsv` records every charged byte in 20 disjoint wire "
        "components after each TU. Each row is decoder-verified, and each row's components "
        "must sum exactly to its canonical complete-transfer byte count. The complete ledger is "
        f"in [`{detail_name}`](ml-artifacts/{detail_name}).",
        "",
        "The tables below are the causal cold run: TU *n* can use only state established before "
        "or during TU *n*. They are not a precomputed-build projection.",
        "",
        "## Full-run composition",
        "",
        "| program | TUs | raw `.ii` | complete wire | literal | blob | array | region control | structure | request/association | remaining |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for corpus in report["corpora"]:
        families = corpus["family_wire_bytes"]
        remaining = sum(
            families[name] for name in ("source", "framing", "other")
        )
        lines.append(
            f"| {corpus['name']} | {corpus['tus']:,} | {corpus['raw_bytes']:,} | "
            f"{corpus['wire_bytes']:,} | {families['literal']:,} | "
            f"{families['blob']:,} | {families['array']:,} | "
            f"{families['region control']:,} | {families['structure']:,} | "
            f"{families['request/association']:,} | {remaining:,} |"
        )

    family_headers = " | ".join(FAMILIES)
    family_align = "|".join("---:" for _ in FAMILIES)
    for corpus in report["corpora"]:
        lines.extend(
            [
                "",
                f"## {corpus['name']}: cumulative bytes through each turn",
                "",
                f"| through TU | complete wire | {family_headers} |",
                f"|---:|---:|{family_align}|",
            ]
        )
        for point in corpus["checkpoints"]:
            lines.append(
                f"| {point['tu']:,} | {point['wire_bytes']:,} | "
                f"{family_cells(point)} |"
            )
        lines.extend(
            [
                "",
                f"### {corpus['name']}: bytes added by turn interval",
                "",
                f"| TU interval | complete wire | {family_headers} |",
                f"|---:|---:|{family_align}|",
            ]
        )
        for band in corpus["bands"]:
            lines.append(
                f"| {band['first_tu']:,}–{band['last_tu']:,} | "
                f"{band['wire_bytes']:,} | {family_cells(band)} |"
            )
        distribution = corpus["per_tu_wire_distribution"]
        lines.extend(
            [
                "",
                f"Per-TU complete-wire distribution: p50 {distribution['p50_bytes']:,} B; "
                f"p90 {distribution['p90_bytes']:,} B; p99 {distribution['p99_bytes']:,} B; "
                f"maximum {distribution['max_bytes']:,} B.",
                "",
                "Largest turns:",
                "",
                "| TU | raw `.ii` | complete wire | dominant family | dominant bytes |",
                "|---:|---:|---:|---|---:|",
            ]
        )
        for turn in corpus["top_tus"]:
            dominant, dominant_bytes = max(
                turn["family_wire_bytes"].items(), key=lambda item: item[1]
            )
            lines.append(
                f"| {turn['tu']:,} | {turn['raw_bytes']:,} | {turn['wire_bytes']:,} | "
                f"{dominant} | {dominant_bytes:,} |"
            )

    if "precomputed" in report:
        lines.extend(
            [
                "",
                "## Fully precomputed material-lane ceiling",
                "",
                "This experiment concatenates each raw material lane in manifest order and "
                "compresses it as one build-wide archive. It keeps every non-material byte from "
                "the exact causal P29 ledger unchanged. Every archive was independently decoded "
                "and SHA-256 checked. The complete numbers are projections until this layout is "
                "wired into the independent decoder.",
                "",
                "The projection is conservative about the unchanged structure and request bytes, "
                "but optimistic about knowing the complete build before transfer. It does not "
                "make any retained receiver state or model bytes free.",
            ]
        )
        candidate_labels = {
            "current_causal": "current causal P29",
            "zstd19_long": "one z19-long frame/lane",
            "zpaq_m3": "one ZPAQ m3 archive/lane",
            "best_zstd19_or_m3_per_lane": "best z19/m3 per lane",
            "zpaq_m4": "one ZPAQ m4 archive/lane",
            "zpaq_m5": "one ZPAQ m5 archive/lane",
        }
        for corpus in report["precomputed"]["corpora"]:
            lines.extend(
                [
                    "",
                    f"### {corpus['name']} precomputed lanes",
                    "",
                    "| lane | raw bytes | current causal wire | z19-long | ZPAQ m3 | ZPAQ m4 | ZPAQ m5 |",
                    "|---|---:|---:|---:|---:|---:|---:|",
                ]
            )
            for lane in corpus["lanes"]:
                candidates = lane["candidate_bytes"]
                lines.append(
                    f"| {lane['name']} | {lane['raw_bytes']:,} | "
                    f"{lane['current_wire_bytes']:,} | "
                    f"{candidates['zstd19_long']:,} | {candidates['zpaq_m3']:,} | "
                    f"{candidates['zpaq_m4']:,} | {candidates['zpaq_m5']:,} |"
                )
            lines.extend(
                [
                    "",
                    f"Unchanged charged wire outside these lanes: {corpus['fixed_wire_bytes']:,} bytes.",
                    "",
                    "| complete projection | bytes | / whole-program z19 | delta to 110% gate |",
                    "|---|---:|---:|---:|",
                ]
            )
            order = (
                "current_causal",
                "zstd19_long",
                "zpaq_m3",
                "best_zstd19_or_m3_per_lane",
                "zpaq_m4",
                "zpaq_m5",
            )
            for candidate in order:
                value = corpus["projections"][candidate]
                delta = corpus["projection_delta_to_gate"][candidate]
                lines.append(
                    f"| {candidate_labels[candidate]} | {value:,} | "
                    f"{corpus['projection_over_whole_ii_z19'][candidate]:.3f}x | "
                    f"{delta:+,} |"
                )
    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            "The curve is lumpy rather than a smooth learning curve. A few turns introduce large "
            "literal, numeric-array, or compressed-blob populations; later turns then reuse them. "
            "This is why an average bytes/TU number hides the actual opportunity. A precomputed "
            "build sequence can batch each material lane into a build-wide stream and retain "
            "cross-TU matches. It must still send enough framing/control information for the "
            "receiver to reconstruct each TU, and all such bytes must be charged.",
            "",
            "A build-wide archive has no honest causal marginal-byte assignment: an early match can "
            "change bytes emitted much later. Therefore this report keeps the real causal per-turn "
            "ledger intact. A precomputed-sequence result should be reported separately as either "
            "(a) an up-front charged archive, or (b) independently decodable charged chunks with "
            "their exact TU ranges.",
            "",
            "## Reproduction",
            "",
            "```text",
            "codec50 <P29 flags> --component-curve-tsv CURVE.tsv",
            "python3 linecache/summarize_component_curves.py \\",
            "  --curve DuckDB=linecache/ml-artifacts/p29-component-curve.tsv \\",
            "  --curve Godot=linecache/ml-artifacts/p29-component-curve.tsv \\",
            "  --detail-tsv linecache/ml-artifacts/p29-component-curve.tsv \\",
            "  --precomputed linecache/ml-artifacts/precomputed-material-lane-ceiling.json \\",
            "  --json linecache/ml-artifacts/p29-component-curve-summary.json \\",
            "  --markdown linecache/P29-TU-COMPONENT-BREAKDOWN.md",
            "```",
            "",
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines))


def write_detail_tsv(curves: Sequence[tuple[str, Sequence[dict[str, int]]]], path: Path) -> None:
    columns = (
        "corpus",
        "tu",
        "raw_bytes",
        "wire_bytes",
        *RAW_COLUMNS,
        *WIRE_COLUMNS,
        "cumulative_raw_bytes",
        "cumulative_wire_bytes",
        "exact",
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=columns, delimiter="\t", lineterminator="\n"
        )
        writer.writeheader()
        for name, rows in curves:
            for row in rows:
                writer.writerow({"corpus": name, **row, "exact": "true"})


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--curve",
        action="append",
        required=True,
        type=parse_curve_argument,
        metavar="NAME=PATH",
    )
    parser.add_argument("--detail-tsv", required=True, type=Path)
    parser.add_argument("--precomputed", type=Path)
    parser.add_argument("--json", required=True, type=Path)
    parser.add_argument("--markdown", required=True, type=Path)
    args = parser.parse_args()

    curves = [(name, read_curve(name, path)) for name, path in args.curve]
    report = {
        "schema": "p29-component-curve-v1",
        "wire_components": list(WIRE_COLUMNS),
        "families": {name: list(columns) for name, columns in FAMILIES.items()},
        "corpora": [
            summarize_corpus(name, path, rows)
            for (name, path), (_, rows) in zip(args.curve, curves, strict=True)
        ],
    }
    if args.precomputed:
        report["precomputed"] = load_precomputed(args.precomputed, report["corpora"])
    write_detail_tsv(curves, args.detail_tsv)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2) + "\n")
    write_markdown(report, args.markdown, args.detail_tsv.name)


if __name__ == "__main__":
    main()
