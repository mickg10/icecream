#!/usr/bin/env python3
"""Summarize the complete P27 Root/Block structural ceiling."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from pathlib import Path

from summarize_complete_codec50 import CORPORA
from summarize_half_cold_codec import parse_key_map


STRUCTURE = re.compile(
    r"structure ceiling .*: raw_root=(\d+) raw_block=(\d+) "
    r"current_root\+block\+root_frames=(\d+)"
)
SEPARATE = re.compile(
    r"batched separate: root=(\d+) block=(\d+) combined=(\d+) "
    r"saving=(-?\d+) => total=(\d+) ratio=([0-9.]+)x"
)
JOINT = re.compile(
    r"batched joint: wire=(\d+) saving=(-?\d+) => total=(\d+) "
    r"ratio=([0-9.]+)x"
)
LDM = re.compile(
    r"batched joint \+ LDM/win27: wire=(\d+) saving=(-?\d+) "
    r"=> total=(\d+) ratio=([0-9.]+)x"
)
ZERO = re.compile(
    r"impossible zero-byte structure bound: saving=(\d+) => total=(\d+) "
    r"ratio=([0-9.]+)x"
)


def find(pattern: re.Pattern[str], text: str, path: Path) -> tuple[str, ...]:
    found = pattern.search(text)
    if found is None:
        raise ValueError(f"{path}: missing {pattern.pattern}")
    return found.groups()


def parse(path: Path, corpus: str) -> dict[str, object]:
    base = parse_key_map(path, -1)
    text = path.read_text()
    raw_root, raw_block, current = map(int, find(STRUCTURE, text, path))
    separate = find(SEPARATE, text, path)
    joint = find(JOINT, text, path)
    ldm = find(LDM, text, path)
    zero = find(ZERO, text, path)
    total = int(base["total_wire_bytes"])
    row: dict[str, object] = {
        "corpus": corpus,
        "tus": int(base["tus"]),
        "raw_bytes": int(base["raw_bytes"]),
        "total_wire_bytes": total,
        "current_ratio": int(base["raw_bytes"]) / total,
        "root_raw_bytes": raw_root,
        "block_raw_bytes": raw_block,
        "current_structure_wire_bytes": current,
        "separate_root_wire_bytes": int(separate[0]),
        "separate_block_wire_bytes": int(separate[1]),
        "separate_wire_bytes": int(separate[2]),
        "separate_saving_bytes": int(separate[3]),
        "separate_projected_wire_bytes": int(separate[4]),
        "joint_wire_bytes": int(joint[0]),
        "joint_saving_bytes": int(joint[1]),
        "joint_projected_wire_bytes": int(joint[2]),
        "ldm_wire_bytes": int(ldm[0]),
        "ldm_saving_bytes": int(ldm[1]),
        "ldm_projected_wire_bytes": int(ldm[2]),
        "zero_projected_wire_bytes": int(zero[1]),
        "zero_projected_ratio": int(base["raw_bytes"]) / int(zero[1]),
        "log_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }
    if int(separate[2]) != int(separate[0]) + int(separate[1]):
        raise ValueError(f"{path}: separate parts do not sum")
    for prefix in ("separate", "joint", "ldm"):
        wire = int(row[f"{prefix}_wire_bytes"])
        saving = int(row[f"{prefix}_saving_bytes"])
        projected = int(row[f"{prefix}_projected_wire_bytes"])
        if saving != current - wire or projected != total - saving:
            raise ValueError(f"{path}: {prefix} arithmetic differs")
    if int(zero[0]) != current or int(zero[1]) != total - current:
        raise ValueError(f"{path}: zero bound arithmetic differs")
    return row


def aggregate(rows: list[dict[str, object]]) -> dict[str, object]:
    summed = {
        field: sum(int(row[field]) for row in rows)
        for field in (
            "raw_bytes",
            "total_wire_bytes",
            "root_raw_bytes",
            "block_raw_bytes",
            "current_structure_wire_bytes",
            "separate_wire_bytes",
            "joint_wire_bytes",
            "ldm_wire_bytes",
        )
    }
    raw = summed["raw_bytes"]
    total = summed["total_wire_bytes"]
    current = summed["current_structure_wire_bytes"]
    for name in ("separate", "joint", "ldm"):
        wire = summed[f"{name}_wire_bytes"]
        projected = total - current + wire
        summed[f"{name}_saving_bytes"] = current - wire
        summed[f"{name}_projected_wire_bytes"] = projected
        summed[f"{name}_projected_ratio"] = raw / projected
    zero = total - current
    summed.update(
        {
            "current_ratio": raw / total,
            "zero_projected_wire_bytes": zero,
            "zero_projected_ratio": raw / zero,
            "cold_400_allowance_bytes": raw / 400,
            "cold_400_gap_after_zero_structure_bytes": zero - raw / 400,
        }
    )
    return summed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tsv", type=Path, required=True)
    args = parser.parse_args()

    rows = [
        parse(args.log_dir / f"corpus{index}.log", corpus)
        for index, corpus in enumerate(CORPORA, 1)
    ]
    report = {
        "experiment": "P27 complete Root/Block whole-run structural ceiling",
        "scope": (
            "cold direct-ordinal P27; current S1 Root and Block wire including "
            "the four-byte Root frame per TU; diagnostic batches omit TU boundaries"
        ),
        "aggregate": aggregate(rows),
        "complete_runs": len(rows),
        "exact_runs": len(rows),
        "per_corpus": rows,
    }
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    with args.tsv.open("w", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=tuple(rows[0]),
            delimiter="\t",
            lineterminator="\n",
        )
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps(report["aggregate"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
