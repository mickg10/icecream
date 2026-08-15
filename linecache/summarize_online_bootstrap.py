#!/usr/bin/env python3
"""Summarize exact online-bootstrap reports across multiple corpora."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path

from online_bootstrap_curves import summarize_learning_gate


def parse_input(value: str) -> tuple[str, Path]:
    corpus, separator, path = value.partition("=")
    if not separator or not corpus or not path:
        raise argparse.ArgumentTypeError("input must be CORPUS=REPORT.json")
    return corpus, Path(path)


def gate(row: dict) -> dict:
    return row.get("learning_gate") or summarize_learning_gate(
        row["per_tu_curve"],
        row["initial_model_wire_bytes"],
        row["raw_bytes"],
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", action="append", type=parse_input, required=True)
    parser.add_argument("--invalid-pretrained", nargs="*", default=())
    parser.add_argument("--output", required=True)
    parser.add_argument("--summary-json")
    args = parser.parse_args()

    invalid = set(args.invalid_pretrained)
    rows_out: list[dict] = []
    selected: list[dict] = []
    for corpus, path in args.input:
        report = json.loads(path.read_text())
        rows = {row["name"]: row for row in report["rows"]}
        candidates: list[dict] = []
        for name in ("empty-online-k2", "pretrained-online-k2"):
            row = rows.get(name)
            if row is None:
                continue
            valid = not (name.startswith("pretrained-") and corpus in invalid)
            learning = gate(row)
            current = {
                "corpus": corpus,
                "report": str(path),
                "tus": row["tus"],
                "raw_bytes": row["raw_bytes"],
                "online_budget": row["online_budget"],
                "row": name,
                "pretraining_valid": valid,
                "selected": False,
                "charged_wire_bytes": row["charged_wire_bytes"],
                "charged_ratio": row["charged_ratio"],
                "model_wire_bytes": row["initial_model_wire_bytes"],
                "definition_wire_bytes": row["definition_wire_bytes"],
                "payload_wire_bytes": row["payload_wire_bytes"],
                "selector_wire_bytes": row["selector_wire_bytes"],
                "c50_ratio": learning["c50_ratio"],
                "h200_fraction": learning["h200_fraction"],
                "final_window_ratio": learning["final_window_ratio"],
                "exact": row["exact"],
            }
            rows_out.append(current)
            if valid:
                candidates.append(current)
        if not candidates:
            raise ValueError(f"{corpus}: no valid online row")
        winner = min(candidates, key=lambda row: row["charged_wire_bytes"])
        winner["selected"] = True
        selected.append(winner)

    columns = (
        "corpus", "report", "tus", "raw_bytes", "online_budget", "row",
        "pretraining_valid", "selected", "charged_wire_bytes", "charged_ratio",
        "model_wire_bytes", "definition_wire_bytes", "payload_wire_bytes",
        "selector_wire_bytes", "c50_ratio", "h200_fraction",
        "final_window_ratio", "exact",
    )
    with open(args.output, "w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=columns,
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows_out)

    ratios = sorted(row["charged_ratio"] for row in selected)
    aggregate_raw = sum(row["raw_bytes"] for row in selected)
    aggregate_wire = sum(row["charged_wire_bytes"] for row in selected)
    summary = {
        "corpora": len(selected),
        "aggregate_raw_bytes": aggregate_raw,
        "aggregate_wire_bytes": aggregate_wire,
        "aggregate_ratio": aggregate_raw / aggregate_wire,
        "minimum_ratio": ratios[0],
        "lower_quartile_ratio": statistics.median(ratios[: len(ratios) // 2]),
        "median_ratio": statistics.median(ratios),
        "maximum_ratio": ratios[-1],
        "cold_400_corpora": sum(row["charged_ratio"] >= 400 for row in selected),
        "h200_pass_corpora": sum(
            row["h200_fraction"] is not None and row["h200_fraction"] <= 0.50
            for row in selected
        ),
        "h200_unavailable_corpora": sum(
            row["h200_fraction"] is None for row in selected
        ),
        "selected": selected,
    }
    if args.summary_json:
        Path(args.summary_json).write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n"
        )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
