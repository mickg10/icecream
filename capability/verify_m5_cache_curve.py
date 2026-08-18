#!/usr/bin/env python3
"""Independently verify an M5 per-F cache-transition curve."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


FIELDS = [
    "logical",
    "worker",
    "before_regions",
    "before_public",
    "before_blocks",
    "before_paths",
    "before_region_hash",
    "before_public_hash",
    "before_block_hash",
    "need_regions",
    "install_regions",
    "install_public",
    "install_blocks",
    "install_paths",
    "filled_regions",
    "filled_public",
    "filled_blocks",
    "filled_paths",
    "filled_region_hash",
    "filled_public_hash",
    "filled_block_hash",
    "drop_regions",
    "drop_public",
    "drop_blocks",
    "after_regions",
    "after_public",
    "after_blocks",
    "after_paths",
    "after_region_hash",
    "after_public_hash",
    "after_block_hash",
    "chain_ok",
    "accounting_ok",
]

STATE_KINDS = ("regions", "public", "blocks")


def state(row: dict[str, int], prefix: str) -> tuple[int, ...]:
    return (
        row[f"{prefix}_regions"],
        row[f"{prefix}_public"],
        row[f"{prefix}_blocks"],
        row[f"{prefix}_paths"],
        row[f"{prefix}_region_hash"],
        row[f"{prefix}_public_hash"],
        row[f"{prefix}_block_hash"],
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def verify(path: Path, expected_tus: int | None, require_removals: bool) -> None:
    previous: dict[int, tuple[int, ...]] = {}
    drops = {kind: 0 for kind in STATE_KINDS}
    rows = 0
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        require(reader.fieldnames == FIELDS, "unexpected cache-curve schema")
        for physical, raw in enumerate(reader):
            row = {field: int(raw[field]) for field in FIELDS}
            logical = row["logical"]
            worker = row["worker"]
            require(logical == physical, f"row {physical}: logical={logical}")
            require(row["chain_ok"] == 1, f"TU {logical}: producer chain failed")
            require(
                row["accounting_ok"] == 1,
                f"TU {logical}: producer accounting failed",
            )

            before = state(row, "before")
            if worker in previous:
                require(
                    before == previous[worker],
                    f"TU {logical}: F{worker} state does not chain",
                )
            else:
                require(
                    before[0:4] == (0, 0, 0, 0),
                    f"TU {logical}: F{worker} did not begin empty",
                )

            require(
                row["install_regions"] == row["need_regions"],
                f"TU {logical}: Need/install Region mismatch",
            )
            for kind in STATE_KINDS:
                require(
                    row[f"filled_{kind}"]
                    == row[f"before_{kind}"] + row[f"install_{kind}"],
                    f"TU {logical}: {kind} Fill equation failed",
                )
                require(
                    row[f"after_{kind}"] + row[f"drop_{kind}"]
                    == row[f"filled_{kind}"],
                    f"TU {logical}: {kind} eviction equation failed",
                )
                drops[kind] += row[f"drop_{kind}"]
            require(
                row["filled_paths"]
                == row["before_paths"] + row["install_paths"],
                f"TU {logical}: path Fill equation failed",
            )
            require(
                row["after_paths"] == row["filled_paths"],
                f"TU {logical}: path state regressed",
            )
            previous[worker] = state(row, "after")
            rows += 1

    require(rows > 0, "empty cache curve")
    if expected_tus is not None:
        require(rows == expected_tus, f"expected {expected_tus} TUs, found {rows}")
    if require_removals:
        require(sum(drops.values()) > 0, "expected at least one removal")
    print(
        "CACHE_CURVE PASS "
        f"rows={rows} workers={len(previous)} "
        f"drops={drops['regions']}/{drops['public']}/{drops['blocks']}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("curve", type=Path)
    parser.add_argument("--expect-tus", type=int)
    parser.add_argument("--require-removals", action="store_true")
    arguments = parser.parse_args()
    try:
        verify(arguments.curve, arguments.expect_tus, arguments.require_removals)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
