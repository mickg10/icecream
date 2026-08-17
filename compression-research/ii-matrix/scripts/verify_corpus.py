#!/usr/bin/env python3
"""Verify every archived TU and emit an absolute manifest for codec readers."""

from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path


def digest(path: Path) -> tuple[int, str]:
    hashed = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            size += len(chunk)
            hashed.update(chunk)
    return size, hashed.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--manifest-out", type=Path)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    table = root / "manifest.tsv"
    output: list[str] = []
    expected_ordinal = 0
    with table.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            ordinal = int(row["ordinal"])
            if ordinal != expected_ordinal:
                raise SystemExit(f"non-contiguous ordinal {ordinal}, expected {expected_ordinal}")
            expected_ordinal += 1
            relative = Path(row["relative_path"])
            if relative.is_absolute() or ".." in relative.parts:
                raise SystemExit(f"invalid relative path: {relative}")
            path = (root / relative).resolve(strict=True)
            if root not in path.parents:
                raise SystemExit(f"path leaves corpus root: {path}")
            size, sha256 = digest(path)
            if size != int(row["raw_bytes"]) or sha256 != row["sha256"]:
                raise SystemExit(f"TU verification failed: {relative}")
            output.append(str(path))
    if not output:
        raise SystemExit("empty manifest.tsv")
    destination = args.manifest_out or root / "manifest.txt"
    destination.write_text("\n".join(output) + "\n", encoding="utf-8")
    print(f"verified {len(output)} TUs; manifest={destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
