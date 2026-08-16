#!/usr/bin/env python3
"""Summarize P28's deeper deterministic S1 match search."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path

from summarize_complete_codec50 import CORPORA, aggregate
from summarize_half_cold_codec import parse_key_map


STATES = ("cold", "bit0", "bit1")
EXPECTED_BITS = {"cold": -1, "bit0": 0, "bit1": 1}
UNCHANGED = ("line_def", "region_def", "path_def", "missing", "framing")
IDENTITY = ("tus", "raw_bytes", "regions", "region_occurrences", "distinct_lines")
S1 = re.compile(
    r"S1 LZ: [0-9.]+s min_match=(\d+) max_chain=(\d+) "
    r"tokens=(\d+) blocks=(\d+)"
)


def log_path(directory: Path, index: int, corpus: str) -> Path:
    named = directory / f"{corpus}.log"
    return named if named.exists() else directory / f"corpus{index}.log"


def parse_p28(path: Path, bit: int) -> dict[str, object]:
    row = parse_key_map(path, bit)
    match = S1.search(path.read_text())
    if match is None:
        raise ValueError(f"{path}: missing parameterized S1 ledger")
    minimum, chain, tokens, blocks = map(int, match.groups())
    if (minimum, chain) != (3, 1024):
        raise ValueError(f"{path}: expected S1 3/1024, got {minimum}/{chain}")
    row.update({"s1_tokens": tokens, "s1_blocks": blocks})
    return row


def target(state: str, row: dict[str, object]) -> dict[str, object]:
    divisor = 400 if state == "cold" else 200
    raw = int(row["raw_bytes"])
    wire = int(row["total_wire_bytes"])
    allowance = raw / divisor
    return {
        "ratio": divisor,
        "allowance_bytes": allowance,
        "gap_bytes": wire - allowance,
        "passed": wire <= allowance,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    for state in STATES:
        parser.add_argument(f"--p27-{state}-dir", type=Path, required=True)
        parser.add_argument(f"--p28-{state}-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    per_corpus: list[dict[str, object]] = [{"corpus": corpus} for corpus in CORPORA]
    states: dict[str, object] = {}
    for state in STATES:
        bit = EXPECTED_BITS[state]
        control_dir = getattr(args, f"p27_{state}_dir")
        candidate_dir = getattr(args, f"p28_{state}_dir")
        controls: list[dict[str, object]] = []
        candidates: list[dict[str, object]] = []
        for index, (corpus, item) in enumerate(zip(CORPORA, per_corpus), 1):
            control = parse_key_map(log_path(control_dir, index, corpus), bit)
            candidate = parse_p28(log_path(candidate_dir, index, corpus), bit)
            for field in IDENTITY:
                if candidate[field] != control[field]:
                    raise ValueError(f"{state}/{corpus}: {field} differs")
            for category in UNCHANGED:
                field = f"{category}_wire_bytes"
                if candidate[field] != control[field]:
                    raise ValueError(f"{state}/{corpus}: {category} differs")
            root_saving = int(control["root_wire_bytes"]) - int(
                candidate["root_wire_bytes"]
            )
            block_saving = int(control["block_def_wire_bytes"]) - int(
                candidate["block_def_wire_bytes"]
            )
            saving = int(control["total_wire_bytes"]) - int(
                candidate["total_wire_bytes"]
            )
            if saving != root_saving + block_saving or saving < 0:
                raise ValueError(f"{state}/{corpus}: S1 saving does not close")
            raw = int(candidate["raw_bytes"])
            item.update(
                {
                    "tus": candidate["tus"],
                    "raw_bytes": raw,
                    f"{state}_p27_wire_bytes": control["total_wire_bytes"],
                    f"{state}_p28_wire_bytes": candidate["total_wire_bytes"],
                    f"{state}_saving_bytes": saving,
                    f"{state}_root_saving_bytes": root_saving,
                    f"{state}_block_saving_bytes": block_saving,
                    f"{state}_p28_ratio": raw / int(candidate["total_wire_bytes"]),
                    f"{state}_p28_pipeline_gbps": candidate["pipeline_gbps"],
                    f"{state}_s1_tokens": candidate["s1_tokens"],
                    f"{state}_s1_blocks": candidate["s1_blocks"],
                    f"{state}_log_sha256": candidate["log_sha256"],
                }
            )
            controls.append(control)
            candidates.append(candidate)
        old = aggregate(controls)
        new = aggregate(candidates)
        states[state] = {
            "p27": old,
            "p28": new,
            "saving_bytes": int(old["total_wire_bytes"]) - int(new["total_wire_bytes"]),
            "target": target(state, new),
        }

    report = {
        "experiment": "P28 deterministic S1 max-chain 64 to 1024",
        "scope": (
            "complete direct-ordinal P27 wire; only Root and Block definition "
            "bytes may change; cold and complementary cache states"
        ),
        "complete_runs": len(STATES) * len(CORPORA),
        "exact_runs": len(STATES) * len(CORPORA),
        "all_per_corpus_states_non_regressing": all(
            int(item[f"{state}_saving_bytes"]) >= 0
            for item in per_corpus
            for state in STATES
        ),
        "states": states,
        "per_corpus": per_corpus,
    }
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    with args.tsv.open("w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=tuple(per_corpus[0]),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(per_corpus)
    print(
        json.dumps(
            {key: value for key, value in report.items() if key != "per_corpus"},
            indent=2,
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
