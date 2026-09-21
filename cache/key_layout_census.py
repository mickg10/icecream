#!/usr/bin/env python3
"""Quick, reproducible KeyLayoutV1 runway census over checked-in measurements."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


ORDINAL_CAPACITY = (1 << 49) - 1


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def render(root: Path) -> str:
    runway_path = root / "research/measurements/runway-census.tsv"
    firefox_path = root / "research/measurements/firefox-corrected.compile-trace.tsv"
    runway = read_tsv(runway_path)
    firefox = read_tsv(firefox_path)
    runway_tus = sum(int(row["total_tus"]) for row in runway)
    runway_raw = sum(int(row["raw_bytes"]) for row in runway)
    firefox_raw = sum(int(row["raw_bytes"]) for row in firefox)
    total_raw = runway_raw + firefox_raw
    factor = ORDINAL_CAPACITY / total_raw
    occupancy = total_raw / ORDINAL_CAPACITY * 100
    return f"""# KeyLayoutV1 quick runway census

This is a deliberately bounded sanity check, not a new compression experiment. It reads the
checked-in 19-corpus runway table and corrected Firefox job trace. The conservative comparison
charges one new object ordinal for every raw input byte; real Line, Region, Block, and other
objects are much coarser and repeated content reuses an earlier key.

| checked-in input | builds/TUs | raw bytes |
|---|---:|---:|
| `research/measurements/runway-census.tsv` | {len(runway)} corpora / {runway_tus:,} TUs | {runway_raw:,} |
| `research/measurements/firefox-corrected.compile-trace.tsv` | 1 build / {len(firefox):,} TUs | {firefox_raw:,} |
| combined conservative charge | {runway_tus + len(firefox):,} TUs | {total_raw:,} |

KeyLayoutV1 is derived in `protocol50.h`: 5 type bits, 10 generation bits, and 49 ordinal
bits. One `(type, generation)` therefore has **{ORDINAL_CAPACITY:,}** usable nonzero ordinals.
Even the one-object-per-byte charge consumes **{occupancy:.6f}%** of one generation, leaving a
**{factor:,.1f}x** runway. There are 1,024 generation values, including generation zero; old
and new generations may coexist. This check supports the 5/10/49 split without importing any
product Key64 assumptions into the historical capability harnesses.

Run `python3 cache/key_layout_census.py --check cache/KEY_LAYOUT_V1_CENSUS.md` after changing
the layout or either checked-in measurement input.
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--check", type=Path)
    args = parser.parse_args()
    output = render(args.root)
    if args.check:
        expected = args.check.read_text(encoding="utf-8")
        if expected != output:
            raise SystemExit(f"{args.check} is stale; rerun the census and review the result")
    else:
        print(output, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
