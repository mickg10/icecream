#!/usr/bin/env python3
"""Summarize exact per-F prior-root capability probes."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", action="append", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = []
    for path in args.report:
        report = json.loads(path.read_text())
        level = report["levels"]["3"]
        if not report["exact"]:
            raise ValueError(f"inexact per-F report: {path}")
        baseline = level["baseline_wire_bytes"]
        selected = level["selected_wire_bytes"]
        rows.append(
            {
                "corpus": Path(report["trace"]).stem.removeprefix("ml-"),
                "tus": report["tus"],
                "endpoints": report["endpoints"],
                "assignment": report["assignment"],
                "baseline_wire_bytes": baseline,
                "selected_wire_bytes": selected,
                "saving_bytes": baseline - selected,
                "saving_fraction": 1 - selected / baseline,
                "copied_fraction": report["copied_fraction"],
                "maximum_endpoint_encoder_state_bytes": report[
                    "maximum_endpoint_encoder_state_bytes"
                ],
                "maximum_endpoint_receiver_state_bytes": report[
                    "maximum_endpoint_receiver_state_bytes"
                ],
                "exact": True,
            }
        )
    rows.sort(key=lambda row: (row["corpus"], row["assignment"], row["endpoints"]))
    output = {
        "experiment": "exact independent-F prior-root availability probe",
        "rows": rows,
        "exact_rows": sum(row["exact"] for row in rows),
    }
    args.output.write_text(json.dumps(output, indent=2, sort_keys=True) + "\n")
    print(json.dumps(output, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
