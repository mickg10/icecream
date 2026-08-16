#!/usr/bin/env python3
"""Aggregate paired prior-root learning curves at fixed TU checkpoints."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

from run_prior_root_matrix import CORPORA


def load(path: Path) -> dict[int, dict[str, str]]:
    with path.open(newline="") as source:
        return {
            int(row["tu"]): row
            for row in csv.DictReader(source, delimiter="\t")
        }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--scenario", default="standard")
    parser.add_argument(
        "--checkpoints", nargs="+", type=int, default=(1, 2, 5, 10, 25, 50, 100, 200)
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    paired = {
        corpus: (
            load(args.input_dir / f"{corpus}-{args.scenario}-control.tsv"),
            load(args.input_dir / f"{corpus}-{args.scenario}-root.tsv"),
        )
        for corpus in CORPORA
    }
    checkpoints = []
    for checkpoint in args.checkpoints:
        eligible = [
            (corpus, control[checkpoint], root[checkpoint])
            for corpus, (control, root) in paired.items()
            if checkpoint in control and checkpoint in root
        ]
        if not eligible:
            continue
        raw = sum(int(root["cumulative_raw_bytes"]) for _, _, root in eligible)
        control_wire = sum(
            int(control["cumulative_charged_bytes"])
            for _, control, _ in eligible
        )
        root_wire = sum(
            int(root["cumulative_charged_bytes"]) for _, _, root in eligible
        )
        checkpoints.append(
            {
                "tu": checkpoint,
                "corpora": len(eligible),
                "raw_bytes": raw,
                "control_wire_bytes": control_wire,
                "root_wire_bytes": root_wire,
                "saving_bytes": control_wire - root_wire,
                "saving_fraction": 1 - root_wire / control_wire,
                "control_ratio": raw / control_wire,
                "root_ratio": raw / root_wire,
                "strict_win_corpora": sum(
                    int(root["cumulative_charged_bytes"])
                    < int(control["cumulative_charged_bytes"])
                    for _, control, root in eligible
                ),
            }
        )
    first_wins = {}
    for corpus, (control, root) in paired.items():
        first_wins[corpus] = next(
            (
                ordinal
                for ordinal in sorted(set(control) & set(root))
                if int(root[ordinal]["cumulative_charged_bytes"])
                < int(control[ordinal]["cumulative_charged_bytes"])
            ),
            None,
        )
    report = {
        "experiment": "paired exact prior-root learning curve",
        "scenario": args.scenario,
        "checkpoints": checkpoints,
        "first_cumulative_win_tu": first_wins,
    }
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
