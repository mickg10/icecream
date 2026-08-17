#!/usr/bin/env python3
"""Small exactness/accounting test for bsc_block_granularity."""

from __future__ import annotations

import argparse
import csv
import struct
import subprocess
import tempfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bench", type=Path, required=True)
    args = parser.parse_args()
    bench = args.bench.resolve()
    if not bench.is_file():
        raise FileNotFoundError(bench)

    lane_parts = [b"alpha" * 2000, b"", b"beta" * 4000, b"alpha" * 3000]
    other_parts = [b"x", b"yz", b"", b"other"]
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        raw = root / "lane.raw"
        lengths = root / "lengths.bin"
        output = root / "summary.tsv"
        detail = root / "detail.tsv"
        raw.write_bytes(b"".join(lane_parts))
        lengths.write_bytes(
            b"".join(
                struct.pack("<II", len(other), len(lane))
                for other, lane in zip(other_parts, lane_parts, strict=True)
            )
        )
        completed = subprocess.run(
            [
                str(bench),
                "--raw",
                str(raw),
                "--lengths",
                str(lengths),
                "--stride",
                "2",
                "--lane",
                "1",
                "--group-tus",
                "1,2,full",
                "--output",
                str(output),
                "--detail-output",
                str(detail),
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if completed.returncode:
            raise RuntimeError(completed.stdout)
        if completed.stdout:
            raise RuntimeError(f"unexpected standard output: {completed.stdout}")
        rows = list(csv.DictReader(output.open(), delimiter="\t"))
        if [row["group_tus"] for row in rows] != ["1", "2", "full"]:
            raise RuntimeError(f"unexpected rows:\n{completed.stdout}")
        expected_raw = sum(map(len, lane_parts))
        for row in rows:
            if row["exact"] != "true" or int(row["raw_bytes"]) != expected_raw:
                raise RuntimeError(f"invalid exactness row: {row}")
            selected = int(row["selected_wire_bytes"])
            candidates = [
                int(row["bsc_wire_bytes"]),
                int(row["zstd3_wire_bytes"]),
                int(row["zstd10_wire_bytes"]),
            ]
            if selected != min(candidates):
                raise RuntimeError(f"selector did not choose minimum: {row}")
        detail_rows = list(csv.DictReader(detail.open(), delimiter="\t"))
        if len(detail_rows) != 4 + 2 + 1:
            raise RuntimeError(f"unexpected detail row count: {len(detail_rows)}")
        by_group: dict[str, list[dict[str, str]]] = {}
        for row in detail_rows:
            by_group.setdefault(row["group_tus"], []).append(row)
        for group, group_rows in by_group.items():
            cumulative = 0
            for row in group_rows:
                cumulative += int(row["selected_wire_bytes"])
                if cumulative != int(row["cumulative_selected_wire_bytes"]):
                    raise RuntimeError(f"bad watermark in group {group}: {row}")
        print("bsc block granularity exactness/accounting PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
