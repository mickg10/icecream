#!/usr/bin/env python3
"""Integrate exact complementary half-warm Line runs with cold structure."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from run_prior_root_matrix import CORPORA


def harmonic(values: list[float]) -> float:
    return len(values) / sum(1 / value for value in values)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--structure-summary", type=Path, required=True)
    parser.add_argument("--half-line-report", type=Path, required=True)
    parser.add_argument("--level", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    structure_report = json.loads(args.structure_summary.read_text())
    line_report = json.loads(args.half_line_report.read_text())
    structure = {
        row["corpus"]: row for row in structure_report["per_corpus"]
    }
    line_rows = [row for row in line_report["rows"] if row["level"] == args.level]
    line = {(row["corpus"], row["cold_bit"]): row for row in line_rows}
    if set(structure) != set(CORPORA) or set(line) != {
        (corpus, bit) for corpus in CORPORA for bit in (0, 1)
    }:
        raise ValueError("half-cold inputs do not contain the balanced corpus matrix")

    per_corpus = []
    for corpus in CORPORA:
        base = structure[corpus]
        variants = []
        for bit in (0, 1):
            lines = line[corpus, bit]
            if not lines["exact"] or lines["raw_bytes"] != base["raw_bytes"]:
                raise ValueError(f"inexact or mismatched half-cold row: {corpus}/{bit}")
            projection_wire = (
                base["selected_structural_wire_bytes"] + lines["wire_bytes"]
            )
            variants.append(
                {
                    "cold_bit": bit,
                    "line_wire_bytes": lines["wire_bytes"],
                    "missing_lines": lines["missing_lines"],
                    "missing_line_bytes": lines["missing_line_bytes"],
                    "preinstalled_lines": lines["preinstalled_lines"],
                    "preinstalled_line_bytes": lines["preinstalled_line_bytes"],
                    "two_plane_projection_wire_bytes": projection_wire,
                    "two_plane_projection_ratio": (
                        base["raw_bytes"] / projection_wire
                    ),
                }
            )
        per_corpus.append(
            {
                "corpus": corpus,
                "raw_bytes": base["raw_bytes"],
                "structural_wire_bytes": base["selected_structural_wire_bytes"],
                "variants": variants,
                "worst_two_plane_ratio": min(
                    row["two_plane_projection_ratio"] for row in variants
                ),
                "best_two_plane_ratio": max(
                    row["two_plane_projection_ratio"] for row in variants
                ),
            }
        )

    variants = []
    for bit in (0, 1):
        chosen = [
            next(value for value in row["variants"] if value["cold_bit"] == bit)
            for row in per_corpus
        ]
        raw = sum(row["raw_bytes"] for row in per_corpus)
        projection_wire = sum(
            row["two_plane_projection_wire_bytes"] for row in chosen
        )
        variants.append(
            {
                "cold_bit": bit,
                "raw_bytes": raw,
                "two_plane_projection_wire_bytes": projection_wire,
                "two_plane_projection_weighted_ratio": raw / projection_wire,
                "two_plane_projection_equal_corpus_ratio": harmonic(
                    [row["two_plane_projection_ratio"] for row in chosen]
                ),
                "two_plane_projection_200_corpora": sum(
                    row["two_plane_projection_ratio"] >= 200 for row in chosen
                ),
                "exact_measured_plane_corpora": len(chosen),
            }
        )
    report = {
        "experiment": (
            "executed complementary half-warm Line cache plus incomplete cold "
            "structural projection"
        ),
        "scope": (
            "exact half-warm first-use Line-definition plane plus exact cold "
            "Region-digest structural plane"
        ),
        "total_transfer_complete": False,
        "missing_complete_transfer_blocks": [
            "Region-to-Line composition",
            "typed values and literal residuals outside the measured Line plane",
            "generation key association and missing-object exchange",
            "final combined framing and real C/F path",
        ],
        "level": args.level,
        "partition": line_report["partition"],
        "variants": variants,
        "worst_variant_two_plane_weighted_ratio": min(
            row["two_plane_projection_weighted_ratio"] for row in variants
        ),
        "worst_variant_two_plane_equal_corpus_ratio": min(
            row["two_plane_projection_equal_corpus_ratio"] for row in variants
        ),
        "minimum_per_corpus_two_plane_ratio_across_both_variants": min(
            row["worst_two_plane_ratio"] for row in per_corpus
        ),
        "per_corpus": per_corpus,
        "measured_planes_exact": True,
    }
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({key: value for key, value in report.items() if key != "per_corpus"}, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
