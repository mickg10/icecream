#!/usr/bin/env python3
"""Summarize exact paired control/prior-root reports."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from run_prior_root_matrix import CORPORA, ROW


def selected(path: Path) -> dict:
    report = json.loads(path.read_text())
    rows = [row for row in report["rows"] if row["name"] == ROW]
    if len(rows) != 1 or not rows[0]["exact"]:
        raise ValueError(f"missing exact {ROW}: {path}")
    return rows[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--scenario", default="standard")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    rows = []
    for corpus in CORPORA:
        control = selected(args.input_dir / f"{corpus}-{args.scenario}-control.json")
        root = selected(args.input_dir / f"{corpus}-{args.scenario}-root.json")
        if control["raw_bytes"] != root["raw_bytes"] or control["tus"] != root["tus"]:
            raise ValueError(f"unpaired target boundary: {corpus}")
        rows.append(
            {
                "corpus": corpus,
                "tus": root["tus"],
                "raw_bytes": root["raw_bytes"],
                "control_wire_bytes": control["charged_wire_bytes"],
                "root_wire_bytes": root["charged_wire_bytes"],
                "saving_bytes": control["charged_wire_bytes"]
                - root["charged_wire_bytes"],
                "saving_fraction": 1
                - root["charged_wire_bytes"] / control["charged_wire_bytes"],
                "control_ratio": control["charged_ratio"],
                "root_ratio": root["charged_ratio"],
                "selected_tus": root["root_copy_selected_tus"],
                "copies": root["root_copy_copies"],
                "copied_regions": root["root_copy_copied_regions"],
                "receiver_state_bytes": root["root_copy_receiver_state_bytes"],
                "encoder_state_bytes": root["root_copy_encoder_state_bytes"],
                "exact": root["exact"],
            }
        )
    raw = sum(row["raw_bytes"] for row in rows)
    control_wire = sum(row["control_wire_bytes"] for row in rows)
    root_wire = sum(row["root_wire_bytes"] for row in rows)
    summary = {
        "scenario": args.scenario,
        "corpora": len(rows),
        "tus": sum(row["tus"] for row in rows),
        "raw_bytes": raw,
        "control_wire_bytes": control_wire,
        "root_wire_bytes": root_wire,
        "saving_bytes": control_wire - root_wire,
        "saving_fraction": 1 - root_wire / control_wire,
        "weighted_control_ratio": raw / control_wire,
        "weighted_root_ratio": raw / root_wire,
        "equal_corpus_control_ratio": len(rows)
        / sum(1 / row["control_ratio"] for row in rows),
        "equal_corpus_root_ratio": len(rows)
        / sum(1 / row["root_ratio"] for row in rows),
        "strict_win_corpora": sum(row["saving_bytes"] > 0 for row in rows),
        "tie_corpora": sum(row["saving_bytes"] == 0 for row in rows),
        "loss_corpora": sum(row["saving_bytes"] < 0 for row in rows),
        "exact_corpora": sum(row["exact"] for row in rows),
        "maximum_receiver_state_bytes": max(
            row["receiver_state_bytes"] for row in rows
        ),
        "maximum_encoder_state_bytes": max(
            row["encoder_state_bytes"] for row in rows
        ),
        "per_corpus": rows,
    }
    rendered = json.dumps(summary, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
