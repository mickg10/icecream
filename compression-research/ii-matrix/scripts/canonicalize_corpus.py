#!/usr/bin/env python3
"""Build the deterministic archive tree from an ordered manifest of loose .ii files."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil


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
    parser.add_argument("--raw-manifest", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--move", action="store_true")
    args = parser.parse_args()

    if args.stage.exists() and any(args.stage.iterdir()):
        raise SystemExit(f"refusing nonempty stage: {args.stage}")
    ii_dir = args.stage / "ii"
    ii_dir.mkdir(parents=True, exist_ok=True)

    source_paths = [Path(line.rstrip("\r\n")) for line in args.raw_manifest.read_text().splitlines()
                    if line.strip()]
    if not source_paths:
        raise SystemExit("empty raw manifest")

    rows = ["ordinal\trelative_path\traw_bytes\tsha256\toriginal_path\n"]
    seen: set[Path] = set()
    for ordinal, source in enumerate(source_paths):
        if "\t" in str(source) or "\n" in str(source):
            raise SystemExit(f"unsupported path characters: {source!s}")
        resolved = source.resolve(strict=True)
        if resolved in seen:
            raise SystemExit(f"duplicate TU path: {resolved}")
        seen.add(resolved)
        relative = Path("ii") / f"{ordinal:08d}.ii"
        target = args.stage / relative
        if args.move:
            try:
                os.replace(resolved, target)
            except OSError:
                shutil.copyfile(resolved, target)
                resolved.unlink()
        else:
            try:
                os.link(resolved, target)
            except OSError:
                shutil.copyfile(resolved, target)
        size, sha256 = digest(target)
        rows.append(f"{ordinal}\t{relative.as_posix()}\t{size}\t{sha256}\t{source}\n")

    (args.stage / "manifest.tsv").write_text("".join(rows), encoding="utf-8")
    print(f"canonicalized {len(source_paths)} TUs into {args.stage}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
