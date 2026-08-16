#!/usr/bin/env python3
"""Execute complementary deterministic half-warm Line-cache scenarios."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from generated_array_line_codec import (
    parse_trace,
    persistent_stream,
    prepare_corpus,
)
from line_stream_ceiling import Corpus, load_corpus


def partition_bit(line: bytes) -> int:
    return hashlib.blake2b(
        line,
        digest_size=8,
        person=b"ice-hc50",
    ).digest()[0] & 1


def missing_partition(corpus: Corpus, cold_bit: int) -> tuple[Corpus, dict]:
    missing_by_tu = []
    warm_lines = warm_bytes = missing_lines = missing_bytes = 0
    for original in corpus.lines_by_tu:
        missing = [line for line in original if partition_bit(line) == cold_bit]
        warm = [line for line in original if partition_bit(line) != cold_bit]
        if sorted((*missing, *warm)) != sorted(original):
            raise ValueError("half-cold Line partition does not reconstruct the source set")
        missing_by_tu.append(missing)
        missing_lines += len(missing)
        missing_bytes += sum(map(len, missing))
        warm_lines += len(warm)
        warm_bytes += sum(map(len, warm))
    return (
        Corpus(
            corpus.name,
            corpus.path,
            list(corpus.raw_by_tu),
            missing_by_tu,
            missing_bytes,
            corpus.candidates,
        ),
        {
            "cold_bit": cold_bit,
            "preinstalled_lines": warm_lines,
            "preinstalled_line_bytes": warm_bytes,
            "missing_lines": missing_lines,
            "missing_line_bytes": missing_bytes,
        },
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", action="append", type=parse_trace, required=True)
    parser.add_argument("--max-tus", type=int, default=0)
    parser.add_argument("--levels", nargs="+", type=int, choices=(1, 3), default=(1, 3))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    rows = []
    for name, path in args.trace:
        corpus = load_corpus(name, path, args.max_tus)
        total_lines = sum(map(len, corpus.lines_by_tu))
        for cold_bit in (0, 1):
            missing, counts = missing_partition(corpus, cold_bit)
            if counts["missing_lines"] + counts["preinstalled_lines"] != total_lines:
                raise ValueError("half-cold Line counts do not sum")
            if (
                counts["missing_line_bytes"] + counts["preinstalled_line_bytes"]
                != corpus.line_bytes
            ):
                raise ValueError("half-cold Line bytes do not sum")
            prepared = prepare_corpus(missing, extended=True)
            for level in args.levels:
                measured = persistent_stream(prepared, level, True)
                row = {
                    "corpus": name,
                    "tus": len(corpus.raw_by_tu),
                    "raw_bytes": sum(corpus.raw_by_tu),
                    "total_lines": total_lines,
                    "total_line_bytes": corpus.line_bytes,
                    "level": level,
                    **counts,
                    **measured,
                }
                rows.append(row)
                print(
                    json.dumps(
                        {
                            "corpus": name,
                            "cold_bit": cold_bit,
                            "level": level,
                            "wire_bytes": row["wire_bytes"],
                            "missing_lines": row["missing_lines"],
                            "exact": row["exact"],
                        },
                        sort_keys=True,
                    ),
                    flush=True,
                )
    report = {
        "experiment": "exact complementary hash-partitioned half-warm Line cache",
        "partition": "blake2b-64(person=ice-hc50) low bit; cold_bit 0 and 1",
        "max_tus": args.max_tus,
        "rows": rows,
        "exact": all(row["exact"] for row in rows),
    }
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
