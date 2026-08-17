#!/usr/bin/env python3
"""Measure one zstd frame over exact ordered file-prefix content.

The issue #16 gates compare the complete codec with one whole-program zstd
stream, never a sum of independently compressed TUs.  This runner consumes an
authoritative newline-delimited file list, concatenates bytes without tar or
path metadata, and records exact prefix measurements plus content/list hashes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path
from typing import BinaryIO, Sequence


COPY_CHUNK = 8 << 20


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(COPY_CHUNK), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_file_list(path: Path) -> list[Path]:
    raw = path.read_bytes()
    if b"\0" in raw:
        raise ValueError(f"{path}: NUL is not valid in the newline file list")
    files = [Path(value.decode()) for value in raw.splitlines() if value]
    if not files:
        raise ValueError(f"{path}: empty file list")
    missing = [value for value in files if not value.is_file()]
    if missing:
        preview = ", ".join(str(value) for value in missing[:3])
        raise FileNotFoundError(f"{path}: {len(missing)} missing files: {preview}")
    return files


def copy_and_hash(files: Sequence[Path], destination: BinaryIO) -> tuple[int, str]:
    digest = hashlib.sha256()
    raw_bytes = 0
    for path in files:
        with path.open("rb") as source:
            while chunk := source.read(COPY_CHUNK):
                destination.write(chunk)
                digest.update(chunk)
                raw_bytes += len(chunk)
    return raw_bytes, digest.hexdigest()


def zstd_version(executable: Path) -> str:
    result = subprocess.run(
        [str(executable), "--version"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def measure(
    files: Sequence[Path],
    executable: Path,
    level: int,
    long_log: int,
) -> dict:
    command = [str(executable), f"-{level}", f"--long={long_log}", "-c"]
    started = time.monotonic()
    with tempfile.TemporaryFile() as compressed:
        process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=compressed,
            stderr=subprocess.PIPE,
        )
        if process.stdin is None or process.stderr is None:
            raise RuntimeError("zstd pipes were not created")
        try:
            raw_bytes, content_sha256 = copy_and_hash(files, process.stdin)
            process.stdin.close()
            stderr = process.stderr.read()
            return_code = process.wait()
        except BaseException:
            process.kill()
            process.wait()
            raise
        if return_code:
            raise RuntimeError(
                f"zstd exited {return_code}: "
                f"{stderr.decode(errors='replace').strip()}"
            )
        compressed.seek(0, os.SEEK_END)
        compressed_bytes = compressed.tell()
    elapsed = time.monotonic() - started
    return {
        "files": len(files),
        "raw_bytes": raw_bytes,
        "content_sha256": content_sha256,
        "compressed_bytes": compressed_bytes,
        "ratio": raw_bytes / max(1, compressed_bytes),
        "seconds": elapsed,
        "input_GBps": raw_bytes / max(elapsed, 1e-12) / 1e9,
        "command": command,
    }


def boundaries(total: int, checkpoints: Sequence[int], include_full: bool) -> list[dict]:
    if total <= 0:
        raise ValueError("file list must not be empty")
    grouped: dict[int, list[int | str]] = {}
    for checkpoint in checkpoints:
        if checkpoint <= 0:
            raise ValueError("checkpoints must be positive")
        grouped.setdefault(min(checkpoint, total), []).append(checkpoint)
    if include_full:
        grouped.setdefault(total, []).append("full")
    return [
        {
            "files": count,
            "requested": requested,
            "complete": count == total,
        }
        for count, requested in sorted(grouped.items())
    ]


def parser() -> argparse.ArgumentParser:
    value = argparse.ArgumentParser()
    value.add_argument("--name", required=True)
    value.add_argument("--kind", choices=("ii", "source"), required=True)
    value.add_argument("--file-list", type=Path, required=True)
    value.add_argument("--zstd", type=Path, default=Path("zstd"))
    value.add_argument("--level", type=int, required=True)
    value.add_argument("--long-log", type=int, default=31)
    value.add_argument("--checkpoints", nargs="+", type=int, default=(100, 200))
    value.add_argument("--include-full", action="store_true")
    value.add_argument("--output", type=Path, required=True)
    return value


def main() -> int:
    args = parser().parse_args()
    files = load_file_list(args.file_list)
    version = zstd_version(args.zstd)
    rows = []
    for boundary in boundaries(len(files), args.checkpoints, args.include_full):
        row = {
            "requested": boundary["requested"],
            "complete": boundary["complete"],
            **measure(
                files[: int(boundary["files"])],
                args.zstd,
                args.level,
                args.long_log,
            ),
        }
        rows.append(row)
        print(
            json.dumps(
                {
                    "name": args.name,
                    "kind": args.kind,
                    "files": row["files"],
                    "compressed_bytes": row["compressed_bytes"],
                    "seconds": row["seconds"],
                }
            ),
            flush=True,
        )

    report = {
        "schema": "whole-stream-reference-v1",
        "name": args.name,
        "kind": args.kind,
        "file_list": str(args.file_list.resolve()),
        "file_list_sha256": sha256_file(args.file_list),
        "total_files": len(files),
        "zstd_version": version,
        "level": args.level,
        "long_log": args.long_log,
        "threads": 1,
        "content_contract": "ordered byte concatenation; no tar or path metadata",
        "measurements": rows,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"output": str(args.output), "measurements": len(rows)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
