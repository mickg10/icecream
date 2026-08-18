#!/usr/bin/env python3
"""Freeze or verify the exact corpus generation selected by corpus.json.

Cell directories may retain older, differently named archives.  This tool never
globs archive names: it resolves only ``payload.path`` from ``corpus.json`` and
binds that compressed payload to the cell manifest and declared extents.
"""

from __future__ import annotations

import argparse
import csv
import difflib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sys
from typing import Any


FIELDS = (
    "project",
    "profile",
    "tu_count",
    "raw_bytes",
    "payload_path",
    "payload_bytes",
    "payload_sha256",
    "corpus_sha256",
    "manifest_sha256",
)
REQUIRED_MANIFEST_FIELDS = {"ordinal", "relative_path", "raw_bytes", "sha256"}
SHA256_RE = re.compile(r"[0-9a-f]{64}")
CHUNK = 8 * 1024 * 1024


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_relative_path(value: object, description: str) -> Path:
    require(isinstance(value, str) and value != "", f"missing {description}")
    path = Path(value)
    require(
        not path.is_absolute()
        and path.parts
        and all(part not in {"", ".", ".."} for part in path.parts),
        f"invalid {description}: {value!r}",
    )
    return path


def checked_nonnegative_int(value: object, description: str) -> int:
    require(not isinstance(value, bool), f"invalid {description}: {value!r}")
    try:
        result = int(value)  # type: ignore[arg-type]
    except (TypeError, ValueError) as error:
        raise RuntimeError(f"invalid {description}: {value!r}") from error
    require(result >= 0, f"negative {description}: {result}")
    return result


def read_cell(corpus_path: Path) -> dict[str, str]:
    cell_dir = corpus_path.parent
    corpus = json.loads(corpus_path.read_text())
    require(corpus.get("schema") == "ice-ii-corpus-v1", f"wrong schema: {corpus_path}")

    project = str(corpus.get("project", ""))
    profile = str(corpus.get("profile", ""))
    require(project == cell_dir.parent.name, f"project/directory mismatch: {corpus_path}")
    require(profile == cell_dir.name, f"profile/directory mismatch: {corpus_path}")

    payload = corpus.get("payload")
    require(isinstance(payload, dict), f"missing payload object: {corpus_path}")
    payload_path = checked_relative_path(payload.get("path"), "payload.path")
    archive = cell_dir / payload_path
    require(archive.is_file(), f"missing declared payload: {archive}")
    payload_bytes = checked_nonnegative_int(payload.get("bytes"), "payload.bytes")
    require(archive.stat().st_size == payload_bytes, f"payload byte count differs: {archive}")
    declared_payload_sha = str(payload.get("sha256", ""))
    require(SHA256_RE.fullmatch(declared_payload_sha) is not None, "invalid payload SHA-256")
    actual_payload_sha = sha256_file(archive)
    require(actual_payload_sha == declared_payload_sha, f"payload SHA-256 differs: {archive}")

    manifest_path = cell_dir / "manifest.tsv"
    require(manifest_path.is_file(), f"missing manifest: {manifest_path}")
    with manifest_path.open(newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        require(reader.fieldnames is not None, f"missing manifest header: {manifest_path}")
        require(
            REQUIRED_MANIFEST_FIELDS <= set(reader.fieldnames),
            f"incomplete manifest header: {manifest_path}",
        )
        rows = list(reader)

    relative_paths: set[Path] = set()
    raw_bytes = 0
    for expected_ordinal, row in enumerate(rows):
        ordinal = checked_nonnegative_int(row["ordinal"], "manifest ordinal")
        require(ordinal == expected_ordinal, f"non-contiguous ordinal in {manifest_path}")
        relative_path = checked_relative_path(row["relative_path"], "manifest relative_path")
        require(relative_path not in relative_paths, f"duplicate manifest path: {relative_path}")
        relative_paths.add(relative_path)
        raw_bytes += checked_nonnegative_int(row["raw_bytes"], "manifest raw_bytes")
        require(SHA256_RE.fullmatch(row["sha256"]) is not None, "invalid manifest SHA-256")

    declared_tus = checked_nonnegative_int(corpus.get("tu_count"), "corpus tu_count")
    declared_raw = checked_nonnegative_int(corpus.get("raw_bytes"), "corpus raw_bytes")
    require(len(rows) == declared_tus, f"manifest TU count differs: {manifest_path}")
    require(raw_bytes == declared_raw, f"manifest raw extent differs: {manifest_path}")

    return {
        "project": project,
        "profile": profile,
        "tu_count": str(declared_tus),
        "raw_bytes": str(declared_raw),
        "payload_path": str(payload_path),
        "payload_bytes": str(payload_bytes),
        "payload_sha256": actual_payload_sha,
        "corpus_sha256": sha256_file(corpus_path),
        "manifest_sha256": sha256_file(manifest_path),
    }


def read_matrix(matrix_root: Path, expected_cells: int) -> list[dict[str, str]]:
    corpus_paths = sorted(matrix_root.glob("*/*/corpus.json"))
    require(
        len(corpus_paths) == expected_cells,
        f"expected {expected_cells} corpus cells, found {len(corpus_paths)}",
    )
    rows = [read_cell(path) for path in corpus_paths]
    identities = [(row["project"], row["profile"]) for row in rows]
    require(len(set(identities)) == len(identities), "duplicate project/profile identity")
    return rows


def render_tsv(rows: list[dict[str, str]]) -> str:
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, delimiter="\t", lineterminator="\n", fieldnames=FIELDS)
    writer.writeheader()
    writer.writerows(rows)
    return stream.getvalue()


def atomic_write(path: Path, content: str) -> None:
    partial = path.with_name(path.name + ".new")
    partial.write_text(content)
    os.replace(partial, path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--matrix-root", required=True, type=Path)
    parser.add_argument("--expected-cells", type=int, default=44)
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--output", type=Path)
    output.add_argument("--check", type=Path)
    args = parser.parse_args()

    try:
        content = render_tsv(read_matrix(args.matrix_root.resolve(), args.expected_cells))
        if args.check is not None:
            expected = args.check.read_text()
            if expected != content:
                sys.stderr.writelines(
                    difflib.unified_diff(
                        expected.splitlines(keepends=True),
                        content.splitlines(keepends=True),
                        fromfile=str(args.check),
                        tofile="current corpus generation",
                    )
                )
                return 1
            print(f"verified {args.expected_cells} corpus cells against {args.check}")
        elif args.output is not None:
            atomic_write(args.output, content)
            print(f"froze {args.expected_cells} corpus cells in {args.output}")
        else:
            sys.stdout.write(content)
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"ledger verification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
