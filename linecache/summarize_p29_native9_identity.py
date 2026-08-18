#!/usr/bin/env python3
"""Validate and summarize corrected-P29 prefix identity on native holdouts."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any

import summarize_p29_fixed16_identity as fixed
import summarize_p29_prefix_matrix as common


def markdown(rows: list[dict[str, Any]], summary: dict[str, Any]) -> str:
    lines = [
        "# Corrected P29 native-nine holdout identity",
        "",
        f"- Exact cells: **{summary['exact_cells']}/{summary['cells']}**.",
        f"- Prefix-identical cells: **{summary['prefix_identity_cells']}/{summary['cells']}**.",
        f"- Corrected complete P29: **{summary['complete_wire_bytes']:,} B**.",
        f"- TU112 probes: **{summary['probe_wire_bytes']:,} B**.",
        "",
        "| holdout | TUs | raw GB | probe raw MB | probe P29 MB | complete P29 MB | trajectory |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['name']} | {row['tus']} | {row['raw_bytes']/1e9:.3f} | "
            f"{row['probe_raw_bytes']/1e6:.3f} | {row['probe_wire_bytes']/1e6:.3f} | "
            f"{row['complete_wire_bytes']/1e6:.3f} | {row['probe_p29_trajectory']:.4f} |"
        )
    lines.extend(
        [
            "",
            "These projects were excluded from the selector-development inputs named by",
            "`causal-selector-policy-v1.json`. Process timings remain diagnostic.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-root", required=True, type=Path)
    parser.add_argument("--expected-cells", default=9, type=int)
    args = parser.parse_args()
    root = args.run_root.resolve()
    ledger = common.read_tsv(root / "fixed16-ledger.tsv")
    common.require(len(ledger) == args.expected_cells, "native-nine ledger count differs")
    expected = {row["id"]: row for row in ledger}
    common.require(len(expected) == len(ledger), "duplicate native-nine identity")
    paths = sorted((root / "cells").glob("*/prefix-identity.json"))
    common.require(len(paths) == args.expected_cells, "native-nine result count differs")
    rows = [fixed.parse_cell(path.parent, expected[path.parent.name]) for path in paths]
    order = {row["id"]: index for index, row in enumerate(ledger)}
    rows.sort(key=lambda row: order[row["id"]])

    source_hashes = {row["p29_source_sha256"] for row in rows}
    binary_hashes = {row["p29_binary_sha256"] for row in rows}
    common.require(len(source_hashes) == 1 and len(binary_hashes) == 1, "P29 tooling drifts")
    summary = {
        "schema": "p29-native9-prefix-identity-v1",
        "cells": len(rows),
        "raw_bytes": sum(row["raw_bytes"] for row in rows),
        "probe_wire_bytes": sum(row["probe_wire_bytes"] for row in rows),
        "complete_wire_bytes": sum(row["complete_wire_bytes"] for row in rows),
        "exact_cells": sum(row["exact"] for row in rows),
        "prefix_identity_cells": sum(row["prefix_identity"] for row in rows),
        "p29_source_sha256": next(iter(source_hashes)),
        "p29_binary_sha256": next(iter(binary_hashes)),
        "ledger_sha256": common.sha256_file(root / "fixed16-ledger.tsv"),
        "timing_scope": "diagnostic two-process research pass",
        "holdout_status": "not used to fit frozen causal-selector-policy-v1",
    }
    with (root / "p29-native9-measurements.tsv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, delimiter="\t", lineterminator="\n", fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (root / "p29-native9-summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    (root / "P29-NATIVE9-PREFIX-IDENTITY.md").write_text(markdown(rows, summary))
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
