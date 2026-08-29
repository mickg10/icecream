#!/usr/bin/env python3
"""Run the three remaining ZSTD_TU S7 cells with fail-closed evidence.

This driver is deliberately narrow: it owns experiment-directory allocation,
cell preflight, and one JSONL result schema; the production compile runner
continues to own daemon lifecycle and byte-exact compilation.  A missing or
ambiguous workload is a HOLD, never a generated substitute.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import stat
from datetime import datetime, timezone
from pathlib import Path


CELLS = (
    ("fmt", "warm"),
    ("RocksDB", "cold"),
    ("RocksDB", "warm"),
)
SCHEMA = "icecream-s7-zstd-tu-cell-v1"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def regular(path: Path) -> bool:
    try:
        info = path.lstat()
    except OSError:
        return False
    return stat.S_ISREG(info.st_mode) and not stat.S_ISLNK(info.st_mode)


def authenticated_input(path: Path) -> tuple[str, int] | None:
    """Read one exact input, rejecting links and mutable aliases."""
    if not regular(path):
        return None
    info = path.lstat()
    if info.st_nlink != 1:
        return None
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError:
        return None
    try:
        opened = os.fstat(fd)
        if (opened.st_dev, opened.st_ino, opened.st_size) != (
            info.st_dev,
            info.st_ino,
            info.st_size,
        ):
            return None
        digest = hashlib.sha256()
        size = 0
        while True:
            block = os.read(fd, 1 << 20)
            if not block:
                break
            digest.update(block)
            size += len(block)
        return digest.hexdigest(), size
    finally:
        os.close(fd)


def source_from_manifest(root: Path, manifest: Path) -> tuple[Path, str] | None:
    """Resolve the first manifest entry without accepting absolute aliases."""
    if not regular(manifest):
        return None
    try:
        lines = manifest.read_text(encoding="utf-8").splitlines()
    except OSError:
        return None
    entries = [line.strip() for line in lines if line.strip()]
    if not entries:
        return None
    entry = entries[0]
    candidate = Path(entry)
    if candidate.is_absolute():
        # Existing manifests use absolute checkout paths.  They are accepted
        # only when they are actually below the explicitly supplied root.
        try:
            relative = candidate.resolve().relative_to(root.resolve())
        except ValueError:
            return None
    else:
        relative = candidate
    if any(part in ("", ".", "..") for part in relative.parts):
        return None
    path = root / relative
    return (path, relative.as_posix()) if authenticated_input(path) else None


def compile_argv_from_database(database: Path, source: str) -> list[str] | None:
    """Return the authoritative command's arguments after its compiler.

    The database and selected source are authenticated regular files.  The
    caller owns replacement of the source/output paths; all flags remain
    byte-for-byte in their database order.
    """
    authenticated = authenticated_input(database)
    if authenticated is None:
        return None
    try:
        entries = json.loads(database.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    if not isinstance(entries, list):
        return None
    for entry in entries:
        if not isinstance(entry, dict) or entry.get("file") != source:
            continue
        command = entry.get("command")
        if not isinstance(command, str):
            return None
        try:
            tokens = shlex.split(command)
        except ValueError:
            return None
        if len(tokens) < 2:
            return None
        # The compiler is selected by the production runner.  Retain every
        # remaining token, including the exact -I/-D ordering.  The live
        # runner applies the same final switch when debug output is enabled:
        # Icecream adds remote-only GCC switches whose DW_AT_producer spelling
        # would otherwise defeat the full-object byte-equality control.
        argv = tokens[1:]
        if (
            any(token == "-g" or token.startswith("-g") for token in argv)
            and not any(
                token in {"-grecord-gcc-switches", "-gno-record-gcc-switches"}
                for token in argv
            )
        ):
            argv.append("-gno-record-gcc-switches")
        return argv
    return None


def cell_row(
    corpus: str,
    regime: str,
    *,
    artifact_root: Path,
    source_root: Path | None,
    source_relative: str | None,
) -> dict[str, object]:
    cell = f"{corpus}/ZSTD_TU/{regime}"
    row: dict[str, object] = {
        "schema": SCHEMA,
        "cell": cell,
        "profile": "ZSTD_TU",
        "regime": regime,
        "status": "HOLD",
        "reason": "",
        "artifact_root": str(artifact_root),
        "source_root": str(source_root) if source_root else None,
        "source_relative": source_relative,
        "input_sha256": None,
        "input_bytes": None,
        "command": None,
        "returncode": None,
        "stdout_sha256": None,
        "stderr_sha256": None,
    }
    if not artifact_root.is_dir() or artifact_root.is_symlink():
        row["reason"] = "authoritative_cell_artifact_root_unavailable"
        return row
    if source_root is None or source_relative is None:
        row["reason"] = "authoritative_source_root_and_relative_input_required"
        return row
    if not source_root.is_dir() or source_root.is_symlink():
        row["reason"] = "authoritative_source_root_unavailable"
        return row
    if Path(source_relative).is_absolute() or ".." in Path(source_relative).parts:
        row["reason"] = "source_relative_path_escapes_root"
        return row
    input_path = source_root / source_relative
    authenticated = authenticated_input(input_path)
    if authenticated is None:
        row["reason"] = "authoritative_source_input_unavailable_or_aliased"
        return row
    row["input_sha256"], row["input_bytes"] = authenticated
    row["reason"] = "preflight_only_no_product_runner"
    return row


def write_jsonl(path: Path, rows: list[dict[str, object]]) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    payload = b"".join(
        (json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n").encode()
        for row in rows
    )
    try:
        with temporary.open("wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--source-relative", required=True)
    parser.add_argument("--compile-db", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    output = args.out.absolute()
    if output.exists():
        parser.error(f"output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    experiment = output.parent / f"s7-zstd-tu-{stamp}-{os.getpid()}"
    experiment.mkdir()
    rows = [
        cell_row(
            corpus,
            regime,
            artifact_root=args.artifacts / corpus / "ZSTD_TU" / regime,
            source_root=args.source_root.absolute(),
            source_relative=args.source_relative,
        )
        for corpus, regime in CELLS
    ]
    if args.compile_db is not None:
        compile_argv = compile_argv_from_database(args.compile_db, str(args.source_root / args.source_relative))
        if compile_argv is None:
            parser.error("authoritative compile database entry unavailable")
        for row in rows:
            row["compile_db"] = str(args.compile_db.absolute())
            row["compile_db_sha256"] = sha256(args.compile_db)
            row["compile_argv"] = compile_argv
    write_jsonl(experiment / "results.jsonl", rows)
    # The caller receives one stable path and one immutable JSONL payload.  A
    # later product run must use a new experiment directory rather than
    # replacing this preflight evidence in place.
    write_jsonl(output, rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
