#!/usr/bin/env python3
"""Build an authenticated S8 input inventory from four local corpora.

The request names one source-relative translation unit and one compile database
for each declared corpus.  This builder only reads and authenticates those
files; it never creates a source, runs a compiler, or guesses a missing input.
Successful output contains one immutable corpus record and all 32 declared
corpus/profile/regime cells.  Any unavailable, aliased, ambiguous, or
malformed input is an exact ``inventory failure: ...`` and produces no output.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import stat
import subprocess
import sys
from pathlib import Path

try:  # Works as both a package module and a directly invoked harness script.
    from .s8_schema import CORPORA, CURRENT_SEMANTICS, DECLARED_CELLS, SPLITS
except ImportError:  # pragma: no cover - exercised by direct script runners.
    from s8_schema import CORPORA, CURRENT_SEMANTICS, DECLARED_CELLS, SPLITS


REQUEST_SCHEMA = "icecream-s8-input-inventory-request-v1"
MANIFEST_SCHEMA = "icecream-s8-input-manifest-v1"
SEMANTICS = CURRENT_SEMANTICS
REQUEST_KEYS = {"schema", "semantics", "corpora"}
CORPUS_KEYS = {"corpus", "root", "source_relative", "compile_db"}
MAX_REQUEST_BYTES = 1 * 1024 * 1024
MAX_SOURCE_BYTES = 256 * 1024 * 1024
MAX_COMPILE_DB_BYTES = 256 * 1024 * 1024
HEX40 = re.compile(r"^[0-9a-fA-F]{40}$")
HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")


class InventoryError(ValueError):
    """Raised when the requested local inventory cannot be authenticated."""


def _failure(corpus: str | None, reason: str, path: object | None = None) -> InventoryError:
    prefix = "inventory failure:"
    if corpus is not None:
        prefix += f" corpus={corpus}"
    message = f"{prefix} reason={reason}"
    if path is not None:
        message += f" path={path}"
    return InventoryError(message)


def canonical_bytes(value: object) -> bytes:
    try:
        return json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("ascii")
    except (TypeError, ValueError, OverflowError, UnicodeError) as exc:
        raise _failure(None, "canonical_json_invalid") from exc


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise _failure(None, f"duplicate_json_key={key}")
        result[key] = value
    return result


def parse_json(raw: bytes, label: str) -> object:
    try:
        return json.loads(
            raw.decode("utf-8"),
            object_pairs_hook=_reject_duplicate_keys,
            parse_constant=lambda value: (_ for _ in ()).throw(
                _failure(None, f"{label}_non_finite_json={value}")
            ),
        )
    except InventoryError:
        raise
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise _failure(None, f"{label}_invalid_json") from exc


def _authenticated_file(path: Path, label: str, limit: int) -> tuple[bytes, str, int]:
    try:
        info = path.lstat()
    except OSError as exc:
        raise _failure(None, f"{label}_unavailable", path) from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
        raise _failure(None, f"{label}_not_private_regular_file", path)
    if info.st_nlink != 1:
        raise _failure(None, f"{label}_hard_link_alias", path)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        fd = os.open(path, flags)
    except OSError as exc:
        raise _failure(None, f"{label}_cannot_open", path) from exc
    try:
        before = os.fstat(fd)
        if (before.st_dev, before.st_ino, before.st_size) != (
            info.st_dev, info.st_ino, info.st_size
        ):
            raise _failure(None, f"{label}_changed_before_read", path)
        digest = hashlib.sha256()
        chunks: list[bytes] = []
        size = 0
        while True:
            try:
                block = os.read(fd, 1 << 20)
            except OSError as exc:
                raise _failure(None, f"{label}_read_failed", path) from exc
            if not block:
                break
            size += len(block)
            if size > limit:
                raise _failure(None, f"{label}_too_large", path)
            chunks.append(block)
            digest.update(block)
        after = os.fstat(fd)
        if any(
            getattr(before, field) != getattr(after, field)
            for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
        ):
            raise _failure(None, f"{label}_changed_while_reading", path)
        return b"".join(chunks), digest.hexdigest(), size
    finally:
        os.close(fd)


def _absolute_path(value: object, label: str, corpus: str) -> Path:
    if not isinstance(value, str) or not value:
        raise _failure(corpus, f"{label}_missing")
    path = Path(value)
    if not path.is_absolute():
        raise _failure(corpus, f"{label}_must_be_absolute", value)
    return path


def _source_relative(value: object, corpus: str) -> str:
    if not isinstance(value, str) or not value or value.startswith("/"):
        raise _failure(corpus, "source_relative_invalid", value)
    parts = value.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise _failure(corpus, "source_relative_escapes_root", value)
    return "/".join(parts)


def _root(path: Path, corpus: str) -> Path:
    try:
        info = path.lstat()
    except OSError as exc:
        raise _failure(corpus, "source_root_unavailable", path) from exc
    if stat.S_ISLNK(info.st_mode) or not stat.S_ISDIR(info.st_mode):
        raise _failure(corpus, "source_root_not_private_directory", path)
    return path.resolve()


def _safe_source(root: Path, relative: str, corpus: str) -> tuple[Path, bytes, str, int]:
    path = root / relative
    try:
        resolved = path.resolve()
        resolved.relative_to(root)
    except (OSError, ValueError) as exc:
        raise _failure(corpus, "source_relative_escapes_root", relative) from exc
    try:
        raw, digest, size = _authenticated_file(path, "source", MAX_SOURCE_BYTES)
    except InventoryError as exc:
        reason = str(exc).split(" reason=", 1)[-1].split(" path=", 1)[0]
        raise _failure(corpus, reason, path) from exc
    return resolved, raw, digest, size


def _git_identity(root: Path, corpus: str) -> tuple[str, str]:
    def rev(spec: str) -> str:
        try:
            completed = subprocess.run(
                ["git", "-C", str(root), "rev-parse", spec],
                check=True,
                capture_output=True,
                text=True,
                timeout=10,
            )
        except (OSError, subprocess.SubprocessError) as exc:
            raise _failure(corpus, "source_identity_unavailable", root) from exc
        value = completed.stdout.strip()
        if HEX40.fullmatch(value) is None:
            raise _failure(corpus, "source_identity_invalid", root)
        return value.lower()

    return rev("HEAD"), rev("HEAD^{tree}")


def _resolved_entry_file(entry: dict[str, object], root: Path, corpus: str) -> Path:
    value = entry.get("file")
    if not isinstance(value, str) or not value:
        raise _failure(corpus, "compile_entry_file_invalid")
    directory = entry.get("directory", str(root))
    if not isinstance(directory, str) or not directory:
        raise _failure(corpus, "compile_entry_directory_invalid")
    base = Path(directory)
    if not base.is_absolute():
        base = root / base
    return (Path(value) if Path(value).is_absolute() else base / value).resolve()


def _argv(entry: dict[str, object], corpus: str) -> list[str]:
    has_command = "command" in entry
    has_arguments = "arguments" in entry
    if has_command == has_arguments:
        raise _failure(corpus, "compile_entry_command_ambiguous")
    if has_arguments:
        value = entry["arguments"]
        if not isinstance(value, list) or not value or not all(isinstance(token, str) and token for token in value):
            raise _failure(corpus, "compile_entry_arguments_invalid")
        return list(value)
    command = entry["command"]
    if not isinstance(command, str) or not command:
        raise _failure(corpus, "compile_entry_command_invalid")
    try:
        value = shlex.split(command)
    except ValueError as exc:
        raise _failure(corpus, "compile_entry_command_unparseable") from exc
    if not value:
        raise _failure(corpus, "compile_entry_command_empty")
    return value


def _token_is_source(token: str, directory: Path, source: Path) -> bool:
    candidate = Path(token)
    if not candidate.is_absolute():
        candidate = directory / candidate
    try:
        return candidate.resolve() == source
    except OSError:
        return False


def _compile_record(path: Path, root: Path, source: Path, corpus: str) -> dict[str, object]:
    try:
        raw, digest, size = _authenticated_file(path, "compile_database", MAX_COMPILE_DB_BYTES)
    except InventoryError as exc:
        reason = str(exc).split(" reason=", 1)[-1].split(" path=", 1)[0]
        raise _failure(corpus, reason, path) from exc
    try:
        value = parse_json(raw, f"corpus={corpus}_compile_database")
    except InventoryError as exc:
        reason = str(exc).split(" reason=", 1)[-1].split(" path=", 1)[0]
        raise _failure(corpus, reason, path) from exc
    if not isinstance(value, list) or not value:
        raise _failure(corpus, "compile_database_entries_invalid", path)
    matches: list[tuple[dict[str, object], list[str]]] = []
    for entry in value:
        if not isinstance(entry, dict):
            raise _failure(corpus, "compile_entry_invalid", path)
        if _resolved_entry_file(entry, root, corpus) != source:
            continue
        argv = _argv(entry, corpus)
        directory_value = entry.get("directory", str(root))
        assert isinstance(directory_value, str)
        directory = Path(directory_value)
        if not directory.is_absolute():
            directory = root / directory
        source_tokens = [token for token in argv if _token_is_source(token, directory, source)]
        if len(source_tokens) != 1:
            raise _failure(corpus, "compile_entry_source_token_ambiguous", path)
        matches.append((entry, argv))
    if len(matches) != 1:
        raise _failure(corpus, f"compile_entry_match_count={len(matches)}", path)
    _entry, argv = matches[0]
    argv_raw = canonical_bytes(argv)
    return {
        "path": str(path.resolve()),
        "sha256": digest,
        "bytes": size,
        "argv": argv,
        "argv_sha256": hashlib.sha256(argv_raw).hexdigest(),
    }


def _request(path: Path) -> tuple[dict[str, object], str, int]:
    raw, digest, size = _authenticated_file(path, "inventory_request", MAX_REQUEST_BYTES)
    value = parse_json(raw, "inventory_request")
    if not isinstance(value, dict) or set(value) != REQUEST_KEYS:
        raise _failure(None, "request_fields_invalid", path)
    if value["schema"] != REQUEST_SCHEMA or value["semantics"] != SEMANTICS:
        raise _failure(None, "request_schema_or_semantics_invalid", path)
    return value, digest, size


def _corpus_requests(value: object) -> dict[str, dict[str, object]]:
    if not isinstance(value, list):
        raise _failure(None, "corpora_not_a_list")
    result: dict[str, dict[str, object]] = {}
    for item in value:
        if not isinstance(item, dict) or set(item) != CORPUS_KEYS:
            raise _failure(None, "corpus_request_fields_invalid")
        name = item["corpus"]
        if not isinstance(name, str) or name not in CORPORA or name in result:
            raise _failure(None, "corpus_request_name_invalid", name)
        result[name] = item
    missing = [name for name in CORPORA if name not in result]
    if missing:
        raise _failure(None, f"missing_corpora={','.join(missing)}")
    if len(result) != len(CORPORA):
        raise _failure(None, "corpus_count_invalid")
    return result


def _build_corpus(name: str, request: dict[str, object]) -> dict[str, object]:
    root = _root(_absolute_path(request["root"], "source_root", name), name)
    relative = _source_relative(request["source_relative"], name)
    source, _raw, source_digest, source_bytes = _safe_source(root, relative, name)
    compile_db = _absolute_path(request["compile_db"], "compile_database", name)
    compile_record = _compile_record(compile_db, root, source, name)
    source_commit, source_tree = _git_identity(root, name)
    return {
        "corpus": name,
        "split": SPLITS[name],
        "source_root": str(root),
        "source_relative": relative,
        "source_sha256": source_digest,
        "source_bytes": source_bytes,
        "source_commit": source_commit,
        "source_tree": source_tree,
        "compile_database": compile_record,
    }


def _cell_record(corpus: dict[str, object], cell: dict[str, str]) -> dict[str, object]:
    name = corpus["corpus"]
    assert isinstance(name, str)
    compile_record = corpus["compile_database"]
    assert isinstance(compile_record, dict)
    return {
        "cell_id": f"{name}/{cell['profile']}/{cell['regime']}",
        "corpus": name,
        "profile": cell["profile"],
        "regime": cell["regime"],
        "split": corpus["split"],
        "source_root": corpus["source_root"],
        "source_relative": corpus["source_relative"],
        "source_sha256": corpus["source_sha256"],
        "source_bytes": corpus["source_bytes"],
        "source_commit": corpus["source_commit"],
        "source_tree": corpus["source_tree"],
        "compile_database_sha256": compile_record["sha256"],
        "compile_database_bytes": compile_record["bytes"],
        "compile_argv": compile_record["argv"],
        "compile_argv_sha256": compile_record["argv_sha256"],
    }


def _write_new(path: Path, value: object) -> None:
    if path.exists() or path.is_symlink():
        raise _failure(None, "output_already_exists", path)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = canonical_bytes(value) + b"\n"
    try:
        with path.open("xb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    except OSError as exc:
        raise _failure(None, "output_write_failed", path) from exc


def build(request_path: Path, output_path: Path) -> dict[str, object]:
    """Authenticate a request and emit one four-corpus, 32-cell manifest."""
    request, request_digest, request_bytes = _request(request_path)
    requests = _corpus_requests(request["corpora"])
    corpora = [_build_corpus(name, requests[name]) for name in CORPORA]
    by_name = {record["corpus"]: record for record in corpora}
    cells = [_cell_record(by_name[cell["corpus"]], cell) for cell in DECLARED_CELLS]
    cell_ids = [cell["cell_id"] for cell in cells]
    if len(cell_ids) != 32 or len(set(cell_ids)) != 32:
        raise _failure(None, "declared_cell_contract_invalid")
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "semantics": SEMANTICS,
        "request": {"path": str(request_path.resolve()), "sha256": request_digest, "bytes": request_bytes},
        "corpora": corpora,
        "cells": cells,
    }
    _write_new(output_path, manifest)
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        build(args.request.absolute(), args.out.absolute())
    except InventoryError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
